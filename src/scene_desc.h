#ifndef SCENE_DESC_H
#define SCENE_DESC_H

/*
 * scene_desc.h - In-memory data model for the declarative scene text format
 *                described in docs/scene_format.md (FROZEN v1).
 *
 * This module defines the types that can represent a scene file exactly and
 * losslessly, plus the lifecycle helpers used to build and tear one down.
 * The parser that fills this model from text lives in src/scene_desc.c
 * (scene_desc_load / scene_desc_load_string) and the canonical text writer
 * lives in src/scene_desc_write.c (scene_desc_write); this header only
 * declares their contracts.
 *
 * Dependencies: only the C11 standard library plus the project's EXISTING
 * types (Vec3, PrimKind, Material, SkyParams), which are REUSED, never
 * redefined. No third-party code, no globals, no I/O beyond diagnostic
 * error-string formatting.
 *
 * ------------------------------------------------------------------
 * Ownership (documented contract)
 * ------------------------------------------------------------------
 * A SceneDesc owns every heap allocation reachable from it:
 *   - each MaterialDesc.name,
 *   - each ScenePrimDesc.material_name,
 *   - each ScenePlantDesc.material_bark / .material_leaf,
 *   - the three dynamically-grown arrays themselves (materials, prims,
 *     plants).
 * scene_desc_free() releases all of it and is safe to call on a zeroed or
 * already-freed struct. scene_desc_init() produces that empty state.
 *
 * ------------------------------------------------------------------
 * Note on water (from docs/scene_format.md §4.11)
 * ------------------------------------------------------------------
 * The format introduces a global `water_enabled` flag that has NO counterpart
 * in `src/scene.h`. It is represented here as SceneDesc.water_enabled. It is
 * NOT added to `struct Scene` in this task. Also note (docs/scene_inventory.md
 * §1) that `src/render.c` does NOT read `scene.water_material`; it detects
 * water per-material via the `is_water` flag. So `water_material` /
 * `water_enabled` are bookkeeping metadata for round-trip fidelity; the
 * actual water look comes from `Material.is_water`. render.c is unchanged.
 */

#include <stddef.h>

#include "vec3.h"
#include "geometry.h"   /* PrimKind (PRIM_SPHERE .. PRIM_CYLINDER) */
#include "material.h"   /* Material, SkyParams */
#include "camera.h"     /* Camera framing */

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel stored in every "resolved index" field that has no material.
 * scene_desc_load()'s resolution pass maps a referenced material NAME to a
 * table index; fields left unreferenced (or defaulted) stay at this value. */
#define SCENE_DESC_NO_MATERIAL (-1)

/* ------------------------------------------------------------------ */
/* Camera block (docs/scene_format.md §4.1)                            */
/* ------------------------------------------------------------------ */

/*
 * Raw framing parameters of the optional `camera` block, matching
 * camera_create(eye, target, up, vfov_deg, aspect).
 *
 * `present` records whether the file carried a `camera { }` block at all, so
 * "no block" (use scene defaults) is distinguishable from "block with every
 * key defaulted". `aspect` is deliberately NOT read from the file (§4.1: the
 * aspect is always the CLI width/height, and a file `aspect` key is a reserved
 * unknown key). It is carried here so a host can keep the render aspect next
 * to the description.
 */
typedef struct {
    int    present;   /* 1 if a camera block was supplied, else 0 */
    Vec3   eye;       /* `from` / eye position                    */
    Vec3   target;    /* `at` / look-at point                      */
    Vec3   up;        /* up hint                                   */
    double vfov_deg;  /* vertical field of view, degrees           */
    double aspect;    /* output aspect (width/height); NOT in file */
    double aperture;  /* lens radius for depth of field (0 = pinhole)  */
    double focus_distance; /* eye -> focal plane; 0 = |target - eye|   */
} CameraDesc;

/* ------------------------------------------------------------------ */
/* Named material (docs/scene_format.md §4.3)                          */
/* ------------------------------------------------------------------ */

/*
 * A `material <name> { ... }` block: a name plus the reused Material payload.
 * `name` is heap-owned by the enclosing SceneDesc (freed by
 * scene_desc_free). Material's table index is simply the array position.
 *
 * The procedural-texture keys (`texture`, `texture_scale`, `texture_color_a`,
 * `texture_color_b`, docs/scene_format.md §4.3) are stored directly in
 * `mat.texture_kind` / `mat.texture_scale` / `mat.texture_color_a` /
 * `mat.texture_color_b` (see src/material.h), so scene_build_from_desc's
 * whole-struct copy wires them into the runtime Material with no extra code.
 */
typedef struct {
    char     *name;   /* owned, may be NULL (unset) */
    Material  mat;    /* reused material payload    */
} MaterialDesc;

/* ------------------------------------------------------------------ */
/* Explicit primitive (docs/scene_format.md §4.4 - §4.8)               */
/* ------------------------------------------------------------------ */

