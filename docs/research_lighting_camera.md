# Research: Soft Shadows (sun-disk) & Depth of Field (thin lens)

Scope: read-only investigation to unblock execution-plan tasks **t-007** (soft shadows) and
**t-008** (depth of field). No source code was modified; this file is the only artifact.

All paths are workspace-relative. Line numbers are exact as of the current tree.

---

## 0. Executive summary / key findings

- **Soft shadows** hook point is a single 9-line block in `src/render.c`
  (`trace_hit`, lines 120–128). The existing deterministic PRNG
  (`render_rand01`, `src/render.c:78`) is already in scope there — no new plumbing
  needed. `SkyParams` (`src/material.h:43-58`) needs one new field `sun_radius`.
- **Depth of field** hook point is `camera_ray` (`src/camera.c:61`, declared
  `src/camera.h:35`) plus its one call site `render_region` (`src/render.c:304`).
  The `Camera` struct (`src/camera.h:16-21`) needs `aperture` + `focus_distance`.
- **CRITICAL WIRING GAP for DOF:** the `camera { }` block is parsed into
  `SceneDesc.camera` (`CameraDesc`) but **`src/main.c` never uses it**. `main.c:394`
  calls `scene_default_view()` and `main.c:395` builds the camera from those
  hardcoded values, discarding `desc.camera`. So "no CLI changes" is true, but a
  **small `main.c` wiring change is required** for DOF to be scene-file-driven
  (details in §4).
- Both features are **fully deterministic** because every random value is a pure
  function of integer hash inputs; no `rand()`, no clock in the render path.

---

## 1. SOFT SHADOWS (sun-disk / area-light sampling)

### 1.1 Current hard-shadow code (exact quote)

`src/render.c`, inside `trace_hit` (defined `src/render.c:98-215`), lines **120–128**:

```c
    /* --- direct sunlight with a hard shadow ray --- */
    Vec3 color = material_ambient(m, N, &scene->sky);
    Vec3 L = scene->sky.sun_dir;
    Vec3 shadow_o = vec3_add(P, vec3_scale(N, 1e-3));
    Hit sh;
    if (!scene_intersect(scene, (Ray){shadow_o, L}, 1e-3, 1e30, &sh)) {
        color = vec3_add(color,
                         material_shade_local(m, N, L, V, scene->sky.sun_color));
    }
```

- `L` is the constant `scene->sky.sun_dir` (a **unit** vector pointing toward the sun;
  see `SkyParams.sun_dir` comment `src/material.h:44`).
- The shadow ray is `(Ray){shadow_o, L}` with origin nudged off the surface by
  `N * 1e-3` and the scene queried over `[1e-3, 1e30]`.
- On occlusion the whole direct term is dropped (binary / hard shadow).

### 1.2 `SkyParams` struct and `sky_sample`

`SkyParams` — `src/material.h:43-58` (fields quoted in full):

```c
typedef struct {
    Vec3   sun_dir;           /* unit vector pointing TOWARD the sun     */
    Vec3   sun_color;         /* sun light color (linear, can exceed 1)  */
    Vec3   horizon_color;
    Vec3   zenith_color;
    double gradient_gamma;    /* e.g. 0.5 .. 1.0                         */
    double sun_glow_exponent; /* e.g. 350                                */
    double sun_glow_strength; /* e.g. 0.8                                */
    double cloud_height;      /* world-space height of the cloud layer   */
    double cloud_scale;       /* fBm frequency, e.g. 0.001..0.005        */
    double cloud_coverage;    /* 0.4 .. 0.7                              */
    double cloud_softness;    /* 0.05 .. 0.25                            */
    double cloud_sharpness;   /* extra pow() on density, e.g. 1.0 .. 3.0 */
    int    cloud_octaves;     /* 4 .. 6                                  */
    unsigned seed;
} SkyParams;
```

- **No `sun_radius` field exists** (confirmed by grep across `src/`, `docs/`, `scenes/`, `tests/`).
- Defaults are set in `sky_default_params` (`src/material.c:51-74`); add the new field there
  (default `0.0` = hard shadow, preserving current output byte-for-byte).
- `sky_sample` signature: `Vec3 sky_sample(Vec3 dir, const SkyParams *sky);`
  (`src/material.h:93`, impl `src/material.c:165`). It is a pure function of direction +
  params, and already consumes `sun_dir` (sun glow term `src/material.c` around lines
  182–188). **It does not need to change for soft shadows** — the disk is a *shadow-ray*
  concept, not a sky-background concept. (Optionally the glow could later be widened to
  match `sun_radius`, but that is cosmetic and out of scope.)

