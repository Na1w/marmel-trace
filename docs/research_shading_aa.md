# Research Note — Shading, Recursive Ray Tracing & Post-Processing (t-002)

**Scope.** Local shading models, shadow rays, recursive reflection/refraction,
Fresnel (Schlick), Beer–Lambert attenuation, gamma/tone mapping, and supersampled
anti-aliasing. Concrete formulas + C-friendly pseudocode only.

**Out of scope.** Intersection math, procedural content, file formats.

**Downstream consumers.** `src/material.h/.c` (`shade`, `fresnel`, `reflect`, `refract`),
`src/render.h/.c` (recursion, AA, gamma).

---

## 0. Conventions used throughout

- All vectors are `Vec3` (`{ double x,y,z; }`), lowercase for directions/colors.
- `I` = **incident** ray direction. We adopt the common convention that `I` points
  **from the origin toward the surface** (i.e. the direction the ray travels). Watch
  sign conventions carefully — most bugs come from a flipped `I` or `N`.
- `N` = surface normal, **unit length**, and in this note we **flip it to face the
  incoming ray** so that `dot(N, I) < 0` (or use `N` flipped toward the viewer `V`).
- `V` = unit vector from surface point **to the eye** = `-I` (normalized).
- `L` = unit vector from surface point **to the light**.
- `H` = unit half-vector between `V` and `L`.
- Colors are **linear** floats in `[0,∞)` during accumulation; we only clamp and
  gamma-encode at the very end.
- `dot(a,b)` = `a.x*b.x + a.y*b.y + a.z*b.z`; `normalize(v)` = `v / |v|`.

> **Key sign rule (memorize):** with the "`I` points toward surface" convention,
> `dot(N, I) < 0` on a visible front face. So the Lambert term is `max(0, dot(N, -I))`.
> If instead your code keeps `I` pointing *away* from the surface, swap the signs.
> Pick one convention in `vec3.h` and never mix it.

---

## 1. Local shading — Phong vs Blinn–Phong

### 1.1 Phong reflection model (classic)

The classic Phong model computes an ideal reflection of `L` about `N` and measures
the angle between that reflected vector and the view vector:

```
R = 2 * dot(N, L) * N - L          // reflect L about N
spec = pow( max(0, dot(R, V)), shininess )
```

### 1.2 Blinn–Phong (half-vector) — preferred

Blinn's modification replaces `R·V` with `N·H`, where `H` is the normalized
half-vector between the light and the viewer:

```
H = normalize( L + V )
spec = pow( max(0, dot(N, H)), shininess )
```

**Why Blinn–Phong:** `H` does not depend on the reflection geometry, so it is cheaper
(one normalize + one dot vs. a reflect + dot), and it avoids the `R·V` branch when the
view and light are far apart. Blinn also produces a slightly broader highlight that many
find more realistic. The two agree at the mirror angle `R == V`. For a fixed `shininess`
the Blinn highlight is a bit larger; a common empirical remap is
`shininess_blinn ≈ 4 * shininess_phong` if you need to match look.

### 1.3 Full local model

```
color = ka * ambient
      + kd * max(0, dot(N, L)) * light_color * diffuse_albedo
      + ks * pow(max(0, dot(N, H)), shininess) * light_color * specular_albedo
```

where:
- `ka` — ambient coefficient (fills unlit areas; typically 0.05–0.2).
- `kd` — diffuse coefficient / albedo.
- `ks` — specular coefficient (metal-ish surfaces near 1.0; dielectrics ~0.04–0.1).
- `shininess` — specular exponent; small (~2–10) = broad/soft, large (~100–1000) = tight/sharp.

Note that `H` is only meaningful when `dot(N, L) > 0` (light above surface). Guard it:

```c
static inline double specular(Vec3 N, Vec3 L, Vec3 V, double shininess) {
    double ndl = dot(N, L);
    if (ndl <= 0.0) return 0.0;          // light behind surface -> no highlight
    Vec3 H = normalize(add(L, V));
    double ndh = dot(N, H);
    if (ndh <= 0.0) return 0.0;
    return pow(ndh, shininess);
}
```

### 1.4 Ambient term

Ambient is a crude stand-in for global illumination. Two common forms:

