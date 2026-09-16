# Design note: emissive PBR primitives as area lights

**Scope.** Make primitives whose material has `pbr = 1` and a non-zero
`emissive` colour illuminate the rest of the scene, using the existing
deterministic Whitted renderer. Zero third-party deps, C11 + libm only.
This is a *design/implementation note*; no source file was modified.

**Problem (documented README limitation).** In `src/render.c` the `emissive`
term is *added to the shaded result of the emitter itself* (`trace_hit`,
`src/render.c:300-312`), so a lamp looks self-lit, but it contributes **no**
direct lighting to any other surface. There is no area-light emission.

---

## 0. Ground truth inspected (local citations)

| File | Lines | What was read |
|---|---|---|
| `src/render.c` | 87-105 | `SUN_SHADOW_SAMPLES` (16), `SUN_RADIUS_EPSILON` (1e-9), hash PRNG (`render_hash_u32/hash3/rand01`, 62-83) |
| `src/render.c` | 124-213 | `trace_hit` signature; direct-sun block: ambient, `L = scene->sky.sun_dir`, `shadow_o`, soft/hard shadow ray casts |
| `src/render.c` | 178-190 | soft-shadow loop: `sky_sun_disk_dir`, `scene_intersect` shadow ray |
| `src/render.c` | 202-210 | hard-shadow single ray |
| `src/render.c` | 300-312 | the `emissive` self-emission add (gated on `pbr` + non-zero) |
| `src/render.c` | 318-330 | `trace` wrapper (`scene_intersect` then `trace_hit`) |
| `src/render.c` | 423-426 | per-primary-ray `seed_key = render_hash3(x, y, s)` |
| `src/scene.h` | 33-41 | `Scene` struct (geo, bvh, materials, material_count, sky, …) |
| `src/scene.c` | 327-340 | `scene_free` |
| `src/scene.c` | 540-601 | `scene_build_from_desc`: material table copy (553-565), primitive append (570-575), `bvh_build` (599) |
| `src/scene.c` | 390-392 | `scene_material(index)` lookup |
| `src/geometry.h` | 28-72 | `Primitive` (kind, material_index, center, radius), `Hit` (t, point, normal, material_index, prim_index, front_face) |
| `src/material.h` | 45-90 | `Material` (`emissive`, `pbr`, `metallic`, `roughness`) |
| `src/material.h` | 100-135 | `SkyParams` (`sun_dir`, `sun_color`, `sun_radius`) |
| `src/material.c` | 362-375 | `sky_basis(n, &t, &b)` (static orthonormal basis helper) |
| `src/material.c` | 377-... | `sky_sun_disk_dir(sun_dir, radius_deg, r1, r2)` |
| `src/material.c` | 122-165 | `material_shade_local` (Blinn-Phong) |
| `src/material.c` | 197-253 | `material_shade_pbr` (Cook-Torrance) |
| `src/material.c` | 255-281 | `material_ambient` (hemisphere term) |

---

## 1. Where the direct-lighting / shadowing loop lives

`src/render.c` `trace_hit` (`src/render.c:124`), immediately after the
material/texture setup:

```c
/* src/render.c:171-210 (abridged) */
Vec3 color = material_ambient(mm, N, &scene->sky);   /* 171: hemisphere/ambient */
Vec3 L = scene->sky.sun_dir;                          /* 172: sun direction     */
Vec3 shadow_o = vec3_add(P, vec3_scale(N, 1e-3));     /* 173: shadow-ray origin */
double sun_radius = scene->sky.sun_radius;            /* 174                    */
if (sun_radius > SUN_RADIUS_EPSILON) {                /* 175: SOFT shadows      */
    double lit = 0.0;
    for (int i = 0; i < SUN_SHADOW_SAMPLES; ++i) {    /* 176                    */
        unsigned k = seed_key * 0x9e3779b9u + (unsigned)i * 2u;      /* 178    */
        double r1 = render_rand01(k, (unsigned)i, 0x5a17u);          /* 179    */
        double r2 = render_rand01(k, (unsigned)i, 0x7c3du);          /* 180    */
        Vec3 Ld = sky_sun_disk_dir(L, sun_radius, r1, r2);           /* 181    */
        Hit sh;
        if (!scene_intersect(scene, (Ray){shadow_o, Ld}, 1e-3, 1e30, &sh)) /* 183 */
            lit += 1.0;
    }
    if (lit > 0.0) {
        /* weight = lit / SUN_SHADOW_SAMPLES, shaded along central L */
        ... material_shade_pbr / material_shade_local ...            /* 187-195 */
    }
} else {                                                              /* 202    */
    Hit sh;
    if (!scene_intersect(scene, (Ray){shadow_o, L}, 1e-3, 1e30, &sh)) /* 204   */
        ... single hard-shadow direct term ...                       /* 205-209 */
}
```