### 1.3 The deterministic integer-hash jitter PRNG (existing)

`src/render.c:50-82`. Exact signatures:

```c
static unsigned render_hash_u32(unsigned x);                       /* line 57 */
static unsigned render_hash3(unsigned a, unsigned b, unsigned c);  /* line 68 */
static double   render_rand01(unsigned a, unsigned b, unsigned c); /* line 78 */
```

- `render_rand01(a, b, c)` returns a uniform double in `[0, 1)` from the hash of three
  `unsigned` keys. It is a **pure function** — same `(a,b,c)` always yields the same value,
  on any thread and any run/platform. This is the project's "small integer hash PRNG".
- Existing usage pattern (`sample_offset`, `src/render.c:240-255`): the three keys are
  `(pixel_x, pixel_y, sample_index_channel)`, e.g. `render_rand01(px, py, s*2+0)`.
- **Availability:** `render_rand01` is a `static` function in `render.c`, already in
  lexical scope inside `trace_hit` — soft-shadow sampling can call it directly with **zero
  new includes or plumbing**.

### 1.4 Proposed integration (soft shadows)

**a) Data model (`SkyParams`, `src/material.h`).** Add one field, e.g.:

```c
    double sun_radius;        /* angular radius of the sun disk, radians (0 = hard) */
```

Add the corresponding default in `sky_default_params` (`src/material.c`, after line 65):
`p->sun_radius = 0.0;` (or a small default such as `0.0093` ≈ the real sun's ~0.53°).

**b) New helper (recommended location: `src/material.c` / declared `src/material.h`).**
Because `render.c` and `material.c` are both shared by sequential tasks, keep the *math*
in `material.c` (it already owns sky/shading math) and keep only the *loop* in `render.c`.
Proposed signature:

```c
/* Build a unit direction toward a jittered point on the sun disk: a cone of
 * half-angle `radius` (radians) around the unit vector `sun_dir`.
 * `r1`, `r2` are uniform samples in [0,1) supplied by the caller's PRNG, so this
 * function stays pure and deterministic. radius <= 0 returns sun_dir unchanged. */
Vec3 sky_sun_disk_dir(Vec3 sun_dir, double radius, double r1, double r2);
```

Implementation sketch (pure, no state): build an orthonormal basis `(t, b)` around
`sun_dir` (Gram–Schmidt against a non-parallel axis, exactly as `camera_create` does for
its look-at basis at `src/camera.c:28-40`), then
`offset = sqrt(r1) * radius` (uniform disk via `r = sqrt(u)`, angle `theta = 2*pi*r2`),
`dir = normalize(sun_dir + tan(offset)*(cos(theta)*t + sin(theta)*b))`.

> Note: a full orthonormal-basis helper does **not** exist in `vec3.h`/`geometry.h`
> (confirmed by grep); the only existing basis construction is inline in `camera.c`.
> Implement the small basis inline in the helper (≈5 lines) — do not add a shared
> `vec3` basis function unless a later task needs it.

**c) The per-hit sampling loop (location: `src/render.c`, `trace_hit`, replace lines 120–128).**
The loop lives in `trace_hit` because that is where the hit point `P`, the (possibly
water-perturbed) normal `N`, and `V` are available. New logic:

```c
    /* --- direct sunlight with a soft (sun-disk) shadow ray --- */
    Vec3 color = material_ambient(m, N, &scene->sky);
    double sr = scene->sky.sun_radius;
    Vec3 shadow_o = vec3_add(P, vec3_scale(N, 1e-3));
    if (sr > 0.0) {
        const int N_SHADOW = 4;            /* configurable constant */
        double lit = 0.0;
        for (int i = 0; i < N_SHADOW; ++i) {
            double r1 = render_rand01(h->prim_index, (unsigned)i, 0x5un);
            double r2 = render_rand01(h->prim_index, (unsigned)i, 0x9un);
            Vec3 Ld = sky_sun_disk_dir(scene->sky.sun_dir, sr, r1, r2);
            Hit sh;
            if (!scene_intersect(scene, (Ray){shadow_o, Ld}, 1e-3, 1e30, &sh)) {
                lit += 1.0;
            }
        }
        if (lit > 0.0) {
            Vec3 L = scene->sky.sun_dir;   /* use the central direction for shading */
            color = vec3_add(color, vec3_scale(
                material_shade_local(m, N, L, V, scene->sky.sun_color),
                lit / (double)N_SHADOW));  /* visibility fraction in [0,1] */
        }
    } else {
        Hit sh;
        if (!scene_intersect(scene, (Ray){shadow_o, scene->sky.sun_dir},
                             1e-3, 1e30, &sh)) {
            color = vec3_add(color, material_shade_local(
                m, N, scene->sky.sun_dir, V, scene->sky.sun_color));
        }
    }
```

