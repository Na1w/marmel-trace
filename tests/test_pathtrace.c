/*
 * tests/test_pathtrace.c - Focused, fast BEHAVIOURAL tests for the opt-in
 *                          unbiased path tracer (src/pathtrace.{h,c}).
 *
 * Unlike tests/test_pathtrace_sampling.c (estimator primitives) and
 * tests/test_integration_pathtrace.c (Whitted-vs-pathtrace byte-identity),
 * this file locks the CORE CONTRACTS of the estimator itself:
 *
 *   1. DETERMINISM
 *        - pathtrace_radiance(scene, ray, depth, seed) called twice with the
 *          SAME triple is bit-identical (pure w.r.t. the scene),
 *        - pathtrace_render(...) called twice with the same args is
 *          byte-identical,
 *        - (under -DUSE_PTHREADS) RAYTRACER_THREADS=1 vs =4 is byte-identical.
 *   2. SEED SENSITIVITY: a different seed_key yields a different result
 *      (the Monte-Carlo stream really depends on the key).
 *   3. FURNACE / ENERGY CONSERVATION: under a uniform bright sky (no sun) a
 *      pure-white Lambertian ground never reflects more than the incident sky
 *      radiance; every radiance sample is finite and within [0, K].
 *   4. NEE DIRECT LIGHTING: a ray hitting diffuse geometry under a bright sun
 *      returns a finite, positive value, and occluding the sun with a big
 *      blocking sphere measurably DROPS the mean radiance over a ray set.
 *   5. CONVERGENCE WITH spp: raising the sample count keeps the image valid
 *      and (loosely, with generous tolerance) shrinks the change between
 *      successive renders (a monotone-ish noise check).
 *   6. ARGUMENT VALIDATION + DEFAULT-SCENE STABILITY: pathtrace_render rejects
 *      NULL / bad arguments with the documented non-zero codes, and the
 *      built-in default scene renders deterministically and reproducibly.
 *
 * The Makefile links every C source in tests/ against all project objects
 * except src/main.o, so this file supplies its own int main(void). C11, -Wall
 * -Wextra
 * clean, fully deterministic (a small LCG, NEVER rand(), never the clock), and
 * kept SMALL (64x36, few spp) so `make test` stays fast (well under a second).
 */

/* Feature-test macro so setenv()/unsetenv() are declared under -std=c11. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "pathtrace.h"
#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "material.h"
#include "vec3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness (same convention as the other tests)           */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* Deterministic 64-bit LCG mapped to [0, 1). Kept for any jitter we need;
 * the path tracer itself owns its own hashed PRNG. */