Key facts:
- **Ambient/hemisphere term**: `material_ambient(mm, N, &scene->sky)`
  (`src/render.c:171` → `src/material.c:255-281`) — a normal-weighted
  lerp(groundish, skyish) × albedo × 0.15. This is the only non-sun light
  today.
- **Sun-disk soft-shadow sampling**: `sky_sun_disk_dir` (`src/material.c:377`)
  builds a cone of half-angle `sun_radius` degrees around `sun_dir` and samples
  uniformly on the disk (`r = sqrt(u1)·radius`, `theta = 2π·u2`, perturb along
  `tan(offset)` using the `sky_basis` frame).
- **Shadow-ray casts**: `scene_intersect(scene, (Ray){shadow_o, dir}, 1e-3,
  1e30, &sh)` at `src/render.c:183` (soft) and `:204` (hard). `scene_intersect`
  wraps `bvh_intersect` over `Scene.bvh` with a linear fallback
  (`src/scene.h:60-64`).
- **Determinism**: the jitter is a pure function of `seed_key` (from
  `render_hash3(x, y, s)`, `src/render.c:423-426`) and the sample index — never
  the thread/tile. This is the RNG to reuse.

### Scene/geometry representation a light list must traverse

```c
/* src/geometry.h:28-49 */
typedef struct { PrimKind kind; int material_index;
                 Vec3 center, axis, half, a, b, c; double radius, radius2; } Primitive;
typedef struct { Primitive *prims; int count, capacity; } Geometry;

/* src/geometry.h:57-64 */
typedef struct { double t; Vec3 point, normal;
                 int material_index, prim_index, front_face; } Hit;

/* src/scene.h:33-41 */
typedef struct { Geometry geo; Bvh *bvh; Material *materials; int material_count;
                 SkyParams sky; double water_level; int water_material; } Scene;
```

A light list traverses `Scene.geo.prims` (indices), reading each primitive's
`kind`, `center`, `radius` and its `material_index` into `Scene.materials[]`.

---

## 2. Collecting emissive primitives at scene-build time

`emissive`/`pbr` are fully available during build: the material table is copied
at `src/scene.c:553-565` and the primitives are appended at
`src/scene.c:570-575`, both **before** `bvh_build` at `src/scene.c:599`. So a
collector can run right after the primitive loop (order does not matter for the
BVH, which only stores indices).

### New struct + Scene fields (`src/scene.h`)

```c
/* v1 supports sphere emitters only (matches scenes/example_materials.scene). */
typedef struct {
    int    prim_index;   /* index into Scene.geo.prims (a PRIM_SPHERE) */
    Vec3   center;       /* sphere center                              */
    double radius;       /* sphere radius                              */
    Vec3   emissive;     /* emitted radiance Le, linear RGB (may > 1)  */
} EmissiveLight;

#define SCENE_MAX_EMISSIVE_LIGHTS 8
```

Add to `Scene`:

```c
EmissiveLight emissive_lights[SCENE_MAX_EMISSIVE_LIGHTS];
int           emissive_light_count;   /* 0 => feature disabled (default) */
```

A **fixed array** (no allocation, no free, no OOM path) keeps the change small
and makes the default scene's memory layout irrelevant. Zero it in
`scene_build_from_desc` next to the other initialisers (`src/scene.c:543-546`)
and leave `scene_free` (`src/scene.c:327-340`) untouched.

