# Research / Design Note: Roughness-Driven Glossy Reflections

**Status:** READ-ONLY research note (no source file was modified).
**Scope:** Add roughness-dependent glossy blur to the recursive Whitted mirror
reflection (`material.reflectivity`) for opt-in PBR materials (`material.pbr == 1`),
with strict byte-identity for the default and all non-PBR scenes.
**Dependencies:** C11 + `libm` only. No new third-party code.

**Problem source (README limitation):** `README.md:628-634`

> **PBR is direct-light only and single-scattering** — … the recursive mirror
> reflection (`reflectivity`) is still a sharp Whitted reflection that the material
> `roughness` does **not** blur.

---

## 0. TL;DR recommendation

- **Where:** the reflection block in `src/render.c` `trace_hit()` at **lines 215-222**
  (`refl_col`, `if (depth < max_depth)`, `vec3_reflect(r.dir, N)`, `trace(..., depth+1, ...)`).
- **Sampling:** importance-sample the GGX/Trowbridge-Reitz half-vector `H` using
  `alpha = roughness^2` (exactly matching `roughness_to_alpha()` in
  `src/material.c:190-195`), then reflect the incident ray about `H`
  (`R = I - 2 (I·H) H`). Fall back to the exact mirror direction when
  `roughness <= GLOSSY_ROUGHNESS_EPSILON`.
- **Sample count:** `GLOSSY_REFLECTION_SAMPLES = 16` (matches `SUN_SHADOW_SAMPLES`,
  `src/render.c:96`); average with weight `1/N`.
- **Determinism:** draw `r1, r2` from `render_rand01`/`render_hash3` as a **pure
  function of `(seed_key, depth, sample_index i)`**, with channel constants distinct
  from the sun-disk (`0x5a17u`, `0x7c3du`) and DOF (`s*2+2`, `s*2+3`) channels.
- **Gating (byte-identity):** take the glossy path **iff**
  `mm->pbr != 0 && m->reflectivity > 0.0 && m->roughness > GLOSSY_ROUGHNESS_EPSILON && depth < max_depth`;
  otherwise execute the **existing** sharp-reflection statements verbatim.

---

## 1. Current reflection code path (`src/render.c`, `trace_hit`)

**Function:** `static Vec3 trace_hit(const Scene *scene, Ray r, int depth, int max_depth,
double time, const Hit *h, unsigned seed_key)` — declared at `src/render.c:124-125`,
forward-referenced via `trace()` at `src/render.c:110-111`.

The reflection is generated in the **opaque** branch, after the direct-light block and
after the Fresnel/refraction branch is skipped. Exact code (`src/render.c:215-222`):

```c
/* --- reflection --- */
Vec3 refl_col = vec3(0.0, 0.0, 0.0);
if (depth < max_depth) {
    Vec3 rd = vec3_reflect(r.dir, N);
    refl_col = trace(scene,
                     (Ray){vec3_add(P, vec3_scale(N, 1e-3)), rd},
                     depth + 1, max_depth, time, seed_key);
}
```

Key facts:

| Aspect | Detail | Location |
| --- | --- | --- |
| Reflection direction | `vec3_reflect(r.dir, N)` = `I - 2 (I·N) N`, exact mirror. `vec3_reflect` is `static inline` in `src/vec3.h:113-118`. | `src/render.c:218` |
| Ray origin | `P + N * 1e-3` (self-intersection offset). | `src/render.c:220` |
| Recursion depth | Guarded by `depth < max_depth`; recursive call passes `depth + 1`. `trace()` re-checks `depth > max_depth` and returns black (`src/render.c:320-322`). | `src/render.c:217,221` |
| Threading of `seed_key` | `seed_key` is passed through unchanged into the recursive `trace()`. | `src/render.c:221` |

**Reflectivity weighting / blending (opaque path, `src/render.c:308`):**

```c
Vec3 out = vec3_lerp(color, refl_col, m->reflectivity);
```

i.e. `out = color*(1 - reflectivity) + refl_col*reflectivity`. `color` is
`material_ambient(...)` plus the direct-sun term (`material_shade_local` or, when
`mm->pbr`, `material_shade_pbr`). Self-emission is added once afterwards
(`src/render.c:310-313`).

**Transparency path (NOT touched by this feature):** when `m->transparency > 0`, the
reflection is combined by Fresnel instead, at `src/render.c:295-296`:

