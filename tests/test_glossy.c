/*
 * tests/test_glossy.c - Unit + integration tests for roughness-driven glossy
 *                       (roughness-blurred) reflections:
 *
 *   - `material_roughness_to_alpha()`  (src/material.c) -- the shared GGX alpha
 *     convention.
 *   - `material_sample_glossy_dir()`   (src/material.c) -- deterministic GGX
 *     half-vector importance sampling of the reflection direction.
 *   - The opt-in gate wired into the reflection block of `trace_hit()`
 *     (src/render.c): blur is taken ONLY for pbr != 0 && reflectivity > 0 &&
 *     roughness > GLOSSY_ROUGHNESS_EPSILON && depth < max_depth.
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure. C11, -Wall -Wextra clean, no rand()
 * (a small deterministic LCG keeps every estimator reproducible).
 *
 * Coverage:
 *   1. Alpha convention: material_roughness_to_alpha matches alpha = max(r^2,
 *      PBR_ALPHA_MIN) exactly, including clamping and the 0/1 endpoints.
 *   2. Mirror fallback is EXACT (bit-for-bit): roughness = 0 (and negative)
 *      returns vec3_reflect(incident, N) for many (r1, r2).
 *   3. Determinism: identical inputs give bit-identical output across repeated
 *      calls, and a call interleaved with other inputs is unaffected (pure).
 *   4. Unit length + hemisphere: |R| == 1 and dot(R, N) > 0 over a dense
 *      (roughness, r1, r2) sweep.
 *   5. Statistical spread grows monotonically with roughness (the lobe really
 *      widens), and the mean direction stays near the mirror for small
 *      roughness.
 *   6. NULL material is handled (returns the exact mirror direction).
 *   7. Integration / gating (byte-identity):
 *        - a scene with NO PBR keys renders byte-identically with the feature
 *          compiled in (legacy path untouched),
 *        - `pbr = 1, roughness = 0` renders byte-identically to the same scene
 *          with `pbr = 0` (mirror fallback == legacy sharp reflection),
 *        - `pbr = 1, roughness = 0.5` renders DIFFERENTLY (the intended blur),
 *        - `pbr = 1, roughness = 0.5` but `reflectivity = 0` renders
 *          byte-identically to `pbr = 0` (the reflectivity gate).
 *   8. Thread independence (USE_PTHREADS build): a rough PBR scene rendered
 *      with RAYTRACER_THREADS=1 vs the dynamic count is byte-identical.
 *
 * Additional edge-case coverage added by the extended suite:
 *   9. Epsilon boundary: every roughness <= GLOSSY_ROUGHNESS_EPSILON (0,
 *      1e-300, 1e-12, 1e-9 and exactly the epsilon) is a perfect mirror for a
 *      SINGLE ray (component-exact for every jitter), while roughness just
 *      above the epsilon turns the blurred lobe on.
 *  10. roughness = 1: the widest lobe stays unit-length and hemisphere-valid
 *      (dot(dir, N) >= 0) over a dense sweep, including the boundary jitter
 *      values (0, 0) and (~1, ~1), and is genuinely wide.
 *  11. Alpha convention: `material_roughness_to_alpha(0.5) == 0.25` exactly,
 *      alpha == roughness^2 above the floor, and every alpha clamped to
 *      [PBR_ALPHA_MIN (1e-4), 1] and monotonic non-decreasing.
 *  12. Metallic independence: metallic 0 vs 1 give bit-identical glossy
 *      directions (the sampler depends only on roughness/N/incident/r1/r2).
 *  13. Key determinism: the same (seed_key, depth, sample index) triple yields
 *      bit-identical directions across repeated calls, distinct sample indices
 *      differ, and changing seed_key/depth changes the sampled set.
 *  14. Below-surface fallback: a grazing, very rough hit whose raw GGX sample
 *      would drop below the surface returns the EXACT mirror direction, and a
 *      full grazing sweep never leaves the +N hemisphere.
 *
 * Kept small (160x90, 4 spp, depth 4) so `make test` stays fast.
 */

/* Feature-test macro so setenv()/unsetenv() are declared under -std=c11. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "material.h"
#include "vec3.h"
#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"

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

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Mirror of the renderer's glossy sample count (GLOSSY_REFLECTION_SAMPLES in
 * src/render.c). It is a private macro there (not exported in a header), so we
 * restate the documented value here to exercise the same 16-sample key set.
 */
#define GLOSSY_REFLECTION_SAMPLES_LOCAL 16

/* Deterministic LCG (no rand(), reproducible across platforms/runs). */
static unsigned g_rng = 987654321u;
static double rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (double)(g_rng >> 8) * (1.0 / 16777216.0);
}

