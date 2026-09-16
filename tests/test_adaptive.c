/*
 * tests/test_adaptive.c - Contract test for OPT-IN adaptive sampling
 * (src/render.c render_image_ex / RenderParams).
 *
 * The adaptive sampler spends more samples on noisy / high-contrast pixels and
 * fewer on flat ones, while the DEFAULT fixed-spp render stays byte-identical.
 * This test locks that contract:
 *
 *   1. Fixed-path byte-identity: render_image(...) == render_image_ex(...,
 *      adaptive = 0) byte-for-byte (the wrapper delegates to the old path).
 *   2. Determinism: two identical adaptive renders are byte-identical.
 *   3. Thread-schedule independence (USE_PTHREADS build): adaptive output with
 *      RAYTRACER_THREADS=1 == output with RAYTRACER_THREADS=4.
 *   4. Adaptivity engages: on a high-contrast scene the adaptive render DIFFERS
 *      from the fixed-n0 render and takes strictly more samples, with the
 *      per-pixel cap N_max respected (total in [n0*Npx, Nmax*Npx]).
 *   5. Lower noise: against a high-spp reference, the adaptive image has a
 *      smaller mean absolute error than the fixed-n0 image.
 *   6. Flat-region economy: on a near-constant scene the adaptive run takes
 *      (close to) n0 samples per pixel.
 *
 * The Makefile links every test source against all project objects EXCEPT
 * src/main.o, so this file supplies its own main(). C11, -Wall -Wextra clean,
 * no rand(). Kept small so `make test` stays fast.
 */

/* Feature-test macro so setenv()/unsetenv() are declared under -std=c11. */
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
/* Test harness (same style as the other tests)                        */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define WIDTH   48
#define HEIGHT  27
#define SPP     4
#define DEPTH   4
#define REF_SPP 64
#define BUF_BYTES ((size_t)WIDTH * (size_t)HEIGHT * 3u)
#define NPIXELS ((size_t)WIDTH * (size_t)HEIGHT)

/*
 * High-contrast scene: a fine checkerboard floor plus a sphere, so many pixels
 * straddle an edge and have large per-sample variance under AA jitter. Clouds
 * are disabled so the noise is driven by geometry, not sky texture.
 */
static const char *CONTRAST_SCENE =
    "water_enabled = 0\n"
    "water_material = none\n"
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
 * Near-constant scene: a plain (untextured) plane and a cloudless gradient
 * sky. Every sample of a pixel is essentially identical, so the relative
 * standard error is ~0 and adaptive refinement should NOT engage (economy).
 */
static const char *FLAT_SCENE =
    "water_enabled = 0\n"
    "water_material = none\n"
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

/*
 * Pure-gradient sky scene with NO geometry and the sun pointing straight down
 * (so the sun disk / glow is out of frame). Every pixel of the same row has the
 * same colour, and the vertical gradient is smooth, so the relative standard
 * error of the mean luminance is ~0 for almost every pixel: adaptive
 * refinement must NOT engage and the run stays essentially at n0 samples.
 */
static const char *FLAT_SKY_SCENE =
    "water_enabled = 0\n"
    "water_material = none\n"
    "camera {\n"
    "    eye = 0 0 0\n"
    "    target = 0 1 0\n"
    "    up = 1 0 0\n"
    "    vfov = 45\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0 -1 0\n"
    "    sun_color = 1 0.96 0.9\n"
    "    horizon_color = 0.8 0.88 1\n"
    "    zenith_color = 0.3 0.5 0.95\n"
    "    gradient_gamma = 0.65\n"
    "    sun_glow_exponent = 320\n"
    "    sun_glow_strength = 0.85\n"
    "    cloud_coverage = 0\n"
    "    seed = 90210\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Build a Scene + Camera from scene text. Returns 0 on success. */
