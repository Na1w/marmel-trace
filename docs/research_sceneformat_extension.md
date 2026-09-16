# Scene-Format Extension — Integration Report

Read-only research. Goal: enable adding NEW keys to the data-driven scene format
(`docs/scene_format.md`, FROZEN v1) without breaking the byte-identity invariant.

All paths are workspace-relative. Line numbers refer to the current tree
(verified with `grep -n`). **No source files were modified.**

---

## 0. TL;DR

| Question | Answer |
|---|---|
| Where is the data model? | `src/scene_desc.h` — `CameraDesc` :75, `MaterialDesc` :93, `ScenePrimDesc` :120, `ScenePlantKind` :138, `ScenePlantDesc` :154, `SceneDesc` :180 |
| How are keys added? | One `KeySpec` row (`{key, KeyType, bit, offsetof}`) + one bit in the block's `enum` (`src/scene_desc.c`) |
| Enum-ish strings? | Special-case in the block applier (pattern: `type = water\|opaque`, `src/scene_desc.c:865`) |
| Does the writer emit all keys? | **Camera/sky/material(else-branch)/globals: YES, unconditionally.** Plants: NO, only presence-flagged. |
| Byte-identity impact? | Adding a key to camera/sky/material always changes `--write-scene` output → must regenerate `scenes/default.scene` + `src/default_scene_text.h` |
| Byte-identity tests? | **None committed.** Verified only by an out-of-tree scratch comparator (`docs/validation_report_scene.md` §3, §7.2). |

---

## 1. Data model — `src/scene_desc.h`

### 1.1 Structs and key→field map

| Struct | Location | Fields (name: type) | File key(s) |
|---|---|---|---|
| `CameraDesc` | `scene_desc.h:75-82` | `present:int` (1 if block present); `eye:Vec3`; `target:Vec3`; `up:Vec3`; `vfov_deg:double`; `aspect:double` | `eye`,`target`,`up`,`vfov`; `aspect` is **NOT** read from file (reserved/unknown) |
| `MaterialDesc` | `scene_desc.h:93-96` | `name:char*` (owned, may be NULL); `mat:Material` | block header name + all `Material` keys |
| `ScenePrimDesc` | `scene_desc.h:120-132` | `kind:PrimKind`; `center,point,normal,half,a,b,c,base,top:Vec3`; `radius:double`; `radius2:double`; `material_name:char*`; `material_index:int` | per-kind keys (see §1.3) |
| `ScenePlantKind` (enum) | `scene_desc.h:138-141` | `SD_PLANT_TREE`, `SD_PLANT_BUSH` | block keyword `tree`/`bush` |
| `ScenePlantDesc` | `scene_desc.h:154-167` | `kind`; `position:Vec3`; `height:double`; `radius:double`; `has_seed:int`; `seed:unsigned`; `material_bark:char*`; `material_leaf:char*`; `has_leaf_variant:int`; `leaf_variant:int`; `material_bark_index:int`; `material_leaf_index:int` | `position`,`height`,`radius`,`seed`,`material_bark`,`material_leaf`,`leaf_variant` |
| `SceneDesc` | `scene_desc.h:180-200` | `camera:CameraDesc`; `sky:SkyParams`; `has_sky:int`; `water_level:double`; `water_material:int`; `water_enabled:int`; `materials*`/`material_count`/`material_capacity`; `prims*`/`prim_count`/`prim_capacity`; `plants*`/`plant_count`/`plant_capacity` | top level |

Sentinel: `SCENE_DESC_NO_MATERIAL (-1)` (`scene_desc.h:58`).

### 1.2 Reused payload structs (`src/material.h`)

`Material` (albedo, specular, shininess, reflectivity, transparency, ior,
is_water, absorption, deep_color) and `SkyParams` (sun_dir, sun_color,
horizon_color, zenith_color, gradient_gamma, sun_glow_exponent,
sun_glow_strength, cloud_height, cloud_scale, cloud_coverage, cloud_softness,
cloud_sharpness, cloud_octaves, seed). These are REUSED, never redefined by
`scene_desc`. **A new key that targets a `Material`/`SkyParams`/`CameraDesc`
field requires a field addition here or in `camera.h`.**

### 1.3 Primitive key→field map (`scene_desc.c`)

