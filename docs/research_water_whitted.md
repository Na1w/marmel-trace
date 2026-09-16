# Whitted Water Path — Code Map & Quotes

Scope: non-pathtrace (Whitted) water path only. Sources: `src/render.c`,
`src/render.h`, `src/material.h`, `src/material.c` (helpers), `src/scene.c`.
Facts only; no correctness verdict, no fixes.

## 1. `Material` fields (`src/material.h`, struct 52–95)

Water-relevant fields: `albedo` 53, `specular` 54, `shininess` 55,
`reflectivity` 56 (0..1 mirror mix), `transparency` 57 (0..1), `ior` 58 (1.33 for
water), `is_water` 59 (wave normal + depth tint), `absorption` 60 (per-channel
Beer-Lambert coeff), `deep_color` 61, `beer_lambert` 74 (attenuate transmission
path), plus opt-in PBR `metallic/roughness/emissive/pbr` 83–88.

There is **no `type` field** on `Material`; `type = water` in a scene file is a
preset name resolved by the parser/writer (`src/scene_desc_write.c:314`).
`Scene` has `double water_level` (scene.h:66) and `int water_material`
(scene.h:67) — neither is read in `render.c`.

Prototypes: `water_normal` material.h:325, `water_attenuate` 338,
`water_depth_tint` 343 (deprecated wrapper).

## 2. `water_normal` — `src/material.c:537–583`

`Vec3 water_normal(double x, double z, double time)`: 4 directional sine waves
(analytic derivatives) + small fBm ripple (central differences), then height-field
normal `normalize(-dhdx, 1, -dhdz)`.

```c
543:     for (int i = 0; i < WATER_NWAVES; ++i) {
544:         const Wave *wv = &WATER_WAVES[i];
545:         double u = (wv->ax * x + wv->az * z) * wv->w
546:                  + wv->phase + time * wv->speed * wv->w;
547:         double c = cos(u) * wv->amp * wv->w;
548:         dhdx += c * wv->ax;
549:         dhdz += c * wv->az;
550:     }
553:     const double freq = 0.35;      /* hard-coded fBm ripple frequency */
554:     const double namp = 0.06;      /* hard-coded ripple amplitude     */
560:     const double e = 0.02;         /* central-difference step, metres */
561:     const unsigned seed = 1337u;   /* hard-coded noise seed           */
563:     double hL = noise_fbm2((x - e) * freq, z * freq, 4, 2.0, 0.5, seed);
572:     Vec3 n = vec3_normalize(vec3(-dhdx, 1.0, -dhdz));
579:     if (n.y < 0.0) { n = vec3_neg(n); }
582:     return n;
```

`WATER_WAVES` (material.c:527–533; `Wave` struct 518–524, fields
`ax, az, w(=2*PI/lambda), amp, speed, phase`):
```c
529: { 0.9584,  0.2855, 2.0*MATERIAL_PI/24.0, 0.1800, 1.0, 0.0 },
530: { 0.7071, -0.7071, 2.0*MATERIAL_PI/13.0, 0.1000, 1.4, 1.7 },
531: { -0.4061, 0.9138, 2.0*MATERIAL_PI/ 7.0, 0.0500, 2.0, 3.1 },
532: { 0.2005,  0.9797, 2.0*MATERIAL_PI/ 4.0, 0.0250, 2.6, 0.6 },
```

**Call site in the Whitted path — exactly one**, top of `trace_hit()`:
```c
596:     if (m->is_water) {
597:         N = water_normal(P.x, P.z, time); /* wave-perturbed normal */
598:     }
599:     if (vec3_dot(N, r.dir) > 0) {
600:         N = vec3_neg(N); /* safety re-flip */
601:     }
```
Applied only when `is_water != 0`; `time` is threaded through `trace()` /
`trace_hit()` (render.c:582–583, 818–819).

## 3. `water_attenuate` — `src/material.c:604–633`