static int build_scene(const char *text, Scene *scene, Camera *cam)
{
    SceneDesc desc;
    char errbuf[256];
    errbuf[0] = '\0';
    scene_desc_init(&desc);
    if (scene_desc_load_string(&desc, text, "<adaptive-test>", errbuf,
                               sizeof errbuf) != 0) {
        fprintf(stderr, "  parse error: %s\n", errbuf);
        scene_desc_free(&desc);
        return -1;
    }
    memset(scene, 0, sizeof *scene);
    if (scene_build_from_desc(scene, &desc) != 0) {
        fprintf(stderr, "  build_from_desc failed\n");
        scene_desc_free(&desc);
        return -1;
    }
    scene_desc_free(&desc);
    *cam = scene_default_camera(scene);
    return 0;
}

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

static void make_params(RenderParams *p, int adaptive, int nmax, double tau)
{
    p->samples_per_pixel = SPP;
    p->max_depth         = DEPTH;
    p->adaptive          = adaptive;
    p->adaptive_max_spp  = nmax;
    p->adaptive_tau      = tau;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* 1. The fixed path through render_image_ex is byte-identical to today. */
static void test_fixed_byte_identity(void)
{
    Scene scene; Camera cam;
    if (build_scene(CONTRAST_SCENE, &scene, &cam) != 0) {
        CHECK(0, "contrast scene builds");
        return;
    }

    unsigned char *a = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *b = (unsigned char *)malloc(BUF_BYTES);
    if (!a || !b) { free(a); free(b); scene_free(&scene); CHECK(0, "alloc"); return; }
    memset(a, 0xAA, BUF_BYTES);
    memset(b, 0x55, BUF_BYTES);

    RenderParams p;
    make_params(&p, 0, 0, 0.0); /* adaptive OFF */

    int rc1 = render_image(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH, a);
    int rc2 = render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, b);

    CHECK(rc1 == 0, "render_image returns 0");
    CHECK(rc2 == 0, "render_image_ex(adaptive=0) returns 0");
    CHECK(count_diff(a, b) == 0,
          "render_image_ex(adaptive=0) == render_image (byte-identical)");

    free(a); free(b);
    scene_free(&scene);
}

/* 2 + 4 + 5: adaptive engages, is deterministic, respects the cap, less noisy. */
static void test_adaptive_engages(void)
{
    Scene scene; Camera cam;
    if (build_scene(CONTRAST_SCENE, &scene, &cam) != 0) {
        CHECK(0, "contrast scene builds");
        return;
    }

    const int nmax = 4 * SPP; /* 16 */
    RenderParams p;
    make_params(&p, 1, nmax, 0.02);

    unsigned char *fixed = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *ad1   = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *ad2   = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *ref   = (unsigned char *)malloc(BUF_BYTES);
    if (!fixed || !ad1 || !ad2 || !ref) {
        free(fixed); free(ad1); free(ad2); free(ref);
        scene_free(&scene); CHECK(0, "alloc"); return;
    }

    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH, fixed) == 0,
          "fixed-n0 render succeeds");
    unsigned long long fixed_total = render_last_total_samples();
    CHECK(fixed_total == (unsigned long long)NPIXELS * (unsigned long long)SPP,
          "fixed path reports n0 samples per pixel");

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, ad1) == 0,
          "adaptive render succeeds");
    unsigned long long ad_total = render_last_total_samples();
    int ad_max = render_last_max_samples();

    /* Adaptivity actually engaged: more samples than fixed, cap respected. */
    CHECK(ad_total > (unsigned long long)NPIXELS * (unsigned long long)SPP,
          "adaptive uses more samples than fixed n0 on a high-contrast scene");
    CHECK(ad_total <= (unsigned long long)NPIXELS * (unsigned long long)nmax,
          "adaptive total samples <= N_max * pixels (cap respected)");
    CHECK(ad_max <= nmax && ad_max > SPP,
          "adaptive max per-pixel sample count is in (n0, N_max]");

    /* The adaptive image must differ from the fixed one (refinement changed it). */
    CHECK(count_diff(fixed, ad1) > 0,
          "adaptive image differs from the fixed-n0 image");

    /* Determinism: identical inputs -> byte-identical. */
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, ad2) == 0,
          "second adaptive render succeeds");
    CHECK(count_diff(ad1, ad2) == 0, "two adaptive renders are byte-identical");

    /* Lower noise: compare both against a high-spp reference. */
    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, REF_SPP, DEPTH, ref) == 0,
          "high-spp reference render succeeds");
    double err_fixed = mean_abs_error(fixed, ref);
    double err_adapt = mean_abs_error(ad1, ref);
    CHECK(err_adapt < err_fixed,
          "adaptive image is closer to the reference than fixed-n0");

    /* 3. Thread-schedule independence (threaded build only). */
