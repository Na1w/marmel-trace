# Research Note: Water Surface Shading for a C Raytracer

**Scope:** water-body modelling (plane + "is this water?" test), wave-normal perturbation
(sum-of-sines and noise/fBm), Fresnel-weighted reflection/refraction blend, reflected-sky
sampling, depth-based absorption/tint, and practical parameter ranges.

**Out of scope (deliberately):** tree generation, cloud/sky internals, general intersection
math, BMP/file formats.

**Consumers (from `.marmel/execution_plan.md`):**
`src/material.c` (water shading), `src/scene.c` (water plane), `src/render.c` (sky reflection).
Assumed existing helpers: `vec3` (`add/sub/mul/div`, `dot`, `cross`, `normalize`, `lerp`,
`reflect`, `refract`), `noise` (`fbm2`, value/Perlin), and `sky_sample(dir)` in `material.c`.

---

## 0. TL;DR / recommended simplest approach

- Represent water as a **large bounded horizontal plane** at `y = waterLevel` (finite quad,
  e.g. 2000×2000, centered on the scene). Do **not** displace geometry — only perturb the normal.
- Detect water at a hit by **flagging the primitive** (`is_water = 1`) or by
  `fabs(hit.p.y - waterLevel) < eps && inside_water_rect(p)`.
- Build the normal from a **sum of 3–4 directional sine waves** (analytic derivatives → exact
  normal). Add a small **fBm finite-difference** term only if you want irregularity.
- Shade with **Schlick Fresnel** (`F0 = 0.02`), reflect the view vector and sample `sky_sample`,
  refract toward the water interior and sample a **Beer–Lambert-tinted deep-water color**.
  Combine: `color = F*reflection + (1-F)*refraction`.
- To kill aliasing: **clamp high-frequency amplitudes**, **limit slope**, and **supersample**.

That is enough for a convincing water band in an outdoor render. Details follow.

---

## 1. Modelling the water body as a plane

### 1.1 Geometry choice

Water is a **flat horizontal plane** at a fixed height `y = waterLevel` (the ground plane
usually sits slightly *below* it, e.g. `groundY = waterLevel - 0.2`, so shore edges read as
banks). Two equivalent options:

| Option | Pros | Cons |
|---|---|---|
| **Infinite plane** (like the ground) | Trivial intersection, no edges | Must be distinguished from ground; reflection extends to horizon (fine) |
| **Bounded rectangle** (large quad) | Clean shore boundary; can sit inside a terrain dip | Needs an in-rect test + fallback to ground behind it |

**Recommendation:** bounded rectangle (quad) of ~2000×2000 units, or an infinite plane whose
material is `is_water`. The rectangle gives a natural shoreline and avoids the "whole world is
an ocean" look.

### 1.2 Detecting "is this hit point on water"

Three robust patterns, pick one:

1. **Per-primitive flag (simplest, preferred).**
   Store an `is_water` (or `MAT_WATER`) flag on the plane primitive. After
   `scene_intersect` returns the nearest hit, just read `hit.material == MAT_WATER`.
   No geometric test needed — works for both bounded and infinite planes.

2. **Geometric test** (useful when water shares one big plane with ground):
   ```c
   int is_water(const Vec3 *p, double waterLevel, double eps) {
       return fabs(p->y - waterLevel) < eps;   /* eps ~ 1e-3 .. 1e-2 */
   }
   ```
   If the plane is bounded, also require `p.x,p.z` inside the water rectangle.

3. **Depth below surface** (needed anyway for absorption, §4):
   ```c
   double depth = waterLevel - p->y;   /* > 0 when the point is under water */
   ```

### 1.3 Two-sided / entering-vs-exiting

You only need the **surface** (air→water) for a reflective pond. Handle two cases in
`scene_intersect`:
- Ray hits water from above (`ray.dir.y < 0`): normal points up `+y` (before perturbation) →
  this is the reflective surface path (§3).
- (Optional) ray hits water from below (camera or refracted ray underwater): normal points
  `-y`; you may simply terminate with the deep-water color to avoid complexity.

The plane's base normal is `n0 = (0, 1, 0)`; the perturbed normal `n` from §2 replaces it.

---

## 2. Wave-normal perturbation without displaced geometry

