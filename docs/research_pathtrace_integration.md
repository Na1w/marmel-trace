# Path-tracer integration: file/line map & minimal-diff strategy

READ-ONLY reconnaissance for adding an opt-in unidirectional path tracer behind
`--pathtrace`, mirroring the `--adaptive` precedent so that `adaptive == 0`
(and `pathtrace == 0`) stays **byte-identical**.

All line numbers refer to the current tree (verified with `sed -n`/`grep -n`).

---

## 1. `src/render.c` / `src/render.h` structure

### Public API (`src/render.h`)
| Symbol | Location | Notes |
|---|---|---|
| `int render_image(...)` | render.h:29-30 | fixed-spp entry, 6 scalar args |
| `typedef struct { ... } RenderParams` | render.h:49-55 | fields below |
| `int render_image_ex(...)` | render.h:65-66 | takes `const RenderParams *` |
| `render_last_seconds/total_samples/max_samples` | render.h:72,78,84 | diagnostics |
| progress API | render.h:110-149 | `render_progress_enabled/begin/update/finish`, `RenderProgress`, `render_progress_init` |

`RenderParams` (render.h:49-55):
```c
typedef struct {
    int    samples_per_pixel;
    int    max_depth;
    int    adaptive;
    int    adaptive_max_spp;
    double adaptive_tau;
} RenderParams;
```

### `render_image` (render.c:1485-1588)
- 1488-1496: argument validation (returns 1/2/3).
- 1499: `const double time = 0.0;`
- 1502-1505: diagnostics `g_render_last_total_samples` / `g_render_last_max_samples`.
- 1508: `int progress = render_progress_enabled();`
- **`#ifdef USE_PTHREADS` branch (1510-1571):** tile grid at 1512-1513
  (`tiles_x/tiles_y = ceil(dim / RENDER_TILE_SIZE)`, `RENDER_TILE_SIZE = 16` at
  render.c:969); `choose_thread_count()` at 1514 (def 1041-1065); `RenderShared`
  filled 1516-1529; progress struct 1532-1536; thread spawn/join 1540-1567.
- **`#else` branch (1572-1585):** single `RenderProgress` with
  `total_expected = height` (1575), then `render_serial()` (1580-1581).
- 1571/1584: `g_render_last_seconds = render_now_seconds() - t0;`

### `render_image_ex` (render.c:1590-1628)
- 1593-1604: validation.
- **1611-1614: the GATING precedent** — `if (!params->adaptive) return render_image(...)`.
- 1616-1624: default resolution (`nmax = 4*n0`, `tau = 0.02`).
- 1626-1627: `return render_image_adaptive(...)`.

### Per-pixel sample loop — **the dispatch site**
`render_region` (render.c:892-938) is the single per-sample kernel loop:
```c
900   for (int s = 0; s < spp; ++s) {
901-902   sample_offset(spp, s, x, y, &du, &dv);
903-904   uu, vv
913-916   lr1, lr2 = render_rand01(x, y, s*2+2 / s*2+3)   // DOF channels
917       Ray ray = camera_ray_dof(cam, uu, vv, lr1, lr2);
923-924   unsigned seed_key = render_hash3(x, y, s);
925-926   acc = vec3_add(acc, trace(scene, ray, 0, max_depth, time, seed_key));  // <-- CALL SITE
```
**Exact call site: `src/render.c:926`** (`trace(...)`); the parallel adaptive
call site is **`src/render.c:1179-1180`**. A path-trace kernel would be
dispatched at these two points (or via a function pointer / mode flag threaded
through `RenderShared` + `AdaptiveImage`).

- `trace()` forward decl: render.c:465-466; definition render.c:818-831
  (miss -> `sky_sample(r.dir, &scene->sky)` at 827).
- `trace_hit()`: render.c:582-... (`sky_sample` on NULL material at 591;
  emissive direct at 505-569; gloss/sun/refraction inside).
- `sample_offset`: render.c:842-857. `to_byte` (gamma 1/2.2 + quantize):
  render.c:860-875.

### Threading summary
- Non-threaded: `render_serial` (render.c:944-952) -> `render_region` over whole image.
- Threaded: `render_worker` (render.c:992-1032) claims tiles from
  `atomic_fetch_add(&sh->next_tile,1)` (998), writes disjoint tiles, counts
  progress at 1026-1029.
