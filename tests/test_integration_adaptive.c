/*
 * tests/test_integration_adaptive.c - End-to-end INTEGRATION test for the
 * OPT-IN adaptive sampling feature (src/render.{h,c} render_image_ex /
 * RenderParams, enabled by --adaptive in src/main.c).
 *
 * Unlike tests/test_adaptive.c (which drives render_image_ex with a Scene built
 * directly), this test exercises the FULL production pipeline that src/main.c
 * uses:
 *
 *   scene_desc_load_string() -> scene_build_from_desc()
 *       -> camera built from the parsed `camera { }` block (camera_create with
 *          the real output aspect ratio, aperture / focus_distance carried over)
 *       -> render_image_ex() with a RenderParams built exactly like main.c's
 *       -> inspect the RGB buffer + render_last_total_samples() /
 *          render_last_max_samples().
 *
 * Contract locked by this test (all through the scene-description path):
 *
 *   (a) HIGH-CONTRAST scene, SAME base spp:
 *         - adaptive=1 DIFFERS from fixed (adaptive=0) at the same base spp;
 *         - adaptive uses MORE total samples than fixed, i.e.
 *           render_last_total_samples() is STRICTLY greater for adaptive;
 *         - CONVERGENCE: against a high-spp reference render the adaptive
 *           result has a SMALLER mean-absolute per-channel byte error than the
 *           low-spp fixed render, and every LARGER sample budget moves the
 *           image strictly closer to the reference with DIMINISHING RETURNS
 *           (the Monte-Carlo 1/sqrt(N) property: the first refinement step
 *           removes more error while spending fewer extra samples than the
 *           second), i.e. raising the cap further changes the image only
 *           marginally.
 *   (b) DEFAULT (no-flag) path stays BYTE-IDENTICAL: adaptive=0 (and the plain
 *       render_image() path) produce bytes identical to a second identical
 *       render, the default scene is unaffected, and the diagnostics report the
 *       fixed n0 budget (max per pixel == n0).
 *
 * The Makefile links every C source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own int main(void). C11,
 * -Wall -Wextra clean, no rand(). Kept small (96x54, low spp) so `make test`
 * stays fast.
 */

/* Feature-test macro so setenv() is declared under -std=c11. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"
#include "vec3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness (same convention as the other integration tests) */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define WIDTH    96
#define HEIGHT   54
#define DEPTH    4
#define BASE_SPP 4                 /* n0 (== --samples)               */
#define DEF_MULT 4                 /* default cap multiplier          */
#define BIG_MULT 16                /* raised cap, same tau            */
#define REF_SPP  64                /* high-spp convergence reference  */
#define DEF_TAU  0.02              /* default tolerance               */

#define BUF_BYTES ((size_t)WIDTH * (size_t)HEIGHT * 3u)
#define NPIXELS   ((size_t)WIDTH * (size_t)HEIGHT)
#define BASE_TOTAL ((unsigned long long)NPIXELS * (unsigned long long)BASE_SPP)

/*
 * High-contrast scene: a fine checkerboard floor plus a sphere, under a
 * cloudless sky. Many pixels straddle an edge and have large per-sample
 * variance under AA jitter, so the relative standard error of the mean
 * luminance exceeds tau and refinement engages. The scene is written as a
 * SCENE DESCRIPTION and carries a `camera { }` block, so the camera is built
 * from the parsed block exactly like src/main.c does.
 */
