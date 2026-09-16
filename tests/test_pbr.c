/*
 * tests/test_pbr.c - Unit tests for the opt-in physically-based
 *                    Cook-Torrance microfacet layer:
 *
 *   - `material_shade_pbr()`          (src/material.c)
 *   - `fresnel_schlick` / `_rgb`      (src/material.c)
 *   - `metallic`/`roughness`/`emissive`/`pbr` keys and the
 *     `metallic`/`roughness`/`emissive`/`pbr` scene schema
 *                                     (src/scene_desc.c, src/scene_desc_write.c)
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure. C11, -Wall -Wextra clean, no rand()
 * (a small LCG keeps the importance-sampling estimator deterministic).
 *
 * Coverage (the named `type =` PRESET field values are covered by
 * tests/test_material_presets.c and are deliberately NOT re-tested here):
 *   1. Structural contract of `material_shade_pbr`: NULL / NdotL <= 0 /
 *      NdotV <= 0 all return black; determinism; finiteness / non-negativity.
 *   2. Energy conservation: hemispherical reflectance rho(V) <= 1 over a
 *      (albedo, metallic, roughness, NdotV) sweep that includes the high-albedo
 *      dielectrics (0.85, 1.0) and every shipped preset albedo, using a GGX
 *      importance-sampled estimator with a fixed seed. Also covers the shipped
 *      ceramic_red scene material (albedo 0.72 0.18 0.14).
 *   3. Roughness monotonicity of the specular highlight peak (smooth > rough).
 *   4. Metalness: metallic = 1 removes the diffuse lobe; metallic = 0 uses the
 *      flat 4 % dielectric F0; metallic is clamped to [0, 1].
 *   5. Fresnel endpoints: F(cos=1) == F0 and F(cos=0) == 1.
 *   6. Parsing of `metallic` / `roughness` / `emissive` / `pbr`: explicit
 *      values, defaults when absent, and explicit-key override of a preset.
 *   7. Writer: the four keys are emitted ONLY when non-default; a PBR material
 *      round-trips parse(write(m)) == m; a default material emits none of them.
 *   8. Regression: `material_shade_local` (legacy Blinn-Phong) is unchanged for
 *      a representative input (golden values from an independent reference).
 */

#include "material.h"
#include "vec3.h"
#include "scene_desc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness (same style as the other tests)                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

static const double PI = 3.14159265358979323846;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Deterministic LCG (no rand(), reproducible across platforms/runs). */
static unsigned g_rng = 123456789u;
static double rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (double)(g_rng >> 8) * (1.0 / 16777216.0);
}

/* A fully-initialised PBR material with an all-important legacy-neutral base. */
static Material make_mat(double metallic, double roughness)
{
    Material m;
    m.albedo = vec3(0.9, 0.7, 0.5);
    m.specular = vec3(0.5, 0.5, 0.5);
    m.shininess = 64;
    m.reflectivity = 0.0;
    m.transparency = 0.0;
    m.ior = 1.0;
    m.is_water = 0;
    m.beer_lambert = 0;
    m.absorption = vec3(0.0, 0.0, 0.0);
    m.deep_color = vec3(0.0, 0.0, 0.0);
    m.metallic = metallic;
    m.roughness = roughness;
    m.emissive = vec3(0.0, 0.0, 0.0);
    m.pbr = 1;
    material_texture_defaults(&m);
    return m;
}

static int vec3_is_black(Vec3 v)
{
    return v.x == 0.0 && v.y == 0.0 && v.z == 0.0;
}

