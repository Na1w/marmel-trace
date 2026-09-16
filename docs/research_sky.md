# Research Note — Procedural Sky & Clouds for a C Raytracer (t-005)

**Scope.** Sky as a direction-sampled background function (`sky_color(dir)`), horizon
gradient / cheap atmospheric-scattering approximation, sun disc + glow, a cloud model built
from value/Perlin noise + fBm mapped onto a horizontal cloud layer, density→opacity/color
mapping with a `cloud_coverage` knob, and practical parameter ranges.

**Out of scope (deliberately).** Tree generation, water-surface shading, general intersection
math, BMP/file formats.

**Consumers (from `.marmel/execution_plan.md`).**
`src/material.c` (`sky_sample(dir)`), `src/render.c` (background fallback when a primary ray
misses everything), `src/scene.c` (sun direction). The water note (`docs/research_water.md`)
already calls `sky_sample(refl_dir)` for reflected sky — so the *same* function is reused as
the environment color for reflections/refractions. Assumed existing helpers:
`vec3` (`add/sub/mul/div`, `dot`, `cross`, `length`, `normalize`, `lerp`, `clamp`),
`noise` (`fbm2`, value/Perlin — see t-010).

---

## 0. TL;DR / recommended simplest approach

The whole sky is **one pure function** `Vec3 sky_color(Vec3 dir)` where `dir` is a normalized
ray direction and **no geometry is intersected**. It is called from exactly two places:

1. `render.c` — when a primary ray hits nothing (`hit.t == INFINITY`), return `sky_color(dir)`.
2. `material.c` — as the environment/reflection color for water and other reflective surfaces.

Recommended minimal recipe (≈40 lines of C, no dependencies beyond `fbm2`):

- **Gradient:** `t = clamp(0.5*(dir.y + 1.0), 0, 1)` → `sky = lerp(horizon, zenith, pow(t, 0.6))`.
- **Sun glow:** `g = pow(max(dot(dir, sunDir), 0), 350) * 0.8` → `sky += sunTint * g`.
- **Sun disc:** if `dot(dir, sunDir) > cos(0.5°)`, add a near-white core.
- **Clouds:** project the ray onto a plane at `cloudHeight` (`p = camera + dir * cloudHeight/dir.y`
  for `dir.y > 0`), sample `fbm2(p * cloudScale)` with 5 octaves, remap with a `smoothstep`
  around a coverage threshold, blend the resulting cloud color over `sky`.
- **Anti-noise:** always use `smoothstep`, never a hard `>` test — that is what keeps clouds
  from looking like uniform TV static.

Details, formulas and parameter tables follow.

---

## 1. Sky as a direction-sampled background function

### 1.1 Contract

```c
/* Pure function: maps a normalized ray direction to a linear-space RGB color.
 * No scene, no intersection, no I/O. Deterministic for a fixed seed/params. */
Vec3 sky_color(Vec3 dir);        /* dir must be normalized: |dir| == 1 */
```

Why a *pure function of direction* is the right abstraction:

- **Background for missed rays.** In `render.c`, when `scene_intersect()` reports no hit,
  the pixel color is simply `sky_color(ray.dir)`. There is no "sky sphere" primitive to
  intersect, no bounding box, no z-fighting with the far plane.
- **Environment color for reflection/refraction.** For a mirror-like water surface
  (see `research_water.md`), the reflected ray direction `refl = reflect(dir, N)` is fed
  straight into `sky_color(refl)`. Because the function is direction-only, it works equally
  well for rays that point *downward* (they simply show a ground-ish/darker gradient) — you
  may special-case `dir.y < 0` to fade to a ground/haze color if desired (§2.4).
- **No geometry, no seams.** A sphere/dome would introduce a UV seam and a silhouette edge;
  a direction function is continuous everywhere and trivially cheap.

### 1.2 Placement in the render loop

