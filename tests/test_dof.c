/*
 * tests/test_dof.c - Unit tests for thin-lens depth of field (DOF).
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure.
 *
 * Coverage (all deterministic; no rand(), no clock):
 *   1. Aperture <= 0 (and the default camera) produces a ray that is EXACTLY
 *      the pinhole camera_ray() -- bit-for-bit -- for many (u,v,r1,r2).
 *   2. Aperture > 0 jitters the origin onto a lens disk: the offset from the
 *      eye lies within the aperture radius, stays in the camera plane
 *      (perpendicular to the view direction), and the sampled disk actually
 *      reaches out to the full radius.
 *   3. Determinism: identical inputs give bit-identical rays, stable across
 *      interleaved unrelated calls.
 *   4. The focal point (pinhole ray at the focus distance) lies exactly on the
 *      thin-lens ray, i.e. the ray is aimed at the plane at focus_distance.
 *   5. camera_create() derives the default focus distance |target - eye|.
 *   6. Scene-format plumbing: `aperture`/`focus_distance` parse, round-trip
 *      bit-for-bit, and the canonical writer OMITS them at their defaults.
 *   7. Render-level wiring: render_image() output actually changes with the
 *      camera aperture (aperture 0 == pinhole render, aperture > 0 differs)
 *      and stays byte-identical for repeated identical runs.
 *
 * C11, -Wall -Wextra clean. All heap memory is freed.
 */

#include "camera.h"
#include "vec3.h"
#include "scene_desc.h"
#include "scene.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int vec3_eq(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static int ray_eq(Ray a, Ray b)
{
    return vec3_eq(a.origin, b.origin) && vec3_eq(a.dir, b.dir);
}

/* A representative non-axis-aligned camera. */
static Camera make_camera(void)
{
    return camera_create(vec3(-18.0, 6.0, 22.0),
                         vec3(0.0, 5.0, -20.0),
                         vec3(0.0, 1.0, 0.0),
                         40.0, 16.0 / 9.0);
}

/* Unit view direction of the camera (eye -> viewport center). */
static Vec3 camera_forward(const Camera *cam)
{
    Vec3 center = vec3_add(cam->lower_left,
                           vec3_add(vec3_scale(cam->horizontal, 0.5),
                                    vec3_scale(cam->vertical, 0.5)));
    return vec3_normalize(vec3_sub(center, cam->position));
}

/* ------------------------------------------------------------------ */
/* Test 1: aperture <= 0 is the exact pinhole ray                      */
/* ------------------------------------------------------------------ */

static void test_zero_aperture_identity(void)
{
    Camera cam = make_camera();
    int exact = 1;

    /* Default camera_create() must be a pinhole. */
    CHECK(cam.aperture == CAMERA_DEFAULT_APERTURE,
          "camera_create defaults aperture to 0 (pinhole)");

    /* A dense sweep over image coords and PRNG samples. */
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            double u = ((double)i + 0.5) / 16.0;
            double v = ((double)j + 0.5) / 16.0;
            for (int k = 0; k < 8; ++k) {
                double r1 = ((double)k + 0.5) / 8.0;
                double r2 = 1.0 - r1;
                Ray pin = camera_ray(&cam, u, v);
                Ray dof = camera_ray_dof(&cam, u, v, r1, r2);
                if (!ray_eq(pin, dof)) {
                    exact = 0;
                }
            }
        }
    }
    CHECK(exact, "aperture 0 yields the exact pinhole ray (bit-for-bit)");

    /* Negative aperture is also treated as a pinhole. */
    cam.aperture = -3.0;
    CHECK(ray_eq(camera_ray(&cam, 0.3, 0.7),
                 camera_ray_dof(&cam, 0.3, 0.7, 0.25, 0.75)),
          "negative aperture yields the exact pinhole ray");
}

/* ------------------------------------------------------------------ */
/* Test 2: aperture > 0 samples a disk of the given radius             */
/* ------------------------------------------------------------------ */

