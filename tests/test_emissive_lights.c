/*
 * tests/test_emissive_lights.c - Tests for emissive PBR primitives acting as
 *                                sampled AREA LIGHTS.
 *
 * Coverage:
 *   1. light_sphere_sample_dir(): returns UNIT vectors; every sampled
 *      direction from an outside point hits the emitter sphere (i.e. lies
 *      within the visible cap of half-angle alpha_max); u1 == 0 returns the
 *      central direction; degenerate inputs are handled without NaN.
 *   2. Collection: a SceneDesc with one emissive PBR sphere yields
 *      emissive_light_count == 1 with the right center/radius/emissive; the
 *      same scene with emissive = 0 or pbr = 0 yields count == 0. Non-sphere
 *      emitters are ignored (v1: spheres only).
 *   3. Backward-compat: the built-in default desc yields count == 0, so the
 *      renderer's area-light block is never entered.
 *   4. Lamp-brightens-surface: an emissive PBR sphere measurably brightens the
 *      non-emissive ground beneath it versus the same scene with the emitter's
 *      emissive set to zero.
 *   5. Determinism: rendering the emissive scene twice is byte-identical; and
 *      (under -DUSE_PTHREADS) threaded vs single-threaded is byte-identical.
 *
 * Additional edge cases (extension):
 *   6. Azimuth uniformity: for a fixed u1 the sampled direction's tangential
 *      component has magnitude sin(alpha) and advances by exactly 2*pi/N per
 *      u2 step, winding once around the cone axis; u2 wraps continuously.
 *   7. Solid-angle identities across a full R/dc sweep (grazing -> head-on),
 *      plus an independent Monte-Carlo estimate of the visible-cap solid angle.
 *   8. Degenerate / clamped sampling: cos_mx == 1 collapses to the axis,
 *      cos_mx == 0 stays in the forward hemisphere, out-of-range u1/u2 clamp,
 *      and a non-unit w is normalised internally (frame invariance).
 *   9. Collection payload fidelity: distinct radius/emissive per emitter, a
 *      single-channel emitter IS collected, zero-emissive / pbr-off spheres
 *      are excluded, and prim_index is strictly increasing.
 *  10. Collection determinism + exact cap: two builds of the same scene give a
 *      byte-identical list, and exactly SCENE_MAX_EMISSIVE_LIGHTS emitters fill
 *      the list with no decoy ever occupying a slot.
 *  11. (seed_key, light index, sample index) stream separation: reproducible
 *      for a fixed triple and distinct across each index.
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own main() and returns non-zero
 * on failure. C11, -Wall -Wextra clean. No rand(). All heap memory freed.
 */

/* Feature-test macro so setenv()/unsetenv() are declared under -std=c11. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"
#include "material.h"
#include "geometry.h"
#include "vec3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness (same style as the other tests)                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

#define TEST_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* Scene texts                                                         */
/* ------------------------------------------------------------------ */

/*
 * A ground plane with a single emissive PBR sphere floating above it. The
 * camera looks down at the ground so the image centre lands on the ground
 * point directly beneath the lamp.
 */
static const char *LAMP_SCENE =
    "camera {\n"
    "    eye = 0 2 8\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 45\n"
    "}\n"
    "material ground {\n"
    "    albedo = 0.8 0.8 0.8\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "}\n"
    "material lamp {\n"
    "    pbr = 1\n"
    "    albedo = 0 0 0\n"
    "    emissive = 3 2.4 1.6\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = ground\n"
    "}\n"
    "sphere {\n"
    "    center = 0 3 0\n"
    "    radius = 1\n"
    "    material = lamp\n"
    "}\n";

/* Identical to LAMP_SCENE but the emitter's emissive is zero. */
static const char *LAMP_OFF_SCENE =
    "camera {\n"
    "    eye = 0 2 8\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 45\n"
    "}\n"
    "material ground {\n"
    "    albedo = 0.8 0.8 0.8\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "}\n"
    "material lamp {\n"
    "    pbr = 1\n"
    "    albedo = 0 0 0\n"
    "    emissive = 0 0 0\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = ground\n"
    "}\n"
    "sphere {\n"
    "    center = 0 3 0\n"
    "    radius = 1\n"
    "    material = lamp\n"
    "}\n";

/* Same emissive colour but pbr = 0: the opt-in gate must exclude it. */
static const char *LAMP_PBR0_SCENE =
    "camera {\n"
    "    eye = 0 2 8\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 45\n"
    "}\n"
    "material ground {\n"
    "    albedo = 0.8 0.8 0.8\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "}\n"
    "material lamp {\n"
    "    pbr = 0\n"
    "    albedo = 0 0 0\n"
    "    emissive = 3 2.4 1.6\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = ground\n"
    "}\n"
    "sphere {\n"
    "    center = 0 3 0\n"
    "    radius = 1\n"
    "    material = lamp\n"
    "}\n";

/* An emissive PBR PLANE: v1 collects spheres only, so this yields count 0. */
static const char *LAMP_PLANE_SCENE =
    "camera {\n"
    "    eye = 0 2 8\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 45\n"
    "}\n"
    "material ground {\n"
    "    albedo = 0.8 0.8 0.8\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "}\n"
    "material emitter_plane {\n"
    "    pbr = 1\n"
    "    albedo = 0 0 0\n"
    "    emissive = 3 2.4 1.6\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = ground\n"
    "}\n"
    "plane {\n"
    "    point = 0 5 0\n"
    "    normal = 0 -1 0\n"
    "    material = emitter_plane\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define WIDTH   32
#define HEIGHT  32
#define SAMPLES 1
#define DEPTH   1
#define BUF_BYTES ((size_t)WIDTH * (size_t)HEIGHT * 3u)

static int vec3_eq(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* Clamp a dot product to [-1, 1] so acos() is well-defined despite rounding. */
static double clamp_unit(double c)
{
    if (c < -1.0) return -1.0;
    if (c > 1.0) return 1.0;
    return c;
}

static size_t count_diff(const unsigned char *a, const unsigned char *b, size_t n)
{
    size_t d = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) ++d;
    }
    return d;
}

/* Sum of every byte of a rendered RGB buffer (a coarse brightness measure). */
static unsigned long sum_bytes(const unsigned char *p, size_t n)
{
    unsigned long s = 0;
    for (size_t i = 0; i < n; ++i) s += p[i];
    return s;
}

/* Build a scene from a text description. Returns 0 on success. */
static int build_scene_from_text(const char *text, Scene *out, Camera *cam)
{
    SceneDesc desc;
    char err[256];
    int rc;

    scene_desc_init(&desc);
    rc = scene_desc_load_string(&desc, text, "<emissive-test>", err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "  parse error: %s\n", err);
        scene_desc_free(&desc);
        return 1;
    }
    memset(out, 0, sizeof *out);
    rc = scene_build_from_desc(out, &desc);
    if (rc != 0) {
        scene_desc_free(&desc);
        return 1;
    }
    if (desc.camera.present) {
        *cam = camera_create(desc.camera.eye, desc.camera.target,
                             desc.camera.up, desc.camera.vfov_deg,
                             (double)WIDTH / (double)HEIGHT);
    } else {
        *cam = scene_default_camera(out);
    }
    scene_desc_free(&desc);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test 1: light_sphere_sample_dir() math                              */
/* ------------------------------------------------------------------ */