- Adaptive threading: `adaptive_worker` (render.c:1216-...),
  `AdaptivePassShared` (1210-1214), barriers between passes.

---

## 2. PRNG

| Function | Location | Signature | Visibility |
|---|---|---|---|
| `render_hash_u32` | render.c:369-377 | `static unsigned render_hash_u32(unsigned x)` | file-static |
| `render_hash3` | render.c:380-387 | `static unsigned render_hash3(unsigned a, unsigned b, unsigned c)` | file-static |
| `render_rand01` | render.c:390-394 | `static double render_rand01(unsigned a, unsigned b, unsigned c)` | file-static |

- **Not declared in any header** — all three are `static` in `render.c`.
- **Minimal change to let a NEW module call them:** either (a) add declarations
  to `src/render.h` and drop `static` in render.c (touches render.c + render.h),
  or (b) **preferred, zero-touch**: duplicate the tiny, frozen hash (≈25 lines)
  inside the new `src/pathtrace.c` as `static` helpers. Because the algorithm is
  a pure integer avalanche, a verbatim copy is bit-identical and requires **no
  edit to render.c at all** (protects byte-identity trivially). If a shared
  symbol is wanted, exposing via render.h is the only header-level change.

### Seed derivation pattern (confirmed thread/tile-independent)
- Per-sample AA jitter: `render_rand01(px, py, s*2+0 / s*2+1)` (render.c:849-855).
- Per-sample DOF: `render_rand01(px, py, s*2+2 / s*2+3)` (render.c:913-916).
- Per-primary-ray key: `render_hash3((unsigned)x, (unsigned)y, (unsigned)s)` (render.c:923-924).
- Per-hit secondary draws use disjoint channel constants:
  sun-disk `0x5a17u`/`0x7c3du` (render.c:637-638), glossy `GLOSSY_CHANNEL_A/B`
  = `0x9e37u`/`0x85ebu` (render.c:433-434, used 709-710), emissive
  `0xE311u`/`0x4c11u`/`0x9d27u` (render.c:456-458, used 539-542).
- **Every draw is a pure function of `(pixel x, pixel y, sample index s)` plus a
  fixed constant — never of thread, tile or schedule** (documented at
  render.c:877-885, 1073-1094). A new mode MUST follow the same discipline: derive
  path-bounce randomness from a per-sample key (e.g.
  `render_hash3(seed_key, bounce, channel)`) with **fresh channel constants
  disjoint** from the existing ones, so no existing stream is perturbed.

---

## 3. Threading & progress

- `RenderProgress` (render.h:119-123): `total`, `total_expected`, `pixels_total`.
- `render_progress_begin` render.c:251-..., `render_progress_update`
  render.c:280-..., `render_progress_finish` render.c:346-...
- Fixed path: work unit = **row** (serial, render.c:934-935) or **tile**
  (threaded, render.c:1026-1029); `total_expected = tiles_x*tiles_y` (1534) or
  `height` (1575).
- Adaptive path: work unit = **row per pass**; `total_expected = max_passes*height`
  (render.c:1414-1418); rows counted only by the leftmost tile column
  (render.c:1196-1204) so both builds agree.
- `render_progress_update(NULL, done)` is used with a NULL state pointer
  everywhere (single process-wide state), so a new mode can reuse it verbatim.

**What a new mode must do:** call `render_progress_enabled()`; compute a
deterministic **upper-bound** `total_expected` in rows (or tiles); call
`render_progress_begin` / `render_progress_update(NULL, done)` /
`render_progress_finish`; count each row exactly once (leftmost tile column in
the threaded build). Progress is stderr-only and never touches pixels
(render.h:100-108), so determinism is unaffected.

**Determinism requirements:** per-pixel sample loop stays serial with a fixed
summation order; per-sample seed independent of thread/tile; disjoint PRNG
channels.

---

## 4. `src/main.c`

- `typedef struct { ... } Options` — **main.c:47-61**. Fields: `width, height,
  samples, depth, seed, out, scene_path, write_scene_path, want_threads,
  adaptive, adaptive_max, adaptive_tau, no_progress`.
