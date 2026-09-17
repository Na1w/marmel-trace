/*
 * tests/test_fog.c - Unit tests for atmospheric fog and smoke.
 *
 * Tests:
 *   1. Default parameters (density == 0 => disabled).
 *   2. Zero density identity (T == 1.0, inscatter == (0,0,0), surface color preserved).
 *   3. Analytic distance fog (exponential Beer-Lambert attenuation along distance).
 *   4. Analytic height fog (exponential decay with height; finite integral towards sky).
 *   5. Forward Mie phase scattering towards the sun.
 *   6. 3D procedural noise turbulence modulation.
 *   7. Scene format parsing, canonical emission, and round-trip fidelity.
 */

#include "material.h"
#include "vec3.h"
#include "scene_desc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

#define CHECK_NEAR(a, b, eps, msg) do { \
    if (fabs((a) - (b)) <= (eps)) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (got %g, expected %g, diff %g > %g at %s:%d)\n", \
                (msg), (double)(a), (double)(b), fabs((double)(a) - (double)(b)), (double)(eps), __FILE__, __LINE__); } \
} while (0)

static void test_defaults(void)
{
    FogParams f;
    fog_default_params(&f);

    CHECK(f.density == 0.0, "default fog density is 0.0");
    CHECK(f.height == 0.0, "default fog height is 0.0");
    CHECK(f.height_falloff == 0.0, "default height_falloff is 0.0");
    CHECK(f.inscatter_strength == 0.50, "default inscatter_strength is 0.50");
    CHECK(f.sun_anisotropy == 0.70, "default sun_anisotropy is 0.70");
    CHECK(f.noise_scale == 0.0, "default noise_scale is 0.0");
    CHECK(f.noise_amount == 0.0, "default noise_amount is 0.0");
}

static void test_zero_density_identity(void)
{
    FogParams f;
    fog_default_params(&f);
    SkyParams sky;
    sky_default_params(&sky);

    Ray r = { vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0) };
    double T = 0.0;
    Vec3 inscatter = vec3(1.0, 1.0, 1.0);

    fog_segment(&f, &sky, r, 50.0, &T, &inscatter);
    CHECK_NEAR(T, 1.0, 1e-9, "T is 1.0 when fog density is 0");
    CHECK_NEAR(inscatter.x, 0.0, 1e-9, "inscatter.x is 0 when fog density is 0");
    CHECK_NEAR(inscatter.y, 0.0, 1e-9, "inscatter.y is 0 when fog density is 0");
    CHECK_NEAR(inscatter.z, 0.0, 1e-9, "inscatter.z is 0 when fog density is 0");

    Vec3 red = vec3(0.8, 0.1, 0.1);
    Vec3 out = fog_apply(&f, &sky, r, 50.0, red);
    CHECK_NEAR(out.x, red.x, 1e-9, "surface color x preserved when fog density is 0");
    CHECK_NEAR(out.y, red.y, 1e-9, "surface color y preserved when fog density is 0");
    CHECK_NEAR(out.z, red.z, 1e-9, "surface color z preserved when fog density is 0");
}

static void test_distance_fog(void)
{
    FogParams f;
    fog_default_params(&f);
    f.density = 0.05;
    f.height_falloff = 0.0; /* uniform distance fog */
    f.color = vec3(0.6, 0.7, 0.8);
    f.inscatter_strength = 0.0; /* pure absorption/extinction */

    SkyParams sky;
    sky_default_params(&sky);

    Ray r = { vec3(0.0, 5.0, 0.0), vec3(0.0, 0.0, 1.0) };
    double T10 = 0.0, T20 = 0.0;
    Vec3 in10, in20;

    fog_segment(&f, &sky, r, 10.0, &T10, &in10);
    fog_segment(&f, &sky, r, 20.0, &T20, &in20);

    /* T = exp(-0.05 * 10) = exp(-0.5) ~= 0.6065 */
    double expected_T10 = exp(-0.5);
    double expected_T20 = exp(-1.0);
    CHECK_NEAR(T10, expected_T10, 1e-4, "uniform distance fog T at 10m");
    CHECK_NEAR(T20, expected_T20, 1e-4, "uniform distance fog T at 20m");
    CHECK(T20 < T10, "transmittance decreases monotonically with distance");

    /* Inscattering = color * (1 - T) */
    CHECK_NEAR(in10.x, f.color.x * (1.0 - expected_T10), 1e-4, "inscattering matches (1-T)*color");
}

