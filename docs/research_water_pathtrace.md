# Research: Water handling in the path tracer (`src/pathtrace.c`)

Scope: PATH-TRACER water handling ONLY (`src/pathtrace.c` + `src/pathtrace.h`).
`src/render.c` out of scope. Facts only — no verdict, no fix proposals.
Line numbers are exact for `src/pathtrace.c` unless stated otherwise.

## 1. Entry points (`src/pathtrace.h`)

- `Vec3 pathtrace_radiance(const Scene*, Ray, int max_depth, unsigned seed_key)`
  — decl `pathtrace.h:61-62`, def `pathtrace.c:551`.
- `unsigned char pathtrace_to_byte(double linear)` — decl `pathtrace.h:71`, def `pathtrace.c:441`.
- `int pathtrace_render(const Scene*, const Camera*, int w, int h, int spp, int max_depth, unsigned char *rgb_out)` — decl `pathtrace.h:83-84`, def `pathtrace.c:946`.

There is no `pt_render_pixel`; the per-pixel kernel is a file-local static helper
(section "Per-pixel / per-region kernel", `pathtrace.c:728+`). The path-integral
loop lives entirely in `pathtrace_radiance` (`:551-726`); `pathtrace_render` only
schedules tiles and calls it.

## 2. Function / line-range map

| Function | Lines | Role |
|---|---|---|
| `pt_apply_medium` | `209-222` | Beer-Lambert segment attenuation + deep-colour add |
| `pt_sample_legacy` | `325-437` | Legacy (non-PBR) lobe sampling incl. dielectric branch |
| `pathtrace_to_byte` | `441` | Gamma/quantization |
| `pt_nee_sun` | `481-499` | Sun-disk Next-Event-Estimation |
| `pt_nee_emissive` | `519-545` | Emissive-sphere NEE |
| `pathtrace_radiance` | `551-726` | Main iterative bounce loop |
| `pathtrace_render` | `946` | Tile-scheduled render entry |

Tunables `:57-66`; channel constants `:114-123`.

## 3. Water detection

Detected via `Material.is_water` (`src/material.h:59`). Read in two places:

- Normal perturbation, `:609-611`: `if (m->is_water) N = water_normal(P.x, P.z, 0.0);`
  — the geometric hit normal is replaced by `water_normal(P.x, P.z, 0.0)` with
  time hard-wired to `0.0`. Then a safety re-flip (`:612-613`) and renormalize (`:615`).
- Medium bookkeeping, `:688-691`:
  ```c
  689  if (refracted && (m->is_water || m->beer_lambert)) {
  690      medium = h.front_face ? m : NULL;
  691  }
  ```
  `medium` (`const Material *`, declared `:570`) is set only when the lobe
  actually transmitted (`refracted == 1`) and the material is water or
  `beer_lambert`; front-face hit enters, else exits.

No `dielectric`/`type == water` material-kind test exists in the kernel. `ior`,
`transparency`, `reflectivity` are consumed by `pt_sample_legacy` (§5), not by an
explicit `is_water` branch. `is_water` gates only the normal and medium tracking.

## 4. Beer-Lambert / absorption attenuation

`pt_apply_medium` (`:209-222`) folds one medium segment of length `dist` into
`(beta, radiance)`: it calls `water_attenuate(m, inner, dist)` twice (white then
black inner colour), subtracts to recover transmittance `T`, then
`radiance += beta * deep`, `beta *= T` (`:215-221`). Called at exactly two sites
in `pathtrace_radiance`:

1. **Sky-miss with an open medium**, `:582-586`: if `medium != NULL`, applies
   `pt_apply_medium(medium, PT_MEDIUM_FALLBACK_DEPTH, ...)` then clears `medium`.
   `PT_MEDIUM_FALLBACK_DEPTH = 5.0` (`:62`) is the representative distance when a
   ray escapes a medium without a further hit.
2. **Before shading each hit**, `:593-596`: `if (medium != NULL)
   pt_apply_medium(medium, h.t, &throughput, &radiance);` where `h.t` is the
   travelled segment length.

Absorption maths is in `water_attenuate` (`src/material.c:604-633`): per-channel
transmittance `exp(-absorption_c * depth)` blended toward `deep_color`:
`out_c = inner_c * T_c + deep_color_c * (1 - T_c)` (`src/material.c:614-625`).
The path tracer never calls `exp()` itself — it reuses `water_attenuate` twice
(white/black inner) and subtracts.

**Kernel constants:** none for absorption/tint; colours come from material fields
`absorption`/`deep_color` (`src/material.h:60-61`). Only literals are the two
inner colours `vec3(1,1,1)`/`vec3(0,0,0)` (`:215-216`) and
`PT_MEDIUM_FALLBACK_DEPTH 5.0` (`:62`). Preset (`src/scene_desc.c`,
`sd_water_preset` `:527-543`): `ior=1.33`, `absorption=(0.45,0.12,0.06)`,
`deep_color=(0.02,0.10,0.16)`, `reflectivity=1.0`, `transparency=0.85`.

## 5. Specular / reflection lobe on water

Non-PBR water is shaded by `pt_sample_legacy` (`:325-437`). Lobe probabilities
from `reflectivity`/`transparency` (`:338-340`): `r_mir = reflectivity`,
`r_die = transparency`, `r_dif = 1 - r_mir - r_die`.
- **Mirror lobe** (`:355-365`): when `u_lobe < p_mir`; sets `*out_specular = 1`
  (`:361-362`, "delta mirror: NEE cannot sample it"), `wi = reflect(d, N)`, scalar
  weight `r_mir / p_mir`.