Idea: the surface is flat (intersection stays trivial), but the *normal* is tilted as if a
height field `h(x,z)` existed. We never move geometry; we only tilt the normal. This is the
cheapest and most common raytracer trick.

### 2.1 Sum-of-sines height field (analytic normal)

Directional sine waves. With `k` wave components:

```text
h(x, z) = Σ_i  A_i * sin( dot(D_i, (x, z)) * w_i + φ_i + t * s_i )
```

where
- `D_i = (dx_i, dz_i)` — unit direction of wave `i` in the horizontal plane,
- `w_i = 2π / λ_i` — angular wavenumber (`λ_i` = wavelength),
- `A_i` — amplitude,
- `φ_i` — phase offset,
- `s_i = w_i * c_i` — temporal angular speed (`c_i` = wave speed); set `t = 0` for a static image.

**Partial derivatives (exact):**

```text
Let  u_i = dot(D_i, (x,z)) * w_i + φ_i + t*s_i
dh/dx = Σ_i  A_i * w_i * dx_i * cos(u_i)
dh/dz = Σ_i  A_i * w_i * dz_i * cos(u_i)
```

**Normal from the height field.** For a height field `y = h(x,z)`, a surface tangent basis is
`Tx = (1, dh/dx, 0)` and `Tz = (0, dh/dz, 1)`; the upward normal is their cross product:

```text
n = normalize( cross(Tz, Tx) )        /* choose order so n.y > 0 */
  = normalize( (-dh/dx, 1, -dh/dz) )   /* analytic simplification */
```

So the explicit formula is:

```text
n = normalize( -dh/dx , 1 , -dh/dz )
```

For a *pure sum of sines* the derivative magnitudes are bounded by
`Σ_i A_i * w_i = Σ_i 2π A_i / λ_i`, so choosing `A_i/λ_i` small keeps slopes gentle.

### 2.2 Gerstner-like variant (optional)

Gerstner waves displace vertices laterally as well as vertically; for *normals only* you can
keep the same derivative structure but bias `D_i` per component and add a slight choppiness by
using `sin` for height and a sharper `cos`-weighted derivative. For a raytracer the plain sine
sum above is usually indistinguishable and far simpler — **prefer §2.1.**

### 2.3 Noise-based alternative (fBm + finite differences)

When you want irregular, non-repeating ripples (better for large bodies), sample an fBm noise
field and estimate the normal by central differences:

```text
h(x,z)   = amplitude * fbm2(x * freq, z * freq, octaves, lacunarity, gain)
dh/dx ≈ ( h(x+e, z) - h(x-e, z) ) / (2e)
dh/dz ≈ ( h(x, z+e) - h(x, z-e) ) / (2e)
n = normalize( -dh/dx , 1 , -dh/dz )
```

`e` is the finite-difference step (pick `e ≈ 0.5 / freq` so it samples roughly one feature
width; too small → numerical noise, too large → smeared normals). fBm continuity guarantees a
smooth normal. Cost: 4 fBm evaluations per shaded point (or use forward differences: 3 evals).

**Hybrid (recommended for realism):** one or two low-frequency *directional* sine waves for the
large swell (cheap, coherent) **plus** a small high-frequency fBm term for detail. Sum the
height derivatives from both before normalizing.

### 2.4 C-friendly pseudocode — `water_normal(x, z)`