- **Flat ambient:** `color += ka * ambient_color;`
- **Hemisphere ambient (nicer):** blend sky/ground colors by `0.5*(1 + dot(N, up))`:
  `amb = mix(ground_color, sky_color, 0.5*(1.0 + dot(N, up)));`
  then `color += ka * amb;`

The hemisphere form gives outdoor scenes a natural "sky above / ground below" tint and
costs almost nothing. Prefer it when a sky model exists (see `src/material.c` sky sampling).

### 1.5 C-friendly pseudocode: `shade`

```c
Vec3 shade(const Scene *sc, const Hit *h, const Vec3 I) {
    Vec3 N  = h->normal;                     // unit
    Vec3 V  = normalize(neg(I));             // toward eye
    if (dot(N, I) > 0.0) N = neg(N);         // ensure N faces the viewer

    Vec3 albedo = h->mat.diffuse;
    Vec3 out = mul3(sc->ambient, h->mat.ka * 0.5 * (1.0 + dot(N, sc->up)));

    for (int li = 0; li < sc->n_lights; ++li) {
        Vec3  L   = normalize(sub(sc->lights[li].pos, h->point));
        double ndl = dot(N, L);
        if (ndl <= 0.0) continue;            // light behind surface

        if (in_shadow(sc, h->point, N, L, sc->lights[li].dist))
            continue;                        // see section 2

        Vec3 lc = sc->lights[li].color;
        // diffuse
        out = add(out, mul3(mul3(albedo, h->mat.kd), scale(lc, ndl)));
        // Blinn-Phong specular
        Vec3   H   = normalize(add(L, V));
        double ndh = fmax(0.0, dot(N, H));
        double s   = pow(ndh, h->mat.shininess);
        out = add(out, scale(mul3(lc, h->mat.specular), h->mat.ks * s));
    }
    return out;   // linear HDR-ish color, not yet gamma-corrected
}
```

---

## 2. Shadow rays (and shadow acne)

To test whether a surface point is lit, shoot a **shadow ray** from the point toward the
light and see if anything blocks it before the light.

```
origin    = P
direction = normalize(L - P)
shadowed  = ( any_hit(origin, direction, t_min, t_max) )
t_max     = |L - P|   (or the light's finite radius/distance)
```

### 2.1 The self-intersection problem ("shadow acne")

If the shadow ray origin is exactly on the surface, floating-point error can make the ray
report a hit **on the very surface it started from** (t ≈ 0). This produces speckled
self-shadowing ("acne"). Fix: **offset the origin along the normal by a small epsilon**.

```c
Vec3 origin = add(h->point, scale(N, EPS));   // push off the surface
```

Practical epsilon values:
- Scene scale matters. If objects span ~1–100 units, `EPS ≈ 1e-4 … 1e-3` works.
- A robust choice is a **relative** epsilon: `EPS = 1e-4 * fmax(1.0, fabs(P.x)+fabs(P.y)+fabs(P.z))`.
- Offset along `N` (not along the ray direction) for shadow rays — the normal is the most
  reliable "outward" direction.
- Alternatively use a `t_min` in the intersection routine (`t > EPS`) and leave the origin
  untouched. Combining both (offset origin **and** a tiny `t_min`) is the safest.

```c
int in_shadow(const Scene *sc, Vec3 P, Vec3 N, Vec3 L, double light_dist) {
    Vec3 origin = add(P, scale(N, SHADOW_EPS));
    Ray  r = { origin, L };
    double t;
    return any_hit(sc, r, SHADOW_EPS, light_dist - SHADOW_EPS, &t);
}
```

### 2.2 Notes

- Return **boolean**, not color — early-out on first blocker (cheaper, and avoids
  double-counting occlusion).
- For area/extended lights, you can return a soft visibility fraction by sampling
  several points on the light; average them for penumbra (soft shadows).
- For transparent occluders you'd attenuate rather than fully block (out of scope here,
  but `T += occluder.transmission` is the hook).

---

## 3. Recursive reflection

### 3.1 Reflection direction

The reflection of incident vector `I` about unit normal `N` (with `N` facing the viewer,
i.e. `dot(N,I) < 0`):

