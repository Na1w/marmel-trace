/*
 * tests/test_softshadow.c - Unit tests for sun-disk (area-light) sampling
 *                           used by soft shadows.
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns 0 on
 * success / non-zero on any failure.
 *
 * Coverage:
 *   1. sky_sun_disk_dir() returns UNIT vectors for every sample (radius > 0).
 *   2. Every perturbed direction lies WITHIN the cone of half-angle
 *      `radius_deg` around sun_dir (exact, since the cone offset is
 *      atan(tan(sqrt(r1)*radius_rad)) == sqrt(r1)*radius_rad).
 *   3. radius == 0 returns sun_dir EXACTLY (bit-for-bit), and radius < 0 too.
 *   4. Determinism: identical inputs give identical output, and a fresh call
 *      after other calls is unaffected (no hidden state, no rand()/clock).
 *   5. r1 == 0 gives the central direction (disk centre); the sampled set
 *      reaches out toward the full radius (non-degenerate disk).
 *   6. Edge cases: degenerate zero sun_dir stays finite/unit; NULL-free pure
 *      math with out-of-range r1/r2 clamped safely.
 *   7. Scene-format plumbing: `sun_radius` parses, survives a load -> write ->
 *      load round-trip with the exact same value, and the canonical writer
 *      OMITS it when it equals the default (0).
 *
 * C11, -Wall -Wextra clean. No rand(). All heap memory is freed.
 */

#include "material.h"
#include "vec3.h"
#include "scene_desc.h"

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

#define TEST_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int vec3_eq(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* A non-axis-aligned unit sun direction so the basis code is exercised. */
static Vec3 sample_sun_dir(void)
{
    return vec3_normalize(vec3(0.45, 0.75, -0.50));
}

/* Angle in radians between two unit vectors. */
static double angle_between(Vec3 a, Vec3 b)
{
    double d = vec3_dot(a, b);
    if (d > 1.0) d = 1.0;
    if (d < -1.0) d = -1.0;
    return acos(d);
}

/* ------------------------------------------------------------------ */
/* Test 1 + 2: unit length and within-cone for radius > 0              */
/* ------------------------------------------------------------------ */

static void test_within_cone(void)
{
    Vec3 sun = sample_sun_dir();
    const double radius_deg = 2.5;               /* a visibly soft sun */
    const double radius_rad = radius_deg * (TEST_PI / 180.0);
    const double tol = 1e-9;
    int unit_ok = 1;
    int cone_ok = 1;
    double max_angle = 0.0;

    /* A dense deterministic sweep over the [0,1) unit square. */
    for (int i = 0; i < 64; ++i) {
        for (int j = 0; j < 64; ++j) {
            double r1 = ((double)i + 0.5) / 64.0;
            double r2 = ((double)j + 0.5) / 64.0;
            Vec3 d = sky_sun_disk_dir(sun, radius_deg, r1, r2);

            if (fabs(vec3_length(d) - 1.0) > 1e-9) {
                unit_ok = 0;
            }
            double ang = angle_between(d, sun);
            if (ang > max_angle) {
                max_angle = ang;
            }
            if (ang > radius_rad + tol) {
                cone_ok = 0;
            }
        }
    }

    CHECK(unit_ok, "sky_sun_disk_dir returns unit vectors for radius > 0");
    CHECK(cone_ok, "every perturbed direction lies within the sun cone");
    /* The disk is non-degenerate: the sweep must actually reach the rim. */
    CHECK(max_angle > 0.9 * radius_rad,
          "sampled directions reach out toward the full sun radius");
}

/* ------------------------------------------------------------------ */
/* Test 3: radius <= 0 is the exact hard-shadow direction              */
/* ------------------------------------------------------------------ */

