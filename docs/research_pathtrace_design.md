# Unidirectional Path Tracer — Math & API Design (t-102)

READ-ONLY design for the opt-in `--pathtrace` mode. Complements
`docs/research_pathtrace_integration.md` (t-101, file/line map). Everything here
reuses existing infrastructure; **no edit to the default Whitted path**.

## Conventions (verified in the tree)

- `Ray.dir` is unit length; `Hit.normal` is **unit and already flipped to oppose
  the incoming ray**, so `dot(N, d) < 0` always (`src/geometry.h`).
- `Hit.front_face == 1` when entering from outside, `0` when emerging from
  inside (`src/geometry.h`).
- `V = -d` points toward the eye; `N` is the shading normal.
- Radiance `Le` (`Material.emissive`, `EmissiveLight.emissive`) is linear,
  distance-independent, may exceed 1 (`src/material.h`, `src/scene.h`).
- `sky_sample(dir, &scene->sky)` returns linear environment radiance including
  sun glow (`src/material.c:350`).
- PRNG: `render_hash3(a,b,c)` / `render_rand01(a,b,c)` (`src/render.c:380,390`),
  currently file-static; t-104 exposes them via `src/render.h` (remove `static`
  only — zero behaviour change) **or** carries a frozen verbatim copy inside
  `src/pathtrace.c`. Either way the algorithm is bit-identical.

Notation: `beta` = throughput (Vec3), `L` = accumulated radiance, `b` = bounce
index, `xi` = uniform draws in `[0,1)`.

---

## 1. Iterative bounce loop (no recursion)

The estimator accumulates `beta * (emitted radiance)` along one path; the path
is a simple `for` loop (no call stack, no `trace()` recursion):

```
Vec3 pathtrace_radiance(scene, r, max_depth, seed_key):
    L    = 0
    beta = (1,1,1)
    d    = r.dir ; p = r.origin
    skip_env = 0                      // set after a NEE emitter bounce
    for b = 0 .. max_depth:
        Hit h
        if !scene_intersect(scene, {p,d}, 1e-4, 1e30, &h):
            if !skip_env:  L += beta * sky_sample(d, &scene->sky)   // sec. 7
            break
        m   = scene_material(scene, h.material_index)
        P   = h.point
        N   = h.normal                 // already opposes d
        if m->is_water: N = water_normal(P.x, P.z, time)  // then re-flip
        V   = -d
        // ---- 1. emission of a directly-hit light (skip if NEE covered it) ----
        if is_emitter(h) && !skip_env:
            L += beta * Le(h)
        // ---- 2. Next Event Estimation (sec. 5) ----
        L += beta * nee(scene, m, P, N, V, h.prim_index, b, seed_key)
        // ---- 3. sample the BSDF -> (d', weight) ----
        (d', beta) = sample_bsdf(m, N, d, b, seed_key)
        if max_component(beta) <= 0: break
        // ---- 4. Russian Roulette (sec. 6) ----
        if b >= RR_START_BOUNCE:
            q = clamp(max_component(beta), 0, 1)
            if q <= 0: break
            if render_rand01(...) >= q: break
            beta /= q
        // ---- 5. advance (offset origin along the outgoing side) ----
        p = P + eps * sign(dot(d', N)) * N     // or eps*d' for transmission
        d = d'
        skip_env = (NEE at this vertex covered the next emitter) ? 1 : 0
    return L
```

Key points:

- **Throughput accumulation**: every contribution is `beta * (radiance)`, so
  the estimator is the standard `L = Σ_b beta_b * Le_b` of a path.
- `max_depth` is the hard bounce cap (`--depth`); RR only *adds* early
  termination beyond `RR_START_BOUNCE`.
- `skip_env` implements the **no-double-count** policy (sec. 5/7): when NEE at
  vertex `b` explicitly sampled the emitters, the continuation ray's emission /
  sky term at `b+1` is suppressed.
- For transmissive surfaces the offset is `p = P + eps*d'` (travel *into* the
  medium) rather than along `N`.

---

## 2. Cosine-weighted hemisphere sampling (Lambertian)

**Sample** (Malley/Kajiya), with `u1,u2 ∈ [0,1)`:

```
r     = sqrt(u1)
phi   = 2*pi*u2
d_loc = ( r*cos(phi), r*sin(phi), sqrt(1 - u1) )      // z = N
d     = r*cos(phi)*T + r*sin(phi)*B + sqrt(1-u1)*N
```

`cosθ = dot(N,d) = sqrt(1-u1)`, and the **pdf is proportional to the cosine**:

```
pdf(d) = cosθ / pi
```

**Lambertian BRDF**: `f = albedo / pi`. The throughput update is

```
beta *= f * cosθ / pdf = (albedo/pi) * cosθ / (cosθ/pi) = albedo
```

i.e. for a cosine-sampled diffuse bounce the multiplier is exactly the albedo —
no residual cosine/pdf ratio, so it is well-conditioned at grazing angles.

**ONB** (`T,B,N` orthonormal): replicate `sky_basis` **verbatim**
(`src/material.c:428-441`, currently file-static): pick the world axis least
parallel to `n`, then `T = normalize(cross(axis,n))`, `B = cross(n,T)`. Expose
it as `sampling_onb()` in `src/sampling.{h,c}` so the frame matches
`material_sample_glossy_dir` / `sky_sun_disk_dir` / `light_sphere_sample_dir`
bit-for-bit (one basis routine project-wide).

A cosine sample always satisfies `dot(N,d) > 0`, so no re-flip is needed.

---

## 3. Glass / water / mirror — Fresnel, Snell, TIR

Dielectric parameters: `ior = m->ior`, `f0 = ((1-ior)/(1+ior))^2`
(symmetric in `n1 ↔ n2`). Incident medium is air when `h.front_face == 1`:

```
eta = (front_face ? 1.0/ior : ior)          // n1/n2
cos_i = clamp(dot(N, V), 0, 1)              // V = -d
sin2_t = eta^2 * (1 - cos_i^2)
```

- **Total internal reflection** `sin2_t > 1`: `F = 1`, always reflect, no
  refraction (no `vec3_refract` call — it would return the zero vector,
  `src/vec3.c:51`).
- Otherwise `cos_t = sqrt(1 - sin2_t)` and Schlick at the **refracted** cosine
  (removes the non-physical discontinuity at the critical angle):

```
F = fresnel_schlick(cos_t, f0) = f0 + (1-f0)*(1-cos_t)^5
```

**Branch selection (unbiased).** Choose reflect vs refract with selection
probability `p_r` (draw `xi`); normalise by the selection probability:

```
if xi < p_r:  d' = reflect(d, N);              beta *= F / p_r
else:         d' = refract(d, N, eta);         beta *= (1-F)/(1-p_r) * eta^2
```

- `eta^2 = (n1/n2)^2` is the **radiance transport factor** for refraction.
- Recommended `p_r = F` (clamped to `[0.05, 0.95]` to keep both branches
  reachable); the expectation is then
  `E[mult] = F*(F/F) + (1-F)*((1-F)/(1-F))*eta^2 = F + (1-F)*eta^2`.
- The brief's per-branch factors **`beta *= F`** (reflect) and
  **`beta *= (1-F)*ior_ratio^2`** (refract) are exactly the physical
  contributions; the code **must** divide by `p_r` (equivalently `1-p_r`) so the
  single-path estimator stays unbiased. With `p_r = F` the reflect weight
  collapses to `1` and the refract weight to `eta^2`.

**Reflectivity mix** (mirrors / `m->reflectivity`, `m->transparency`). Follow
one lobe stochastically and divide by its probability:

```
p_mirror = clamp(m->reflectivity, 0, 1)
p_diel   = clamp(m->transparency, 0, 1)
p_diff   = max(0, 1 - p_mirror - p_diel)
```

- mirror lobe: `d' = reflect(d,N)`, `beta *= reflectivity / p_mirror`
  (for a pure mirror `p_mirror = 1`, so `beta` is unchanged).
- dielectric lobe: sec. 3 above with `p_r` chosen *within* this lobe.
- diffuse lobe: sec. 2, `beta *= albedo / p_diff` with `p_diff` the cosine pdf
  denominator already folded as in sec. 2.

Water: `is_water` selects the wave-perturbed normal (`water_normal`,
`src/material.c:537`), and `is_water || beer_lambert` applies Beer-Lambert
attenuation along the transmitted segment via `water_attenuate(m, inner, depth)`
(`src/material.c:604`), reusing the exit-hit parameter `h2.t` exactly as
`trace_hit` does (`src/render.c:781`).

---

## 4. PBR — lobe selection + GGX importance sampling

**Parameters** (identical conventions to `material_shade_pbr`,
`src/material.c:210`):

```
F0    = lerp(vec3(0.04), albedo, metallic)          // metallic = clamp01(m->metallic)
alpha = material_roughness_to_alpha(roughness)      // = max(roughness^2, 1e-4)
```