static unsigned long long g_rng = 0xD1B54A32D192ED03ULL;
static double rnd01(void)
{
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((g_rng >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define WIDTH    64
#define HEIGHT   36
#define DEPTH    4
#define BUF_BYTES ((size_t)WIDTH * (size_t)HEIGHT * 3u)

#define LUMA_R 0.2126
#define LUMA_G 0.7152
#define LUMA_B 0.0722

/* ------------------------------------------------------------------ */
/* Scene texts                                                         */
/* ------------------------------------------------------------------ */

/*
 * FURNACE: a uniform bright sky (sun_color = 0, glow off, no clouds) and a
 * pure-white Lambertian ground. For a white diffuse surface under a uniform
 * environment of radiance L the outgoing radiance is exactly L, so no sample
 * may exceed L (here L = 1). This is the classic "furnace" energy test.
 */
static const char *FURNACE_SCENE =
    "camera {\n"
    "    eye = 0 2 6\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 40\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0 1 0\n"
    "    sun_color = 0 0 0\n"
    "    horizon_color = 1 1 1\n"
    "    zenith_color = 1 1 1\n"
    "    gradient_gamma = 1\n"
    "    sun_glow_exponent = 350\n"
    "    sun_glow_strength = 0\n"
    "    cloud_coverage = 2\n"
    "    sun_radius = 0\n"
    "}\n"
    "material white {\n"
    "    albedo = 1 1 1\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "    pbr = 0\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = white\n"
    "}\n";

/*
 * SEED: a gradient sky (non-uniform) over a grey Lambertian ground. A ray that
 * hits the ground scatters with cosine-weighted sampling, so the sky radiance
 * it eventually escapes to depends on the sampled bounce direction — and thus
 * on seed_key. Used only for the seed-sensitivity check.
 */
static const char *SEED_SCENE =
    "camera {\n"
    "    eye = 0 2 6\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 40\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0 1 0\n"
    "    sun_color = 0 0 0\n"
    "    horizon_color = 0.1 0.1 0.1\n"
    "    zenith_color = 1 1 1\n"
    "    gradient_gamma = 1\n"
    "    sun_glow_strength = 0\n"
    "    cloud_coverage = 2\n"
    "    sun_radius = 0\n"
    "}\n"
    "material grey {\n"
    "    albedo = 0.8 0.8 0.8\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "    pbr = 0\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = grey\n"
    "}\n";

/*
 * SUN: a bright, point-like sun straight overhead over the same white ground,
 * with a dark sky (so the ONLY meaningful illumination is the sun's direct
 * light). Used for the NEE "direct light is present" check.
 */
static const char *SUN_SCENE =
    "camera {\n"
    "    eye = 0 2 6\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 40\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0 1 0\n"
    "    sun_color = 5 5 5\n"
    "    horizon_color = 0 0 0\n"
    "    zenith_color = 0 0 0\n"
    "    sun_glow_strength = 0\n"
    "    cloud_coverage = 2\n"
    "    sun_radius = 0\n"
    "}\n"
    "material white {\n"
    "    albedo = 1 1 1\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "    pbr = 0\n"
    "}\n"
    "material black {\n"
    "    albedo = 0 0 0\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "    pbr = 0\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = white\n"
    "}\n";

/*
 * SUN_SHADOWED: SUN_SCENE plus a large opaque BLACK sphere floating above the
 * ground so that the vertical shadow ray from every test ground point is
 * occluded. The sphere is black so it contributes no light of its own, and it
 * sits at y = 3 with radius 1.5 (spans y in [1.5, 4.5]) while the primary test
 * rays start at y = 1, so the primaries never hit it.
 */
static const char *SUN_SHADOWED_SCENE =
    "camera {\n"
    "    eye = 0 2 6\n"
    "    target = 0 0 0\n"
    "    up = 0 1 0\n"
    "    vfov = 40\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0 1 0\n"
    "    sun_color = 5 5 5\n"
    "    horizon_color = 0 0 0\n"
    "    zenith_color = 0 0 0\n"
    "    sun_glow_strength = 0\n"
    "    cloud_coverage = 2\n"
    "    sun_radius = 0\n"
    "}\n"
    "material white {\n"
    "    albedo = 1 1 1\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "    pbr = 0\n"
    "}\n"
    "material black {\n"
    "    albedo = 0 0 0\n"
    "    specular = 0 0 0\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    ior = 1\n"
    "    pbr = 0\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = white\n"
    "}\n"
    "sphere {\n"
    "    center = 0 3 0\n"
    "    radius = 1.5\n"
    "    material = black\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Parse `text` and build a Scene into `out`. Returns 0 on success. */
static int build_scene(Scene *out, const char *text)
{
    SceneDesc desc;
    char      err[256];

    scene_desc_init(&desc);
    if (scene_desc_load_string(&desc, text, "<test_pathtrace>",
                               err, sizeof err) != 0) {
        fprintf(stderr, "  scene parse failed: %s\n", err);
        scene_desc_free(&desc);
        return -1;
    }

    memset(out, 0, sizeof *out);
    if (scene_build_from_desc(out, &desc) != 0) {
        scene_desc_free(&desc);
        return -1;
    }

    scene_desc_free(&desc);
    return 0;
}

/* Build the built-in default scene + its default camera. Returns 0 on ok. */
static int build_default_scene(Scene *scene, Camera *cam)
{
    SceneDesc desc;

    scene_desc_init(&desc);
    scene_default_desc(&desc);
    if (desc.material_count == 0) {
        scene_desc_free(&desc);
        return -1;
    }

    memset(scene, 0, sizeof *scene);
    if (scene_build_from_desc(scene, &desc) != 0) {
        scene_desc_free(&desc);
        return -1;
    }

    *cam = scene_default_camera(scene);
    scene_desc_free(&desc);
    return 0;
}

/* Number of differing bytes between two equal-sized buffers. */
static size_t count_diff(const unsigned char *a, const unsigned char *b,
                         size_t n)
{
    size_t d = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) ++d;
    }
    return d;
}

/* Luminance of an RGB triple. */
static double luma3(double r, double g, double b)
{
    return LUMA_R * r + LUMA_G * g + LUMA_B * b;
}

/* Exact vector inequality (any component differs). */
static int vec_differs(Vec3 a, Vec3 b)
{
    return a.x != b.x || a.y != b.y || a.z != b.z;
}

/*
 * Mean luminance of an (2*half+1)^2 block centred on (cx, cy) of an RGB byte
 * buffer. Used by the convergence check to average out per-pixel noise.
 */
static double block_luma_mean(const unsigned char *rgb, int cx, int cy,
                              int half)
{
    double sum = 0.0;
    int    n   = 0;

    for (int y = cy - half; y <= cy + half; ++y) {
        if (y < 0 || y >= HEIGHT) continue;
        for (int x = cx - half; x <= cx + half; ++x) {
            if (x < 0 || x >= WIDTH) continue;
            const unsigned char *p = rgb + ((size_t)y * WIDTH + (size_t)x) * 3u;
            sum += luma3((double)p[0], (double)p[1], (double)p[2]);
            ++n;
        }
    }
    return (n > 0) ? sum / (double)n : 0.0;
}

/* A camera looking down at the ground from the furnace/sun scene's viewpoint. */
static Camera test_camera(void)
{
    return camera_create(vec3(0.0, 2.0, 6.0), vec3(0.0, 0.0, 0.0),
                         vec3(0.0, 1.0, 0.0), 40.0,
                         (double)WIDTH / (double)HEIGHT);
}

/* ------------------------------------------------------------------ */
/* 1. Determinism of pathtrace_radiance                                */
/* ------------------------------------------------------------------ */

static void test_radiance_determinism(const Scene *seed_scene)
{
    Ray r;
    Vec3 a, b;

    r.origin = vec3(0.0, 2.0, 0.0);
    r.dir    = vec3(0.0, -1.0, 0.0);   /* straight down onto the ground */

    a = pathtrace_radiance(seed_scene, r, DEPTH, 0x1234u);
    b = pathtrace_radiance(seed_scene, r, DEPTH, 0x1234u);
    CHECK(!vec_differs(a, b),
          "pathtrace_radiance is bit-identical for a repeated (ray,depth,seed)");

    /* Interleave a different call and re-check purity. */
    (void)pathtrace_radiance(seed_scene, r, DEPTH, 0x9999u);
    b = pathtrace_radiance(seed_scene, r, DEPTH, 0x1234u);
    CHECK(!vec_differs(a, b),
          "pathtrace_radiance is unaffected by an interleaved call (pure)");

    CHECK(isfinite(a.x) && isfinite(a.y) && isfinite(a.z),
          "pathtrace_radiance returns finite components");
    CHECK(a.x >= 0.0 && a.y >= 0.0 && a.z >= 0.0,
          "pathtrace_radiance returns non-negative components");
}

/* ------------------------------------------------------------------ */
/* 2. Determinism vs seed                                              */
/* ------------------------------------------------------------------ */

static void test_seed_sensitivity(const Scene *seed_scene)
{
    const int GRID = 5;
    int    any_diff = 0;

    /* A non-uniform scene (gradient sky) makes the estimator's result depend
     * on the sampled bounce direction, hence on seed_key. Scan a small ray
     * grid so the check cannot hinge on a single lucky/unlucky ray. */
    for (int i = 0; i < GRID; ++i) {
        for (int j = 0; j < GRID; ++j) {
            Ray  r;
            Vec3 a, b;

            r.origin = vec3(-1.0 + 0.5 * (double)i, 2.0,
                            -1.0 + 0.5 * (double)j);
            r.dir    = vec3(0.0, -1.0, 0.0);   /* hit the ground */

            a = pathtrace_radiance(seed_scene, r, DEPTH, 1u);
            b = pathtrace_radiance(seed_scene, r, DEPTH, 2u);
            if (vec_differs(a, b)) any_diff = 1;
        }
    }
    CHECK(any_diff,
          "different seed_key yields a different radiance estimate");

    /* Repeat with a far-apart key over the same grid. */
    any_diff = 0;
    for (int i = 0; i < GRID; ++i) {
        for (int j = 0; j < GRID; ++j) {
            Ray  r;
            Vec3 a, b;

            r.origin = vec3(-1.0 + 0.5 * (double)i, 2.0,
                            -1.0 + 0.5 * (double)j);
            r.dir    = vec3(0.0, -1.0, 0.0);

            a = pathtrace_radiance(seed_scene, r, DEPTH, 1u);
            b = pathtrace_radiance(seed_scene, r, DEPTH, 0xDEADBEEFu);
            if (vec_differs(a, b)) any_diff = 1;
        }
    }
    CHECK(any_diff,
          "a distant seed_key also yields a different estimate");
}

/* ------------------------------------------------------------------ */
/* 3. Furnace / energy conservation                                    */
/* ------------------------------------------------------------------ */

static void test_furnace(const Scene *furnace)
{
    const int    GRID  = 5;
    const double K_SKY = 1.0;        /* uniform sky radiance is exactly 1 */
    const double TOL   = 0.05;       /* generous energy bound slack */

    double sum = 0.0;
    int    n   = 0;
    int    all_finite = 1;
    int    all_in_range = 1;
    int    any_bright = 0;

    for (int i = 0; i < GRID; ++i) {
        for (int j = 0; j < GRID; ++j) {
            Ray  r;
            Vec3 v;
            double l;

            r.origin = vec3(-1.0 + 0.5 * (double)i, 2.0,
                            -1.0 + 0.5 * (double)j);
            r.dir    = vec3(0.0, -1.0, 0.0);  /* hit the ground */

            v = pathtrace_radiance(furnace, r, DEPTH, (unsigned)(i * GRID + j));
            l = luma3(v.x, v.y, v.z);

            if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z))
                all_finite = 0;
            if (v.x < 0.0 || v.y < 0.0 || v.z < 0.0) all_in_range = 0;
            if (v.x > K_SKY + TOL || v.y > K_SKY + TOL || v.z > K_SKY + TOL)
                all_in_range = 0;
            if (l > 0.5) any_bright = 1;

            sum += l;
            ++n;
        }
    }

    CHECK(all_finite, "furnace: every radiance sample is finite");
    CHECK(all_in_range,
          "furnace: every radiance sample lies in [0, sky_radiance + tol]");
    CHECK(any_bright, "furnace: the white ground reflects real light");

    {
        double mean = sum / (double)n;
        CHECK(mean <= K_SKY + TOL,
              "furnace: mean radiance does not exceed the sky radiance");
        CHECK(mean > 0.1,
              "furnace: mean radiance is meaningfully non-zero");
    }
}