### Collector (`src/scene.c`, static, called after the prim loop)

```c
static void scene_collect_emissive_lights(Scene *s)
{
    s->emissive_light_count = 0;
    if (!s->materials) return;
    for (int i = 0; i < s->geo.count; ++i) {
        if (s->emissive_light_count >= SCENE_MAX_EMISSIVE_LIGHTS) break; /* bounded */
        const Primitive *p = &s->geo.prims[i];
        if (p->kind != PRIM_SPHERE) continue;                 /* v1: spheres only */
        if (p->material_index < 0 || p->material_index >= s->material_count) continue;
        const Material *m = &s->materials[p->material_index];
        if (!m->pbr) continue;                                /* opt-in gate */
        if (m->emissive.x == 0.0 && m->emissive.y == 0.0 && m->emissive.z == 0.0)
            continue;                                         /* non-emitter  */
        EmissiveLight *l = &s->emissive_lights[s->emissive_light_count++];
        l->prim_index = i;
        l->center     = p->center;
        l->radius     = p->radius;
        l->emissive   = m->emissive;
    }
}
```

The list is **bounded** (`SCENE_MAX_EMISSIVE_LIGHTS`) and built in deterministic
primitive/file order, so it is identical for every run and thread count.

---

## 3. Sampling one emissive area light (exact math)

For a sphere emitter with centre `C`, radius `R`, emitted radiance `Le =
emissive`, and a shading point `P` with normal `N`, view `V`:

```
D   = C - P          dc  = |D|        w = D / dc
```

If `dc <= R` the shading point is inside/on the emitter → **skip** this light
(no self-illumination, no divide-by-zero).

### 3a. Uniform **solid-angle** cone sampling (recommended)

The sphere subtends a cone of half-angle `alpha_max` with

```
sin(alpha_max) = R / dc        cos(alpha_max) = sqrt(1 - (R/dc)^2)
```

The visible spherical cap has solid angle `Omega = 2π·(1 - cos(alpha_max))`, so
the uniform-in-solid-angle pdf is

```
pdf(w_i) = 1 / (2π·(1 - cos(alpha_max)))
```

Sampling (using the deterministic RNG below):

```
u1, u2 ∈ [0,1)
cos_alpha = 1 - u1·(1 - cos(alpha_max))       // uniform in solid angle
sin_alpha = sqrt(max(0, 1 - cos_alpha^2))
phi       = 2π·u2
{t, b}    = orthonormal_basis(w)              // reuse sky_basis (src/material.c:362)
w_i       = normalize( cos_alpha·w + sin_alpha·(cos(phi)·t + sin(phi)·b) )
```

Because `alpha <= alpha_max`, the ray `P + t·w_i` is **guaranteed** to hit the
emitter's sphere, so the visibility test is a pure occlusion query.

### 3b. Emitted radiance toward P and the estimator

The rendering equation in solid-angle form is

```
L_o(P) = ∫ f_r(w_i, w_o) · Le · (N · w_i) dω_i
```

Sampling `w_i` uniformly over the cap gives the unbiased single-sample estimator

```
contribution = f_r(w_i, w_o) · Le · (N · w_i) · V(P, Q) / pdf(w_i)
             = f_r(w_i, w_o) · Le · (N · w_i) · V(P, Q) · 2π·(1 - cos(alpha_max))
```

- `Le` is **radiance** (distance-independent), so the `1/dc²` inverse-square
  falloff and the `cos(light)` geometry term are *already folded into the
  solid-angle measure* — no separate `1/r²` or `cos_l` factor may be added.
- `V(P, Q)` is the binary visibility (shadow-ray) term below.
- `N · w_i <= 0` → the sample contributes nothing (back-facing).

Then average over `EMISSIVE_LIGHT_SAMPLES` samples (see §4).

### 3c. Visibility (shadow ray)

```c
Vec3 shadow_o = vec3_add(P, vec3_scale(N, 1e-3));   /* same eps as the sun path */
Hit sh;
int visible = !scene_intersect(scene, (Ray){shadow_o, w_i}, 1e-3, 1e30, &sh)
           || sh.prim_index == light->prim_index;   /* emitter is the nearest hit */
```

