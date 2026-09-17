#ifndef RAYTRACER_SDF_H
#define RAYTRACER_SDF_H

/*
 * sdf.h - Signed Distance Fields, Domain Warping / Displacement, and CSG.
 *
 * Implements:
 *   - Analytical SDFs for Box, Sphere, and Cylinder.
 *   - Domain Warping / Modulators: Sine wave wobble, 3D Perlin noise, Twist, Repel.
 *   - Constructive Solid Geometry (CSG): Union, Difference, Intersection.
 *   - Sphere-tracing (Ray Marching) intersection and analytical/numerical normals.
 *   - Conservative AABB calculation for seamless integration with BVH.
 *
 * Fully deterministic, warning-free under -Wall -Wextra, C11 standard library only.
 */

#include "vec3.h"

typedef enum {
    SDF_OP_NONE = 0,
    SDF_OP_UNION,
    SDF_OP_DIFFERENCE,
    SDF_OP_INTERSECTION
} SdfOp;

typedef enum {
    SDF_SHAPE_BOX,
    SDF_SHAPE_SPHERE,
    SDF_SHAPE_CYLINDER
} SdfShapeKind;

typedef enum {
    DISPLACE_NONE = 0,
    DISPLACE_SINE,
    DISPLACE_NOISE,
    DISPLACE_TWIST,
    DISPLACE_REPEL
} DisplaceKind;

typedef struct {
    DisplaceKind kind;
    Vec3         direction;     /* for sine: displacement direction */
    Vec3         axis;          /* for twist: rotation axis */
    Vec3         center;        /* for repel: center point */
    double       amplitude;     /* sine / noise amplitude */
    double       frequency;     /* sine / noise frequency */
    double       phase;         /* sine phase */
    double       radius;        /* repel influence radius */
    double       strength;      /* twist rate or repel strength */
    unsigned     seed;          /* noise seed */
} DisplaceModifier;

#define SDF_MAX_DISPLACEMENTS 8

typedef struct {
    SdfOp            op;
    SdfShapeKind     shape_a;
    SdfShapeKind     shape_b;
    Vec3             center_a;
    Vec3             param1_a;  /* box half extents, or (0, half_h, 0) for cylinder */
    double           param2_a;  /* sphere radius or cylinder radius */
    Vec3             center_b;
    Vec3             param1_b;
    double           param2_b;
    DisplaceModifier displace;  /* backwards compat / primary modifier */
    DisplaceModifier displaces[SDF_MAX_DISPLACEMENTS];
    int              displace_count;
} SdfData;

/* Evaluate distance function at world space point p */
double sdf_eval(const SdfData *sdf, Vec3 p);

/* Evaluate surface normal at world space point p */
Vec3 sdf_normal(const SdfData *sdf, Vec3 p);

/* Compute conservative bounding box for the BVH */
void sdf_bounds(const SdfData *sdf, Vec3 *out_min, Vec3 *out_max);

/* Ray intersection: returns 1 on hit within [tmin, tmax], 0 otherwise */
int sdf_intersect(const SdfData *sdf, Ray ray, double tmin, double tmax,
                  double *out_t, Vec3 *out_point, Vec3 *out_normal);

/* Ray occlusion test for shadow rays */
int sdf_occluded(const SdfData *sdf, Ray ray, double tmin, double tmax);

#endif /* RAYTRACER_SDF_H */