```c
Vec3 surface = vec3_add(vec3_scale(refl_col, F),
                        vec3_scale(refr_col, 1.0 - F));
Vec3 out = vec3_lerp(color, surface, m->transparency);
```

Glossy blur is therefore only meaningful in the **opaque** branch (`reflectivity` is
already documented as ignored when `transparency > 0`, see
`docs/research_pbr_shading.md:92-94`). Keeping the transparent branch untouched also
preserves glass/water output exactly.

**Material fields (`src/material.h`):**
`reflectivity` (`:56`), `metallic` (`:83`), `roughness` (`:84`), `pbr` (`:86`).
All PBR fields zero-initialise to legacy-neutral defaults (`src/material.h:92-99`).

---

## 2. GGX importance sampling of the reflection direction

### 2.1 Alpha convention — must match `roughness_to_alpha`

`src/material.c:189-195`:

```c
/* Disney/GGX perceptual-roughness remap: alpha = max(roughness^2, A_MIN). */
static double roughness_to_alpha(double roughness)
{
    double r = clamp01(roughness);
    double a = r * r;
    return a < PBR_ALPHA_MIN ? PBR_ALPHA_MIN : a;
}
```

with `#define PBR_ALPHA_MIN 1e-4` (`src/material.c:164`). So the convention is
**`alpha = max(roughness^2, 1e-4)`** — squared, Disney/GGX. The glossy sampler MUST use
the same `alpha` so the blur width agrees with the direct-sun highlight width produced
by `material_shade_pbr()` (which uses `d_ggx(ndh, alpha)` at `src/material.c:218,234`).

**Recommendation:** promote `roughness_to_alpha` to a public helper
(`double material_roughness_to_alpha(double roughness)`) so `render.c` never re-derives
it, and have the sampler call it. This makes the shared convention a single source of truth.

### 2.2 Half-vector sampling (GGX / Trowbridge-Reitz), tangent-space

Given two independent uniforms `u1, u2 ∈ [0,1)`:

```
alpha  = material_roughness_to_alpha(roughness)     // max(roughness^2, 1e-4)
a2     = alpha * alpha

cosTheta = sqrt( (1 - u1) / (1 + (a2 - 1) * u1) )   // GGX NDF importance sampling
sinTheta = sqrt( 1 - cosTheta^2 )
phi      = 2*pi*u2

H_local  = ( sinTheta*cos(phi), sinTheta*sin(phi), cosTheta )
```

This is the standard Karis/UE4 GGX sampling: the denominator
`1 + (a2-1) u1` is `>= a2 > 0`, so there is no division by zero, and `cosTheta ∈ (0,1]`
so `H` always lies in the `+N` hemisphere.

### 2.3 Tangent frame and world-space half-vector

Build an orthonormal basis `(t, b)` perpendicular to `N`. The project already has exactly
this helper — `sky_basis()` in `src/material.c:362-375` (Gram-Schmidt against the world
axis least parallel to `n`), currently `static`. Reuse it (either make it public, or add a
public wrapper) rather than writing a second basis routine:

```c
Vec3 t, b;
basis(N, &t, &b);
Vec3 H = vec3_normalize(vec3_add(
             vec3_add(vec3_scale(t, H_local.x), vec3_scale(b, H_local.y)),
             vec3_scale(N, H_local.z)));
```

### 2.4 Reflect the incident ray about `H`

`R = reflect(I, H) = I - 2 (I·H) H`, where `I = r.dir` (points away from the eye, into the
surface). Equivalently `R = 2 (V·H) H - V` with `V = -I`. Because `I·H < 0` and `H` is in
the `+N` hemisphere, `R` lands in the `+N` hemisphere.

**Robustness:** if `dot(R, N) <= 0` (possible for very rough, near-grazing hits), fall back
to the exact mirror direction `vec3_reflect(I, N)`. This keeps the traced ray on the correct
side of the surface without any clamping artifacts.

### 2.5 Roughness ≈ 0 handling

Short-circuit before sampling:

```c
#define GLOSSY_ROUGHNESS_EPSILON 1e-6
if (roughness <= GLOSSY_ROUGHNESS_EPSILON) {
    return vec3_reflect(I, N);   // EXACT mirror direction, one ray
}
```