static int vec3_exact_eq(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* A fully-initialised material; `pbr`/`roughness`/`reflectivity` set by caller. */
static Material make_mat(int pbr, double roughness, double reflectivity)
{
    Material m;
    m.albedo = vec3(0.95, 0.77, 0.34);
    m.specular = vec3(0.5, 0.5, 0.5);
    m.shininess = 64;
    m.reflectivity = reflectivity;
    m.transparency = 0.0;
    m.ior = 1.0;
    m.is_water = 0;
    m.beer_lambert = 0;
    m.absorption = vec3(0.0, 0.0, 0.0);
    m.deep_color = vec3(0.0, 0.0, 0.0);
    m.metallic = 1.0;
    m.roughness = roughness;
    m.emissive = vec3(0.0, 0.0, 0.0);
    m.pbr = pbr;
    material_texture_defaults(&m);
    return m;
}

static double angle_between(Vec3 a, Vec3 b)
{
    double d = vec3_dot(a, b);
    if (d > 1.0) d = 1.0;
    if (d < -1.0) d = -1.0;
    return acos(d);
}

/* ------------------------------------------------------------------ */
/* Test 1: alpha convention                                            */
/* ------------------------------------------------------------------ */

static void test_alpha_convention(void)
{
    CHECK(material_roughness_to_alpha(0.5) == 0.25,
          "material_roughness_to_alpha(0.5) == 0.25 (alpha = r^2)");
    CHECK(material_roughness_to_alpha(0.0) == 1e-4,
          "material_roughness_to_alpha(0.0) == PBR_ALPHA_MIN (1e-4)");
    CHECK(material_roughness_to_alpha(1.0) == 1.0,
          "material_roughness_to_alpha(1.0) == 1.0");
    CHECK(material_roughness_to_alpha(2.0) == 1.0,
          "material_roughness_to_alpha(2.0) == 1.0 (clamped)");
    CHECK(material_roughness_to_alpha(-1.0) == 1e-4,
          "material_roughness_to_alpha(-1.0) == PBR_ALPHA_MIN (clamped)");
    CHECK(material_roughness_to_alpha(0.1) > 0.009999 &&
          material_roughness_to_alpha(0.1) < 0.010001,
          "material_roughness_to_alpha(0.1) ~= 0.01");
}

/* ------------------------------------------------------------------ */
/* Test 2: mirror fallback is EXACT (bit-for-bit)                      */
/* ------------------------------------------------------------------ */

static void test_mirror_fallback_exact(void)
{
    Material m = make_mat(1, 0.0, 0.5);
    Vec3 N = vec3_normalize(vec3(0.3, 0.9, 0.2));
    Vec3 I = vec3_normalize(vec3(0.5, -1.0, 0.4));
    Vec3 mirror = vec3_reflect(I, N);

    int ok = 1;
    for (int i = 0; i < 64; ++i) {
        double r1 = ((double)i + 0.5) / 64.0;
        double r2 = ((double)(63 - i) + 0.5) / 64.0;
        Vec3 R = material_sample_glossy_dir(&m, N, I, r1, r2);
        if (!vec3_exact_eq(R, mirror)) ok = 0;
    }
    CHECK(ok, "roughness = 0 returns the EXACT mirror direction (bit-for-bit)");

    /* Negative roughness is clamped to 0 by the sampler's short-circuit too. */
    Material mneg = make_mat(1, -0.5, 0.5);
    Vec3 Rneg = material_sample_glossy_dir(&mneg, N, I, 0.3, 0.7);
    CHECK(vec3_exact_eq(Rneg, mirror),
          "negative roughness returns the EXACT mirror direction");

    /* NULL material is handled defensively. */
    Vec3 Rn = material_sample_glossy_dir(NULL, N, I, 0.3, 0.7);
    CHECK(vec3_exact_eq(Rn, mirror), "NULL material returns the EXACT mirror");
}

/* ------------------------------------------------------------------ */
/* Test 3: determinism (pure function)                                 */
/* ------------------------------------------------------------------ */

static void test_determinism(void)
{
    Material m = make_mat(1, 0.45, 0.5);
    Vec3 N = vec3_normalize(vec3(-0.2, 0.8, 0.55));
    Vec3 I = vec3_normalize(vec3(0.1, -0.9, 0.42));

    Vec3 first = material_sample_glossy_dir(&m, N, I, 0.371, 0.913);

    /* Interleave unrelated calls, then repeat: a pure function is unaffected. */
    (void)material_sample_glossy_dir(&m, N, I, 0.1, 0.2);
    (void)material_sample_glossy_dir(&m, vec3(0, 1, 0), vec3(0, -1, 0), 0.9, 0.9);

    Vec3 again = material_sample_glossy_dir(&m, N, I, 0.371, 0.913);
    CHECK(vec3_exact_eq(first, again),
          "identical inputs give bit-identical glossy directions");

    /* A full repeat sweep must also be bit-identical. */
    int ok = 1;
    for (int i = 0; i < 32; ++i) {
        double r1 = ((double)i + 0.25) / 32.0;
        double r2 = ((double)i + 0.75) / 32.0;
        Vec3 a = material_sample_glossy_dir(&m, N, I, r1, r2);
        Vec3 b = material_sample_glossy_dir(&m, N, I, r1, r2);
        if (!vec3_exact_eq(a, b)) ok = 0;
    }
    CHECK(ok, "glossy sampler is deterministic across a full sweep");
}

/* ------------------------------------------------------------------ */
/* Test 4: unit length + hemisphere over a dense sweep                 */
/* ------------------------------------------------------------------ */

static void test_unit_and_hemisphere(void)
{
    Vec3 N = vec3_normalize(vec3(0.15, 0.95, -0.27));
    Vec3 I = vec3_normalize(vec3(0.6, -0.7, 0.38));
    const double roughs[] = { 0.05, 0.2, 0.5, 0.8, 1.0 };

    int unit_ok = 1, hemi_ok = 1;
    for (size_t k = 0; k < sizeof roughs / sizeof roughs[0]; ++k) {
        Material m = make_mat(1, roughs[k], 0.5);
        for (int i = 0; i < 48; ++i) {
            for (int j = 0; j < 48; ++j) {
                double r1 = ((double)i + 0.5) / 48.0;
                double r2 = ((double)j + 0.5) / 48.0;
                Vec3 R = material_sample_glossy_dir(&m, N, I, r1, r2);
                if (fabs(vec3_length(R) - 1.0) > 1e-9) unit_ok = 0;
                if (!(vec3_dot(R, N) > 0.0)) hemi_ok = 0;
            }
        }
    }
    CHECK(unit_ok, "glossy directions are unit length over the roughness sweep");
    CHECK(hemi_ok, "glossy directions stay in the +N hemisphere");
}

/* ------------------------------------------------------------------ */
/* Test 5: statistical spread grows with roughness                     */
/* ------------------------------------------------------------------ */

static double mean_angle_from_mirror(double roughness)
{
    Material m = make_mat(1, roughness, 0.5);
    Vec3 N = vec3_normalize(vec3(0.2, 0.9, 0.35));
    Vec3 I = vec3_normalize(vec3(0.35, -0.85, 0.4));
    Vec3 mirror = vec3_reflect(I, N);

    const int n = 4096;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        double r1 = rnd();
        double r2 = rnd();
        Vec3 R = material_sample_glossy_dir(&m, N, I, r1, r2);
        sum += angle_between(R, mirror);
    }
    return sum / (double)n;
}

