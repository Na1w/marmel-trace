/*
 * tests/test_noise.c - Unit tests for the procedural noise library (src/noise.c).
 *
 * The Makefile links every test source in tests/ against all project objects EXCEPT
 * src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure.
 *
 * Coverage:
 *   1. Determinism (bit-identical, order-independent, no hidden global state).
 *   2. Seed sensitivity (seed 1 vs seed 2 differ on >= 90% of samples).
 *   3. Output ranges + non-triviality (stddev) for every function.
 *   4. fBm parameter behaviour (octaves=1 == Perlin, detail grows with octaves,
 *      degenerate octaves/lacunarity/gain stay finite).
 *   5. Continuity (no jumps) along a fine-stepped line.
 *   6. Perlin lattice property (near-zero at integer lattice points).
 *   7. Finiteness (no NaN/inf) across all functions.
 *
 * C11, -Wall -Wextra clean. No rand() (a small deterministic LCG is used).
 */

#include "noise.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

#define CHECK_NEAR(a, b, eps, msg) CHECK(fabs((a) - (b)) <= (eps), msg)

/* ------------------------------------------------------------------ */
/* Deterministic sampling (no rand())                                  */
/* ------------------------------------------------------------------ */

static uint32_t g_lcg = 0x12345678u;

static uint32_t lcg_next(void)
{
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg;
}

/* Uniform double in [0, 1) built from the top 24 bits (deterministic). */
static double lcg_unit(void)
{
    return (double)(lcg_next() >> 8) / 16777216.0;
}

static double lcg_range(double lo, double hi)
{
    return lo + (hi - lo) * lcg_unit();
}

/* ------------------------------------------------------------------ */
/* Uniform evaluation wrapper over all noise functions                 */
/* ------------------------------------------------------------------ */

enum {
    F_VALUE1, F_VALUE2, F_VALUE3,
    F_PERLIN2, F_PERLIN3,
    F_FBM2, F_FBM3, F_TURB2,
    F_COUNT
};

static const char *func_name(int id)
{
    switch (id) {
    case F_VALUE1:  return "noise_value1";
    case F_VALUE2:  return "noise_value2";
    case F_VALUE3:  return "noise_value3";
    case F_PERLIN2: return "noise_perlin2";
    case F_PERLIN3: return "noise_perlin3";
    case F_FBM2:    return "noise_fbm2";
    case F_FBM3:    return "noise_fbm3";
    case F_TURB2:   return "noise_turbulence2";
    default:        return "?";
    }
}

/* Evaluate function `id` at coordinates c[0..2] with the given seed. */
static double eval(int id, const double c[3], unsigned seed)
{
    switch (id) {
    case F_VALUE1:  return noise_value1(c[0], seed);
    case F_VALUE2:  return noise_value2(c[0], c[1], seed);
    case F_VALUE3:  return noise_value3(c[0], c[1], c[2], seed);
    case F_PERLIN2: return noise_perlin2(c[0], c[1], seed);
    case F_PERLIN3: return noise_perlin3(c[0], c[1], c[2], seed);
    case F_FBM2:    return noise_fbm2(c[0], c[1], 4, 2.0, 0.5, seed);
    case F_FBM3:    return noise_fbm3(c[0], c[1], c[2], 4, 2.0, 0.5, seed);
    case F_TURB2:   return noise_turbulence2(c[0], c[1], 4, 2.0, 0.5, seed);
    default:        return 0.0;
    }
}

/* Bit-identical comparison (NaN-free inputs assumed). */
static int bits_equal(double a, double b)
{
    uint64_t ua, ub;
    memcpy(&ua, &a, sizeof ua);
    memcpy(&ub, &b, sizeof ub);
    return ua == ub;
}

/* ------------------------------------------------------------------ */
/* Test 1: determinism                                                 */
/* ------------------------------------------------------------------ */

#define DET_N 2000

