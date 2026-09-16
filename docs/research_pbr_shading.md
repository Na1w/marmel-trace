# Research: Opt-in physically-based metallic/roughness BRDF (Cook-Torrance)

**Scope.** Read-only investigation. This document specifies an **opt-in**,
energy-conserving microfacet (Cook-Torrance) shading path for the C11
zero-dependency Whitted raytracer, designed so that the existing Blinn-Phong
path — and therefore the default render — stays **byte-identical**.

Nothing here is implemented; it is a design + wiring plan. All file paths and
line numbers refer to the tree at the time of writing (CWD
`/Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer`).

---

## 1. Current local shading model (`src/material.c`)

### 1.1 `material_shade_local` — `src/material.c:122-153`
Declared at `src/material.h:145`. Signature:
`Vec3 material_shade_local(const Material *m, Vec3 N, Vec3 L, Vec3 V, Vec3 light_color)`.

- Early-out when the light is behind the surface: `ndl = dot(N,L)`, `ndl <= 0`
  returns black (`src/material.c:129-133`).
- **Diffuse** (`src/material.c:136`):
  `diffuse = (m->albedo ⊙ light_color) * ndl` — i.e. `albedo * L * NdotL`.
- **Specular, Blinn-Phong** (`src/material.c:139-150`): `h = L + V`; if
  `|h|² > 1e-12`, `H = normalize(h)`, `ndh = dot(N,H)`; if `ndh > 0`,
  `spec = (m->specular ⊙ light_color) * pow(ndh, m->shininess)` using the
  guarded `pow_nonneg` helper (`src/material.c:44-48`).
- Returns `diffuse + spec` (`src/material.c:152`).

**π convention (explicit):** the existing diffuse is `albedo * L * NdotL`; it
has **no `1/π` factor**. A true Lambert BRDF is `albedo/π`, so the current code
implicitly folds the `1/π` into the light/colour normalisation (i.e. it treats
`albedo` as the already-radiance-scaled diffuse response). This matters for §4.2.

### 1.2 `material_ambient` — `src/material.c:155-178`
Declared at `src/material.h:149`.
- Ground/sky hemisphere colours derived from `sky->horizon_color * 0.45` and
  `sky->zenith_color` (`src/material.c:165-169`).
- `w = clamp01(0.5 + 0.5 * N.y)`, `hemi = lerp(groundish, skyish, w)`
  (`src/material.c:171-172`).
- Returns `(m->albedo ⊙ hemi) * 0.15` (`ambient_factor`, `src/material.c:175-177`).

### 1.3 Fresnel — already present and reusable
- `double fresnel_schlick(double cos_theta, double f0)` — decl
  `src/material.h:133`, def `src/material.c:99-105`. Clamps `cos` to `[0,1]`,
  computes `f0 + (1-f0)*(1-cos)^5`.
- `Vec3 fresnel_schlick_rgb(double cos_theta, Vec3 f0)` — decl
  `src/material.h:136`, def `src/material.c:107-115`. Same, per channel.
  **This is the exact function the PBR specular term should reuse** (`F`).

---

## 2. Call sites in `src/render.c` `trace_hit` (`src/render.c:124-287`)

Setup:
- `m = scene_material(scene, h->material_index)` (`:131`).
- Water normal override (`:138-140`); re-flip safety (`:141-143`);
  `V = -r.dir` (`:144`).
- **Local material copy for texturing** (`:156-158`):
  `Material m_local = *m; m_local.albedo = texture_albedo(m, P); const Material *mm = &m_local;`
  Only `albedo` is modulated; every physical field is copied verbatim.

Direct sun term:
- Ambient seed (`:171`): `Vec3 color = material_ambient(mm, N, &scene->sky);`
- `L = scene->sky.sun_dir` (`:172`); `shadow_o = P + N*1e-3` (`:173`).
- **Soft-shadow variant** (`sun_radius > SUN_RADIUS_EPSILON`, `:175-192`):
  `color += material_shade_local(mm, N, L, V, scene->sky.sun_color) * (lit/16)`
  (`:189-191`).
- **Hard-shadow variant** (`:193-199`):
  `color += material_shade_local(mm, N, L, V, scene->sky.sun_color)` (`:196-197`).

Reflection (`:201-208`):
- `refl_col = trace(scene, (Ray){P + N*1e-3, reflect(r.dir,N)}, depth+1, ...)`.