Rationale: as `alpha → 0`, `cosTheta → 1`, `H → N`, and `R → mirror`. The GGX lobe
degenerates to a delta, so averaging N samples would waste `N-1` identical rays. The
epsilon gate avoids that cost **and** guarantees the exact legacy direction (not merely a
limit of it) so there is no numerical discontinuity.

---

## 3. Sample count and deterministic seeding

### 3.1 Sample count

Use `GLOSSY_REFLECTION_SAMPLES = 16`, mirroring `SUN_SHADOW_SAMPLES = 16`
(`src/render.c:96`). 16 is enough to stabilise the lobe for the roughness range exercised
by `scenes/example_materials.scene` (0.05 → 0.90) and keeps the cost predictable:
`GLOSSY_REFLECTION_SAMPLES` extra recursive rays **per reflecting PBR hit per depth level**.
If cost is a concern, 8 is acceptable; keep it a single named `#define` so it can be tuned
in one place. Because the feature is opt-in (`pbr && reflectivity>0 && roughness>eps`), the
default render pays **zero** extra cost.

### 3.2 Deterministic, thread-independent seeding

The existing deterministic RNG is `render_hash3()` (`src/render.c:70-78`) +
`render_rand01()` (`src/render.c:80-86`). `seed_key` is already a pure function of the
primary pixel and AA sample index: `seed_key = render_hash3((unsigned)x, (unsigned)y,
(unsigned)s)` (`src/render.c:423-424`), and is documented as thread/tile independent
(`src/render.c:117-122`).

**Sun-disk soft shadows** use it as (`src/render.c:175-186`):

```c
unsigned k = seed_key * 0x9e3779b9u + (unsigned)i * 2u;
double r1 = render_rand01(k, (unsigned)i, 0x5a17u);
double r2 = render_rand01(k, (unsigned)i, 0x7c3du);
```

**DOF** uses PRNG channels `s*2+2` / `s*2+3` (`src/render.c:413-415`) — deliberately
disjoint from the AA channels `s*2+0` / `s*2+1`.

For glossy reflection, the same `seed_key` is reused across every recursion level, so the
per-sample key MUST also fold in the **depth** to decorrelate successive glossy bounces
(otherwise a rough mirror facing a rough mirror would reuse identical samples at every
level). Recommended:

```c
for (int i = 0; i < GLOSSY_REFLECTION_SAMPLES; ++i) {
    unsigned k = seed_key * 0x9e3779b9u
               + (unsigned)depth * 0x85ebca6bu
               + (unsigned)i * 2u;
    double r1 = render_rand01(k, (unsigned)i, 0x9e37u);   // glossy channel A
    double r2 = render_rand01(k, (unsigned)i, 0x85ebu);   // glossy channel B
    ...
}
```

Using **distinct channel constants** (`0x9e37u`, `0x85ebu`) from the sun-disk channels
(`0x5a17u`, `0x7c3du`) keeps glossy jitter statistically independent of the shadow jitter
at the same hit. The key is a pure function of `(x, y, s, depth, i)` — never of thread,
tile, or schedule — so output is **byte-identical for any thread count**, exactly like the
sun-disk and DOF paths.

---

## 4. Energy / weighting and double-counting

**Rule:** keep the existing opaque blend exactly and only replace the single `refl_col`
with the glossy-averaged `refl_col`:

```
refl_col = (1/N) * Σ_i trace(glossy_ray_i)          // N = GLOSSY_REFLECTION_SAMPLES
out      = lerp(color, refl_col, m->reflectivity)   // UNCHANGED (src/render.c:308)
```

- **Uniform 1/N weights are correct here** because we *importance-sample the GGX lobe
  itself*. Each reflected ray is drawn with density proportional to the NDF lobe, so the
  plain average is the natural estimator of the lobe-integrated reflected radiance. (No
  explicit PDF division is required because we are not combining estimators via MIS.)
- **No double counting with `material_shade_pbr`:** the Cook-Torrance term
  (`src/material.c:197-247`) is the **direct-sun** specular lobe; the recursive reflection
  is **indirect** transport (other geometry / sky). They are different light paths, exactly
  as the sharp reflection already coexists with the direct highlight today. This feature
  only blurs the indirect term.
