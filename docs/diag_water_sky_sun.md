# Diagnostic: Water renders near-white — Hypothesis B (sky/sun double-count)

**Task:** t-122 (bounded diagnosis, hypothesis B only). **Scope:** diagnosis only — **NO source change.**
**CWD:** `/Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer`
**Build:** `make` (threaded default). **Scene:** `scenes/default.scene`.

## Hypothesis B (as stated)

> The pond/water surface renders near-WHITE in default pathtrace mode because the
> bright SKY reflection (and/or SUN light) is **DOUBLE-COUNTED or over-weighted**
> on the water surface — e.g. `sky_sample` already contains a smooth sun glow AND
> `pt_nee_sun`/`pt_nee_emissive` additionally add a highlight, so the reflection
> lobe saturates to white.

## Verdict (up front)

**Hypothesis B is REFUTED as stated** (no discrete sun-disk double-count between
the sky-miss term and `pt_nee_sun`; `pt_nee_emissive` contributes nothing in the
default scene). **However**, the *underlying over-weighting sub-mechanism is
CONFIRMED*: the water material's `reflectivity = 1.0` **plus**
`transparency = 0.85` sum to **1.85 > 1**, and `pt_sample_legacy` renormalises the
lobe *selection probabilities* but keeps the lobe *weights* at `r/p = 1.85` for
**both** the mirror and dielectric lobes. The reflected sky is therefore
over-weighted by ~1.85×, which is what drives the water region to near-white.

- Dominant cause of the white pond = **SKY REFLECTION** (sky-only luma 244.9 ≈
  default 250.2), not a sun-disk term.
- Precise defective lines: **`src/scene.c:184-190`** (water preset
  `reflectivity=1.0`, `transparency=0.85`) feeding **`src/pathtrace.c:325-437`**
  (`pt_sample_legacy` weights `w = r_mir/p_mir` and `lobe = r_die/p_die`, both
  `= 1.85`).

---

## 1. Code evidence

### 1a. Sky-miss branch adds `sky_sample` once per escape

`src/pathtrace.c:588-589` (primary escape):
```c
            /* Escaped to the environment: add the sky once, weighted by beta. */
            radiance = vec3_add(radiance,
                                vec3_mul(throughput, sky_sample(r.dir, &scene->sky)));
```
`src/pathtrace.c:601-603` (null-material escape — same term):
```c
        const Material *m = scene_material(scene, h.material_index);
        if (m == NULL) {
            radiance = vec3_add(radiance,
                                vec3_mul(throughput, sky_sample(r.dir, &scene->sky)));
            break;
        }
```
The sky term is added **exactly once** on escape. There is **no** discrete sun-disk
term here.

### 1b. `sky_sample` contains only a smooth sun GLOW (no disk)

`src/material.c:356-367`:
```c
    /* --- Base gradient: horizon -> zenith ----------------------------- */
    double h = clamp01(dir.y);
    double t = pow_nonneg(h, sky->gradient_gamma);
    Vec3 base = vec3_lerp(sky->horizon_color, sky->zenith_color, t);

    /* --- Sun glow (tight halo around the sun direction) --------------- */
    double cos_sun = vec3_dot(dir, sky->sun_dir);
    if (cos_sun > 0.0) {
        double glow = pow_nonneg(cos_sun, sky->sun_glow_exponent)
                      * sky->sun_glow_strength;
        base = vec3_add(base, vec3_scale(sky->sun_color, glow));
    }
```
So the sky term used for the water reflection contains a **continuous halo**
(`pow(cos_sun, 350) * 0.8`) plus clouds — **no** sun disk.

### 1c. `pt_nee_sun` is a separate direct-sun term, first bounce only

`src/pathtrace.c:481-499`:
```c
static Vec3 pt_nee_sun(const Scene *scene, const Material *mm, Vec3 P, Vec3 N, Vec3 V,
                       Vec3 throughput, unsigned seed_key, int bounce)
{
    Vec3 sun = scene->sky.sun_dir;
    ...
    Vec3 L = sky_sun_disk_dir(sun, scene->sky.sun_radius, r1, r2);
    if (!pt_is_finite(L) || vec3_dot(N, L) <= 0.0) return vec3(0.0, 0.0, 0.0);
    Hit sh;
    Ray sr; sr.origin = vec3_add(P, vec3_scale(N, 1e-3)); sr.dir = L;
    if (scene_intersect(scene, sr, 1e-3, 1e30, &sh)) return vec3(0.0, 0.0, 0.0);
    double rad = scene->sky.sun_radius * 3.14159265358979323846 / 180.0;
    double Omega = (scene->sky.sun_radius > 0.0)
                       ? 2.0 * 3.14159265358979323846 * (1.0 - cos(rad))
                       : 1.0;
    Vec3 lit = mm->pbr ? material_shade_pbr(mm, N, L, V, scene->sky.sun_color)
                       : material_shade_local(mm, N, L, V, scene->sky.sun_color);
    return vec3_mul(throughput, vec3_scale(lit, Omega));   /* 1/pdf = Omega */
}
```
Its own header comment (`src/pathtrace.c:472-479`) explicitly documents the
skip-env policy — the author already reasoned about the double-count:
```
 * NOTE (skip_env policy, see the miss path in pathtrace_radiance): `sky_sample`
 * carries only a smooth sun *glow* (pow(cos_sun, exponent) * strength) and no
 * discrete sun-disk term, and the glow fields are not part of the public
 * SkyParams surface relied on here. Subtracting "the sun disk" from a miss is
 * therefore not cleanly expressible, so NEE is applied on the FIRST bounce only
 * (b == 0) and the miss path is left untouched. This avoids double counting the
 * sun's direct contribution while keeping the gradient/cloud sky intact.
```

