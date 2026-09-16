# Diagnostic A (t-121): Does the path tracer apply the WRONG/absent Beer-Lambert absorption for `type = water`?

**Hypothesis A (as stated):** the pond/water surface renders near-WHITE in default
pathtrace mode because the path tracer applies the WRONG (or no) Beer-Lambert
absorption / blue tint for `type = water`, whereas Whitted applies it correctly.

**VERDICT: REFUTED.** The path tracer *does* apply Beer-Lambert absorption, and it
is numerically correct (the deep-water refracted path is strongly blue-tinted).
The near-white is real, but its root cause is a **different defect**: a spurious
`x1.85` energy gain in the *legacy lobe-selection weights* (`pt_sample_legacy`),
which amplifies the mirror-lobe reflection of the bright sky/cloud layer to
near-white. The precise defective lines are identified in §5.

No source was modified for this diagnosis.

---

## 1. Code evidence — absorption maths is shared and correct

### 1a. The shared attenuation helper (used by BOTH renderers)

`src/material.c:589` `water_transmittance`, and `src/material.c:604-633`
`water_attenuate`:

```c
604  Vec3 water_attenuate(const Material *m, Vec3 inner, double depth)
...
613  /* Clamp depth at 0 (also coerces NaN depth to 0). */
614  double d = depth > 0.0 ? depth : 0.0;
615
616  /* Beer-Lambert transmittance per channel: exp(-absorption_c * depth). */
617  Vec3 tr = vec3(water_transmittance(m->absorption.x, d),
618                 water_transmittance(m->absorption.y, d),
619                 water_transmittance(m->absorption.z, d));
...
624  Vec3 out = vec3(inner.x * tr.x + m->deep_color.x * (1.0 - tr.x),
625                  inner.y * tr.y + m->deep_color.y * (1.0 - tr.y),
626                  inner.z * tr.z + m->deep_color.z * (1.0 - tr.z));
```

This is affine in `inner`: `out = T(d)*inner + (1-T(d))*deep_color`.

### 1b. Whitted call sites

- `src/render.c:781` — on an exit hit inside water: `refr_col = water_attenuate(m, inner, h2.t);`
  (`h2.t` = propagation distance inside the medium).
- `src/render.c:790` — ray escapes without a further hit: `refr_col = water_attenuate(m, inner, 5.0);`
- Combine, `src/render.c:796-798`:
  ```c
  796  Vec3 surface = vec3_add(vec3_scale(refl_col, F),
  797                          vec3_scale(refr_col, 1.0 - F));
  798  Vec3 out = vec3_lerp(color, surface, m->transparency);
  ```
  Reflection is weighted by the **Fresnel** term `F` (<= 1), refraction by `1-F`;
  the whole surface is then scaled by `transparency`. The result is bounded and
  cannot exceed the scene radiance.

### 1c. Path-tracer call sites

`src/pathtrace.c:209-222` `pt_apply_medium`:

```c
209  static void pt_apply_medium(const Material *m, double dist, Vec3 *beta, Vec3 *radiance)
...
215      Vec3 white = water_attenuate(m, vec3(1.0, 1.0, 1.0), dist); /* T + (1-T)*deep */
216      Vec3 deep  = water_attenuate(m, vec3(0.0, 0.0, 0.0), dist); /*     (1-T)*deep */
217
218      Vec3 transmittance = vec3_sub(white, deep); /* = T, component-wise in [0,1] */
219
220      *radiance = vec3_add(*radiance, vec3_mul(*beta, deep));
221      *beta = vec3_mul(*beta, transmittance);
```

The white/black argument pair is a **correct algebra trick** to recover `T` and the
additive `(1-T)*deep_color` term from the affine helper; it is NOT a colour bug.
Call sites: `src/pathtrace.c:583` (sky-miss fallback `PT_MEDIUM_FALLBACK_DEPTH = 5.0`,
defined `:62`) and `src/pathtrace.c:595` (`pt_apply_medium(medium, h.t, ...)`).
Medium entry/exit gating, `src/pathtrace.c:689-691`:
`if (refracted && (m->is_water || m->beer_lambert)) medium = h.front_face ? m : NULL;`.