static const char *CONTRAST_SCENE =
    "# adaptive integration scene (high contrast)\n"
    "water_enabled = 0\n"
    "water_material = none\n"
    "camera {\n"
    "    eye = 0 3 9\n"
    "    target = 0 1.2 -0.5\n"
    "    up = 0 1 0\n"
    "    vfov = 45\n"
    "    aperture = 0\n"
    "    focus_distance = 0\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0.45 0.70 -0.55\n"
    "    sun_color = 1 0.96 0.9\n"
    "    horizon_color = 0.8 0.88 1\n"
    "    zenith_color = 0.3 0.5 0.95\n"
    "    gradient_gamma = 0.65\n"
    "    sun_glow_exponent = 320\n"
    "    sun_glow_strength = 0.85\n"
    "    cloud_coverage = 0\n"
    "    seed = 90210\n"
    "}\n"
    "material floor {\n"
    "    albedo = 1 1 1\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 16\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    texture = checker\n"
    "    texture_scale = 0.35\n"
    "    texture_color_a = 0.95 0.95 0.95\n"
    "    texture_color_b = 0.02 0.03 0.05\n"
    "}\n"
    "material ball {\n"
    "    albedo = 0.2 0.35 0.75\n"
    "    specular = 0.1 0.1 0.1\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = floor\n"
    "}\n"
    "sphere {\n"
    "    center = -1.0 1.2 -0.5\n"
    "    radius = 1.2\n"
    "    material = ball\n"
    "}\n";

/*
 * "Default" scene: the same geometry/materials but NO adaptive keys at all, so
 * it represents the flag-absent, unmodified default render path. The default
 * (no-flag) render of this scene must stay byte-identical across repeats.
 */
static const char *DEFAULT_SCENE =
    "# adaptive integration scene (default, no adaptive keys)\n"
    "camera {\n"
    "    eye = 0 3 9\n"
    "    target = 0 1.2 -0.5\n"
    "    up = 0 1 0\n"
    "    vfov = 45\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0.45 0.70 -0.55\n"
    "    sun_color = 1 0.96 0.9\n"
    "    horizon_color = 0.8 0.88 1\n"
    "    zenith_color = 0.3 0.5 0.95\n"
    "    gradient_gamma = 0.65\n"
    "    sun_glow_exponent = 320\n"
    "    sun_glow_strength = 0.85\n"
    "    cloud_coverage = 0\n"
    "    seed = 90210\n"
    "}\n"
    "material plain {\n"
    "    albedo = 0.6 0.6 0.6\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 16\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = plain\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Helpers (mirror src/main.c's scene + camera construction)           */
/* ------------------------------------------------------------------ */

/*
 * Parse `text` into `desc`, build `scene`, and construct the Camera from the
 * parsed `camera { }` block exactly like src/main.c: camera_create() with the
 * real output aspect ratio, then aperture / focus_distance carried over.
 * Returns 0 on success; the caller owns `desc` (scene_desc_free) and `scene`
 * (scene_free).
 */
static int build_scene_from_desc(const char *text, const char *name,
                                 SceneDesc *desc, Scene *scene, Camera *cam)
{
    char errbuf[256];
    errbuf[0] = '\0';

    scene_desc_init(desc);
    if (scene_desc_load_string(desc, text, name, errbuf, sizeof errbuf) != 0) {
        fprintf(stderr, "  parse error (%s): %s\n", name, errbuf);
        scene_desc_free(desc);
        return -1;
    }

    memset(scene, 0, sizeof *scene);
    if (scene_build_from_desc(scene, desc) != 0) {
        fprintf(stderr, "  scene_build_from_desc failed (%s)\n", name);
        scene_desc_free(desc);
        return -1;
    }

    {
        const CameraDesc *cd = &desc->camera;
        if (cd->present) {
            *cam = camera_create(cd->eye, cd->target, cd->up, cd->vfov_deg,
                                 (double)WIDTH / (double)HEIGHT);
            cam->aperture = cd->aperture;
            if (cd->focus_distance > CAMERA_FOCUS_DISTANCE_DERIVED) {
                cam->focus_distance = cd->focus_distance;
            }
        } else {
            *cam = scene_default_camera(scene);
        }
    }
    return 0;
}

/* Number of differing bytes between two equal-sized RGB buffers. */
static size_t count_diff(const unsigned char *a, const unsigned char *b)
{
    size_t d = 0;
    for (size_t i = 0; i < BUF_BYTES; ++i) {
        if (a[i] != b[i]) ++d;
    }
    return d;
}