Casting the ray unbounded and accepting when the nearest hit *is* the emitter
is robust (no analytic sphere-`t` needed) and handles occluders behind the lamp
correctly.

### 3d. Reusing the existing BRDF helpers

`material_shade_pbr` / `material_shade_local` already return
`light_color · (N·w_i) · f_r`. Passing

```
light_color = Le · (1/pdf) · V          (a Vec3)
```

makes the helper produce exactly the §3b estimator term. This mirrors the sun
path (`src/render.c:187-195`) and keeps PBR/legacy surfaces consistent:

```c
Vec3 radiance = vec3_scale(lt->emissive, solid_angle);   /* Le / pdf, V folded by skip */
Vec3 term = mm->pbr ? material_shade_pbr(mm, N, w_i, V, radiance)
                    : material_shade_local(mm, N, w_i, V, radiance);
```

### 3e. Deterministic RNG (reuse the sun-disk machinery)

Reuse `render_hash3` / `render_rand01` (`src/render.c:62-83`). Use **distinct
channel constants** from the sun-disk path (`0x5a17`/`0x7c3d`, `src/render.c:179-180`)
so the existing sun-disk stream is not perturbed:

```c
unsigned k  = render_hash3(seed_key ^ 0xE311u, (unsigned)li, (unsigned)si);
double  u1 = render_rand01(k, (unsigned)si, 0x4c11u);
double  u2 = render_rand01(k, (unsigned)si, 0x9d27u);
```

`seed_key` is already the per-primary-ray key `render_hash3(x, y, s)`
(`src/render.c:423-426`); `li` (light index) and `si` (sample index) are plain
loop counters. The helper is therefore a **pure function of (P, N, V, seed_key,
scene, light list)**.

### 3f. Alternative: area (surface) sampling (documented, not recommended)

Uniform on the sphere: `Q = C + R·n`, `n` uniform on the unit sphere,
`pdf_area = 1/(4πR²)`. Convert to solid angle with `pdf_ω = pdf_area · cos_l /
|P-Q|²`, where `cos_l = dot(normalize(Q-C), normalize(P-Q))`, clamped `> 0`.
The estimator simplifies to `f_r · Le · (N·w_i) · 4πR² · V`, but ~half the
samples land on the back hemisphere (`cos_l <= 0` → zero contribution), so it
has strictly higher variance than §3a for the same sample count. Use §3a.

---

## 4. Sample count and determinism

- **Constant**: `#define EMISSIVE_LIGHT_SAMPLES 16` (same as
  `SUN_SHADOW_SAMPLES`). Cost per lit, non-emitter hit is
  `O(light_count × EMISSIVE_LIGHT_SAMPLES)` extra shadow rays — with the bounds
  below that is at most `8 × 16 = 128` shadow rays, comparable to existing soft
  shadows.
- **Bounds**: `SCENE_MAX_EMISSIVE_LIGHTS 8` (first 8 in file order) +
  `EMISSIVE_LIGHT_SAMPLES 16`. A total-budget variant
  (`samples = max(4, BUDGET / light_count)`) is possible but the fixed pair is
  simpler and predictable.
- **Determinism for any thread count**: the light list is built once,
  single-threaded, read-only during render; sample indices come from `seed_key`
  (pixel, sample) plus loop counters. Nothing reads thread id, tile id, time, or
  globals — so the output is byte-identical for any thread count, exactly like
  the existing sun-disk soft shadows (`src/render.c:119-123`).
- **Self-lighting**: skip the light whose `prim_index == h->prim_index` so an
  emitter is not lit by its own surface (it already gets `emissive` added at
  `src/render.c:300-312`).

---

## 5. Exact backward-compat gating

**Gating condition (the whole feature is one guard):**

```c
/* in trace_hit, right after the sun block (after src/render.c:210) */
if (scene->emissive_light_count > 0) {
    color = vec3_add(color, emissive_direct(scene, mm, P, N, V,
                                            h->prim_index, seed_key));
}
```