static void test_determinism(void)
{
    double (*cx)[3] = (double (*)[3])malloc(sizeof(double) * 3 * DET_N);
    double *r1 = (double *)malloc(sizeof(double) * DET_N);
    double *r2 = (double *)malloc(sizeof(double) * DET_N);
    int id, i;
    int alloc_ok = (cx != NULL && r1 != NULL && r2 != NULL);

    CHECK(alloc_ok, "alloc determinism buffers");
    if (!alloc_ok) {
        free(cx); free(r1); free(r2);
        return;
    }

    for (id = 0; id < F_COUNT; ++id) {
        char msg[128];
        int mismatch = 0;

        g_lcg = 0xC0FFEEu;
        for (i = 0; i < DET_N; ++i) {
            cx[i][0] = lcg_range(-50.0, 50.0);
            cx[i][1] = lcg_range(-50.0, 50.0);
            cx[i][2] = lcg_range(-50.0, 50.0);
        }

        /* Same inputs + seed twice -> bit-identical. */
        for (i = 0; i < DET_N; ++i) r1[i] = eval(id, cx[i], 7u);
        for (i = 0; i < DET_N; ++i) r2[i] = eval(id, cx[i], 7u);
        for (i = 0; i < DET_N; ++i) {
            if (!bits_equal(r1[i], r2[i])) mismatch++;
        }
        snprintf(msg, sizeof msg, "%s: repeated evaluation is bit-identical",
                 func_name(id));
        CHECK(mismatch == 0, msg);

        /* Evaluate again in reverse order; each value must be unchanged
         * (proves there is no hidden global mutable state). */
        mismatch = 0;
        for (i = 0; i < DET_N; ++i) r2[i] = 0.0;
        for (i = DET_N - 1; i >= 0; --i) r2[i] = eval(id, cx[i], 7u);
        for (i = 0; i < DET_N; ++i) {
            if (!bits_equal(r1[i], r2[i])) mismatch++;
        }
        snprintf(msg, sizeof msg, "%s: order-independent (no global state)",
                 func_name(id));
        CHECK(mismatch == 0, msg);
    }

    free(cx);
    free(r1);
    free(r2);
}

/* ------------------------------------------------------------------ */
/* Test 2: seed sensitivity                                            */
/* ------------------------------------------------------------------ */

#define SEED_N 4000

static void test_seed_sensitivity(void)
{
    double c[3];
    int id;

    for (id = 0; id < F_COUNT; ++id) {
        char msg[128];
        int diff = 0, i;

        g_lcg = 0xABCDEFu;
        for (i = 0; i < SEED_N; ++i) {
            double a, b;
            c[0] = lcg_range(-30.0, 30.0);
            c[1] = lcg_range(-30.0, 30.0);
            c[2] = lcg_range(-30.0, 30.0);
            a = eval(id, c, 1u);
            b = eval(id, c, 2u);
            if (fabs(a - b) > 1e-9) diff++;
        }
        snprintf(msg, sizeof msg, "%s: seed 1 vs 2 differ on >=90%% of samples",
                 func_name(id));
        CHECK((double)diff / (double)SEED_N >= 0.90, msg);
    }
}

/* ------------------------------------------------------------------ */
/* Test 3: range + non-triviality                                      */
/* ------------------------------------------------------------------ */

#define RANGE_N 200000