static void test_lens_disk_bounds(void)
{
    Camera cam = make_camera();
    cam.aperture = 0.4;
    cam.focus_distance = 30.0;

    Vec3 fwd = camera_forward(&cam);
    double max_len = 0.0;
    int within = 1;
    int in_plane = 1;
    int nonzero = 0;

    for (int i = 0; i < 64; ++i) {
        for (int j = 0; j < 64; ++j) {
            double r1 = ((double)i + 0.5) / 64.0;
            double r2 = ((double)j + 0.5) / 64.0;
            Ray r = camera_ray_dof(&cam, 0.5, 0.5, r1, r2);
            Vec3 off = vec3_sub(r.origin, cam.position);
            double len = vec3_length(off);

            if (len > cam.aperture + 1e-12) {
                within = 0;
            }
            if (fabs(vec3_dot(off, fwd)) > 1e-12) {
                in_plane = 0; /* the lens disk is perpendicular to the view */
            }
            if (len > 1e-6) {
                nonzero = 1;
            }
            if (len > max_len) {
                max_len = len;
            }
        }
    }

    CHECK(within, "every lens origin lies within the aperture radius");
    CHECK(in_plane, "lens origins lie in the camera plane (perpendicular)");
    CHECK(nonzero, "aperture > 0 actually jitters the ray origin");
    CHECK(max_len > 0.95 * cam.aperture,
          "the sampled disk reaches out to the full lens radius");

    /* r1 = 0 is the lens centre: the origin is exactly the eye. */
    Ray center = camera_ray_dof(&cam, 0.2, 0.8, 0.0, 0.5);
    CHECK(vec3_eq(center.origin, cam.position),
          "r1 = 0 puts the origin exactly at the eye (lens centre)");
}

/* ------------------------------------------------------------------ */
/* Test 3: determinism                                                 */
/* ------------------------------------------------------------------ */

static void test_determinism(void)
{
    Camera cam = make_camera();
    cam.aperture = 0.25;
    cam.focus_distance = 12.0;

    Ray a = camera_ray_dof(&cam, 0.42, 0.17, 0.31, 0.87);
    Ray b = camera_ray_dof(&cam, 0.42, 0.17, 0.31, 0.87);
    CHECK(ray_eq(a, b), "identical inputs give bit-identical rays");

    /* Interleave unrelated calls: no hidden mutable state may leak through. */
    for (int i = 0; i < 32; ++i) {
        (void)camera_ray_dof(&cam, (double)i / 32.0, 0.5,
                             (double)i / 64.0, 0.9);
    }
    Ray c = camera_ray_dof(&cam, 0.42, 0.17, 0.31, 0.87);
    CHECK(ray_eq(a, c), "result is stable after unrelated calls");

    /* Different lens samples must generally give a different origin. */
    Ray d = camera_ray_dof(&cam, 0.42, 0.17, 0.93, 0.87);
    CHECK(!vec3_eq(a.origin, d.origin),
          "different r1 gives a different lens origin");
}

/* ------------------------------------------------------------------ */
/* Test 4: the ray is aimed at the focal plane                         */
/* ------------------------------------------------------------------ */

static void test_focal_point_on_plane(void)
{
    Camera cam = make_camera();
    cam.aperture = 0.3;
    cam.focus_distance = 17.5;

    int on_ray = 1;
    int plane_ok = 1;

    for (int i = 1; i < 8; ++i) {
        for (int j = 1; j < 8; ++j) {
            double u = (double)i / 8.0;
            double v = (double)j / 8.0;
            Ray pin = camera_ray(&cam, u, v);
            /* Focal point = pinhole ray at the focus distance. */
            Vec3 focal = vec3_add(cam.position,
                                  vec3_scale(pin.dir, cam.focus_distance));

            Ray dof = camera_ray_dof(&cam, u, v, 0.37, 0.61);
            Vec3 to_focal = vec3_sub(focal, dof.origin);

            /* The focal point must lie ON the thin-lens ray (parallel and
             * ahead of the origin). */
            Vec3 cr = vec3_cross(to_focal, dof.dir);
            if (vec3_length(cr) > 1e-9 * (1.0 + vec3_length(to_focal))) {
                on_ray = 0;
            }
            if (vec3_dot(to_focal, dof.dir) <= 0.0) {
                on_ray = 0;
            }
            /* And it must sit at the focus distance from the eye. */
            if (fabs(vec3_length(vec3_sub(focal, cam.position))
                     - cam.focus_distance) > 1e-9) {
                plane_ok = 0;
            }
        }
    }

    CHECK(on_ray, "the focal point lies exactly on the thin-lens ray");
    CHECK(plane_ok, "the focal point lies on the plane at focus_distance");

    /* r1 = 0 (lens centre) reproduces the pinhole direction. */
    Ray pin = camera_ray(&cam, 0.6, 0.4);
    Ray dof0 = camera_ray_dof(&cam, 0.6, 0.4, 0.0, 0.0);
    CHECK(fabs(dof0.dir.x - pin.dir.x) < 1e-12 &&
          fabs(dof0.dir.y - pin.dir.y) < 1e-12 &&
          fabs(dof0.dir.z - pin.dir.z) < 1e-12,
          "lens-centre sample reproduces the pinhole direction");
}