static void test_height_fog(void)
{
    FogParams f;
    fog_default_params(&f);
    f.density = 0.10;
    f.height = 0.0;
    f.height_falloff = 0.20; /* decays by e^-0.2 per metre of altitude */
    f.inscatter_strength = 0.0;

    SkyParams sky;
    sky_default_params(&sky);

    /* Horizontal ray at ground y = 0 */
    Ray r_ground = { vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0) };
    /* Horizontal ray high up at y = 10 */
    Ray r_high = { vec3(0.0, 10.0, 0.0), vec3(0.0, 0.0, 1.0) };

    double T_ground = 0.0, T_high = 0.0;
    Vec3 in_g, in_h;
    fog_segment(&f, &sky, r_ground, 10.0, &T_ground, &in_g);
    fog_segment(&f, &sky, r_high, 10.0, &T_high, &in_h);

    /* At y = 0, base density is 0.10 * exp(0) = 0.10, tau = 1.0 -> T = exp(-1) ~= 0.3678 */
    /* At y = 10, base density is 0.10 * exp(-2.0) ~= 0.0135, tau ~= 0.135 -> T = exp(-0.135) ~= 0.8734 */
    CHECK_NEAR(T_ground, exp(-1.0), 1e-4, "height fog at ground level");
    CHECK_NEAR(T_high, exp(-0.10 * exp(-2.0) * 10.0), 1e-4, "height fog at high altitude");
    CHECK(T_high > T_ground, "fog is much clearer at higher altitude than ground");

    /* Ray pointing straight up into the sky from ground: finite optical depth */
    Ray r_up = { vec3(0.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0) };
    double T_sky = 0.0;
    fog_segment(&f, &sky, r_up, 1e30, &T_sky, &in_g);
    /* tau = rho0 / (lambda * dy) = 0.10 / (0.20 * 1.0) = 0.50 -> T = exp(-0.5) */
    CHECK_NEAR(T_sky, exp(-0.5), 1e-4, "upward sky ray has finite optical thickness tau = rho0/lambda");
}

static void test_sun_inscattering(void)
{
    FogParams f;
    fog_default_params(&f);
    f.density = 0.05;
    f.inscatter_strength = 1.0;
    f.sun_anisotropy = 0.75; /* strong forward scattering */

    SkyParams sky;
    sky_default_params(&sky);
    sky.sun_dir = vec3(0.0, 0.0, 1.0);
    sky.sun_color = vec3(2.0, 2.0, 2.0);

    /* Ray looking directly at sun */
    Ray r_sun = { vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0) };
    /* Ray looking directly away from sun */
    Ray r_away = { vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0) };

    double T_sun = 0.0, T_away = 0.0;
    Vec3 in_sun, in_away;
    fog_segment(&f, &sky, r_sun, 20.0, &T_sun, &in_sun);
    fog_segment(&f, &sky, r_away, 20.0, &T_away, &in_away);

    CHECK(in_sun.x > in_away.x * 2.0, "forward Mie scattering creates bright sun glow in fog");
}