**d) Determinism & PRNG key choice.** `trace_hit` does **not** currently receive the pixel
`(x, y)` or the sample index `s`. To keep output byte-identical regardless of thread/tile
scheduling, the jitter keys must be a **pure function of (pixel, sample, hit)** — never of
thread or tile. Two options:

- *Minimal-diff option:* key on `h->prim_index` (available in `Hit`, `src/geometry.h:59`)
  plus the ray-sample counter `i`. This is deterministic but gives every hit on the same
  primitive the same shadow pattern (visually fine; still deterministic).
- *Preferred option:* thread `unsigned seed_key` (or `(unsigned px, unsigned py, unsigned s)`)
  down through `trace()` → `trace_hit()` and key on it. This changes two private function
  signatures (`trace` at `src/render.c:89` and `trace_hit` at `src/render.c:98`) plus the
  two recursive call sites (`src/render.c:134` reflection, `src/render.c:189` refraction)
  and the top-level call (`src/render.c:305`). More edits, better spatial decorrelation.

Recommend the **preferred** option, since `render.c` is already a shared/serialized file
and the seed threading is a clean, self-contained change.

**e) Performance trade-off.** Cost is **linear in `N_SHADOW`**: each extra shadow sample is
one full `scene_intersect` (BVH query) per lit hit. With `N_SHADOW = 4` expect roughly
`1 + 4×(shadow fraction of rays)` × primary-hit cost; typical scenes are ~2–3× slower than
hard shadows, worst case ~5×. Hard shadows must remain **free** when `sun_radius == 0`
(the `else` branch above), so existing scenes/tests and the byte-identical default
(`scenes/default.scene`) are unaffected. Make `N_SHADOW` a `#define` constant (not a CLI
flag) so no CLI changes are required; it can later be promoted to a `sky` key if desired.

---

## 2. DEPTH OF FIELD (thin-lens aperture)

### 2.1 Camera struct and `camera_ray` (exact quotes)

`Camera` — `src/camera.h:16-21`:

```c
typedef struct {
    Vec3 position;   /* eye */
    Vec3 lower_left; /* viewport lower-left corner in world space */
    Vec3 horizontal; /* full-width viewport vector */
    Vec3 vertical;   /* full-height viewport vector */
} Camera;
```

`camera_ray` declaration — `src/camera.h:35`:

```c
Ray camera_ray(const Camera *cam, double u, double v);
```

Implementation — `src/camera.c:61-74`:

```c
Ray camera_ray(const Camera *cam, double u, double v)
{
    Ray r;
    r.origin = cam->position;
    /* point = lower_left + u*horizontal + v*vertical */
    Vec3 point = vec3_add(cam->lower_left,
                          vec3_add(vec3_scale(cam->horizontal, u),
                                   vec3_scale(cam->vertical, v)));
    r.dir = vec3_normalize(vec3_sub(point, cam->position));
    return r;
}
```

Important property: the viewport sits at **unit distance** in front of the eye
(`camera_create`, `src/camera.c:41-51` builds `lower_left = from + forward - 0.5*h - 0.5*v`),
so the "focal distance" is folded into the FOV and the ray direction is already normalized.

### 2.2 Primary-ray generation per pixel/sample

`src/render.c`, `render_region`, lines **296–306**:

```c
        for (int x = x0; x < x1; ++x) {
            Vec3 acc = vec3(0.0, 0.0, 0.0);
            for (int s = 0; s < spp; ++s) {
                double du, dv;
                sample_offset(spp, s, (unsigned)x, (unsigned)y, &du, &dv);
                double uu = ((double)x + du) / (double)width;
                double vv = 1.0 - ((double)y + dv) / (double)height;
                Ray ray = camera_ray(cam, uu, vv);
                acc = vec3_add(acc, trace(scene, ray, 0, max_depth, time));
            }
```

