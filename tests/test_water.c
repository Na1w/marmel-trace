/*
 * tests/test_water.c - Regression tests for the water helpers in
 * src/material.c / src/material.h:
 *
 *   water_attenuate(m, inner, depth)  (NEW Beer-Lambert blend)
 *   water_normal(x, z, time)          (wave normal; step reduced to 0.02 m)
 *
 * water_attenuate contract:
 *     T_c      = exp(-absorption_c * depth),  T_c = 1 if absorption_c == 0
 *     result_c = inner_c * T_c + deep_color_c * (1 - T_c)
 *   - depth 0        -> exactly `inner`
 *   - depth -> inf   -> `deep_color` (within 1e-12 at depth 1e9)
 *   - monotone convergence toward `deep_color` as depth grows
 *   - zero absorption -> identity for any depth
 *   - negative / NaN depth -> treated as 0 (identity), all channels finite
 *   - never NaN/Inf, even for extreme inputs
 *
 * water_normal contract:
 *   - always a UNIT vector (|n| within ~1e-9 of 1.0) and finite over a grid
 *     of (x, z, time),
 *   - responds at the ~0.02 m scale: the finite-difference step was reduced
 *     from ~1.43 m to 0.02 m, so a 2 cm offset must measurably change the
 *     normal (a large-offset response alone is NOT sufficient evidence).
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own main() and returns non-zero
 * on any failure. C11, -Wall -Wextra clean. No rand().
 */

#include "material.h"
#include "vec3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Test harness (same style as the existing tests)                     */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } } while (0)

#define CHECK_NEAR(a, b, eps, msg) CHECK(fabs((a) - (b)) <= (eps), msg)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int finite_vec(Vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static int vec_near(Vec3 a, Vec3 b, double eps)
{
    return fabs(a.x - b.x) <= eps &&
           fabs(a.y - b.y) <= eps &&
           fabs(a.z - b.z) <= eps;
}

/* Build a water material with per-channel absorption and a deep color. */
static Material make_water(Vec3 absorption, Vec3 deep)
{
    Material m;
    m.albedo       = vec3(0.1, 0.3, 0.4);
    m.specular     = vec3(1.0, 1.0, 1.0);
    m.shininess    = 128.0;
    m.reflectivity = 0.0;
    m.transparency = 0.9;
    m.ior          = 1.33;
    m.is_water     = 1;
    m.absorption   = absorption;
    m.deep_color   = deep;
    return m;
}

/* ------------------------------------------------------------------ */
/* 1. depth 0 -> exactly inner; huge depth -> deep_color              */
/* ------------------------------------------------------------------ */

static void test_depth_limits(void)
{
    Material m = make_water(vec3(0.35, 0.12, 0.06), vec3(0.02, 0.10, 0.16));
    Vec3 inner = vec3(0.9, 0.7, 0.5);

    Vec3 at0 = water_attenuate(&m, inner, 0.0);
    CHECK(vec_near(at0, inner, 0.0), "attenuate: depth 0 returns exactly inner");

    Vec3 deep = water_attenuate(&m, inner, 1e9);
    CHECK(vec_near(deep, m.deep_color, 1e-12),
          "attenuate: depth 1e9 converges to deep_color (1e-12)");
    CHECK(finite_vec(deep), "attenuate: deep result finite");

    /* A very large but finite depth likewise converges. */
    Vec3 deep2 = water_attenuate(&m, inner, 1e6);
    CHECK(vec_near(deep2, m.deep_color, 1e-9),
          "attenuate: depth 1e6 converges to deep_color");
}

/* ------------------------------------------------------------------ */
/* 2. Monotone convergence toward deep_color                          */
/* ------------------------------------------------------------------ */

static void test_monotone_convergence(void)
{
    Material m = make_water(vec3(0.5, 0.25, 0.1), vec3(0.0, 0.05, 0.2));
    /* inner brighter than deep_color on every channel -> each channel must
     * decrease monotonically toward deep_color as depth grows. */
    Vec3 inner = vec3(1.0, 0.9, 0.8);

    double depths[] = { 0.0, 0.1, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0,
                        1e4, 1e6 };
    const int n = (int)(sizeof(depths) / sizeof(depths[0]));

    Vec3 prev = water_attenuate(&m, inner, depths[0]);
    int monotone = 1;
    int toward_deep = 1;

    for (int i = 1; i < n; ++i) {
        Vec3 cur = water_attenuate(&m, inner, depths[i]);
        /* Each channel must not increase (it moves toward the darker deep). */
        if (cur.x > prev.x + 1e-15 || cur.y > prev.y + 1e-15 ||
            cur.z > prev.z + 1e-15) monotone = 0;
        /* Distance to deep_color must not increase. */
        double dprev = fabs(prev.x - m.deep_color.x) +
                       fabs(prev.y - m.deep_color.y) +
                       fabs(prev.z - m.deep_color.z);
        double dcur  = fabs(cur.x - m.deep_color.x) +
                       fabs(cur.y - m.deep_color.y) +
                       fabs(cur.z - m.deep_color.z);
        if (dcur > dprev + 1e-15) toward_deep = 0;
        prev = cur;
    }

    CHECK(monotone, "attenuate: channels converge monotonically");
    CHECK(toward_deep, "attenuate: distance to deep_color decreases monotonically");
    CHECK(vec_near(prev, m.deep_color, 1e-12),
          "attenuate: final depth reached deep_color");
}