- **Dielectric (glass/water) lobe** (`:367-412`): when `u_lobe < p_mir + p_die`;
  sets `*out_specular = 1` (`:371-373`). Fresnel:
  ```c
  374  double ior = (m->ior > 0.0) ? m->ior : 1.0;
  375  double eta = front_face ? (1.0 / ior) : ior; /* eta_i / eta_t */
  377  double F = sampling_fresnel_dielectric(pt_clamp01(vec3_dot(N, V)), eta);
  ```
  Split uses `p_r = pt_clamp(F, PT_GLASS_P_MIN, PT_GLASS_P_MAX)` (`:387`;
  `0.05` / `0.95`, `:65-66`). Reflect weight `lobe * (F / p_r)` (`:390`); refract
  weight `lobe * ((1-F)/(1-p_r)) * eta*eta` (`:405`, radiance form). TIR at
  `F >= 1.0` = 100 % reflect (`:380-385`); a failed `sampling_refract` falls back
  to reflect (`:398-404`). `*out_refract = 1` only on real transmission (`:408-410`).

The reflection lobe is a stochastic delta choice with a scalar (grey) weight; no
per-channel Fresnel tint on the water specular in this path.

## 6. NEE highlight on the water surface

Both NEE passes run at every non-terminated hit (`:645-656`), regardless of
material: `pt_nee_emissive` (`:646`) at every bounce, `pt_nee_sun` (`:654`) only
when `b == 0`.
- `pt_nee_sun` (`:481-499`): samples the sun disk (`sky_sun_disk_dir`), shadows
  (`:489-491`), evaluates material response via `material_shade_local` (legacy) /
  `material_shade_pbr` (PBR) at `:496-497`. For legacy water,
  `material_shade_local` (`src/material.c:127`) returns
  `diffuse = albedo*light*N·L` plus Blinn-Phong
  `specular*light*(N·H)^shininess` — so a highlight can appear in the NEE term
  via `specular`/`shininess` (water preset `specular=(0.90,0.90,0.90)`,
  `shininess=256.0`).
- `pt_nee_emissive` (`:519-545`): loops emissive lights, shadows each, adds
  `material_shade_local`/`_pbr` × cap solid angle × `throughput` (`:540-542`).

De-dup: directly-hit emitted radiance is added at `:638-640` only when
`b == 0 || last_bounce_specular`. Water mirror/dielectric lobes set
`out_specular = 1`, so `last_bounce_specular = 1` after a specular water bounce
(`:680`) and emitted radiance is still added (NEE cannot sample a delta lobe).
`mm` is a stack copy `m_local = *m` with `albedo` overwritten by `texture_albedo`
(`:621-624`); physical fields (`is_water`, `ior`, `absorption`, `deep_color`,
`specular`, …) are preserved.

## 7. Sky-miss branch

When `scene_intersect` fails (`:581-591`): if `medium != NULL`,
`pt_apply_medium(medium, PT_MEDIUM_FALLBACK_DEPTH, ...)` is applied and `medium`
cleared; then `radiance += throughput * sky_sample(r.dir, &scene->sky)` (`:588-589`)
and the loop breaks. A parallel miss branch exists when `scene_material` returns
NULL (`:599-603`), adding the same `throughput * sky_sample(...)` term.

`sky_sample` (`src/material.c:350-417`) returns the horizon→zenith gradient plus a
**smooth sun glow** `pow(cos_sun, sun_glow_exponent) * sun_glow_strength`
(`src/material.c:361-367`) and a cloud layer (`:369-409`). It contains **no
discrete sun-disk term** — the glow is a continuous halo only. Per the module's
own note (`:473-479`), because the disk cannot be subtracted from a miss, sun NEE
is restricted to `b == 0` and the miss path is left untouched; the miss always
includes the smooth glow, weighted by `throughput`. So a ray that refracts into
water and later escapes to sky gets the sky term (with sun glow) attenuated by
`throughput` (carrying the water transmittance), and if the medium is still open
at escape, one extra `PT_MEDIUM_FALLBACK_DEPTH = 5.0` segment is applied
(`:582-586`).

## 8. Consolidated hard-coded constants (water path)

| Constant | Value | Location |
|---|---|---|
| `PT_MEDIUM_FALLBACK_DEPTH` | `5.0` | `pathtrace.c:62` |
| `PT_GLASS_P_MIN` / `PT_GLASS_P_MAX` | `0.05` / `0.95` | `pathtrace.c:65-66` |
| `PT_RAY_EPS` / `PT_RAY_MAX` | `1e-4` / `1e30` | `pathtrace.c:57-58` |
| water wave-normal time | `0.0` (hard-wired) | `pathtrace.c:610` |
| `pt_apply_medium` inner colours | `vec3(1,1,1)` / `vec3(0,0,0)` | `pathtrace.c:215-216` |
| `pt_nee_sun` shadow origin eps | `1e-3` | `pathtrace.c:490` |
| NEE sun-disk solid angle | `2π(1 - cos(sun_radius))` | `pathtrace.c:492-495` |

No Fresnel F0 / tint / blend literals are hard-coded in the water path: `F` comes
from `sampling_fresnel_dielectric(cos_i, eta)` (`:377`) with `eta` from `m->ior`
(`:374-375`); absorption/tint come from `absorption`/`deep_color` via
`water_attenuate`.