static void test_ranges(void)
{
    int id;

    for (id = 0; id < F_COUNT; ++id) {
        char msg[128];
        int i;
        double mn = 1e300, mx = -1e300;
        double sum = 0.0, sumsq = 0.0;
        double lo, hi, sd;

        g_lcg = 0x5EEDu;
        for (i = 0; i < RANGE_N; ++i) {
            double c[3];
            double v;
            c[0] = lcg_range(-100.0, 100.0);
            c[1] = lcg_range(-100.0, 100.0);
            c[2] = lcg_range(-100.0, 100.0);
            v = eval(id, c, 12345u);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
            sumsq += v * v;
        }
        {
            double mean = sum / (double)RANGE_N;
            double var = sumsq / (double)RANGE_N - mean * mean;
            sd = (var > 0.0) ? sqrt(var) : 0.0;
        }

        /* Value and Perlin noise are clamped to [-1, 1]. */
        if (id == F_VALUE1 || id == F_VALUE2 || id == F_VALUE3 ||
            id == F_PERLIN2 || id == F_PERLIN3) {
            lo = -1.0; hi = 1.0;
            snprintf(msg, sizeof msg, "%s: within [-1, 1]", func_name(id));
            CHECK(mn >= lo - 1e-12 && mx <= hi + 1e-12, msg);
        } else if (id == F_FBM2 || id == F_FBM3) {
            lo = -1.0; hi = 1.0;
            snprintf(msg, sizeof msg, "%s: within [-1, 1]", func_name(id));
            CHECK(mn >= lo - 1e-9 && mx <= hi + 1e-9, msg);
            snprintf(msg, sizeof msg, "%s: non-trivial (stddev > 0.05)",
                     func_name(id));
            CHECK(sd > 0.05, msg);
        } else { /* turbulence */
            lo = 0.0; hi = 1.0;
            snprintf(msg, sizeof msg, "%s: within [0, 1]", func_name(id));
            CHECK(mn >= lo - 1e-12 && mx <= hi + 1e-12, msg);
            snprintf(msg, sizeof msg, "%s: non-trivial (stddev > 0.02)",
                     func_name(id));
            CHECK(sd > 0.02, msg);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Test 4: fBm parameter behaviour                                     */
/* ------------------------------------------------------------------ */

/* Mean |f(x+0.01,y) - f(x,y)| for noise_fbm2 with the given octave count. */
static double mean_step_diff(int octaves, int n)
{
    int i;
    double acc = 0.0;
    double x = 3.7, y = -2.3;

    for (i = 0; i < n; ++i) {
        double a = noise_fbm2(x, y, octaves, 2.0, 0.5, 7u);
        double b = noise_fbm2(x + 0.01, y, octaves, 2.0, 0.5, 7u);
        acc += fabs(b - a);
        x += 0.013;
        y += 0.007;
    }
    return acc / (double)n;
}

static void test_fbm_parameters(void)
{
    int i;
    int single_ok = 1;

    /* octaves=1 must equal Perlin at the same coordinates. */
    g_lcg = 0xF00Du;
    for (i = 0; i < 2000; ++i) {
        double x = lcg_range(-40.0, 40.0);
        double y = lcg_range(-40.0, 40.0);
        double f = noise_fbm2(x, y, 1, 2.0, 0.5, 99u);
        double p = noise_perlin2(x, y, 99u);
        if (fabs(f - p) > 1e-9) {
            single_ok = 0;
            break;
        }
    }
    CHECK(single_ok, "noise_fbm2(octaves=1) == noise_perlin2");

    /* Increasing octaves increases high-frequency detail. */
    {
        double d1 = mean_step_diff(1, 6000);
        double d6 = mean_step_diff(6, 6000);
        CHECK(d6 > d1, "octaves=6 has more detail than octaves=1");
    }

    /* Degenerate parameters still produce finite results (no crash). */
    {
        double a = noise_fbm2(1.2, 3.4, 0, 2.0, 0.5, 1u);
        double b = noise_fbm2(1.2, 3.4, -3, 2.0, 0.5, 1u);
        double c = noise_fbm2(1.2, 3.4, 4, 0.0, 0.5, 1u);
        double d = noise_fbm2(1.2, 3.4, 4, 2.0, 0.0, 1u);
        double e = noise_fbm2(1.2, 3.4, 4, -1.0, -1.0, 1u);
        double f = noise_fbm3(1.2, 3.4, 5.6, 0, 0.0, 0.0, 1u);
        double g = noise_turbulence2(1.2, 3.4, 0, 0.0, 0.0, 1u);

        CHECK(isfinite(a) && isfinite(b) && isfinite(c) &&
              isfinite(d) && isfinite(e) && isfinite(f) && isfinite(g),
              "degenerate octaves/lacunarity/gain stay finite");
        CHECK_NEAR(a, b, 1e-12, "octaves<=0 treated as 1 (same as octaves=1)");
        CHECK(c >= -1.0 && c <= 1.0 && d >= -1.0 && d <= 1.0 && e >= -1.0 && e <= 1.0,
              "degenerate params keep fBm in range");
        CHECK(g >= 0.0 && g <= 1.0, "degenerate turbulence in [0, 1]");
    }
}

/* ------------------------------------------------------------------ */
/* Test 5: continuity                                                  */
/* ------------------------------------------------------------------ */

#define CONT_N 4000
#define CONT_STEP 1e-4

static void test_continuity(void)
{
    int id;

    for (id = 0; id < F_COUNT; ++id) {
        char msg[128];
        int i;
        double t0 = 1.2345;
        double prev;
        double maxdiff = 0.0;

        /* Initialise "previous" from the FIRST sample, not coordinate 0. */
        prev = eval(id, (double[3]){ t0, t0 * 0.7, t0 * 0.3 }, 1337u);

        for (i = 1; i <= CONT_N; ++i) {
            double t = t0 + (double)i * CONT_STEP;
            double c[3];
            double v, d;
            c[0] = t;
            c[1] = t * 0.7;
            c[2] = t * 0.3;
            v = eval(id, c, 1337u);
            d = fabs(v - prev);
            if (d > maxdiff) maxdiff = d;
            prev = v;
        }

        snprintf(msg, sizeof msg, "%s: continuous (max step diff < 1e-3)",
                 func_name(id));
        CHECK(maxdiff < 1e-3, msg);
    }
}

/* ------------------------------------------------------------------ */
/* Test 6: Perlin lattice property                                     */
/* ------------------------------------------------------------------ */

static void test_perlin_lattice(void)
{
    int ix, iy;
    double maxabs = 0.0;
    double maxabs3 = 0.0;

    for (ix = -6; ix <= 6; ++ix) {
        for (iy = -6; iy <= 6; ++iy) {
            double v2 = noise_perlin2((double)ix, (double)iy, 42u);
            double v3 = noise_perlin3((double)ix, (double)iy, (double)(ix - iy), 42u);
            if (fabs(v2) > maxabs) maxabs = fabs(v2);
            if (fabs(v3) > maxabs3) maxabs3 = fabs(v3);
        }
    }
    CHECK(maxabs <= 1e-6, "noise_perlin2 ~ 0 at integer lattice points");
    CHECK(maxabs3 <= 1e-6, "noise_perlin3 ~ 0 at integer lattice points");
}

/* ------------------------------------------------------------------ */
/* Test 7: finiteness (no NaN / inf)                                   */
/* ------------------------------------------------------------------ */

static void test_finite(void)
{
    int id;

    for (id = 0; id < F_COUNT; ++id) {
        char msg[128];
        int i, allfinite = 1;

        g_lcg = 0xDEADBEEFu;
        for (i = 0; i < 50000; ++i) {
            double c[3];
            c[0] = lcg_range(-1000.0, 1000.0);
            c[1] = lcg_range(-1000.0, 1000.0);
            c[2] = lcg_range(-1000.0, 1000.0);
            if (!isfinite(eval(id, c, (unsigned)lcg_next()))) {
                allfinite = 0;
                break;
            }
        }
        snprintf(msg, sizeof msg, "%s: no NaN/inf across samples", func_name(id));
        CHECK(allfinite, msg);
    }

    /* Also exercise the parameterised variants with varied octave counts. */
    {
        int allfinite = 1;
        int oc;
        for (oc = -2; oc <= 8; ++oc) {
            int i;
            for (i = 0; i < 200; ++i) {
                double x = lcg_range(-50.0, 50.0);
                double y = lcg_range(-50.0, 50.0);
                double z = lcg_range(-50.0, 50.0);
                if (!isfinite(noise_fbm2(x, y, oc, 2.0, 0.5, 3u)) ||
                    !isfinite(noise_fbm3(x, y, z, oc, 2.0, 0.5, 3u)) ||
                    !isfinite(noise_turbulence2(x, y, oc, 2.0, 0.5, 3u))) {
                    allfinite = 0;
                }
            }
        }
        CHECK(allfinite, "fbm/turbulence finite across octave range");
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_determinism();
    test_seed_sensitivity();
    test_ranges();
    test_fbm_parameters();
    test_continuity();
    test_perlin_lattice();
    test_finite();

    printf("test_noise.c: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
