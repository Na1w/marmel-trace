#ifndef RAYTRACER_BVH_H
#define RAYTRACER_BVH_H

/*
 * bvh.h - Bounding volume hierarchy acceleration structure.
 *
 * A binary BVH built over the primitives of a Geometry using the binned
 * surface area heuristic (SAH), with a median-split fallback. Nodes live in a
 * flat array for cache efficiency and hold only INDICES into Geometry.prims;
 * no primitive data is copied.
 *
 * Lifetime contract:
 *   - The BVH does not own the Geometry. The caller must keep `g` alive and
 *     unmodified for as long as the BVH is used.
 *   - `bvh_intersect` must be given the very same Geometry the BVH was built
 *     from; indices stored in the BVH are meaningless otherwise.
 *
 * Semantics of `bvh_intersect` are identical to `geometry_intersect`: the
 * nearest intersection within [tmin, tmax] is returned, using the same
 * normal-orientation and epsilon conventions.
 *
 * Depends only on geometry.h (which pulls in vec3.h). No I/O, no globals.
 */

#include "geometry.h"

typedef struct Bvh Bvh;   /* opaque */

/* Build a BVH over the primitives of `g`. Returns NULL on failure or if
 * g->count == 0. The BVH stores only indices into g->prims; it does NOT copy
 * the primitives. The caller must keep `g` alive and unmodified while the BVH
 * is in use. */
Bvh *bvh_build(const Geometry *g);

/* Release all memory owned by the BVH (NULL-safe). */
void bvh_free(Bvh *b);

/* Nearest-hit query, semantically identical to geometry_intersect().
 * `g` must be the same Geometry the BVH was built from.
 * Returns 1 on hit (writing the nearest hit within [tmin,tmax] to *out),
 * 0 otherwise. */
int bvh_intersect(const Bvh *b, const Geometry *g, Ray r, double tmin, double tmax, Hit *out);

/* Any-hit occlusion query for shadow rays.
 * Returns 1 on the first primitive intersecting `r` within [tmin, tmax], 0 otherwise.
 * If `last_occluder` is non-NULL, *last_occluder is tested first as a shadow cache;
 * if an occluder is found, its primitive index is stored into *last_occluder. */
int bvh_occluded(const Bvh *b, const Geometry *g, Ray r, double tmin, double tmax, int *last_occluder);

/* Introspection helpers (useful for tests/diagnostics). */
int bvh_node_count(const Bvh *b);
/* Maximum leaf depth (root = 1). Not bounded by the build; highly skewed
 * scenes can produce large depths. Traversal does not depend on this: it uses
 * a fixed on-C-stack array with a malloc fallback for pathological depths. */
int bvh_depth(const Bvh *b);

#endif /* RAYTRACER_BVH_H */