static void test_sample_dir(void)
{
    /* Emitter sphere: center C, radius R; shading point P outside it. */
    Vec3 C = vec3(0.0, 3.0, 0.0);
    double R = 1.0;
    Vec3 P = vec3(0.5, 0.2, 4.0);

    Vec3 to_c = vec3_sub(C, P);
    double dc2 = vec3_length_sq(to_c);
    double dc = sqrt(dc2);
    Vec3 w = vec3_scale(to_c, 1.0 / dc);
    double cos_mx = sqrt(1.0 - (R * R) / dc2);

    Primitive sph = prim_sphere(C, R, 0);

    int unit_ok = 1, hit_ok = 1, central_ok = 1;

    for (int i = 0; i < 64; ++i) {
        for (int j = 0; j < 64; ++j) {
            double u1 = ((double)i + 0.5) / 64.0;
            double u2 = ((double)j + 0.5) / 64.0;
            Vec3 d = light_sphere_sample_dir(w, cos_mx, u1, u2);
            Hit h;

            if (fabs(vec3_length(d) - 1.0) > 1e-9) unit_ok = 0;

            /* Every sampled direction must hit the emitter's sphere. */
            if (!primitive_intersect(&sph, (Ray){P, d}, 1e-9, 1e30, &h)) {
                hit_ok = 0;
            }
        }
    }

    /* u1 == 0 selects the cone axis: the central direction, bit-for-bit. */
    {
        Vec3 d = light_sphere_sample_dir(w, cos_mx, 0.0, 0.0);
        if (!vec3_eq(d, w)) central_ok = 0;
    }

    CHECK(unit_ok, "light_sphere_sample_dir returns unit vectors");
    CHECK(hit_ok, "every sampled direction hits the emitter sphere");
    CHECK(central_ok, "u1 == 0 returns the central direction (bit-for-bit)");

    /* Degenerate / defensive inputs must stay finite and unit length. */
    {
        Vec3 z = light_sphere_sample_dir(vec3(0, 0, 0), 0.5, 0.3, 0.7);
        int finite = (z.x == z.x) && (z.y == z.y) && (z.z == z.z);
        CHECK(finite && fabs(vec3_length(z) - 1.0) < 1e-9,
              "zero w stays finite and unit length");

        /* cos_alpha_max out of range is clamped, not NaN. */
        Vec3 c1 = light_sphere_sample_dir(w, 2.0, 0.5, 0.5);
        Vec3 c2 = light_sphere_sample_dir(w, -1.0, 0.5, 0.5);
        CHECK(fabs(vec3_length(c1) - 1.0) < 1e-9 &&
              fabs(vec3_length(c2) - 1.0) < 1e-9,
              "out-of-range cos_alpha_max is clamped safely");

        /* u1 == 1 selects the cone edge; still within the cap (hits sphere). */
        Vec3 e = light_sphere_sample_dir(w, cos_mx, 0.999999, 0.25);
        Hit h;
        CHECK(primitive_intersect(&sph, (Ray){P, e}, 1e-9, 1e30, &h),
              "cone-edge sample still hits the emitter sphere");
    }
}

/* ------------------------------------------------------------------ */
/* Test 2: collection                                                  */
/* ------------------------------------------------------------------ */

static void test_collection(void)
{
    Scene scene;
    Camera cam;

    /* Emissive PBR sphere -> exactly one light with the right payload. */
    if (build_scene_from_text(LAMP_SCENE, &scene, &cam) != 0) {
        CHECK(0, "build emissive scene");
        return;
    }
    CHECK(scene.emissive_light_count == 1,
          "emissive PBR sphere is collected as one area light");
    if (scene.emissive_light_count == 1) {
        const EmissiveLight *l = &scene.emissive_lights[0];
        CHECK(vec3_eq(l->center, vec3(0.0, 3.0, 0.0)),
              "collected light center matches the sphere");
        CHECK(l->radius == 1.0, "collected light radius matches the sphere");
        CHECK(vec3_eq(l->emissive, vec3(3.0, 2.4, 1.6)),
              "collected light emissive matches the material");
        CHECK(l->prim_index >= 0 && l->prim_index < scene.geo.count,
              "collected prim_index is in range");
    }
    scene_free(&scene);

    /* emissive == 0 -> not collected. */
    if (build_scene_from_text(LAMP_OFF_SCENE, &scene, &cam) == 0) {
        CHECK(scene.emissive_light_count == 0,
              "zero-emissive PBR sphere is NOT collected");
        scene_free(&scene);
    }

    /* pbr == 0 -> not collected (opt-in gate). */
    if (build_scene_from_text(LAMP_PBR0_SCENE, &scene, &cam) == 0) {
        CHECK(scene.emissive_light_count == 0,
              "pbr == 0 emitter is NOT collected (opt-in gate)");
        scene_free(&scene);
    }

    /* Emissive PLANE -> not collected (v1 supports spheres only). */
    if (build_scene_from_text(LAMP_PLANE_SCENE, &scene, &cam) == 0) {
        CHECK(scene.emissive_light_count == 0,
              "emissive plane is NOT collected (v1: spheres only)");
        scene_free(&scene);
    }
}

/* ------------------------------------------------------------------ */
/* Test 2b: cone membership over a (u1,u2) grid                        */
/* ------------------------------------------------------------------ */

/*
 * For a grid of (u1,u2) and several emitter geometries (small/distant and
 * large/close, so the cone half-angle varies a lot), every sampled direction
 * must be unit length, lie INSIDE the visible cone (angle from w <= alpha_max,
 * i.e. dot(d, w) >= cos(alpha_max)), and hit the emitter sphere. We also pin
 * the exact solid-angle mapping cos(alpha) == 1 - u1*(1 - cos(alpha_max)).
 */
static void test_sample_dir_cone_grid(void)
{
    struct { Vec3 C; double R; Vec3 P; } cases[3] = {
        { {  0.0, 3.0,  0.0}, 1.00, {  0.5,  0.2, 4.0} }, /* small, nearby  */
        { { -2.0, 5.0,  1.0}, 0.25, { 10.0, -3.0, 0.5} }, /* tiny, distant  */
        { {  1.0, 1.0,  1.0}, 2.50, {  4.0,  1.0, 1.0} }  /* large, close   */
    };
    const int N = 32;
    int unit_ok = 1, cone_ok = 1, hit_ok = 1, cosmap_ok = 1;
    int n = 0;

    for (int ci = 0; ci < 3; ++ci) {
        Vec3 C = cases[ci].C, P = cases[ci].P;
        double R = cases[ci].R;
        Vec3 to_c = vec3_sub(C, P);
        double dc2 = vec3_length_sq(to_c);
        double dc = sqrt(dc2);
        Vec3 w = vec3_scale(to_c, 1.0 / dc);
        double cos_mx = sqrt(1.0 - (R * R) / dc2);
        Primitive sph = prim_sphere(C, R, 0);

        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                double u1 = ((double)i + 0.5) / (double)N;
                double u2 = ((double)j + 0.5) / (double)N;
                Vec3 d = light_sphere_sample_dir(w, cos_mx, u1, u2);
                double ca = clamp_unit(vec3_dot(d, w));
                double expect_ca = 1.0 - u1 * (1.0 - cos_mx);
                Hit h;

                ++n;
                if (fabs(vec3_length(d) - 1.0) > 1e-9) unit_ok = 0;
                /* Inside the visible cap: dot >= cos(alpha_max). */
                if (ca < cos_mx - 1e-9) cone_ok = 0;
                if (fabs(ca - expect_ca) > 1e-9) cosmap_ok = 0;
                if (!primitive_intersect(&sph, (Ray){P, d}, 1e-9, 1e30, &h))
                    hit_ok = 0;
            }
        }
    }

    CHECK(n == 3 * N * N, "cone grid exercised every (u1,u2) cell");
    CHECK(unit_ok, "grid samples are unit length");
    CHECK(cone_ok, "grid samples lie within the visible cone (dot >= cos_mx)");
    CHECK(cosmap_ok, "cos(alpha) == 1 - u1*(1 - cos_mx) for every grid sample");
    CHECK(hit_ok, "every grid sample direction hits the emitter sphere");
}