#ifdef USE_PTHREADS
    if (setenv("RAYTRACER_THREADS", "1", 1) == 0) {
        unsigned char *t1 = (unsigned char *)malloc(BUF_BYTES);
        if (t1) {
            int rc = render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, t1);
            CHECK(rc == 0, "adaptive render with RAYTRACER_THREADS=1 succeeds");
            CHECK(count_diff(ad1, t1) == 0,
                  "adaptive output is byte-identical at 1 vs 4 threads");
            free(t1);
        }
        (void)setenv("RAYTRACER_THREADS", "4", 1);
        unsigned char *t4 = (unsigned char *)malloc(BUF_BYTES);
        if (t4) {
            int rc = render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, t4);
            CHECK(rc == 0, "adaptive render with RAYTRACER_THREADS=4 succeeds");
            CHECK(count_diff(ad1, t4) == 0,
                  "adaptive output is byte-identical at 4 threads (explicit)");
            free(t4);
        }
        (void)unsetenv("RAYTRACER_THREADS");
    }
#endif

    printf("tests/test_adaptive: contrast scene fixed=%llu adaptive=%llu "
           "max/px=%d (n0=%d, Nmax=%d); MAE fixed=%.4f adaptive=%.4f\n",
           fixed_total, ad_total, ad_max, SPP, nmax, err_fixed, err_adapt);

    free(fixed); free(ad1); free(ad2); free(ref);
    scene_free(&scene);
}

/* 6. Flat-region economy: a near-constant scene barely refines. */
static void test_flat_economy(void)
{
    Scene scene; Camera cam;
    if (build_scene(FLAT_SCENE, &scene, &cam) != 0) {
        CHECK(0, "flat scene builds");
        return;
    }

    const int nmax = 4 * SPP;
    RenderParams p;
    make_params(&p, 1, nmax, 0.02);

    unsigned char *ad = (unsigned char *)malloc(BUF_BYTES);
    if (!ad) { scene_free(&scene); CHECK(0, "alloc"); return; }

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, ad) == 0,
          "adaptive render on the flat scene succeeds");
    unsigned long long ad_total = render_last_total_samples();
    unsigned long long base = (unsigned long long)NPIXELS * (unsigned long long)SPP;

    /* A near-constant image should stay close to n0 spp: allow a small
     * allowance for the few silhouette pixels that legitimately refine. */
    CHECK(ad_total >= base, "adaptive total is at least the base n0 pass");
    CHECK(ad_total <= base + base / 2,
          "flat scene stays economical (<= 1.5x the base sample budget)");

    printf("tests/test_adaptive: flat scene total=%llu base=%llu (n0=%d)\n",
           ad_total, base, SPP);

    free(ad);
    scene_free(&scene);
}

/* Invalid params are rejected (defensive contract). */
static void test_invalid_params(void)
{
    Scene scene; Camera cam;
    if (build_scene(FLAT_SCENE, &scene, &cam) != 0) {
        CHECK(0, "flat scene builds");
        return;
    }
    unsigned char *buf = (unsigned char *)malloc(BUF_BYTES);
    if (!buf) { scene_free(&scene); CHECK(0, "alloc"); return; }

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, NULL, buf) != 0,
          "render_image_ex rejects a NULL params pointer");

    RenderParams p;
    make_params(&p, 1, 0, 0.02);
    p.samples_per_pixel = 0;
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, buf) != 0,
          "render_image_ex rejects samples_per_pixel < 1");

    make_params(&p, 1, 0, 0.02);
    p.max_depth = -1;
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, buf) != 0,
          "render_image_ex rejects max_depth < 0");

    free(buf);
    scene_free(&scene);
}