`PRIM_SPHERE_KEYS` :553, `PRIM_PLANE_KEYS` :557, `PRIM_BOX_KEYS` :561,
`PRIM_TRIANGLE_KEYS` :565, `PRIM_CYLINDER_KEYS` :570; aggregated in `PRIM_SPECS`
:585-591 with per-kind `required` bitmasks. Each key uses `offsetof` straight
into `ScenePrimDesc`. The `material = <name>` key is handled separately
(`SD_MAT_REF_BIT = 1u<<30`, `sd_apply_prim_key` :934).

### 1.4 Defaults

| Where | What |
|---|---|
| `sd_apply_defaults` (`scene_desc.c:1449`) | Pre-parse defaults: camera (`-18 6 22` / `0 5 -20` / `0 1 0` / vfov 40 / aspect 0), `sky_default_params()`, `has_sky=0`, `water_level=0.02`, `water_material=-1`, `water_enabled=0` |
| `sd_mat_begin` (`scene_desc.c:525`) | Material starts as `sd_opaque_preset()` (all-zero, ior 1) |
| `sd_prim_begin` (`scene_desc.c:536`) | `memset` zero |
| `sd_plant_begin` (`scene_desc.c:635`) | `memset` zero; **plant `radius` default (0.20 tree / 0.05 bush) is applied by the builder, not the parser** |
| `sky_default_params` (`material.c:51`) | The actual sky defaults |

---

## 2. Parser — `src/scene_desc.c`

### 2.1 Pipeline

`scene_desc_load` :1689 / `scene_desc_load_string` :1733 → `sd_run_parser` :1635
(`sd_apply_defaults` → `sd_parse_lines` :1469 → resolution passes
`sd_resolve_prims` :1359, `sd_resolve_plants` :1382, `sd_resolve_water` :1420).
`sd_parse_lines` is one loop over physical lines; each line is tokenized by
`sd_lex_line` :315 and dispatched by `sd_handle_line` :1328 → `sd_handle_top`
:1269 / `sd_handle_body` :1286. Blocks are one level deep (no nesting;
`{` in a body is a hard error).

### 2.2 Generic key application

`KeySpec` (`scene_desc.c:429-434`): `{ const char *key; KeyType type; unsigned bit; size_t offset; }`.
`KeyType` (`scene_desc.c:417-423`): `KT_DOUBLE, KT_VEC3, KT_INT, KT_UINT, KT_BOOL`.

`sd_apply_key` :799 iterates the block's table:
- **Duplicate key** (bit already in `Block.seen`) → hard error `"duplicate key"`.
- **Unknown key** → `sd_warn_unknown_key` :759 → `stderr` warning
  `FILE:LINE: warning: unknown key, using default (near 'KEY')` (non-fatal, §6).
- Match → `sd_apply_value` :765 dispatches on `type` and rejects trailing tokens
  (`"unexpected token after value"`).

Value readers: `sd_val_double` :648, `sd_val_vec3` :662 (**requires exactly 3
numbers**), `sd_val_int` :691, `sd_val_uint` :710, `sd_val_bool` :726 (only
`0`/`1`), `sd_val_name` :741 (ident or quoted string).

### 2.3 Required vs optional

Required keys are enforced at block close via the `required` bitmask:
`sd_first_missing` :922 + `sd_close_prim` :990 (`material` ref also required);
`sd_close_plant` :1101 requires `position` and `height`. Everything else is
optional. There is no per-key range validation (documented out of scope).

### 2.4 Errors / warnings

- Hard error form: `FILE:LINE: error: MSG (near 'TOKEN')` — `sd_err_at` :163.
- Unknown key: warning only (`sd_warn_unknown_key` :759).
- Errors latch `Parser.failed`; on failure `scene_desc_free(d)` leaves `*d` safe.

### 2.5 Exact patterns to ADD a new key

**(a) Scalar (e.g. `sun_radius`, double) — add to `SKY_KEYS`:**
1. `src/material.h` — add `double sun_radius;` to `SkyParams`.
2. `src/scene_desc.c` — add a bit to the `SKY_*` enum (:400-414), e.g.
   `SKY_SUN_RADIUS = 1u << 14`.
3. `src/scene_desc.c` — add row to `SKY_KEYS` (:443):
   `{ "sun_radius", KT_DOUBLE, SKY_SUN_RADIUS, offsetof(SkyParams, sun_radius) }`.
4. `src/material.c:51` (`sky_default_params`) — set the default (`0.0`).
That is the entire parser change; `sd_apply_key`/`sd_apply_value` handle the rest.

