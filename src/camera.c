/*
 * camera.c - Perspective camera construction and primary-ray generation.
 */

#include "camera.h"

#define CAMERA_PI 3.14159265358979323846

/* Squared-length threshold below which a cross product is degenerate. */
#define CAMERA_DEGENERATE_EPS 1e-12

/*
 * Choose a fallback up axis that is guaranteed not to be parallel to the
 * (unit) forward vector. Used when the caller's up hint is parallel to it.
 */
static Vec3 camera_fallback_up(Vec3 forward)
{
    if (fabs(forward.y) < 0.99) {
        return vec3(0.0, 1.0, 0.0);
    }
    return vec3(1.0, 0.0, 0.0);
}

Camera camera_create(Vec3 from, Vec3 at, Vec3 up, double vfov_deg, double aspect)
{
    Camera cam;

    /* --- Orthonormal look-at basis ------------------------------------- */
    Vec3 forward = vec3_normalize(vec3_sub(at, from));
    if (vec3_length_sq(forward) <= CAMERA_DEGENERATE_EPS) {
        /* `from == at`: no meaningful view direction. Default to -z. */
        forward = vec3(0.0, 0.0, -1.0);
    }

    Vec3 right = vec3_cross(forward, up);
    if (vec3_length_sq(right) <= CAMERA_DEGENERATE_EPS) {
        /* Up hint parallel to forward: nudge to a usable up axis. */
        right = vec3_cross(forward, camera_fallback_up(forward));
    }
    right = vec3_normalize(right);

    Vec3 true_up = vec3_cross(right, forward); /* already unit */

    /* --- Viewport half-extents ---------------------------------------- */
    double vfov_rad = vfov_deg * (CAMERA_PI / 180.0);
    double half_h = tan(vfov_rad * 0.5);
    double half_w = aspect * half_h;

    /* --- Viewport frame in world space -------------------------------- */
    cam.position = from;
    cam.horizontal = vec3_scale(right, 2.0 * half_w);
    cam.vertical = vec3_scale(true_up, 2.0 * half_h);
    cam.lower_left = vec3_sub(
        vec3_add(from, forward),
        vec3_add(vec3_scale(cam.horizontal, 0.5),
                 vec3_scale(cam.vertical, 0.5)));

    /* --- Depth of field defaults: ideal pinhole, focus at the target -- */
    cam.aperture = CAMERA_DEFAULT_APERTURE;
    cam.focus_distance = vec3_length(vec3_sub(at, from));

    return cam;
}

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

Ray camera_ray_dof(const Camera *cam, double u, double v, double r1, double r2)
{
    /* Ideal pinhole: delegate so the result is bit-for-bit camera_ray(). */
    if (cam->aperture <= CAMERA_APERTURE_EPSILON) {
        return camera_ray(cam, u, v);
    }

    /* Pinhole ray: origin at the eye, aimed through the viewport point. */
    Ray pinhole = camera_ray(cam, u, v);

    /*
     * Focal plane. The viewport sits at unit distance in front of the eye, so
     * the pinhole ray reaches the focal plane at t = focus_distance. If the
     * distance is not positive (a degenerate camera) fall back to the unit
     * viewport distance so the ray stays well-defined.
     */
    double fd = cam->focus_distance;
    if (fd <= 0.0) {
        fd = 1.0;
    }
    Vec3 focal = vec3_add(cam->position, vec3_scale(pinhole.dir, fd));

    /*
     * Lens disk sample in the camera plane: uniform over the disk
     * (r = aperture*sqrt(r1) for area-uniformity, theta = 2*pi*r2), using the
     * normalized viewport edge vectors as the in-plane axes. r1 and r2 are
     * independent PRNG channels supplied by the caller, so the two axes are
     * decorrelated and the sampling is deterministic.
     */
    Vec3 right = vec3_normalize(cam->horizontal);
    Vec3 up = vec3_normalize(cam->vertical);
    double rad = cam->aperture * sqrt(r1);
    double theta = 2.0 * CAMERA_PI * r2;
    Vec3 offset = vec3_add(vec3_scale(right, rad * cos(theta)),
                           vec3_scale(up, rad * sin(theta)));

    Ray r;
    r.origin = vec3_add(cam->position, offset);
    r.dir = vec3_normalize(vec3_sub(focal, r.origin));
    return r;
}