```c
/* ---- Tunable wave set (see §5 for ranges) ---- */
typedef struct { double ax, az;   /* unit dir in xz          */
                 double w;        /* 2*PI/lambda            */
                 double amp;      /* amplitude              */
                 double speed;    /* temporal speed         */
                 double phase; }  Wave;

static const Wave WAVES[] = {
    /* dir (norm)          w=2pi/lam   amp      speed   phase */
    {  1.0,  0.30,  2.0*M_PI/24.0, 0.18,  1.0,  0.0 },
    {  0.7, -0.7,   2.0*M_PI/13.0, 0.10,  1.4,  1.7 },
    { -0.4,  0.9,   2.0*M_PI/7.0,  0.05,  2.0,  3.1 },
    {  0.2,  0.98,  2.0*M_PI/4.0,  0.025, 2.6,  0.6 },
};
#define NWAVES ((int)(sizeof(WAVES)/sizeof(WAVES[0])))

Vec3 water_normal(double x, double z, double t) {
    double dhdx = 0.0, dhdz = 0.0;

    /* --- directional sine swell (analytic derivatives) --- */
    for (int i = 0; i < NWAVES; ++i) {
        const Wave *w = &WAVES[i];
        double u  = (w->ax * x + w->az * z) * w->w + w->phase + t * w->speed * w->w;
        double c  = cos(u) * w->amp * w->w;
        dhdx += c * w->ax;
        dhdz += c * w->az;
    }

    /* --- optional fBm detail via finite differences --- */
    const double freq = 0.35, namp = 0.06, e = 0.5 / freq;
    double hL = fbm2((x - e) * freq, z * freq, 4, 2.0, 0.5);
    double hR = fbm2((x + e) * freq, z * freq, 4, 2.0, 0.5);
    double hD = fbm2(x * freq, (z - e) * freq, 4, 2.0, 0.5);
    double hU = fbm2(x * freq, (z + e) * freq, 4, 2.0, 0.5);
    dhdx += namp * (hR - hL) / (2.0 * e);
    dhdz += namp * (hU - hD) / (2.0 * e);

    /* --- analytic normal from height-field gradient --- */
    return normalize(vec3(-dhdx, 1.0, -dhdz));
}
```

> Static render → call `water_normal(x, z, 0.0)` or drop `t` entirely.
> For an animated sequence, pass `t = frame_index / fps`.

---

## 3. Fresnel-weighted reflection / refraction blend

### 3.1 Schlick's approximation with F0 ≈ 0.02 for water

Fresnel reflectance at normal incidence for water:

```text
F0 = ( (n1 - n2) / (n1 + n2) )^2 = ( (1.0 - 1.33) / (1.0 + 1.33) )^2 ≈ 0.020
```

Schlick (using `cosθ = dot(n, view_dir_toward_camera)`; take `|cosθ|` for safety):

```text
F(θ) = F0 + (1 - F0) * (1 - cosθ)^5
```

At grazing angles `cosθ → 0`, so `F → 1` (strong reflection — the classic bright water at the
horizon). At head-on viewing `F → F0 ≈ 0.02` (almost all light refracts, water looks clear).

### 3.2 Reflected and refracted rays

```text
reflected_dir = reflect( view_dir, n )          /* view_dir points from eye to surface */
refracted_dir = refract( view_dir, n, eta )     /* eta = n1/n2 = 1.0/1.33 ≈ 0.752 */
```

- `reflect(I, N) = I - 2*dot(I,N)*N`.
- `refract(I, N, eta)` uses Snell: `eta*I + (eta*cosθi - cosθt)*N` with
  `cosθt = sqrt(1 - eta^2 * (1 - cosθi^2))`. **If the discriminant is negative → total internal
  reflection (TIR)**, which for air→water never happens, but from water→air it can; then return
  `F = 1` (all reflected).

### 3.3 Combining reflection and refraction

```text
reflection_color = sky_sample(reflected_dir)          /* from render.c / material.c */
refraction_color = underwater_color(...)              /* §4, Beer–Lambert tinted */
color            = F * reflection_color + (1 - F) * refraction_color
```

For a simple pond you may use a single `underwater_color` constant, optionally modulated by the
local normal / fresnel to fake caustics.

### 3.4 C-friendly pseudocode — water shading

```c
Vec3 shade_water(const Ray *ray, const Hit *hit,
                 const Scene *scn, int depth, double t)
{
    /* 1. Perturbed normal */
    Vec3 n = water_normal(hit->p.x, hit->p.z, t);
    if (dot(ray->dir, n) > 0.0) n = neg(n);       /* face the incoming ray */

    Vec3 V = normalize(neg(ray->dir));            /* surface -> eye */
    double cos_theta = clamp(dot(n, V), 0.0, 1.0);

    /* 2. Schlick Fresnel, F0 = 0.02 */
    const double F0 = 0.02;
    double m  = 1.0 - cos_theta;
    double m2 = m * m;
    double F  = F0 + (1.0 - F0) * (m2 * m2 * m);  /* (1-cos)^5 */

    /* 3. Reflection ray -> sky (with recursion budget) */
    Vec3 refl_dir = reflect(ray->dir, n);
    Vec3 reflection = (depth > 0)
        ? sky_sample(refl_dir)                    /* sky_sample in material.c */
        : sky_sample(refl_dir);

    /* 4. Refraction ray -> tinted underwater color (Beer–Lambert, §4) */
    const double eta = 1.0 / 1.33;
    Vec3 refr_dir = refract(ray->dir, n, eta);
    double depth_m = 1.5;                         /* see-through distance to "bottom" */
    Vec3 refraction = underwater_color(depth_m);

    /* 5. Fresnel blend */
    return add(mul(reflection, F), mul(refraction, 1.0 - F));
}
```