**(b) Vec3 (e.g. `texture_color_a`) — identical, with `KT_VEC3`:**
`{ "texture_color_a", KT_VEC3, MAT_TEX_COLOR_A, offsetof(Material, texture_color_a) }`.

**(c) Enum-ish string (e.g. `texture = none|checker|stripes`) — copy the `type`
pattern.** `type = water|opaque` is NOT a `KeySpec`; it is special-cased in
`sd_apply_material_key` (`scene_desc.c:865-890`): check `strcmp(key,"type")`,
guard the bit, call `sd_val_name`, `strcmp` the name to an int, store in a
parser-side field (`p->mat_type`), and resolve at close
(`sd_close_material` :894). For `texture` the same shape applies, but store
directly into `Material.texture_kind` (int) or a parser-side flag. Note
`water_material = none` (`sd_handle_global` :1172) and `material = <name>` are
other name-valued special cases worth mirroring.

**Material presets caveat:** if a new material field must survive
`type = water` / `type = opaque`, it must also be added to `sd_overlay_explicit`
(:851) — otherwise the preset zeroes it.

---

## 3. Canonical writer — `src/scene_desc_write.c`

`scene_desc_write` :442 emits, in §9 order: `emit_globals` :299,
`emit_camera` :312, `emit_sky` :325, `emit_material` :350 (loop),
`emit_prim` :376 (loop), `emit_plant` :418 (loop).

**Answer: the writer emits keys UNCONDITIONALLY for camera, sky, material
(non-preset) and globals; only plants are conditional.**

`emit_camera` (:312) — all four keys always when `camera.present`:
```c
w_key_vec3(w, "eye", d->camera.eye);
w_key_vec3(w, "target", d->camera.target);
w_key_vec3(w, "up", d->camera.up);
w_key_double(w, "vfov", d->camera.vfov_deg);
```
`emit_sky` (:325) — all 14 keys always when `has_sky` (unconditional list,
no default checks).

`emit_material` (:350) — **preset-detected**:
```c
if (mat_is_water_preset(m)) {
    w_key_name(w, "type", "water");
} else if (mat_is_opaque_preset(m)) {
    w_key_name(w, "type", "opaque");
} else {
    w_key_vec3(w, "albedo", m->albedo);
    ... /* all 9 explicit keys, unconditional */
}
```
`mat_is_opaque_preset` :276 / `mat_is_water_preset` :285 compare against exact
literals; a new non-zero material field will make these predicates fail unless
updated, silently turning preset blocks into full 9-key blocks.

`emit_plant` (:418) — **conditional** (the only place presence flags gate output):
```c
w_key_vec3(w, "position", p->position);
w_key_double(w, "height", p->height);
w_key_double(w, "radius", p->radius);
if (p->has_seed)            w_key_uint(w, "seed", p->seed);
if (p->material_bark != NULL) w_key_name(w, "material_bark", p->material_bark);
if (p->material_leaf != NULL) w_key_name(w, "material_leaf", p->material_leaf);
if (p->has_leaf_variant)    w_key_int(w, "leaf_variant", p->leaf_variant);
```

**Consequence for byte-identity:** any new key added to camera/sky/material is
emitted *always* → the `--write-scene` output changes → `scenes/default.scene`
and `src/default_scene_text.h` must be regenerated. New plant keys can be
gated behind presence flags to keep default output unchanged. Emit primitives
available: `w_key_double` :175, `w_key_int` :191, `w_key_uint` :205,
`w_key_vec3` :219, `w_key_name` :231. Number formatting is
precision-preserving (`w_fmt_double` :88; `%.6g` then shortest `%.*g`).

---

## 4. Builder — `src/scene.c`

### 4.1 `scene_build_from_desc` :464

- **Sky:** `s->sky = d->sky;` (whole-struct copy — new `SkyParams` fields flow through free).
- **Materials:** allocates `d->material_count` and copies **whole structs**:
  `mats[i] = d->materials[i].mat;` — a new `Material` field is copied with no
  code change. `s->water_material = d->water_material;`
- **Primitives:** `scene_prim_from_desc` :398 → `prim_sphere/plane/box/triangle/cylinder`, in file order.
- **Plants:** per `ScenePlantDesc` → `scene_resolve_plant_mats` :442 (bark/leaf
  precedence) → `scene_add_plant_desc` :422 → `grow_tree` :227.