/* ------------------------------------------------------------------ */
/* Test 2c: solid-angle pdf consistency and cone-edge mapping          */
/* ------------------------------------------------------------------ */

/*
 * The visible cap's half-angle must satisfy sin(alpha_max) = R/dc and
 * cos(alpha_max) = sqrt(1 - (R/dc)^2) (so the solid angle is
 * Omega = 2*pi*(1 - cos(alpha_max)) = 1/pdf). A direction built exactly AT
 * the cone edge (angle alpha_max from w, any azimuth) must map to
 * cos(alpha) == cos(alpha_max) and be TANGENT to the emitter sphere. Finally,
 * u1 -> 1 must approach the edge from inside.
 */
static void test_sample_dir_edge_and_solid_angle(void)
{
    Vec3 C = vec3(0.0, 4.0, -1.0);
    double R = 1.5;
    Vec3 P = vec3(2.0, 0.5, 6.0);

    Vec3 to_c = vec3_sub(C, P);
    double dc2 = vec3_length_sq(to_c);
    double dc = sqrt(dc2);
    Vec3 w = vec3_scale(to_c, 1.0 / dc);

    double sin_am = R / dc;
    double cos_am = sqrt(1.0 - sin_am * sin_am);
    double alpha_max = acos(clamp_unit(cos_am));
    double omega = 2.0 * TEST_PI * (1.0 - cos_am);

    CHECK(fabs(sin(alpha_max) - R / dc) < 1e-12,
          "sin(alpha_max) == R/dc");
    CHECK(fabs(cos(alpha_max) - sqrt(1.0 - (R / dc) * (R / dc))) < 1e-12,
          "cos(alpha_max) == sqrt(1 - (R/dc)^2)");
    CHECK(omega > 0.0 && omega < 2.0 * TEST_PI,
          "visible-cap solid angle lies in (0, 2*pi)");

    /* Build an orthonormal frame around w independently of the implementation
     * (axis = component of least magnitude, mirroring the usual construction)
     * and place directions exactly on the cone edge. */
    {
        Vec3 axis = (fabs(w.x) <= fabs(w.y) && fabs(w.x) <= fabs(w.z))
                        ? vec3(1.0, 0.0, 0.0)
                        : (fabs(w.y) <= fabs(w.z) ? vec3(0.0, 1.0, 0.0)
                                                  : vec3(0.0, 0.0, 1.0));
        Vec3 t = vec3_normalize(vec3_cross(axis, w));
        Vec3 b = vec3_cross(w, t);
        int edge_ok = 1, tangent_ok = 1;

        for (int k = 0; k < 32; ++k) {
            double phi = 2.0 * TEST_PI * ((double)k + 0.5) / 32.0;
            Vec3 radial = vec3_add(vec3_scale(t, cos(phi)),
                                   vec3_scale(b, sin(phi)));
            Vec3 edge = vec3_normalize(vec3_add(vec3_scale(w, cos_am),
                                                vec3_scale(radial, sin_am)));

            if (fabs(clamp_unit(vec3_dot(edge, w)) - cos_am) > 1e-9)
                edge_ok = 0;

            /* Perpendicular distance from the sphere centre to the ray line
             * must equal R for a tangent direction. */
            double tc = vec3_dot(to_c, edge);
            Vec3 closest = vec3_add(P, vec3_scale(edge, tc));
            if (fabs(vec3_length(vec3_sub(C, closest)) - R) > 1e-9)
                tangent_ok = 0;
        }
        CHECK(edge_ok, "edge direction maps to cos(alpha) == cos(alpha_max)");
        CHECK(tangent_ok, "cone-edge directions are tangent to the emitter sphere");
    }

    /* u1 -> 1 approaches the cone edge (alpha -> alpha_max) from inside. */
    {
        Vec3 d = light_sphere_sample_dir(w, cos_am, 1.0 - 1e-9, 0.25);
        double alpha = acos(clamp_unit(vec3_dot(d, w)));
        CHECK(alpha <= alpha_max + 1e-6 && alpha > alpha_max - 1e-4,
              "u1 -> 1 samples the cone edge (alpha ~ alpha_max)");
    }
}

/* ------------------------------------------------------------------ */
/* Test 2d: bounded collection + deterministic file order              */
/* ------------------------------------------------------------------ */

/*
 * A scene with more emissive PBR spheres than SCENE_MAX_EMISSIVE_LIGHTS, with
 * non-collectable primitives interleaved (non-sphere emitters, a pbr-off
 * emitter and a zero-emissive pbr emitter), must collect exactly the cap and
 * preserve file order. The non-sphere emitters sit at x = -9 so their
 * absence from the list is directly observable.
 */
static void test_collection_bounded_order(void)
{
    char text[8192];
    int  off = 0;
    double ex[16];
    int  n_exp = 0;
    Scene scene;
    Camera cam;

    off += snprintf(text + off, sizeof text - (size_t)off,
        "camera {\n"
        "    eye = 0 0 8\n"
        "    target = 0 0 0\n"
        "    up = 0 1 0\n"
        "    vfov = 45\n"
        "}\n"
        "material ground {\n"
        "    albedo = 0.8 0.8 0.8\n"
        "    shininess = 8\n"
        "    ior = 1\n"
        "}\n"
        "material lamp {\n"
        "    pbr = 1\n"
        "    albedo = 0 0 0\n"
        "    emissive = 1 1 1\n"
        "}\n"
        "material lamp_off {\n"
        "    pbr = 1\n"
        "    albedo = 0 0 0\n"
        "    emissive = 0 0 0\n"
        "}\n"
        "material plain_on {\n"
        "    pbr = 0\n"
        "    albedo = 0 0 0\n"
        "    emissive = 1 1 1\n"
        "}\n");

    for (int k = 0; k < 10 && off > 0 && off < (int)sizeof text; ++k) {
        double cx = (double)k * 1.5;
        off += snprintf(text + off, sizeof text - (size_t)off,
            "sphere {\n"
            "    center = %.3f 0 0\n"
            "    radius = 0.5\n"
            "    material = lamp\n"
            "}\n", cx);
        ex[n_exp++] = cx;

        /* Interleave one non-collectable primitive after early spheres. */
        if (k == 0)
            off += snprintf(text + off, sizeof text - (size_t)off,
                "box {\n"
                "    center = -9 0 0\n"
                "    half = 1 1 1\n"
                "    material = lamp\n"
                "}\n");
        if (k == 1)
            off += snprintf(text + off, sizeof text - (size_t)off,
                "cylinder {\n"
                "    base = -9 0 0\n"
                "    top = -9 2 0\n"
                "    r_bottom = 0.5\n"
                "    r_top = 0.5\n"
                "    material = lamp\n"
                "}\n");
        if (k == 2)
            off += snprintf(text + off, sizeof text - (size_t)off,
                "triangle {\n"
                "    a = -9 0 0\n"
                "    b = -8 0 0\n"
                "    c = -9 1 0\n"
                "    material = lamp\n"
                "}\n");
        if (k == 3)
            off += snprintf(text + off, sizeof text - (size_t)off,
                "sphere {\n"
                "    center = -9 3 0\n"
                "    radius = 0.5\n"
                "    material = lamp_off\n"
                "}\n");
        if (k == 4)
            off += snprintf(text + off, sizeof text - (size_t)off,
                "sphere {\n"
                "    center = -9 4 0\n"
                "    radius = 0.5\n"
                "    material = plain_on\n"
                "}\n");
    }

    if (off <= 0 || off >= (int)sizeof text) {
        CHECK(0, "scene text buffer overflowed");
        return;
    }
    if (build_scene_from_text(text, &scene, &cam) != 0) {
        CHECK(0, "build bounded-collection scene");
        return;
    }

    CHECK(scene.emissive_light_count == SCENE_MAX_EMISSIVE_LIGHTS,
          "list is truncated to SCENE_MAX_EMISSIVE_LIGHTS");

    if (scene.emissive_light_count == SCENE_MAX_EMISSIVE_LIGHTS) {
        int order_ok = 1, nonsphere_ok = 1, prim_ok = 1;
        for (int k = 0; k < SCENE_MAX_EMISSIVE_LIGHTS; ++k) {
            const EmissiveLight *l = &scene.emissive_lights[k];
            /* File order: the k-th collected light is the k-th emissive
             * sphere (x = k*1.5), never one of the x = -9 non-sphere
             * emitters or the excluded spheres. */
            if (l->center.x != ex[k] || l->center.y != 0.0 || l->center.z != 0.0)
                order_ok = 0;
            if (l->center.x == -9.0) nonsphere_ok = 0;
            if (l->prim_index < 0 || l->prim_index >= scene.geo.count ||
                scene.geo.prims[l->prim_index].kind != PRIM_SPHERE)
                prim_ok = 0;
        }
        CHECK(order_ok, "collected lights preserve deterministic file order");
        CHECK(nonsphere_ok, "non-sphere / gated emitters are NOT collected");
        CHECK(prim_ok, "every collected prim_index references a SPHERE");
    }
    scene_free(&scene);
}