### 1d. NEE call sites — every hit, material-agnostic

`src/pathtrace.c:645-656`:
```c
        radiance = vec3_add(radiance,
                            pt_nee_emissive(scene, mm, P, N, V, throughput,
                                            h.prim_index, seed_key, b));

        /* Sun Next-Event-Estimation (t-104b1): first bounce only ... */
        if (b == 0) {
            radiance = vec3_add(radiance,
                                pt_nee_sun(scene, mm, P, N, V, throughput,
                                           seed_key, b));
        }
```
`pt_nee_emissive` runs at every bounce, but the default scene has **no emissive
spheres** (`scene->emissive_light_count == 0`), so it contributes exactly zero.
`pt_nee_sun` runs **only at `b == 0`** and only on the **camera-visible** first
hit; it is **never** applied on the reflected/refracted water path at `b >= 1`.

### 1e. Water specular lobe — the over-weighting

`src/pathtrace.c:336-372` (`pt_sample_legacy`):
```c
    double r_mir = pt_clamp01(m->reflectivity);   /* = 1.0  */
    double r_die = pt_clamp01(m->transparency);   /* = 0.85 */
    double r_dif = 1.0 - r_mir - r_die;           /* = -0.85 -> 0 */
    if (r_dif < 0.0) r_dif = 0.0;

    double p_mir = r_mir;                         /* = 1.0  */
    double p_die = r_die;                         /* = 0.85 */
    double p_dif = 1.0 - p_mir - p_die;           /* = -0.85 < 0 */
    if (p_dif < 0.0) {
        double s = p_mir + p_die;                 /* s = 1.85 */
        if (s > 0.0) {
            p_mir /= s;                           /* p_mir = 0.5405 */
            p_die /= s;                           /* p_die = 0.4595 */
        }
        p_dif = 0.0;
    }

    /* ---- mirror lobe ---- */
    if (u_lobe < p_mir) {
        if (!(p_mir > 0.0)) return 0;
        double w = r_mir / p_mir;                 /* w = 1.0 / 0.5405 = 1.85 */
        *wi_out = vec3_reflect(d, N);
        *w_out = vec3(w, w, w);
        ...
    }

    /* ---- dielectric (glass / water) lobe ---- */
    if (u_lobe < p_mir + p_die) {
        ...
        double lobe = r_die / p_die;              /* lobe = 0.85 / 0.4595 = 1.85 */
        ...
```
Only the **probabilities** are renormalised to sum to 1; the **weights** are
`r/p`, and because `r_mir + r_die = 1.85` every lobe weight evaluates to
`1.85` — i.e. the reflected sky is multiplied by **1.85×**. This is exactly the
"over-weighted reflection lobe" the hypothesis suspected, but the source is the
material parameters (`reflectivity + transparency > 1`), not a sky/sun
double-count.

`src/scene.c:184-190` (water preset supplying those parameters):
```c
    m[MAT_WATER].albedo       = vec3(0.05, 0.15, 0.20);
    m[MAT_WATER].specular     = vec3(0.90, 0.90, 0.90);
    m[MAT_WATER].shininess    = 256.0;
    m[MAT_WATER].reflectivity = 1.0;
    m[MAT_WATER].transparency = 0.85;
    m[MAT_WATER].ior          = 1.33;
```
(`type = water` in the scene resolves to the identical preset,
`src/scene_desc.c:527-543` `sd_water_preset()`.)

---

## 2. Empirical isolation

Variants were built by copying `scenes/default.scene` and editing **only** the
sky/water fields (`docs/scene_format.md` §4.2/§4.3). Rendered 320×180, 16 spp,
depth 6, seed 1337, `--no-adaptive --no-progress`. Distinct output paths; the
unmodified default is `output/diagB_default.bmp` and `output/scene.bmp` was **not**
touched.

| Variant file | Edit |
|---|---|
| `output/diagB_default.bmp` | none (reference) |
| `output/diagB_sunonly.bmp` | sky gradient off (`horizon_color=0 0 0`, `zenith_color=0 0 0`), clouds opaque (`cloud_coverage=1`, `cloud_softness=0`); sun + glow kept |
| `output/diagB_skyonly.bmp` | `sun_color = 0 0 0`, `sun_glow_strength = 0` |
| `output/diagB_glowoff.bmp` | `sun_glow_strength = 0` (sun_color kept) |
| `output/diagB_w_mirror.bmp` | water `reflectivity=1.0, transparency=0.0` (sum = 1.0) |
| `output/diagB_w_sum1.bmp` | water `reflectivity=0.6, transparency=0.4` (sum = 1.0) |

