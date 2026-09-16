/*
 * tests/test_integration_emissive.c - End-to-end INTEGRATION test for emissive
 *                                      PBR spheres acting as sampled AREA LIGHTS.
 *
 * Feature under test (through the FULL scene-description -> render pipeline):
 *
 *   Emissive PBR spheres (material.pbr != 0 AND a non-zero `emissive`) are
 *   collected by src/scene.c into the bounded Scene.emissive_lights[] list and
 *   sampled as area lights inside src/render.c `trace_hit`, so an emitter not
 *   only looks self-lit but ALSO BRIGHTENS neighbouring geometry. The whole
 *   block is gated on `scene->emissive_light_count > 0`; the sampling helper is
 *   `light_sphere_sample_dir()`.
 *
 * Pipeline under test (mirrors src/main.c exactly):
 *   scene_desc_load_string -> scene_build_from_desc -> camera from the parsed
 *     `camera` block -> render_image -> compare byte buffers.
 *
 * ASSERTIONS (end-to-end, deterministic, small + fast):
 *   (a) BRIGHTENING: a scene with an emissive PBR lamp over a NON-emissive
 *       ground is measurably brighter than the SAME scene with the lamp's
 *       `emissive` set to zero. The two scenes differ ONLY in that scalar
 *       vector, so any difference is caused by the area-light term. We assert
 *       BOTH the mean Rec.709 luminance of the whole frame AND the luminance of
 *       the pixel directly beneath the lamp are strictly greater with the lamp
 *       on, plus that the two renders actually differ (not a no-op).
 *   (b) DEFAULT BYTE-IDENTITY: the DEFAULT (no-flag) render is unchanged by the
 *       feature. We render the built-in default scene twice and require the
 *       bytes to match, render a fixed no-emitter scene twice and require the
 *       bytes to match, and require both to have an EMPTY light list (so the
 *       area-light block is never entered).
 *
 * The Makefile links every C source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure.
 *
 * C11, -Wall -Wextra clean. No rand(). All heap memory is freed.
 */

/* Feature-test macro so setenv() is declared under -std=c11. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"
#include "material.h"
#include "vec3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness (same convention as the other integration tests) */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Render dimensions (small + fast, but enough pixels to average)      */
/* ------------------------------------------------------------------ */

#define WIDTH   96
#define HEIGHT  54
#define SAMPLES 2
#define DEPTH   4
#define BUF_BYTES ((size_t)WIDTH * (size_t)HEIGHT * 3u)

/* ------------------------------------------------------------------ */
/* Scene texts                                                         */
/*                                                                     */
/* LAMP_SCENE: a diffuse ground plane with a single warm emissive PBR   */
/* sphere floating directly above the origin. The camera looks down at  */
/* the origin, so the image centre lands on the ground point directly   */
/* beneath the lamp.                                                    */
/* ------------------------------------------------------------------ */

static const char *LAMP_SCENE =
    "# emissive area-light integration scene\n"
    "camera {\n"
    "    eye = 0 2 8\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 45\n"
    "    aperture = 0\n"
    "    focus_distance = 0\n"
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

/*
 * LAMP_OFF_SCENE is byte-for-byte LAMP_SCENE except that the lamp material's
 * `emissive` is zero. The geometry, camera, ground material and the lamp's
 * `pbr = 1` flag are identical, so the ONLY difference between the two renders
 * is the sampled area-light contribution (and the emitter's own self-lit term,
 * which cannot touch the ground pixels we measure for the pixel assertion).
 */
static const char *LAMP_OFF_SCENE =
    "# emissive area-light integration scene\n"
    "camera {\n"
    "    eye = 0 2 8\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 45\n"
    "    aperture = 0\n"
    "    focus_distance = 0\n"
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

/*
 * A fixed scene with NO emitters at all: two diffuse spheres over a floor and
 * a sun/sky. Its light list must stay empty, and it is the baseline used for
 * the default-path byte-identity regression.
 */