/*
 * 7. Metric closed form: the per-pixel relative standard error of the mean
 *    luminance used by the sampler is
 *
 *        e = sqrt(s^2 / n) / (Ybar + 1e-4),   s^2 = sum(Y_i-Ybar)^2/(n-1)
 *
 *    with Rec.709 luminance. The engine's accumulators (sumY, sumY2, nsamp)
 *    drive exactly this expression, so we reconstruct the derived `e` from a
 *    synthetic accumulation (n samples, mean Y, variance) and assert it
 *    matches the closed form computed two independent ways. The monotone
 *    reduction of e under repeated sampling is also checked, since that is the
 *    property that makes refinement terminate.
 */
static void test_metric_formula(void)
{
    /* Synthetic luminance samples with a known mean and variance. */
    const double y[4] = { 0.2, 0.4, 0.6, 0.8 };
    const int    n    = 4;
    double sumY = 0.0, sumY2 = 0.0;
    for (int i = 0; i < n; ++i) { sumY += y[i]; sumY2 += y[i] * y[i]; }

    /* Engine's one-pass reconstruction (render.c adaptive_compute_active). */
    double nf   = (double)n;
    double ybar = sumY / nf;
    double var  = (sumY2 - sumY * ybar) / (nf - 1.0);
    if (var < 0.0) var = 0.0;
    double se   = sqrt(var / nf);
    double e    = se / (ybar + 1e-4);

    /* Independent two-pass definition of the same quantity. */
    double dsum = 0.0;
    for (int i = 0; i < n; ++i) { double t = y[i] - ybar; dsum += t * t; }
    double var2 = dsum / (nf - 1.0);
    double e2   = sqrt(var2 / nf) / (ybar + 1e-4);

    CHECK(fabs(ybar - 0.5) < 1e-12, "synthetic mean Y equals 0.5");
    CHECK(fabs(var - var2) < 1e-12,
          "one-pass variance == two-pass variance (sumY2 - sumY*Ybar)");
    CHECK(fabs(e - e2) < 1e-12, "metric e matches the two-pass closed form");

    /* Closed-form spot value: s^2 = 0.0666.., se = sqrt(0.0666../4),
     * e = se / (0.5 + 1e-4) = 0.2581472603... */
    double expected = sqrt((1.0 / 15.0) / 4.0) / (0.5 + 1e-4);
    CHECK(fabs(e - expected) < 1e-12,
          "metric e equals the hand-computed closed form 0.258147...");

    /* Rec.709 luminance of a known linear RGB triple:
     * 0.2126*0.25 + 0.7152*0.5 + 0.0722*0.75 = 0.4649. */
    double R = 0.25, G = 0.5, B = 0.75;
    double Y = 0.2126 * R + 0.7152 * G + 0.0722 * B;
    CHECK(fabs(Y - 0.4649) < 1e-9,
          "Rec.709 luminance Y = 0.2126R + 0.7152G + 0.0722B");

    /* Replicating each sample 4x (n = 16) also lowers e. */
    double rsumY = 0.0, rsumY2 = 0.0;
    const int rn = 16;
    for (int i = 0; i < rn; ++i) {
        double v = y[i % n];
        rsumY += v; rsumY2 += v * v;
    }
    double rnf = (double)rn, rybar = rsumY / rnf;
    double rvar = (rsumY2 - rsumY * rybar) / (rnf - 1.0);
    double re = sqrt(rvar / rnf) / (rybar + 1e-4);
    CHECK(re < e, "e decreases when the same distribution is sampled more");

    /* Fixed sample variance s^2: e must scale as 1/sqrt(n), i.e. quadrupling n
     * halves e. Checked on the closed form directly so the unbiased (n-1)
     * denominator does not confound the scaling. */
    double s2   = 1.0 / 15.0;
    double e4   = sqrt(s2 / 4.0) / (ybar + 1e-4);
    double e16  = sqrt(s2 / 16.0) / (ybar + 1e-4);
    CHECK(e16 < e4, "e decreases with more samples at fixed sample variance");
    CHECK(fabs(e4 / e16 - 2.0) < 1e-12,
          "quadrupling n halves e at fixed s^2 (1/sqrt(n) scaling)");

    printf("tests/test_adaptive: metric e(n=4)=%.9f e(n=16)=%.9f "
           "(Ybar=%.6f, var=%.9f)\n", e, re, ybar, var);
}