static void test_zero_radius_is_identity(void)
{
    Vec3 sun = sample_sun_dir();

    CHECK(vec3_eq(sky_sun_disk_dir(sun, 0.0, 0.0, 0.0), sun),
          "radius 0 returns sun_dir exactly (r1=r2=0)");
    CHECK(vec3_eq(sky_sun_disk_dir(sun, 0.0, 0.3, 0.9), sun),
          "radius 0 returns sun_dir exactly (generic r1,r2)");
    CHECK(vec3_eq(sky_sun_disk_dir(sun, -1.0, 0.5, 0.5), sun),
          "negative radius returns sun_dir exactly");

    /* sky_default_params() must set the point-sun default. */
    SkyParams p;
    sky_default_params(&p);
    CHECK(p.sun_radius == 0.0, "sky_default_params sets sun_radius = 0");

    /* Center sample (r1 = 0) is the unperturbed direction for any radius. */
    CHECK(vec3_eq(sky_sun_disk_dir(sun, 3.0, 0.0, 0.25), sun),
          "r1 = 0 gives the disk centre (sun_dir)");
}

/* ------------------------------------------------------------------ */
/* Test 4: determinism                                                 */
/* ------------------------------------------------------------------ */

static void test_determinism(void)
{
    Vec3 sun = sample_sun_dir();
    Vec3 a = sky_sun_disk_dir(sun, 1.75, 0.137, 0.842);
    Vec3 b = sky_sun_disk_dir(sun, 1.75, 0.137, 0.842);

    CHECK(vec3_eq(a, b), "identical inputs give bit-identical output");

    /* Interleave unrelated calls: no hidden mutable state may leak through. */
    for (int i = 0; i < 32; ++i) {
        (void)sky_sun_disk_dir(sun, 5.0, (double)i / 32.0, 0.5);
    }
    Vec3 c = sky_sun_disk_dir(sun, 1.75, 0.137, 0.842);
    CHECK(vec3_eq(a, c), "result is stable after unrelated calls");

    /* Different r2 must generally give a different direction. */
    Vec3 d = sky_sun_disk_dir(sun, 1.75, 0.137, 0.111);
    CHECK(!vec3_eq(a, d), "different r2 gives a different direction");
}

/* ------------------------------------------------------------------ */
/* Test 5: robustness / edge cases                                     */
/* ------------------------------------------------------------------ */

static void test_edge_cases(void)
{
    Vec3 sun = sample_sun_dir();

    /* Out-of-range r1/r2 are clamped, never producing NaN/Inf. */
    Vec3 hi = sky_sun_disk_dir(sun, 2.0, 5.0, 5.0);
    Vec3 lo = sky_sun_disk_dir(sun, 2.0, -3.0, -3.0);
    CHECK(isfinite(hi.x) && isfinite(hi.y) && isfinite(hi.z),
          "r > 1 clamped: finite result");
    CHECK(isfinite(lo.x) && isfinite(lo.y) && isfinite(lo.z),
          "r < 0 clamped: finite result");
    CHECK(fabs(vec3_length(hi) - 1.0) < 1e-9, "clamped (hi) result is unit");
    CHECK(fabs(vec3_length(lo) - 1.0) < 1e-9, "clamped (lo) result is unit");

    /* Degenerate zero sun_dir must still yield a finite unit vector. */
    Vec3 z = sky_sun_disk_dir(vec3(0.0, 0.0, 0.0), 2.0, 0.4, 0.6);
    CHECK(isfinite(z.x) && isfinite(z.y) && isfinite(z.z),
          "zero sun_dir gives a finite direction");
    CHECK(fabs(vec3_length(z) - 1.0) < 1e-9,
          "zero sun_dir gives a unit direction");

    /* A sun along a world axis exercises the least-aligned-axis basis pick. */
    Vec3 axis_dirs[3] = {
        vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0)
    };
    for (int i = 0; i < 3; ++i) {
        Vec3 d = sky_sun_disk_dir(axis_dirs[i], 1.0, 0.8, 0.3);
        CHECK(fabs(vec3_length(d) - 1.0) < 1e-9,
              "axis-aligned sun_dir yields a unit direction");
    }
}

/* ------------------------------------------------------------------ */
/* Test 6: scene-format plumbing (parse + conditional write)           */
/* ------------------------------------------------------------------ */