```c
604: Vec3 water_attenuate(const Material *m, Vec3 inner, double depth)
605: {
606:     if (m == NULL) { return vec3(0.0, 0.0, 0.0); }
611:     double d = depth > 0.0 ? depth : 0.0;              /* clamp at 0 */
614:     Vec3 tr = vec3(water_transmittance(m->absorption.x, d),
615:                    water_transmittance(m->absorption.y, d),
616:                    water_transmittance(m->absorption.z, d));
623:     Vec3 out = vec3(inner.x * tr.x + m->deep_color.x * (1.0 - tr.x),
624:                     inner.y * tr.y + m->deep_color.y * (1.0 - tr.y),
625:                     inner.z * tr.z + m->deep_color.z * (1.0 - tr.z));
628:     if (!(out.x >= 0.0)) out.x = 0.0;   /* finite/non-negative guard */
632:     return out;
```
`water_transmittance` (material.c:589–602): `T = exp(-absorption * depth)`;
`absorption == 0` => `T = 1`; clamped to [0,1].

**Call sites in the Whitted path — exactly two**, in the refraction branch:
```c
777:                 if (m->is_water || m->beer_lambert) {
781:                     refr_col = water_attenuate(m, inner, h2.t);   /* exit hit */
787:                 if (m->is_water || m->beer_lambert) {
790:                     refr_col = water_attenuate(m, inner, 5.0);    /* no hit   */
```
`5.0` is a **hard-coded "representative deep water" path length in metres**
(comment render.c:788–789). `water_depth_tint` has no Whitted call sites;
`pathtrace.c` also calls both helpers — out of scope.

## 4. Surface-colour combination — `trace_hit()` (`src/render.c:582–816`)

1. **Albedo modulation** (614–616): `Material m_local = *m;`
   `m_local.albedo = texture_albedo(m, P); const Material *mm = &m_local;` —
   physical fields (`ior, transparency, reflectivity, is_water, beer_lambert,
   absorption, deep_color`) copied verbatim.
2. **Direct sun** (629–671): `color = material_ambient(mm, N, &scene->sky)` +
   hard-shadow (660–671) or soft sun-disk (633–659) Blinn-Phong/PBR term.
3. **Emissive area lights** (682–685, gated on `scene->emissive_light_count > 0`).
4. **Reflection** (687–723): sharp mirror `vec3_reflect` (718–721), unless
   `pbr && reflectivity > 0 && roughness > GLOSSY_ROUGHNESS_EPSILON` (697–698),
   which averages `GLOSSY_REFLECTION_SAMPLES` jittered samples (700–716); both
   offset the ray by `vec3_scale(N, 1e-3)`.
5. **Fresnel / TIR** (725–759): `f0 = ((1-ior)/(1+ior))^2` (735–736);
   `cos_i = clamp(dot(N,V),0,1)` (738–743); `eta = front_face ? 1/ior : ior`
   (745); `sin2_t = eta^2*(1-cos_i^2)` (748); `sin2_t > 1` => TIR, `F = 1`, else
   `F = fresnel_schlick(cos_t, f0)` (752–759).
6. **Refraction + attenuation** (761–795), gated on `m->transparency > 0.0`:
   TIR => `refr_col = refl_col` (765); else refract (769–770), intersect the
   transmitted ray (772), shade the exit hit via `trace_hit` (775–776), then
   `water_attenuate(m, inner, h2.t)` when `is_water || beer_lambert` (781), else
   raw `inner` (783). No exit hit => `sky_sample` (786), then
   `water_attenuate(m, inner, 5.0)` (790) or raw (792).
7. **Final combine** (796–806):
```c
796:         Vec3 surface = vec3_add(vec3_scale(refl_col, F),
797:                                 vec3_scale(refr_col, 1.0 - F));
798:         Vec3 out = vec3_lerp(color, surface, m->transparency);
```
   Fresnel-weighted mix of reflection and (attenuated) refraction, then blended
   with local `color` by `transparency`; optional self-emission (802–805).