When `emissive_light_count == 0` the block is **never entered**, `color` is
untouched, and no extra RNG draws happen → byte-identical output.

Why the default and all emissive-free scenes yield `count == 0`:

1. The collector only appends a light when the primitive is a `PRIM_SPHERE`
   whose material has `pbr != 0` **and** a non-zero `emissive` (§2).
2. `pbr`/`emissive` default to `0` / `(0,0,0)` and are opt-in
   (`src/material.h` `MATERIAL_DEFAULT_PBR`, `MATERIAL_DEFAULT_EMISSIVE`), so
   the built-in default scene (no `--scene`) defines **no** such material.
3. Therefore the list is empty, the guard is false, and the renderer behaves
   exactly as before. Scenes that *do* opt into an emissive PBR emitter are the
   only ones whose output changes — which is the point.

Two extra precautions to guarantee byte-identity:
- Use **new** RNG channel constants (`0xE311`, `0x4c11`, `0x9d27`) so the
  existing sun-disk draws (`src/render.c:178-180`) are byte-for-byte unchanged.
- Keep `trace_hit`'s existing statement order (ambient → sun → reflection →
  Fresnel/refraction → emissive) and append the new block **after** the sun
  block, so no floating-point operation is reordered.

---

## 6. Concrete change list + implementation outline