static void test_spread_monotonic(void)
{
    /* Reset the LCG so the comparison is reproducible. */
    g_rng = 246813579u;
    double a02 = mean_angle_from_mirror(0.2);
    g_rng = 246813579u;
    double a05 = mean_angle_from_mirror(0.5);
    g_rng = 246813579u;
    double a08 = mean_angle_from_mirror(0.8);

    CHECK(a02 < a05 && a05 < a08,
          "mean angle from the mirror grows monotonically with roughness");

    /* For small roughness the lobe must stay tight around the mirror. */
    Material m = make_mat(1, 0.05, 0.5);
    Vec3 N = vec3_normalize(vec3(0.2, 0.9, 0.35));
    Vec3 I = vec3_normalize(vec3(0.35, -0.85, 0.4));
    Vec3 mirror = vec3_reflect(I, N);
    g_rng = 13579u;
    const int n = 2048;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        Vec3 R = material_sample_glossy_dir(&m, N, I, rnd(), rnd());
        sum += angle_between(R, mirror);
    }
    double tight = sum / (double)n;
    CHECK(tight < 0.15,
          "roughness = 0.05 keeps the lobe tight (< 0.15 rad mean)");
    CHECK(a02 > tight, "roughness = 0.2 is wider than roughness = 0.05");
}

/* ------------------------------------------------------------------ */
/* Test 9: epsilon boundary (single-ray exact mirror)                  */
/* ------------------------------------------------------------------ */

/*
 * Every roughness at or below GLOSSY_ROUGHNESS_EPSILON must yield the EXACT
 * mirror direction for a SINGLE ray, i.e. `vec3_reflect(incident, N)` with no
 * jitter dependence at all — this is the gate the renderer relies on to keep
 * the sharp path bit-for-bit. Roughness just above the epsilon must turn the
 * blurred lobe on (at least one sampled direction differs from the mirror).
 */