/* ------------------------------------------------------------------ */
/* Test 2e: empty-list gating for non-default scenes                   */
/* ------------------------------------------------------------------ */

/*
 * A scene whose materials are all PBR but none emissive, and a scene with no
 * PBR materials at all, must both yield emissive_light_count == 0 (the
 * renderer's area-light block is gated on the count).
 */
static void test_empty_list_gating(void)
{
    static const char *NO_EMITTER =
        "camera {\n"
        "    eye = 0 2 8\n"
        "    target = 0 0 0\n"
        "    up = 0 1 0\n"
        "    vfov = 45\n"
        "}\n"
        "material ground {\n"
        "    albedo = 0.8 0.8 0.8\n"
        "    shininess = 8\n"
        "    ior = 1\n"
        "}\n"
        "material shiny {\n"
        "    pbr = 1\n"
        "    albedo = 0.2 0.2 0.2\n"
        "    metallic = 1\n"
        "    roughness = 0.2\n"
        "}\n"
        "plane {\n"
        "    point = 0 0 0\n"
        "    normal = 0 1 0\n"
        "    material = ground\n"
        "}\n"
        "sphere {\n"
        "    center = 0 3 0\n"
        "    radius = 1\n"
        "    material = shiny\n"
        "}\n";
    static const char *NO_PBR =
        "camera {\n"
        "    eye = 0 2 8\n"
        "    target = 0 0 0\n"
        "    up = 0 1 0\n"
        "    vfov = 45\n"
        "}\n"
        "material ground {\n"
        "    albedo = 0.8 0.8 0.8\n"
        "    shininess = 8\n"
        "    ior = 1\n"
        "}\n"
        "plane {\n"
        "    point = 0 0 0\n"
        "    normal = 0 1 0\n"
        "    material = ground\n"
        "}\n"
        "sphere {\n"
        "    center = 0 3 0\n"
        "    radius = 1\n"
        "    material = ground\n"
        "}\n";
    Scene scene;
    Camera cam;

    if (build_scene_from_text(NO_EMITTER, &scene, &cam) == 0) {
        CHECK(scene.emissive_light_count == 0,
              "non-emissive PBR materials leave the light list empty");
        scene_free(&scene);
    } else {
        CHECK(0, "build non-emissive PBR scene");
    }

    if (build_scene_from_text(NO_PBR, &scene, &cam) == 0) {
        CHECK(scene.emissive_light_count == 0,
              "scene with no PBR materials leaves the light list empty");
        scene_free(&scene);
    } else {
        CHECK(0, "build no-PBR scene");
    }
}

/* ------------------------------------------------------------------ */
/* Test 2f: sampling purity + multi-light render determinism            */
/* ------------------------------------------------------------------ */

/*
 * The renderer derives (u1, u2) purely from (seed_key, light index, sample
 * index); since light_sphere_sample_dir is a pure function of its inputs, a
 * fixed triple is reproducible. We assert that purity over a grid and then
 * confirm at the renderer level: a scene with MULTIPLE emissive lights (so the
 * per-light loop runs) renders byte-identically across repeated calls.
 */
static void test_sampling_determinism(void)
{
    Vec3 C = vec3(-1.0, 6.0, 2.0);
    double R = 0.75;
    Vec3 P = vec3(3.0, 1.0, 9.0);
    Vec3 to_c = vec3_sub(C, P);
    double dc2 = vec3_length_sq(to_c);
    double dc = sqrt(dc2);
    Vec3 w = vec3_scale(to_c, 1.0 / dc);
    double cos_mx = sqrt(1.0 - (R * R) / dc2);

    int pure_ok = 1;

    /* A deterministic "stream" of (u1,u2) standing in for the renderer's
     * (seed_key, light index, sample index) derivations. */
    for (int sk = 0; sk < 6; ++sk) {
        for (int li = 0; li < 4; ++li) {
            for (int si = 0; si < 16; ++si) {
                unsigned h = (unsigned)(sk * 977 + li * 131 + si * 17 + 1);
                double u1 = (double)((h * 2654435761u) % 1000003u) / 1000003.0;
                double u2 = (double)((h * 40503u + 12345u) % 999983u) / 999983.0;
                Vec3 a = light_sphere_sample_dir(w, cos_mx, u1, u2);
                Vec3 b = light_sphere_sample_dir(w, cos_mx, u1, u2);
                if (!vec3_eq(a, b)) pure_ok = 0;
            }
        }
    }
    CHECK(pure_ok, "light_sphere_sample_dir is reproducible for fixed inputs");

    /* Different u1 values must give different cone angles (non-degenerate). */
    {
        Vec3 d0 = light_sphere_sample_dir(w, cos_mx, 0.10, 0.30);
        Vec3 d1 = light_sphere_sample_dir(w, cos_mx, 0.90, 0.30);
        double a0 = acos(clamp_unit(vec3_dot(d0, w)));
        double a1 = acos(clamp_unit(vec3_dot(d1, w)));
        CHECK(a1 > a0, "larger u1 yields a wider cone angle (stream not flat)");
    }

    /* Multi-light renderer determinism (exercises the per-light sample loop). */
    {
        static const char *TRI_LAMP =
            "camera {\n"
            "    eye = 0 3 12\n"
            "    target = 0 1 0\n"
            "    up = 0 1 0\n"
            "    vfov = 45\n"
            "}\n"
            "material ground {\n"
            "    albedo = 0.7 0.7 0.7\n"
            "    shininess = 8\n"
            "    ior = 1\n"
            "}\n"
            "material lamp {\n"
            "    pbr = 1\n"
            "    albedo = 0 0 0\n"
            "    emissive = 2 1.5 1\n"
            "}\n"
            "plane {\n"
            "    point = 0 0 0\n"
            "    normal = 0 1 0\n"
            "    material = ground\n"
            "}\n"
            "sphere {\n"
            "    center = -2 3 0\n"
            "    radius = 0.6\n"
            "    material = lamp\n"
            "}\n"
            "sphere {\n"
            "    center = 0 3 0\n"
            "    radius = 0.6\n"
            "    material = lamp\n"
            "}\n"
            "sphere {\n"
            "    center = 2 3 0\n"
            "    radius = 0.6\n"
            "    material = lamp\n"
            "}\n";
        Scene scene;
        Camera cam;
        unsigned char *a, *b;

        if (build_scene_from_text(TRI_LAMP, &scene, &cam) != 0) {
            CHECK(0, "build multi-lamp scene");
            return;
        }
        CHECK(scene.emissive_light_count == 3,
              "three emissive spheres are collected");

        a = (unsigned char *)malloc(BUF_BYTES);
        b = (unsigned char *)malloc(BUF_BYTES);
        if (!a || !b) {
            free(a); free(b);
            scene_free(&scene);
            CHECK(0, "allocation failed");
            return;
        }
        memset(a, 0x11, BUF_BYTES);
        memset(b, 0x22, BUF_BYTES);

        CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, a) == 0,
              "multi-lamp render 1 returns 0");
        CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, b) == 0,
              "multi-lamp render 2 returns 0");
        CHECK(count_diff(a, b, BUF_BYTES) == 0,
              "multi-light emissive render is byte-identical across calls");

        free(a); free(b);
        scene_free(&scene);
    }
}