**Lobe-selection probability** `p_spec` (diffuse vs specular), a deterministic
function of `roughness`/`metallic`/`F0`:

```
p_spec = clamp01( 0.5*(1 - roughness) + metallic*0.5
                  + (1 - metallic)*luminance(F0)*0.5 )
```

Rationale: smooth surfaces concentrate energy in the specular lobe
(`1 - roughness`), conductors are almost purely specular (`metallic`), and
dielectrics contribute their small `F0 ≈ 0.04` weight. Clamp to `[0.05, 0.95]`
so neither lobe is unreachable. (Tunable heuristic; the estimator stays
unbiased for any `p_spec ∈ (0,1)` because it is divided out below.)

**GGX / Trowbridge-Reitz half-vector sampling** (Karis/UE4, same as
`material_sample_glossy_dir`, `src/material.c:295`):

```
cosθ_h = sqrt( (1-u1) / (1 + (alpha^2 - 1)*u1) )
sinθ_h = sqrt(1 - cosθ_h^2)
phi    = 2*pi*u2
H      = normalize( sinθ_h*cos(phi)*T + sinθ_h*sin(phi)*B + cosθ_h*N )
d'     = reflect(d, H)          // = 2*dot(d,H)*H - d
```

If `dot(d',N) <= 0` (only at very rough grazing hits), fall back to the mirror
direction `reflect(d,N)`. Reflected-direction pdf:

```
pdf(d') = D(H) * cosθ_h / (4 * dot(V,H))
```

with `D = alpha^2 / (pi * ((N·H)^2*(alpha^2-1)+1)^2)`.

**Cook-Torrance specular**:

```
F  = fresnel_schlick_rgb(dot(V,H), F0)              // rgb F0
D  = d_ggx(N·H, alpha)
G  = g_smith_schlick(N·L, N·V, roughness)           // k = (roughness+1)^2/8
f_spec = D*G*F / (4 * (N·L) * (N·V))
```

Specular throughput (`N·L = dot(N,d')`):

```
beta *= f_spec * (N·L) / pdf(d') / p_spec
      = [G * F * dot(V,H) / ((N·V)*(N·H))] / p_spec
```

**Diffuse** (`kD = (1 - F)*(1 - metallic)`, cosine-sampled):

```
f_diff = kD * albedo / pi
beta  *= f_diff * (N·L) / pdf_diff / (1 - p_spec)
       = kD * albedo / (1 - p_spec)
```

Full BRDF for **NEE** at direction `w` is the sum
`f = f_diff + f_spec` (both evaluated at `L = w`), matching
`material_shade_pbr`'s energy split (`src/material.c:255`).

---

## 5. Next Event Estimation (NEE)

At every non-emitter vertex, estimate direct lighting from the explicit
emitters. **One shadow ray per emitter**, radiance folded into the BRDF·cos/pdf
estimator.

### 5.1 Sun disk

```
θ_sun   = scene->sky.sun_radius * (pi/180)          // half-angle, radians
Ω_sun   = 2*pi*(1 - cos(θ_sun))                     // solid angle (≈ pi*θ_sun^2)
w       = sky_sun_disk_dir(sun_dir, sun_radius, u1, u2)   // src/material.c:443
pdf     = 1 / Ω_sun
```

If `dot(N,w) > 0` and the shadow ray from `P + eps*N` toward `w` is unoccluded:

```
L += beta * f(N,V,w) * sun_color * dot(N,w) * Ω_sun
```

(`1/pdf = Ω_sun`; `sun_color` is `Le_sun`, linear.) For a **delta sun**
(`sun_radius <= 0`, the default) `sky_sun_disk_dir` returns `sun_dir` unchanged:
use the directional form `L += beta * f * sun_color * dot(N,w) * V` with no
`Ω_sun` factor.

### 5.2 Emissive spheres

For each `scene->emissive_lights[li]` with `prim_index != h.prim_index`:

```
to_c = C - P ; dc = |to_c| ;  (skip if dc <= R)
w    = to_c / dc
cos_mx = sqrt(1 - (R/dc)^2)
Ω      = 2*pi*(1 - cos_mx) ;  pdf = 1/Ω
w_i  = light_sphere_sample_dir(w, cos_mx, u1, u2)   // src/material.c:479
```

If `dot(N,w_i) > 0` and the nearest hit along `w_i` is that emitter
(`!scene_intersect(...) || sh.prim_index == lt->prim_index`, the robust test
from `emissive_direct`, `src/render.c:552`):

```
L += beta * f(N,V,w_i) * lt->emissive * dot(N,w_i) * Ω
```