/* Mean absolute per-channel byte error against a reference buffer. */
static double mean_abs_error(const unsigned char *a, const unsigned char *ref)
{
    double sum = 0.0;
    for (size_t i = 0; i < BUF_BYTES; ++i) {
        int d = (int)a[i] - (int)ref[i];
        if (d < 0) d = -d;
        sum += (double)d;
    }
    return sum / (double)BUF_BYTES;
}

/* Build a RenderParams the way src/main.c does (0 == "use the default"). */
static void make_params(RenderParams *p, int adaptive, int max_spp, double tau)
{
    p->samples_per_pixel = BASE_SPP;
    p->max_depth         = DEPTH;
    p->adaptive          = adaptive;
    p->adaptive_max_spp  = max_spp;   /* 0 => engine default 4*n0 */
    p->adaptive_tau      = tau;       /* 0 => engine default 0.02 */
}

/* ------------------------------------------------------------------ */
/* (a) Adaptive engages end-to-end on a high-contrast scene            */
/* ------------------------------------------------------------------ */

static void test_adaptive_engages_e2e(void)
{
    SceneDesc desc;
    Scene scene;
    Camera cam;
    if (build_scene_from_desc(CONTRAST_SCENE, "<adaptive-e2e>", &desc, &scene,
                              &cam) != 0) {
        CHECK(0, "high-contrast scene parses + builds via scene_desc");
        return;
    }

    CHECK(desc.camera.present == 1, "camera block is present in the description");
    CHECK(scene.geo.count > 0, "built scene has geometry");

    unsigned char *fixed = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *adap  = (unsigned char *)malloc(BUF_BYTES);
    if (fixed == NULL || adap == NULL) {
        free(fixed); free(adap); scene_free(&scene); scene_desc_free(&desc);
        CHECK(0, "buffers allocated");
        return;
    }
    memset(fixed, 0xAA, BUF_BYTES);
    memset(adap,  0x55, BUF_BYTES);

    /* Fixed path: adaptive OFF, default cap/tau (0 == "use defaults"). */
    RenderParams off;
    make_params(&off, 0, 0, 0.0);
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &off, fixed) == 0,
          "fixed render (adaptive=0) succeeds end-to-end");
    unsigned long long fixed_total = render_last_total_samples();
    CHECK(fixed_total == BASE_TOTAL,
          "fixed path reports exactly n0 samples per pixel");
    CHECK(render_last_max_samples() == BASE_SPP,
          "fixed path reports n0 as the max per-pixel count");

    /* Adaptive path: default cap (4*n0) and default tau (0.02). */
    RenderParams on;
    make_params(&on, 1, 0, 0.0);
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &on, adap) == 0,
          "adaptive render (adaptive=1) succeeds end-to-end");
    unsigned long long adap_total = render_last_total_samples();
    int adap_max = render_last_max_samples();

    /* The adaptive render must DIFFER from the fixed one at the same base spp. */
    CHECK(count_diff(fixed, adap) > 0,
          "adaptive render DIFFERS from fixed at the same base spp");

    /* ... and must use STRICTLY more total samples. */
    CHECK(adap_total > fixed_total,
          "adaptive uses STRICTLY more total samples than fixed (same base spp)");
    CHECK(adap_total > BASE_TOTAL,
          "adaptive total exceeds the base n0 budget");
    CHECK(adap_total <=
              (unsigned long long)NPIXELS *
                  (unsigned long long)(DEF_MULT * BASE_SPP),
          "adaptive total respects the default cap N_max = 4*n0");
    CHECK(adap_max > BASE_SPP && adap_max <= DEF_MULT * BASE_SPP,
          "adaptive max per-pixel count is in (n0, 4*n0]");

    printf("integration_adaptive: fixed total=%llu adaptive total=%llu "
           "max/px=%d (n0=%d, Nmax=%d)\n",
           fixed_total, adap_total, adap_max, BASE_SPP, DEF_MULT * BASE_SPP);

    free(fixed); free(adap);
    scene_free(&scene);
    scene_desc_free(&desc);
}