/* ------------------------------------------------------------------ */
/* Test 3: backward-compat (default scene)                             */
/* ------------------------------------------------------------------ */

static void test_default_scene_empty(void)
{
    Scene scene;
    SceneDesc desc;

    memset(&scene, 0, sizeof scene);
    scene_desc_init(&desc);
    scene_default_desc(&desc);
    if (scene_build_from_desc(&scene, &desc) == 0) {
        CHECK(scene.emissive_light_count == 0,
              "built-in default scene has an EMPTY light list");
        scene_free(&scene);
    } else {
        CHECK(0, "build default scene");
    }
    scene_desc_free(&desc);
}

/* ------------------------------------------------------------------ */
/* Test 4: the lamp brightens the ground beneath it                    */
/* ------------------------------------------------------------------ */

static void test_lamp_brightens(void)
{
    Scene s_on, s_off;
    Camera cam_on, cam_off;
    unsigned char *b_on, *b_off;
    unsigned long sum_on, sum_off;
    int ok = 1;

    if (build_scene_from_text(LAMP_SCENE, &s_on, &cam_on) != 0) {
        CHECK(0, "build lamp-on scene");
        return;
    }
    if (build_scene_from_text(LAMP_OFF_SCENE, &s_off, &cam_off) != 0) {
        CHECK(0, "build lamp-off scene");
        scene_free(&s_on);
        return;
    }

    b_on = (unsigned char *)malloc(BUF_BYTES);
    b_off = (unsigned char *)malloc(BUF_BYTES);
    if (!b_on || !b_off) {
        free(b_on); free(b_off);
        scene_free(&s_on); scene_free(&s_off);
        CHECK(0, "allocation failed");
        return;
    }
    memset(b_on, 0, BUF_BYTES);
    memset(b_off, 0, BUF_BYTES);

    ok &= (render_image(&s_on, &cam_on, WIDTH, HEIGHT, SAMPLES, DEPTH, b_on) == 0);
    ok &= (render_image(&s_off, &cam_off, WIDTH, HEIGHT, SAMPLES, DEPTH, b_off) == 0);
    CHECK(ok, "both renders return 0");

    sum_on = sum_bytes(b_on, BUF_BYTES);
    sum_off = sum_bytes(b_off, BUF_BYTES);
    CHECK(sum_on > sum_off,
          "emissive lamp brightens the scene overall (sum with lamp > without)");

    /* The image centre lands on the ground directly beneath the lamp: it must
     * be strictly brighter with the lamp on. */
    {
        size_t cx = (size_t)(WIDTH / 2);
        size_t cy = (size_t)(HEIGHT / 2);
        size_t idx = (cy * (size_t)WIDTH + cx) * 3u;
        unsigned on_lum = (unsigned)b_on[idx] + b_on[idx + 1] + b_on[idx + 2];
        unsigned off_lum = (unsigned)b_off[idx] + b_off[idx + 1] + b_off[idx + 2];
        CHECK(on_lum > off_lum,
              "ground pixel directly under the lamp is brighter with the lamp");
    }

    /* Sanity: the two images must differ, not be a no-op. */
    CHECK(count_diff(b_on, b_off, BUF_BYTES) > 0,
          "lamp-on and lamp-off renders differ");

    free(b_on);
    free(b_off);
    scene_free(&s_on);
    scene_free(&s_off);
}

/* ------------------------------------------------------------------ */
/* Test 5: determinism (repeat + threaded identity)                    */
/* ------------------------------------------------------------------ */

static void test_determinism(void)
{
    Scene scene;
    Camera cam;
    unsigned char *a, *b, *t;

    if (build_scene_from_text(LAMP_SCENE, &scene, &cam) != 0) {
        CHECK(0, "build scene for determinism");
        return;
    }

    a = (unsigned char *)malloc(BUF_BYTES);
    b = (unsigned char *)malloc(BUF_BYTES);
    t = (unsigned char *)malloc(BUF_BYTES);
    if (!a || !b || !t) {
        free(a); free(b); free(t);
        scene_free(&scene);
        CHECK(0, "allocation failed");
        return;
    }
    memset(a, 0xAA, BUF_BYTES);
    memset(b, 0x55, BUF_BYTES);
    memset(t, 0x00, BUF_BYTES);

    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, a) == 0,
          "render 1 returns 0");
    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, b) == 0,
          "render 2 returns 0");
    CHECK(count_diff(a, b, BUF_BYTES) == 0,
          "emissive render is byte-identical across repeated calls");

#ifdef USE_PTHREADS
    {
        int have_env = (setenv("RAYTRACER_THREADS", "1", 1) == 0);
        if (have_env) {
            CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, t) == 0,
                  "single-worker render returns 0");
            CHECK(count_diff(a, t, BUF_BYTES) == 0,
                  "threaded output == single-threaded output (byte-identical)");
            (void)unsetenv("RAYTRACER_THREADS");
            CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, t) == 0,
                  "dynamic-thread render returns 0");
            CHECK(count_diff(a, t, BUF_BYTES) == 0,
                  "dynamic threads == single-threaded (byte-identical)");
        }
    }
#endif

    free(a); free(b); free(t);
    scene_free(&scene);
}

/* ------------------------------------------------------------------ */
/* Test 2g: azimuthal uniformity of the cone sampler                   */
/* ------------------------------------------------------------------ */

/*
 * light_sphere_sample_dir builds the direction as
 *
 *     dir = normalize( w*cos(alpha) + t*sin(alpha)*cos(phi)
 *                                  + b*sin(alpha)*sin(phi) ),  phi = 2*pi*u2
 *
 * with {t,b} an orthonormal frame around w. Consequently, for a FIXED u1:
 *   - every sample has the same polar angle alpha(u1) (so the same axial
 *     component dot(d,w) == cos(alpha));
 *   - the component of d perpendicular to w has magnitude sin(alpha);
 *   - as u2 advances by 1/N the perpendicular component rotates by exactly
 *     2*pi/N about w (the tangential vector winds once around the axis).
 * This pins the azimuthal part of the mapping, which the cone-grid test (2b)
 * only checks for membership.
 */