static const char *NO_EMITTER_SCENE =
    "# no-emitter baseline scene\n"
    "camera {\n"
    "    eye = 0 3 11\n"
    "    target = 0 2 -4\n"
    "    up = 0 1 0\n"
    "    vfov = 55\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0.40 0.72 -0.57\n"
    "    sun_color = 1 0.96 0.88\n"
    "    horizon_color = 0.78 0.86 1\n"
    "    zenith_color = 0.32 0.52 0.95\n"
    "    gradient_gamma = 0.65\n"
    "    sun_glow_exponent = 350\n"
    "    sun_glow_strength = 0.8\n"
    "    cloud_height = 120\n"
    "    cloud_scale = 0.0025\n"
    "    cloud_coverage = 0.5\n"
    "    cloud_softness = 0.12\n"
    "    cloud_sharpness = 1.5\n"
    "    cloud_octaves = 5\n"
    "    seed = 1337\n"
    "    sun_radius = 0\n"
    "}\n"
    "material floor {\n"
    "    albedo = 0.9 0.9 0.9\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "}\n"
    "material red {\n"
    "    albedo = 0.8 0.15 0.12\n"
    "    specular = 0.2 0.2 0.2\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "}\n"
    "material blue {\n"
    "    albedo = 0.15 0.25 0.85\n"
    "    specular = 0.2 0.2 0.2\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = floor\n"
    "}\n"
    "sphere {\n"
    "    center = -2 1.2 0\n"
    "    radius = 1.2\n"
    "    material = red\n"
    "}\n"
    "sphere {\n"
    "    center = 2 1.2 0\n"
    "    radius = 1.2\n"
    "    material = blue\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static size_t count_diff(const unsigned char *a, const unsigned char *b, size_t n)
{
    size_t d = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) ++d;
    }
    return d;
}

/* Rec. 709 relative luminance of one RGB triple, 0..255. */
static double pixel_luma(const unsigned char *rgb, size_t x, size_t y)
{
    size_t i = (y * (size_t)WIDTH + x) * 3u;
    return 0.2126 * (double)rgb[i + 0u]
         + 0.7152 * (double)rgb[i + 1u]
         + 0.0722 * (double)rgb[i + 2u];
}

/* Mean Rec. 709 relative luminance of a top-down RGB buffer, 0..255. */
static double buffer_mean_luma(const unsigned char *rgb, size_t npix)
{
    double sum = 0.0;
    for (size_t i = 0; i < npix; ++i) {
        sum += 0.2126 * (double)rgb[i * 3u + 0u]
             + 0.7152 * (double)rgb[i * 3u + 1u]
             + 0.0722 * (double)rgb[i * 3u + 2u];
    }
    return (npix > 0u) ? (sum / (double)npix) : 0.0;
}

/*
 * Build a Scene + Camera from scene text exactly as src/main.c does:
 * parse -> build -> camera from the parsed `camera` block (or the scene
 * default view when the file has none). Returns 0 on success.
 */