/* ------------------------------------------------------------------ */
/* (a) Convergence: adaptive closer to a high-spp reference than fixed */
/* ------------------------------------------------------------------ */

static void test_adaptive_convergence_e2e(void)
{
    SceneDesc desc;
    Scene scene;
    Camera cam;
    if (build_scene_from_desc(CONTRAST_SCENE, "<adaptive-conv>", &desc, &scene,
                              &cam) != 0) {
        CHECK(0, "high-contrast scene parses + builds via scene_desc");
        return;
    }

    unsigned char *fixed = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *adap  = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *ref   = (unsigned char *)malloc(BUF_BYTES);
    if (fixed == NULL || adap == NULL || ref == NULL) {
        free(fixed); free(adap); free(ref);
        scene_free(&scene); scene_desc_free(&desc);
        CHECK(0, "buffers allocated");
        return;
    }

    /* Low-spp fixed render at the base spp (n0). */
    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, BASE_SPP, DEPTH, fixed) == 0,
          "low-spp fixed render succeeds");
    /* Adaptive render at the SAME base spp with the default cap + tau. */
    RenderParams on;
    make_params(&on, 1, 0, 0.0);
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &on, adap) == 0,
          "adaptive render succeeds");
    /* High-spp fixed reference: the "ground truth" the sampler converges to. */
    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, REF_SPP, DEPTH, ref) == 0,
          "high-spp reference render succeeds");

    double err_fixed = mean_abs_error(fixed, ref);
    double err_adap  = mean_abs_error(adap,  ref);

    printf("integration_adaptive: MAE vs %d-spp ref: fixed(n0=%d)=%.4f "
           "adaptive=%.4f\n", REF_SPP, BASE_SPP, err_fixed, err_adap);

    /* CONVERGENCE (primary): adaptive is strictly closer to the reference. */
    CHECK(err_adap < err_fixed,
          "adaptive result is closer to the high-spp reference than fixed-n0");
    /* And by a clear margin (the extra samples buy real accuracy). */
    CHECK(err_adap < err_fixed * 0.95,
          "adaptive reduces the reference error by a clear margin (>5%)");

    free(fixed); free(adap); free(ref);
    scene_free(&scene);
    scene_desc_free(&desc);
}

/*
 * ------------------------------------------------------------------ */
/* (a) Convergence: accuracy improves with budget, with diminishing    */
/*     returns (the Monte-Carlo 1/sqrt(N) property).                   */
/* ------------------------------------------------------------------ */