Fresnel / refraction (`:210-244`):
- `f0 = ((1-m->ior)/(1+m->ior))^2` (`:220-221`) — note this is the **same**
  dielectric F0 formula the PBR path needs for `ior`.
- `eta = front_face ? 1/ior : ior` (`:230`); `sin2_t` (`:233`); TIR (`:237-240`);
  else `F = fresnel_schlick(cos_t, f0)` (`:242-243`).

Transmission (`m->transparency > 0`, `:246-283`):
- Refracted ray `vec3_refract` (`:254-255`); reuse exit hit `h2` (`:256-261`);
  Beer-Lambert via `water_attenuate(m, inner, h2.t)` when
  `m->is_water || m->beer_lambert` (`:262-269`, sky-exit fallback `:270-278`).
- `surface = refl_col*F + refr_col*(1-F)` (`:281-282`);
  `return vec3_lerp(color, surface, m->transparency)` (`:283`).

Opaque return (`:286`):
- `return vec3_lerp(color, refl_col, m->reflectivity);`

Interactions today: `specular`/`shininess` only feed the direct-light term;
`reflectivity` mixes the recursively-traced mirror term; `transparency` (with
`ior`) replaces the mirror mix with a Fresnel-weighted reflection/refraction
blend. `reflectivity` is **ignored** when `transparency > 0` (the `:246` branch
returns first). Ambient uses only `albedo`.

---

## 3. `Material` struct layout (`src/material.h:52-81`)

| Field | Line | Type | Role |
|---|---|---|---|
| `albedo` | 53 | Vec3 | diffuse / base colour |
| `specular` | 54 | Vec3 | Blinn-Phong specular tint |
| `shininess` | 55 | double | Blinn-Phong exponent |
| `reflectivity` | 56 | double | 0..1 mirror mix |
| `transparency` | 57 | double | 0..1 transmissive mix |
| `ior` | 58 | double | index of refraction |
| `is_water` | 59 | int | wave-normal + depth tint |
| `absorption` | 60 | Vec3 | per-channel Beer-Lambert |
| `deep_color` | 61 | Vec3 | deep medium colour |
| `beer_lambert` | 74 | int | opt-in attenuation |
| `texture_kind` | 77 | int | `TextureKind` (0 none/1 checker/2 stripes) |
| `texture_scale` | 78 | double | world units per cell |
| `texture_color_a` | 79 | Vec3 | cell/band A |
| `texture_color_b` | 80 | Vec3 | cell/band B |

`TextureKind` enum at `src/material.h:33-37`; defaults `TEXTURE_DEFAULT_*` at
`src/material.h:43-46`; `material_texture_defaults` def `src/material.c:84-92`.

**Texture modulation:** done in `render.c:156-157` on the local copy `mm` via
`texture_albedo` (`src/texture.c:62-97`). For `TEXTURE_NONE` it returns
`m->albedo` bit-for-bit (`src/texture.c:92-95`). Consequence for PBR: any
`F0 = mix(0.04, albedo, metallic)` must use **`mm->albedo`** (the modulated
value), so a metallic checker material gets per-cell F0. Note the interaction:
with `metallic > 0`, the diffuse lobe vanishes and the texture only modulates
F0/tint, not the diffuse — document this.

Copy paths that carry new fields for free: `scene.c:562` whole-struct copy
(`mats[i] = d->materials[i].mat;`) and `render.c:156` (`m_local = *m`).

---

## 4. Design

### 4.1 Cook-Torrance specular
For one light with incoming colour `Lc` and `NdotL = max(dot(N,L), 0)`:

```
f_spec = D * G * F / (4 * NdotL * NdotV)
L_o,spec = Lc * NdotL * f_spec
```

with `NdotV = max(dot(N,V), 0)`.

**GGX / Trowbridge-Reitz D** (let `a = alpha`):
```
denom = NdotH*NdotH * (a*a - 1) + 1        // >= a*a > 0 for a > 0
D = a*a / (PI * denom*denom)
```
`a` is derived from perceptual roughness (§4.4). The denominator is strictly
positive for `a > 0`, so no explicit divide-by-zero guard is needed beyond the
`a` clamp.

**Smith-Schlick-GGX visibility G** (direct lighting, per the brief):
```
k = (roughness + 1)^2 / 8                  // roughness, not alpha
Gv(L) = NdotL / (NdotL * (1 - k) + k)
Gv(V) = NdotV / (NdotV * (1 - k) + k)
G = Gv(L) * Gv(V)
```
Guards: if `NdotL <= 0` return black (same early-out as `material_shade_local`);
`k` clamped to `[0, 1]`; the visibility denominators are `>= k >= 0`, and with
`NdotL,NdotV >= 0` they are positive whenever `k > 0`.