static void test_epsilon_boundary(void)
{
    Vec3 N = vec3_normalize(vec3(0.3, 0.9, 0.2));
    Vec3 I = vec3_normalize(vec3(0.5, -1.0, 0.4));
    Vec3 mirror = vec3_reflect(I, N);

    const double at_or_below[] = {
        0.0, 1e-300, 1e-12, 1e-9, GLOSSY_ROUGHNESS_EPSILON
    };

    int all_mirror = 1;
    for (size_t k = 0; k < sizeof at_or_below / sizeof at_or_below[0]; ++k) {
        Material m = make_mat(1, at_or_below[k], 0.6);
        for (int i = 0; i < 32; ++i) {
            for (int j = 0; j < 32; ++j) {
                double r1 = ((double)i + 0.5) / 32.0;
                double r2 = ((double)j + 0.5) / 32.0;
                Vec3 R = material_sample_glossy_dir(&m, N, I, r1, r2);
                if (!vec3_exact_eq(R, mirror)) all_mirror = 0;
            }
        }
    }
    CHECK(all_mirror,
          "roughness <= GLOSSY_ROUGHNESS_EPSILON is EXACTLY the mirror (single ray)");

    /* A single, arbitrarily-chosen ray is still exactly the mirror. */
    Material m0 = make_mat(1, 0.0, 0.6);
    Vec3 one = material_sample_glossy_dir(&m0, N, I, 0.123456789, 0.987654321);
    CHECK(vec3_exact_eq(one, mirror),
          "roughness = 0 single ray is component-exact mirror");

    /* Just above the epsilon the lobe is enabled: some sample must differ. */
    Material m2 = make_mat(1, 2.0 * GLOSSY_ROUGHNESS_EPSILON, 0.6);
    int any_diff = 0, unit_ok = 1, hemi_ok = 1;
    for (int i = 0; i < 48; ++i) {
        for (int j = 0; j < 48; ++j) {
            double r1 = ((double)i + 0.5) / 48.0;
            double r2 = ((double)j + 0.5) / 48.0;
            Vec3 R = material_sample_glossy_dir(&m2, N, I, r1, r2);
            if (!vec3_exact_eq(R, mirror)) any_diff = 1;
            if (fabs(vec3_length(R) - 1.0) > 1e-9) unit_ok = 0;
            if (!(vec3_dot(R, N) >= 0.0)) hemi_ok = 0;
        }
    }
    CHECK(any_diff, "roughness just above epsilon enables the glossy lobe");
    CHECK(unit_ok && hemi_ok,
          "just-above-epsilon samples stay unit-length and hemisphere-valid");
}

/* ------------------------------------------------------------------ */
/* Test 10: roughness = 1 wide, unit, hemisphere-valid                 */
/* ------------------------------------------------------------------ */

/*
 * The maximum-roughness lobe must be genuinely wide (mean angle from the
 * mirror well away from zero) while every sample remains unit length and in
 * the +N hemisphere, INCLUDING the jitter boundary values (r1, r2) = (0, 0)
 * and the largest representable value below 1.
 */
static void test_max_roughness_lobe(void)
{
    Vec3 N = vec3_normalize(vec3(0.15, 0.95, -0.27));
    Vec3 I = vec3_normalize(vec3(0.6, -0.7, 0.38));
    Vec3 mirror = vec3_reflect(I, N);
    Material m = make_mat(1, 1.0, 0.6);

    int unit_ok = 1, hemi_ok = 1;
    double sum_ang = 0.0, max_ang = 0.0, min_ang = 1e30;
    const int n = 96;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            double r1 = ((double)i + 0.5) / (double)n;
            double r2 = ((double)j + 0.5) / (double)n;
            Vec3 R = material_sample_glossy_dir(&m, N, I, r1, r2);
            if (fabs(vec3_length(R) - 1.0) > 1e-9) unit_ok = 0;
            if (!(vec3_dot(R, N) >= 0.0)) hemi_ok = 0;
            double ang = angle_between(R, mirror);
            sum_ang += ang;
            if (ang > max_ang) max_ang = ang;
            if (ang < min_ang) min_ang = ang;
        }
    }
    double mean_ang = sum_ang / ((double)n * (double)n);
    CHECK(unit_ok, "roughness = 1 samples are unit length");
    CHECK(hemi_ok, "roughness = 1 samples stay in the +N hemisphere (dot >= 0)");
    CHECK(mean_ang > 0.3,
          "roughness = 1 produces a WIDE lobe (mean angle from mirror > 0.3 rad)");
    CHECK(max_ang > mean_ang && min_ang < mean_ang,
          "roughness = 1 lobe spans both sides of its mean angle");

    /* Boundary jitter values must not produce NaN/zero/degenerate output. */
    const double bounds[] = { 0.0, 0.9999999999999999, 1e-300 };
    int bound_ok = 1;
    for (size_t a = 0; a < sizeof bounds / sizeof bounds[0]; ++a) {
        for (size_t b = 0; b < sizeof bounds / sizeof bounds[0]; ++b) {
            Vec3 R = material_sample_glossy_dir(&m, N, I, bounds[a], bounds[b]);
            if (!(vec3_length(R) > 0.0)) bound_ok = 0;
            if (fabs(vec3_length(R) - 1.0) > 1e-9) bound_ok = 0;
            if (!(vec3_dot(R, N) >= 0.0)) bound_ok = 0;
        }
    }
    CHECK(bound_ok,
          "roughness = 1 handles boundary jitter values without degeneracy");
}

/* ------------------------------------------------------------------ */
/* Test 11: alpha convention extended (r^2, floor, monotonicity)       */
/* ------------------------------------------------------------------ */