- **Graceful limits:**
  - `roughness → 0`: all `N` samples collapse to the exact mirror ray and the average equals
    the sharp reflection → continuous transition (and the epsilon gate makes it exact).
  - `reflectivity == 0`: `lerp(color, refl_col, 0) == color`, so the term contributes
    nothing (this is why all conductor/dielectric presets set `reflectivity = 0.0`, e.g.
    `src/scene_desc.c:656,685`, to avoid double counting with the PBR specular).
  - `pbr == 0`: legacy Blinn-Phong path, no glossy sampling at all.
- **Fresnel consideration:** for the current design we deliberately keep the existing
  `reflectivity` mix (a scene-authored artistic weight) rather than introducing a new
  Fresnel weighting, so the feature is purely "blur the same mirror term". A future
  enhancement could weight by `F(cos θ)` from `fresnel_schlick` (`src/material.c:113-119`),
  but that would change existing `reflectivity` semantics and is out of scope here.

---

## 5. Exact backward-compatibility gating

**Gating condition to enter the glossy branch:**

```c
int glossy = (mm->pbr != 0) &&
             (m->reflectivity > 0.0) &&
             (m->roughness > GLOSSY_ROUGHNESS_EPSILON) &&
             (depth < max_depth);
```

If `glossy == 0`, the code executes the **verbatim existing statements**
(`src/render.c:216-222`) — same `vec3_reflect(r.dir, N)`, same origin, same `trace` call,
same arguments. There is no arithmetic difference, so the output bytes are unchanged.

Why this guarantees byte-identity:

1. **Default (no `--scene`) render:** every default material has `pbr = 0` and
   `roughness = 0` (zero-initialised; `scenes/default.scene` lists no `pbr`/`roughness`
   keys). `mm->pbr != 0` is false → legacy path → **byte-identical**.
2. **All non-PBR scenes:** `pbr == 0` for every material (only
   `scenes/example_materials.scene` and `scenes/example_presets.scene` enable PBR, via the
   `pbr` key or named `type = …` presets) → legacy path → **byte-identical**.
3. **`scenes/example_presets.scene`:** uses named presets; the conductor/dielectric/emissive
   presets set `pbr = 1` but `reflectivity = 0.0` (`src/scene_desc.c:656,685,762,791`), so
   `reflectivity > 0.0` is false → **byte-identical**.
4. **Transparent/water/glass:** the glossy branch lives only in the opaque `else` after the
   `transparency > 0` early return; those paths are untouched → **byte-identical**.
5. **`--depth 0`:** `depth < max_depth` false → unchanged black reflection behavior.

The only scene whose output changes is `scenes/example_materials.scene`, which is a PBR
demonstration scene (gold spheres with `reflectivity = 0.30`, `pbr = 1`, roughness ramp
`0.05 → 0.90` at `scenes/example_materials.scene:108-149`) — that change is the intended
feature, and it is a PBR scene, not the default or a non-PBR scene.

---

## 6. Concrete implementation outline

### 6.1 Files / functions to change

| File | Change |
| --- | --- |
| `src/material.h` | Declare `double material_roughness_to_alpha(double roughness);` and `Vec3 material_sample_glossy_dir(const Material *m, Vec3 N, Vec3 incident, double r1, double r2);`. Optionally declare an orthonormal-basis helper. |
| `src/material.c` | Make `roughness_to_alpha` public (rename/wrap); add `material_sample_glossy_dir` (GGX half-vector sampling + reflect); expose/reuse `sky_basis` (`src/material.c:362-375`). |
| `src/render.c` | Add `#define GLOSSY_REFLECTION_SAMPLES 16` and `#define GLOSSY_ROUGHNESS_EPSILON 1e-6` near `SUN_SHADOW_SAMPLES` (`:96`); branch the reflection block (`:215-222`). |
| `docs/` + `README.md` | Update the limitation text at `README.md:628-634` once implemented (documentation only). |

No changes to the scene parser/writer are required — no new keys. Existing
`pbr`/`roughness`/`reflectivity` keys drive the feature.

### 6.2 New helper signatures (material.c / material.h)