Notes:
- For a **recursive** tracer, `reflection` can instead be `trace(ray{p + eps*n, refl_dir}, depth-1)`
  so trees/clouds also reflect; fall back to `sky_sample` when `depth == 0`.
- Offset secondary ray origins by a small `eps` along `n` to avoid self-intersection.

---

## 4. Depth-based absorption / water tint (Beer–Lambert)

### 4.1 Law

Beer–Lambert (a.k.a. Bouguer–Lambert): a beam of initial intensity `I0` after path length `d`
through an absorbing medium is

```text
I(d) = I0 * exp(-μ * d)
```

where `μ` is the (Napierian) attenuation coefficient. Per-channel:

```text
T_c(d) = exp(-absorption_c * d)          /* c ∈ {r, g, b} */
```

Water absorbs red first, then green, leaving blue/cyan — hence the green-blue tint.

### 4.2 Mapping depth to see-through distance

For a *flat* pond, define an effective depth. Two practical choices:
1. **Constant** see-through distance (e.g. `d = 1.5`–`3.0` units): simple, reads as a shallow
   pond. Use directly in §3.
2. **View-dependent depth**: `d = depth_scale / max(dot(n, V), 0.15)` so grazing views pass
   through more water → stronger absorption near the horizon. Clamp to avoid explosions.

Then combine with the deep-water color:

```text
underwater_color = lerp( shallow_color, deep_color, 1 - exp(-absorption * d) )
```

or, applied per channel to a bottom/albedo color:

```text
underwater_color_c = bottom_color_c * exp(-absorption_c * d)
                     + deep_color_c * (1 - exp(-absorption_c * d))
```

### 4.3 Suggested coefficients and colors

| Parameter | Suggested value | Notes |
|---|---|---|
| `absorption` (scalar, per unit length) | **0.25 – 1.5** | Higher = murkier; 0.4 is a calm pond |
| `absorption` (RGB, per channel) | **(0.45, 0.12, 0.06)** | Red absorbed fastest → cyan-blue water |
| `deep_color` (RGB, 0–1) | **(0.02, 0.10, 0.16)** | Dark teal/navy |
| `shallow_color` / bottom tint | **(0.10, 0.22, 0.20)** | Greenish shallows |
| `eta = n1/n2` | **1.0 / 1.33 ≈ 0.752** | IOR of water ≈ 1.33 |
| `F0` | **0.02** | Schlick normal-incidence reflectance |

### 4.4 C-friendly pseudocode — `underwater_color`

```c
static const Vec3 ABSORB     = {0.45, 0.12, 0.06};   /* per-unit-length, RGB */
static const Vec3 DEEP_COLOR = {0.02, 0.10, 0.16};
static const Vec3 SHALLOW    = {0.10, 0.22, 0.20};

Vec3 underwater_color(double d) {
    Vec3 out;
    out.x = SHALLOW.x * exp(-ABSORB.x * d) + DEEP_COLOR.x * (1.0 - exp(-ABSORB.x * d));
    out.y = SHALLOW.y * exp(-ABSORB.y * d) + DEEP_COLOR.y * (1.0 - exp(-ABSORB.y * d));
    out.z = SHALLOW.z * exp(-ABSORB.z * d) + DEEP_COLOR.z * (1.0 - exp(-ABSORB.z * d));
    return out;
}
```

---

## 5. Practical parameter ranges

### 5.1 Wave parameters