/* ------------------------------------------------------------------ */
/* 4. NEE direct lighting + shadowing                                  */
/* ------------------------------------------------------------------ */

/*
 * Mean radiance luminance over a GRID x GRID set of straight-down rays that
 * hit the ground at (x,0,z). `sun_up` is always +Y in these scenes.
 */
static double ground_ray_mean(const Scene *s, int grid, int depth,
                              unsigned base, int *all_finite,
                              int *all_nonneg, double *max_seen)
{
    double sum = 0.0;
    int    n   = 0;

    for (int i = 0; i < grid; ++i) {
        for (int j = 0; j < grid; ++j) {
            Ray    r;
            Vec3   v;
            double l;

            r.origin = vec3(-1.0 + 0.5 * (double)i, 1.0,
                            -1.0 + 0.5 * (double)j);
            r.dir    = vec3(0.0, -1.0, 0.0);

            v = pathtrace_radiance(s, r, depth,
                                   base + (unsigned)(i * grid + j));
            l = luma3(v.x, v.y, v.z);

            if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z))
                *all_finite = 0;
            if (v.x < 0.0 || v.y < 0.0 || v.z < 0.0) *all_nonneg = 0;
            if (l > *max_seen) *max_seen = l;

            sum += l;
            ++n;
        }
    }
    return (n > 0) ? sum / (double)n : 0.0;
}