```c
/* render.c */
Vec3 trace(const Scene *s, Ray r, int depth) {
    Hit h;
    if (!scene_intersect(s, r, 0.001, INFINITY, &h)) {
        return sky_color(r.dir);           /* <-- background fallback */
    }
    ... /* shade; reflection/refraction calls also use sky_color(...) */
}

/* material.c */
Vec3 sky_sample(Vec3 dir) { return sky_color(dir); }   /* thin alias, or use directly */
```

> **Note on water:** `research_water.md` samples `sky_sample(refl_dir)` for reflections and
> falls back to it at depth exhaustion. Keep the function allocation-free and branch-light so
> it is cheap inside recursion.

---

## 2. Horizon gradient / atmospheric scattering approximation

Full Rayleigh/Mie scattering needs optical-depth integration through an atmosphere. For a
raymatracer we only need a **convincing analytic gradient** driven by the ray's vertical
component `dir.y ∈ [-1, 1]` (`y` = up).

### 2.1 Core blend

Let `h = clamp(dir.y, 0, 1)` be the "up-ness" of the ray (0 = horizon, 1 = zenith). Then:

```
t        = pow(h, gamma_grad)                 gamma_grad ≈ 0.5 .. 0.8   (horizon-biased)
sky      = lerp(horizon_color, zenith_color, t)
```

- `gamma_grad < 1` **stretches the bright horizon band** and compresses the zenith, which
  matches how real skies look (the color change is fastest just above the horizon).
- `gamma_grad = 1` gives a linear ramp (usable but flatter/less natural).
- `gamma_grad > 1` pushes brightness toward the zenith (good for a "high-altitude" look).

Suggested colors (linear-ish sRGB values, `0..1`; convert to your linear working space if you
gamma-correct at the end — see `research_shading_aa.md`):

| Sky type        | Horizon RGB        | Zenith RGB         | gamma_grad |
|-----------------|--------------------|--------------------|------------|
| Clear midday    | `(0.70, 0.82, 1.00)` | `(0.25, 0.45, 0.85)` | 0.6 |
| Hazy / warm     | `(0.85, 0.85, 0.80)` | `(0.35, 0.50, 0.80)` | 0.7 |
| Sunset          | `(1.00, 0.55, 0.25)` | `(0.15, 0.25, 0.55)` | 0.5 |

A very readable clear-sky pair used in many small renderers is horizon `(0.70, 0.80, 1.00)`
and zenith `(0.30, 0.50, 0.90)`.

### 2.2 Adding a sun-proximity term

Real skies brighten toward the sun (forward scattering). Blend the gradient toward a warm
"sun-side" color based on `dot(dir, sunDir)`:

```
k_sun  = max(dot(dir, sunDir), 0)             /* 0..1, 1 = looking at the sun */
k_sun  = pow(k_sun, 2.0)                       /* tighten the warm halo, ~2..4 */
sunset = 0.15 + 0.35 * max(sunDir.y, 0)        /* more warm tint when sun is low */
sky    = lerp(sky, sun_haze_color, k_sun * sunset)
```

with `sun_haze_color ≈ (1.00, 0.85, 0.60)` for a warm daytime halo, or
`(1.00, 0.45, 0.20)` for a sunset. This term is *optional* and cheap; it makes the sky read
as lit by a directional sun rather than a flat ramp.

### 2.3 Cheap "Rayleigh-ish" alternative (optional)

If you want slightly more physical falloff without integrating anything, use a two-term mix:

```
sky = zenith_color * (0.5 + 0.5 * h)          /* bluer overhead */
    + horizon_color * (1.0 - h) * haze_boost;  /* brighter band at horizon */
```

This is not energy-correct; it is simply a hand-tuned curve. The `pow` form in §2.1 is
usually enough and is preferred for clarity.

### 2.4 Below-horizon rays

For `dir.y < 0` (rays pointing at/below the horizon — e.g. water reflections near grazing),
clamp `h` to 0 and optionally fade toward a ground/haze color so reflections don't show an
abrupt band:

```c
if (dir.y < 0.0) {
    double b = clamp(-dir.y / 0.1, 0.0, 1.0);   /* blend over ~6 deg below horizon */
    sky = lerp(horizon_color, ground_haze, b);  /* ground_haze ≈ (0.35,0.35,0.30) */
}
```

---

## 3. Sun disc and glow

The sun is a **directional light** defined by a unit vector `sunDir` (from `scene.c`, e.g.
normalized `(0.4, 0.5, -0.6)`). Two additive terms model it in the sky.

### 3.1 Glow (angular falloff)

```
c    = max(dot(dir, sunDir), 0)                /* cosine of angle to sun */
glow = pow(c, exponent) * strength             /* tight halo */
sky += sun_tint * glow
```

| Parameter | Suggested range | Typical | Notes |
|-----------|-----------------|---------|-------|
| `exponent` | 100 .. 2000 | 350 | larger = tighter, more point-like halo |
| `strength` | 0.3 .. 1.5 | 0.8 | additive scale; clamp final color |
| `sun_tint` | warm white | `(1.0, 0.95, 0.85)` | multiply by `strength` |

A second, **broad** halo improves realism (a wide soft veil around the sun):

```
glow2 = pow(c, 8.0) * 0.15;
sky  += sun_tint * glow2;
```

Combining a tight term (`exp ≈ 350`) with a broad term (`exp ≈ 8`) gives a natural sun.

### 3.2 Sun disc

The visible sun disc has an angular radius `θ_sun ≈ 0.53°` (real value; use
`0.0046 .. 0.01 rad` for a slightly stylized, more visible disc). Test with a dot-product
threshold to avoid an `acos`:

```
cos_r = cos(theta_sun);                        /* ~0.99996 for the real sun */
if (dot(dir, sunDir) > cos_r) {
    sky = sun_core;                            /* near-white, e.g. (1.0, 1.0, 0.95) */
}
```

To avoid a hard aliased disc edge, soften it:

```
double d = dot(dir, sunDir);
double disc = smoothstep(cos_r, cos(0.0), d);  /* 0 outside, 1 inside the disc */
sky = lerp(sky, sun_core, disc);
```

Because a primary ray is a point sample, a small disc can alias badly. Either (a) keep
`θ_sun` comfortably large (≥ 0.5°), or (b) rely on the supersampled AA from
`research_shading_aa.md` to antialias the edge.

### 3.3 Order of operations

```
sky = gradient(dir);          /* §2 */
sky += glow terms;            /* §3.1  (additive) */
sky = clouds_over(sky, dir);  /* §4-5  (clouds occlude glow, so blend AFTER) */
sky = lerp(sky, sun_core, disc); /* §3.2  (disc drawn last, in front of clouds) */
```

Clouds should attenuate the glow (a cloud in front of the sun is bright but not a hard disc),
so compute clouds *after* adding glow. The physical disc itself is drawn last.

---

## 4. Cloud model: fBm on a horizontal cloud layer

### 4.1 Mapping the ray direction to a 2D cloud coordinate

The standard trick: pretend a flat cloud plane exists at height `cloudHeight` above the
camera. For rays with `dir.y > 0`, the intersection parameter is `t = cloudHeight / dir.y`,
giving a world-space point, which we scale into noise space:

```
if (dir.y > eps) {                 /* eps ~ 1e-3 to avoid /0 at the horizon */
    double t = cloudHeight / dir.y;
    double px = camPos.x + dir.x * t;
    double pz = camPos.z + dir.z * t;
    /* optional: project onto a dome instead (see below) */
    double n = fbm2(px * cloudScale, pz * cloudScale);
}
```

- `cloudHeight`: 300 .. 1500 world units (pick relative to scene size; e.g. `800`).
- `cloudScale`: 0.0005 .. 0.005 (`1/200 .. 1/2000`); controls the base cloud feature size.
  Larger scale = smaller, more numerous clouds.