| Parameter | Range | Visual effect |
|---|---|---|
| Wave amplitude `A_i` | **0.01 – 0.30** units | Bigger = choppier, more normal tilt, more sparkle |
| Wavelength `λ_i` | **4 – 40** units (large swell) | Larger λ = broad, gentle rollers |
| Short ripples `λ_i` | **0.5 – 4** units | Small λ = fine chop; keep amplitude tiny |
| Number of components | **2 – 6** (3–4 sweet spot) | More = richer detail but higher aliasing + cost |
| Wave speed `c_i` | **0.5 – 3.0** units/s | Only matters for animation |
| fBm frequency `freq` | **0.2 – 0.6** | Higher = finer noise ripples |
| fBm amplitude | **0.02 – 0.10** | Keep small; large values distort silhouettes |
| fBm octaves | **3 – 5** | More = finer detail, more cost |
| fBm lacunarity / gain | **2.0 / 0.5** | Standard |
| Finite-diff step `e` | **≈ 0.5 / freq** | Balance noise vs. smearing |

**Rule of thumb:** keep the slope `Σ_i A_i * w_i = Σ_i 2π A_i / λ_i` below ~0.5 so the surface
stays plausible and does not fold over. Amplitude should scale roughly with wavelength
(`A_i ≈ 0.01 * λ_i`) — long waves tall, ripples shallow.

### 5.2 Fresnel / refraction

| Parameter | Range | Effect |
|---|---|---|
| `F0` | 0.02 (water) | Lower → more transparent head-on |
| `eta` | 0.75 (=1/1.33) | Governs refraction bend |
| `cosθ` clamp | `max(dot, 0.0)` | Prevents negative-power artifacts |

### 5.3 Avoiding aliasing from high-frequency waves

High-frequency ripples produce near-random normals per pixel → shimmering sparkle / noise.
Mitigations, cheapest first:

1. **Cap the highest wavenumber by distance / projected pixel size.** Estimate the ray's
   footprint on the surface; if `λ_i` is smaller than the footprint, drop that component:
   ```c
   if (w->w * footprint > 1.0) continue;   /* wavelength < ~pixel size -> skip */
   ```
2. **Clamp the total slope** before normalizing (`if (len(grad) > maxSlope) scale down`) so
   normals never flip past vertical.
3. **Amplitude falloff with frequency:** give the highest-`w` components the smallest `A_i`
   (e.g. `A_i ∝ 1/w_i` or `1/w_i^2`), so noise energy at high frequency is low.
4. **Supersample** (already planned in `render.c`: N×N jittered AA) — averaging sub-samples
   smooths the sparkle into a plausible glint.
5. **Band-limit fBm:** use fewer octaves for distant water; stop octaves once the octave
   frequency exceeds the pixel footprint.

A quick heuristic: choose `N` waves such that the smallest wavelength `λ_min` is at least
**2× the pixel footprint** at typical viewing distance, and put the fBm detail below that in
amplitude.

---

## 6. Integration notes for the raytracer

- **`src/scene.c`:** add the water plane with `material = MAT_WATER` (or `is_water = 1`); store
  `waterLevel`, the water rectangle bounds, and `t` if animating.
- **`src/material.c`:** implement `water_normal()` and `shade_water()` here; add
  `underwater_color()` as a small helper. `shade()` dispatches to `shade_water` when the hit
  material is water.
- **`src/render.c`:** the reflection ray calls `sky_sample()` (or recurses); ensure the sky
  fallback also triggers when a secondary ray leaves the scene. Apply gamma at the very end.
- **Depth/recursion:** treat water reflection as one recursion level; when `max_depth` is
  exhausted, use `sky_sample` only.
- **Determinism:** all wave/noise parameters are constants (or seeded), so renders are
  reproducible for a fixed seed.

---

## 7. References

- Schlick's approximation & Fresnel F0 for water (derivation `((1-1.33)/(1+1.33))² ≈ 0.020`):
  <https://en.wikipedia.org/wiki/Schlick%27s_approximation>
- Beer–Lambert law, `I = I0·exp(-μd)`:
  <https://en.wikipedia.org/wiki/Beer%E2%80%93Lambert_law>
- Gerstner wave / sum-of-sines height-field normals (background):
  <https://en.wikipedia.org/wiki/Gerstner_wave>
- Schlick 1994, "An Inexpensive BRDF Model for Physically-based Rendering", *Computer Graphics
  Forum* 13(3):233–246.
- IOR of water ≈ 1.33: <https://en.wikipedia.org/wiki/Refractive_index>

---

*Note length kept intentionally tight and pragmatic (~450 lines) per task brief; no encyclopedic
background included.*