**Conclusion of §1:** the path tracer reuses the *exact same* absorption helper as
Whitted, applies it on every refracted segment, and applies the same 5.0 fallback.
There is no "no absorption" or "wrong absorption" in the medium code.

---

## 2. Empirical evidence — absorption demonstrably works in pathtrace

Linear radiance from a standalone probe (`output/diagA_probe.c`, real
`pathtrace_radiance` kernel; compile:
`cc -std=c11 -O2 -Isrc output/diagA_probe.c $(ls src/*.o | grep -v main.o) -lm -o output/diagA_probe`).

Deep-water scene (`output/scene_deepwater.scene`, 10 m water box, grey floor at bottom):

| probe ray | reflectivity | linear radiance | blue/red |
|---|---|---|---|
| straight-down into water (t≈5 m path) | 1.0 | (0.05701, 0.20526, 0.27090) | **4.75 (BLUE)** |
| camera v=0.35 | 1.0 | (2.19375, 2.24102, 2.28726) | 1.04 (near-white) |
| camera v=0.35 | 0.0 | (0.04766, 0.15806, 0.20558) | **4.31 (BLUE)** |

The straight-down ray traverses ~5 m of water and comes back **strongly blue** —
Beer-Lambert absorption with `absorption=(0.45,0.12,0.06)` is applied correctly by
the path tracer. Removing only the mirror lobe (`reflectivity = 0`) makes the water
correctly blue in pathtrace too. Absorption is therefore **not** the defect.

---

## 3. Empirical evidence — the near-white is the mirror lobe reflecting the sky/cloud

Isolation on the deep-water scene (linear probe, camera v=0.35, reflectivity = 1):

| variant | linear radiance | note |
|---|---|---|
| base (refl=1) | (2.19375, 2.24102, 2.28726) | near-white |
| black sky + black sun, **clouds on** | **(1.75750, 1.75750, 1.81300)** | = **1.85 x (0.95, 0.95, 0.98)** |
| black sky + black sun, **clouds off** | (0.0, 0.0, 0.0) | no radiance at all |

The constant `(1.7575, 1.7575, 1.8130)` equals exactly `1.85 * (0.95, 0.95, 0.98)`,
and `(0.95, 0.95, 0.98)` is the cloud albedo (`src/material.c:399`
`Vec3 cloud_color = vec3_scale(vec3(0.95, 0.95, 0.98), shadow);`). With the cloud
disabled the radiance is identically zero. **The near-white is the mirror lobe
reflecting the bright cloud/sky layer, amplified by a factor of exactly 1.85.**

---

## 4. Root cause — spurious `x1.85` lobe-weight energy gain

Default water material (`src/scene.c:185-186`, `src/scene_desc.c:527-543`):
`reflectivity = 1.0`, `transparency = 0.85` (sum = **1.85**).

`src/pathtrace.c:338-353` (`pt_sample_legacy`):

```c
338  double r_mir = pt_clamp01(m->reflectivity);
339  double r_die = pt_clamp01(m->transparency);
340  double r_dif = 1.0 - r_mir - r_die;      /* 1 - 1.0 - 0.85 = -0.85 -> clamped */
341  if (r_dif < 0.0) r_dif = 0.0;
342
343  double p_mir = r_mir;
344  double p_die = r_die;
345  double p_dif = 1.0 - p_mir - p_die;      /* 1 - 1.0 - 0.85 = -0.85 < 0 */
346  if (p_dif < 0.0) {
347      double s = p_mir + p_die;            /* s = 1.85 */
348      if (s > 0.0) {
349          p_mir /= s;                      /* p_mir = 1.0/1.85 */
350          p_die /= s;                      /* p_die = 0.85/1.85 */
351      }
352      p_dif = 0.0;
353  }
```

Only the **selection probabilities** `p_mir, p_die` are renormalised; the
**reflectance numerators** `r_mir, r_die` are NOT. The estimator weights are then:

- mirror lobe, `src/pathtrace.c:358`: `double w = r_mir / p_mir;`
  → `1.0 / (1.0/1.85) = 1.85`
- dielectric lobe, `src/pathtrace.c:378`: `double lobe = r_die / p_die;`
  → `0.85 / (0.85/1.85) = 1.85`

Both branch weights equal `s = reflectivity + transparency = 1.85`, so **every**
BSDF bounce at the water surface multiplies the throughput by `1.85` — a spurious
energy gain proportional to `s`. The mirror lobe (selection probability ≈ 0.54)
then reflects the bright sky/cloud with weight 1.85, producing the near-white.
Whitted has no such factor (it weights reflection by `F` and refraction by `1-F`),
so it stays dark/blue.

The unbiased estimator requires the weight numerator to be the *renormalised*
reflectance as well (`r_mir/s`, `r_die/s`), i.e. both would then equal 1.0; or the
material model must not permit `reflectivity + transparency > 1`.

---

## 5. Precise defective lines (for a future fix — NOT changed here)

- `src/pathtrace.c:344-353` — renormalisation divides only `p_mir, p_die` by `s`.
- `src/pathtrace.c:358` — mirror weight `w = r_mir / p_mir` uses the *un-renormalised* `r_mir`.
- `src/pathtrace.c:378` — dielectric weight `lobe = r_die / p_die` uses the *un-renormalised* `r_die`.

These three points together yield the `x(r_mir + r_die) = x1.85` energy gain.
By contrast `src/render.c:796-798` (Whitted) weights by Fresnel `F` / `1-F`, bounded.

---

## 6. Measured water-region RGB (rendered images)

Rendered at `--width 320 --height 180` (default scene, pathtrace spp default;
Whitted via `--no-pathtrace --no-adaptive`), analysed with a stdlib-only script
(`output/diagA_final.py`). Water band = rows 110..170 (lower half of frame).

| image | mode | mean (R,G,B) | B-R | near-white share |
|---|---|---|---|---|
| `output/diagA_pt.bmp` | pathtrace | (242.5, 253.6, 252.8) | +10.3 | **79.5 %** |
| `output/diagA_whitted.bmp` | Whitted | ( 53.7,  72.3,  71.9) | +18.2 | 0.0 % |
| `output/diagA_pt_deepwater.bmp` | pathtrace | (253.4, 254.6, 255.0) | +1.6 | **97.8 %** |
| `output/diagA_wh_deepwater.bmp` | Whitted | ( 62.0,  99.0, 119.0) | +56.9 | 0.0 % |

The pathtrace water is near-white; the Whitted water is dark and blue-dominant.
The pathtrace water's blue-red margin collapses to `+1.6` because the `x1.85`
mirror lobe (reflecting the neutral-bright cloud) swamps the blue-tinted refracted
term. It is not that absorption is missing — the refracted term is blue (see §2);
it is that the mirror term is over-weighted and dominant.

---

## 7. Verdict

**Hypothesis A is REFUTED.**

- The path tracer applies Beer-Lambert absorption correctly, reusing
  `water_attenuate` via `pt_apply_medium` (`src/pathtrace.c:209-222`) with the same
  `h.t` segment length (`:595`) and the same 5.0 fallback (`:583`) as Whitted.
  The straight-down deep-water probe returns a strongly blue radiance
  `(0.057, 0.205, 0.271)` — absorption is demonstrably active.
- The near-white is caused by a **different** defect: the legacy lobe-selection
  renormalisation in `pt_sample_legacy` (`src/pathtrace.c:344-353`, `:358`, `:378`)
  multiplies the throughput by `reflectivity + transparency = 1.85` at every water
  bounce, so the mirror lobe reflects the bright sky/cloud with a spurious `1.85x`
  gain. Disabling the mirror lobe (`reflectivity = 0`) restores a correctly
  blue-tinted pathtrace water.

The blue tint that Hypothesis A claims is "missing" is present in the refracted
term; it is merely dominated by the over-weighted mirror term.
