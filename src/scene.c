/*
 * scene.c - Procedural outdoor scene assembly.
 *
 * scene_build_from_desc() is the SINGLE scene-construction path. From a
 * declarative SceneDesc (docs/scene_format.md) it deterministically builds:
 *   1. an infinite grass plane at y = 0,
 *   2. a bounded pond modelled as a very flat axis-aligned box whose top face
 *      sits exactly at water_level (2 cm above the ground, see below),
 *   3. several procedurally grown trees and a few bushes built from
 *      tapered-cylinder branch segments and leaf-cluster spheres,
 *   4. the sky/sun parameters,
 * and then wraps everything in a BVH for fast nearest-hit queries.
 *
 * Water level note: the pond top face is placed at y = SCENE_WATER_LEVEL
 * (= 0.02), i.e. 2 cm above the grass plane. The ground plane is infinite, so
 * placing the water at exactly y = 0 would z-fight with it; a 2 cm offset keeps
 * the surface visually flush while guaranteeing a unique nearest hit.
 *
 * Determinism: the recursive growth uses a pure integer hash of
 * (seed, depth, branch index) instead of rand(), so a given seed always
 * produces the identical primitive layout regardless of traversal order.
 *
 * No I/O, no mutable global state. C11, -Wall -Wextra clean.
 */

#include "scene.h"
#include "scene_desc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tunable scene constants                                             */
/* ------------------------------------------------------------------ */

#define SCENE_PI 3.14159265358979323846
#define SCENE_DEG (SCENE_PI / 180.0)

/* Water surface height above the ground plane (documented above). */
#define SCENE_WATER_LEVEL 0.02

/* Pond footprint: centered off to one side, top face at water_level. */
#define SCENE_POND_CX     0.0
#define SCENE_POND_CZ    -25.0
#define SCENE_POND_HALF_X 45.0
#define SCENE_POND_HALF_Z 45.0
#define SCENE_POND_HALF_Y 0.03

/* Tree growth parameters (docs/research_trees.md §1.5). */
#define SCENE_TREE_MAX_DEPTH     4
#define SCENE_MIN_BRANCH_RADIUS  0.02
#define SCENE_TAPER              0.7   /* radius_top  = 0.7 * radius_bottom   */
#define SCENE_LEN_DECAY          0.72  /* child length = parent length * 0.72 */
#define SCENE_SPREAD             (33.0 * SCENE_DEG) /* ~33 deg branch angle  */
#define SCENE_PERTURB            (7.0 * SCENE_DEG)  /* +-7 deg jitter        */
#define SCENE_UP_BIAS            0.12  /* upward pull to fight drooping       */
#define SCENE_THIRD_CHILD_CHANCE 0.25  /* probability a node forks 3 ways     */

/* Leaf clusters: 15..(15+SPAN-1) spheres per terminal branch tip. */
#define SCENE_LEAF_MIN   15
#define SCENE_LEAF_SPAN  5    /* -> 15..19 spheres per tip */

/* Hard safety cap so a pathological seed can never explode memory. */
#define SCENE_MAX_SEGMENTS 4096

/*
 * Per-plant generator parameters (docs/scene_format.md §4.9/§4.10).
 *
 * These mirror the SCENE_* constants above, but are resolved per `tree`/`bush`
 * directive from the scene description. A scene that omits every optional key
 * resolves each field to its legacy constant (via scene_tree_params_from_desc),
 * so the generated geometry is byte-identical to the pre-parameterised build.
 */
typedef struct {
    int    max_depth;          /* <- SCENE_TREE_MAX_DEPTH                  */
    double min_branch_radius;  /* <- SCENE_MIN_BRANCH_RADIUS               */
    double taper;              /* <- SCENE_TAPER                           */
    double len_decay;          /* <- SCENE_LEN_DECAY                       */
    double spread;             /* <- SCENE_SPREAD        (radians)         */
    double perturb;            /* <- SCENE_PERTURB       (radians)         */
    double up_bias;            /* <- SCENE_UP_BIAS                         */
    double third_child_chance; /* <- SCENE_THIRD_CHILD_CHANCE              */
    int    leaf_min;           /* <- SCENE_LEAF_MIN                        */
    int    leaf_span;          /* <- SCENE_LEAF_SPAN                       */
} TreeParams;

