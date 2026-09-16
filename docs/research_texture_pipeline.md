# Procedural Texture Pipeline — Integration Report

Read-only research. Goal: add per-material **procedural textures** (checkerboard,
stripes, optionally noise) that modulate albedo (and optionally specular) as a
function of 3D world position.

All paths are workspace-relative. Line numbers refer to the current tree.

---

## 0. TL;DR — Recommended Integration

| Item | Decision |
|------|----------|
| New module | `src/texture.h` + `src/texture.c` (new files) |
| Header deps | `texture.h` includes ONLY `vec3.h` + `material.h` (respects material.h's self-containment rule; no geometry/scene/render) |
| Entry function | `Vec3 texture_albedo(const Material *m, Vec3 p);` |
| Optional specular | `Vec3 texture_specular(const Material *m, Vec3 p);` |
| New `Material` fields | `int texture_kind; double texture_scale; Vec3 texture_color_a, texture_color_b; unsigned texture_seed;` (see §7) |
| Call site | `src/render.c`, inside `trace_hit()` **after** `Vec3 P = h->point;` (line 110), before shading (line 121). Must feed a *local* copy of the material or pass the modulated albedo into the shade helpers. |
| Makefile | Add `src/texture.c` to the `SRCS` list (`Makefile:18`). |
| Coordinates | No UV / primitive-local coords exist in `src/`. Use **world-space positional** textures (`p.x`, `p.y`, `p.z`). |
| Determinism | No `rand()`/`time()`. Reuse `src/noise.{h,c}` (seed is an explicit argument). |

---

## 1. `Material` struct and pure shading helpers

### 1.1 Struct definition — `src/material.h:27-37`

```c
typedef struct {
    Vec3   albedo;        /* base diffuse color                          */
    Vec3   specular;      /* specular tint                               */
    double shininess;     /* Blinn-Phong exponent, e.g. 16..256          */
    double reflectivity;  /* 0..1 mirror mix                             */
    double transparency;  /* 0..1 (0 = opaque)                           */
    double ior;           /* index of refraction, e.g. 1.33 for water    */
    int    is_water;      /* 1 => wave-normal perturbation + depth tint  */
    Vec3   absorption;    /* per-channel Beer-Lambert coefficient        */
    Vec3   deep_color;    /* color of deep transmissive medium           */
} Material;
```

`Material` is a plain, copyable POD. There are **no** texture/UV fields today.

### 1.2 Module dependency rule — `src/material.h:8-10` (CONFIRMED, must respect)

> This module is intentionally self-contained: it depends ONLY on
> `src/vec3.h` (vector math) and `src/noise.h` (procedural noise). It must NOT
> include geometry/scene/render headers.

`material.h` currently includes only `"vec3.h"` (line 17). Any new texture
header **must** keep the same constraint — texture code cannot see `Hit`,
`Primitive`, `Scene`, or `Ray`.

### 1.3 Pure helper functions

| Function | Location | Signature |
|----------|----------|-----------|
| `fresnel_schlick` | `src/material.c:80` | `double fresnel_schlick(double cos_theta, double f0)` |
| `fresnel_schlick_rgb` | `src/material.c:88` | `Vec3 fresnel_schlick_rgb(double cos_theta, Vec3 f0)` |
| `material_shade_local` | `src/material.c:103` | `Vec3 material_shade_local(const Material *m, Vec3 N, Vec3 L, Vec3 V, Vec3 light_color)` |
| `material_ambient` | `src/material.c:136` | `Vec3 material_ambient(const Material *m, Vec3 N, const SkyParams *sky)` |
| `sky_sample` | `src/material.c:165` | `Vec3 sky_sample(Vec3 dir, const SkyParams *sky)` |
| `water_attenuate` | `src/material.c:324` | `Vec3 water_attenuate(const Material *m, Vec3 inner, double depth)` |

### 1.4 Where albedo / specular are consumed (the exact lines)

- `material_shade_local` — **albedo** at `src/material.c:117`:
  ```c
  Vec3 diffuse = vec3_scale(vec3_mul(m->albedo, light_color), ndl);
  ```
  **specular** at `src/material.c:129`:
  ```c
  spec = vec3_scale(vec3_mul(m->specular, light_color), spec_term);
  ```
- `material_ambient` — **albedo** at `src/material.c:158`:
  ```c
  return vec3_scale(vec3_mul(m->albedo, hemi), ambient_factor);
  ```

These two functions are the ONLY consumers of `m->albedo` / `m->specular` in
the shading path. **Consequence:** a texture that modulates albedo must either
(a) supply a modulated material copy to both helpers, or (b) pass a modulated
albedo into them. Option (a) is cleaner and requires no signature change.

---

## 2. `src/render.c` — `trace` and the shading path

### 2.1 Functions

- `trace()` — forward-declared at `src/render.c:88-90` (the comment at line 88
  notes "trace_hit() recurses back through trace()"), defined at
  `src/render.c:217`. It intersects (`scene_intersect`) and delegates to
  `trace_hit`.
- `trace_hit()` — **defined** at `src/render.c:98-215` (signature starts at
  line 98). **This is the single shading function** where all material lookups
  happen. (There is no separate forward declaration for `trace_hit`; the
  forward declaration at 88-90 is for `trace`.)

### 2.2 Data available at a hit — `src/render.c:105-118`

```c
const Material *m = scene_material(scene, h->material_index);   /* 105 */
if (!m) return sky_sample(r.dir, &scene->sky);
Vec3 P = h->point;                                              /* 110 */
Vec3 N = h->normal; /* already flipped to oppose the ray */     /* 111 */
if (m->is_water) { N = water_normal(P.x, P.z, time); }          /* 112-114 */
if (vec3_dot(N, r.dir) > 0) { N = vec3_neg(N); }                /* 115-117 */
Vec3 V = vec3_neg(r.dir); /* toward the eye */                  /* 118 */
```

**Available at a hit (all confirmed):**

| Data | Expression | Source |
|------|-----------|--------|
| Hit point (world space) | `h->point` → `P` | `Hit.point` |
| Unit normal (ray-opposing) | `h->normal` → `N` | `Hit.normal` |
| Material index | `h->material_index` | `Hit.material_index` |
| Primitive index | `h->prim_index` | `Hit.prim_index` (available but unused in render.c) |
| Front/back face | `h->front_face` (used at `src/render.c:159`) | `Hit.front_face` |
| Distance `t` | `h->t` (used e.g. `h2.t` at `src/render.c:194`) | `Hit.t` |

### 2.3 Hit record — `src/geometry.h:55-62` (quoted verbatim)

```c
typedef struct {
    double t;             /* distance along ray */
    Vec3 point;           /* hit position */
    Vec3 normal;          /* unit surface normal, oriented to face the incoming ray */
    int material_index;
    int prim_index;       /* index of the hit primitive in Geometry.prims */
    int front_face;       /* 1 if the ray hit the outside surface, 0 if inside */
} Hit;
```

Normals are **unit** and already flipped to oppose the ray (geometry.h:17-18).
`point` is filled in world space at `src/geometry.c:83-86`.

### 2.4 Shading code that multiplies albedo into the final colour

Inside `trace_hit()` (`src/render.c`):

```c
Vec3 color = material_ambient(m, N, &scene->sky);                 /* 121 */
Vec3 L = scene->sky.sun_dir;
Vec3 shadow_o = vec3_add(P, vec3_scale(N, 1e-3));
Hit sh;
if (!scene_intersect(scene, (Ray){shadow_o, L}, 1e-3, 1e30, &sh)) {
    color = vec3_add(color,
        material_shade_local(m, N, L, V, scene->sky.sun_color));  /* 127 */
}
```

The albedo enters `color` **only** through `material_ambient` (line 121) and
`material_shade_local` (line 127). The final assembly is:

```c
return vec3_lerp(color, surface, m->transparency);   /* 211 */
return vec3_lerp(color, refl_col, m->reflectivity);  /* 214 */
```

So the cleanest injection is to modulate the material **before** lines 121/127.

---

## 3. Best injection point & coordinate strategy

### 3.1 No UV / primitive-local coordinates exist

Confirmed by a search restricted to `src/` (`grep` for
`texture|uv|texcoord|checker` → **no matches in `src/`**; note: matches exist
in `docs/` prose but none in code), and by reading `Hit` (`src/geometry.h:55-62`)
and `Primitive` (`src/geometry.h:33-47`). There is **no** UV parameterization,
no per-vertex attribute, no object-space transform. Primitives are
spheres/planes/boxes/triangles/tapered cylinders (`src/geometry.h:25-31`) with
no texture coordinates.

### 3.2 Recommended: world-space positional procedural texture

Use the world-space hit point `P = h->point` directly. This is deterministic,
requires zero changes to geometry/intersection code, and works for every
primitive kind. Recommended formulas (all pure functions of `P`):

- **Checkerboard** (3D positional, i.e. triplanar-style by position):
  ```
  int cx = (int)floor(p.x * inv_scale);
  int cy = (int)floor(p.y * inv_scale);
  int cz = (int)floor(p.z * inv_scale);
  int parity = (cx + cy + cz) & 1;
  albedo = parity ? color_a : color_b;
  ```
  (Optional 2D variant using only `p.x`, `p.z` for a "tiled floor" look.)
- **Stripes** (axis-aligned):
  ```
  int band = (int)floor(p.y * inv_scale);   /* or p.x / p.z */
  albedo = (band & 1) ? color_a : color_b;
  ```
- **Noise-modulated blend** (organic look) via `src/noise.{h,c}`:
  ```
  double n = 0.5 * (noise_fbm3(p.x*s, p.y*s, p.z*s, 4, 2.0, 0.5, seed) + 1.0);
  albedo = vec3_lerp(color_a, color_b, n);
  ```

### 3.3 Where to call it

In `trace_hit()`, immediately after line 110 (`Vec3 P = h->point;`) and before
line 121. Make a **local mutable copy** of the material so the pure helpers
stay untouched:

```c
/* after Vec3 P = h->point; (line 110) */
Material m_local = *m;
m_local.albedo   = texture_albedo(m, P);              /* modulate albedo  */
m_local.specular = texture_specular(m, P);            /* optional         */
const Material *mm = &m_local;
/* then use `mm` at lines 121 and 127 instead of `m` */
```

**Note:** `m` is also used for `ior`/`transparency`/`reflectivity`/
`is_water`/`water_attenuate` later (lines 159-211). Those fields are untouched
by the texture, so using `mm` throughout is safe and consistent. Alternatively,
keep `m` for the physical fields and only pass modulated values into the two
shade helpers — but a single local copy is simplest and least error-prone.

### 3.4 Avoiding self-intersection / shading artifacts

The world-space position is continuous, so texture seams are only at integer
cell boundaries — expected for checker/stripes. No epsilon changes needed.

---

## 4. Proposed signatures & module layout

### 4.1 New module `src/texture.h` / `src/texture.c`

**Recommended:** a dedicated module (mirrors the existing one-module-per-concern
layout and keeps `material.c` focused). It must obey material.h's dependency
rule.

`src/texture.h` (proposed):

```c
#ifndef TEXTURE_H
#define TEXTURE_H

/*
 * texture.h - Procedural per-material textures (checkerboard, stripes, noise).
 * Self-contained: depends ONLY on vec3.h and material.h. Must NOT include
 * geometry/scene/render headers. All functions pure, no I/O, no globals.
 */

#include "vec3.h"
#include "material.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Modulated albedo at world-space point p for material m. */
Vec3 texture_albedo(const Material *m, Vec3 p);

/* Optional modulated specular tint (returns m->specular when untextured). */
Vec3 texture_specular(const Material *m, Vec3 p);

#ifdef __cplusplus
}
#endif
#endif /* TEXTURE_H */
```

`src/texture.c` includes `"texture.h"`, `"noise.h"`, `<math.h>`. Implement the
switch on `m->texture_kind` (see §7).

### 4.2 How `render.c` calls it

Add `#include "texture.h"` to `src/render.c` (near the other includes,
lines 22-27), then in `trace_hit()` do the local-copy modulation described in
§3.3. This is the only call site needed — all reflection/refraction recursion
flows back through `trace_hit`, so textures apply at every bounce automatically.

### 4.3 Dependency-rule compliance

- `texture.h` → `vec3.h`, `material.h` only. ✅ (no geometry/scene/render)
- `texture.c` → `texture.h`, `noise.h`, `<math.h>`. ✅
- `render.c` already includes `material.h` (line 26) and `geometry.h` (line 25);
  adding `texture.h` is fine (render.c is allowed to see everything).
- No cycles: `texture` does not include `render`; `material` does not include
  `texture` (texture consumes `Material`, not vice-versa).

---

## 5. Determinism requirements

- **Contract (`src/render.c:11`, `src/noise.h:8`):** no `rand()`, no `time()`,
  no global mutable state. Randomness is an integer-hash PRNG seeded from
  `(x, y, sample index, constant)` (`src/render.c:53-82`).
- Texture sampling must be a **pure function of (material fields, world point
  `p`, seed)**. Never call `rand()`/`clock()`/`time()`.
- Reuse `src/noise.h`:
  - `noise_value2/3`, `noise_perlin2/3` → `[-1, 1]`
  - `noise_fbm2/3` → `~[-1, 1]` (takes `octaves, lacunarity, gain, seed`)
  - `noise_turbulence2` → `[0, 1]`
  - The seed is always an explicit argument — store it in the material
    (`texture_seed`) so builds remain reproducible.
- `floor()` from `<math.h>` is deterministic and matches the existing
  `pow`/`sqrt` usage (material.c includes `<math.h>`).
- Threaded renderer: because sampling depends only on `p` (which depends only
  on the ray, itself a pure function of pixel/sample), output stays
  byte-identical across thread counts — matching the guarantee documented at
  `src/render.c:282` (render_region comment: "byte-identical for any thread
  count and any work-splitting strategy").

---

## 6. Makefile

`Makefile:18`:

```make
SRCS = src/vec3.c src/camera.c src/bmp.c src/noise.c src/geometry.c src/bvh.c src/material.c src/scene.c src/scene_desc.c src/scene_desc_write.c src/render.c
```

**Action:** append ` src/texture.c` to `SRCS`. `OBJS` is derived
(`Makefile:19`), and tests link `LIB_OBJS` (`Makefile:23`), so a new
`tests/test_texture.c` will be picked up automatically by the
`TEST_SRCS := $(wildcard tests/*.c)` rule (`Makefile:25`). Header deps are
auto-tracked via `-MMD -MP` (`Makefile:40`), so no other change is needed.

---

## 7. Suggested `Material` field additions (for the implementer)

Extend `Material` in `src/material.h:27-37` (all POD, keeps struct copyable):

```c
int    texture_kind;   /* 0=none, 1=checker, 2=stripes, 3=noise */
double texture_scale;  /* world units per cell (e.g. 1.0)        */
Vec3   texture_color_a;/* first cell color                       */
Vec3   texture_color_b;/* second cell color                      */
unsigned texture_seed; /* seed for noise kinds                   */
```

Zero-init default = `texture_kind == 0` ⇒ untextured (backwards compatible with
`scene_desc`'s zeroed `Material` at `src/scene_desc.h:93-96`). The declarative
scene format (`docs/scene_format.md` §4.3) and `scene_desc.c` /
`scene_desc_write.c` would need matching `texture_*` keys if the feature is to
be authored from `.scene` files — out of scope for this report but noted for
the implementer.

---

## 8. Exact-edit checklist (for downstream coder)

1. Create `src/texture.h` + `src/texture.c` (deps: vec3.h, material.h, noise.h).
2. Add texture fields to `Material` (`src/material.h:27-37`).
3. `#include "texture.h"` in `src/render.c` (near line 27).
4. In `trace_hit()` (`src/render.c`), after line 110 insert local-copy
   modulation; replace `m` with `mm` at lines 121 and 127.
5. Append `src/texture.c` to `SRCS` (`Makefile:18`).
6. (Optional) add `tests/test_texture.c` — auto-discovered.
7. (Optional, separate task) extend `scene_desc` parse/write for texture keys.

---

## Sources (workspace files, line numbers re-verified against the current tree)

- `src/material.h` (Material struct 27-37; dep-rule comment 8-10; `#include "vec3.h"` line 17; prototypes `fresnel_schlick` 69, `fresnel_schlick_rgb` 72, `material_shade_local` 81, `material_ambient` 85, `sky_sample` 93, `water_normal` 102, `water_attenuate` 115, `water_depth_tint` 120)
- `src/material.c` (shade 103-134; ambient 136-159; fresnel 80-97; sky_sample 165; water_attenuate 324)
- `src/render.c` (trace fwd decl 88-90; trace_hit 98-215; trace def 217-229; albedo consumers 121,127; hit data 105-118; front_face use 159; h2.t use 194; determinism note 11; PRNG 53-82; byte-identical note 282)
- `src/geometry.h` (Hit 55-62; Primitive 33-47; PrimKind enum 25-31; normals convention 17-18)
- `src/geometry.c` (hit fill 83-86)
- `src/noise.h` / `src/noise.c` (deterministic noise, seed-as-argument; `no rand()` note noise.h:8)
- `src/vec3.h` (Vec3, Ray, inline math)
- `src/scene.h` (Scene struct 32-40; scene_material 96)
- `src/scene_desc.h` (MaterialDesc 93-96)
- `Makefile` (SRCS 18; OBJS 19; LIB_OBJS 23; TEST_SRCS 25; `-MMD -MP` deps 40)
- `docs/scene_format.md` (§4.3 material keys)