- **BVH:** `bvh_build` last.
- **Camera is NOT built here** — `main.c` builds it via `camera_create` from the
  desc fields. `scene_default_camera` :374 and `scene_default_view` :366 are the
  hardcoded fallbacks (aspect `16/9`).

### 4.2 `scene_default_desc` :549

Builds the built-in default: camera (`-18 6 22`/`0 5 -20`/`0 1 0`/40/16:9),
`sky_default_params()` + `seed=1337`, water globals, materials from
`scene_fill_materials` :125 (`MAT_GROUND..MAT_WATER`, enum :73-81), the ground
plane + pond box prims, then trees (`scene_trees` :324) and bushes
(`scene_bushes` :336) with explicit seeds. Result: **121 tapered cylinders +
1163 spheres for seed 1337** (`scene.h` doc comment).

### 4.3 Procedural tree/bush generator — ALL hardcoded constants

`grow_tree` :227 (recursion) and `add_leaf_cluster` :181. Constants in
`src/scene.c:51-65`:

| Constant | Line | Value | Role |
|---|---|---|---|
| `SCENE_TREE_MAX_DEPTH` | 51 | 4 | recursion depth limit |
| `SCENE_MIN_BRANCH_RADIUS` | 52 | 0.02 | tip-radius stop |
| `SCENE_TAPER` | 53 | 0.7 | `r_top = 0.7 * r_bottom` |
| `SCENE_LEN_DECAY` | 54 | 0.72 | child length ratio |
| `SCENE_SPREAD` | 55 | 33° | base branch angle |
| `SCENE_PERTURB` | 56 | 7° | ±jitter on angle/phi |
| `SCENE_UP_BIAS` | 57 | 0.12 | upward pull (anti-droop) |
| `SCENE_THIRD_CHILD_CHANCE` | 58 | 0.25 | P(fork 3 ways) else 2 |
| `SCENE_LEAF_MIN` | 61 | 15 | min leaf spheres per tip |
| `SCENE_LEAF_SPAN` | 62 | 5 | → 15..19 spheres per tip |
| `SCENE_MAX_SEGMENTS` | 65 | 4096 | hard safety cap |

Additional inline magic numbers (not `#define`s):

| Value | Line | Role |
|---|---|---|
| `cluster_r = branch_len * 1.1 + 0.7` | 187 | leaf-cluster radius |
| `clamp(cluster_r, 0.7, 2.2)` | 193 | cluster radius clamp |
| `leaf_scale = clamp(cluster_r / 1.6, 0.4, 1.0)` | 194 | leaf size scaling |
| `off = (ox*r*1.15, oy*r*0.85, oz*r*1.15)` | 201-203 | cluster ellipsoid |
| `r = (0.5 + rand*0.9) * leaf_scale` | 204 | per-leaf radius |
| `len_jit = 1.0 + rand(-0.12, 0.12)` | 270 | per-child length jitter |
| `child_seed = scene_hash(seed, d, k+1)` | 276 | child seed derivation |
| `children = rand < CHANCE ? 3 : 2` | 260 | branch count |
| `axis` degenerate eps `1e-12` | 241 | guard |

**Already file-controllable via scene keys:** `position`, `height`, `radius`
(which also selects depth), `seed`, `material_bark`, `material_leaf`,
`leaf_variant`. **Everything in the two tables above is hardcoded** and would
need to become per-plant (or global) parameters to be exposed.

---

## 5. Byte-identity invariant

### 5.1 How the three artefacts relate

```
scenes/default.scene  ──(byte-for-byte)──►  src/default_scene_text.h : DEFAULT_SCENE_TEXT
        │                                              │
        │                                    main.c:369  scene_desc_load_string()
        │                                              ▼
        └─ main.c --scene ──────────►  SceneDesc ──► scene_build_from_desc() ──► Scene
                                                       ▲
main.c:341 --write-scene ── scene_default_desc() ──────┘  → scene_desc_write()
```

- `src/default_scene_text.h:22` holds `DEFAULT_SCENE_TEXT`, "byte-identical to
  the contents of `scenes/default.scene` (ASCII)" (auto-generated, do not hand-edit).
- `main.c` no-`--scene` path parses `DEFAULT_SCENE_TEXT` via
  `scene_desc_load_string` (`main.c:369`).