#define SCENE_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------ */
/* Material table indices                                              */
/* ------------------------------------------------------------------ */

enum {
    MAT_GROUND = 0,
    MAT_BARK,
    MAT_LEAF0,
    MAT_LEAF1,
    MAT_LEAF2,
    MAT_LEAF3,
    MAT_WATER,
    MAT_COUNT
};

/* ------------------------------------------------------------------ */
/* Deterministic PRNG: pure hash of (seed, depth, index)               */
/* ------------------------------------------------------------------ */

static unsigned scene_hash(unsigned seed, unsigned depth, unsigned idx)
{
    unsigned h = seed * 2654435761u;
    h ^= (depth + 1u) * 0x9E3779B9u;
    h ^= (idx + 1u) * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

/* Uniform in [0, 1). */
static double scene_rand(unsigned seed, unsigned depth, unsigned idx)
{
    return (double)(scene_hash(seed, depth, idx) & 0xFFFFFFu) / 16777216.0;
}

/* Uniform in [lo, hi). */
static double scene_rand_range(unsigned seed, unsigned depth, unsigned idx,
                               double lo, double hi)
{
    return lo + (hi - lo) * scene_rand(seed, depth, idx);
}

static double scene_clamp(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ------------------------------------------------------------------ */
/* Material table                                                      */
/* ------------------------------------------------------------------ */

static void scene_fill_materials(Material *m)
{
    static const Vec3 leaf_albedo[4] = {
        { 0.16, 0.38, 0.12 },
        { 0.22, 0.45, 0.15 },
        { 0.30, 0.50, 0.18 },
        { 0.12, 0.30, 0.10 }
    };
    int i;

    memset(m, 0, sizeof(Material) * (size_t)MAT_COUNT);

    /* Grass ground: green, slightly rough, non-reflective. */
    m[MAT_GROUND].albedo       = vec3(0.30, 0.42, 0.16);
    m[MAT_GROUND].specular     = vec3(0.05, 0.05, 0.05);
    m[MAT_GROUND].shininess    = 8.0;
    m[MAT_GROUND].reflectivity = 0.0;
    m[MAT_GROUND].ior          = 1.0;

    /* Bark: warm brown, matte. */
    m[MAT_BARK].albedo       = vec3(0.26, 0.18, 0.11);
    m[MAT_BARK].specular     = vec3(0.04, 0.04, 0.04);
    m[MAT_BARK].shininess    = 6.0;
    m[MAT_BARK].reflectivity = 0.0;
    m[MAT_BARK].ior          = 1.0;

    /* Four distinct leaf greens. */
    for (i = 0; i < 4; ++i) {
        m[MAT_LEAF0 + i].albedo       = leaf_albedo[i];
        m[MAT_LEAF0 + i].specular     = vec3(0.03, 0.04, 0.03);
        m[MAT_LEAF0 + i].shininess    = 10.0;
        m[MAT_LEAF0 + i].reflectivity = 0.0;
        m[MAT_LEAF0 + i].ior          = 1.0;
    }

    /* Water: highly specular, reflective, transmissive, absorbing. */
    m[MAT_WATER].albedo       = vec3(0.05, 0.15, 0.20);
    m[MAT_WATER].specular     = vec3(0.90, 0.90, 0.90);
    m[MAT_WATER].shininess    = 256.0;
    m[MAT_WATER].reflectivity = 1.0;
    m[MAT_WATER].transparency = 0.85;
    m[MAT_WATER].ior          = 1.33;
    m[MAT_WATER].is_water     = 1;
    m[MAT_WATER].absorption   = vec3(0.45, 0.12, 0.06);
    m[MAT_WATER].deep_color   = vec3(0.02, 0.10, 0.16);

    /*
     * Procedural textures are off for the built-in scene. Setting the explicit
     * no-op defaults (rather than leaving the memset zeros) means the canonical
     * writer emits no `texture*` keys for these materials, keeping
     * scenes/default.scene byte-identical (the render is unaffected because
     * TEXTURE_NONE returns the albedo verbatim).
     */
    for (i = 0; i < MAT_COUNT; ++i) {
        material_texture_defaults(&m[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Leaf clusters                                                       */
/* ------------------------------------------------------------------ */

/*
 * Scatter a cluster of small leaf spheres within an ellipsoid around a
 * terminal branch tip. The cluster size scales with the remaining branch
 * length so bushes get proportionally smaller foliage.
 */
static int add_leaf_cluster(Geometry *g, int leaf_mat, Vec3 tip,
                            double branch_len, int depth, unsigned seed,
                            const TreeParams *tp)
{
    unsigned d = (unsigned)depth;
    int count = tp->leaf_min
              + (int)(scene_rand(seed, d, 0xBEEFu) * (double)tp->leaf_span);
    double cluster_r = branch_len * 1.1 + 0.7;
    double leaf_scale;
    int i;

    if (count < 1) count = 1;

    cluster_r = scene_clamp(cluster_r, 0.7, 2.2);
    leaf_scale = scene_clamp(cluster_r / 1.6, 0.4, 1.0);

    for (i = 0; i < count; ++i) {
        unsigned k = (unsigned)i;
        double ox = scene_rand_range(seed, d, 100u + k, -1.0, 1.0);
        double oy = scene_rand_range(seed, d, 200u + k, -1.0, 1.0);
        double oz = scene_rand_range(seed, d, 300u + k, -1.0, 1.0);
        Vec3 off = vec3(ox * cluster_r * 1.15,
                        oy * cluster_r * 0.85,
                        oz * cluster_r * 1.15);
        double r = (0.5 + scene_rand(seed, d, 400u + k) * 0.9) * leaf_scale;

        if (g->count >= SCENE_MAX_SEGMENTS) return 0;
        if (geometry_add(g, prim_sphere(vec3_add(tip, off), r, leaf_mat)) < 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Recursive tree growth                                               */
/* ------------------------------------------------------------------ */

/*
 * Grow one branch segment (a tapered cylinder) and recurse into 2-3 children.
 * At the tips (depth limit or branch too thin) a leaf cluster is added.
 *
 * The requested conceptual signature `grow_tree(Geometry*, Material, ...)` is
 * realised here as (bark material index, leaf material index) because the
 * geometry stores materials by table index.
 *
 * Returns 0 on success, non-zero on allocation failure.
 */
static int grow_tree(Geometry *g, int bark_mat, int leaf_mat,
                     Vec3 base, Vec3 dir, double length, double radius,
                     int depth, unsigned seed, const TreeParams *tp)
{
    unsigned d = (unsigned)depth;
    Vec3 axis = vec3_normalize(dir);
    double r_bottom = radius;
    double r_top = radius * tp->taper;
    Vec3 top;
    Vec3 ref, u, v;
    int children, i;

    if (g->count >= SCENE_MAX_SEGMENTS) return 0;

    if (vec3_length_sq(axis) < 1e-12) {
        axis = vec3(0.0, 1.0, 0.0);
    }

    top = vec3_add(base, vec3_scale(axis, length));

    if (geometry_add(g, prim_cylinder(base, top, r_bottom, r_top, bark_mat)) < 0)
        return -1;

    /* Terminal: depth limit reached or branch too thin to be visible. */
    if (depth >= tp->max_depth || r_top < tp->min_branch_radius) {
        return add_leaf_cluster(g, leaf_mat, top, length, depth, seed, tp);
    }

    /* Orthonormal frame around the parent direction. */
    ref = (fabs(axis.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    u = vec3_normalize(vec3_cross(ref, axis));
    v = vec3_cross(axis, u);

    children = (scene_rand(seed, d, 0xABC1u) < tp->third_child_chance) ? 3 : 2;

    /* Radius is continuous across the joint: children start at the parent tip. */
    for (i = 0; i < children; ++i) {
        unsigned k = (unsigned)i;
        double phi = 2.0 * SCENE_PI * (double)i / (double)children
                   + scene_rand_range(seed, d, 100u + k, -tp->perturb, tp->perturb);
        double spread = tp->spread
                   + scene_rand_range(seed, d, 300u + k, -tp->perturb, tp->perturb);
        double len_jit = 1.0
                   + scene_rand_range(seed, d, 500u + k, -0.12, 0.12);
        Vec3 child_dir = vec3_normalize(vec3_add(
            vec3_scale(axis, cos(spread)),
            vec3_scale(vec3_add(vec3_scale(u, cos(phi)),
                                vec3_scale(v, sin(phi))), sin(spread))));
        unsigned child_seed = scene_hash(seed, d, k + 1u);
        int rc;

        /* Upward bias keeps the crown from drooping toward the ground. */
        child_dir = vec3_normalize(vec3_add(child_dir, vec3(0.0, tp->up_bias, 0.0)));

        rc = grow_tree(g, bark_mat, leaf_mat, top, child_dir,
                       length * tp->len_decay * len_jit, r_top,
                       depth + 1, child_seed, tp);
        if (rc != 0) return rc;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Scene lifecycle                                                     */
/* ------------------------------------------------------------------ */

void scene_free(Scene *s)
{
    if (!s) return;

    free(s->materials);
    s->materials = NULL;
    s->material_count = 0;

    bvh_free(s->bvh);
    s->bvh = NULL;

    geometry_free(&s->geo);

    s->water_material = -1;
    s->water_level = 0.0;
}

/* ------------------------------------------------------------------ */
/* Tree / bush placement                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    double x, z;         /* ground position                              */
    double trunk_len;    /* first (trunk) segment length                 */
    double trunk_radius; /* first segment radius; also selects the depth */
} TreeSpec;

/* The initial radius controls how deep the recursion runs: the branch stops
 * when its tip radius drops below SCENE_MIN_BRANCH_RADIUS. Radius ~0.20 grows
 * a full depth-4 tree; ~0.07 stops at depth 3; ~0.05 stops at depth 2. */
static const TreeSpec scene_trees[] = {
    /* one large, full depth-4 tree on the far shore */
    {  50.0,  -80.0, 3.4, 0.20 },
    /* medium, depth-3 trees spread across the far bank */
    {  30.0, -105.0, 2.8, 0.07 },
    { -35.0, -100.0, 2.7, 0.07 },
    {  70.0,  -60.0, 2.6, 0.07 },
    /* small, depth-2 trees */
    {  15.0, -115.0, 2.2, 0.05 },
    { -60.0,  -85.0, 2.2, 0.05 }
};

static const TreeSpec scene_bushes[] = {
    { -48.0, -55.0, 0.8, 0.05 },
    {  55.0, -100.0, 0.7, 0.05 }
};

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

int scene_intersect(const Scene *s, Ray r, double tmin, double tmax, Hit *out)
{
    if (!s || !out) return 0;

    if (s->bvh) {
        return bvh_intersect(s->bvh, &s->geo, r, tmin, tmax, out);
    }
    return geometry_intersect(&s->geo, r, tmin, tmax, out);
}

const Material *scene_material(const Scene *s, int index)
{
    if (!s || !s->materials) return NULL;
    if (index < 0 || index >= s->material_count) return NULL;
    return &s->materials[index];
}

/* ------------------------------------------------------------------ */
/* Default camera framing                                              */
/* ------------------------------------------------------------------ */

void scene_default_view(Vec3 *eye, Vec3 *target, Vec3 *up, double *vfov_deg)
{
    if (eye)      *eye      = vec3(-18.0, 6.0, 22.0);
    if (target)   *target   = vec3(0.0, 5.0, -20.0);
    if (up)       *up       = vec3(0.0, 1.0, 0.0);
    if (vfov_deg) *vfov_deg = 40.0;
}

Camera scene_default_camera(const Scene *s)
{
    Vec3 eye, target, up;
    double vfov_deg;

    (void)s; /* framing is scene-independent for now */

    scene_default_view(&eye, &target, &up, &vfov_deg);
    return camera_create(eye, target, up, vfov_deg, 16.0 / 9.0);
}

/* ================================================================== */
/* Scene construction from a SceneDesc                                 */
/* ================================================================== */

/* Material names used by the built-in default description. */
static const char *const scene_mat_names[MAT_COUNT] = {
    "ground", "bark", "leaf0", "leaf1", "leaf2", "leaf3", "water"
};

/*
 * Translate one explicit ScenePrimDesc into a Primitive via the existing
 * geometry constructors, preserving the exact field layout.
 */
static Primitive scene_prim_from_desc(const ScenePrimDesc *pd, int mat)
{
    switch (pd->kind) {
    case PRIM_SPHERE:
        return prim_sphere(pd->center, pd->radius, mat);
    case PRIM_PLANE:
        return prim_plane(pd->point, pd->normal, mat);
    case PRIM_BOX:
        return prim_box(pd->center, pd->half, mat);
    case PRIM_TRIANGLE:
        return prim_triangle(pd->a, pd->b, pd->c, mat);
    case PRIM_CYLINDER:
        return prim_cylinder(pd->base, pd->top, pd->radius, pd->radius2, mat);
    }
    /* Unreachable for a well-formed PrimKind; default to a degenerate
     * plane so the caller never sees an uninitialised Primitive. */
    return prim_plane(pd->point, pd->normal, mat);
}

/*
 * Resolve the per-plant generator parameters from a plant directive.
 *
 * Every optional key that the file carried (has_* == 1) overrides the legacy
 * SCENE_* constant; omitted keys fall back to that constant, so a scene
 * without any of these keys generates byte-identical geometry to the
 * pre-parameterised generator.
 *
 * Angles are stored in degrees in the desc and converted to radians here.
 */
static void scene_tree_params_from_desc(const ScenePlantDesc *pd, TreeParams *tp)
{
    tp->max_depth          = pd->has_max_depth
                                 ? pd->max_depth : SCENE_TREE_MAX_DEPTH;
    tp->min_branch_radius  = pd->has_min_branch_radius
                                 ? pd->min_branch_radius : SCENE_MIN_BRANCH_RADIUS;
    tp->taper              = pd->has_taper
                                 ? pd->taper : SCENE_TAPER;
    tp->len_decay          = pd->has_len_decay
                                 ? pd->len_decay : SCENE_LEN_DECAY;
    tp->spread             = pd->has_spread_deg
                                 ? pd->spread_deg * SCENE_DEG : SCENE_SPREAD;
    tp->perturb            = pd->has_perturb_deg
                                 ? pd->perturb_deg * SCENE_DEG : SCENE_PERTURB;
    tp->up_bias            = pd->has_up_bias
                                 ? pd->up_bias : SCENE_UP_BIAS;
    tp->third_child_chance = pd->has_third_child_chance
                                 ? pd->third_child_chance : SCENE_THIRD_CHILD_CHANCE;
    tp->leaf_min           = pd->has_leaf_min
                                 ? pd->leaf_min : SCENE_LEAF_MIN;
    tp->leaf_span          = pd->has_leaf_span
                                 ? pd->leaf_span : SCENE_LEAF_SPAN;
}

/*
 * Grow one plant directive. `leaf_variant` selects MAT_LEAF0 + variant%4.
 * The trunk starts at ground level (position.y forced to 0) growing straight
 * up.
 */
static int scene_add_plant_desc(Geometry *g, const ScenePlantDesc *pd,
                                int bark_mat, int leaf_mat, unsigned seed)
{
    Vec3 base = vec3(pd->position.x, 0.0, pd->position.z);
    Vec3 up = vec3(0.0, 1.0, 0.0);
    TreeParams tp;

    scene_tree_params_from_desc(pd, &tp);

    return grow_tree(g, bark_mat, leaf_mat, base, up,
                     pd->height, pd->radius, 0, seed, &tp);
}

/*
 * Resolve the bark / leaf material indices for a plant directive.
 *
 * Precedence:
 *   1. an explicit resolved index (>= 0) recorded on the directive;
 *   2. the built-in defaults (bark; auto leaf variant from the seed).
 * The auto leaf variant is derived as
 * clamp((int)(rand(seed,0,0x51)*4),0,3) when neither an explicit leaf index
 * nor a leaf_variant is present.
 */
static void scene_resolve_plant_mats(const ScenePlantDesc *pd, unsigned seed,
                                     int *bark_out, int *leaf_out)
{
    int bark = (pd->material_bark_index >= 0) ? pd->material_bark_index
                                              : MAT_BARK;
    int leaf;

    if (pd->material_leaf_index >= 0) {
        leaf = pd->material_leaf_index;
    } else if (pd->has_leaf_variant) {
        leaf = MAT_LEAF0 + (pd->leaf_variant % 4);
    } else {
        int variant = (int)(scene_rand(seed, 0u, 0x51u) * 4.0);
        if (variant < 0) variant = 0;
        if (variant > 3) variant = 3;
        leaf = MAT_LEAF0 + (variant % 4);
    }

    *bark_out = bark;
    *leaf_out = leaf;
}

/* ------------------------------------------------------------------ */
/* Emissive area-light collection                                      */
/* ------------------------------------------------------------------ */

/*
 * Collect the primitives that act as sampled AREA LIGHTS into the bounded
 * Scene.emissive_lights[] list, in deterministic primitive (file) order.
 *
 * A primitive qualifies when it is a SPHERE whose resolved material has
 * `pbr != 0` AND a non-zero `emissive` — i.e. exactly the opt-in PBR
 * self-emitters the renderer already brightens with the added `emissive` term.
 * `pbr`/`emissive` default to 0 / (0,0,0) and are opt-in, so a scene without
 * any such material leaves the list EMPTY and the renderer's area-light block
 * is never entered (byte-identical output).
 *
 * The list is fixed-size (no allocation, no OOM path) and truncated at
 * SCENE_MAX_EMISSIVE_LIGHTS. It is built once, single-threaded, before the BVH
 * (the BVH stores indices only, so the order relative to bvh_build does not
 * matter); the renderer only reads it, so the result is identical for every
 * run and thread count.
 */
static void scene_collect_emissive_lights(Scene *s)
{
    int i;

    s->emissive_light_count = 0;
    if (!s->materials) {
        return;
    }

    for (i = 0; i < s->geo.count; ++i) {
        const Primitive *p;
        const Material *m;
        EmissiveLight *l;

        if (s->emissive_light_count >= SCENE_MAX_EMISSIVE_LIGHTS) {
            break; /* bounded: ignore any further emitters */
        }
        p = &s->geo.prims[i];
        if (p->kind != PRIM_SPHERE) {
            continue; /* v1: sphere emitters only */
        }
        if (p->material_index < 0 || p->material_index >= s->material_count) {
            continue;
        }
        m = &s->materials[p->material_index];
        if (!m->pbr) {
            continue; /* opt-in gate */
        }
        if (m->emissive.x == 0.0 && m->emissive.y == 0.0 &&
            m->emissive.z == 0.0) {
            continue; /* non-emitter */
        }
        if (!(p->radius > 0.0)) {
            continue; /* degenerate sphere cannot be sampled */
        }

        l = &s->emissive_lights[s->emissive_light_count++];
        l->prim_index = i;
        l->center     = p->center;
        l->radius     = p->radius;
        l->emissive   = m->emissive;
    }
}

int scene_build_from_desc(Scene *s, const SceneDesc *d)
{
    Material *mats;
    int i;

    if (!s || !d) return 1;

    /* Zero the container and initialise every owned pointer. */
    geometry_init(&s->geo);
    s->bvh = NULL;
    s->materials = NULL;
    s->material_count = 0;
    s->water_level = d->water_level;
    s->water_material = -1;
    s->emissive_light_count = 0;   /* empty until the collector runs below */

    /* --- Sky --------------------------------------------------------- */
    s->sky = d->sky;

    /* --- Material table --------------------------------------------- */
    if (d->material_count > 0) {
        mats = (Material *)calloc((size_t)d->material_count, sizeof(Material));
        if (!mats) {
            goto fail;
        }
        for (i = 0; i < d->material_count; ++i) {
            /* Whole-struct copy: this carries the procedural-texture fields
             * (texture_kind / texture_scale / texture_color_a / texture_color_b)
             * into the runtime Material table with no per-field wiring. */
            mats[i] = d->materials[i].mat;
        }
        s->materials = mats;
        s->material_count = d->material_count;
    }
    s->water_material = d->water_material;

    /* --- Explicit primitives, in file order ------------------------- */
    for (i = 0; i < d->prim_count; ++i) {
        const ScenePrimDesc *pd = &d->prims[i];
        int mat = (pd->material_index >= 0) ? pd->material_index : MAT_GROUND;
        if (geometry_add(&s->geo, scene_prim_from_desc(pd, mat)) < 0) {
            goto fail;
        }
    }

    /* --- Plant directives, in file order ---------------------------- */
    {
        unsigned tree_index = 0;
        unsigned bush_index = 0;

        for (i = 0; i < d->plant_count; ++i) {
            const ScenePlantDesc *pd = &d->plants[i];
            unsigned group = (pd->kind == SD_PLANT_BUSH) ? 0xBu : 0x7u;
            unsigned idx = (pd->kind == SD_PLANT_BUSH) ? bush_index++ : tree_index++;
            unsigned seed = pd->has_seed ? pd->seed
                                         : scene_hash(d->sky.seed, group, idx);
            int bark_mat, leaf_mat;

            scene_resolve_plant_mats(pd, seed, &bark_mat, &leaf_mat);
            if (scene_add_plant_desc(&s->geo, pd, bark_mat, leaf_mat, seed) != 0) {
                goto fail;
            }
        }
    }

    /* --- Emissive area lights (bounded, deterministic file order) ---- */
    scene_collect_emissive_lights(s);

    /* --- Acceleration structure ------------------------------------- */
    s->bvh = bvh_build(&s->geo);
    if (!s->bvh && s->geo.count > 0) {
        goto fail;
    }

    return 0;

fail:
    scene_free(s);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Built-in default description                                        */
/* ------------------------------------------------------------------ */

/* Build one explicit ScenePrimDesc with all fields zeroed. */
static void scene_desc_zero_prim(ScenePrimDesc *p)
{
    memset(p, 0, sizeof(*p));
    p->material_index = SCENE_DESC_NO_MATERIAL;
}

void scene_default_desc(SceneDesc *out)
{
    Material mats[MAT_COUNT];
    ScenePrimDesc prim;
    ScenePlantDesc plant;
    int i;

    if (!out) return;

    scene_desc_init(out);

    /* --- Camera (§8 items 1-4) -------------------------------------- */
    out->camera.present = 1;
    out->camera.eye = vec3(-18.0, 6.0, 22.0);
    out->camera.target = vec3(0.0, 5.0, -20.0);
    out->camera.up = vec3(0.0, 1.0, 0.0);
    out->camera.vfov_deg = 40.0;
    out->camera.aspect = 16.0 / 9.0;
    /* Depth-of-field defaults: pinhole; focus derived from |target - eye|. */
    out->camera.aperture = CAMERA_DEFAULT_APERTURE;
    out->camera.focus_distance = CAMERA_FOCUS_DISTANCE_DERIVED;

    /* --- Sky (§8 items 6-19): defaults, seed = DEFAULT_SEED 1337 ----- */
    sky_default_params(&out->sky);
    out->sky.seed = 1337u;
    out->has_sky = 1;

    /* --- Globals (§8 items 29-30) ----------------------------------- */
    out->water_level = SCENE_WATER_LEVEL;
    out->water_material = MAT_WATER;
    out->water_enabled = 1;

    /* --- Materials (§8 items 20-26), in table order ----------------- */
    scene_fill_materials(mats);
    for (i = 0; i < MAT_COUNT; ++i) {
        if (scene_desc_add_material(out, scene_mat_names[i], &mats[i]) < 0)
            goto oom;
    }

    /* --- Ground plane (§8 item 27): prim_index 0 -------------------- */
    scene_desc_zero_prim(&prim);
    prim.kind = PRIM_PLANE;
    prim.point = vec3(0.0, 0.0, 0.0);
    prim.normal = vec3(0.0, 1.0, 0.0);
    prim.material_name = (char *)scene_mat_names[MAT_GROUND];
    if (scene_desc_add_prim(out, &prim) < 0) goto oom;
    out->prims[out->prim_count - 1].material_index = MAT_GROUND;

    /* --- Water pond box (§8 item 28): prim_index 1 ------------------ */
    scene_desc_zero_prim(&prim);
    prim.kind = PRIM_BOX;
    prim.center = vec3(SCENE_POND_CX,
                       SCENE_WATER_LEVEL - SCENE_POND_HALF_Y,
                       SCENE_POND_CZ);
    prim.half = vec3(SCENE_POND_HALF_X, SCENE_POND_HALF_Y, SCENE_POND_HALF_Z);
    prim.material_name = (char *)scene_mat_names[MAT_WATER];
    if (scene_desc_add_prim(out, &prim) < 0) goto oom;
    out->prims[out->prim_count - 1].material_index = MAT_WATER;

    /* --- Trees (§8 items 31-36) ------------------------------------- */
    for (i = 0; i < (int)SCENE_ARRAY_LEN(scene_trees); ++i) {
        unsigned seed = scene_hash(1337u, 0x7u, (unsigned)i);
        int variant = (int)(scene_rand(seed, 0u, 0x51u) * 4.0);

        if (variant < 0) variant = 0;
        if (variant > 3) variant = 3;

        memset(&plant, 0, sizeof(plant));
        plant.kind = SD_PLANT_TREE;
        plant.position = vec3(scene_trees[i].x, 0.0, scene_trees[i].z);
        plant.height = scene_trees[i].trunk_len;
        plant.radius = scene_trees[i].trunk_radius;
        plant.has_seed = 1;
        plant.seed = seed;
        plant.material_bark = (char *)scene_mat_names[MAT_BARK];
        plant.material_leaf = (char *)scene_mat_names[MAT_LEAF0 + variant];
        plant.has_leaf_variant = 1;
        plant.leaf_variant = variant;
        plant.material_bark_index = MAT_BARK;
        plant.material_leaf_index = MAT_LEAF0 + variant;
        if (scene_desc_add_plant(out, &plant) < 0) goto oom;
    }

    /* --- Bushes (§8 items 37-38) ------------------------------------ */
    for (i = 0; i < (int)SCENE_ARRAY_LEN(scene_bushes); ++i) {
        unsigned seed = scene_hash(1337u, 0xBu, (unsigned)i);
        int variant = (int)(scene_rand(seed, 0u, 0x51u) * 4.0);

        if (variant < 0) variant = 0;
        if (variant > 3) variant = 3;

        memset(&plant, 0, sizeof(plant));
        plant.kind = SD_PLANT_BUSH;
        plant.position = vec3(scene_bushes[i].x, 0.0, scene_bushes[i].z);
        plant.height = scene_bushes[i].trunk_len;
        plant.radius = scene_bushes[i].trunk_radius;
        plant.has_seed = 1;
        plant.seed = seed;
        plant.material_bark = (char *)scene_mat_names[MAT_BARK];
        plant.material_leaf = (char *)scene_mat_names[MAT_LEAF0 + variant];
        plant.has_leaf_variant = 1;
        plant.leaf_variant = variant;
        plant.material_bark_index = MAT_BARK;
        plant.material_leaf_index = MAT_LEAF0 + variant;
        if (scene_desc_add_plant(out, &plant) < 0) goto oom;
    }

    return;

oom:
    /* On allocation failure leave an empty (but valid) description. */
    scene_desc_free(out);
}