static int vec3_eq(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* Full structural equality of every Material field (for round-trip checks). */
static int material_eq(const Material *a, const Material *b)
{
    return vec3_eq(a->albedo, b->albedo) &&
           vec3_eq(a->specular, b->specular) &&
           a->shininess == b->shininess &&
           a->reflectivity == b->reflectivity &&
           a->transparency == b->transparency &&
           a->ior == b->ior &&
           a->is_water == b->is_water &&
           a->beer_lambert == b->beer_lambert &&
           vec3_eq(a->absorption, b->absorption) &&
           vec3_eq(a->deep_color, b->deep_color) &&
           a->metallic == b->metallic &&
           a->roughness == b->roughness &&
           vec3_eq(a->emissive, b->emissive) &&
           a->pbr == b->pbr &&
           a->texture_kind == b->texture_kind &&
           a->texture_scale == b->texture_scale &&
           vec3_eq(a->texture_color_a, b->texture_color_a) &&
           vec3_eq(a->texture_color_b, b->texture_color_b);
}

/* GGX NDF, used only to build the importance-sampling pdf. */
static double ggx_d(double ndh, double a)
{
    double a2 = a * a;
    double d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

/* ------------------------------------------------------------------ */
/* 1. Structural contract of material_shade_pbr                        */
/* ------------------------------------------------------------------ */

static void test_structural_contract(void)
{
    Vec3 N = vec3(0.0, 1.0, 0.0);
    Vec3 L = vec3(0.0, 1.0, 0.0);
    Vec3 V = vec3(0.0, 1.0, 0.0);
    Vec3 lc = vec3(1.0, 1.0, 1.0);

    CHECK(vec3_is_black(material_shade_pbr(NULL, N, L, V, lc)),
          "NULL material returns black");

    Material m = make_mat(0.5, 0.4);

    /* Light behind the surface: NdotL < 0 and the NdotL == 0 boundary. */
    CHECK(vec3_is_black(material_shade_pbr(&m, N, vec3(0.0, -1.0, 0.0), V, lc)),
          "NdotL < 0 returns black");
    CHECK(vec3_is_black(material_shade_pbr(&m, N, vec3(1.0, 0.0, 0.0), V, lc)),
          "NdotL == 0 returns black");

    /* Viewer at/below the surface horizon: no response. */
    CHECK(vec3_is_black(material_shade_pbr(&m, N, L, vec3(0.0, -1.0, 0.0), lc)),
          "NdotV < 0 returns black");
    CHECK(vec3_is_black(material_shade_pbr(&m, N, L, vec3(1.0, 0.0, 0.0), lc)),
          "NdotV == 0 returns black");
}

static void test_determinism(void)
{
    Vec3 N = vec3(0.0, 1.0, 0.0);
    Vec3 lc = vec3(1.0, 0.95, 0.9);
    int ok = 1;

    for (int mi = 0; mi <= 10; ++mi) {
        for (int ri = 0; ri <= 20; ++ri) {
            Material m = make_mat(mi / 10.0, ri / 20.0);
            for (int s = 0; s < 300; ++s) {
                double ct = rnd();
                double st = sqrt(1.0 - ct * ct);
                double ph = 2.0 * PI * rnd();
                Vec3 L = vec3(st * cos(ph), ct, st * sin(ph));
                Vec3 V = vec3(0.0, 1.0, 0.0);
                Vec3 a = material_shade_pbr(&m, N, L, V, lc);
                Vec3 b = material_shade_pbr(&m, N, L, V, lc);
                if (a.x != b.x || a.y != b.y || a.z != b.z) ok = 0;
            }
        }
    }
    CHECK(ok, "material_shade_pbr is bit-exactly deterministic");
}

static void test_finite_nonneg(void)
{
    Vec3 N = vec3(0.0, 1.0, 0.0);
    Vec3 lc = vec3(1.0, 1.0, 1.0);
    int finite_ok = 1, nonneg_ok = 1;

    for (int mi = 0; mi <= 10; ++mi) {
        for (int ri = 0; ri <= 20; ++ri) {
            Material m = make_mat(mi / 10.0, ri / 20.0);
            for (int s = 0; s < 500; ++s) {
                double ct = rnd();
                double st = sqrt(1.0 - ct * ct);
                double ph = 2.0 * PI * rnd();
                Vec3 L = vec3(st * cos(ph), ct, st * sin(ph));
                Vec3 V = vec3(0.0, 1.0, 0.0);
                Vec3 o = material_shade_pbr(&m, N, L, V, lc);
                if (!isfinite(o.x) || !isfinite(o.y) || !isfinite(o.z))
                    finite_ok = 0;
                if (o.x < 0.0 || o.y < 0.0 || o.z < 0.0) nonneg_ok = 0;
            }
        }
    }
    CHECK(finite_ok, "output is finite across the (metallic, roughness) grid");
    CHECK(nonneg_ok, "output is non-negative");

    /* roughness = 0 at NdotH = 1 must stay finite (alpha is clamped). */
    Material mirror = make_mat(1.0, 0.0);
    Vec3 o = material_shade_pbr(&mirror, N, N, N, lc);
    CHECK(isfinite(o.x) && isfinite(o.y) && isfinite(o.z),
          "roughness = 0 mirror limit is finite (guarded alpha)");
}

/* ------------------------------------------------------------------ */
/* 2. Energy conservation (hemispherical reflectance <= 1)             */
/* ------------------------------------------------------------------ */

/*
 * rho(V) = integral f(L,V) * NdotL dOmega, evaluated per RGB channel and
 * reported as the largest channel (the worst case for energy conservation).
 *
 * With the energy-conserving weighting the diffuse lobe integrates
 * ANALYTICALLY to kD * albedo, where
 *
 *      kD = (1 - F(NdotV)) * (1 - metallic)
 *
 * (F is constant w.r.t. L, so integral (kD*albedo/PI) * NdotL dOmega collapses
 * to kD*albedo). The specular lobe is estimated by importance-sampling the GGX
 * half-vector and using the standard estimator mean( NdotL * f_spec / pdf_L ).
 * The analytic diffuse term is subtracted from the shaded output to isolate the
 * specular lobe exactly. A fixed seed keeps the reported number reproducible.
 */
static double hemi_reflectance(Vec3 albedo, double metallic, double roughness,
                               double ndv, int nsamples)
{
    Vec3 N = vec3(0.0, 1.0, 0.0);
    Vec3 lc = vec3(1.0, 1.0, 1.0);
    Material m = make_mat(metallic, roughness);
    m.albedo = albedo;

    double st_v = sqrt(fmax(0.0, 1.0 - ndv * ndv));
    Vec3 V = vec3(st_v, ndv, 0.0);

    double r = roughness;
    double a = r * r;
    if (a < 1e-4) a = 1e-4;
    double a2 = a * a;

    /* Analytic diffuse contribution: rho_diff = kD * albedo. */
    Vec3 f0 = vec3_lerp(vec3(0.04, 0.04, 0.04), albedo, metallic);
    Vec3 F = fresnel_schlick_rgb(ndv, f0);
    Vec3 kd = vec3_scale(vec3_sub(vec3(1.0, 1.0, 1.0), F), (1.0 - metallic));

    double sum_x = 0.0, sum_y = 0.0, sum_z = 0.0;
    for (int s = 0; s < nsamples; ++s) {
        double u1 = rnd();
        double u2 = rnd();
        double ct = sqrt((1.0 - u1) / (1.0 + (a2 - 1.0) * u1));
        double st = sqrt(fmax(0.0, 1.0 - ct * ct));
        double ph = 2.0 * PI * u2;
        Vec3 H = vec3(st * cos(ph), ct, st * sin(ph));
        double vdh = vec3_dot(V, H);
        if (vdh <= 1e-9) continue;
        Vec3 L = vec3_sub(vec3_scale(H, 2.0 * vdh), V);
        double ndl = vec3_dot(N, L);
        if (ndl <= 0.0) continue;
        double pdf = ggx_d(ct, a) * ct / (4.0 * vdh);
        if (pdf <= 0.0) continue;

        Vec3 lo = material_shade_pbr(&m, N, L, V, lc);
        /* Remove the exact analytic diffuse term to isolate f_spec. */
        double fd_x = kd.x * albedo.x / PI;
        double fd_y = kd.y * albedo.y / PI;
        double fd_z = kd.z * albedo.z / PI;
        sum_x += (lo.x - ndl * fd_x) / pdf;
        sum_y += (lo.y - ndl * fd_y) / pdf;
        sum_z += (lo.z - ndl * fd_z) / pdf;
    }

    double rho_x = kd.x * albedo.x + sum_x / (double)nsamples;
    double rho_y = kd.y * albedo.y + sum_y / (double)nsamples;
    double rho_z = kd.z * albedo.z + sum_z / (double)nsamples;
    return fmax(rho_x, fmax(rho_y, rho_z));
}

/*
 * Albedos swept by the energy-conservation test: the two high-albedo
 * dielectric regression cases (0.85 and 1.0) plus every shipped
 * `type = <preset>` conductor/dielectric albedo.
 */
static const double PBR_TEST_ALBEDOS[][3] = {
    { 0.85,  0.85,  0.85  }, /* high-albedo dielectric (was ~1.31)      */
    { 1.00,  1.00,  1.00  }, /* extreme high-albedo dielectric          */
    { 1.000, 0.766, 0.336 }, /* gold                                    */
    { 0.955, 0.637, 0.538 }, /* copper                                  */
    { 0.972, 0.960, 0.915 }, /* silver                                  */
    { 0.913, 0.921, 0.925 }, /* aluminum                                */
    { 0.560, 0.570, 0.580 }, /* iron                                    */
    { 0.550, 0.556, 0.554 }, /* chrome                                  */
    { 0.910, 0.778, 0.423 }, /* brass                                   */
    { 0.30,  0.05,  0.06  }, /* plastic                                 */
    { 0.05,  0.05,  0.05  }, /* rubber                                  */
    { 0.85,  0.85,  0.82  }, /* ceramic                                 */
    { 0.02,  0.02,  0.02  }, /* diamond                                 */
    { 0.00,  0.00,  0.00  }, /* emissive (black albedo)                 */
};

#define PBR_TEST_ALBEDO_COUNT \
    (sizeof PBR_TEST_ALBEDOS / sizeof PBR_TEST_ALBEDOS[0])

static void test_energy_conservation(void)
{
    static const double metallic_vals[]  = { 0.0, 1.0 };
    static const double roughness_vals[] = { 0.03, 0.05, 0.10, 0.20, 0.30,
                                             0.40, 0.50, 0.60, 0.70, 0.80,
                                             0.90, 1.00 };
    static const double ndv_vals[]       = { 1.0, 0.7, 0.4, 0.15 };

    const int NS = 20000;
    const double eps = 1e-3;
    double worst = 0.0;
    double worst_085 = 0.0, worst_100 = 0.0;
    int ok = 1, finite = 1;

    g_rng = 123456789u; /* fixed seed -> deterministic estimator */
    for (size_t i = 0; i < PBR_TEST_ALBEDO_COUNT; ++i) {
        Vec3 albedo = vec3(PBR_TEST_ALBEDOS[i][0],
                           PBR_TEST_ALBEDOS[i][1],
                           PBR_TEST_ALBEDOS[i][2]);
        for (size_t j = 0; j < sizeof roughness_vals / sizeof roughness_vals[0]; ++j) {
            for (size_t k = 0; k < sizeof metallic_vals / sizeof metallic_vals[0]; ++k) {
                for (size_t l = 0; l < sizeof ndv_vals / sizeof ndv_vals[0]; ++l) {
                    double rho = hemi_reflectance(albedo, metallic_vals[k],
                                                  roughness_vals[j],
                                                  ndv_vals[l], NS);
                    if (!isfinite(rho)) finite = 0;
                    if (rho > worst) worst = rho;
                    /* Track the dielectric (metallic = 0) worst case: that is
                     * the configuration the energy-conservation fix targets. */
                    if (metallic_vals[k] == 0.0) {
                        if (i == 0 && rho > worst_085) worst_085 = rho;
                        if (i == 1 && rho > worst_100) worst_100 = rho;
                    }
                    if (!(rho <= 1.0 + eps)) {
                        ok = 0;
                        fprintf(stderr,
                                "FAIL: rho = %.6f > 1 at albedo "
                                "(%.3f,%.3f,%.3f) rough=%.2f metal=%.1f ndv=%.2f\n",
                                rho, albedo.x, albedo.y, albedo.z,
                                roughness_vals[j], metallic_vals[k],
                                ndv_vals[l]);
                    }
                }
            }
        }
    }
    printf("test_pbr: max hemispherical reflectance = %.5f (<= 1 required)\n",
           worst);
    printf("test_pbr:   albedo 0.85 -> %.5f, albedo 1.0 -> %.5f\n",
           worst_085, worst_100);
    CHECK(finite, "hemispherical reflectance is finite across the sweep");
    CHECK(ok, "hemispherical reflectance never exceeds incident energy");
}

/*
 * Regression for the shipped scenes/example_materials.scene `ceramic_red`
 * material (albedo 0.72 0.18 0.14, roughness 0.55, metallic 0), which used to
 * measure rho ~= 1.0192 before the kD compensation.
 */
static void test_energy_conservation_ceramic_red(void)
{
    static const double ndv_vals[] = { 1.0, 0.7, 0.4, 0.15 };
    const int NS = 20000;
    double worst = 0.0;
    int ok = 1;

    g_rng = 123456789u;
    for (size_t l = 0; l < sizeof ndv_vals / sizeof ndv_vals[0]; ++l) {
        double rho = hemi_reflectance(vec3(0.72, 0.18, 0.14), 0.0, 0.55,
                                      ndv_vals[l], NS);
        if (rho > worst) worst = rho;
        if (!(rho <= 1.0 + 1e-3)) ok = 0;
    }
    printf("test_pbr: ceramic_red (0.72/0.55/0.0) max rho = %.5f\n", worst);
    CHECK(ok, "ceramic_red preset albedo is energy conserving");
}

/* ------------------------------------------------------------------ */
/* 3. Roughness monotonicity of the specular highlight peak            */
/* ------------------------------------------------------------------ */

static void test_roughness_monotonicity(void)
{
    Vec3 N = vec3(0.0, 1.0, 0.0);
    Vec3 V = N;
    Vec3 L = N; /* mirror direction of V about N is N */
    Vec3 lc = vec3(1.0, 1.0, 1.0);
    static const double pairs[3][2] = { { 0.02, 0.30 },
                                        { 0.05, 0.60 },
                                        { 0.10, 0.90 } };

    for (int i = 0; i < 3; ++i) {
        Material smooth = make_mat(1.0, pairs[i][0]);
        Material rough  = make_mat(1.0, pairs[i][1]);
        Vec3 ls = material_shade_pbr(&smooth, N, L, V, lc);
        Vec3 lr = material_shade_pbr(&rough,  N, L, V, lc);
        CHECK(ls.x > lr.x,
              "smooth surface has a brighter mirror-direction highlight peak");
    }
}

/* ------------------------------------------------------------------ */
/* 4. Metalness: diffuse vanishes; dielectric F0 ~= 0.04; clamped      */
/* ------------------------------------------------------------------ */

static void test_metalness(void)
{
    Vec3 N = vec3(0.0, 1.0, 0.0);
    Vec3 V = N;
    Vec3 L = N; /* NdotL = NdotV = NdotH = 1, D = 1/(PI*a^2) */
    Vec3 lc = vec3(1.0, 1.0, 1.0);

    /* roughness = 1 -> a = 1 -> D = 1/PI, G = 1, so the closed form is exact.
     * At NdotL = NdotV = NdotH = 1 the Fresnel term is F = F0, hence
     *   kD = (1 - F0) * (1 - metallic), kS = F0
     *   dielectric (F0 = 0.04): lc * ( 0.5*(1-0.04)/PI + 0.04/(4*PI) )
     *   metal      (F0 = albedo, metallic = 1): lc * ( albedo/(4*PI) )
     */
    Material die = make_mat(0.0, 1.0);
    Material met = make_mat(1.0, 1.0);
    die.albedo = vec3(0.5, 0.5, 0.5);
    met.albedo = vec3(0.5, 0.5, 0.5);

    Vec3 ld = material_shade_pbr(&die, N, L, V, lc);
    Vec3 lm = material_shade_pbr(&met, N, L, V, lc);

    double exp_die = 0.5 * (1.0 - 0.04) / PI + 0.04 / (4.0 * PI);
    double exp_met = 0.5 / (4.0 * PI);

    CHECK(fabs(ld.x - exp_die) < 1e-12,
          "dielectric uses albedo diffuse + flat 0.04 dielectric F0");
    CHECK(fabs(lm.x - exp_met) < 1e-12,
          "metallic = 1 uses albedo as F0 with the diffuse term removed");
    CHECK(lm.x < ld.x, "metallic = 1 darkens vs dielectric (diffuse removed)");

    /* metallic = 1, albedo = 0 -> F0 = 0 and no diffuse -> pure black. */
    Material mz = make_mat(1.0, 0.5);
    mz.albedo = vec3(0.0, 0.0, 0.0);
    CHECK(vec3_is_black(material_shade_pbr(&mz, N, L, V, lc)),
          "metallic = 1 with zero albedo has no diffuse term");

    /* metallic is clamped to [0, 1]: 2.0 behaves exactly like 1.0. */
    Material m2 = make_mat(2.0, 1.0);
    m2.albedo = vec3(0.5, 0.5, 0.5);
    Vec3 l2 = material_shade_pbr(&m2, N, L, V, lc);
    CHECK(fabs(l2.x - lm.x) < 1e-15, "metallic > 1 is clamped to 1");
}

/* ------------------------------------------------------------------ */
/* 5. Fresnel endpoints                                                */
/* ------------------------------------------------------------------ */

static void test_fresnel_endpoints(void)
{
    Vec3 f0 = vec3(0.04, 0.5, 0.9);

    Vec3 f_at1 = fresnel_schlick_rgb(1.0, f0);
    CHECK(vec3_eq(f_at1, f0), "fresnel_schlick_rgb(cos = 1, F0) == F0");

    Vec3 f_at0 = fresnel_schlick_rgb(0.0, f0);
    CHECK(f_at0.x == 1.0 && f_at0.y == 1.0 && f_at0.z == 1.0,
          "fresnel_schlick_rgb(cos = 0, F0) == 1");

    /* Scalar variant shares the same endpoints. */
    CHECK(fabs(fresnel_schlick(1.0, 0.04) - 0.04) < 1e-15,
          "fresnel_schlick(cos = 1, f0) == f0");
    CHECK(fabs(fresnel_schlick(0.0, 0.04) - 1.0) < 1e-15,
          "fresnel_schlick(cos = 0, f0) == 1");
}

/* ------------------------------------------------------------------ */
/* Scene parsing helpers                                               */
/* ------------------------------------------------------------------ */

/* Read a whole file into a malloc'd, NUL-terminated buffer (or NULL). */
static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long  sz;
    char *buf;

    if (fp == NULL)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    buf = (char *)malloc((size_t)sz + 1u);
    if (buf == NULL) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    buf[sz] = '\0';
    return buf;
}

/* Pointer to the start of the body of the first `material <name>` block. */
static const char *material_body(const char *text, const char *name)
{
    char needle[128];
    const char *at;

    snprintf(needle, sizeof needle, "material %s {", name);
    at = strstr(text, needle);
    return (at == NULL) ? NULL : strchr(at, '\n');
}

/* 1 if `key` occurs in the block body before its closing brace. */
static int body_has_key(const char *body, const char *key)
{
    const char *close;

    if (body == NULL)
        return 0;
    close = strstr(body, "}\n");
    return strstr(body, key) != NULL &&
           (close == NULL || strstr(body, key) < close);
}

/*
 * Parse a scene declaring exactly one material and copy it into *out.
 * Returns 0 on success, non-zero on parse error / unexpected material count.
 */
static int parse_single_material(const char *text, Material *out,
                                 char *errbuf, size_t errlen)
{
    SceneDesc d;
    int ok = 1;

    scene_desc_init(&d);
    if (scene_desc_load_string(&d, text, "<test>", errbuf, errlen) != 0) {
        ok = 0;
    } else if (d.material_count != 1) {
        ok = 0;
    } else {
        *out = d.materials[0].mat;
    }
    scene_desc_free(&d);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Scene fixtures                                                      */
/* ------------------------------------------------------------------ */

/* Explicit values for all four PBR keys on a NON-preset material. */
static const char *PBR_FULL_SCENE =
    "material pm {\n"
    "    albedo = 0.2 0.3 0.4\n"
    "    metallic = 0.7\n"
    "    roughness = 0.3\n"
    "    emissive = 0.1 0.2 0.3\n"
    "    pbr = 1\n"
    "}\n";

/* A plain material with none of the four keys: they must default. */
static const char *PLAIN_SCENE =
    "material pm {\n"
    "    albedo = 0.5 0.5 0.5\n"
    "}\n";

/* A preset with one PBR key overridden explicitly. */
static const char *GOLD_OVERRIDE_SCENE =
    "material pm {\n"
    "    type = gold\n"
    "    metallic = 0.25\n"
    "}\n";

/* Only `metallic` non-default (used for the writer emission test). */
static const char *ONLY_METALLIC_SCENE =
    "material pm {\n"
    "    albedo = 0.2 0.3 0.4\n"
    "    metallic = 0.5\n"
    "}\n";

/* An invalid boolean for `pbr` (only 0 / 1 are accepted). */
static const char *BAD_PBR_SCENE =
    "material pm {\n"
    "    pbr = 2\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* 6. Parsing of the four new keys                                     */
/* ------------------------------------------------------------------ */

static void test_parse_explicit_keys(void)
{
    Material m;
    char errbuf[256];

    CHECK(parse_single_material(PBR_FULL_SCENE, &m, errbuf, sizeof errbuf) == 0,
          "parse material with all four PBR keys succeeds");
    CHECK(m.metallic == 0.7, "metallic key parsed (0.7)");
    CHECK(m.roughness == 0.3, "roughness key parsed (0.3)");
    CHECK(vec3_eq(m.emissive, vec3(0.1, 0.2, 0.3)), "emissive key parsed");
    CHECK(m.pbr == 1, "pbr key parsed (1)");
    CHECK(vec3_eq(m.albedo, vec3(0.2, 0.3, 0.4)),
          "the other keys are unaffected");
}

static void test_parse_defaults(void)
{
    Material m;
    char errbuf[256];

    CHECK(parse_single_material(PLAIN_SCENE, &m, errbuf, sizeof errbuf) == 0,
          "parse material without any PBR key succeeds");
    CHECK(m.metallic == MATERIAL_DEFAULT_METALLIC,
          "absent metallic defaults to 0");
    CHECK(m.roughness == MATERIAL_DEFAULT_ROUGHNESS,
          "absent roughness defaults to 0");
    CHECK(vec3_eq(m.emissive, vec3(0.0, 0.0, 0.0)),
          "absent emissive defaults to black");
    CHECK(m.pbr == MATERIAL_DEFAULT_PBR, "absent pbr defaults to 0");
}

static void test_parse_preset_override(void)
{
    Material m;
    char errbuf[256];

    CHECK(parse_single_material(GOLD_OVERRIDE_SCENE, &m,
                                errbuf, sizeof errbuf) == 0,
          "parse `type = gold` + explicit `metallic` succeeds");
    CHECK(m.metallic == 0.25, "explicit metallic overrides the gold preset");
    /* The rest of the gold preset survives the overlay. */
    CHECK(m.roughness == 0.05, "preset roughness survives the metallic override");
    CHECK(m.pbr == 1, "preset pbr = 1 survives the metallic override");
    CHECK(vec3_eq(m.albedo, vec3(1.000, 0.766, 0.336)),
          "preset albedo survives the metallic override");
}

static void test_parse_bool_validation(void)
{
    Material m;
    char errbuf[256];

    CHECK(parse_single_material(BAD_PBR_SCENE, &m, errbuf, sizeof errbuf) != 0,
          "pbr = 2 is rejected (only 0 or 1 accepted)");
}

/* ------------------------------------------------------------------ */
/* 7. Writer emission + round-trip                                     */
/* ------------------------------------------------------------------ */

/*
 * Write `scene` to `path` and return the emitted text (caller frees).
 * Returns NULL on any parse/write/read failure.
 */
static char *write_scene(const char *scene, const char *path,
                         char *errbuf, size_t errlen)
{
    SceneDesc d;
    char *text;

    scene_desc_init(&d);
    if (scene_desc_load_string(&d, scene, "<test>", errbuf, errlen) != 0) {
        scene_desc_free(&d);
        return NULL;
    }
    if (scene_desc_write(&d, path, errbuf, errlen) != 0) {
        scene_desc_free(&d);
        return NULL;
    }
    scene_desc_free(&d);
    text = slurp(path);
    remove(path);
    return text;
}

static void test_writer_emits_all_when_non_default(void)
{
    char errbuf[256];
    char *text = write_scene(PBR_FULL_SCENE,
                             "/tmp/rt_test_pbr_all.scene",
                             errbuf, sizeof errbuf);

    CHECK(text != NULL, "write PBR material scene succeeds");
    if (text != NULL) {
        const char *body = material_body(text, "pm");
        CHECK(body_has_key(body, "metallic"), "writer emits metallic (0.7)");
        CHECK(body_has_key(body, "roughness"), "writer emits roughness (0.3)");
        CHECK(body_has_key(body, "emissive"), "writer emits emissive");
        CHECK(body_has_key(body, "pbr"), "writer emits pbr");
        CHECK(strstr(text, "metallic = 0.7") != NULL,
              "metallic value is written as 0.7");
        CHECK(strstr(text, "roughness = 0.3") != NULL,
              "roughness value is written as 0.3");
        CHECK(strstr(text, "emissive = 0.1 0.2 0.3") != NULL,
              "emissive value is written as 0.1 0.2 0.3");
        CHECK(strstr(text, "pbr = 1") != NULL, "pbr value is written as 1");
        free(text);
    }
}

static void test_writer_omits_defaults(void)
{
    char errbuf[256];
    char *text = write_scene(ONLY_METALLIC_SCENE,
                             "/tmp/rt_test_pbr_partial.scene",
                             errbuf, sizeof errbuf);

    CHECK(text != NULL, "write partially-PBR material scene succeeds");
    if (text != NULL) {
        const char *body = material_body(text, "pm");
        CHECK(body_has_key(body, "metallic"),
              "writer emits the non-default metallic key");
        CHECK(body_has_key(body, "roughness") == 0,
              "writer omits the default roughness key");
        CHECK(body_has_key(body, "emissive") == 0,
              "writer omits the default emissive key");
        CHECK(body_has_key(body, "pbr") == 0,
              "writer omits the default pbr key");
        free(text);
    }

    /* A fully-default material (not a named preset) emits none of the four. */
    text = write_scene(PLAIN_SCENE, "/tmp/rt_test_pbr_default.scene",
                       errbuf, sizeof errbuf);
    CHECK(text != NULL, "write default-PBR material scene succeeds");
    if (text != NULL) {
        const char *body = material_body(text, "pm");
        CHECK(body_has_key(body, "metallic") == 0,
              "default material emits no metallic key");
        CHECK(body_has_key(body, "roughness") == 0,
              "default material emits no roughness key");
        CHECK(body_has_key(body, "emissive") == 0,
              "default material emits no emissive key");
        CHECK(body_has_key(body, "pbr") == 0,
              "default material emits no pbr key");
        free(text);
    }
}

/* Write `scene`, re-parse it, and assert the single material round-trips. */
static void roundtrip(const char *scene, const char *path, const char *label,
                      char *errbuf, size_t errlen)
{
    SceneDesc d1, d2;

    scene_desc_init(&d1);
    scene_desc_init(&d2);

    CHECK(scene_desc_load_string(&d1, scene, "<test>", errbuf, errlen) == 0,
          label);
    CHECK(scene_desc_write(&d1, path, errbuf, errlen) == 0,
          "write for round-trip succeeds");
    CHECK(scene_desc_load(&d2, path, errbuf, errlen) == 0 &&
          d2.material_count == d1.material_count,
          "re-parse written scene succeeds with the same material count");
    if (d1.material_count == 1 && d2.material_count == 1) {
        CHECK(material_eq(&d1.materials[0].mat, &d2.materials[0].mat),
              "PBR material round-trips parse(write(m)) == m");
    }

    scene_desc_free(&d1);
    scene_desc_free(&d2);
    remove(path);
}

static void test_roundtrip(void)
{
    char errbuf[256];

    roundtrip(PBR_FULL_SCENE, "/tmp/rt_test_pbr_rt1.scene",
              "round-trip: parse full PBR material", errbuf, sizeof errbuf);
    roundtrip(ONLY_METALLIC_SCENE, "/tmp/rt_test_pbr_rt2.scene",
              "round-trip: parse metallic-only material", errbuf, sizeof errbuf);
    roundtrip(PLAIN_SCENE, "/tmp/rt_test_pbr_rt3.scene",
              "round-trip: parse default-PBR material", errbuf, sizeof errbuf);
}

/* ------------------------------------------------------------------ */
/* 8. Regression: legacy material_shade_local (Blinn-Phong)            */
/* ------------------------------------------------------------------ */

/*
 * Golden values from an independent reference implementation of the legacy
 * formula: N=(0,1,0), L=normalize(0.3,0.9,0.3), V=(0,1,0), lc=(1,1,1),
 * albedo=(0.9,0.7,0.5), specular=(0.5,0.5,0.5), shininess=64.
 *   ndl = 0.90453403373329089
 *   out = (0.91861274932648607, 0.7377059425798278, 0.55679913583316976)
 */
static void test_legacy_shade_regression(void)
{
    Vec3 N = vec3(0.0, 1.0, 0.0);
    Vec3 L = vec3_normalize(vec3(0.3, 0.9, 0.3));
    Vec3 V = vec3(0.0, 1.0, 0.0);
    Vec3 lc = vec3(1.0, 1.0, 1.0);

    Material m = make_mat(0.0, 0.0);
    m.albedo = vec3(0.9, 0.7, 0.5);
    m.specular = vec3(0.5, 0.5, 0.5);
    m.shininess = 64;

    Vec3 o = material_shade_local(&m, N, L, V, lc);

    CHECK(fabs(o.x - 0.91861274932648607) < 1e-12,
          "legacy shade_local x unchanged");
    CHECK(fabs(o.y - 0.7377059425798278) < 1e-12,
          "legacy shade_local y unchanged");
    CHECK(fabs(o.z - 0.55679913583316976) < 1e-12,
          "legacy shade_local z unchanged");

    /* Contract parity: NULL and NdotL <= 0 still return black. */
    CHECK(vec3_is_black(material_shade_local(NULL, N, L, V, lc)),
          "legacy shade_local NULL returns black");
    CHECK(vec3_is_black(material_shade_local(&m, N, vec3(0.0, -1.0, 0.0),
                                             V, lc)),
          "legacy shade_local NdotL <= 0 returns black");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_structural_contract();
    test_determinism();
    test_finite_nonneg();
    test_energy_conservation();
    test_energy_conservation_ceramic_red();
    test_roughness_monotonicity();
    test_metalness();
    test_fresnel_endpoints();
    test_parse_explicit_keys();
    test_parse_defaults();
    test_parse_preset_override();
    test_parse_bool_validation();
    test_writer_emits_all_when_non_default();
    test_writer_omits_defaults();
    test_roundtrip();
    test_legacy_shade_regression();

    fprintf(stderr, "test_pbr: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