**Fresnel F** — reuse the existing helper:
```
F = fresnel_schlick_rgb(clamp(NdotV, 0, 1), F0)   // src/material.c:107-115
```
(The brief specifies `fresnel_schlick_rgb(cos, F0)`; the recommended `cos` is
`NdotV`. Using `dot(V,H)` instead is the Filament variant — pick one and document
it; `NdotV` is simpler and needs no extra normalisation.)

### 4.2 Diffuse
Proposed convention (physically consistent with §4.1):

```
f_diff = (1 - metallic) * albedo / PI
L_o,diff = Lc * NdotL * f_diff
```

Because §1.1 has **no** `1/π`, a PBR material's diffuse will be `π×` darker
than the equivalent Blinn-Phong material for the same `albedo`. This is the
**correct** relative weighting against a unit-integral `f_spec`; the opt-in
scene is expected to compensate with a brighter `sun_color`/sky (or accept the
darker, more physically plausible look). If brightness parity is required
instead, the alternative is `f_diff = (1-metallic)*albedo` (drop the `1/π`),
which is energy-inconsistent with `f_spec` but visually matches the legacy
path. **Recommendation: use `albedo/π` and document the brightness shift**; do
not silently mix the two conventions.

Energy budget: with `F0` dielectric and `metallic ∈ [0,1]`, `f_diff` is scaled
by `(1-metallic)` so a pure metal (`metallic = 1`) has zero diffuse and a
dielectric keeps a full Lambert lobe — the standard metallic/roughness split.

### 4.3 F0
```
F0 = mix(vec3(0.04), albedo, metallic)          // per channel
```
- Dielectric `F0 = 0.04` corresponds to `ior = 1.5`, from
  `F0 = ((ior-1)/(ior+1))^2` — the **same** expression already used at
  `src/render.c:220-221`. If the implementer wants `F0` tied to the material's
  `ior`, use `f0 = ((ior-1)/(ior+1))^2` clamped to `[0,1]`, falling back to
  `0.04` when `ior <= 1`. Recommend the simple constant `0.04` for v1 and treat
  `ior`-derived F0 as a follow-up (keeps the two paths decoupled).
- `metallic` is clamped to `[0,1]`.

### 4.4 Roughness → alpha
```
a = max(roughness * roughness, A_MIN)     // A_MIN = 1e-4
```
- Squared remapping (Disney/GGX convention) gives perceptually linear roughness.
- `roughness = 0` would give `a = 0` and a `0/0` in `D` at `NdotH = 1`; clamping
  `a >= A_MIN` keeps `D` finite and makes `D` peak at `a^2/(π a^2) = 1/π` at
  `NdotH = 1`, the correct mirror limit.
- Clamp `roughness` to `[0,1]` on input (parser/writer also enforce).

### 4.5 Emissive
```
L_o += emissive          // added ONCE per shaded hit, not per light
```
Add it **after** the reflection/transmission mix at the end of `trace_hit`
(§5, step 3), so it is independent of `reflectivity`/`transparency` and not
scaled by the light loop. `emissive = (0,0,0)` must be an exact no-op.

### 4.6 Combined radiance
```
color = ambient                                   // existing material_ambient (see §5 note)
color += Lc * NdotL * (f_diff + f_spec)           // per visible light (sun, weighted by visibility)
out   = mix-path(color, refl_col / refr_col)      // UNCHANGED (render.c:246-286)
out  += emissive                                  // once
```

---

## 5. GATING — guarantee byte-identity on the non-PBR path

Add one field, defaulting to the legacy path:

```
int pbr;      /* 0 = legacy Blinn-Phong (default), 1 = Cook-Torrance */
```

**Branch placement (`src/render.c`).** Keep the existing
`material_shade_local(...)` calls **textually intact** inside an `else`; do not
refactor them into a shared helper (re-association could perturb FP rounding):

- Soft-shadow site (`:187-192`): replace the body of the `if (lit > 0.0)` block:
  ```
  if (mm->pbr) {
      color = vec3_add(color, vec3_scale(
          material_shade_pbr(mm, N, L, V, scene->sky.sun_color),
          lit / (double)SUN_SHADOW_SAMPLES));
  } else {
      color = vec3_add(color, vec3_scale(
          material_shade_local(mm, N, L, V, scene->sky.sun_color),
          lit / (double)SUN_SHADOW_SAMPLES));      /* identical to today */
  }
  ```