So `(x, y, s)` are available here, and `sample_offset` already draws `(du, dv)` jitter via
`render_rand01(px, py, s*2+0/1)`. This is the ideal place to also draw the **lens** sample.

### 2.3 Proposed integration (thin-lens DOF)

**a) Data model (`Camera`, `src/camera.h`).** Add two fields:

```c
    double aperture;       /* lens radius, world units (0 = pinhole, no DOF) */
    double focus_distance; /* distance from eye to the focal plane, world units */
```

`camera_create` (`src/camera.c:24`) does **not** take these; initialize them to
`0.0` in `camera_create` so a plain pinhole is the default (byte-identical output). The DOF
values come from the scene file (§4), so extend `camera_create`'s signature is *not* required
if `main.c` sets `cam.aperture` / `cam.focus_distance` after construction — recommended to
avoid touching all existing `camera_create` call sites/tests
(`tests/test_math.c:221`, `tests/test_integration.c:198`, `src/scene.c:382`, `src/main.c:395`).

**b) New helper (recommended location: `src/camera.c` / declared `src/camera.h`).**
Keep `camera_ray` unchanged (it stays the pinhole primitive) and add a lens-aware helper so
existing callers/tests keep working:

```c
/*
 * Thin-lens primary ray for normalized image coords (u,v) in [0,1]^2.
 * `r1`, `r2` are uniform samples in [0,1) from the caller's PRNG. When
 * cam->aperture <= 0 this is exactly camera_ray(cam, u, v) (pinhole).
 *
 * Lens sample: a uniform point on a disk of radius `aperture` in the camera
 * plane, centred at cam->position. Focal point: the intersection of the
 * pinhole ray with the plane at `focus_distance` along the view direction.
 * The lens point aims at that focal point, so objects at focus_distance are
 * sharp and everything else blurs.
 */
Ray camera_ray_dof(const Camera *cam, double u, double v, double r1, double r2);
```

Implementation sketch (pure): compute the pinhole ray exactly as `camera_ray` does, take
`focal = vec3_at(pinhole, focus_distance)`; build the camera-plane basis from
`cam->horizontal`/`cam->vertical` normalized; set
`origin = position + aperture*sqrt(r1)*(cos(2*pi*r2)*right + sin(2*pi*r2)*up)`;
`dir = normalize(focal - origin)`.

> The camera basis vectors are available as `cam->horizontal`/`cam->vertical`; normalize them
> to get the lens-plane axes. (No shared ONB helper exists; see §1.4 note.)

**c) Call-site change (`src/render.c`, `render_region`, line 304).** Draw the lens sample
from the **same** PRNG and route through the DOF helper:

```c
                double du, dv;
                sample_offset(spp, s, (unsigned)x, (unsigned)y, &du, &dv);
                double uu = ((double)x + du) / (double)width;
                double vv = 1.0 - ((double)y + dv) / (double)height;
                double lr1 = render_rand01((unsigned)x, (unsigned)y,
                                           (unsigned)(s * 2 + 2));
                double lr2 = render_rand01((unsigned)x, (unsigned)y,
                                           (unsigned)(s * 2 + 3));
                Ray ray = (cam->aperture > 0.0)
                          ? camera_ray_dof(cam, uu, vv, lr1, lr2)
                          : camera_ray(cam, uu, vv);
```

**d) Determinism.** `lr1`/`lr2` are drawn from `render_rand01(x, y, s*2+2 / s*2+3)` — a pure
function of `(pixel, sample)`, exactly like the existing AA jitter (`sample_offset` uses
`s*2+0` / `s*2+1`). Using the **unused channels `s*2+2`, `s*2+3`** guarantees no collision
with the AA jitter, so the lens sample is deterministic and identical across any thread
count / tile split. Confirmed: `render_region` writes each pixel as a pure function of
`(x, y, s)` (doc comment `src/render.c:282-286`).

**e) Performance trade-off.** DOF adds **zero** extra rays per sample — it only changes the
ray origin/direction — so it is essentially free. Blur quality scales with `spp` (more
samples = smoother bokeh); no new sample loop is required.

---

## 3. Scene-file-driven configuration (no CLI changes)

### 3.1 Soft shadows — CONFIRMED scene-file-driven, no CLI change