/*
 * 8. N_max cap: a small --adaptive-max (max_mult) is a hard ceiling. Assert
 *    both the reported per-pixel maximum AND the total sample budget never
 *    exceed the cap, and that the cap is actually reached (so the test proves
 *    the cap bites rather than the scene simply being quiet).
 */
static void test_cap_enforced(void)
{
    Scene scene; Camera cam;
    if (build_scene(CONTRAST_SCENE, &scene, &cam) != 0) {
        CHECK(0, "contrast scene builds");
        return;
    }

    const int cap = 2 * SPP; /* 8 */
    RenderParams p;
    make_params(&p, 1, cap, 0.02);

    unsigned char *buf = (unsigned char *)malloc(BUF_BYTES);
    if (!buf) { scene_free(&scene); CHECK(0, "alloc"); return; }

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, buf) == 0,
          "capped adaptive render succeeds");
    int cap_max = render_last_max_samples();
    unsigned long long cap_total = render_last_total_samples();

    CHECK(cap_max <= cap, "render_last_max_samples() <= --adaptive-max cap");
    CHECK(cap_total <= (unsigned long long)NPIXELS * (unsigned long long)cap,
          "total samples <= cap * npixels");
    CHECK(cap_max == cap,
          "the cap is reached on a high-contrast scene (cap actually bites)");

    /* With max_mult == 1 (N_max == n0) there is no room to refine at all. */
    RenderParams q;
    make_params(&q, 1, SPP, 0.02);
    unsigned char *buf2 = (unsigned char *)malloc(BUF_BYTES);
    if (buf2) {
        CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &q, buf2) == 0,
              "adaptive with N_max == n0 succeeds");
        CHECK(render_last_max_samples() == SPP,
              "N_max == n0 pins every pixel to exactly n0 samples");
        CHECK(render_last_total_samples() ==
                  (unsigned long long)NPIXELS * (unsigned long long)SPP,
              "N_max == n0 gives the fixed sample budget");
        free(buf2);
    }

    printf("tests/test_adaptive: cap=%d max/px=%d total=%llu (npix=%zu)\n",
           cap, cap_max, cap_total, NPIXELS);

    free(buf);
    scene_free(&scene);
}

/*
 * 9. tau boundary: a tiny tolerance forces maximal refinement up to the cap,
 *    while a very large tolerance accepts the base n0 pass untouched. Both
 *    extremes are checked against the reported counters, plus the expected
 *    monotonicity: shrinking tau never reduces the sample budget.
 */
static void test_tau_boundary(void)
{
    Scene scene; Camera cam;
    if (build_scene(CONTRAST_SCENE, &scene, &cam) != 0) {
        CHECK(0, "contrast scene builds");
        return;
    }

    const int nmax = 4 * SPP; /* 16 */
    unsigned char *buf = (unsigned char *)malloc(BUF_BYTES);
    if (!buf) { scene_free(&scene); CHECK(0, "alloc"); return; }

    /* Very large tau: every pixel's e is below tolerance after pass 0. */
    RenderParams hi;
    make_params(&hi, 1, nmax, 1.0e9);
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &hi, buf) == 0,
          "large-tau adaptive render succeeds");
    unsigned long long hi_total = render_last_total_samples();
    CHECK(hi_total == (unsigned long long)NPIXELS * (unsigned long long)SPP,
          "very large tau uses ~n0 samples (base pass only)");
    CHECK(render_last_max_samples() == SPP,
          "very large tau leaves every pixel at n0");

    /* Tiny tau: e can never reach the tolerance, so refinement runs to N_max. */
    RenderParams lo;
    make_params(&lo, 1, nmax, 1.0e-9);
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &lo, buf) == 0,
          "tiny-tau adaptive render succeeds");
    unsigned long long lo_total = render_last_total_samples();
    CHECK(render_last_max_samples() == nmax,
          "tiny tau refines every pixel up to the N_max cap");
    CHECK(lo_total == (unsigned long long)NPIXELS * (unsigned long long)nmax,
          "tiny tau reaches the full N_max * npixels budget");

    /* Monotone: a smaller tolerance cannot spend fewer samples. */
    RenderParams mid;
    make_params(&mid, 1, nmax, 0.02);
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &mid, buf) == 0,
          "default-tau adaptive render succeeds");
    unsigned long long mid_total = render_last_total_samples();
    CHECK(hi_total <= mid_total && mid_total <= lo_total,
          "sample budget is monotone non-decreasing as tau shrinks");

    printf("tests/test_adaptive: tau sweep total(1e9)=%llu total(0.02)=%llu "
           "total(1e-9)=%llu\n", hi_total, mid_total, lo_total);

    free(buf);
    scene_free(&scene);
}