- Hard-shadow site (`:195-198`): same shape around the existing
  `material_shade_local` add.
- Ambient (`:171`): **leave unchanged** for v1 (`material_ambient` is albedo-based
  and shared by both paths). Optionally add a `material_ambient_pbr` that scales
  by `(1-metallic)*albedo/π`; if added, gate it with the same `if (mm->pbr)` so
  the legacy arithmetic is untouched.
- Emissive (`:283` and `:286`): add at the very end, guarded so zero emissive
  returns the exact original expression:
  ```
  /* transmission path */
  Vec3 out = vec3_lerp(color, surface, m->transparency);
  if (m->pbr && (m->emissive.x != 0 || m->emissive.y != 0 || m->emissive.z != 0))
      out = vec3_add(out, m->emissive);
  return out;
  ```
  and analogously `vec3_lerp(color, refl_col, m->reflectivity)` for the opaque
  return. (Guard with `m->pbr` so the legacy path's `return` is byte-identical
  even for a hypothetical non-zero emissive on a non-PBR material.)

**New function** `material_shade_pbr` lives in `material.c` alongside
`material_shade_local`; it must be pure (no globals, no I/O) to preserve the
thread/tile determinism documented at `src/render.c:437` (render_notes) and the
byte-identical determinism asserted at `tests/test_integration.c:271`.

**Why the legacy path stays byte-identical:** `pbr` is 0 for every existing and
preset material (zero-init, §7), so the `else` branch executes the *same
statements in the same order* as today; the only added operation is an integer
comparison, which performs no floating-point work. All new FP is confined to
`material_shade_pbr`, reachable only when `pbr == 1`.

---

## 6. Byte-identity checklist

| # | Site | Requirement |
|---|---|---|
| 1 | `src/material.c:122-153` | `material_shade_local` body/order unchanged |
| 2 | `src/material.c:155-178` | `material_ambient` body unchanged (new PBR ambient = separate function) |
| 3 | `src/render.c:171` | ambient call unchanged |
| 4 | `src/render.c:189-191` | soft-shadow term: legacy expression preserved in `else` |
| 5 | `src/render.c:196-197` | hard-shadow term: legacy expression preserved in `else` |
| 6 | `src/render.c:201-208` | reflection recursion unchanged |
| 7 | `src/render.c:220-244` | Fresnel/TIR math unchanged |
| 8 | `src/render.c:246-283` | transmission block unchanged |
| 9 | `src/render.c:286` | opaque return; emissive add guarded by `pbr` + non-zero |
| 10 | `src/scene.c:146-200` | `memset` (`:156`) zeroes new fields → `pbr=0, metallic=0, roughness=0, emissive=0` |
| 11 | `src/scene.c:562` | whole-struct copy carries new fields verbatim |
| 12 | `src/scene_desc.c:474-509` | new `MAT_*` bits + `MAT_KEYS` rows; defaults zero |
| 13 | `src/scene_desc.c:515-570` | presets (`memset` first) keep new fields at defaults |
| 14 | `src/scene_desc.c:923-939` | `sd_overlay_explicit` copies new fields only when `seen` |
| 15 | `src/scene_desc_write.c:292-326` | `mat_is_*_preset` must also require new fields == default, else presets stop matching |
| 16 | `src/scene_desc_write.c:439-471` | emit new keys **only** when non-default |

**Zero-init safety:** every new field is an `int`/`double`/`Vec3` with a
zero-representable default (`pbr=0`, `metallic=0`, `roughness=0`,
`emissive=(0,0,0)`), so `memset(&m,0,sizeof m)` at `src/scene.c:156`,
`src/scene_desc.c:519/546/566` and the `calloc` at `src/scene.c:554` all yield
the legacy path. No binary/memcmp serialisation depends on `sizeof(Material)`
(grep found only `memset`/`calloc` uses), so adding a field is safe.

---

## 7. Proposed implementation checklist (not executed)