- Add `sun_radius` to `SkyParams` (`src/material.h:43-58`) + default in `sky_default_params`
  (`src/material.c:51-74`).
- Parser: add one `KeySpec` row to `SKY_KEYS` (`src/scene_desc.c:443-458`) and one bit to the
  sky enum (`src/scene_desc.c:399-412`, currently `SKY_SUN_DIR … SKY_SEED`). The generic
  applier `sd_apply_key` (`src/scene_desc.c:799`) + `sd_apply_value` (`src/scene_desc.c:765`)
  handle `KT_DOUBLE` automatically via `offsetof(SkyParams, sun_radius)` — **no new parsing
  code**, just the table row.
- Writer: add one line `w_key_double(w, "sun_radius", s->sun_radius);` to `emit_sky`
  (`src/scene_desc_write.c:325-348`, e.g. after line 339).
- `main.c`/CLI: **unchanged**. `Scene.sky` is a copy of `SceneDesc.sky` (`src/scene.c:480`),
  and `trace_hit` reads `scene->sky` directly. Done.

### 3.2 Depth of field — scene-file keys exist in the parser, BUT wiring to the camera is MISSING

- Parser side is already generic: `CameraDesc` (`src/scene_desc.h:74-82`) + `CAM_KEYS`
  (`src/scene_desc.c:436-441`) + camera enum (`src/scene_desc.c:393-397`). Adding
  `aperture` and `focus_distance` means **one field each in `CameraDesc`**, **one enum bit
  each**, and **one `CAM_KEYS` row each** (`offsetof(CameraDesc, aperture)` etc.). No new
  parse logic.
- Writer: `emit_camera` (`src/scene_desc_write.c:312-323`) — add two `w_key_double` lines.
- Defaults: `sd_apply_defaults` (`src/scene_desc.c:1449-1466`) sets `camera.*` defaults;
  add `d->camera.aperture = 0.0; d->camera.focus_distance = 0.0;` there and in
  `scene_default_desc` (`src/scene.c:560-566`).
- **CRITICAL GAP — `main.c` ignores `desc.camera`.** `src/main.c:393-395`:

```c
    /* 3. Camera with the actual output aspect ratio. */
    scene_default_view(&eye, &target, &up, &vfov);
    cam = camera_create(eye, target, up, vfov, (double)opt.width / (double)opt.height);
```

  `scene_default_view` (`src/scene.c:366-372`) returns **hardcoded** eye/target/up/vfov and
  `main.c` builds the camera from them; the parsed `desc.camera` is never read (it is freed
  at `main.c:391`). So today the `camera { }` block is effectively cosmetic for rendering.
  **For `aperture`/`focus_distance` to take effect, `main.c` must be changed to prefer
  `desc.camera` when `desc.camera.present` (mirroring how `scene_default_view` is the
  fallback), and to set `cam.aperture` / `cam.focus_distance` from the parsed values.**
  This is still **NO CLI change** (no new flags), but it *is* a `main.c` source change —
  flag this explicitly to the t-008 coder so it is not missed. Note `desc` is freed before
  the camera is built; the coder must capture the camera fields (or build the camera)
  **before** `scene_desc_free(&desc)` at `main.c:391`.

> Confirm: **Both features need NO CLI flag/option changes.** DOF does require the
> `main.c` camera-wiring fix described above; soft shadows require none.

---

## 4. Shared helpers & minimal touch-set per feature

### 4.1 Shared helpers

| Helper | Location | Notes |
|---|---|---|
| `render_rand01(a,b,c)` | `src/render.c:78` | **The** deterministic jitter PRNG. `static` — reusable directly inside `render.c` by both features; pure function of 3 `unsigned` keys. |
| `render_hash3` / `render_hash_u32` | `src/render.c:68` / `:57` | Underlying hash; no need to call directly. |
| `sample_offset` | `src/render.c:240` | Existing AA jitter; uses PRNG channels `s*2+0/1`. DOF must use **different** channels (`s*2+2/3`) to avoid collisions. |
| `Ray` | `src/vec3.h:22-25` | `{ Vec3 origin; Vec3 dir; }`, dir assumed normalized. Shared by both features; **do not modify**. |
| `Hit` | `src/geometry.h:55-62` | `t`, `point`, `normal`, `material_index`, `prim_index`, `front_face`. Soft shadows may key the PRNG on `prim_index` (minimal option) or thread a pixel/sample key. |
| `scene_intersect` | `src/scene.h:93` | Shadow-ray query; unchanged. |
| `material_shade_local` | `src/material.h:81`, impl `src/material.c:103` | Per-light Blinn-Phong; soft shadows scale its result by the visibility fraction. **No signature change.** |
| `material_ambient` | `src/material.h:85`, impl `src/material.c:136` | Unchanged. |
| `sky_sample` | `src/material.h:93`, impl `src/material.c:165` | Unchanged (sun disk is a shadow concept, not a background concept). |