static void test_sample_dir_azimuth_uniform(void)
{
    Vec3 C = vec3(0.5, 2.0, -0.5);
    double R = 0.8;
    Vec3 P = vec3(-1.0, 0.5, 3.0);

    Vec3 to_c = vec3_sub(C, P);
    double dc2 = vec3_length_sq(to_c);
    double dc = sqrt(dc2);
    Vec3 w = vec3_scale(to_c, 1.0 / dc);
    double cos_mx = sqrt(1.0 - (R * R) / dc2);

    const int N = 64;
    const double u1 = 0.375; /* fixed polar slice */
    double cos_alpha = 1.0 - u1 * (1.0 - cos_mx);
    double sin_alpha = sqrt(1.0 - cos_alpha * cos_alpha);

    int const_ok = 1, perp_ok = 1, step_ok = 1;
    Vec3 prev_perp = vec3(0.0, 0.0, 0.0);
    int have_prev = 0;

    for (int k = 0; k < N; ++k) {
        double u2 = ((double)k + 0.5) / (double)N;
        Vec3 d = light_sphere_sample_dir(w, cos_mx, u1, u2);
        double axial = vec3_dot(d, w);
        Vec3 perp = vec3_sub(d, vec3_scale(w, axial));
        double perp_len = vec3_length(perp);

        if (fabs(axial - cos_alpha) > 1e-9) const_ok = 0;
        if (fabs(perp_len - sin_alpha) > 1e-9) perp_ok = 0;

        if (have_prev && perp_len > 1e-12) {
            /* Angle between consecutive perpendicular components == 2*pi/N
             * (only valid because u2 steps evenly and the map is linear in
             * phi: no clamping, all u2 in (0,1)). */
            double c = clamp_unit(vec3_dot(prev_perp, perp) /
                                  (vec3_length(prev_perp) * perp_len));
            double step = acos(c);
            if (fabs(step - 2.0 * TEST_PI / (double)N) > 1e-6) step_ok = 0;
        }
        prev_perp = perp;
        have_prev = 1;
    }

    CHECK(const_ok, "fixed u1 -> constant polar angle cos(alpha) across u2");
    CHECK(perp_ok, "perpendicular component magnitude == sin(alpha)");
    CHECK(step_ok, "u2 advances the azimuth by exactly 2*pi/N per step");

    /* u2 near 0 and near 1 are continuous (no discontinuity at the wrap). */
    {
        Vec3 a = light_sphere_sample_dir(w, cos_mx, u1, 0.0);
        Vec3 b = light_sphere_sample_dir(w, cos_mx, u1, 1e-9);
        CHECK(vec3_length(vec3_sub(a, b)) < 1e-7,
              "u2 == 0 and u2 -> 0+ are continuous");
    }
}

/* ------------------------------------------------------------------ */
/* Test 2h: solid-angle identity sweep + Monte-Carlo check             */
/* ------------------------------------------------------------------ */

/*
 * Across a wide sweep of R/dc (grazing cones where alpha_max -> pi/2, and
 * near head-on cones where alpha_max -> 0), the visible-cap half-angle must
 * satisfy sin(alpha_max) == R/dc and cos(alpha_max) == sqrt(1-(R/dc)^2), and
 * the solid angle Omega == 2*pi*(1 - cos(alpha_max)) must lie in (0, 2*pi).
 * An independent uniform-cosine Monte-Carlo estimate of Omega (fraction of
 * random directions from P whose ray hits the sphere, times 4*pi) must match
 * the analytic value — a cross-check that does not reuse the sampler.
 */
static void test_solid_angle_sweep(void)
{
    Vec3 C = vec3(1.0, 3.0, -2.0);
    Vec3 P = vec3(0.0, 0.0, 5.0);
    Vec3 to_c = vec3_sub(C, P);
    double dc = sqrt(vec3_length_sq(to_c));

    double ratios[5] = { 0.02, 0.2, 0.5, 0.8, 0.98 };
    int id_ok = 1, range_ok = 1, mc_ok = 1;

    for (int i = 0; i < 5; ++i) {
        double R = ratios[i] * dc;
        double sin_am = R / dc;
        double cos_am = sqrt(1.0 - sin_am * sin_am);
        double alpha_max = acos(clamp_unit(cos_am));
        double omega = 2.0 * TEST_PI * (1.0 - cos_am);

        if (fabs(sin(alpha_max) - sin_am) > 1e-12) id_ok = 0;
        if (fabs(cos(alpha_max) - cos_am) > 1e-12) id_ok = 0;
        if (fabs(sin_am * sin_am + cos_am * cos_am - 1.0) > 1e-12) id_ok = 0;
        if (!(omega > 0.0 && omega < 2.0 * TEST_PI)) range_ok = 0;

        /* Independent Monte-Carlo solid angle: sample directions uniformly on
         * the sphere (area measure) and count those that hit. The estimator
         * Omega_hat = 4*pi * hits/M has standard error
         * SE = 4*pi*sqrt(p*(1-p)/M) with p = Omega/(4*pi); we accept within
         * 5*SE so the check is a valid statistical test even for tiny cones
         * (where a fixed relative tolerance would be far below the noise). */
        {
            Primitive sph = prim_sphere(C, R, 0);
            long hits = 0;
            const long M = 400000;
            unsigned s = 0x12345678u;
            for (long m = 0; m < M; ++m) {
                /* xorshift32 -> two uniforms -> uniform direction. */
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                double a = (double)(s % 1000000u) / 1000000.0;
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                double b = (double)(s % 1000000u) / 1000000.0;
                double z = 1.0 - 2.0 * a;
                double r = sqrt(1.0 - z * z);
                double phi = 2.0 * TEST_PI * b;
                Vec3 dir = vec3(r * cos(phi), r * sin(phi), z);
                Hit h;
                if (primitive_intersect(&sph, (Ray){P, dir}, 1e-9, 1e30, &h))
                    ++hits;
            }
            double est = 4.0 * TEST_PI * (double)hits / (double)M;
            double p = omega / (4.0 * TEST_PI);
            double se = 4.0 * TEST_PI * sqrt(p * (1.0 - p) / (double)M);
            if (fabs(est - omega) > 5.0 * se + 1e-6) mc_ok = 0;
        }
    }

    CHECK(id_ok, "sin/cos(alpha_max) == R/dc / sqrt(1-(R/dc)^2) across sweep");
    CHECK(range_ok, "visible-cap solid angle in (0, 2*pi) across sweep");
    CHECK(mc_ok, "analytic solid angle matches Monte-Carlo estimate");
}

/* ------------------------------------------------------------------ */
/* Test 2i: degenerate / clamped sampler inputs                        */
/* ------------------------------------------------------------------ */

/*
 * Defensive-input behaviour the renderer relies on:
 *   - cos_mx == 1 (cone collapses to the axis) -> the exact central direction;
 *   - cos_mx == 0 (grazing cone, alpha_max == pi/2) -> still in the forward
 *     hemisphere (dot(d,w) >= 0) and unit length;
 *   - out-of-range u1/u2 are clamped into [0, 1] rather than producing NaN or
 *     escaping the cone;
 *   - a NON-unit w is normalised internally, so the returned direction is
 *     invariant to the length of w (the frame only depends on its direction).
 */