- `main.c:341` `--write-scene PATH` calls `scene_default_desc(&desc)` then
  `scene_desc_write(&desc, PATH, ...)` and exits (`main.c:339-349`).
- `scene_default_desc()` is the source of truth for the built-in description;
  `scenes/default.scene` is claimed to be exactly its `--write-scene` output.

### 5.2 Which tests assert byte-identity / `--write-scene` output

**None in the committed suite.** Verified:

- `tests/test_integration.c` (`main`, ~:180) calls `scene_default_desc` +
  `scene_build_from_desc` and asserts render content/determinism, **not**
  text/byte identity of the description.
- `tests/test_render_threads.c:87` calls `scene_default_desc`; asserts
  byte-identical *renders* across thread counts (`CHECK` at :122, :141).
- No test references `scene_desc_write`, `scene_desc_load_string`,
  `DEFAULT_SCENE_TEXT`, or `scenes/default.scene`.
- `tests/run_integration.sh` only validates the BMP file size (`57654`) and magic.
- `grep -rn "scene_desc_write\|DEFAULT_SCENE_TEXT\|scenes/default" tests/` → no matches.

The byte-identity gate was proven **out-of-tree** by an independent scratch
comparator (`docs/validation_report_scene.md` §3 and §7.2): embedded default vs
`scenes/default.scene` vs hardcoded build, 25,890 field checks, 0 mismatches;
`write(load(write(x))) == write(x)` (3379 bytes). `DEFAULT_SCENE_TEXT` and the
file are both 3862 bytes. This is a **manual/audit** guarantee, not a
regression test — so a new always-emitted key silently invalidates it unless the
artefacts are regenerated.

### 5.3 Practical implication

Adding any new camera/sky/material key (all emitted unconditionally) or changing
`sky_default_params`/`scene_default_desc` values will make
`--write-scene` output diverge from `scenes/default.scene` and
`DEFAULT_SCENE_TEXT`. To preserve the invariant you must: regenerate both
artefacts, and (recommended) add a committed test asserting
`DEFAULT_SCENE_TEXT` bytes == `scenes/default.scene` bytes and
`write(scene_default_desc())` body == those bytes.

---

## 6. Concrete proposal for the requested keys

### 6.1 Material procedural texture

| Key | Type | Default | Notes |
|---|---|---|---|
| `texture` | name | `none` | `none\|checker\|stripes`; enum-ish → §2.5(c) pattern |
| `texture_scale` | f64 | `1.0` | world units per cell |
| `texture_color_a` | vec3 | `1 1 1` | first cell |
| `texture_color_b` | vec3 | `0 0 0` | second cell |

Files:
1. `src/material.h` — add to `Material`: `int texture_kind;` (0 none, 1 checker,
   2 stripes), `double texture_scale;`, `Vec3 texture_color_a, texture_color_b;`
   (zero-init keeps existing scenes untextured).
2. `src/scene_desc.c` — add `MAT_TEXTURE`, `MAT_TEX_SCALE`,
   `MAT_TEX_COLOR_A`, `MAT_TEX_COLOR_B` bits (:466-475) + `MAT_KEYS` rows (:477);
   add `texture` to the `sd_apply_material_key` special-case (:865) and to
   `sd_overlay_explicit` (:851) so presets zero it.
3. `src/scene_desc_write.c` — emit `texture*` in `emit_material` else-branch
   (:350); update `mat_is_opaque_preset` :276 / `mat_is_water_preset` :285 if
   the fields are non-zero for presets.
4. `src/render.c` — hook albedo modulation in `trace_hit` (~:110-121) per the
   existing `docs/research_texture_pipeline.md`; new `src/texture.{c,h}`; add to
   `Makefile` `SRCS`.
5. Regenerate `scenes/default.scene` + `src/default_scene_text.h`.
Default-material output unchanged (presets emit `type = ...` only).

### 6.2 Sky soft shadows

| Key | Type | Default | Notes |
|---|---|---|---|
| `sun_radius` | f64 | `0` | angular radius in degrees; `0` = hard shadow |

Files:
1. `src/material.h` — add `double sun_radius;` to `SkyParams`.
2. `src/material.c:51` — `p->sun_radius = 0.0;`.
3. `src/scene_desc.c` — `SKY_SUN_RADIUS` bit + `SKY_KEYS` row (offsetof
   `SkyParams`, `KT_DOUBLE`).