static void test_noise_turbulence(void)
{
    FogParams f;
    fog_default_params(&f);
    f.density = 0.05;
    f.noise_scale = 0.5;
    f.noise_amount = 0.8;

    SkyParams sky;
    sky_default_params(&sky);

    Ray r = { vec3(1.2, 3.4, 5.6), vec3_normalize(vec3(1.0, 0.2, -0.5)) };
    double T = 0.0;
    Vec3 inscatter;
    fog_segment(&f, &sky, r, 25.0, &T, &inscatter);

    CHECK(T >= 0.0 && T <= 1.0, "turbulent fog transmittance is within [0, 1]");
    CHECK(!isnan(inscatter.x) && !isnan(inscatter.y) && !isnan(inscatter.z), "inscatter is finite");
}

static void test_scene_desc_roundtrip(void)
{
    const char *scene_src =
        "camera {\n"
        "  eye = 0 2 -10\n"
        "  target = 0 2 0\n"
        "  up = 0 1 0\n"
        "  vfov = 45\n"
        "}\n"
        "sky {\n"
        "  sun_dir = 0.5 0.7 -0.5\n"
        "}\n"
        "fog {\n"
        "  density = 0.0450\n"
        "  color = 0.7200 0.7800 0.8500\n"
        "  height = 0.5000\n"
        "  height_falloff = 0.1500\n"
        "  inscatter_strength = 0.8000\n"
        "  sun_anisotropy = 0.6500\n"
        "  noise_scale = 0.2500\n"
        "  noise_amount = 0.4000\n"
        "}\n";

    SceneDesc d1, d2;
    char errbuf[256];
    scene_desc_init(&d1);
    scene_desc_init(&d2);

    int err = scene_desc_load_string(&d1, scene_src, "<test>", errbuf, sizeof(errbuf));
    CHECK(err == 0, "loaded scene with fog block successfully");
    CHECK(d1.has_fog == 1, "has_fog is 1");
    CHECK_NEAR(d1.fog.density, 0.0450, 1e-4, "parsed fog density");
    CHECK_NEAR(d1.fog.color.x, 0.7200, 1e-4, "parsed fog color.x");
    CHECK_NEAR(d1.fog.height, 0.5000, 1e-4, "parsed fog height");
    CHECK_NEAR(d1.fog.height_falloff, 0.1500, 1e-4, "parsed fog height_falloff");
    CHECK_NEAR(d1.fog.inscatter_strength, 0.8000, 1e-4, "parsed fog inscatter_strength");
    CHECK_NEAR(d1.fog.sun_anisotropy, 0.6500, 1e-4, "parsed fog sun_anisotropy");
    CHECK_NEAR(d1.fog.noise_scale, 0.2500, 1e-4, "parsed fog noise_scale");
    CHECK_NEAR(d1.fog.noise_amount, 0.4000, 1e-4, "parsed fog noise_amount");

    const char *tmp_path = "/tmp/test_fog_out.scene";
    CHECK(scene_desc_write(&d1, tmp_path, errbuf, sizeof(errbuf)) == 0, "wrote scene with fog");

    CHECK(scene_desc_load(&d2, tmp_path, errbuf, sizeof(errbuf)) == 0, "re-loaded written fog scene");
    CHECK(d2.has_fog == 1, "re-loaded has_fog is 1");
    CHECK_NEAR(d2.fog.density, d1.fog.density, 1e-4, "roundtrip fog density");
    CHECK_NEAR(d2.fog.color.x, d1.fog.color.x, 1e-4, "roundtrip fog color.x");
    CHECK_NEAR(d2.fog.height, d1.fog.height, 1e-4, "roundtrip fog height");
    CHECK_NEAR(d2.fog.height_falloff, d1.fog.height_falloff, 1e-4, "roundtrip fog height_falloff");

    scene_desc_free(&d1);
    scene_desc_free(&d2);
    remove(tmp_path);
}

int main(void)
{
    test_defaults();
    test_zero_density_identity();
    test_distance_fog();
    test_height_fog();
    test_sun_inscattering();
    test_noise_turbulence();
    test_scene_desc_roundtrip();

    printf("test_fog: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