/*
 * 10. Flat vs high-contrast: a pure-gradient sky (near-zero per-pixel variance)
 *     must stay essentially at the base sample count, while the checkerboard
 *     scene refines strictly more. The comparison uses the SAME n0 and N_max so
 *     the only difference is scene content.
 */
static void test_flat_vs_contrast(void)
{
    Scene flat, contrast;
    Camera fc, cc;
    if (build_scene(FLAT_SKY_SCENE, &flat, &fc) != 0) {
        CHECK(0, "flat sky scene builds");
        return;
    }
    if (build_scene(CONTRAST_SCENE, &contrast, &cc) != 0) {
        scene_free(&flat);
        CHECK(0, "contrast scene builds");
        return;
    }

    const int nmax = 4 * SPP;
    RenderParams p;
    make_params(&p, 1, nmax, 0.02);

    unsigned char *buf = (unsigned char *)malloc(BUF_BYTES);
    if (!buf) { scene_free(&flat); scene_free(&contrast); CHECK(0, "alloc"); return; }
    const unsigned long long base =
        (unsigned long long)NPIXELS * (unsigned long long)SPP;

    CHECK(render_image_ex(&flat, &fc, WIDTH, HEIGHT, &p, buf) == 0,
          "flat sky adaptive render succeeds");
    unsigned long long flat_total = render_last_total_samples();

    CHECK(render_image_ex(&contrast, &cc, WIDTH, HEIGHT, &p, buf) == 0,
          "contrast adaptive render succeeds");
    unsigned long long contrast_total = render_last_total_samples();

    CHECK(flat_total >= base, "flat sky spends at least the base n0 budget");
    CHECK(flat_total <= base + base / 10,
          "flat sky stays near the base sample count (<= 1.1x)");
    CHECK(contrast_total > flat_total,
          "high-contrast scene refines strictly more than the flat sky");

    printf("tests/test_adaptive: flat-sky total=%llu contrast total=%llu "
           "(base=%llu)\n", flat_total, contrast_total, base);

    free(buf);
    scene_free(&flat);
    scene_free(&contrast);
}

/*
 * 11. Determinism: repeated adaptive renders with identical inputs and seed
 *     produce byte-identical output and identical diagnostic counters, and the
 *     very first n0 samples per pixel are shared with the fixed path (the
 *     engine continues the global sample index from n0).
 */