```c
/* Public: the single alpha convention shared with material_shade_pbr().
 * alpha = max(clamp01(roughness)^2, PBR_ALPHA_MIN). */
double material_roughness_to_alpha(double roughness);

/*
 * Importance-sample a GGX half-vector for perceptual `roughness`, reflect the
 * incident unit vector about it, and return a UNIT reflection direction.
 *   - roughness <= GLOSSY_ROUGHNESS_EPSILON  -> exact vec3_reflect(incident, N)
 *   - dot(R, N) <= 0                          -> exact vec3_reflect(incident, N)
 * Deterministic pure function of (N, incident, r1, r2) and m->roughness.
 */
Vec3 material_sample_glossy_dir(const Material *m, Vec3 N, Vec3 incident,
                                double r1, double r2);
```

Reference body sketch for `material_sample_glossy_dir` (reuses `vec3_reflect`,
`vec3_normalize`, `vec3_add`, `vec3_scale` from `src/vec3.h`, and `sky_basis`):

```c
Vec3 material_sample_glossy_dir(const Material *m, Vec3 N, Vec3 incident,
                                double r1, double r2)
{
    double r = clamp01(m->roughness);
    if (r <= GLOSSY_ROUGHNESS_EPSILON) {
        return vec3_reflect(incident, N);          /* exact mirror */
    }
    double alpha = material_roughness_to_alpha(r);
    double a2 = alpha * alpha;

    /* Clamp uniforms defensively. */
    if (r1 < 0.0) r1 = 0.0; else if (r1 >= 1.0) r1 = 0.9999999999999999;
    if (r2 < 0.0) r2 = 0.0; else if (r2 >= 1.0) r2 = 0.9999999999999999;

    double cos_theta = sqrt((1.0 - r1) / (1.0 + (a2 - 1.0) * r1));
    double sin_theta = sqrt(1.0 - cos_theta * cos_theta);
    double phi = 2.0 * MATERIAL_PI * r2;

    Vec3 t, b;
    sky_basis(N, &t, &b);                          /* src/material.c:362 */
    Vec3 H = vec3_add(
        vec3_add(vec3_scale(t, sin_theta * cos(phi)),
                 vec3_scale(b, sin_theta * sin(phi))),
        vec3_scale(N, cos_theta));
    H = vec3_normalize(H);

    Vec3 R = vec3_reflect(incident, H);
    if (vec3_dot(R, N) <= 0.0) {
        R = vec3_reflect(incident, N);             /* safety fallback */
    }
    return vec3_normalize(R);
}
```

### 6.3 `trace_hit` reflection block (render.c) — final shape

```c
/* --- reflection --- */
Vec3 refl_col = vec3(0.0, 0.0, 0.0);
if (depth < max_depth) {
    /* Opt-in roughness-blurred (glossy) reflection. The legacy sharp path is
     * kept textually intact below so the default / non-PBR renders stay
     * byte-identical. */
    if (mm->pbr && m->reflectivity > 0.0 &&
        m->roughness > GLOSSY_ROUGHNESS_EPSILON) {
        const double inv = 1.0 / (double)GLOSSY_REFLECTION_SAMPLES;
        for (int i = 0; i < GLOSSY_REFLECTION_SAMPLES; ++i) {
            unsigned k = seed_key * 0x9e3779b9u
                       + (unsigned)depth * 0x85ebca6bu
                       + (unsigned)i * 2u;
            double r1 = render_rand01(k, (unsigned)i, 0x9e37u);
            double r2 = render_rand01(k, (unsigned)i, 0x85ebu);
            Vec3 rd = material_sample_glossy_dir(mm, N, r.dir, r1, r2);
            refl_col = vec3_add(refl_col, vec3_scale(
                trace(scene, (Ray){vec3_add(P, vec3_scale(N, 1e-3)), rd},
                      depth + 1, max_depth, time, seed_key), inv));
        }
    } else {
        Vec3 rd = vec3_reflect(r.dir, N);
        refl_col = trace(scene,
                         (Ray){vec3_add(P, vec3_scale(N, 1e-3)), rd},
                         depth + 1, max_depth, time, seed_key);
    }
}
```

Note: `GLOSSY_ROUGHNESS_EPSILON` is used in both `render.c` (gating) and `material.c`
(short-circuit); define it once in `material.h` next to `MATERIAL_DEFAULT_ROUGHNESS`.

### 6.4 Suggested deterministic unit-test strategy

Add `tests/test_glossy.c` (auto-discovered by the Makefile wildcard, linked against all
project objects except `src/main.o`). Mirror the style of `tests/test_softshadow.c`,
`tests/test_dof.c`, and `tests/test_render_threads.c`.