/*
 * One explicit primitive block (sphere/plane/box/triangle/cylinder).
 *
 * `kind` reuses PrimKind, so the value set is exactly
 * { PRIM_SPHERE, PRIM_PLANE, PRIM_BOX, PRIM_TRIANGLE, PRIM_CYLINDER }.
 *
 * The geometry fields mirror the grammar keys (and the flat layout of
 * `Primitive`), only the subset relevant to `kind` being meaningful:
 *   SPHERE   : center, radius
 *   PLANE    : point, normal (normalised on load)
 *   BOX      : center, half
 *   TRIANGLE : a, b, c
 *   CYLINDER : base, top, radius (r_bottom), radius2 (r_top)
 *
 * Materials are referenced BY NAME (`material_name`, owned). `material_index`
 * holds the resolved table index, filled in by scene_desc_load()'s resolution
 * pass, or SCENE_DESC_NO_MATERIAL when no material is referenced.
 */
typedef struct {
    PrimKind kind;            /* which shape (reused PrimKind)             */
    Vec3     center;          /* sphere center / box center                */
    Vec3     point;           /* plane point                               */
    Vec3     normal;          /* plane normal                              */
    Vec3     half;            /* box half extents                          */
    Vec3     a, b, c;         /* triangle vertices                         */
    Vec3     base, top;       /* cylinder base / top centers               */
    double   radius;          /* sphere radius / cylinder r_bottom         */
    double   radius2;         /* cylinder r_top                            */
    char    *material_name;   /* owned referenced name, may be NULL        */
    int      material_index;  /* resolved index, or SCENE_DESC_NO_MATERIAL */
} ScenePrimDesc;

/* ------------------------------------------------------------------ */
/* Procedural plant directive (docs/scene_format.md §4.9 - §4.10)      */
/* ------------------------------------------------------------------ */

typedef enum {
    SD_PLANT_TREE,
    SD_PLANT_BUSH
} ScenePlantKind;

/*
 * One `tree { }` or `bush { }` directive. `position.y` is forced to 0
 * (ground) by the grammar; it is stored as read for lossless round-trip.
 *
 * Optional keys use explicit presence flags so "omitted" (derive from the CLI
 * seed / auto-select a leaf variant) is distinguishable from an explicit
 * value. `material_bark` / `material_leaf` are owned names; when NULL the
 * grammar default applies (bark, or an auto leaf variant). The *_index fields
 * hold resolved table indices, or SCENE_DESC_NO_MATERIAL when the
 * corresponding name is absent.
 */
typedef struct {
    ScenePlantKind kind;               /* tree or bush                       */
    Vec3           position;           /* world position (y forced to 0)     */
    double         height;             /* trunk length                       */
    double         radius;             /* trunk radius (selects depth)       */
    int            has_seed;           /* 1 if `seed` key present            */
    unsigned       seed;               /* explicit seed, else derived        */
    char          *material_bark;      /* owned name, or NULL for default    */
    char          *material_leaf;      /* owned name, or NULL for auto       */
    int            has_leaf_variant;   /* 1 if `leaf_variant` key present    */
    int            leaf_variant;       /* [0, 3] when present                */
    int            material_bark_index;/* resolved index, or NO_MATERIAL     */
    int            material_leaf_index;/* resolved index, or NO_MATERIAL     */

    /*
     * Optional generator parameters (docs/scene_format.md §4.9/§4.10).
     *
     * Each mirrors one hardcoded constant of the procedural generator in
     * src/scene.c. The `has_*` flag records whether the file carried the key,
     * so "omitted" (use the legacy constant default) is distinguishable from
     * an explicit value. When a flag is 0 the builder substitutes the
     * corresponding SCENE_* constant, so a scene with none of these keys
     * generates byte-identical geometry to the pre-parameterised generator.
     *
     *   max_depth           <- SCENE_TREE_MAX_DEPTH      (4)
     *   min_branch_radius   <- SCENE_MIN_BRANCH_RADIUS   (0.02)
     *   taper               <- SCENE_TAPER               (0.7)
     *   len_decay           <- SCENE_LEN_DECAY           (0.72)
     *   spread_deg          <- SCENE_SPREAD (deg)        (33)
     *   perturb_deg         <- SCENE_PERTURB (deg)       (7)
     *   up_bias             <- SCENE_UP_BIAS             (0.12)
     *   third_child_chance  <- SCENE_THIRD_CHILD_CHANCE  (0.25)
     *   leaf_min            <- SCENE_LEAF_MIN            (15)
     *   leaf_span           <- SCENE_LEAF_SPAN           (5)
     */
    int            has_max_depth;          /* 1 if `max_depth` key present    */
    int            max_depth;              /* recursion depth limit           */
    int            has_min_branch_radius;  /* 1 if `min_branch_radius` present*/
    double         min_branch_radius;      /* tip-radius stop                 */
    int            has_taper;              /* 1 if `taper` key present        */
    double         taper;                  /* radius_top / radius_bottom      */
    int            has_len_decay;          /* 1 if `len_decay` key present    */
    double         len_decay;              /* child length / parent length    */
    int            has_spread_deg;         /* 1 if `spread_deg` key present   */
    double         spread_deg;             /* base branch angle, degrees      */
    int            has_perturb_deg;        /* 1 if `perturb_deg` present      */
    double         perturb_deg;            /* +- angle/phi jitter, degrees    */
    int            has_up_bias;            /* 1 if `up_bias` key present      */
    double         up_bias;                /* upward pull (anti-droop)        */
    int            has_third_child_chance; /* 1 if `third_child_chance` set   */
    double         third_child_chance;     /* P(fork 3 ways) else 2           */
    int            has_leaf_min;           /* 1 if `leaf_min` key present     */
    int            leaf_min;               /* min leaf spheres per tip        */
    int            has_leaf_span;          /* 1 if `leaf_span` key present    */
    int            leaf_span;              /* extra leaf spheres range        */
} ScenePlantDesc;