**(a) Schema / parser / writer**
1. `src/material.h` — append to `Material` (after `texture_color_b`, `:80`):
   `int pbr; double metallic; double roughness; Vec3 emissive;`
   Add `#define MATERIAL_DEFAULT_PBR 0`, `MATERIAL_DEFAULT_METALLIC 0.0`,
   `MATERIAL_DEFAULT_ROUGHNESS 0.0`, `MATERIAL_DEFAULT_EMISSIVE ((Vec3){0,0,0})`.
   Declare `Vec3 material_shade_pbr(const Material*, Vec3 N, Vec3 L, Vec3 V, Vec3 light_color);`
   near `src/material.h:145`.
2. `src/material.c` — add `material_pbr_defaults(Material*)` (mirrors
   `material_texture_defaults`, `:84`); call it where
   `material_texture_defaults` is called (`src/scene.c:200`,
   `src/scene_desc.c:529/557/568`). Add `material_shade_pbr` + static
   `d_ggx`, `g_smith_schlick`, `roughness_to_alpha`.
3. `src/scene_desc.c` — extend the enum (`:474-493`) with
   `MAT_PBR = 1u<<15`, `MAT_METALLIC = 1u<<16`, `MAT_ROUGHNESS = 1u<<17`,
   `MAT_EMISSIVE = 1u<<18` (bit 15 is free; bits 20/21/30 are used elsewhere,
   `:685/686/1034`). Add `MAT_KEYS` rows (`:495-509`) using `KT_BOOL`
   (`sd_val_bool`, `:798`) and `KT_DOUBLE`/`KT_VEC3`. Add the four lines to
   `sd_overlay_explicit` (`:923-939`).
4. `src/scene_desc_write.c` — add the new fields to the default checks in
   `mat_is_opaque_preset` (`:292`), `mat_is_water_preset` (`:303`),
   `mat_is_glass_preset` (`:316`) so presets still match; in `emit_material`
   (`:439-471`) emit `pbr`/`metallic`/`roughness`/`emissive` **only when
   non-default** (pattern of `beer_lambert`, `:461-465`).
5. `docs/scene_format.md` §4.3 table (`:234-250`) — document the new keys,
   ranges, defaults and the byte-identity rule.

**(b) BRDF in `material.c`**
6. `material_shade_pbr`: `NdotL <= 0` early-out; compute `NdotH`, `NdotV`;
   `a = roughness_to_alpha(m->roughness)`; `D = d_ggx(NdotH,a)`;
   `G = g_smith_schlick(NdotL,NdotV,m->roughness)`;
   `F0 = mix(0.04, albedo, metallic)`;
   `F = fresnel_schlick_rgb(NdotV, F0)`;
   `f_spec = D*G*F/(4*NdotL*NdotV)`; `f_diff = (1-metallic)*albedo/PI`;
   return `Lc * NdotL * (f_diff + f_spec)`.

**(c) `render.c` wiring**
7. Guarded `if (mm->pbr)` branches at `:187-192` and `:195-198` (§5).
8. Guarded emissive add at the two return sites (`:283`, `:286`).
9. (Optional) `material_ambient_pbr` gated at `:171`.

**Verification hooks:** re-run `tests/run_integration.sh` (exact BMP size
assertion) and `tests/test_integration.c:271` (two-render byte-identity); add a
new test asserting that a scene with `pbr = 0` (and an absent key) renders
identically to a pre-change baseline, and that `pbr = 1` is deterministic
across thread counts (`tests/test_render_threads.c`).

---

## 8. References

- `src/material.h:33-46,52-81,99-149` — `TextureKind`, `Material`, defaults,
  Fresnel/shading prototypes.
- `src/material.c:84-178` — texture defaults, Fresnel, `material_shade_local`,
  `material_ambient`.
- `src/render.c:124-287` — `trace_hit` (all shading call sites).
- `src/texture.c:62-97` — `texture_albedo` (albedo modulation, `TEXTURE_NONE`
  identity at `:92-95`).
- `src/scene.c:146-200,554-566` — built-in materials, `memset`/`calloc`,
  whole-struct copy.
- `src/scene_desc.c:474-570,798-811,923-1027` — material keys, presets,
  `sd_val_bool`, overlay, close.
- `src/scene_desc_write.c:281-326,410-471` — default checks and emitters.
- `docs/scene_format.md:229-303` — §4.3 material block contract.
- `docs/render_notes.md:455-473` — "No global illumination" and
  "does not conserve energy" notes (motivation for the opt-in PBR path).
- `tests/test_integration.c:271` — byte-identical determinism assertion;
  `tests/run_integration.sh` — exact output-size smoke test.

MISSION COMPLETE