1. **Mirror fallback is exact** — for `roughness = 0` (and negative), assert
   `material_sample_glossy_dir` returns **bit-for-bit** `vec3_reflect(incident, N)` for many
   `(r1, r2)` (compare doubles exactly, not with epsilon).
2. **Alpha convention** — `material_roughness_to_alpha(0.5) == 0.25`,
   `material_roughness_to_alpha(0.0) == PBR_ALPHA_MIN`, `material_roughness_to_alpha(2.0)
   == 1.0`.
3. **Determinism** — same `(N, incident, r1, r2, roughness)` → identical result across
   repeated calls (bit-for-bit).
4. **Unit length + hemisphere** — for many random `r1, r2` and `roughness ∈ {0.05..1.0}`,
   assert `|R| ≈ 1` and `dot(R, N) > 0`.
5. **Statistical spread grows with roughness** — average the angle between `R` and the
   mirror direction over many samples; assert it increases monotonically across
   `roughness = 0.2 < 0.5 < 0.8`, and that the mean `R` stays near the mirror direction for
   small roughness (sanity of the GGX lobe).
6. **Byte-identity gating (integration)** — render the default scene (from
   `scene_default_desc()`, as in `tests/test_render_threads.c`) and assert the buffer is
   byte-identical to a reference captured with the feature compiled in (guards that the
   default path is untouched). Also render a PBR material with `roughness = 0` and assert it
   equals the sharp-reflection result, and a material with `pbr = 0, roughness = 0.5` and
   assert it equals the legacy result.
7. **Thread independence** — extend the pattern from `tests/test_render_threads.c`: under
   `-DUSE_PTHREADS`, render a PBR glossy scene with `RAYTRACER_THREADS=1` vs `=4` and assert
   byte-identical buffers (proves the `(seed_key, depth, i)` seeding is schedule-independent).

---

## 7. Sources inspected (local)

| File / line(s) | What it establishes |
| --- | --- |
| `src/render.c:110-111,124-125` | `trace()` forward decl and `trace_hit()` signature. |
| `src/render.c:215-222` | The sharp reflection: direction, origin, recursion, `seed_key` pass-through. |
| `src/render.c:217,221,320-322` | Recursion depth handling (`depth < max_depth`, `depth + 1`, `depth > max_depth`). |
| `src/render.c:295-296` | Fresnel blend in the transparency branch (untouched). |
| `src/render.c:308` | Opaque reflectivity blend `vec3_lerp(color, refl_col, m->reflectivity)`. |
| `src/render.c:70-86` | `render_hash3` / `render_rand01` deterministic PRNG. |
| `src/render.c:96,103` | `SUN_SHADOW_SAMPLES 16`, `SUN_RADIUS_EPSILON 1e-9` (pattern to mirror). |
| `src/render.c:175-186` | Sun-disk soft-shadow sampling (`seed_key*0x9e3779b9u + i*2`, channels `0x5a17u`/`0x7c3du`). |
| `src/render.c:413-424` | DOF sampling channels `s*2+2`/`s*2+3`; `seed_key = render_hash3(x,y,s)`. |
| `src/material.c:164,168-172` | `PBR_ALPHA_MIN`, `d_ggx`. |
| `src/material.c:189-195` | `roughness_to_alpha` — **alpha = max(roughness^2, 1e-4)**. |
| `src/material.c:197-247` | `material_shade_pbr` (direct-sun Cook-Torrance; no indirect term). |
| `src/material.c:362-375` | `sky_basis` orthonormal basis (reusable). |
| `src/material.h:56,83-99` | `reflectivity`, `metallic`, `roughness`, `pbr` fields + zero defaults. |
| `src/scene_desc.c:648-668,676-698,754-775,783-804` | Presets set `reflectivity = 0.0` with `pbr = 1`. |
| `scenes/example_materials.scene:108-149` | Gold spheres: `reflectivity = 0.30`, `pbr = 1`, roughness ramp 0.05→0.90. |
| `README.md:628-634` | The documented limitation this note addresses. |
| `tests/test_softshadow.c`, `tests/test_dof.c`, `tests/test_render_threads.c` | Existing determinism/identity test patterns. |
| `Makefile` | Tests auto-discovered via `wildcard tests/*.c`. |
