#ifndef RAYTRACER_SCENE_H
#define RAYTRACER_SCENE_H

/*
 * scene.h - Procedural outdoor scene: ground, pond, trees, bushes, sky.
 *
 * A Scene owns:
 *   - a Geometry container holding every primitive (ground plane, pond box,
 *     tapered-cylinder branches and leaf spheres),
 *   - a BVH built over that geometry (acceleration structure),
 *   - a flat material table referenced by index from each primitive,
 *   - the sky/sun parameters used by the renderer for the background.
 *
 * Determinism: scene_build_from_desc() is a pure function of the description
 * (no rand(), no time(), no global mutable state), so two builds from the
 * same SceneDesc produce byte-identical primitive layouts.
 *
 * Coordinate system: +Y is up, ground level is y = 0, the water surface sits
 * at y = water_level (SCENE_WATER_LEVEL = 0.02, i.e. 2 cm above the ground so
 * the pond top face never z-fights with the grass plane).
 *
 * The module performs no I/O and uses no globals. C11, warning-free.
 */

#include "vec3.h"
#include "camera.h"
#include "geometry.h"
#include "bvh.h"
#include "material.h"
#include "scene_desc.h"   /* SceneDesc: declarative scene description model */

/*
 * One emissive PBR primitive acting as a sampled AREA LIGHT.
 *
 * v1 supports SPHERE emitters only (a sphere has a single well-defined
 * solid-angle cone from any outside shading point, which is what the
 * renderer's uniform cone sampler needs). The list is collected once at
 * scene-build time, in deterministic primitive/file order, into a BOUNDED
 * fixed-size array so there is no dynamic allocation and no new failure path.
 *
 * `emissive` is the emitted radiance Le (linear RGB, may exceed 1); it is
 * distance-independent, so the renderer folds the inverse-square falloff and
 * the cosine-geometry term into the solid-angle sampling measure instead.
 */
typedef struct {
    int    prim_index;   /* index into Scene.geo.prims (a PRIM_SPHERE) */
    Vec3   center;       /* sphere center                              */
    double radius;       /* sphere radius                              */
    Vec3   emissive;     /* emitted radiance Le, linear RGB (may > 1)  */
} EmissiveLight;

/*
 * Hard cap on the number of collected area lights. The first
 * SCENE_MAX_EMISSIVE_LIGHTS emissive PBR spheres (in file order) are used;
 * any further ones are ignored. Together with the renderer's fixed per-light
 * sample count this bounds the extra shadow-ray cost.
 */
#define SCENE_MAX_EMISSIVE_LIGHTS 8

typedef struct {
    Geometry   geo;              /* all primitives                            */
    Bvh       *bvh;              /* acceleration structure over geo (owned)   */
    Material  *materials;        /* material table (owned)                    */
    int        material_count;
    SkyParams  sky;              /* sun + sky/cloud parameters                */
    FogParams  fog;              /* atmospheric fog / smoke parameters        */
    double     water_level;      /* world-space y of the water surface        */
    int        water_material;   /* index of the water material, or -1        */

    /*
     * Emissive PBR primitives promoted to sampled area lights (see
     * EmissiveLight). Built once, single-threaded, by
     * scene_build_from_desc(); read-only during rendering. `count == 0`
     * disables the feature entirely: the renderer's emissive block is gated
     * on it, so a scene with no emissive PBR material renders byte-identically
     * to the pre-area-light renderer.
     */
    EmissiveLight emissive_lights[SCENE_MAX_EMISSIVE_LIGHTS];
    int           emissive_light_count;   /* 0 => feature disabled (default) */
} Scene;

/*
 * Build a scene from a declarative description (docs/scene_format.md).
 *
 * This is the SINGLE scene-construction path: it constructs geometry, the
 * BVH, the material table, and the sky/water metadata from `d`, using the
 * primitive constructors and the procedural tree/bush generator.
 *
 * Primitives are appended in FILE ORDER: the order they appear in `d`
 * (explicit primitives in `d->prims` order, then plant directives in
 * `d->plants` order, each expanding in the deterministic parent-first,
 * depth-first order of the generator). This order determines `prim_index`
 * and the BVH tie-break, so it is significant.
 *
 * Material indices are taken from the resolved fields of `d`
 * (`ScenePrimDesc.material_index`, `ScenePlantDesc.material_*_index`) when
 * present; otherwise they fall back to the built-in defaults (ground/bark and
 * the auto leaf variant). Plant seeds come from `ScenePlantDesc.seed` when
 * `has_seed` is set; otherwise they are derived from the root seed
 * `d->sky.seed` as scene_hash(d->sky.seed, group, index).
 *
 * `scene_default_desc()` produces a SceneDesc for the built-in default scene,
 * so `scene_build_from_desc(s, &default_desc)` reproduces it exactly.
 *
 * Returns 0 on success, non-zero on failure (e.g. allocation failure). On
 * failure the scene is left fully released (as if scene_free had run).
 */
int scene_build_from_desc(Scene *s, const SceneDesc *d);

/*
 * Populate `out` with the built-in default description: a SceneDesc for the
 * default scene (same seven materials with every field, same camera, same sky
 * parameters, same globals/water, same primitive counts/kinds/order, and the
 * same tree/bush plant directives so the procedural generator reproduces the
 * identical geometry — 121 tapered cylinders + 1163 spheres for seed 1337).
 *
 * The caller owns `out` and must release it with scene_desc_free(). The
 * description is first reset with scene_desc_init(), so it is safe to call on
 * a zeroed struct. On allocation failure the description is left empty.
 */
void scene_default_desc(SceneDesc *out);

/*
 * Release everything owned by the scene (material table, BVH, geometry).
 * Safe to call on a zeroed struct and on a partially built scene.
 */
void scene_free(Scene *s);

/*
 * Nearest-hit query. Wraps bvh_intersect() (falling back to a linear scan if
 * no BVH is present). Returns 1 on hit, 0 otherwise.
 */
int scene_intersect(const Scene *s, Ray r, double tmin, double tmax, Hit *out);

/*
 * Any-hit occlusion query for shadow rays. Returns 1 if any primitive blocks
 * the ray within [tmin, tmax], 0 otherwise. Uses an internal thread-local
 * shadow cache for instant early termination on coherent shadow rays.
 */
int scene_occluded(const Scene *s, Ray r, double tmin, double tmax);

/* Any-hit occlusion query with caller-supplied shadow cache pointer. */
int scene_occluded_cached(const Scene *s, Ray r, double tmin, double tmax, int *cache_prim);

/* Material lookup by index; returns NULL if out of range (or s is NULL). */
const Material *scene_material(const Scene *s, int index);

/*
 * Raw framing parameters for the default view (eye, target, up hint, vertical
 * field of view in degrees). Exported so src/main.c can rebuild the camera
 * with the *true* output aspect ratio via camera_create().
 */
void scene_default_view(Vec3 *eye, Vec3 *target, Vec3 *up, double *vfov_deg);

/*
 * A sensible framing camera for the scene, built from scene_default_view()
 * with the default 16:9 aspect ratio. main.c may rebuild the camera with the
 * real aspect using scene_default_view() + camera_create().
 */
Camera scene_default_camera(const Scene *s);

#endif /* RAYTRACER_SCENE_H */