static void test_sample_dir_degenerate_inputs(void)
{
    Vec3 w = vec3_normalize(vec3(0.3, -0.7, 0.5));

    /* cos_mx == 1: the whole cap is the axis; every u1 must give w exactly. */
    {
        int axis_ok = 1;
        for (int k = 0; k < 16; ++k) {
            double u1 = (double)k / 15.0;
            Vec3 d = light_sphere_sample_dir(w, 1.0, u1, 0.4);
            if (vec3_length(vec3_sub(d, w)) > 1e-9) axis_ok = 0;
        }
        CHECK(axis_ok, "cos_mx == 1 collapses every sample to the cone axis");
    }

    /* cos_mx == 0: forward hemisphere, unit length, in [0,1] axial range. */
    {
        int fwd_ok = 1, unit_ok = 1;
        for (int i = 0; i < 24; ++i) {
            for (int j = 0; j < 24; ++j) {
                double u1 = ((double)i + 0.5) / 24.0;
                double u2 = ((double)j + 0.5) / 24.0;
                Vec3 d = light_sphere_sample_dir(w, 0.0, u1, u2);
                double ca = vec3_dot(d, w);
                if (fabs(vec3_length(d) - 1.0) > 1e-9) unit_ok = 0;
                if (ca < -1e-12 || ca > 1.0 + 1e-9) fwd_ok = 0;
            }
        }
        CHECK(unit_ok, "cos_mx == 0 samples are unit length");
        CHECK(fwd_ok, "cos_mx == 0 samples stay in the forward hemisphere");
    }

    /* Out-of-range u1/u2 clamp: results equal the clamped endpoints. */
    {
        Vec3 lo = light_sphere_sample_dir(w, 0.4, 0.0, 0.0);
        Vec3 neg = light_sphere_sample_dir(w, 0.4, -5.0, -3.0);
        Vec3 hi = light_sphere_sample_dir(w, 0.4, 1.0, 1.0);
        Vec3 big = light_sphere_sample_dir(w, 0.4, 9.0, 7.0);
        int finite = 1;
        Vec3 all[4] = { lo, neg, hi, big };
        for (int i = 0; i < 4; ++i) {
            Vec3 d = all[i];
            if (!(d.x == d.x && d.y == d.y && d.z == d.z)) finite = 0;
            if (fabs(vec3_length(d) - 1.0) > 1e-9) finite = 0;
        }
        CHECK(finite, "out-of-range u1/u2 stay finite and unit length");
        CHECK(vec3_eq(neg, lo), "u1 < 0 clamps to u1 == 0");
        CHECK(vec3_eq(big, hi), "u1 > 1 clamps to u1 == 1");
    }

    /* Non-unit w is normalised internally: direction frame is length-invariant. */
    {
        Vec3 w2 = vec3_scale(w, 7.5);
        Vec3 w3 = vec3_scale(w, 0.013);
        int inv_ok = 1;
        for (int i = 0; i < 20; ++i) {
            double u1 = ((double)i + 0.5) / 20.0;
            double u2 = ((double)i + 0.25) / 20.0;
            Vec3 a = light_sphere_sample_dir(w, 0.5, u1, u2);
            Vec3 b = light_sphere_sample_dir(w2, 0.5, u1, u2);
            Vec3 c = light_sphere_sample_dir(w3, 0.5, u1, u2);
            if (vec3_length(vec3_sub(a, b)) > 1e-9) inv_ok = 0;
            if (vec3_length(vec3_sub(a, c)) > 1e-9) inv_ok = 0;
        }
        CHECK(inv_ok, "sampler normalises w internally (length-invariant)");
    }
}

/* ------------------------------------------------------------------ */
/* Test 2j: collection payload fidelity + strict ordering              */
/* ------------------------------------------------------------------ */

/*
 * Three emitters with DISTINCT centers, radii and emissive colours, plus a
 * zero-emissive PBR sphere and a pbr-off sphere interleaved, must yield exactly
 * three collected lights whose payload matches the source primitives and whose
 * prim_index is STRICTLY increasing (deterministic file order). A single
 * non-zero emissive CHANNEL is enough to qualify.
 */
static void test_collection_payload_and_order(void)
{
    static const char *SCENE_TEXT =
        "camera {\n"
        "    eye = 0 0 10\n"
        "    target = 0 0 0\n"
        "    up = 0 1 0\n"
        "    vfov = 45\n"
        "}\n"
        "material g {\n"
        "    albedo = 0.8 0.8 0.8\n"
        "    shininess = 8\n"
        "    ior = 1\n"
        "}\n"
        "material off {\n"
        "    pbr = 1\n"
        "    albedo = 0 0 0\n"
        "    emissive = 0 0 0\n"
        "}\n"
        "material no_pbr {\n"
        "    pbr = 0\n"
        "    albedo = 0 0 0\n"
        "    emissive = 5 5 5\n"
        "}\n"
        "material lamp_a {\n"
        "    pbr = 1\n"
        "    albedo = 0 0 0\n"
        "    emissive = 2 0 0\n"
        "}\n"
        "material lamp_b {\n"
        "    pbr = 1\n"
        "    albedo = 0 0 0\n"
        "    emissive = 0 3 0\n"
        "}\n"
        "material lamp_c {\n"
        "    pbr = 1\n"
        "    albedo = 0 0 0\n"
        "    emissive = 0 0 4\n"
        "}\n"
        "plane {\n"
        "    point = 0 0 0\n"
        "    normal = 0 1 0\n"
        "    material = g\n"
        "}\n"
        "sphere {\n"
        "    center = -3 1 0\n"
        "    radius = 0.5\n"
        "    material = lamp_a\n"
        "}\n"
        "sphere {\n"
        "    center = -1 2 0\n"
        "    radius = 1.0\n"
        "    material = off\n"
        "}\n"
        "sphere {\n"
        "    center = 1 1 5\n"
        "    radius = 0.75\n"
        "    material = no_pbr\n"
        "}\n"
        "sphere {\n"
        "    center = 2 3 0\n"
        "    radius = 1.5\n"
        "    material = lamp_b\n"
        "}\n"
        "sphere {\n"
        "    center = 4 1 0\n"
        "    radius = 0.25\n"
        "    material = lamp_c\n"
        "}\n";

    struct { Vec3 center; double radius; Vec3 emissive; } exp[3] = {
        { { -3.0, 1.0, 0.0 }, 0.50, { 2.0, 0.0, 0.0 } },
        { {  2.0, 3.0, 0.0 }, 1.50, { 0.0, 3.0, 0.0 } },
        { {  4.0, 1.0, 0.0 }, 0.25, { 0.0, 0.0, 4.0 } }
    };
    Scene scene;
    Camera cam;

    if (build_scene_from_text(SCENE_TEXT, &scene, &cam) != 0) {
        CHECK(0, "build payload-fidelity scene");
        return;
    }

    CHECK(scene.emissive_light_count == 3,
          "exactly the three emissive PBR spheres are collected");

    if (scene.emissive_light_count == 3) {
        int payload_ok = 1, order_ok = 1, range_ok = 1;
        int prev_prim = -1;
        for (int k = 0; k < 3; ++k) {
            const EmissiveLight *l = &scene.emissive_lights[k];
            if (!vec3_eq(l->center, exp[k].center)) payload_ok = 0;
            if (l->radius != exp[k].radius) payload_ok = 0;
            if (!vec3_eq(l->emissive, exp[k].emissive)) payload_ok = 0;
            if (l->prim_index <= prev_prim) order_ok = 0;
            prev_prim = l->prim_index;
            if (l->prim_index < 0 || l->prim_index >= scene.geo.count)
                range_ok = 0;
            else if (scene.geo.prims[l->prim_index].kind != PRIM_SPHERE)
                range_ok = 0;
        }
        CHECK(payload_ok, "each collected light carries its own center/radius/Le");
        CHECK(order_ok, "collected prim_index is strictly increasing (file order)");
        CHECK(range_ok, "each collected prim_index references a valid SPHERE");

        /* A single non-zero emissive channel is sufficient to qualify. */
        CHECK(vec3_eq(scene.emissive_lights[0].emissive, vec3(2.0, 0.0, 0.0)),
              "single-channel emitter (2,0,0) is collected");
    }
    scene_free(&scene);
}