```
reflect(I, N) = I - 2 * dot(I, N) * N
```

This is a pure mirror reflection. It is independent of the "I toward surface" convention:
reflecting `I` gives the outgoing mirrored direction (the ray continues away from the
surface along `reflect(I,N)`).

```c
static inline Vec3 reflect_dir(Vec3 I, Vec3 N) {
    return sub(I, scale(N, 2.0 * dot(I, N)));
}
```

### 3.2 Recursive shading structure (Whitted-style)

The standard Whitted ray tracer computes local color, then recursively adds reflection
(and refraction) contributions, with a **depth limit** to guarantee termination.

```
trace(P, I, depth):
    hit = intersect(scene, P, I)
    if !hit: return background_color(I)

    local = shade(hit, I)                       // section 1
    if depth >= MAX_DEPTH: return local

    // reflection
    Rdir = reflect_dir(I, N)
    Rorg = P + N * EPS
    refl = trace(Rorg, Rdir, depth + 1)
    color = local + reflectivity * refl

    // refraction (section 4)
    if material is transmissive:
        Tdir, tir = refract(I, N, eta)
        Torg = P + Tdir * EPS
        refr = trace(Torg, Tdir, depth + 1)
        color = mix(color, refr, transmittance) // or add, per model

    return color
```

### 3.3 Combining local + reflected with a coefficient

Two common blends:

- **Additive with reflectivity `k_r`** (energy is not strictly conserved unless
  `k_r + k_d ≤ 1`):
  `color = local + k_r * reflected;`
- **Weighted blend** (conserves energy if `k_r` scales down the local diffuse):
  `color = (1 - k_r) * local + k_r * reflected;`