- `usage()` — main.c:63-... (flag text at 80-88; `--no-progress` 86-87; `--help` 89).
- `parse_long` main.c:97-...; `take_value` main.c:115-...; `parse_args` main.c:130-...
- Defaults initialized: main.c:134-146 (`opt->adaptive=0` at 143, etc.).
- **Value-less flag pattern (`--adaptive`):** main.c:191-199 —
  rejects a value with `eq != NULL` ("takes no value"), else sets the int and
  `continue`. (`--threads` 178-187, `--no-progress` 202-210.)
- **Valued-flag pattern (`--samples`, `--depth`):** recognized in the
  `if (strcmp(name,"--width")||...)` list at main.c:218-225; value taken at
  226-232; integer validation at 284-332 (`--samples` 304-311, `--depth`
  312-320). `--adaptive-tau` uses a special `strtod` branch at 266-280.
- **Render call site: main.c:533-549**, `RenderParams rp;` filled 538-543,
  `rc = render_image_ex(&scene, &cam, opt.width, opt.height, &rp, rgb);` at
  **main.c:548**.
- Summary block: main.c:586-594 (`if (opt.adaptive) {...}`) — a `--pathtrace`
  summary line would slot alongside.

A `--pathtrace` pass-through is: add `int pathtrace;` to `Options` (main.c:47-61),
init to 0 (main.c:134-146), a value-less `if (strcmp(name,"--pathtrace")==0)`
block next to `--adaptive` (main.c:191-199), a usage line, set
`rp.pathtrace = opt.pathtrace;` at main.c:538-543, and a summary `if`.

---

## 5. Reusable helpers (all already exist, all pure)

| Helper | Header location | Signature |
|---|---|---|
| `scene_intersect` | scene.h:132 | `int scene_intersect(const Scene *s, Ray r, double tmin, double tmax, Hit *out)` |
| `camera_ray_dof` | camera.h:86 | `Ray camera_ray_dof(const Camera *cam, double u, double v, double r1, double r2)` |
| `sky_sample` | material.h:258 | `Vec3 sky_sample(Vec3 dir, const SkyParams *sky)` |
| `sky_sun_disk_dir` | material.h:282 | `Vec3 sky_sun_disk_dir(Vec3 sun_dir, double radius_deg, double r1, double r2)` |
| `fresnel_schlick` | material.h:159 | `double fresnel_schlick(double cos_theta, double f0)` |
| `material_roughness_to_alpha` | material.h:226 | `double material_roughness_to_alpha(double roughness)` |
| `material_sample_glossy_dir` | material.h:246-247 | `Vec3 material_sample_glossy_dir(const Material *m, Vec3 N, Vec3 incident, double r1, double r2)` |
| `material_shade_pbr` | material.h:205-206 | `Vec3 material_shade_pbr(const Material *m, Vec3 N, Vec3 L, Vec3 V, Vec3 light_color)` |
| `light_sphere_sample_dir` | material.h:315-316 | `Vec3 light_sphere_sample_dir(Vec3 w, double cos_alpha_max, double u1, double u2)` |

Additional useful: `fresnel_schlick_rgb` (material.h:162), `material_shade_local`
(material.h:171), `material_ambient` (material.h:250), `vec3_normalize`
(vec3.h:140), `vec3_reflect` (vec3.h:103), `vec3_add/scale/dot/mul/neg`.

### `Scene` fields
- `SkyParams sky` — scene.h:65. `SkyParams` fields: `sun_dir` (material.h:114),
  `sun_color` (material.h:115), `sun_radius` (material.h:136, degrees; 0 = point
  sun). Plus horizon/zenith/glow/cloud params (material.h:116-127).
- `EmissiveLight emissive_lights[SCENE_MAX_EMISSIVE_LIGHTS]` — scene.h:77;
  `int emissive_light_count` — scene.h:78 (`0` => feature disabled).
  `EmissiveLight` = `{ int prim_index; Vec3 center; double radius; Vec3 emissive; }`
  (scene.h:45-50); cap `SCENE_MAX_EMISSIVE_LIGHTS = 8` (scene.h:58).
- `Hit` = `{ double t; Vec3 point, normal; int material_index, prim_index, front_face; }`
  (geometry.h:55-62). `scene_material()` (scene.h:135) resolves a material index.

Note: emissive lights are **sphere area lights only**; `scene_intersect` is the
only visibility/occlusion query needed. `sky_sample` already includes the sun
glow/disk, so a path tracer can treat a miss as `sky_sample(r.dir, &scene->sky)`
(see render.c:827) — but note this bakes the sun disk into the background, which
interacts with explicit sun NEE (see gotchas).