static void test_nee_and_shadow(const Scene *sun, const Scene *shadowed)
{
    const int GRID = 5;
    int    finite_a = 1, nonneg_a = 1, finite_b = 1, nonneg_b = 1;
    double max_a = 0.0, max_b = 0.0;
    double mean_sun, mean_shadow;

    mean_sun = ground_ray_mean(sun, GRID, DEPTH, 100u,
                               &finite_a, &nonneg_a, &max_a);
    mean_shadow = ground_ray_mean(shadowed, GRID, DEPTH, 100u,
                                  &finite_b, &nonneg_b, &max_b);

    CHECK(finite_a && finite_b,
          "NEE: radiance under a bright sun is finite (lit and shadowed)");
    CHECK(nonneg_a && nonneg_b,
          "NEE: radiance under a bright sun is non-negative");
    CHECK(mean_sun > 0.05,
          "NEE: direct sun lighting on diffuse geometry is present (> 0)");
    CHECK(max_a <= 100.0,
          "NEE: direct sun radiance stays within a sane bound");
    CHECK(mean_shadow < mean_sun * 0.5,
          "NEE: occluding the sun drops the mean ground radiance");
    CHECK(mean_shadow >= 0.0,
          "NEE: the shadowed mean radiance is non-negative");
}

/* ------------------------------------------------------------------ */
/* 5. Convergence with spp                                             */
/* ------------------------------------------------------------------ */