static int build_scene_from_text(const char *text, Scene *out, Camera *cam)
{
    SceneDesc desc;
    char err[256];
    int rc;

    scene_desc_init(&desc);
    rc = scene_desc_load_string(&desc, text, "<emissive-integration>",
                                err, sizeof err);
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
/* (a) The lamp brightens the neighbouring non-emissive surface        */
/* ------------------------------------------------------------------ */

static void test_lamp_brightens_surface(void)
{
    Scene s_on, s_off;
    Camera cam_on, cam_off;
    unsigned char *b_on, *b_off;
    double mean_on, mean_off;
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

    /* The gating invariant: exactly one collected light with the lamp on,
     * none with the emissive zeroed. This is what enables the render block. */
    CHECK(s_on.emissive_light_count == 1,
          "lamp-on scene collects exactly one emissive area light");
    CHECK(s_off.emissive_light_count == 0,
          "lamp-off scene collects no emissive area light");

    b_on  = (unsigned char *)malloc(BUF_BYTES);
    b_off = (unsigned char *)malloc(BUF_BYTES);
    if (!b_on || !b_off) {
        free(b_on); free(b_off);
        scene_free(&s_on); scene_free(&s_off);
        CHECK(0, "allocation failed");
        return;
    }
    memset(b_on, 0x00, BUF_BYTES);
    memset(b_off, 0xFF, BUF_BYTES);

    ok &= (render_image(&s_on,  &cam_on,  WIDTH, HEIGHT, SAMPLES, DEPTH, b_on) == 0);
    ok &= (render_image(&s_off, &cam_off, WIDTH, HEIGHT, SAMPLES, DEPTH, b_off) == 0);
    CHECK(ok, "both lamp-on and lamp-off renders return 0");

    /* The two scenes differ ONLY in the lamp's emissive vector, so a non-zero
     * byte difference proves the area-light term reached the geometry. */
    CHECK(count_diff(b_on, b_off, BUF_BYTES) > 0,
          "lamp-on and lamp-off renders differ (area light is not a no-op)");

    /* (a.1) Whole-frame mean luminance must be strictly greater. */
    mean_on  = buffer_mean_luma(b_on,  (size_t)WIDTH * (size_t)HEIGHT);
    mean_off = buffer_mean_luma(b_off, (size_t)WIDTH * (size_t)HEIGHT);
    printf("  emissive area light: mean luma %.4f (on) vs %.4f (off)\n",
           mean_on, mean_off);
    CHECK(mean_on > mean_off,
          "mean frame luminance is strictly greater with the lamp on");

    /* (a.2) The ground pixel directly beneath the lamp (image centre) must be
     * strictly brighter: the light really brightens the NON-emissive surface,
     * not merely the emitter's own self-lit pixels. */
    {
        size_t cx = (size_t)(WIDTH / 2);
        size_t cy = (size_t)(HEIGHT / 2);
        double under_on  = pixel_luma(b_on,  cx, cy);
        double under_off = pixel_luma(b_off, cx, cy);
        printf("  ground pixel under lamp: luma %.4f (on) vs %.4f (off)\n",
               under_on, under_off);
        CHECK(under_on > under_off,
              "ground pixel directly under the lamp is brighter with the lamp on");
    }

    free(b_on);
    free(b_off);
    scene_free(&s_on);
    scene_free(&s_off);
}

/* ------------------------------------------------------------------ */
/* (b) The DEFAULT (no-flag) render stays BYTE-IDENTICAL               */
/* ------------------------------------------------------------------ */

static void test_default_render_byte_identical(void)
{
    Scene scene;
    Camera cam;
    unsigned char *a, *b, *c, *d;

    /* --- b.1 the built-in default scene renders identically twice ------ */
    {
        SceneDesc desc;

        memset(&scene, 0, sizeof scene);
        scene_desc_init(&desc);
        scene_default_desc(&desc);
        if (scene_build_from_desc(&scene, &desc) != 0) {
            CHECK(0, "build built-in default scene");
            scene_desc_free(&desc);
            return;
        }
        scene_desc_free(&desc);

        CHECK(scene.emissive_light_count == 0,
              "built-in default scene has an EMPTY light list (area-light block skipped)");

        cam = scene_default_camera(&scene);
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
              "default render 1 returns 0");
        CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, b) == 0,
              "default render 2 returns 0");
        CHECK(count_diff(a, b, BUF_BYTES) == 0,
              "default (no-flag) render is byte-identical across two calls");

        free(a);
        free(b);
        scene_free(&scene);
    }

    /* --- b.2 a fixed no-emitter scene renders identically twice -------- */
    {
        Camera cam2;
        if (build_scene_from_text(NO_EMITTER_SCENE, &scene, &cam2) != 0) {
            CHECK(0, "build no-emitter baseline scene");
            return;
        }
        CHECK(scene.emissive_light_count == 0,
              "no-emitter baseline scene has an EMPTY light list");

        c = (unsigned char *)malloc(BUF_BYTES);
        d = (unsigned char *)malloc(BUF_BYTES);
        if (!c || !d) {
            free(c); free(d);
            scene_free(&scene);
            CHECK(0, "allocation failed");
            return;
        }
        memset(c, 0x33, BUF_BYTES);
        memset(d, 0x44, BUF_BYTES);

        CHECK(render_image(&scene, &cam2, WIDTH, HEIGHT, SAMPLES, DEPTH, c) == 0,
              "baseline render 1 returns 0");
        CHECK(render_image(&scene, &cam2, WIDTH, HEIGHT, SAMPLES, DEPTH, d) == 0,
              "baseline render 2 returns 0");
        CHECK(count_diff(c, d, BUF_BYTES) == 0,
              "fixed no-emitter baseline render is byte-identical across two calls");

        free(c);
        free(d);
        scene_free(&scene);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Silence the stderr progress meter so the test output stays clean. */
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    test_lamp_brightens_surface();
    test_default_render_byte_identical();

    printf("tests/test_integration_emissive: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