**Water region** = rows 108..180 (bottom 40% of the 320×180 image; the pond),
23 040 pixels. `near-white` = pixels with all channels ≥ 250.

### 2a. Sky/sun isolation (rows 108..180)

| Variant | mean RGB | luma | near-white | any-255 |
|---|---|---|---|---|
| **default** | (241.1, 252.7, 252.3) | **250.2** | 51.41 % | 93.39 % |
| **sky-only** (sun off) | (233.3, 247.9, 248.7) | **244.9** | 31.96 % | 80.80 % |
| **sun-only** (sky off) | (57.8, 92.6, 100.0) | **85.8** | 0.00 % | 0.00 % |
| glowoff (glow only off) | (241.0, 252.7, 252.3) | 250.2 | 51.34 % | 93.37 % |

Lower strip (rows 144..180) reproduces the pattern: default luma 250.4,
sun-only 86.2, sky-only 244.4.

**Reading:** turning the sun completely off (sky-only) leaves the water at luma
**244.9**, i.e. essentially the full default brightness (250.2, a 2 % drop).
Turning the sky off (sun-only) collapses the water to luma **85.8** and **0 %**
near-white. Removing only the `sky_sample` sun *glow* (`glowoff`) changes luma by
**0.0** (250.2 → 250.2) and near-white by 0.07 pp. So:
- The near-white water is **sky-reflection dominated**.
- The smooth sun **glow inside `sky_sample` contributes negligibly** to the pond.
- The discrete `pt_nee_sun` term is **not** the driver (sun-only is the darkest
  variant; and `pt_nee_sun` only fires on the camera-visible first hit, never on
  the reflected water path).
- The default-vs-sky-only gap (250.2 vs 244.9) is the small direct sun/NEE +
  cloud contribution, **not** a double-count of the sky.

### 2b. Over-weighting isolation (rows 108..180)

| Water parameters | mean RGB | luma | near-white | any-255 |
|---|---|---|---|---|
| default `r=1.0, t=0.85` (sum **1.85**) | (241.1, 252.7, 252.3) | **250.2** | 51.41 % | 93.39 % |
| `r=1.0, t=0.0` (sum 1.0, pure mirror) | (215.2, 235.4, 246.9) | **231.9** | 12.55 % | 85.06 % |
| `r=0.6, t=0.4` (sum 1.0, split) | (190.8, 213.7, 228.1) | **209.9** | 1.28 % | 7.98 % |

Forcing the weights to sum to 1.0 (a pure, unbiased mirror) drops the water from
luma 250.2 → 231.9 and near-white 51.4 % → 12.6 %. Splitting the same unit budget
between mirror and dielectric lobes drops it further to luma 209.9 / 1.3 %.
This is the signature of the `r/p = 1.85` over-weighting in
`pt_sample_legacy` (the reflected sky is multiplied by 1.85).

---

## 3. Conclusion

| Claim in hypothesis B | Result |
|---|---|
| `sky_sample` contains a sun glow | **TRUE** (`material.c:361-366`) |
| `pt_nee_sun` adds a specular sun highlight | **TRUE**, but **bounce-0 only** (`pathtrace.c:652-655`), never on the reflected water path |
| `sky_sample` + `pt_nee_sun` **double-count a discrete sun disk** | **FALSE** — `sky_sample` has only a smooth halo (no disk); the author's own skip-env note documents this; `glowoff` changes water luma by 0.0 |
| `pt_nee_emissive` adds a highlight | **FALSE** in the default scene (no emissive lights → contributes 0) |
| The reflection lobe is **over-weighted** | **TRUE** — `reflectivity(1.0)+transparency(0.85)=1.85`, both lobe weights `r/p = 1.85` (`pathtrace.c:355-412`) |

**VERDICT: Hypothesis B is REFUTED as literally stated** (no sky/sun *double
count*; the near-white water is a **single** sky-reflection term), **but its
"over-weighted" branch is CONFIRMED**. The precise mechanism producing the
near-white pond:

1. **Primary (confirmed defect):** `src/scene.c:184-190` sets water
   `reflectivity = 1.0` and `transparency = 0.85` (sum 1.85 > 1); `pt_sample_legacy`
   (`src/pathtrace.c:325-437`) renormalises lobe *probabilities* to 0.5405/0.4595
   but leaves lobe *weights* at `r/p = 1.85` for **both** the mirror and dielectric
   lobes, over-weighting the reflected sky ~1.85× → saturation to white.
2. **Contributing scene factor:** the water reflects a bright sky
   (`horizon_color = 0.75 0.85 1`, `zenith_color = 0.35 0.55 0.95`) at a grazing
   angle, so the already over-weighted reflection saturates.

No source change was made (diagnosis-only task).