static void test_convergence(const Scene *furnace)
{
    Camera cam = test_camera();
    unsigned char *img1 = malloc(BUF_BYTES);
    unsigned char *img2 = malloc(BUF_BYTES);
    unsigned char *img4 = malloc(BUF_BYTES);
    unsigned char *img8 = malloc(BUF_BYTES);

    CHECK(img1 && img2 && img4 && img8, "convergence: allocations succeed");
    if (!img1 || !img2 || !img4 || !img8) {
        free(img1); free(img2); free(img4); free(img8);
        return;
    }

    CHECK(pathtrace_render(furnace, &cam, WIDTH, HEIGHT, 1, 3, img1) == 0,
          "convergence: render at 1 spp succeeds");
    CHECK(pathtrace_render(furnace, &cam, WIDTH, HEIGHT, 2, 3, img2) == 0,
          "convergence: render at 2 spp succeeds");
    CHECK(pathtrace_render(furnace, &cam, WIDTH, HEIGHT, 4, 3, img4) == 0,
          "convergence: render at 4 spp succeeds");
    CHECK(pathtrace_render(furnace, &cam, WIDTH, HEIGHT, 8, 3, img8) == 0,
          "convergence: render at 8 spp succeeds");

    {
        const int cx = WIDTH / 2, cy = HEIGHT / 2, half = 3;
        double m1 = block_luma_mean(img1, cx, cy, half);
        double m2 = block_luma_mean(img2, cx, cy, half);
        double m4 = block_luma_mean(img4, cx, cy, half);
        double m8 = block_luma_mean(img8, cx, cy, half);

        double d12 = fabs(m2 - m1);
        double d24 = fabs(m4 - m2);
        double d48 = fabs(m8 - m4);

        CHECK(isfinite(m1) && isfinite(m2) && isfinite(m4) && isfinite(m8),
              "convergence: reference-block means are finite");

        /* Successive refinement should not make the change LARGER (generous
         * slack absorbs the stochastic wobble of a finite sample set). */
        CHECK(d24 <= d12 + 30.0,
              "convergence: 2->4 spp change is not larger than 1->2 spp");
        CHECK(d48 <= d12 + 30.0,
              "convergence: 4->8 spp change stays within the initial band");
    }

    free(img1); free(img2); free(img4); free(img8);
}

/* ------------------------------------------------------------------ */
/* 6. Argument validation + default-scene stability                    */
/* ------------------------------------------------------------------ */

static void test_arg_validation(void)
{
    Camera cam = test_camera();
    Scene  scene;
    unsigned char buf[16];
    int have_scene = (build_scene(&scene, FURNACE_SCENE) == 0);

    CHECK(have_scene, "arg-validation: furnace scene builds");
    if (!have_scene) return;

    /* NULL pointers -> code 1. */
    CHECK(pathtrace_render(NULL, &cam, WIDTH, HEIGHT, 1, 1, buf) == 1,
          "pathtrace_render(NULL scene) returns 1");
    CHECK(pathtrace_render(&scene, NULL, WIDTH, HEIGHT, 1, 1, buf) == 1,
          "pathtrace_render(NULL camera) returns 1");
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, 1, 1, NULL) == 1,
          "pathtrace_render(NULL rgb) returns 1");

    /* Non-positive dimensions -> code 2. */
    CHECK(pathtrace_render(&scene, &cam, 0, HEIGHT, 1, 1, buf) == 2,
          "pathtrace_render(width 0) returns 2");
    CHECK(pathtrace_render(&scene, &cam, WIDTH, 0, 1, 1, buf) == 2,
          "pathtrace_render(height 0) returns 2");
    CHECK(pathtrace_render(&scene, &cam, -4, HEIGHT, 1, 1, buf) == 2,
          "pathtrace_render(negative width) returns 2");

    /* Bad sample / depth counts -> code 3. */
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, 0, 1, buf) == 3,
          "pathtrace_render(spp 0) returns 3");
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, 1, -1, buf) == 3,
          "pathtrace_render(max_depth -1) returns 3");

    scene_free(&scene);
}