---

## 6. `Makefile`

- `SRCS = src/vec3.c src/camera.c src/bmp.c src/noise.c src/geometry.c src/bvh.c
  src/material.c src/texture.c src/scene.c src/scene_desc.c src/scene_desc_write.c
  src/render.c` — **Makefile:18**.
- `OBJS = $(SRCS:.c=.o) src/main.o` — Makefile:19. Register a new file by
  appending `src/pathtrace.c` to `SRCS` (Makefile:18) — it is then compiled,
  linked into the binary, and (via `LIB_OBJS`, Makefile:23) linked into every
  test.
- Compile rule `src/%.o: src/%.c` (Makefile:34-35) with `-MMD -MP` auto deps
  (Makefile:38, `-include $(DEPS)`).
- **Tests are auto-discovered**: `TEST_SRCS := $(wildcard tests/*.c)` —
  **Makefile:25**; binaries `bin/%` (Makefile:26, rule 38-40); `make test`
  (Makefile:51-59) runs them all. A new `tests/test_pathtrace.c` is picked up
  automatically with no Makefile edit.
- Threaded build: `make threads` (Makefile:42-46) adds `-DUSE_PTHREADS -pthread`.

---

## 7. Adaptive precedent (closest model for byte-identity)

How `--adaptive` kept `adaptive == 0` byte-identical:
1. **Single new entry point** `render_image_ex` with a `RenderParams` superset
   (render.h:33-55), while `render_image` is untouched.
2. **Hard gate at the top of `render_image_ex`** (render.c:1611-1614):
   `if (!params->adaptive) return render_image(...)` — the default path is
   literally the old function, so no arithmetic changes.
3. New code lives in a **separate engine** (`render_image_adaptive`,
   render.c:1350-1464) with its own `AdaptiveImage` struct (render.c:1096-1116),
   own per-sample offset (`adaptive_sample_offset`, render.c:1124-1133) and own
   worker (`adaptive_worker`, render.c:1216).
4. Disjoint PRNG channel constants so existing streams are untouched
   (render.c:427-434, 448-458).
5. Byte-identity asserted by tests: `tests/test_adaptive.c` (e.g.
   `test_fixed_byte_identity`, test_adaptive.c:254-...; `make_params` at 241-248)
   and `tests/test_integration_adaptive.c` (`make_params` at 258-266).
   `tests/test_integration_glossy.c:472-495` also verifies
   `render_image_ex(adaptive=0)` == `render_image`.

**Gotcha:** existing tests use uninitialized `RenderParams` locals
(e.g. `tests/test_integration_glossy.c:478`, `test_adaptive.c:269`) and assign
only the 5 current fields. Adding a 6th field (`pathtrace`) to `RenderParams`
means those tests read an indeterminate value. **Do NOT rely on a bare new
struct field**; instead pass the mode out-of-band (see strategy) or update the
tests' `make_params` helpers — but the latter is a diff on tests, and the former
keeps byte-identity safest.

---

## 8. Recommended minimal-diff integration strategy

### Which files change
| File | Change | Serialized? |
|---|---|---|
| `src/pathtrace.c` (**NEW**) | path-trace kernel, own PRNG copy, own tiled/adaptive-free engine | — |
| `src/pathtrace.h` (**NEW**) | `int pathtrace_render(const Scene*, const Camera*, int w, int h, int spp, int max_depth, unsigned char *out);` | — |
| `Makefile:18` | append `src/pathtrace.c` to `SRCS` | yes (one line) |
| `src/render.c` | minimal dispatch hook (see below) | **strictly serialized** |
| `src/main.c` | `--pathtrace` flag + pass-through + summary | isolated |
| `tests/test_pathtrace.c` (**NEW**) | auto-discovered; assert `--pathtrace` off == byte-identical | — |

### Two dispatch options
**Option A (zero edit to render.c) — recommended for max safety.**
Do the mode selection entirely in `main.c` at the call site (main.c:548):
```c
if (opt.pathtrace)
    rc = pathtrace_render(&scene, &cam, opt.width, opt.height,
                          opt.samples, opt.depth, rgb);
else
    rc = render_image_ex(&scene, &cam, opt.width, opt.height, &rp, rgb);
```
No `RenderParams` field, no render.c change, no risk to existing tests'
uninitialized structs. `pathtrace.c` reuses `scene_intersect`, `camera_ray_dof`,
`sky_sample`, the material helpers, and its own copy of the hash PRNG. This is
the smallest, safest diff.