No orthonormal-basis helper exists; both features need a ~5-line inline basis (see §1.4/§2.3).

### 4.2 Minimal function touch-set

**t-007 — Soft shadows (`render.c` / `material.c` are shared, serialized):**

1. `src/material.h` — add `double sun_radius;` to `SkyParams`; declare `sky_sun_disk_dir`.
2. `src/material.c` — `sky_default_params` (default `sun_radius`); add `sky_sun_disk_dir`.
3. `src/render.c` — `trace_hit` (replace the hard-shadow block, lines 120–128);
   if the *preferred* seed threading is used, also `trace` (`:89`), `trace_hit` (`:98`),
   the two recursive call sites (`:134`, `:189`) and the top-level call (`:305`).
4. `src/scene_desc.c` — `SKY_KEYS` row + sky enum bit.
5. `src/scene_desc_write.c` — one line in `emit_sky`.
6. New `tests/test_softshadow.c` (per execution plan).

**t-008 — Depth of field (`render.c` / `camera.*` / `main.c`):**

1. `src/camera.h` — add `aperture`, `focus_distance` to `Camera`; declare `camera_ray_dof`.
2. `src/camera.c` — initialize the two fields in `camera_create`; add `camera_ray_dof`
   (`camera_ray` itself stays unchanged).
3. `src/render.c` — `render_region` primary-ray loop (line 304): draw lens samples + call
   `camera_ray_dof`.
4. `src/scene_desc.h` — add `aperture`, `focus_distance` to `CameraDesc`.
5. `src/scene_desc.c` — camera enum bits, `CAM_KEYS` rows, `sd_apply_defaults`.
6. `src/scene_desc_write.c` — `emit_camera`: two `w_key_double` lines.
7. `src/scene.c` — `scene_default_desc` (`:560-566`): set the two new camera defaults.
8. **`src/main.c`** — **wire `desc.camera` into the render camera** (currently ignored,
   lines 393–395) and set `cam.aperture`/`cam.focus_distance` **before** `scene_desc_free`
   at line 391.
9. New `tests/test_dof.c` (per execution plan).

### 4.3 Serialization note

`src/render.c`, `src/material.c`, and `src/scene_desc.*` are declared as **strictly
serialized** shared files in `.marmel/execution_plan.md`. Concretely:

- **t-007 and t-008 both edit `src/render.c`** (t-007: `trace_hit`; t-008: `render_region`)
  and **both edit `src/scene_desc.c` / `src/scene_desc_write.c`** (different key tables).
- t-007 also edits `src/material.c`; t-008 also edits `src/camera.*` and `src/main.c`.
- Therefore **run t-007 and t-008 strictly sequentially**, and (per the plan) after the
  Phase-2 features that also touch `src/material.c` (t-005 textures) and
  `src/scene_desc.*` (t-005). Rebase/refresh line numbers before each edit since the shared
  files shift.

---

## 5. Verification checklist for the implementing tasks

- **Byte-identical baseline preserved:** with `sun_radius = 0` and `aperture = 0`, output
  must equal the current render (both features must short-circuit to the exact old code path).
  `scenes/default.scene` must remain byte-identical to `DEFAULT_SCENE_TEXT`.
- **Determinism:** render the same scene with `RAYTRACER_THREADS=1` and `RAYTRACER_THREADS=8`
  (threaded build) and diff the BMPs — must be byte-identical. This validates that all new
  PRNG keys are pure functions of `(pixel, sample, hit)`.
- **Round-trip:** `scene_desc_write` must emit `sun_radius` / `aperture` / `focus_distance`
  and reload to the same values (extends the existing round-trip tests).
- **Source of truth:** the `camera { }` wiring fix in `main.c` must be covered by a test that
  parses a scene file with `aperture > 0` and confirms the rendered image differs from the
  pinhole render.