/* ------------------------------------------------------------------ */
/* Test 5: camera_create derives focus_distance = |target - eye|       */
/* ------------------------------------------------------------------ */

static void test_default_focus_distance(void)
{
    Vec3 eye = vec3(-18.0, 6.0, 22.0);
    Vec3 target = vec3(0.0, 5.0, -20.0);
    Camera cam = camera_create(eye, target, vec3(0.0, 1.0, 0.0), 40.0, 1.6);
    double expected = vec3_length(vec3_sub(target, eye));

    CHECK(fabs(cam.focus_distance - expected) < 1e-12,
          "camera_create derives focus_distance = |target - eye|");

    /* A degenerate from == at still yields a finite, non-negative distance. */
    Camera same = camera_create(vec3(1.0, 2.0, 3.0), vec3(1.0, 2.0, 3.0),
                                vec3(0.0, 1.0, 0.0), 40.0, 1.6);
    CHECK(same.focus_distance >= 0.0 && isfinite(same.focus_distance),
          "degenerate camera has a finite, non-negative focus_distance");
}

/* ------------------------------------------------------------------ */
/* Test 6: scene-format plumbing (parse + conditional write)           */
/* ------------------------------------------------------------------ */

static const char *DOF_SCENE =
    "# DOF round-trip\n"
    "water_level = 0.02\n"
    "water_material = none\n"
    "water_enabled = 0\n"
    "\n"
    "camera {\n"
    "    eye = -18.0 6.0 22.0\n"
    "    target = 0.0 5.0 -20.0\n"
    "    up = 0.0 1.0 0.0\n"
    "    vfov = 40.0\n"
    "    aperture = 0.35\n"
    "    focus_distance = 12.5\n"
    "}\n"
    "\n"
    "material ground {\n"
    "    albedo = 0.3 0.42 0.16\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n";