- **Dome projection alternative** (avoids the extreme stretching near the horizon and the
  `dir.y→0` blow-up): use `(dir.x, dir.z) / (dir.y + k)` with `k ≈ 0.2 .. 0.5` as the
  coordinate, or normalize the plane point to a virtual dome of radius `R`:
  `dome = R * dir / max(dir.y, k)`. The plane projection is simpler and usually fine; the dome
  just keeps cloud cells more even when looking near the horizon.

> **Horizon guard:** near `dir.y ≈ 0` the plane coordinate → ∞, so clouds compress into a thin
> band. Either clamp `t` to a max distance (e.g. `t_max = 4*cloudHeight`) or fade the cloud
> opacity to 0 as `dir.y → 0`:
> `cloud_alpha *= smoothstep(0.0, 0.15, dir.y);`

### 4.2 fBm (fractal Brownian motion)

fBm is a weighted sum of noise octaves — successive higher frequencies with lower amplitudes.
Reference (Book of Shaders, ch. 13): start `frequency = 1`, `amplitude = 0.5`; each octave
`frequency *= lacunarity`, `amplitude *= gain`; `lacunarity = 2.0`, `gain = 0.5` are the
canonical values.

```
F(p) = Σ_{i=0}^{N-1} gain^i * noise(p * lacunarity^i)
```

with `lacunarity ≈ 2.0` and `gain ≈ 0.5` (Hurst exponent H ≈ 0.5 gives the classic "1/f"
self-similar cloud look; see Wikipedia "Fractional Brownian motion").

Practical points for clouds:

- **Octaves `N = 4 .. 6`.** Fewer than 4 → blobby and obviously noise-like; more than 6 →
  expensive with little visual gain at typical cloud size. **5 is the sweet spot.**
- **Normalize** the sum by `Σ gain^i` so the result stays in a predictable `[0,1]` (or
  `[-1,1]`) range before remapping. Without normalization, changing octave count changes the
  effective threshold and thus the coverage — normalize to keep `cloud_coverage` stable.
- **`fbm2` sign convention:** if your `noise()` returns `[-1,1]` (Perlin) rather than `[0,1]`
  (value noise), remap first: `v = 0.5 * (fbm + 1.0)` or use `fbm` directly but adjust the
  thresholds in §5 by the same offset. Pick one convention in `noise.c` and document it.

### 4.3 Domain warping (optional, big visual win)

Warping the sample position with a second noise field breaks up the "griddy"/isotropic look
and produces curly, wind-blown cloud edges:

```
q = ( fbm2(p + (0,0)), fbm2(p + (5.2, 1.3)) );   /* warp vector */
v = fbm2(p + warpStrength * q);
```

`warpStrength ≈ 0.5 .. 2.0` in noise-space units. Warping is cheap (two extra fBm evals) and
is the single most effective anti-"uniform noise" trick after `smoothstep` thresholding.

### 4.4 Height / thickness term (optional, cumulus look)