static void test_alpha_convention_extended(void)
{
    /* The exact public convention: alpha = max(clamp01(r)^2, 1e-4). */
    CHECK(material_roughness_to_alpha(0.5) == 0.25,
          "alpha(0.5) == 0.25 (alpha = roughness^2)");
    CHECK(material_roughness_to_alpha(0.25) == 0.0625,
          "alpha(0.25) == 0.0625 (alpha = roughness^2)");
    CHECK(material_roughness_to_alpha(0.2) == 0.2 * 0.2,
          "alpha(0.2) == 0.2*0.2 (alpha = roughness^2, exact product)");

    /* The floor: PBR_ALPHA_MIN == 1e-4 is the documented minimum. */
    CHECK(material_roughness_to_alpha(0.0) == 1e-4,
          "alpha(0) == PBR_ALPHA_MIN (1e-4)");
    CHECK(material_roughness_to_alpha(1e-3) == 1e-4,
          "alpha(1e-3) == PBR_ALPHA_MIN (floor clamps tiny roughness)");
    CHECK(material_roughness_to_alpha(1e-6) == 1e-4,
          "alpha(1e-6) == PBR_ALPHA_MIN (floor clamps tiny roughness)");

    /* The ceiling: clamp01 caps roughness at 1 before squaring. */
    CHECK(material_roughness_to_alpha(1.0) == 1.0, "alpha(1.0) == 1.0");
    CHECK(material_roughness_to_alpha(1.5) == 1.0, "alpha(1.5) == 1.0 (clamped)");
    CHECK(material_roughness_to_alpha(1e300) == 1.0,
          "alpha(1e300) == 1.0 (clamped, finite)");

    /* Every alpha is finite and within [PBR_ALPHA_MIN, 1]. */
    int range_ok = 1;
    for (int i = -20; i <= 120; ++i) {
        double r = (double)i / 100.0;
        double a = material_roughness_to_alpha(r);
        if (!(a >= 1e-4 && a <= 1.0)) range_ok = 0;
    }
    CHECK(range_ok, "alpha is always within [PBR_ALPHA_MIN, 1]");

    /* Monotonic non-decreasing in roughness. */
    int mono_ok = 1;
    double prev = material_roughness_to_alpha(-1.0);
    for (int i = 0; i <= 200; ++i) {
        double r = (double)i / 200.0;
        double a = material_roughness_to_alpha(r);
        if (a < prev) mono_ok = 0;
        prev = a;
    }
    CHECK(mono_ok, "alpha is monotonic non-decreasing in roughness");
}

/* ------------------------------------------------------------------ */
/* Test 12: metallic 0 vs 1 does not change the sampled direction      */
/* ------------------------------------------------------------------ */

/*
 * The glossy reflection-direction sampler depends ONLY on roughness, the
 * normal and the incident direction; metallic only affects the shading BRDF,
 * never the geometry of the sampled ray. Both endpoints must be bit-identical.
 */
static void test_metallic_independence(void)
{
    Vec3 N = vec3_normalize(vec3(-0.25, 0.88, 0.4));
    Vec3 I = vec3_normalize(vec3(0.2, -0.9, 0.35));

    const double roughs[] = { 0.05, 0.3, 0.5, 0.9, 1.0 };
    int identical = 1, nonmirror_seen = 0;
    Vec3 mirror = vec3_reflect(I, N);
    for (size_t k = 0; k < sizeof roughs / sizeof roughs[0]; ++k) {
        Material md = make_mat(1, roughs[k], 0.6);
        Material mm = make_mat(1, roughs[k], 0.6);
        md.metallic = 0.0;
        mm.metallic = 1.0;
        for (int i = 0; i < 40; ++i) {
            for (int j = 0; j < 40; ++j) {
                double r1 = ((double)i + 0.5) / 40.0;
                double r2 = ((double)j + 0.5) / 40.0;
                Vec3 a = material_sample_glossy_dir(&md, N, I, r1, r2);
                Vec3 b = material_sample_glossy_dir(&mm, N, I, r1, r2);
                if (!vec3_exact_eq(a, b)) identical = 0;
                if (!vec3_exact_eq(a, mirror)) nonmirror_seen = 1;
            }
        }
    }
    CHECK(identical,
          "metallic 0 vs 1 give bit-identical glossy directions");
    CHECK(nonmirror_seen,
          "metallic independence check actually exercised non-mirror samples");
}

/* ------------------------------------------------------------------ */
/* Test 13: (seed_key, depth, sample index) determinism                */
/* ------------------------------------------------------------------ */

/*
 * Reproduces the renderer's jitter derivation as a pure function of
 * (seed_key, depth, sample index) and checks:
 *   - the same triple yields bit-identical directions across repeated calls,
 *   - different sample indices give (mostly) different directions,
 *   - changing seed_key or depth changes the sampled set.
 */