8. **Opaque path** (809–815): `out = vec3_lerp(color, refl_col, m->reflectivity)`
   — only when `transparency <= 0`, so **not** taken by water (default 0.85).

Blue tint enters via `deep_color` + per-channel `absorption` inside
`water_attenuate`; reflection is mixed in with Fresnel `F`. There is **no
separate explicit "water colour RGB" constant** in `render.c`.

Default water material (`src/scene.c:183–190`): `specular = (0.90,0.90,0.90)`,
`shininess = 256.0`, `reflectivity = 1.0`, `transparency = 0.85`, `ior = 1.33`,
`is_water = 1`, `absorption = (0.45, 0.12, 0.06)`,
`deep_color = (0.02, 0.10, 0.16)`.

## 5. Tone mapping / quantization — `to_byte` (`src/render.c:859–875`)

```c
859: /* Gamma-encode one linear channel and quantize to an 8-bit byte. */
860: static unsigned char to_byte(double c)
861: {
862:     if (c < 0.0) {
863:         c = 0.0;
864:     } else if (c > 1.0) {
865:         c = 1.0;
866:     }
867:     double v = pow(c, 1.0 / 2.2) * 255.0;
868:     int iv = (int)(v + 0.5);
869:     if (iv < 0) { iv = 0; } else if (iv > 255) { iv = 255; }
874:     return (unsigned char)iv;
875: }
```
Clamp linear `c` to [0,1] first, gamma `^(1/2.2)`, scale by 255, round via `+0.5`,
clamp int to [0,255]. **No exposure parameter and no tone-mapping operator** —
pure clamp-then-gamma-2.2; the clamp at 1.0 discards values > 1.0.

**Call sites:** fixed-path pixel loop render.c:928–932
(`acc = vec3_scale(acc, 1.0/spp); p[0..2] = to_byte(acc.x/y/z)`); adaptive
resolve render.c:1332–1343 (`to_byte(im->sumR/G/B[p] * inv)`). Both average the
accumulated linear samples first, then quantize once per pixel.

## 6. Hard-coded constants

`WATER_WAVES` table (material.c:527–533); ripple `freq = 0.35`, `namp = 0.06`,
step `e = 0.02` m, seed `1337u`, `noise_fbm2(x, z, 4, 2.0, 0.5, seed)`
(material.c:553–566); no-hit water path `5.0` m (render.c:790); Fresnel
`f0 = ((1-ior)/(1+ior))^2` ~0.02 for water (render.c:735–736); gamma `1/2.2`,
scale `255.0`, round `+0.5` (render.c:867–868); default water `ior = 1.33`,
`absorption = (0.45,0.12,0.06)`, `deep_color = (0.02,0.10,0.16)`,
`transparency = 0.85`, `reflectivity = 1.0` (scene.c:185–190).

## 7. Function / line-range map

| Function | Lines |
|---|---|
| `trace_hit` (Whitted shading) | render.c:582–816 |
| — water normal override | render.c:596–601 |
| — albedo/texture local copy | render.c:614–616 |
| — direct sun (hard/soft) | render.c:629–671 |
| — emissive area lights | render.c:682–685 |
| — reflection (sharp/glossy) | render.c:687–723 |
| — Fresnel / TIR | render.c:725–759 |
| — refraction + `water_attenuate` calls | render.c:761–795 |
| — final surface combine / opaque combine | render.c:796–806 / 809–815 |
| `trace` | render.c:818–831 |
| `to_byte` | render.c:859–875 |
| fixed-path pixel loop (`render_region`) | render.c:892–938 |
| `adaptive_resolve` (uses `to_byte`) | render.c:1332–1343 |
| `water_normal` / `water_transmittance` | material.c:537–583 / 589–602 |
| `water_attenuate` | material.c:604–633 |
| `water_depth_tint` (deprecated wrapper) | material.c:635–641 |