static void test_adaptive_convergence_budget_e2e(void)
{
    SceneDesc desc;
    Scene scene;
    Camera cam;
    if (build_scene_from_desc(CONTRAST_SCENE, "<adaptive-sat>", &desc, &scene,
                              &cam) != 0) {
        CHECK(0, "high-contrast scene parses + builds via scene_desc");
        return;
    }

    unsigned char *c1 = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *c2 = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *fixed = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *ref = (unsigned char *)malloc(BUF_BYTES);
    if (c1 == NULL || c2 == NULL || fixed == NULL || ref == NULL) {
        free(c1); free(c2); free(fixed); free(ref);
        scene_free(&scene); scene_desc_free(&desc);
        CHECK(0, "buffers allocated");
        return;
    }

    /* Same base spp and same tau (default 0.02); only the cap is raised. */
    RenderParams lo;
    make_params(&lo, 1, DEF_MULT * BASE_SPP, 0.0);  /* cap = 4*n0  (default) */
    RenderParams hi;
    make_params(&hi, 1, BIG_MULT * BASE_SPP, 0.0);  /* cap = 16*n0 (raised)  */

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &lo, c1) == 0,
          "adaptive render at the default cap succeeds");
    unsigned long long total_lo = render_last_total_samples();

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &hi, c2) == 0,
          "adaptive render at the raised cap succeeds");
    unsigned long long total_hi = render_last_total_samples();

    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, BASE_SPP, DEPTH, fixed) == 0,
          "low-spp fixed render succeeds");
    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, REF_SPP, DEPTH, ref) == 0,
          "high-spp reference render succeeds");

    double err_fixed = mean_abs_error(fixed, ref); /* low-spp fixed vs ref   */
    double err_lo    = mean_abs_error(c1,    ref); /* default cap vs ref     */
    double err_hi    = mean_abs_error(c2,    ref); /* raised cap vs ref      */

    printf("integration_adaptive: convergence total: base=%llu cap4=%llu "
           "cap16=%llu; MAE vs %d-spp ref: fixed(n0=%d)=%.4f cap4=%.4f "
           "cap16=%.4f\n",
           BASE_TOTAL, total_lo, total_hi, REF_SPP, BASE_SPP,
           err_fixed, err_lo, err_hi);

    /* Sample budget is monotone in the cap, and the raised cap is respected. */
    CHECK(total_hi >= total_lo,
          "raising the cap never reduces the sample budget");
    CHECK(total_hi <=
              (unsigned long long)NPIXELS *
                  (unsigned long long)(BIG_MULT * BASE_SPP),
          "raised-cap total respects N_max = 16*n0");

    /* CONVERGENCE (monotone): every larger sample budget moves the image
     * strictly closer to the high-spp reference. */
    CHECK(err_lo < err_fixed,
          "adaptive(default cap) is closer to the reference than low-spp fixed");
    CHECK(err_hi < err_lo,
          "adaptive(raised cap) is even closer to the reference (monotone)");

    /* CONVERGENCE (diminishing returns): the estimator behaves like a
     * Monte-Carlo mean, whose error decays ~1/sqrt(N). The FIRST refinement
     * step (n0 -> 4*n0) removes MORE error while spending FEWER extra samples
     * than the SECOND step (4*n0 -> 16*n0). This is the robust "increasing the
     * cap further changes the image only marginally" statement: it is a RATIO
     * of error reductions vs a RATIO of budgets, so it is stable across
     * compilers / libm / architectures, unlike an absolute MAE threshold. */
    double gain_step1 = err_fixed - err_lo;   /* n0   -> cap 4*n0        */
    double gain_step2 = err_lo    - err_hi;   /* cap4 -> cap 16*n0       */
    unsigned long long cost1 = total_lo - BASE_TOTAL;   /* extra for step 1 */
    unsigned long long cost2 = total_hi - total_lo;     /* extra for step 2 */
    CHECK(cost2 > cost1,
          "the second refinement step spends more extra samples than the first");
    CHECK(gain_step1 > gain_step2,
          "diminishing returns: step 1 removes more error for fewer samples");

    free(c1); free(c2); free(fixed); free(ref);
    scene_free(&scene);
    scene_desc_free(&desc);
}

/* ------------------------------------------------------------------ */
/* (b) DEFAULT (no-flag) render stays byte-identical + deterministic   */
/* ------------------------------------------------------------------ */

