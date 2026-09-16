#ifndef RAYTRACER_CAMERA_H
#define RAYTRACER_CAMERA_H

/*
 * camera.h - Perspective pinhole camera with an orthonormal look-at basis.
 *
 * The viewport is a unit-distance rectangle in front of the eye, so the
 * focal distance is folded into the field of view. `horizontal`/`vertical`
 * are the full-width / full-height viewport edge vectors in world space and
 * `lower_left` is the world-space position of the viewport's lower-left
 * corner.
 */

#include "vec3.h"

typedef struct {
    Vec3 position;   /* eye */
    Vec3 lower_left; /* viewport lower-left corner in world space */
    Vec3 horizontal; /* full-width viewport vector */
    Vec3 vertical;   /* full-height viewport vector */

    /*
     * --- depth of field (thin-lens aperture) -------------------------
     * `aperture` is the radius of the lens disk in world units. It is
     * initialised to CAMERA_DEFAULT_APERTURE (0.0), i.e. an ideal pinhole
     * with no depth of field, so a camera built by camera_create() keeps the
     * exact original behaviour unless a host opts in.
     *
     * `focus_distance` is the distance from the eye to the focal plane, in
     * world units. camera_create() sets it to the distance from the eye to
     * the look-at point, |at - from| (the documented default), so the focal
     * plane passes through the target. A host may override it. It is only
     * meaningful when `aperture > 0`.
     */
    double aperture;       /* lens radius, world units (0 = pinhole)        */
    double focus_distance; /* eye -> focal plane distance (default |at-from|)*/
} Camera;

/* Camera DOF defaults. `aperture == 0` is an ideal pinhole. */
#define CAMERA_DEFAULT_APERTURE 0.0

/* Sentinel for "derive focus_distance from the look-at distance |at - from|".
 * A Camera built by camera_create() stores the resolved distance, never this
 * sentinel; it is used by CameraDesc / the canonical writer to denote an
 * omitted `focus_distance` key. */
#define CAMERA_FOCUS_DISTANCE_DERIVED 0.0

/*
 * Build a camera looking from `from` at `at` with up hint `up`,
 * vertical field of view `vfov_deg` (degrees), and aspect ratio
 * (width / height). Degenerate up vectors are handled internally.
 *
 * The depth-of-field fields are set to their defaults: aperture = 0 (an ideal
 * pinhole) and focus_distance = |at - from| (the focal plane through the
 * target). A host that wants DOF sets aperture (and optionally
 * focus_distance) afterwards.
 */
Camera camera_create(Vec3 from, Vec3 at, Vec3 up, double vfov_deg, double aspect);

/*
 * Primary ray for normalized image coordinates (u, v) in [0,1]^2.
 * u increases to the right, v increases upward (v = 0 is the bottom row).
 * Direction is normalized so the ray parameter is a distance.
 *
 * This is the pinhole primitive: the ray origin is exactly cam->position. It
 * is unchanged for all existing callers/tests. Use camera_ray_dof() for a
 * thin-lens (aperture) ray.
 */
Ray camera_ray(const Camera *cam, double u, double v);

/*
 * Thin-lens (depth-of-field) primary ray for normalized image coordinates
 * (u, v) in [0,1]^2. `r1`, `r2` are uniform samples in [0, 1) supplied by the
 * caller's deterministic PRNG; this function is pure and uses no rand()/clock.
 *
 * The ray origin is jittered to a point on a lens disk of radius
 * cam->aperture in the camera plane (uniform via r = aperture*sqrt(r1),
 * theta = 2*pi*r2, so r1/r2 are decorrelated), and the ray is aimed at the
 * focal point where the pinhole ray crosses the plane at focus_distance. When
 * focus_distance is not positive (a degenerate camera) the unit viewport
 * distance is used so the ray stays well-defined.
 *
 * When cam->aperture <= CAMERA_APERTURE_EPSILON the result is EXACTLY
 * camera_ray(cam, u, v) (bit-for-bit), so a pinhole camera is unaffected.
 */
Ray camera_ray_dof(const Camera *cam, double u, double v, double r1, double r2);

/* Aperture radii at or below this are treated as an ideal pinhole. */
#define CAMERA_APERTURE_EPSILON 1e-12

#endif /* RAYTRACER_CAMERA_H */