static const char *SS_SCENE =
    "# soft shadow round-trip\n"
    "water_level = 0.02\n"
    "water_material = none\n"
    "water_enabled = 0\n"
    "\n"
    "sky {\n"
    "    sun_dir = 0.45 0.75 -0.50\n"
    "    sun_color = 1 0.95 0.85\n"
    "    horizon_color = 0.75 0.85 1\n"
    "    zenith_color = 0.35 0.55 0.95\n"
    "    gradient_gamma = 0.6\n"
    "    sun_glow_exponent = 350\n"
    "    sun_glow_strength = 0.8\n"
    "    cloud_height = 120\n"
    "    cloud_scale = 0.0025\n"
    "    cloud_coverage = 0.5\n"
    "    cloud_softness = 0.12\n"
    "    cloud_sharpness = 1.5\n"
    "    cloud_octaves = 5\n"
    "    seed = 1337\n"
    "    sun_radius = 1.25\n"
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

static const char *SS_SCENE_DEFAULT_RADIUS =
    "# soft shadow default radius\n"
    "water_level = 0.02\n"
    "water_material = none\n"
    "water_enabled = 0\n"
    "\n"
    "sky {\n"
    "    sun_dir = 0.45 0.75 -0.50\n"
    "    sun_color = 1 0.95 0.85\n"
    "    horizon_color = 0.75 0.85 1\n"
    "    zenith_color = 0.35 0.55 0.95\n"
    "    gradient_gamma = 0.6\n"
    "    sun_glow_exponent = 350\n"
    "    sun_glow_strength = 0.8\n"
    "    cloud_height = 120\n"
    "    cloud_scale = 0.0025\n"
    "    cloud_coverage = 0.5\n"
    "    cloud_softness = 0.12\n"
    "    cloud_sharpness = 1.5\n"
    "    cloud_octaves = 5\n"
    "    seed = 1337\n"
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

static void test_parser_roundtrip(void)
{
    const char *tmp_out = "/tmp/ss_test_softshadow_out.scene";
    SceneDesc d1, d2;
    char errbuf[256];

    scene_desc_init(&d1);
    scene_desc_init(&d2);

    CHECK(scene_desc_load_string(&d1, SS_SCENE, "<test>", errbuf, sizeof errbuf) == 0,
          "parse scene with sun_radius succeeds");
    CHECK(d1.has_sky == 1, "sky block present");
    CHECK(d1.sky.sun_radius == 1.25, "sun_radius parsed as 1.25");

    /* write (canonical) then re-parse: value must survive bit-for-bit. */
    CHECK(scene_desc_write(&d1, tmp_out, errbuf, sizeof errbuf) == 0,
          "write scene with sun_radius succeeds");

    char *text = slurp(tmp_out);
    CHECK(text != NULL, "read back written scene");
    if (text != NULL) {
        CHECK(strstr(text, "sun_radius = 1.25") != NULL,
              "writer emits non-default sun_radius");
        free(text);
    }

    CHECK(scene_desc_load(&d2, tmp_out, errbuf, sizeof errbuf) == 0,
          "re-parse written scene succeeds");
    CHECK(d2.sky.sun_radius == d1.sky.sun_radius,
          "sun_radius round-trips bit-for-bit");

    scene_desc_free(&d1);
    scene_desc_free(&d2);
}

static void test_writer_omits_default(void)
{
    const char *tmp_out = "/tmp/ss_test_softshadow_default_out.scene";
    SceneDesc d;
    char errbuf[256];

    scene_desc_init(&d);
    CHECK(scene_desc_load_string(&d, SS_SCENE_DEFAULT_RADIUS, "<test>",
                                 errbuf, sizeof errbuf) == 0,
          "parse scene without sun_radius succeeds");
    CHECK(d.sky.sun_radius == 0.0, "absent sun_radius defaults to 0");

    CHECK(scene_desc_write(&d, tmp_out, errbuf, sizeof errbuf) == 0,
          "write scene with default sun_radius succeeds");

    char *text = slurp(tmp_out);
    CHECK(text != NULL, "read back written default scene");
    if (text != NULL) {
        CHECK(strstr(text, "sun_radius") == NULL,
              "writer omits sun_radius when it equals the default (0)");
        free(text);
    }

    scene_desc_free(&d);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_within_cone();
    test_zero_radius_is_identity();
    test_determinism();
    test_edge_cases();
    test_parser_roundtrip();
    test_writer_omits_default();

    fprintf(stderr, "test_softshadow: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