For metals, use the additive/multiplicative form with a colored `k_r` (tint the
reflection by the metal's reflectance). For dielectrics (glass, water), the *physically
correct* weight on reflection is the Fresnel term `F` (section 5), and refraction gets
`1 - F`. So the natural formulation is:

```
color = local
      + F * reflected
      + (1 - F) * refracted
```

where for a dielectric, `local` should be the diffuse term scaled by `(1 - k_r)` and
`F` from Schlick.

### 3.4 Depth limit and ray termination

- `MAX_DEPTH` typically 4–10. Each level can spawn up to `2^n` rays in the worst case;
  keep it small.
- Also apply an **energy cutoff**: if the accumulated contribution weight drops below a
  threshold (e.g. `< 1e-3`), stop — this prunes dim, expensive branches.
- Guard against pathological scenes (mirror-in-mirror) — the depth limit is your hard
  stop; never recurse without it.

```c
#define MAX_DEPTH 6
#define MIN_WEIGHT 1e-3

Vec3 trace(const Scene *sc, Vec3 P, Vec3 I, int depth, double weight) {
    Hit h;
    if (!intersect(sc, P, I, &h)) return background(sc, I);
    Vec3 local = shade(sc, &h, I);

    if (depth >= MAX_DEPTH || weight < MIN_WEIGHT)
        return local;

    Vec3 N = h.normal; if (dot(N, I) > 0) N = neg(N);
    Vec3 color = local;

    // --- reflection ---
    if (h.mat.reflectivity > 0.0) {
        Vec3 Rdir = reflect_dir(I, N);
        Vec3 Rorg = add(h.point, scale(N, EPS));
        color = add(color,
                    scale(trace(sc, Rorg, Rdir, depth+1,
                                weight * h.mat.reflectivity),
                          h.mat.reflectivity));
    }
    // --- refraction handled in section 4 ---
    return color;
}
```

---

## 4. Refraction — Snell's law in vector form

### 4.1 Snell's law

Scalar form: `n1 * sin(θ1) = n2 * sin(θ2)`, where `θ` is measured from the normal.
Define the **relative IOR** `eta = n1 / n2` (incident medium over transmitted medium).

### 4.2 Vector form of the refracted direction

Let `I` be the unit incident direction (pointing toward the surface), `N` the unit normal
**facing the incident side** (i.e. `dot(N, I) < 0`). Then:

```
cosi = -dot(N, I)                       // cosine of incidence angle, >= 0
eta  = n1 / n2
k    = 1 - eta^2 * (1 - cosi^2)
if k < 0:  total internal reflection  -> no refracted ray
T    = eta * I + (eta * cosi - sqrt(k)) * N
```

Equivalently written as `T = eta*I + (eta*cosi - cosθt)*N` with
`cosθt = sqrt(1 - eta^2 * (1 - cosi^2))`. `T` is already unit length when `I` and `N`
are unit.

### 4.3 Total internal reflection (TIR)

When `k < 0` (i.e. `eta^2 * (1 - cosi^2) > 1`, equivalently the transmission angle would
have `sinθ2 > 1`), there is **no** transmitted ray; the light is fully reflected. Handle
this by falling back to the reflection direction and putting all energy into `F = 1`.
TIR happens only when entering from a denser medium (`eta > 1`), e.g. glass→air.

### 4.4 Which normal / eta?

- Entering the surface (from outside): `n1 = 1.0` (air), `n2 = material.ior`. Use `N` as-is.
- Exiting the surface (from inside): `n1 = material.ior`, `n2 = 1.0`, and **flip** `N`
  (so it faces the incident side). Detect by `dot(I, N) > 0` before flipping.

```c
// Returns 1 on success, 0 on total internal reflection.
static inline int refract_dir(Vec3 I, Vec3 N, double n1, double n2, Vec3 *T) {
    double eta  = n1 / n2;
    double cosi = -dot(N, I);
    double k    = 1.0 - eta * eta * (1.0 - cosi * cosi);
    if (k < 0.0) {                       // TIR
        *T = reflect_dir(I, N);
        return 0;
    }
    *T = add(scale(I, eta), scale(N, eta * cosi - sqrt(k)));
    return 1;                            // *T is unit length
}
```

### 4.5 Combining reflection + refraction (glass)

```c
// inside trace(), for a dielectric material:
double n1 = 1.0, n2 = h.mat.ior;
Vec3 Nf = N;
if (dot(I, N) > 0.0) {         // ray exiting the object
    Nf = neg(N); n1 = h.mat.ior; n2 = 1.0;
}
Vec3 Tdir;
int ok = refract_dir(I, Nf, n1, n2, &Tdir);
double F = schlick_fresnel(cos_theta(I, Nf), h.mat.F0);   // section 5

Vec3 Rdir = reflect_dir(I, Nf);
Vec3 Rorg = add(h.point, scale(Nf, EPS));
Vec3 Torg = add(h.point, scale(Tdir, EPS));               // offset along T for the refract side

Vec3 refl = trace(sc, Rorg, Rdir, depth+1, weight * F);
Vec3 refr = ok ? trace(sc, Torg, Tdir, depth+1, weight * (1.0 - F)) : refl;

color = add(color, add(scale(refl, F), scale(refr, 1.0 - F)));
```

---

## 5. Schlick's approximation of Fresnel

### 5.1 The term

The Fresnel reflectance `F` gives the fraction of light reflected at a dielectric
interface, as a function of the angle between the view direction and the normal.

```
cosθ = clamp( dot(N, V), 0, 1 )       // V = toward eye; N faces viewer
F    = F0 + (1 - F0) * (1 - cosθ)^5
```

`F0` is the reflectance at normal incidence (θ = 0):

- **Dielectrics** (glass, water, plastic), with IOR `n`:
  ```
  F0 = ((n - 1) / (n + 1))^2
  ```
  Examples: glass `n=1.5 → F0 ≈ 0.04`; water `n=1.33 → F0 ≈ 0.02`; diamond `n=2.42 → F0 ≈ 0.17`.
- **Metals:** `F0` is a **per-channel color** (the metal's specular tint), typically
  0.5–1.0, e.g. gold `≈ (1.0, 0.71, 0.29)`, copper `≈ (0.95, 0.64, 0.54)`,
  silver `≈ (0.97, 0.96, 0.92)`, iron `≈ (0.56, 0.57, 0.58)`.

For metals there is no `(1-F0)` dielectric tail behavior in the same way, but Schlick is
still commonly used with the colored `F0`; the `(1 - cosθ)^5` term handles grazing
brightening.

### 5.2 Using it

- **Weighting reflection vs refraction** for dielectrics: reflection gets `F`, refraction
  gets `1 - F` (energy conserving).
- **Specular highlight strength:** `ks * F` (or just `F` for physical metals).
- **Ambient occlusion / grazing darkening:** some models multiply diffuse by `(1 - F)`.

```c
static inline double schlick_fresnel(double cos_theta, double F0) {
    double m = 1.0 - fmax(0.0, fmin(1.0, cos_theta));
    double m5 = m*m*m*m*m;
    return F0 + (1.0 - F0) * m5;
}

// color version for metals (F0 is Vec3):
static inline Vec3 schlick_fresnel3(double cos_theta, Vec3 F0) {
    double m  = 1.0 - fmax(0.0, fmin(1.0, cos_theta));
    double m5 = m*m*m*m*m;
    return add(F0, scale(sub(vec3(1,1,1), F0), m5));
}
```

---

## 6. Beer–Lambert attenuation for transmissive media

### 6.1 The law

Light passing through a participating/absorbing medium is attenuated exponentially with
distance:

```
transmittance = exp( -absorption * distance )
```

- `absorption` — per-channel coefficient `(a_r, a_g, a_b)`, units of 1/length.
  Larger `a` in a channel → that channel is absorbed faster → the medium tints the
  surviving light toward the *complementary* color.
- `distance` — length traveled inside the medium.
- For a colored medium, apply **per channel**: `T_c = exp(-a_c * d)`.

### 6.2 Applying it

When a refracted ray travels through an object (e.g. water, tinted glass), attenuate the
transmitted color by the distance traveled inside the medium:

```
refr_color = refracted * exp(-absorption * distance_inside)
```

Two practical ways to get `distance_inside`:
1. **Track entry/exit**: record `t_in` when entering, `t_out` when the transmitted ray
   exits the same object; `distance = t_out - t_in`.
2. **Approximate** with a fixed thickness or an analytic slab (e.g. water plane depth)
   when the geometry is simple — cheap and often good enough for a water surface.

```c
static inline Vec3 beer_lambert(Vec3 color, Vec3 absorption, double distance) {
    return vec3(color.x * exp(-absorption.x * distance),
                color.y * exp(-absorption.y * distance),
                color.z * exp(-absorption.z * distance));
}

// usage: after computing the transmitted color
Vec3 transmitted = trace(sc, Torg, Tdir, depth+1, weight * (1.0 - F));
transmitted = beer_lambert(transmitted, h.mat.absorption, dist_inside);
```

**Tuning tip:** start with a neutral, low absorption and small distances; exponential
falloff saturates fast. For water, absorption per unit length ~ `(0.35, 0.05, 0.02)` for
a blue-green tint over ~1–5 units of depth.

---

## 7. Gamma correction & tone mapping

### 7.1 Why

The renderer accumulates light in **linear** space, but displays/PNG/BMP expect
**sRGB-encoded** values. Writing linear values directly produces an image that looks too
dark and has muddy midtones. We must apply the transfer function **once**, at the end.

### 7.2 Linear → sRGB transfer function (exact)

For a linear channel value `c` clamped to `[0,1]`:

```
if c <= 0.0031308:  c_srgb = 12.92 * c
else:               c_srgb = 1.055 * c^(1/2.4) - 0.055
```

Then scale to 8-bit: `byte = round(255 * c_srgb)`.

### 7.3 The cheap approximation

`c_srgb ≈ c^(1/2.2)` — a common, fast approximation of the true sRGB curve. Use it when
per-pixel cost matters or when exact color management is not required:

```c
static inline double to_srgb_fast(double c) {
    c = fmax(0.0, fmin(1.0, c));
    return pow(c, 1.0 / 2.2);
}
```

### 7.4 Tone mapping (HDR → LDR)

Before gamma, high dynamic range values must be brought into `[0,1]`. Options:

- **Clamp:** `c = min(c, 1.0)` — simplest, but hard-clips highlights.
- **Reinhard:** `c' = c / (1 + c)` — soft roll-off, never clips; good default.
- **Reinhard with white point:** `c' = c*(1 + c/W^2) / (1 + c)` — keeps bright lights white.
- **Exposure:** `c *= 2^exposure` before tone mapping, for artistic control.

Order of operations: **exposure → tone map → gamma → quantize to 8-bit.**

```c
static inline unsigned char encode_pixel(double linear) {
    double c = linear * EXPOSURE;          // optional exposure
    c = c / (1.0 + c);                     // Reinhard tone map (or clamp)
    c = fmax(0.0, fmin(1.0, c));
    c = pow(c, 1.0 / 2.2);                 // gamma (use exact sRGB for best quality)
    return (unsigned char)(c * 255.0 + 0.5);
}
```

### 7.5 Where to apply

Apply gamma **once, per final pixel**, in `render.c` after all AA samples are averaged
(or after averaging, apply to the averaged value). **Never** gamma-encode intermediate
shading/reflection colors — that would double-correct and darken the image.

---

## 8. Supersampled anti-aliasing (AA)

### 8.1 Why supersample

One ray per pixel gives hard, jagged edges ("aliasing"). Averaging multiple samples
inside each pixel reconstructs smoother edges and reduces shimmer.

### 8.2 N×N regular grid supersampling

Cast `N*N` rays per pixel, at evenly spaced sub-pixel positions, then average:

```
for sy in 0..N-1:
  for sx in 0..N-1:
     px = (x + (sx + 0.5) / N) / width
     py = (y + (sy + 0.5) / N) / height
     color += trace(ray_for(px, py))
color /= N*N
```

Cost scales as `N^2`. `N=2` (4 spp) and `N=3` (9 spp) are the usual sweet spots;
`N=4` (16 spp) gives diminishing returns.

### 8.3 Stratified jittered sampling (better)

Regular grids can alias against scene edges (moiré). **Stratified jitter** perturbs each
sample within its own cell by a random amount, turning structured aliasing into noise
that averages out more gracefully:

```
for sy in 0..N-1:
  for sx in 0..N-1:
     jx = rand01()
     jy = rand01()
     px = (x + (sx + jx) / N) / width
     py = (y + (sy + jy) / N) / height
     color += trace(ray_for(px, py))
color /= N*N
```

Notes:
- Seed the RNG **deterministically per pixel** (`seed = hash(x, y)`) so renders are
  reproducible and can be re-run/tiled.
- One jittered sample per stratum = "stratified sampling"; combining multiple jittered
  passes gives further variance reduction.
- For soft shadows / glossy reflection, reuse the same stratified samples.

### 8.4 Cost/quality trade-off

| Samples/pixel | Grid | Quality | Relative cost |
|---------------|------|---------|---------------|
| 1             | 1×1  | aliased | 1×            |
| 4             | 2×2  | good    | 4×            |
| 9             | 3×3  | very good | 9×          |
| 16            | 4×4  | excellent, diminishing returns | 16× |
| 8 (jittered)  | 2×2×2 passes | ≈ 9 spp quality, less structured | 8× |

Adaptive sampling (cast more samples only where neighbors differ) can cut cost
substantially but adds complexity; a fixed jittered grid is a good default.

```c
Vec3 render_pixel(const Scene *sc, int x, int y, int N) {
    Vec3 acc = vec3(0,0,0);
    unsigned seed = hash2u(x, y);
    for (int sy = 0; sy < N; ++sy)
        for (int sx = 0; sx < N; ++sx) {
            double jx = rand01(&seed), jy = rand01(&seed);
            double px = (x + (sx + jx) / N) / (double)sc->width;
            double py = (y + (sy + jy) / N) / (double)sc->height;
            Ray r = camera_ray(sc->cam, px, py);
            acc = add(acc, trace(sc, r.origin, r.dir, 0, 1.0));
        }
    return scale(acc, 1.0 / (N * N));
}
```

---

## 9. Common mistakes

1. **Normal not facing the viewer.** Forgetting `if (dot(N,I) > 0) N = -N;` flips diffuse
   and specular terms → black or wrongly lit surfaces. Always orient `N` before shading.
2. **Shadow acne / self-shadowing.** Not offsetting the shadow-ray origin along `N` (or
   not using a `t_min`). Use `origin = P + N*EPS` and/or `t > EPS`.
3. **Offset too large.** A big epsilon causes **peter-panning** (shadows detached from the
   object). Keep `EPS` small and scale-relative.
4. **Reflection sign error.** `reflect = I - 2*dot(I,N)*N` assumes unit `N`. If `N` isn't
   normalized the reflected direction is wrong (and non-unit) → artifacts.
5. **Missing depth limit.** Recursion without `MAX_DEPTH` → stack overflow / infinite loop
   on mirrors or TIR. Always cap and apply an energy cutoff.
6. **TIR not handled.** `sqrt(k)` with `k < 0` gives `NaN`. Check `k < 0` and fall back to
   reflection with `F = 1`.
7. **Wrong `eta` direction.** `eta = n1/n2` where `n1` is the *incident* medium. Forgetting
   to swap when exiting an object (`dot(I,N) > 0`) makes refraction bend the wrong way.
8. **Fresnel `cosθ` from the wrong vector.** Use `cosθ = dot(N, V)` (view) clamped to
   `[0,1]`; using `dot(N, I)` with a sign flip is fine only if consistent. A negative
   `cosθ` fed to `pow` → `NaN`.
9. **Gamma applied twice (or not at all).** Encode linear→sRGB exactly once at output.
   Applying it to intermediate colors darkens the whole render; skipping it washes it out.
10. **Clamping before tone mapping.** Clamping linear HDR values to `[0,1]` before a
    Reinhard/filmic curve destroys highlight detail. Tone map first, clamp last.
11. **Regular-grid AA on thin features.** Pure N×N grids still alias on near-horizontal/
    vertical edges; use stratified jitter.
12. **Non-deterministic AA.** Using a global RNG across pixels makes renders
    irreproducible and hurts multi-threaded tiling; seed per pixel.
13. **Beer–Lambert with negative/zero distance.** Ensure `distance >= 0`; a negative
    distance turns attenuation into amplification.
14. **Energy conservation ignored.** `k_r + k_d > 1` produces over-bright surfaces; keep
    diffuse and specular weights summing sensibly (use `(1-F)` scaling where physical).

---

## 10. Quick reference — formulas

```
Half-vector:      H = normalize(L + V)
Diffuse:          kd * max(0, N·L)
Blinn specular:   ks * max(0, N·H)^shininess
Phong specular:   ks * max(0, R·V)^shininess,  R = 2(N·L)N - L
Shadow offset:    origin = P + N*EPS
Reflect:          R = I - 2(I·N)N
Refract:          T = eta*I + (eta*cosi - sqrt(k))*N,
                  cosi = -N·I, k = 1 - eta^2(1 - cosi^2), eta = n1/n2
                  TIR when k < 0
Schlick:          F = F0 + (1-F0)(1 - cosθ)^5,
                  F0 = ((n-1)/(n+1))^2  (dielectric)
Beer–Lambert:     T = exp(-absorption * distance)   (per channel)
sRGB:             c<=0.0031308 ? 12.92c : 1.055c^(1/2.4)-0.055
Gamma (fast):     c^(1/2.2)
Reinhard:         c' = c/(1+c)
AA average:       color = (1/N^2) * Σ trace(sample)
```

---

## References

- Blinn, J. F. (1977). *Models of Light Reflection for Computer Synthesized Pictures.*
  SIGGRAPH '77. (Half-vector specular.)
- Phong, B. T. (1975). *Illumination for Computer Generated Pictures.* CACM 18(6).
- Whitted, T. (1980). *An Improved Illumination Model for Shaded Display.* CACM 23(6).
  (Recursive reflection/refraction.)
- Schlick, C. (1994). *An Inexpensive BRDF Model for Physically-based Rendering.*
  Computer Graphics Forum 13(3). (Fresnel approximation.)
- Cook, R. L. & Torrance, K. E. (1982). *A Reflectance Model for Computer Graphics.*
  ACM TOG 1(1). (Fresnel F0 values, energy conservation.)
- Pharr, M., Jakob, W., Humphreys, G. *Physically Based Rendering: From Theory to
  Implementation* (3rd/4th ed.), chapters on reflection models, Fresnel, and sampling.
- IEC 61966-2-1:1999 — sRGB color space (transfer function coefficients).
- Reinhard, E. et al. (2002). *Photographic Tone Reproduction for Digital Images.*
  SIGGRAPH '02. (Reinhard tone mapping.)