4. `src/render.c` — replace the single hard shadow ray (`trace_hit` :120-128)
   with cone sampling around `sun_dir` when `sun_radius > 0` (deterministic,
   hash-seeded like the existing PRNG).
5. `src/scene_desc_write.c` — `emit_sky` :325 add `w_key_double("sun_radius", ...)`.
6. Regenerate `scenes/default.scene` + `default_scene_text.h` (always-emitted).

### 6.3 Camera DOF

| Key | Type | Default | Notes |
|---|---|---|---|
| `aperture` | f64 | `0` | lens radius; `0` = pinhole |
| `focus_distance` | f64 | `0` (→ use `\|target-eye\|`) | focal plane distance |

Files:
1. `src/scene_desc.h` — add `double aperture; double focus_distance;` to
   `CameraDesc` (:75).
2. `src/scene_desc.c` — `CAM_APERTURE`/`CAM_FOCUS` bits + `CAM_KEYS` rows (:436);
   defaults in `sd_apply_defaults` (:1449).
3. `src/camera.h`/`src/camera.c` — add `aperture`/`focus_distance` to `Camera`
   and either extend `camera_create` or set post-create; `camera_ray` :61 jitters
   the origin on a lens disk (deterministic seed from pixel/sample).
4. `src/render.c` — pass the per-sample seed into `camera_ray` (`render_region`
   :290-305 already has `px,py,s`).
5. `src/scene_desc_write.c` — `emit_camera` :312 add both keys.
6. Regenerate `scenes/default.scene` + `default_scene_text.h` (always-emitted).

### 6.4 Tree/bush generator params

Expose as optional, presence-flagged plant keys (defaults = current constants, so
existing files and default output are unchanged). Add `has_*` flags + fields to
`ScenePlantDesc` (`scene_desc.h:154`), rows to `PLANT_KEYS` (`scene_desc.c:626`)
with far bits, and thread a params struct through `grow_tree` :227 /
`add_leaf_cluster` :181 (currently they read the `#define`s directly).

| Key | Type | Default (= constant) |
|---|---|---|
| `max_depth` | int | 4 (`SCENE_TREE_MAX_DEPTH`) |
| `min_branch_radius` | f64 | 0.02 (`SCENE_MIN_BRANCH_RADIUS`) |
| `taper` | f64 | 0.7 (`SCENE_TAPER`) |
| `len_decay` | f64 | 0.72 (`SCENE_LEN_DECAY`) |
| `spread_deg` | f64 | 33 (`SCENE_SPREAD`) |
| `perturb_deg` | f64 | 7 (`SCENE_PERTURB`) |
| `up_bias` | f64 | 0.12 (`SCENE_UP_BIAS`) |
| `third_child_chance` | f64 | 0.25 (`SCENE_THIRD_CHILD_CHANCE`) |
| `leaf_min` | int | 15 (`SCENE_LEAF_MIN`) |
| `leaf_span` | int | 5 (`SCENE_LEAF_SPAN`) |

Files: `src/scene_desc.h` (fields/flags), `src/scene_desc.c` (bits + `PLANT_KEYS`
+ presence flags in `sd_close_plant` :1101), `src/scene.c` (params struct +
`grow_tree`/`add_leaf_cluster`), `src/scene_desc_write.c` (`emit_plant` :418,
emit only when the flag is set → **default output unchanged**). Regeneration of
the default artefacts is NOT required if all new keys are presence-gated.

---

## 7. Change-surface summary

| New key type | scene_desc.h | scene_desc.c | scene_desc_write.c | scene.c / other | Regen default? |
|---|---|---|---|---|---|
| material scalar/vec3 | — (Material in material.h) | bits + `MAT_KEYS` + `sd_overlay_explicit` | `emit_material` else-branch | render.c hook | No (presets unchanged) |
| material enum string | — | special-case in `sd_apply_material_key` | as above | — | No |
| sky scalar | — (SkyParams in material.h) | `SKY_*` bit + `SKY_KEYS` | `emit_sky` (always) | material.c default | **Yes** |
| camera scalar | `CameraDesc` field | `CAM_*` bit + `CAM_KEYS` + defaults | `emit_camera` (always) | camera.{c,h}, render.c | **Yes** |
| plant scalar | `ScenePlantDesc` field + `has_*` | bit + `PLANT_KEYS` + close flags | `emit_plant` (gated) | scene.c generator | No |
