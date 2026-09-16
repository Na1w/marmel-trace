#ifndef RAYTRACER_GEOMETRY_H
#define RAYTRACER_GEOMETRY_H

/*
 * geometry.h - Geometry primitives and ray/primitive intersection.
 *
 * This module owns the shape representation used by the renderer:
 *   - a tagged-union style Primitive (sphere / plane / box / triangle /
 *     tapered cylinder),
 *   - a dynamically growing Geometry container,
 *   - convenience constructors,
 *   - the ray intersection routines and axis-aligned bounding boxes.
 *
 * It depends ONLY on src/vec3.h. There is no I/O and no global state.
 *
 * Conventions:
 *   - All normals stored in a Hit are UNIT length and oriented to oppose the
 *     incoming ray (so `dot(hit.normal, ray.dir) < 0` always holds).
 *   - `front_face` records whether the ray struck the outside surface
 *     (1) or emerged from inside the primitive (0).
 */

#include "vec3.h"

typedef enum {
    PRIM_SPHERE,
    PRIM_PLANE,     /* infinite plane: point + normal */
    PRIM_BOX,       /* axis-aligned box */
    PRIM_TRIANGLE,
    PRIM_CYLINDER   /* finite, capped, with independent bottom/top radii (tapered frustum) */
} PrimKind;

typedef struct {
    PrimKind kind;
    int material_index;   /* index into the scene material table */
    /* SPHERE:    center, radius
       PLANE:     center = a point on plane, radius unused; axis = unit normal
       BOX:       center = box center, half = half extents
       TRIANGLE:  a, b, c
       CYLINDER:  a = base center, b = top center, radius = bottom radius, radius2 = top radius */
    Vec3 center;
    Vec3 axis;
    Vec3 half;
    Vec3 a, b, c;
    double radius;
    double radius2;
} Primitive;

typedef struct {
    Primitive *prims;
    int count;
    int capacity;
} Geometry;

typedef struct {
    double t;             /* distance along ray */
    Vec3 point;           /* hit position */
    Vec3 normal;          /* unit surface normal, oriented to face the incoming ray */
    int material_index;
    int prim_index;       /* index of the hit primitive in Geometry.prims */
    int front_face;       /* 1 if the ray hit the outside surface, 0 if inside */
} Hit;

/* ------------------------------------------------------------------ */
/* Geometry lifecycle (dynamic array)                                  */
/* ------------------------------------------------------------------ */

void geometry_init(Geometry *g);
void geometry_free(Geometry *g);
int  geometry_add(Geometry *g, Primitive p);   /* returns index, or -1 on failure */

/* ------------------------------------------------------------------ */
/* Convenience constructors                                            */
/* ------------------------------------------------------------------ */

Primitive prim_sphere(Vec3 center, double radius, int material_index);
Primitive prim_plane(Vec3 point, Vec3 normal, int material_index);
Primitive prim_box(Vec3 center, Vec3 half, int material_index);
Primitive prim_triangle(Vec3 a, Vec3 b, Vec3 c, int material_index);
Primitive prim_cylinder(Vec3 base, Vec3 top, double r_bottom, double r_top, int material_index);

/* ------------------------------------------------------------------ */
/* Intersection                                                        */
/* ------------------------------------------------------------------ */

/* `tmin`/`tmax` bound the valid ray parameter interval (use tmin = 1e-4 to
 * avoid self-intersection). Returns 1 on hit, 0 otherwise. When a hit is
 * returned, the NEAREST intersection within [tmin, tmax] is written to *out.
 * `r.dir` is normalized internally if it is not already unit length. */
int primitive_intersect(const Primitive *p, Ray r, double tmin, double tmax, Hit *out);

/* Linear scan over all primitives, keeping the nearest hit. */
int geometry_intersect(const Geometry *g, Ray r, double tmin, double tmax, Hit *out);

/* Axis-aligned bounding box of a primitive (for the BVH built in a later task).
 * For PRIM_PLANE writes a large but finite box (e.g. +/-1e4). */
void primitive_bounds(const Primitive *p, Vec3 *out_min, Vec3 *out_max);

#endif /* RAYTRACER_GEOMETRY_H */