### 5.3 No-double-count policy

- NEE covers **all** emitters at each non-emitter vertex, so the continuation
  ray must not re-add those emitters: set `skip_env = 1` after the NEE block
  (sec. 1). When the BSDF ray then hits an emitter, its `Le` is **not** added.
- At an **emitter hit** no NEE is performed (a lamp is not lit by itself), so
  `skip_env = 0` and the next hit may add its emission normally.
- The sun is treated as an explicit emitter: on a BSDF **miss** whose direction
  lies inside the sun cone (`dot(d, sun_dir) >= cos(θ_sun)`) the sky term is
  suppressed, because NEE already sampled the sun at the previous vertex. For
  strict unbiasedness this avoids double counting the sun disk; outside the cone
  `sky_sample` is added in full (sec. 7). For the default delta sun
  (`sun_radius <= 0`) the cone is a single direction and the suppression is a
  no-op.

---

## 6. Russian Roulette (unbiased termination)

Applied only once `b >= RR_START_BOUNCE` (3 or 4):

```
q = clamp( max_component(beta), 0.0, 1.0 )
if q <= 0: terminate
if render_rand01(...) >= q: terminate
beta /= q
```

Unbiasedness: with probability `q` the path survives with throughput
`beta/q`, with probability `1-q` it is killed with throughput `0`:

```
E[beta'] = q * (beta/q) + (1-q) * 0 = beta
```

Using `max_component` (not the luminance) keeps every channel's expected value
intact even for saturated/coloured throughputs, and the `[0,1]` clamp keeps `q`
a valid probability.

---

## 7. Miss / environment

On `scene_intersect` failure the path escapes to the sky:

```
if !skip_env:
    L += beta * sky_sample(d, &scene->sky)     // src/material.c:350
break
```

- `sky_sample` is added **once**, weighted by the full path throughput.
- Suppressed (`skip_env == 1`) when the previous vertex performed NEE, and when
  the miss direction is inside the sun cone (sec. 5.3), so the direct sun disk
  is never counted twice. Outside those cases the environment (horizon gradient
  + glow + clouds) is the unbiased background term.
- No PDF division: `sky_sample` returns radiance directly, and the BSDF/pdf
  factors were already folded into `beta` at the previous vertex.

---

## 8. Determinism

All randomness is a pure function of `(seed_key, bounce, channel)`; never of the
thread, tile or schedule (same discipline as `render.c:877-885`).

- `seed_key = render_hash3((unsigned)x, (unsigned)y, (unsigned)s)` for primary
  ray `s` of pixel `(x,y)` — identical to the existing Whitted dispatch
  (`src/render.c:923`), so the path tracer inherits thread-count independence.
- **Disjoint per-bounce channels**: derive a fresh key per bounce and per draw

```
k_b = render_hash3(seed_key ^ PT_KEY, (unsigned)b, 0u)
u1  = render_rand01(k_b, (unsigned)b, PT_CH_BSDF_A)
u2  = render_rand01(k_b, (unsigned)b, PT_CH_BSDF_B)
... NEE draws PT_CH_NEE_A/B, lobe coin PT_CH_LOBE, RR PT_CH_RR
```

- **Fresh channel constants**, disjoint from every existing stream
  (`0x5a17/0x7c3d` sun-disk, `0x9e37/0x85eb` glossy, `0xE311/0x4c11/0x9d27`
  emissive, `s*2+2/s*2+3` DOF):

```
PT_KEY        0x5054u
PT_CH_BSDF_A  0xB501u
PT_CH_BSDF_B  0xB502u
PT_CH_NEE_A   0x4E01u
PT_CH_NEE_B   0x4E02u
PT_CH_LOBE    0x1001u
PT_CH_RR      0x5201u
PT_CH_GLASS   0x1002u
```

- Per-pixel sample accumulation keeps a **fixed summation order** (the existing
  serial `for (s = 0..spp)` loop), so the float result is bit-reproducible.
- The Whitted renderer's own draws are untouched (no new calls into its
  streams), so `adaptive == 0 && pathtrace == 0` stays byte-identical.

---

## 9. Proposed API

### 9.1 `src/sampling.{h,c}` (new, pure — depends only on `src/vec3.h`)