/* ------------------------------------------------------------------ */
/* 3. Zero absorption -> identity for any depth                        */
/* ------------------------------------------------------------------ */

static void test_zero_absorption_identity(void)
{
    Material m = make_water(vec3(0.0, 0.0, 0.0), vec3(0.0, 0.05, 0.2));
    Vec3 inner = vec3(0.42, 0.13, 0.77);

    double depths[] = { 0.0, 1.0, 100.0, 1e6, 1e9 };
    for (int i = 0; i < (int)(sizeof(depths) / sizeof(depths[0])); ++i) {
        Vec3 r = water_attenuate(&m, inner, depths[i]);
        CHECK(vec_near(r, inner, 0.0),
              "attenuate: zero absorption is identity at any depth");
    }

    /* Partial zero absorption: only the non-zero channel moves. */
    Material pm = make_water(vec3(0.0, 1.0, 0.0), vec3(0.1, 0.2, 0.3));
    Vec3 r = water_attenuate(&pm, vec3(1.0, 1.0, 1.0), 1000.0);
    CHECK_NEAR(r.x, 1.0, 0.0, "attenuate: zero-absorption channel stays put");
    CHECK(r.y < 1.0 && fabs(r.y - 0.2) < 1e-9,
          "attenuate: absorbing channel converges to deep_color");
    CHECK_NEAR(r.z, 1.0, 0.0, "attenuate: second zero-absorption channel stays put");
}

/* ------------------------------------------------------------------ */
/* 4. Negative / NaN depth -> identity                                 */
/* ------------------------------------------------------------------ */

static void test_bad_depth(void)
{
    Material m = make_water(vec3(0.5, 0.3, 0.2), vec3(0.0, 0.0, 0.0));
    Vec3 inner = vec3(0.8, 0.6, 0.4);

    Vec3 neg = water_attenuate(&m, inner, -1.0);
    CHECK(vec_near(neg, inner, 0.0), "attenuate: negative depth is identity");
    CHECK(finite_vec(neg), "attenuate: negative depth result finite");

    Vec3 nan = water_attenuate(&m, inner, NAN);
    CHECK(vec_near(nan, inner, 0.0), "attenuate: NaN depth is identity");
    CHECK(finite_vec(nan), "attenuate: NaN depth result finite");

    Vec3 ninf = water_attenuate(&m, inner, -INFINITY);
    CHECK(vec_near(ninf, inner, 0.0), "attenuate: -Inf depth is identity");
    CHECK(finite_vec(ninf), "attenuate: -Inf depth result finite");

    /* +Inf depth: the spec says T = exp(-Inf) = 0, i.e. full convergence to
     * deep_color (no NaN/Inf). */
    Vec3 pinf = water_attenuate(&m, inner, INFINITY);
    CHECK(finite_vec(pinf), "attenuate: +Inf depth result finite");
    CHECK(vec_near(pinf, m.deep_color, 0.0),
          "attenuate: +Inf depth converges exactly to deep_color");
}

/* ------------------------------------------------------------------ */
/* 5. Extreme inputs stay finite (no NaN/Inf)                          */
/* ------------------------------------------------------------------ */

static void test_extremes_finite(void)
{
    /* Extreme absorption coefficients (finite; note that -1e6 * 1e9 overflows
     * to +Inf which water_transmittance maps to T = 0, so the blend stays
     * finite), extreme inner and deep colors. */
    Material m = make_water(vec3(1e6, 1e-6, 1e12), vec3(1e6, 0.0, 1e3));
    Vec3 inners[] = {
        vec3(0.0, 0.0, 0.0),
        vec3(1e6, 1e6, 1e6),
        vec3(0.0, 1e-3, 0.5),
        vec3(1e-9, 1.0, 1.0),
        vec3(1e6, 0.5, 0.25)
    };
    double depths[] = { 0.0, 1e-12, 1.0, 1e3, 1e9, -1.0, NAN, INFINITY };

    int bad = 0;
    for (int i = 0; i < (int)(sizeof(inners) / sizeof(inners[0])); ++i) {
        for (int j = 0; j < (int)(sizeof(depths) / sizeof(depths[0])); ++j) {
            Vec3 r = water_attenuate(&m, inners[i], depths[j]);
            if (!finite_vec(r) || r.x < 0.0 || r.y < 0.0 || r.z < 0.0) bad++;
        }
    }
    CHECK(bad == 0, "attenuate: extreme inputs stay finite and non-negative");

    /* NULL material is handled defensively (documented zero result). */
    Vec3 z = water_attenuate(NULL, vec3(1.0, 1.0, 1.0), 1.0);
    CHECK(z.x == 0.0 && z.y == 0.0 && z.z == 0.0,
          "attenuate: NULL material returns zero");
}