static void test_default_scene_stability(void)
{
    Scene  scene;
    Camera cam;
    unsigned char *a = malloc(BUF_BYTES);
    unsigned char *b = malloc(BUF_BYTES);

    CHECK(a && b, "default-scene: allocations succeed");
    if (!a || !b) { free(a); free(b); return; }

    if (build_default_scene(&scene, &cam) != 0) {
        CHECK(0, "default-scene: scene_default_desc + build succeeds");
        free(a); free(b);
        return;
    }
    CHECK(1, "default-scene: scene_default_desc + build succeeds");

    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, 2, 3, a) == 0,
          "default-scene: render succeeds");
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, 2, 3, b) == 0,
          "default-scene: second render succeeds");
    CHECK(count_diff(a, b, BUF_BYTES) == 0,
          "default-scene: two renders are byte-identical");

    scene_free(&scene);
    free(a);
    free(b);
}

/* ------------------------------------------------------------------ */
/* Threaded determinism (only in a -DUSE_PTHREADS build)               */
/* ------------------------------------------------------------------ */

#ifdef USE_PTHREADS
static void test_threaded_determinism(const Scene *furnace)
{
    Camera cam = test_camera();
    unsigned char *a = malloc(BUF_BYTES);
    unsigned char *b = malloc(BUF_BYTES);

    CHECK(a && b, "threads: allocations succeed");
    if (!a || !b) { free(a); free(b); return; }

    if (setenv("RAYTRACER_THREADS", "1", 1) != 0) {
        CHECK(0, "threads: setenv(RAYTRACER_THREADS=1) failed");
        free(a); free(b);
        return;
    }
    CHECK(pathtrace_render(furnace, &cam, WIDTH, HEIGHT, 2, 3, a) == 0,
          "threads: single-threaded render succeeds");

    if (setenv("RAYTRACER_THREADS", "4", 1) != 0) {
        CHECK(0, "threads: setenv(RAYTRACER_THREADS=4) failed");
        free(a); free(b);
        return;
    }
    CHECK(pathtrace_render(furnace, &cam, WIDTH, HEIGHT, 2, 3, b) == 0,
          "threads: multi-threaded render succeeds");

    CHECK(count_diff(a, b, BUF_BYTES) == 0,
          "threads: 1 vs 4 threads produce a byte-identical image");

    (void)unsetenv("RAYTRACER_THREADS");
    free(a);
    free(b);
}
#endif /* USE_PTHREADS */

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    Scene furnace, seed_scene, sun, shadowed;
    int   ok = 1;

    /* Silence the renderer's stderr progress meter (never affects pixels). */
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    (void)rnd01();  /* exercise the LCG so it is not "unused" */

    if (build_scene(&furnace, FURNACE_SCENE) != 0) {
        fprintf(stderr, "FAIL: could not build the furnace scene\n");
        return 1;
    }
    if (build_scene(&seed_scene, SEED_SCENE) != 0) {
        fprintf(stderr, "FAIL: could not build the seed scene\n");
        scene_free(&furnace);
        return 1;
    }
    if (build_scene(&sun, SUN_SCENE) != 0) {
        fprintf(stderr, "FAIL: could not build the sun scene\n");
        scene_free(&furnace);
        scene_free(&seed_scene);
        return 1;
    }
    if (build_scene(&shadowed, SUN_SHADOWED_SCENE) != 0) {
        fprintf(stderr, "FAIL: could not build the shadowed scene\n");
        scene_free(&furnace);
        scene_free(&seed_scene);
        scene_free(&sun);
        return 1;
    }

    test_radiance_determinism(&seed_scene);
    test_seed_sensitivity(&seed_scene);
    test_furnace(&furnace);
    test_nee_and_shadow(&sun, &shadowed);
    test_convergence(&furnace);
    test_arg_validation();
    test_default_scene_stability();
#ifdef USE_PTHREADS
    test_threaded_determinism(&furnace);
#endif

    scene_free(&furnace);
    scene_free(&seed_scene);
    scene_free(&sun);
    scene_free(&shadowed);

    if (g_fail != 0) ok = 0;

    printf("tests/test_pathtrace: %d passed, %d failed\n", g_pass, g_fail);
    return ok ? 0 : 1;
}