static const char *DOF_SCENE_DEFAULTS =
    "# DOF defaults\n"
    "water_level = 0.02\n"
    "water_material = none\n"
    "water_enabled = 0\n"
    "\n"
    "camera {\n"
    "    eye = -18.0 6.0 22.0\n"
    "    target = 0.0 5.0 -20.0\n"
    "    up = 0.0 1.0 0.0\n"
    "    vfov = 40.0\n"
    "}\n"
    "\n"
    "material ground {\n"
    "    albedo = 0.3 0.42 0.16\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n";

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf != NULL) {
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

static void test_scene_format(void)
{
    const char *tmp_out = "/tmp/dof_test_out.scene";
    SceneDesc d1, d2;
    char errbuf[256];

    scene_desc_init(&d1);
    scene_desc_init(&d2);

    CHECK(scene_desc_load_string(&d1, DOF_SCENE, "<test>",
                                 errbuf, sizeof errbuf) == 0,
          "parse scene with aperture/focus_distance succeeds");
    CHECK(d1.camera.present == 1, "camera block present");
    CHECK(d1.camera.aperture == 0.35, "aperture parsed as 0.35");
    CHECK(d1.camera.focus_distance == 12.5, "focus_distance parsed as 12.5");

    CHECK(scene_desc_write(&d1, tmp_out, errbuf, sizeof errbuf) == 0,
          "write scene with DOF keys succeeds");

    char *text = slurp(tmp_out);
    CHECK(text != NULL, "read back written DOF scene");
    if (text != NULL) {
        CHECK(strstr(text, "aperture = 0.35") != NULL,
              "writer emits non-default aperture");
        CHECK(strstr(text, "focus_distance = 12.5") != NULL,
              "writer emits non-default focus_distance");
        free(text);
    }

    CHECK(scene_desc_load(&d2, tmp_out, errbuf, sizeof errbuf) == 0,
          "re-parse written DOF scene succeeds");
    CHECK(d2.camera.aperture == d1.camera.aperture,
          "aperture round-trips bit-for-bit");
    CHECK(d2.camera.focus_distance == d1.camera.focus_distance,
          "focus_distance round-trips bit-for-bit");

    scene_desc_free(&d1);
    scene_desc_free(&d2);
}

static void test_writer_omits_defaults(void)
{
    const char *tmp_out = "/tmp/dof_test_default_out.scene";
    SceneDesc d;
    char errbuf[256];

    scene_desc_init(&d);
    CHECK(scene_desc_load_string(&d, DOF_SCENE_DEFAULTS, "<test>",
                                 errbuf, sizeof errbuf) == 0,
          "parse scene without DOF keys succeeds");
    CHECK(d.camera.aperture == CAMERA_DEFAULT_APERTURE,
          "absent aperture defaults to 0");
    CHECK(d.camera.focus_distance == CAMERA_FOCUS_DISTANCE_DERIVED,
          "absent focus_distance defaults to the derived sentinel (0)");

    CHECK(scene_desc_write(&d, tmp_out, errbuf, sizeof errbuf) == 0,
          "write scene with default DOF keys succeeds");

    char *text = slurp(tmp_out);
    CHECK(text != NULL, "read back written default scene");
    if (text != NULL) {
        CHECK(strstr(text, "aperture") == NULL,
              "writer omits aperture when it equals the default (0)");
        CHECK(strstr(text, "focus_distance") == NULL,
              "writer omits focus_distance when it equals the default (0)");
        free(text);
    }

    scene_desc_free(&d);
}

/* ------------------------------------------------------------------ */
/* Test 7: render-level wiring (render.c consumes cam.aperture)        */
/* ------------------------------------------------------------------ */

static void test_render_uses_aperture(void)
{
    enum { W = 48, H = 27, SPP = 2, DEPTH = 2 };
    SceneDesc desc;
    Scene scene;
    Camera cam;
    Vec3 eye, target, up;
    double vfov = 0.0;
    unsigned char *buf_a = NULL, *buf_b = NULL, *buf_c = NULL;
    size_t nbytes = (size_t)W * (size_t)H * 3u;
    int differ = 0;

    scene_desc_init(&desc);
    scene_default_desc(&desc);
    memset(&scene, 0, sizeof scene);
    CHECK(scene_build_from_desc(&scene, &desc) == 0, "build default scene");
    scene_desc_free(&desc);

    scene_default_view(&eye, &target, &up, &vfov);
    cam = camera_create(eye, target, up, vfov, (double)W / (double)H);

    buf_a = (unsigned char *)malloc(nbytes);
    buf_b = (unsigned char *)malloc(nbytes);
    buf_c = (unsigned char *)malloc(nbytes);
    if (buf_a == NULL || buf_b == NULL || buf_c == NULL) {
        CHECK(0, "allocate render buffers");
        goto done;
    }

    /* Pinhole render (aperture 0). */
    cam.aperture = 0.0;
    CHECK(render_image(&scene, &cam, W, H, SPP, DEPTH, buf_a) == 0,
          "pinhole render succeeds");

    /* Same pinhole camera again: must be byte-identical (determinism). */
    CHECK(render_image(&scene, &cam, W, H, SPP, DEPTH, buf_b) == 0,
          "repeat pinhole render succeeds");
    CHECK(memcmp(buf_a, buf_b, nbytes) == 0,
          "repeated renders are byte-identical (deterministic)");

    /* A large aperture must change the image, proving DOF is wired in. */
    cam.aperture = 3.0;
    CHECK(render_image(&scene, &cam, W, H, SPP, DEPTH, buf_c) == 0,
          "aperture render succeeds");
    for (size_t i = 0; i < nbytes; ++i) {
        if (buf_a[i] != buf_c[i]) {
            differ = 1;
            break;
        }
    }
    CHECK(differ, "aperture > 0 changes the rendered image (DOF is active)");

done:
    free(buf_a);
    free(buf_b);
    free(buf_c);
    scene_free(&scene);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_zero_aperture_identity();
    test_lens_disk_bounds();
    test_determinism();
    test_focal_point_on_plane();
    test_default_focus_distance();
    test_scene_format();
    test_writer_omits_defaults();
    test_render_uses_aperture();

    fprintf(stderr, "test_dof: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