/* ------------------------------------------------------------------ */
/* Test 2k: collection determinism + exact cap fill                    */
/* ------------------------------------------------------------------ */

/*
 * Building the SAME scene twice must produce a byte-identical emissive list
 * (deterministic, no hidden state). A scene with EXACTLY
 * SCENE_MAX_EMISSIVE_LIGHTS emitters plus a trailing decoy must fill every
 * slot with an emitter (never the decoy), proving the bound is a cap not an
 * off-by-one.
 */
static void test_collection_determinism_and_cap(void)
{
    char text[8192];
    int off = 0;
    Scene s1, s2;
    Camera c1, c2;

    off += snprintf(text + off, sizeof text - (size_t)off,
        "camera {\n"
        "    eye = 0 0 20\n"
        "    target = 0 0 0\n"
        "    up = 0 1 0\n"
        "    vfov = 45\n"
        "}\n"
        "material g {\n"
        "    albedo = 0.8 0.8 0.8\n"
        "    shininess = 8\n"
        "    ior = 1\n"
        "}\n"
        "material lamp {\n"
        "    pbr = 1\n"
        "    albedo = 0 0 0\n"
        "    emissive = 1 1 1\n"
        "}\n"
        "material decoy {\n"
        "    pbr = 0\n"
        "    albedo = 0 0 0\n"
        "    emissive = 1 1 1\n"
        "}\n"
        "plane {\n"
        "    point = 0 0 0\n"
        "    normal = 0 1 0\n"
        "    material = g\n"
        "}\n");

    for (int k = 0; k < SCENE_MAX_EMISSIVE_LIGHTS && off > 0 &&
                    off < (int)sizeof text; ++k) {
        off += snprintf(text + off, sizeof text - (size_t)off,
            "sphere {\n"
            "    center = %.3f 1 0\n"
            "    radius = 0.4\n"
            "    material = lamp\n"
            "}\n", (double)k);
    }
    /* A trailing non-collectable sphere that would be slot #9 if the cap were
     * off by one. */
    off += snprintf(text + off, sizeof text - (size_t)off,
        "sphere {\n"
        "    center = 99 1 0\n"
        "    radius = 0.4\n"
        "    material = decoy\n"
        "}\n");

    if (off <= 0 || off >= (int)sizeof text) {
        CHECK(0, "cap scene text buffer overflowed");
        return;
    }
    if (build_scene_from_text(text, &s1, &c1) != 0 ||
        build_scene_from_text(text, &s2, &c2) != 0) {
        CHECK(0, "build cap-determinism scenes");
        return;
    }

    CHECK(s1.emissive_light_count == SCENE_MAX_EMISSIVE_LIGHTS,
          "exactly SCENE_MAX_EMISSIVE_LIGHTS emitters fill the list");

    {
        int same = (s1.emissive_light_count == s2.emissive_light_count);
        int no_decoy = 1;
        for (int k = 0; k < s1.emissive_light_count && same; ++k) {
            if (memcmp(&s1.emissive_lights[k], &s2.emissive_lights[k],
                       sizeof(EmissiveLight)) != 0)
                same = 0;
            if (s1.emissive_lights[k].center.x == 99.0) no_decoy = 0;
        }
        CHECK(same, "two builds of the same scene give an identical light list");
        CHECK(no_decoy, "the trailing decoy never occupies a light slot");
    }

    scene_free(&s1);
    scene_free(&s2);
}

/* ------------------------------------------------------------------ */
/* Test 2l: (seed_key, light index, sample index) stream separation    */
/* ------------------------------------------------------------------ */

/*
 * The renderer derives each area-light sample's (u1,u2) purely from
 * (seed_key, light index, sample index). We reproduce that derivation and
 * assert (a) a fixed triple is reproducible and (b) changing any one index
 * changes the resulting direction — i.e. the streams are actually separate,
 * which is what makes multi-light, multi-sample rendering well distributed.
 */
/* Mirror of render.c's per-sample hash stream (same constants family). */
static unsigned emis_hash(unsigned k)
{
    return (k * 2654435761u) ^ ((k + 0x9e3779b9u) >> 15);
}

static double emis_r01(unsigned k, unsigned chan)
{
    return (double)(emis_hash(k ^ chan) % 1000003u) / 1000003.0;
}

static Vec3 emis_sample_dir(Vec3 w, double cos_mx, unsigned sk, unsigned li,
                            unsigned si)
{
    unsigned k = emis_hash(sk ^ 0x51ed2701u) + li * 0x85ebca6bu + si;
    double u1 = emis_r01(k, 0x5a17u);
    double u2 = emis_r01(k, 0x7c3du);
    return light_sphere_sample_dir(w, cos_mx, u1, u2);
}

static void test_sampling_stream_separation(void)
{
    Vec3 C = vec3(2.0, 4.0, -1.0);
    double R = 1.0;
    Vec3 P = vec3(-2.0, 1.0, 8.0);
    Vec3 to_c = vec3_sub(C, P);
    double dc2 = vec3_length_sq(to_c);
    double dc = sqrt(dc2);
    Vec3 w = vec3_scale(to_c, 1.0 / dc);
    double cos_mx = sqrt(1.0 - (R * R) / dc2);

    int repro_ok = 1, sep_ok = 1;
    for (unsigned sk = 0; sk < 5; ++sk) {
        for (unsigned li = 0; li < 4; ++li) {
            for (unsigned si = 0; si < 8; ++si) {
                Vec3 a = emis_sample_dir(w, cos_mx, sk, li, si);
                Vec3 b = emis_sample_dir(w, cos_mx, sk, li, si);
                if (!vec3_eq(a, b)) repro_ok = 0;

                /* Changing the sample index (same seed+light) must move the
                 * direction: otherwise the per-sample loop is degenerate. */
                Vec3 c = emis_sample_dir(w, cos_mx, sk, li, si + 1u);
                if (vec3_length(vec3_sub(a, c)) < 1e-9) sep_ok = 0;

                /* Changing the light index (same seed+sample) must move it. */
                Vec3 d = emis_sample_dir(w, cos_mx, sk, li + 1u, si);
                if (vec3_length(vec3_sub(a, d)) < 1e-9) sep_ok = 0;
            }
        }
    }
    CHECK(repro_ok, "(seed_key, light, sample) triple is reproducible");
    CHECK(sep_ok, "distinct sample/light indices give distinct directions");
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    test_sample_dir();
    test_collection();
    test_sample_dir_cone_grid();
    test_sample_dir_edge_and_solid_angle();
    test_collection_bounded_order();
    test_empty_list_gating();
    test_sampling_determinism();
    test_sample_dir_azimuth_uniform();
    test_solid_angle_sweep();
    test_sample_dir_degenerate_inputs();
    test_collection_payload_and_order();
    test_collection_determinism_and_cap();
    test_sampling_stream_separation();
    test_default_scene_empty();
    test_lamp_brightens();
    test_determinism();

    printf("tests/test_emissive_lights: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