/* ------------------------------------------------------------------ */
/* Top-level scene description                                         */
/* ------------------------------------------------------------------ */

/*
 * Complete, lossless in-memory form of one scene file.
 *
 * All three collections use the count + capacity + pointer idiom and are
 * grown by the scene_desc_add_* helpers. They are owned and freed by
 * scene_desc_free().
 */
typedef struct {
    CameraDesc   camera;            /* optional camera block               */
    SkyParams    sky;               /* sky/atmosphere (all SkyParams fields)*/
    int          has_sky;           /* 1 if a sky block was supplied       */

    double       water_level;       /* global water_level (§4.11)          */
    int          water_material;    /* material index, or NO_MATERIAL      */
    int          water_enabled;     /* global water_enabled flag (0/1)     */

    MaterialDesc    *materials;     /* owned dynamic array                 */
    int              material_count;
    int              material_capacity;

    ScenePrimDesc   *prims;         /* owned dynamic array                 */
    int              prim_count;
    int              prim_capacity;

    ScenePlantDesc  *plants;        /* owned dynamic array                 */
    int              plant_count;
    int              plant_capacity;
} SceneDesc;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/*
 * Zero-initialise `d` into the empty description: no blocks, no materials,
 * no primitives, no plants, and water_material = SCENE_DESC_NO_MATERIAL
 * (so 0 stays a valid table index). Defaults from the spec are NOT applied
 * here; a parser task fills them. Safe on NULL.
 */
void scene_desc_init(SceneDesc *d);

/*
 * Load and parse `path` into `d`. Returns 0 on success, non-zero on error.
 * On error a diagnostic of the form
 *     FILE:LINE: error: MESSAGE (near 'TOKEN')
 * is written into `errbuf` (NUL-terminated, truncated to `errlen`).
 */
int scene_desc_load(SceneDesc *d, const char *path, char *errbuf, size_t errlen);

/*
 * Parse a scene description held in memory (`text`, NUL-terminated) instead
 * of on disk. `name` is used verbatim in diagnostics (e.g. "<embedded>").
 * The text is copied internally, so it is never modified. Same return
 * contract as scene_desc_load().
 *
 * Live callers: src/main.c calls this on the no-`--scene` path with the
 * compiled-in DEFAULT_SCENE_TEXT (src/default_scene_text.h) and the name
 * "<embedded>", so the built-in default goes through the very same parser as
 * a scene file and the program works standalone without a scenes/ directory.
 */
int scene_desc_load_string(SceneDesc *d, const char *text, const char *name,
                           char *errbuf, size_t errlen);

/*
 * Release everything owned by `d` and reset it to the empty state (as if
 * scene_desc_init had just run). Safe on NULL, on a zeroed struct, and on an
 * already-freed struct (idempotent).
 */
void scene_desc_free(SceneDesc *d);

/*
 * Emit the canonical text form of `d` to `path`. Returns 0 on success,
 * non-zero on error, writing a diagnostic into `errbuf` like scene_desc_load.
 * The implementation lives in src/scene_desc_write.c; the emitted form is
 * precision-preserving (see docs/scene_format.md §9.4), NOT plain `%.6g`.
 */
int scene_desc_write(const SceneDesc *d, const char *path, char *errbuf, size_t errlen);

/* ------------------------------------------------------------------ */
/* Model construction helpers                                          */
/* ------------------------------------------------------------------ */

/*
 * Append helpers. Each grows the relevant dynamic array and returns the index
 * of the new element (>= 0), or -1 on NULL input or allocation failure.
 *
 * Ownership: names passed in are COPIED (the caller keeps its own string).
 * The new primitive/plant starts with material index fields set to
 * SCENE_DESC_NO_MATERIAL; scene_desc_load()'s resolution pass fills them in.
 */
int scene_desc_add_material(SceneDesc *d, const char *name, const Material *mat);
int scene_desc_add_prim(SceneDesc *d, const ScenePrimDesc *prim);
int scene_desc_add_plant(SceneDesc *d, const ScenePlantDesc *plant);

#ifdef __cplusplus
}
#endif

#endif /* SCENE_DESC_H */