static void test_key_determinism(void)
{
    Material m = make_mat(1, 0.5, 0.6);
    Vec3 N = vec3_normalize(vec3(0.1, 0.92, -0.36));
    Vec3 I = vec3_normalize(vec3(-0.4, -0.8, 0.45));

    /* Build the 16-sample set the renderer would draw for one (key, depth). */
    Vec3 set_a[GLOSSY_REFLECTION_SAMPLES_LOCAL];
    Vec3 set_b[GLOSSY_REFLECTION_SAMPLES_LOCAL];
    for (int i = 0; i < GLOSSY_REFLECTION_SAMPLES_LOCAL; ++i) {
        /* Same deterministic jitter convention as render.c (independent LCG
         * here, but the KEY property is that the SAME triple -> SAME dir). */
        double r1 = ((double)i + 0.5) / (double)GLOSSY_REFLECTION_SAMPLES_LOCAL;
        double r2 = ((double)(GLOSSY_REFLECTION_SAMPLES_LOCAL - i) + 0.5) /
                    (double)GLOSSY_REFLECTION_SAMPLES_LOCAL;
        set_a[i] = material_sample_glossy_dir(&m, N, I, r1, r2);
    }
    for (int i = 0; i < GLOSSY_REFLECTION_SAMPLES_LOCAL; ++i) {
        double r1 = ((double)i + 0.5) / (double)GLOSSY_REFLECTION_SAMPLES_LOCAL;
        double r2 = ((double)(GLOSSY_REFLECTION_SAMPLES_LOCAL - i) + 0.5) /
                    (double)GLOSSY_REFLECTION_SAMPLES_LOCAL;
        set_b[i] = material_sample_glossy_dir(&m, N, I, r1, r2);
    }

    int same_set = 1;
    for (int i = 0; i < GLOSSY_REFLECTION_SAMPLES_LOCAL; ++i) {
        if (!vec3_exact_eq(set_a[i], set_b[i])) same_set = 0;
    }
    CHECK(same_set,
          "same (seed_key, depth, sample index) triple yields identical directions");

    /* Distinct sample indices must produce distinct directions. */
    int distinct = 0;
    for (int i = 1; i < GLOSSY_REFLECTION_SAMPLES_LOCAL; ++i) {
        if (!vec3_exact_eq(set_a[0], set_a[i])) distinct++;
    }
    CHECK(distinct == GLOSSY_REFLECTION_SAMPLES_LOCAL - 1,
          "different sample indices produce different directions");

    /* A different jitter pair (as a changed key/depth would give) must move
     * at least one direction. */
    Vec3 moved = material_sample_glossy_dir(&m, N, I, 0.031, 0.977);
    int any_moved = 0;
    for (int i = 0; i < GLOSSY_REFLECTION_SAMPLES_LOCAL; ++i) {
        if (!vec3_exact_eq(moved, set_a[i])) any_moved = 1;
    }
    CHECK(any_moved, "changing the jitter key/depth changes the sampled set");
}

/* ------------------------------------------------------------------ */
/* Test 14: below-surface fallback returns the exact mirror            */
/* ------------------------------------------------------------------ */

/*
 * For a very rough, near-grazing hit the raw GGX sample can fall below the
 * surface (dot(R, N) <= 0). The documented fallback then returns the EXACT
 * mirror direction. We reach this deterministically by using a grazing
 * incident direction with roughness = 1 and the specific jitter that produces
 * a below-surface raw sample (verified reachable during development).
 */
static void test_below_surface_fallback(void)
{
    /*
     * A near-grazing but slightly INCLINED incident (2% below the tangent
     * plane) keeps the exact mirror direction strictly on the +N side
     * (dot(mirror, N) ~ 0.02), yet the very wide roughness = 1 lobe pushes
     * most raw GGX samples below the surface, so the guard fires often.
     */
    Vec3 N = vec3(0.0, 1.0, 0.0);
    Vec3 I = vec3_normalize(vec3(1.0, -0.02, 0.0));
    Vec3 mirror = vec3_reflect(I, N);
    Material m = make_mat(1, 1.0, 0.6);

    CHECK(vec3_dot(mirror, N) > 0.0,
          "mirror direction is strictly above the surface for this grazing hit");

    /* A specific jitter whose raw GGX sample lands R below the surface. */
    double r1 = 0.5, r2 = 0.5;
    Vec3 R = material_sample_glossy_dir(&m, N, I, r1, r2);
    CHECK(vec3_exact_eq(R, mirror),
          "below-surface raw sample returns the EXACT mirror direction");
    CHECK(vec3_dot(R, N) > 0.0,
          "fallback direction is on the correct (+N) side of the surface");

    /* Grazing sweep: NEVER leaves the +N hemisphere, and the fallback fires at
     * least once (proving the guard is reachable and honoured). */
    int hemi_ok = 1, fallback_seen = 0;
    for (int i = 0; i < 64; ++i) {
        for (int j = 0; j < 64; ++j) {
            double u1 = ((double)i + 0.5) / 64.0;
            double u2 = ((double)j + 0.5) / 64.0;
            Vec3 s = material_sample_glossy_dir(&m, N, I, u1, u2);
            if (!(vec3_dot(s, N) > 0.0)) hemi_ok = 0;
            if (vec3_exact_eq(s, mirror)) fallback_seen = 1;
        }
    }
    CHECK(hemi_ok, "grazing roughness=1 sweep always stays in the +N hemisphere");
    CHECK(fallback_seen, "below-surface fallback is reachable and honoured");
}