To make clouds look like they have vertical extent rather than a flat decal, modulate density
by a second dimension of the noise (or by the ray's elevation within the layer):

```
/* A pseudo-height: how "deep" the ray is into the layer, 0 at base, 1 at top */
double hz = smoothstep(0.0, 0.4, dir.y);          /* higher rays hit the "top" */
double dens = fbm3(px*cloudScale, hz*0.5, pz*cloudScale);  /* use 3D noise */
```

Or add a second fBm term that only gates the *edges* (an "erosion"/detail term), which is a
very cheap way to get wispy cumulus rather than uniform fog:

```
base   = fbm2(p * cloudScale);                    /* low frequency shape   */
detail = fbm2(p * cloudScale * 4.0);             /* high frequency edges  */
dens   = base - (1.0 - detail) * erode;           /* erode ≈ 0.1 .. 0.3    */
```

---

## 5. Cloud density → opacity / color mapping

### 5.1 Remap density to coverage with smoothstep

`cloud_coverage` is the primary artistic knob. Rather than a hard threshold (which produces
harsh, aliased, static-like edges), use two thresholds `a` (cloud base) and `b` (cloud top)
with `smoothstep`:

```
a = cloud_coverage - softness;      /* lower threshold */
b = cloud_coverage + softness;      /* upper threshold */
alpha = smoothstep(a, b, density);  /* 0 = clear sky, 1 = solid cloud */
```

- `softness` ≈ `0.05 .. 0.25`. Small → crisp cauliflower edges; large → soft, hazy clouds.
- `cloud_coverage` ≈ `0.4 .. 0.7`. Low → mostly clear; high → overcast.
  - `0.35` = sparse fair-weather puffs
  - `0.50` = balanced
  - `0.65` = broken / mostly cloudy

A single-sided variant (sharp below, soft above) is also common:

```
alpha = smoothstep(cloud_coverage, cloud_coverage + softness, density);
```

### 5.2 Density → color

Simple and effective: white at the top/outside, grey in the thick interior — fake self-shadowing
by darkening as density grows:

```
cloud_white = (0.95, 0.95, 0.98);
cloud_grey  = (0.55, 0.58, 0.65);
cloud_col   = lerp(cloud_white, cloud_grey, clamp(density, 0, 1) * shade);
```

with `shade ≈ 0.5 .. 0.8` controlling how dark the thick parts get. Add sun influence so lit
clouds glow and unlit sides darken:

```
lit  = 0.5 + 0.5 * max(dot(dir, sunDir), 0);   /* facing the sun = brighter */
cloud_col = cloud_col * (0.7 + 0.6 * lit);
```

### 5.3 Blending clouds over the sky gradient

Clouds are semi-transparent; composite with the sky from §2/§3:

```
Vec3 final = lerp(sky, cloud_col, alpha);      /* alpha from §5.1 */
```

`alpha` is the cloud coverage/opacity, so a thin cloud lets the sky show through and a thick
cloud fully covers it. This single `lerp` is the entire "cloud over sky" operation.

### 5.4 Two-layer cumulus (optional)

Stacking two cloud layers at different heights with different coverage/scale gives depth and
is cheap:

```c
Vec3 c1 = cloud_layer(dir, 0.6,  0.55, 0.0020, 5);   /* low, larger puffs  */
Vec3 c2 = cloud_layer(dir, 1.4,  0.45, 0.0040, 5);   /* high, smaller wisps */
/* composite far (high) first, then near (low) on top */
Vec3 sky2 = lerp(sky, c2, c2.a);
sky2     = lerp(sky2, c1, c1.a);
```

The lower layer is usually the dominant cumulus; the upper adds cirrus detail. Keep each
layer's `cloud_coverage` independent so you can tune the mix.

---

## 6. Practical parameter ranges (summary table)

| Parameter         | Range              | Typical | Effect |
|-------------------|--------------------|---------|--------|
| `gamma_grad`      | 0.5 .. 0.8         | 0.6     | horizon-to-zenith curve steepness |
| `sun glow exp`    | 100 .. 2000        | 350     | tightness of the sun halo |
| `sun glow str`    | 0.3 .. 1.5         | 0.8     | halo brightness |
| `sun theta`       | 0.0046 .. 0.01 rad | 0.0093  | disc angular radius (~0.53°) |
| `cloudHeight`     | 300 .. 1500        | 800     | layer altitude (world units) |
| `cloudScale`      | 0.0005 .. 0.005    | 0.002   | base cloud frequency (1/size) |
| `octaves`         | 4 .. 6             | 5       | detail levels |
| `lacunarity`      | 2.0                | 2.0     | frequency step (canonical) |
| `gain`            | 0.5                | 0.5     | amplitude step (canonical) |
| `cloud_coverage`  | 0.4 .. 0.7         | 0.55    | clear ↔ overcast |
| `softness`        | 0.05 .. 0.25       | 0.12    | edge softness (smoothstep width) |
| `warpStrength`    | 0.5 .. 2.0         | 1.0     | domain-warp curl amount |
| `erode`           | 0.1 .. 0.3         | 0.15    | edge wisps (cumulus detail) |
| `shade`           | 0.5 .. 0.8         | 0.6     | interior darkening of thick clouds |

### How to avoid "uniform noise"

1. **Always `smoothstep`, never a hard threshold.** A binary `density > coverage` test is what
   makes clouds look like flat static. `smoothstep` is the single most important fix.
2. **Low base frequency + enough octaves.** A base frequency that is too high yields
   salt-and-pepper; keep `cloudScale` small enough that a cloud spans tens of pixels.
3. **Domain warp** (§4.3) to break isotropy and add wind-swept structure.
4. **Erode with a second fBm** (§4.4) so edges feather instead of forming uniform blobs.
5. **Fade near the horizon** (§4.1) to avoid a compressed noise band.
6. **Tune `softness` before `coverage`.** If clouds look mushy, shrink `softness`; if they look
   harsh, grow it.

---

## 7. C-friendly pseudocode: `sky_color(dir)`

```c
/* Sky + cloud parameters (put in a struct or #defines; scene-tunable). */
typedef struct {
    Vec3   horizon, zenith;      /* gradient colors              */
    double gamma_grad;           /* 0.6                          */
    Vec3   sun_dir;              /* unit, from scene.c           */
    Vec3   sun_tint, sun_core;   /* warm halo / near-white disc  */
    double sun_glow_exp;         /* 350.0                        */
    double sun_glow_str;         /* 0.8                          */
    double sun_theta;            /* 0.0093 rad                   */
    Vec3   cam_pos;              /* for cloud-plane projection   */
    double cloud_height;         /* 800.0                        */
    double cloud_scale;          /* 0.002                        */
    int    cloud_octaves;        /* 5                            */
    double cloud_coverage;       /* 0.55                         */
    double cloud_softness;       /* 0.12                         */
    double cloud_warp;           /* 1.0                          */
    double cloud_erode;          /* 0.15                         */
    int    clouds_enabled;       /* 1                            */
} SkyParams;

static double smoothstep(double e0, double e1, double x) {
    if (e0 == e1) return (x < e0) ? 0.0 : 1.0;
    double t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/* --- clouds: returns linear RGB color + alpha ------------------------- */
static Vec3 cloud_layer(const SkyParams *P, Vec3 dir, double *out_alpha) {
    *out_alpha = 0.0;
    if (dir.y <= 1e-3) return vec3(0, 0, 0);          /* guard near horizon */

    /* 1. map direction onto the horizontal cloud plane */
    double t  = P->cloud_height / dir.y;
    double px = P->cam_pos.x + dir.x * t;
    double pz = P->cam_pos.z + dir.z * t;
    double x  = px * P->cloud_scale;
    double z  = pz * P->cloud_scale;

    /* 2. optional domain warp */
    if (P->cloud_warp > 0.0) {
        double qx = fbm2(x + 0.0, z + 0.0, P->cloud_octaves);
        double qz = fbm2(x + 5.2, z + 1.3, P->cloud_octaves);
        x += P->cloud_warp * qx;
        z += P->cloud_warp * qz;
    }

    /* 3. fBm density (normalize so coverage is octave-independent) */
    double base   = fbm2(x, z, P->cloud_octaves);
    double detail = fbm2(x * 4.0, z * 4.0, P->cloud_octaves);
    double dens   = base - (1.0 - detail) * P->cloud_erode;  /* erosion */

    /* 4. coverage remap with smoothstep */
    double a = P->cloud_coverage - P->cloud_softness;
    double b = P->cloud_coverage + P->cloud_softness;
    double alpha = smoothstep(a, b, dens);

    /* 5. fade out near the horizon (avoid compressed noise band) */
    alpha *= smoothstep(0.0, 0.15, dir.y);

    /* 6. density -> color (white outside, grey inside) + sun lighting */
    Vec3 white = vec3(0.95, 0.95, 0.98);
    Vec3 grey  = vec3(0.55, 0.58, 0.65);
    Vec3 col   = lerp(white, grey, clamp(dens, 0.0, 1.0) * 0.6);
    double lit = 0.5 + 0.5 * fmax(dot(dir, P->sun_dir), 0.0);
    col = mul(col, 0.7 + 0.6 * lit);

    *out_alpha = alpha;
    return col;
}

/* --- the single entry point ------------------------------------------- */
Vec3 sky_color(Vec3 dir) {
    dir = normalize(dir);
    SkyParams *P = &g_sky;                          /* global or passed in */

    /* --- 1. horizon gradient --- */
    double h = clamp(dir.y, 0.0, 1.0);
    double t = pow(h, P->gamma_grad);
    Vec3 sky = lerp(P->horizon, P->zenith, t);

    /* below-horizon fade (optional) */
    if (dir.y < 0.0) {
        double b = clamp(-dir.y / 0.1, 0.0, 1.0);
        sky = lerp(P->horizon, vec3(0.35, 0.35, 0.30), b);
    }

    /* --- 2. sun glow (tight + broad) --- */
    double c = fmax(dot(dir, P->sun_dir), 0.0);
    double glow = pow(c, P->sun_glow_exp) * P->sun_glow_str
                + pow(c, 8.0) * 0.15;
    sky = add(sky, mul(P->sun_tint, glow));

    /* --- 3. clouds (composited over the sky) --- */
    if (P->clouds_enabled) {
        double alpha;
        Vec3 cloud = cloud_layer(P, dir, &alpha);
        sky = lerp(sky, cloud, alpha);
    }

    /* --- 4. sun disc (drawn last, in front) --- */
    double cos_r = cos(P->sun_theta);
    double disc  = smoothstep(cos_r, 1.0, c);
    sky = lerp(sky, P->sun_core, disc);

    return sky;                                     /* clamp/gamma later */
}
```

### Integration notes

- **`src/material.c`:** expose `sky_sample(dir)` = `sky_color(dir)` (or rename). Water/reflective
  materials call it for the reflected environment (`research_water.md`).
- **`src/render.c`:** on miss, return `sky_color(ray.dir)`; do not recurse further for background.
- **`src/scene.c`:** provide the normalized `sun_dir` (and optionally the sun color) to the sky
  params; the same vector feeds the directional light in `shade()`.
- **Determinism:** all randomness lives in `noise.c` (seeded, t-010). `sky_color` itself is
  deterministic for fixed params, so renders are reproducible.
- **Performance:** one `sky_color` call ≈ a handful of `fbm2` evaluations (5 octaves × 2–3 fBm
  calls). This is cheap relative to recursive ray tracing; enable `clouds_enabled = 0` for a
  quick sky-only render.

---

## 8. References

- Book of Shaders, ch. 13 "Noise" — fBm octaves / lacunarity 2.0 / gain 0.5 / turbulence.
  https://thebookofshaders.com/13/
- Wikipedia, "Perlin noise" — gradient noise construction, `[-1,1]` output range, octaves.
  https://en.wikipedia.org/wiki/Perlin_noise
- Wikipedia, "Fractional Brownian motion" — Hurst exponent H; H = 0.5 ⇒ classic Brownian
  (the "1/f" cloud spectrum), higher H = smoother. https://en.wikipedia.org/wiki/Fractional_Brownian_motion
- Cross-reference: `docs/research_water.md` (calls `sky_sample(refl_dir)`),
  `docs/research_shading_aa.md` (gamma correction, AA that antialiases the sun disc),
  `docs/research_trees.md` (leaf density uses the same fBm noise library).

> Note: the horizon-gradient `pow(h, gamma_grad)` form, the sun `pow(dot, exponent)` glow, and
> the plane-projection cloud mapping are standard hand-tuned techniques (widely used in shader
> tutorials and small raytracers); they are approximations chosen for clarity and cost, not
> physically calibrated scattering.