```c
/* Orthonormal frame (t,b) around unit n; MUST match sky_basis (material.c:428). */
void   sampling_onb(Vec3 n, Vec3 *t, Vec3 *b);

/* Cosine-weighted hemisphere direction around unit n; u1,u2 in [0,1). Unit. */
Vec3   sampling_cosine_hemisphere(Vec3 n, double u1, double u2);
double sampling_cosine_pdf(double cos_theta);        /* cos/pi */

/* Uniform point on the unit sphere (u1,u2 in [0,1)); unit. */
Vec3   sampling_uniform_sphere(double u1, double u2);

/* GGX/Trowbridge-Reitz half-vector around unit n for alpha>0; unit. */
Vec3   sampling_ggx_half(Vec3 n, double alpha, double u1, double u2);
/* Reflected-direction pdf of the GGX sample: D*cos_h / (4*dot(V,H)). */
double sampling_ggx_reflect_pdf(Vec3 n, Vec3 V, Vec3 H, double alpha);

/* Fresnel */
double sampling_fresnel_schlick(double cos_theta, double f0);       /* Schlick */
double sampling_fresnel_dielectric(double cos_i, double ior);       /* F0=((1-ior)/(1+ior))^2 */

/* Snell refraction with TIR: returns 1 on success (unit *out), 0 on TIR. */
int    sampling_refract(Vec3 d, Vec3 n, double eta, Vec3 *out);

/* PBR lobe-selection probability p_spec from roughness/metallic (sec. 4). */
double sampling_lobe_prob_spec(double roughness, double metallic, Vec3 f0);
```

All functions are pure (no globals, no I/O, no allocation) and deterministic for
fixed `render_rand01`-derived inputs — unit-testable in isolation (t-106).

### 9.2 `src/pathtrace.{h,c}` (new)

```c
/* Unbiased unidirectional path-trace estimate of the radiance arriving along
 * `r` (r.dir unit). `max_depth` = bounce cap; `seed_key` = deterministic
 * per-primary-ray key (render_hash3(x,y,s)). Pure w.r.t. the scene. */
Vec3 pathtrace_radiance(const Scene *scene, Ray r, int max_depth,
                        unsigned seed_key);
```

Uses `scene_intersect`, `scene_material`, `water_normal`, `water_attenuate`,
`texture_albedo`, `sky_sample`, `sky_sun_disk_dir`, `light_sphere_sample_dir`,
`material_roughness_to_alpha`, `fresnel_schlick(_rgb)`, `vec3_reflect`,
`vec3_refract`, the public `render_rand01`/`render_hash3`, and
`src/sampling.{h,c}`. Returns linear radiance (gamma/quantize downstream).

### 9.3 Integration touch-points (owned by t-105, not here)

- `RenderParams` gains `int pathtrace;` (`src/render.h`); `render_region`'s
  per-sample dispatch (`src/render.c:926`) and the adaptive dispatch
  (`src/render.c:1179`) select `pathtrace_radiance` when set, else the existing
  `trace()` — so `pathtrace == 0` is byte-identical.
- `main.c` adds a value-less `--pathtrace` flag mirroring `--adaptive`
  (`src/main.c:191-199`).
- Makefile `SRCS` gains `src/sampling.c` (t-103) and `src/pathtrace.c` (t-104).

---

## References (verified in-tree)

- `src/material.h` — `Material`, `SkyParams`, `EmissiveLight` consumers,
  `fresnel_schlick`, `material_roughness_to_alpha`, `material_sample_glossy_dir`,
  `sky_sample`, `sky_sun_disk_dir`, `light_sphere_sample_dir`, `water_normal`,
  `water_attenuate`.
- `src/material.c:157,165` Fresnel; `:197` `material_roughness_to_alpha`;
  `:255` `material_shade_pbr` (kD/energy split); `:283` glossy GGX sampler;
  `:350` `sky_sample`; `:428` `sky_basis`; `:443` `sky_sun_disk_dir`;
  `:479` `light_sphere_sample_dir`; `:537` `water_normal`; `:604`
  `water_attenuate`.
- `src/vec3.c:51` `vec3_refract` (zero vector on TIR); `src/vec3.h:103`
  `vec3_reflect`.
- `src/render.c:380,390` `render_hash3`/`render_rand01`; `:505` `emissive_direct`
  (NEE solid-angle form, visibility test); `:637` sun-disk channels; `:433-458`
  glossy/emissive channels; `:781` Beer-Lambert via exit hit; `:923` `seed_key`;
  `:926` Whitted dispatch call site.
- `src/geometry.h` — `Hit` (flipped unit normal, `front_face`).
- `docs/research_pathtrace_integration.md` (t-101) — integration map.
- `.marmel/execution_plan.md` — t-103…t-109 downstream tasks.

MISSION COMPLETE (t-102)