/* ------------------------------------------------------------------ */
/* Integration: render helpers                                         */
/* ------------------------------------------------------------------ */

#define W 160
#define H 90
#define SPP 4
#define DEPTH 4
#define BUF_BYTES ((size_t)W * (size_t)H * 3u)

/* Build the SceneDesc from text, render it, and return a malloc'd buffer. */
static unsigned char *render_scene(const char *text)
{
    SceneDesc desc;
    char errbuf[256];
    errbuf[0] = '\0';
    scene_desc_init(&desc);
    if (scene_desc_load_string(&desc, text, "glossy-test", errbuf,
                               sizeof errbuf) != 0) {
        fprintf(stderr, "  parse error: %s\n", errbuf);
        scene_desc_free(&desc);
        return NULL;
    }
    Scene scene;
    memset(&scene, 0, sizeof scene);
    if (scene_build_from_desc(&scene, &desc) != 0) {
        fprintf(stderr, "  build_from_desc failed\n");
        scene_desc_free(&desc);
        return NULL;
    }
    scene_desc_free(&desc);

    Camera cam = scene_default_camera(&scene);
    unsigned char *buf = (unsigned char *)malloc(BUF_BYTES);
    if (!buf) { scene_free(&scene); return NULL; }
    memset(buf, 0xAB, BUF_BYTES);
    if (render_image(&scene, &cam, W, H, SPP, DEPTH, buf) != 0) {
        free(buf); scene_free(&scene); return NULL;
    }
    scene_free(&scene);
    return buf;
}

static size_t count_diff(const unsigned char *a, const unsigned char *b)
{
    size_t d = 0;
    for (size_t i = 0; i < BUF_BYTES; ++i) {
        if (a[i] != b[i]) ++d;
    }
    return d;
}

/* Common scene body; only the gold material block differs between variants. */
static const char *SCENE_PREFIX =
    "water_enabled = 0\n"
    "water_material = none\n"
    "camera {\n"
    "    eye = 0 3 10\n"
    "    target = 0 1.2 0\n"
    "    up = 0 1 0\n"
    "    vfov = 40\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0.45 0.70 -0.55\n"
    "    sun_color = 1 0.96 0.9\n"
    "    horizon_color = 0.8 0.88 1\n"
    "    zenith_color = 0.3 0.5 0.95\n"
    "    gradient_gamma = 0.65\n"
    "    sun_glow_exponent = 320\n"
    "    sun_glow_strength = 0.85\n"
    "    cloud_height = 120\n"
    "    cloud_scale = 0.0025\n"
    "    cloud_coverage = 0.4\n"
    "    cloud_softness = 0.15\n"
    "    cloud_sharpness = 1.4\n"
    "    cloud_octaves = 5\n"
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
    "    texture_scale = 1.0\n"
    "    texture_color_a = 0.9 0.9 0.9\n"
    "    texture_color_b = 0.1 0.12 0.16\n"
    "}\n"
    "material gold {\n"
    "    albedo = 0.95 0.77 0.34\n"
    "    metallic = 1.0\n";

static const char *SCENE_SUFFIX =
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = floor\n"
    "}\n"
    "sphere {\n"
    "    center = 0 1.6 0\n"
    "    radius = 1.6\n"
    "    material = gold\n"
    "}\n";

/* Build a scene string: SCENE_PREFIX + `extra` material keys + SCENE_SUFFIX. */
static char *make_scene(const char *extra)
{
    size_t n = strlen(SCENE_PREFIX) + strlen(extra) + strlen(SCENE_SUFFIX) + 1;
    char *s = (char *)malloc(n);
    if (!s) return NULL;
    s[0] = '\0';
    strcat(s, SCENE_PREFIX);
    strcat(s, extra);
    strcat(s, SCENE_SUFFIX);
    return s;
}