| File | Change |
|---|---|
| `src/scene.h` | Add `EmissiveLight` struct, `SCENE_MAX_EMISSIVE_LIGHTS`, and the `emissive_lights[]` / `emissive_light_count` fields to `Scene`. |
| `src/scene.c` | Zero the count in `scene_build_from_desc` (`~543-546`); add static `scene_collect_emissive_lights()`; call it after the primitive loop (before or after `bvh_build` at `:599`). `scene_free` unchanged (fixed array). |
| `src/material.h` | Declare `Vec3 light_sphere_sample_dir(Vec3 w, double cos_alpha_max, double u1, double u2);` (pure, vec3-only — keeps `material.h`'s no-geometry/no-scene rule). |
| `src/material.c` | Implement `light_sphere_sample_dir` next to `sky_sun_disk_dir` (`~377`), reusing the static `sky_basis` (`:362`). |
| `src/render.c` | Add `#define EMISSIVE_LIGHT_SAMPLES 16`; add static `emissive_direct(...)`; add the guarded `vec3_add` after the sun block (`after :210`). |
| `tests/test_emissive_lights.c` | New deterministic test (below); the Makefile auto-discovers `tests/*.c`. |

### Implementation outline (`emissive_direct`, ~40 lines)

```c
static Vec3 emissive_direct(const Scene *scene, const Material *mm,
                            Vec3 P, Vec3 N, Vec3 V, int self_prim,
                            unsigned seed_key)
{
    Vec3 sum = vec3(0.0, 0.0, 0.0);
    Vec3 shadow_o = vec3_add(P, vec3_scale(N, 1e-3));
    for (int li = 0; li < scene->emissive_light_count; ++li) {
        const EmissiveLight *lt = &scene->emissive_lights[li];
        if (lt->prim_index == self_prim) continue;              /* no self-light */
        Vec3   to_c = vec3_sub(lt->center, P);
        double dc2  = vec3_length_sq(to_c);
        double dc   = sqrt(dc2);
        if (dc <= lt->radius) continue;                          /* inside lamp  */
        double sin2   = (lt->radius * lt->radius) / dc2;
        double cos_mx = sqrt(1.0 - sin2);                        /* cos(alpha_max)*/
        double solid  = 2.0 * MATERIAL_PI * (1.0 - cos_mx);      /* = 1/pdf      */
        if (solid <= 0.0) continue;
        Vec3 w = vec3_scale(to_c, 1.0 / dc);
        for (int si = 0; si < EMISSIVE_LIGHT_SAMPLES; ++si) {
            unsigned k  = render_hash3(seed_key ^ 0xE311u, (unsigned)li, (unsigned)si);
            double  u1  = render_rand01(k, (unsigned)si, 0x4c11u);
            double  u2  = render_rand01(k, (unsigned)si, 0x9d27u);
            Vec3 w_i = light_sphere_sample_dir(w, cos_mx, u1, u2);
            if (vec3_dot(N, w_i) <= 0.0) continue;
            Hit sh;
            int visible = !scene_intersect(scene, (Ray){shadow_o, w_i}, 1e-3, 1e30, &sh)
                       || sh.prim_index == lt->prim_index;
            if (!visible) continue;
            Vec3 radiance = vec3_scale(lt->emissive, solid);     /* Le / pdf */
            Vec3 term = mm->pbr ? material_shade_pbr(mm, N, w_i, V, radiance)
                                : material_shade_local(mm, N, w_i, V, radiance);
            sum = vec3_add(sum, term);
        }
    }
    return vec3_scale(sum, 1.0 / (double)EMISSIVE_LIGHT_SAMPLES);
}
```

(`MATERIAL_PI` is the local constant already defined in `src/material.c:27`;
redefine locally in `render.c` if the helper stays there.)

### Deterministic test strategy (`tests/test_emissive_lights.c`)

The Makefile links every `tests/*.c` against all objects except `main.o`
(`Makefile:26-31`), so a self-contained `int main(void)` returning non-zero on
failure matches the existing `CHECK` harness style (`tests/test_softshadow.c:38-47`).

1. **Sampling unit tests** (pure math, no render):
   - `light_sphere_sample_dir` returns unit vectors for many `(u1,u2)`.
   - Every sampled direction from a point `P` outside the sphere **hits that
     sphere** (verified via `primitive_intersect`), i.e. `alpha <= alpha_max`.
   - `u1 = 0` returns the central direction `w` (bit-for-bit).
   - Degenerate inputs (`dc <= R`) are handled without NaN.
2. **Collection test**: build a `SceneDesc` with one emissive PBR sphere →
   `scene.emissive_light_count == 1` with the right `center`/`radius`/`emissive`;
   the same desc with `emissive = 0 0 0` or `pbr = 0` → `count == 0`.
3. **Backward-compat test**: build the built-in default desc
   (`scene_default_desc`) → `emissive_light_count == 0`. Optionally render a
   small image and compare it byte-for-byte against a stored reference, or
   against a build where the guard block is absent.
4. **Lamp-brightens-surface test** (the core requirement): two tiny scenes,
   identical except the lamp's `emissive` (`0 0 0` vs a warm `3.0 2.4 1.6`),
   each = a small ground plane + a sphere lamp above it. Render e.g. `32×32`,
   `spp = 1`, `max_depth = 1`. Assert the linear/byte value of the pixel
   directly under the lamp is **strictly brighter** with the emissive lamp than
   without, and that a far-away ground pixel changes less (falloff sanity).
5. **Determinism test**: render the emissive scene twice → byte-identical; and
   (under `-DUSE_PTHREADS`) render threaded vs single-threaded → byte-identical,
   proving the per-`seed_key` sampling is thread-count independent.

---

## 7. Summary

- **Where**: `trace_hit` direct-light block (`src/render.c:161-210`); new guarded
  block appended after it. Light list traverses `Scene.geo.prims` +
  `Scene.materials` (`src/scene.h:33-41`, `src/geometry.h:28-49`).
- **Collect**: fixed, bounded `EmissiveLight[]` filled in `scene_build_from_desc`
  where `pbr`/`emissive` are already resolved (`src/scene.c:553-575`).
- **Sample**: uniform **solid-angle cone** sampling of the visible spherical
  cap — `cos(alpha_max) = sqrt(1 - (R/dc)²)`, `pdf = 1/(2π(1-cos(alpha_max)))`,
  estimator `f_r · Le · (N·w_i) · V / pdf`, reusing `material_shade_pbr` /
  `material_shade_local` and the hash RNG (`render_hash3`/`render_rand01`).
- **Count**: `EMISSIVE_LIGHT_SAMPLES = 16`, `SCENE_MAX_EMISSIVE_LIGHTS = 8`;
  deterministic for any thread count via `seed_key`.
- **Gate**: `if (scene->emissive_light_count > 0)`; empty for the default scene
  and every scene without an emissive PBR material → byte-identical output.