**Option B (mirror adaptive exactly).** Add `int pathtrace;` to `RenderParams`,
gate at render.c:1611 (`if (params->pathtrace) return pathtrace_render(...)`),
set it at main.c:541. Requires updating the tests' `make_params` (uninitialized
struct gotcha) — larger blast radius. Only choose if a single `render_image_ex`
call site is mandated.

### Strictly serialized edits (protect byte-identity)
- Any edit inside `render_image`/`render_region`/`trace`/`trace_hit` must be
  **gated behind the new mode** and must not reorder existing float ops or PRNG
  draws. Prefer Option A (no edit at all).
- Keep new PRNG channel constants disjoint from `0x5a17u/0x7c3du`,
  `0x9e37u/0x85ebu`, `0xE311u/0x4c11u/0x9d27u`.
- Do not change `sample_offset`, `to_byte`, or the existing `#ifdef USE_PTHREADS`
  tile math.

### Progress & threading for the new mode
- Reuse `render_progress_begin/update/finish` with a row- or tile-based
  `total_expected`; count each row once (leftmost tile column) as adaptive does
  (render.c:1196-1204).
- If threaded, reuse the `atomic_fetch_add` tile-claim pattern
  (render.c:998-1029) writing disjoint tiles; keep per-pixel sample loop serial
  and seed purely from `(x, y, s, bounce, channel)`.

### Gotchas
1. **Gamma/quantize/tone mapping:** the fixed renderer averages linear samples
   then applies `to_byte` = clamp + `pow(c, 1/2.2)` + round (render.c:860-875).
   A path tracer must accumulate **linear radiance** and reuse the identical
   `to_byte` convention (a private copy, or expose it). Do not double-gamma or
   add a different tonemap, or the two modes will not be comparable and the
   byte-identity test for `pathtrace==0` could be misread.
2. **Sky background on miss:** `trace()` returns `sky_sample(r.dir, &scene->sky)`
   (render.c:827). `sky_sample` already contains the **sun disk/glow**, so a
   naive path tracer that also does explicit sun NEE would **double-count the
   sun**. Either (a) exclude the sun disk from the background when doing NEE, or
   (b) skip sun NEE and rely on the background only. Document the choice; it
   affects variance and brightness but not the `pathtrace==0` path.
3. **Emissive lights are sphere-only** (scene.h:33-50) and are skipped for
   self-intersection by `prim_index` (render.c:519-521); reuse the same
   self-prim guard to avoid light-leak/self-illumination.
4. **`time` is fixed 0.0** (render.c:1499); the water normal uses it
   (render.c:596-598). Keep `time = 0.0` in the new mode for a static frame.
5. **Uninitialized `RenderParams` in existing tests** (see §7) — the decisive
   reason to prefer Option A.
6. **Do not create files under `.marmel/`** (this doc is under `docs/`).
7. `-Wall -Wextra -std=c11` clean is required (Makefile:11); avoid GNU-only
   constructs (the code uses `__atomic_*` builtins already, so those are fine).

---

## Source citations
- `src/render.h` (API, RenderParams render.h:49-55, gating doc 59-62).
- `src/render.c` (render_region 892-938; trace 818-831; trace_hit 582+;
  PRNG 369-394; adaptive engine 1069-1464; entry points 1485-1628).
- `src/main.c` (Options 47-61; usage 63-95; parse_args 130-338; render call 533-549).
- `src/scene.h` (Scene 60-79; EmissiveLight 45-58; scene_intersect 132).
- `src/material.h` (SkyParams 113-137; helpers 159,205,226,246,258,282,315).
- `src/camera.h` (camera_ray 69, camera_ray_dof 86).
- `src/geometry.h` (Hit 55-62).
- `Makefile` (SRCS 18; OBJS 19; LIB_OBJS 23; TEST_SRCS 25; rules 34-40, 42-46, 51-59).
- `tests/test_adaptive.c`, `tests/test_integration_adaptive.c`,
  `tests/test_integration_glossy.c` (byte-identity precedent + `make_params`).