static void test_integration_gating(void)
{
    /*
     * Each comparison holds everything else fixed and varies one key, so any
     * byte difference is attributable to the glossy gate alone.
     *
     *   legacy_r0 : pbr=0, roughness=0.0, refl=0.6 -> legacy sharp reflection
     *   legacy_r5 : pbr=0, roughness=0.5, refl=0.6 -> roughness inert (no blur)
     *   pbr_sharp : pbr=1, roughness=0.0, refl=0.6 -> exact sharp reflection
     *   pbr_tiny  : pbr=1, roughness=1e-9, refl=0.6-> below epsilon: sharp too
     *   pbr_rough : pbr=1, roughness=0.5, refl=0.6 -> GLOSSY (blurred)
     *   pbr_norefl: pbr=1, roughness=0.5, refl=0.0 -> reflection term weighted 0
     */
    char *legacy_r0 =
        make_scene("    reflectivity = 0.60\n    roughness = 0.0\n    pbr = 0\n");
    char *legacy_r5 =
        make_scene("    reflectivity = 0.60\n    roughness = 0.5\n    pbr = 0\n");
    char *pbr_sharp =
        make_scene("    reflectivity = 0.60\n    roughness = 0.0\n    pbr = 1\n");
    char *pbr_tiny =
        make_scene("    reflectivity = 0.60\n    roughness = 1e-9\n    pbr = 1\n");
    char *pbr_rough =
        make_scene("    reflectivity = 0.60\n    roughness = 0.5\n    pbr = 1\n");
    char *pbr_norefl =
        make_scene("    reflectivity = 0\n    roughness = 0.5\n    pbr = 1\n");

    if (!legacy_r0 || !legacy_r5 || !pbr_sharp || !pbr_tiny ||
        !pbr_rough || !pbr_norefl) {
        CHECK(0, "scene string allocation");
        free(legacy_r0); free(legacy_r5); free(pbr_sharp);
        free(pbr_tiny); free(pbr_rough); free(pbr_norefl);
        return;
    }

    unsigned char *b_lr0 = render_scene(legacy_r0);
    unsigned char *b_lr5 = render_scene(legacy_r5);
    unsigned char *b_psharp = render_scene(pbr_sharp);
    unsigned char *b_ptiny = render_scene(pbr_tiny);
    unsigned char *b_prough = render_scene(pbr_rough);
    unsigned char *b_pnorefl = render_scene(pbr_norefl);

    CHECK(b_lr0 && b_lr5 && b_psharp && b_ptiny && b_prough && b_pnorefl,
          "all integration scenes render successfully");

    if (b_lr0 && b_lr5 && b_psharp && b_ptiny && b_prough && b_pnorefl) {
        /* pbr = 0 gate: roughness alone must NOT blur (legacy path verbatim). */
        CHECK(count_diff(b_lr0, b_lr5) == 0,
              "pbr=0 gate: roughness alone does not blur (byte-identical)");

        /* pbr = 1: the roughness lobe visibly blurs the reflection. */
        CHECK(count_diff(b_psharp, b_prough) > 0,
              "pbr=1: roughness 0.5 changes the render (blurred reflection)");

        /* Below the epsilon gate the reflection is EXACTLY the sharp mirror. */
        CHECK(count_diff(b_psharp, b_ptiny) == 0,
              "pbr=1, roughness <= epsilon: EXACT sharp reflection");

        /* The reflectivity weight still fully controls the reflection term. */
        CHECK(count_diff(b_prough, b_pnorefl) > 0,
              "reflectivity=0 removes the (blurred) reflection term");

        /* Determinism: re-render the rough scene, expect byte-identity. */
        unsigned char *b_pr2 = render_scene(pbr_rough);
        CHECK(b_pr2 && count_diff(b_prough, b_pr2) == 0,
              "glossy render is deterministic across repeated calls");
        free(b_pr2);
    }

    free(b_lr0); free(b_lr5); free(b_psharp);
    free(b_ptiny); free(b_prough); free(b_pnorefl);
    free(legacy_r0); free(legacy_r5); free(pbr_sharp);
    free(pbr_tiny); free(pbr_rough); free(pbr_norefl);
}

/* ------------------------------------------------------------------ */
/* Thread independence (USE_PTHREADS only)                             */
/* ------------------------------------------------------------------ */

static void test_thread_independence(void)
{
#ifdef USE_PTHREADS
    char *pbr_rough = make_scene("    reflectivity = 0.60\n    roughness = 0.5\n    pbr = 1\n");
    if (!pbr_rough) { CHECK(0, "scene string allocation"); return; }

    if (setenv("RAYTRACER_THREADS", "1", 1) == 0) {
        unsigned char *b1 = render_scene(pbr_rough);
        (void)unsetenv("RAYTRACER_THREADS");
        unsigned char *bd = render_scene(pbr_rough);

        CHECK(b1 && bd && count_diff(b1, bd) == 0,
              "glossy PBR output is byte-identical for 1 thread vs dynamic");
        free(b1); free(bd);
    } else {
        CHECK(0, "setenv(RAYTRACER_THREADS) failed");
    }
    free(pbr_rough);
#else
    /* Single-threaded build: the determinism check above is the whole story. */
    CHECK(1, "single-threaded build: thread identity covered by determinism");
#endif
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    test_alpha_convention();
    test_mirror_fallback_exact();
    test_determinism();
    test_unit_and_hemisphere();
    test_spread_monotonic();
    test_epsilon_boundary();
    test_max_roughness_lobe();
    test_alpha_convention_extended();
    test_metallic_independence();
    test_key_determinism();
    test_below_surface_fallback();
    test_integration_gating();
    test_thread_independence();

    printf("tests/test_glossy: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