static void test_default_byte_identical_e2e(void)
{
    SceneDesc desc;
    Scene scene;
    Camera cam;
    if (build_scene_from_desc(DEFAULT_SCENE, "<adaptive-default>", &desc, &scene,
                              &cam) != 0) {
        CHECK(0, "default scene parses + builds via scene_desc");
        return;
    }

    unsigned char *fixed_a = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *fixed_b = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *ex_off  = (unsigned char *)malloc(BUF_BYTES);
    if (fixed_a == NULL || fixed_b == NULL || ex_off == NULL) {
        free(fixed_a); free(fixed_b); free(ex_off);
        scene_free(&scene); scene_desc_free(&desc);
        CHECK(0, "buffers allocated");
        return;
    }

    /* The plain render_image() path, twice: must be reproducible. */
    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, BASE_SPP, DEPTH, fixed_a) == 0,
          "render_image #1 succeeds on the default scene");
    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, BASE_SPP, DEPTH, fixed_b) == 0,
          "render_image #2 succeeds on the default scene");
    CHECK(count_diff(fixed_a, fixed_b) == 0,
          "two identical default renders are BYTE-IDENTICAL (deterministic)");

    /* The opt-in entry point with adaptive OFF (== the no-flag default). */
    RenderParams off;
    make_params(&off, 0, 0, 0.0);
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &off, ex_off) == 0,
          "render_image_ex(adaptive=0) succeeds on the default scene");
    CHECK(count_diff(fixed_a, ex_off) == 0,
          "adaptive=0 is BYTE-IDENTICAL to render_image (default unaffected)");

    /* The default (no-flag) render must not be perturbed by the diagnostics. */
    CHECK(render_last_total_samples() == BASE_TOTAL,
          "adaptive=0 reports the fixed n0 sample budget");
    CHECK(render_last_max_samples() == BASE_SPP,
          "adaptive=0 reports n0 as the max per-pixel count");

    /* A second adaptive=0 render is still byte-identical (no hidden state). */
    {
        unsigned char *ex_off2 = (unsigned char *)malloc(BUF_BYTES);
        if (ex_off2 != NULL) {
            CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &off, ex_off2) == 0,
                  "second adaptive=0 render succeeds");
            CHECK(count_diff(ex_off, ex_off2) == 0,
                  "adaptive=0 renders are byte-identical across repeats");
            free(ex_off2);
        }
    }

    printf("integration_adaptive: default scene fixed==adaptive=0 "
           "(byte-identical), total=%llu\n", render_last_total_samples());

    free(fixed_a); free(fixed_b); free(ex_off);
    scene_free(&scene);
    scene_desc_free(&desc);
}

/* ------------------------------------------------------------------ */
/* (a/b) Determinism of the adaptive path end-to-end                   */
/* ------------------------------------------------------------------ */

static void test_adaptive_determinism_e2e(void)
{
    SceneDesc desc;
    Scene scene;
    Camera cam;
    if (build_scene_from_desc(CONTRAST_SCENE, "<adaptive-det>", &desc, &scene,
                              &cam) != 0) {
        CHECK(0, "high-contrast scene parses + builds via scene_desc");
        return;
    }

    unsigned char *a = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *b = (unsigned char *)malloc(BUF_BYTES);
    if (a == NULL || b == NULL) {
        free(a); free(b); scene_free(&scene); scene_desc_free(&desc);
        CHECK(0, "buffers allocated");
        return;
    }

    RenderParams on;
    make_params(&on, 1, 0, 0.0);

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &on, a) == 0,
          "adaptive render #1 succeeds");
    unsigned long long t1 = render_last_total_samples();
    int m1 = render_last_max_samples();

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &on, b) == 0,
          "adaptive render #2 succeeds");
    unsigned long long t2 = render_last_total_samples();
    int m2 = render_last_max_samples();

    CHECK(count_diff(a, b) == 0,
          "two adaptive renders are BYTE-IDENTICAL (deterministic)");
    CHECK(t1 == t2, "adaptive total sample counts are reproducible");
    CHECK(m1 == m2, "adaptive max per-pixel counts are reproducible");

    printf("integration_adaptive: adaptive determinism total=%llu max/px=%d "
           "(x2)\n", t1, m1);

    free(a); free(b);
    scene_free(&scene);
    scene_desc_free(&desc);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    test_adaptive_engages_e2e();
    test_adaptive_convergence_e2e();
    test_adaptive_convergence_budget_e2e();
    test_default_byte_identical_e2e();
    test_adaptive_determinism_e2e();

    printf("tests/test_integration_adaptive: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}