/* ------------------------------------------------------------------ */
/* 6. water_normal: unit, finite over a grid                           */
/* ------------------------------------------------------------------ */

static void test_normal_unit_grid(void)
{
    int bad_unit = 0, bad_finite = 0, bad_y = 0, samples = 0;

    for (int ix = -10; ix <= 10; ++ix) {
        for (int iz = -10; iz <= 10; ++iz) {
            for (int it = 0; it <= 4; ++it) {
                double x = ix * 0.37;
                double z = iz * 0.53;
                double t = it * 0.75;
                Vec3 n = water_normal(x, z, t);
                ++samples;
                if (!finite_vec(n)) bad_finite++;
                if (fabs(vec3_length(n) - 1.0) > 1e-9) bad_unit++;
                if (n.y < 0.0) bad_y++;
            }
        }
    }

    CHECK(samples > 0, "water_normal: grid produced samples");
    CHECK(bad_finite == 0, "water_normal: finite over the whole grid");
    CHECK(bad_unit == 0, "water_normal: unit length (1e-9) over the whole grid");
    CHECK(bad_y == 0, "water_normal: always points generally +Y");
}

/* ------------------------------------------------------------------ */
/* 7. water_normal: responds at the ~0.02 m scale                      */
/* ------------------------------------------------------------------ */

static void test_normal_subcentimetre_response(void)
{
    /* The finite-difference step is 0.02 m. A 2 cm horizontal offset must
     * measurably change the normal; a test that only probes large offsets
     * would still pass even if the step were the old ~1.43 m one. */
    const double eps_step = 0.02;
    const double thresh   = 1e-5;

    double max_d_small = 0.0;
    double max_d_large = 0.0;
    int small_nonzero = 0;
    int samples = 0;

    for (int ix = 0; ix < 40; ++ix) {
        for (int iz = 0; iz < 40; ++iz) {
            double x = ix * 0.031 + 0.0113;
            double z = iz * 0.047 + 0.0071;
            double t = 0.0;

            Vec3 n0 = water_normal(x, z, t);
            Vec3 n1 = water_normal(x + eps_step, z, t);
            Vec3 n2 = water_normal(x, z + eps_step, t);

            double dx = vec3_length(vec3_sub(n1, n0));
            double dz = vec3_length(vec3_sub(n2, n0));
            double d_small = dx > dz ? dx : dz;

            /* Much larger offset, for contrast. */
            Vec3 n3 = water_normal(x + 1.0, z + 1.0, t);
            double d_large = vec3_length(vec3_sub(n3, n0));

            if (d_small > max_d_small) max_d_small = d_small;
            if (d_large > max_d_large) max_d_large = d_large;
            if (d_small > thresh) small_nonzero++;
            ++samples;
        }
    }

    CHECK(samples > 0, "water_normal: sub-cm sweep produced samples");
    CHECK(small_nonzero > 0,
          "water_normal: varies at the 0.02 m scale (max |dn| > 1e-5)");
    CHECK(max_d_small > thresh,
          "water_normal: measurable 2 cm response (max delta > 1e-5)");

    /* The 2 cm response must be a real signal, not merely numerical dust:
     * it should be a non-trivial fraction of the large-offset response. */
    CHECK(max_d_large > thresh, "water_normal: large-offset response is non-trivial");
    CHECK(max_d_small > 0.05 * max_d_large,
          "water_normal: 2 cm response comparable to large-offset response");

    printf("tests/test_water: normal sub-cm response: %d/%d samples > 1e-5, "
           "max|dn(0.02m)|=%.6g, max|dn(1m)|=%.6g\n",
           small_nonzero, samples, max_d_small, max_d_large);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_depth_limits();
    test_monotone_convergence();
    test_zero_absorption_identity();
    test_bad_depth();
    test_extremes_finite();
    test_normal_unit_grid();
    test_normal_subcentimetre_response();

    printf("tests/test_water: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