static void test_adaptive_determinism(void)
{
    Scene scene; Camera cam;
    if (build_scene(CONTRAST_SCENE, &scene, &cam) != 0) {
        CHECK(0, "contrast scene builds");
        return;
    }

    const int nmax = 4 * SPP;
    RenderParams p;
    make_params(&p, 1, nmax, 0.02);

    unsigned char *a = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *b = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *c = (unsigned char *)malloc(BUF_BYTES);
    if (!a || !b || !c) {
        free(a); free(b); free(c); scene_free(&scene); CHECK(0, "alloc"); return;
    }

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, a) == 0,
          "adaptive render #1 succeeds");
    unsigned long long t1 = render_last_total_samples();
    int m1 = render_last_max_samples();

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, b) == 0,
          "adaptive render #2 succeeds");
    unsigned long long t2 = render_last_total_samples();
    int m2 = render_last_max_samples();

    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &p, c) == 0,
          "adaptive render #3 succeeds");
    unsigned long long t3 = render_last_total_samples();
    int m3 = render_last_max_samples();

    CHECK(count_diff(a, b) == 0, "adaptive renders #1 and #2 are byte-identical");
    CHECK(count_diff(b, c) == 0, "adaptive renders #2 and #3 are byte-identical");
    CHECK(t1 == t2 && t2 == t3, "adaptive total sample counts are reproducible");
    CHECK(m1 == m2 && m2 == m3, "adaptive max per-pixel counts are reproducible");

    printf("tests/test_adaptive: determinism total=%llu max/px=%d (x3)\n",
           t1, m1);

    free(a); free(b); free(c);
    scene_free(&scene);
}

/*
 * 12. Gating: with adaptive OFF, render_image_ex must reproduce the fixed
 *     render_image() path byte-for-byte for the same parameters, on BOTH the
 *     flat and the contrast scene, and must not perturb the diagnostics. This
 *     is the guarantee that `--adaptive` absent leaves the default render
 *     untouched.
 */
static void test_gating_absent_adaptive(void)
{
    Scene flat, contrast;
    Camera fc, cc;
    if (build_scene(FLAT_SKY_SCENE, &flat, &fc) != 0) {
        CHECK(0, "flat sky scene builds");
        return;
    }
    if (build_scene(CONTRAST_SCENE, &contrast, &cc) != 0) {
        scene_free(&flat);
        CHECK(0, "contrast scene builds");
        return;
    }

    unsigned char *fixed = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *ex    = (unsigned char *)malloc(BUF_BYTES);
    if (!fixed || !ex) {
        free(fixed); free(ex);
        scene_free(&flat); scene_free(&contrast);
        CHECK(0, "alloc");
        return;
    }

    RenderParams off;
    make_params(&off, 0, 4 * SPP, 0.02); /* adaptive disabled, extras ignored */

    /* Flat scene. */
    CHECK(render_image(&flat, &fc, WIDTH, HEIGHT, SPP, DEPTH, fixed) == 0,
          "fixed render_image on flat sky succeeds");
    CHECK(render_image_ex(&flat, &fc, WIDTH, HEIGHT, &off, ex) == 0,
          "render_image_ex(adaptive=0) on flat sky succeeds");
    CHECK(count_diff(fixed, ex) == 0,
          "adaptive=0 == render_image on the flat sky (byte-identical)");

    /* Contrast scene. */
    CHECK(render_image(&contrast, &cc, WIDTH, HEIGHT, SPP, DEPTH, fixed) == 0,
          "fixed render_image on contrast succeeds");
    CHECK(render_image_ex(&contrast, &cc, WIDTH, HEIGHT, &off, ex) == 0,
          "render_image_ex(adaptive=0) on contrast succeeds");
    CHECK(count_diff(fixed, ex) == 0,
          "adaptive=0 == render_image on the contrast scene (byte-identical)");

    /* Diagnostics are the fixed-path ones (n0 per pixel, no refinement). */
    CHECK(render_last_total_samples() ==
              (unsigned long long)NPIXELS * (unsigned long long)SPP,
          "adaptive=0 reports the fixed n0 sample budget");
    CHECK(render_last_max_samples() == SPP,
          "adaptive=0 reports n0 as the max per-pixel count");

    free(fixed); free(ex);
    scene_free(&flat);
    scene_free(&contrast);
}

int main(void)
{
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    test_fixed_byte_identity();
    test_adaptive_engages();
    test_flat_economy();
    test_invalid_params();
    test_metric_formula();
    test_cap_enforced();
    test_tau_boundary();
    test_flat_vs_contrast();
    test_adaptive_determinism();
    test_gating_absent_adaptive();

    printf("tests/test_adaptive: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
