/*
 * tests/test_pathtrace_sampling.c - Focused UNIT tests for the pure Monte-Carlo
 *                                   sampling module used by the unidirectional
 *                                   path tracer (src/sampling.{h,c}).
 *
 * The Makefile links every test source in tests/ against all project objects
 * except src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure. C11, -Wall -Wextra clean, and fully
 * deterministic (a small LCG, NEVER rand(), never the clock).
 *
 * Coverage (path-tracer oriented edge cases):
 *   1. Cosine-weighted hemisphere: every sample around several normals (incl.
 *      +/-Y and a tilted unit normal) is unit length and in the +n hemisphere;
 *      the empirical mean direction ~ (2/3) n; sampling_cosine_pdf == cos/PI
 *      for representative cosines and exactly 0 for cos <= 0.
 *   2. Uniform sphere: unit length, roughly isotropic (mean ~ 0 and P(z>0) ~
 *      0.5), and deterministic (same u1,u2 -> bit-identical output).
 *   3. Orthonormal basis (sampling_basis): t, b, n mutually orthogonal, unit
 *      length and right-handed for several normals (incl. +/-Y and near
 *      degenerate), and byte-identical to the shared sky_basis convention.
 *   4. Schlick Fresnel: F(1, f0) == f0, F(0, f0) == 1, monotonic non-increasing
 *      in cos_theta, and per-channel RGB correctness.
 *   5. Dielectric Fresnel + refract: normal-incidence reflectance equals
 *      ((1-eta)/(1+eta))^2 for eta both > 1 and < 1; reflectance -> 1 at
 *      grazing; total internal reflection (dense -> rare) gives reflectance 1
 *      and sampling_refract returns 0; a valid refraction is unit length,
 *      satisfies Snell's law and lands on the correct side of the surface.
 *   6. PBR lobe selection (sampling_specular_probability): result in (0, 1],
 *      increases with metallic, decreases with roughness (metallic < 1) and at
 *      metallic = 1 the diffuse lobe is fully suppressed (p_spec == 1).
 *   7. GGX half-vector (sampling_ggx_half_vector): unit vector in the +n
 *      hemisphere, deterministic, plus a closed-form check of sampling_ggx_pdf.
 */

#include "sampling.h"
#include "vec3.h"

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness (same style as the other tests)                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

#define PI 3.14159265358979323846

/* Deterministic 64-bit LCG mapped to [0, 1). */
static unsigned long long g_rng = 0xD1B54A32D192ED03ULL;
static double rnd01(void)
{
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((g_rng >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
}

/* Exact (bit-for-bit) vector equality for determinism checks. */
static int vec_eq(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* ------------------------------------------------------------------ */
/* 1. Cosine-weighted hemisphere                                       */
/* ------------------------------------------------------------------ */

static void test_cosine_hemisphere(void)
{
    Vec3 normals[3];
    normals[0] = vec3(0.0, 1.0, 0.0);   /* +Y */
    normals[1] = vec3(0.0, -1.0, 0.0);  /* -Y */
    normals[2] = vec3_normalize(vec3(0.3, 0.8, -0.5)); /* tilted */

    const int N = 20000;
    int all_unit = 1, all_hemi = 1, all_finite = 1;

    for (int k = 0; k < 3; k++) {
        Vec3 n = normals[k];
        for (int i = 0; i < N; i++) {
            Vec3 d = sampling_cosine_hemisphere(n, rnd01(), rnd01());
            if (fabs(vec3_length(d) - 1.0) > 1e-9) all_unit = 0;
            if (vec3_dot(d, n) < -1e-9) all_hemi = 0;
            if (!isfinite(d.x) || !isfinite(d.y) || !isfinite(d.z))
                all_finite = 0;
        }
    }
    CHECK(all_unit, "cosine: all samples unit length");
    CHECK(all_hemi, "cosine: all samples in +n hemisphere");
    CHECK(all_finite, "cosine: all samples finite");

    /* For the cosine distribution E[dir] = (2/3) n. Estimate the mean vector
     * around the tilted normal and compare component-wise. */
    {
        Vec3 n = normals[2];
        Vec3 sum = vec3(0.0, 0.0, 0.0);
        for (int i = 0; i < N; i++) {
            sum = vec3_add(sum, sampling_cosine_hemisphere(n, rnd01(), rnd01()));
        }
        Vec3 mean = vec3_scale(sum, 1.0 / (double)N);
        Vec3 expect = vec3_scale(n, 2.0 / 3.0);
        CHECK(fabs(mean.x - expect.x) < 0.02 &&
              fabs(mean.y - expect.y) < 0.02 &&
              fabs(mean.z - expect.z) < 0.02,
              "cosine: empirical mean ~ (2/3) n");
    }

    /* Analytic PDF: p(cos) = cos / PI, 0 for cos <= 0. */
    CHECK(fabs(sampling_cosine_pdf(1.0) - 1.0 / PI) < 1e-15,
          "cosine pdf: cos=1 -> 1/PI");
    CHECK(fabs(sampling_cosine_pdf(0.5) - 0.5 / PI) < 1e-15,
          "cosine pdf: cos=0.5 -> 0.5/PI");
    CHECK(fabs(sampling_cosine_pdf(0.25) - 0.25 / PI) < 1e-15,
          "cosine pdf: cos=0.25 -> 0.25/PI");
    CHECK(sampling_cosine_pdf(0.0) == 0.0, "cosine pdf: cos=0 -> 0");
    CHECK(sampling_cosine_pdf(-1.0) == 0.0, "cosine pdf: cos<0 -> 0");
}

/* ------------------------------------------------------------------ */
/* 2. Uniform sphere                                                   */
/* ------------------------------------------------------------------ */

static void test_uniform_sphere(void)
{
    const int N = 20000;
    int all_unit = 1, all_finite = 1, pos_z = 0, pos_y = 0;
    Vec3 sum = vec3(0.0, 0.0, 0.0);

    for (int i = 0; i < N; i++) {
        Vec3 d = sampling_uniform_sphere(rnd01(), rnd01());
        if (fabs(vec3_length(d) - 1.0) > 1e-9) all_unit = 0;
        if (!isfinite(d.x) || !isfinite(d.y) || !isfinite(d.z)) all_finite = 0;
        if (d.z >= 0.0) pos_z++;
        if (d.y >= 0.0) pos_y++;
        sum = vec3_add(sum, d);
    }
    CHECK(all_unit, "sphere: all samples unit length");
    CHECK(all_finite, "sphere: all samples finite");

    Vec3 mean = vec3_scale(sum, 1.0 / (double)N);
    CHECK(fabs(mean.x) < 0.02 && fabs(mean.y) < 0.02 && fabs(mean.z) < 0.02,
          "sphere: mean direction ~ 0 (isotropic)");

    /* Isotropic => fraction in each hemisphere ~ 0.5 for any axis. */
    CHECK(fabs((double)pos_z / (double)N - 0.5) < 0.02,
          "sphere: P(z>0) ~ 0.5");
    CHECK(fabs((double)pos_y / (double)N - 0.5) < 0.02,
          "sphere: P(y>0) ~ 0.5");

    /* Determinism: identical uniforms -> bit-identical output. */
    Vec3 a = sampling_uniform_sphere(0.25, 0.75);
    Vec3 b = sampling_uniform_sphere(0.25, 0.75);
    CHECK(vec_eq(a, b), "sphere: deterministic (same u1,u2 -> identical)");
}

/* ------------------------------------------------------------------ */
/* 3. Orthonormal basis                                                */
/* ------------------------------------------------------------------ */

/* Reference re-implementation of the shared sky_basis() convention used in
 * src/material.c: pick the world axis least aligned with n, then Gram-Schmidt. */
static void ref_sky_basis(Vec3 n, Vec3 *t, Vec3 *b)
{
    Vec3 axis;
    if (fabs(n.x) <= fabs(n.y) && fabs(n.x) <= fabs(n.z)) {
        axis = vec3(1.0, 0.0, 0.0);
    } else if (fabs(n.y) <= fabs(n.z)) {
        axis = vec3(0.0, 1.0, 0.0);
    } else {
        axis = vec3(0.0, 0.0, 1.0);
    }
    *t = vec3_normalize(vec3_cross(axis, n));
    *b = vec3_cross(n, *t);
}

static void test_basis(void)
{
    Vec3 normals[] = {
        vec3(0.0, 1.0, 0.0),   vec3(0.0, -1.0, 0.0),
        vec3(1.0, 0.0, 0.0),   vec3(0.0, 0.0, 1.0),
        vec3(0.0, 0.999999999, 1e-5),   /* near +Y */
        vec3(1e-5, -0.999999999, 1e-5), /* near -Y */
        vec3(0.3, 0.8, -0.5),  vec3(-0.6, 0.2, 0.77)
    };
    size_t count = sizeof(normals) / sizeof(normals[0]);

    for (size_t i = 0; i < count; i++) {
        Vec3 n = vec3_normalize(normals[i]);
        Vec3 t, b;
        sampling_basis(n, &t, &b);

        CHECK(fabs(vec3_length(t) - 1.0) < 1e-12, "basis: |t| == 1");
        CHECK(fabs(vec3_length(b) - 1.0) < 1e-12, "basis: |b| == 1");
        CHECK(fabs(vec3_dot(t, b)) < 1e-12, "basis: t . b == 0");
        CHECK(fabs(vec3_dot(t, n)) < 1e-12, "basis: t . n == 0");
        CHECK(fabs(vec3_dot(b, n)) < 1e-12, "basis: b . n == 0");

        /* Right-handed: cross(t, b) == n. */
        Vec3 c = vec3_cross(t, b);
        CHECK(fabs(c.x - n.x) < 1e-12 && fabs(c.y - n.y) < 1e-12 &&
              fabs(c.z - n.z) < 1e-12, "basis: cross(t,b) == n");

        /* Byte-identical to the shared sky_basis frame convention. */
        Vec3 rt, rb;
        ref_sky_basis(n, &rt, &rb);
        CHECK(fabs(t.x - rt.x) < 1e-15 && fabs(t.y - rt.y) < 1e-15 &&
              fabs(t.z - rt.z) < 1e-15, "basis: matches sky_basis t");
        CHECK(fabs(b.x - rb.x) < 1e-15 && fabs(b.y - rb.y) < 1e-15 &&
              fabs(b.z - rb.z) < 1e-15, "basis: matches sky_basis b");
    }

    /* Non-unit normal is normalized internally. */
    {
        Vec3 t, b;
        sampling_basis(vec3(0.0, 5.0, 0.0), &t, &b);
        CHECK(fabs(vec3_length(t) - 1.0) < 1e-12 &&
              fabs(vec3_length(b) - 1.0) < 1e-12 &&
              fabs(vec3_dot(t, b)) < 1e-12,
              "basis: non-unit n normalized");
    }

    /* Degenerate (zero) normal must still yield a well-formed frame. */
    {
        Vec3 t, b;
        sampling_basis(vec3(0.0, 0.0, 0.0), &t, &b);
        CHECK(fabs(vec3_length(t) - 1.0) < 1e-12 &&
              fabs(vec3_length(b) - 1.0) < 1e-12 &&
              fabs(vec3_dot(t, b)) < 1e-12,
              "basis: degenerate n -> well-formed frame");
    }

    /* NULL pointers are a no-op (must not crash). */
    {
        Vec3 t, b;
        sampling_basis(vec3(0.0, 1.0, 0.0), NULL, &b);
        sampling_basis(vec3(0.0, 1.0, 0.0), &t, NULL);
        CHECK(1, "basis: NULL args are a no-op");
    }
}

/* ------------------------------------------------------------------ */
/* 4. Schlick Fresnel                                                  */
/* ------------------------------------------------------------------ */

static void test_fresnel_schlick(void)
{
    const double f0 = 0.04;

    CHECK(fabs(sampling_fresnel_schlick(1.0, f0) - f0) < 1e-15,
          "schlick: F(cos=1) == f0");
    CHECK(fabs(sampling_fresnel_schlick(0.0, f0) - 1.0) < 1e-15,
          "schlick: F(cos=0) == 1.0");

    /* Monotonic non-increasing in cos_theta. */
    {
        int monotonic = 1;
        double prev = sampling_fresnel_schlick(0.0, f0);
        for (int i = 1; i <= 100; i++) {
            double c = (double)i / 100.0;
            double cur = sampling_fresnel_schlick(c, f0);
            if (cur > prev + 1e-15) monotonic = 0;
            prev = cur;
        }
        CHECK(monotonic, "schlick: monotonic non-increasing in cos");
    }

    /* Per-channel RGB correctness. */
    {
        Vec3 f0v = vec3(0.04, 0.5, 0.9);
        Vec3 at1 = sampling_fresnel_schlick_rgb(1.0, f0v);
        CHECK(fabs(at1.x - 0.04) < 1e-15 && fabs(at1.y - 0.5) < 1e-15 &&
              fabs(at1.z - 0.9) < 1e-15, "schlick rgb: F(cos=1) == f0 per chan");

        Vec3 at0 = sampling_fresnel_schlick_rgb(0.0, f0v);
        CHECK(fabs(at0.x - 1.0) < 1e-15 && fabs(at0.y - 1.0) < 1e-15 &&
              fabs(at0.z - 1.0) < 1e-15, "schlick rgb: F(cos=0) == 1 per chan");

        double c = 0.35;
        Vec3 atc = sampling_fresnel_schlick_rgb(c, f0v);
        CHECK(fabs(atc.x - sampling_fresnel_schlick(c, 0.04)) < 1e-15 &&
              fabs(atc.y - sampling_fresnel_schlick(c, 0.5)) < 1e-15 &&
              fabs(atc.z - sampling_fresnel_schlick(c, 0.9)) < 1e-15,
              "schlick rgb: matches scalar per channel");
    }
}

/* ------------------------------------------------------------------ */
/* 5. Dielectric Fresnel + refract                                     */
/* ------------------------------------------------------------------ */

static void test_dielectric(void)
{
    /* Normal incidence closed form for eta > 1 and eta < 1. */
    {
        double eta = 1.5;
        double e = (1.0 - eta) / (1.0 + eta);
        CHECK(fabs(sampling_fresnel_dielectric(1.0, eta) - e * e) < 1e-12,
              "dielectric: normal incidence eta>1 closed form");
    }
    {
        double eta = 0.5;
        double e = (1.0 - eta) / (1.0 + eta);
        CHECK(fabs(sampling_fresnel_dielectric(1.0, eta) - e * e) < 1e-12,
              "dielectric: normal incidence eta<1 closed form");
    }

    /* Grazing incidence -> 1 for eta both > 1 and < 1. */
    CHECK(fabs(sampling_fresnel_dielectric(0.0, 1.5) - 1.0) < 1e-12,
          "dielectric: grazing -> 1 (eta>1)");
    CHECK(fabs(sampling_fresnel_dielectric(0.0, 0.5) - 1.0) < 1e-12,
          "dielectric: grazing -> 1 (eta<1)");

    /* Total internal reflection: dense -> rare (eta = 1.5) at a steep angle
     * (cos_i = 0.3 < 1/eta = 0.667). */
    CHECK(fabs(sampling_fresnel_dielectric(0.3, 1.5) - 1.0) < 1e-12,
          "dielectric: TIR reflectance -> 1");

    /* Reflectance stays in [0, 1] over a sweep. */
    {
        int in_range = 1;
        for (int i = 0; i <= 100; i++) {
            double c = (double)i / 100.0;
            double f = sampling_fresnel_dielectric(c, 1.33);
            if (!(f >= 0.0 && f <= 1.0)) in_range = 0;
        }
        CHECK(in_range, "dielectric: reflectance in [0,1]");
    }

    /* -------- refract -------- */

    /* Convention: `n` faces the incident side, so dot(incident, n) <= 0.
     * Incident points toward +Y, normal faces back toward the incident side. */
    Vec3 n = vec3(0.0, -1.0, 0.0);

    /* Normal incidence, entering a denser medium (eta = 1/1.5 < 1): straight
     * through, unit length. */
    {
        Vec3 in = vec3(0.0, 1.0, 0.0);
        Vec3 out;
        int ok = sampling_refract(in, n, 1.0 / 1.5, &out);
        CHECK(ok == 1, "refract: normal incidence succeeds");
        CHECK(fabs(vec3_length(out) - 1.0) < 1e-12, "refract: unit output");
        CHECK(fabs(out.x) < 1e-12 && fabs(out.z) < 1e-12 &&
              fabs(out.y - 1.0) < 1e-12, "refract: normal incidence straight");
    }

    /* Snell's law + unit length + correct side over many angles. */
    {
        double eta = 1.0 / 1.5;
        int snell_ok = 1, unit_ok = 1, side_ok = 1;
        for (int i = 1; i < 60; i++) {
            double ang = (double)i / 60.0 * (PI * 0.49);
            Vec3 d = vec3(sin(ang), cos(ang), 0.0); /* unit, dot(d,n) < 0 */
            Vec3 t;
            if (sampling_refract(d, n, eta, &t) == 1) {
                double cos_i = -vec3_dot(d, n);
                double sin_i = sqrt(1.0 - cos_i * cos_i);
                double cos_t = -vec3_dot(t, n);
                double sin_t = sqrt(fmax(0.0, 1.0 - cos_t * cos_t));
                /* eta_i * sin_i == eta_t * sin_t  <=>  sin_t == eta * sin_i. */
                if (fabs(sin_t - eta * sin_i) > 1e-9) snell_ok = 0;
                if (fabs(vec3_length(t) - 1.0) > 1e-12) unit_ok = 0;
                /* Transmitted direction continues into the far side. */
                if (vec3_dot(t, n) > 1e-9) side_ok = 0;
            }
        }
        CHECK(snell_ok, "refract: satisfies Snell's law");
        CHECK(unit_ok, "refract: transmitted always unit length");
        CHECK(side_ok, "refract: transmitted on the correct side");
    }

    /* Total internal reflection: dense -> rare (eta = 1.5) at a steep angle
     * (sin_i = sin(1.2) = 0.932 > 1/1.5) -> 0. */
    {
        Vec3 d = vec3(sin(1.2), cos(1.2), 0.0);
        Vec3 t;
        CHECK(sampling_refract(d, n, 1.5, &t) == 0,
              "refract: TIR returns 0");
    }

    /* NULL output pointer is a failure, not a crash. */
    {
        Vec3 in = vec3(0.0, 1.0, 0.0);
        CHECK(sampling_refract(in, n, 1.0, NULL) == 0,
              "refract: NULL out -> 0");
    }
}

/* ------------------------------------------------------------------ */
/* 6. PBR lobe selection                                               */
/* ------------------------------------------------------------------ */

static void test_specular_probability(void)
{
    const double f0 = 0.04;

    /* Range (0, 1] over a dense (metallic, roughness) sweep. */
    {
        int in_range = 1;
        for (int i = 0; i <= 10; i++) {
            for (int j = 0; j <= 10; j++) {
                double p = sampling_specular_probability(i / 10.0, j / 10.0, f0);
                if (!(p > 0.0 && p <= 1.0)) in_range = 0;
            }
        }
        CHECK(in_range, "pspec: in (0,1] over sweep");
    }

    /* Increases with metallic. */
    CHECK(sampling_specular_probability(0.8, 0.5, f0) >
          sampling_specular_probability(0.2, 0.5, f0),
          "pspec: increases with metallic");

    /* Decreases with roughness for metallic < 1. */
    CHECK(sampling_specular_probability(0.0, 0.1, f0) >
          sampling_specular_probability(0.0, 0.9, f0),
          "pspec: decreases with roughness (metallic<1)");
    CHECK(sampling_specular_probability(0.5, 0.1, f0) >
          sampling_specular_probability(0.5, 0.9, f0),
          "pspec: decreases with roughness (metallic=0.5)");

    /* metallic = 1 fully suppresses the diffuse lobe: p_spec == 1 exactly,
     * independent of roughness. */
    CHECK(sampling_specular_probability(1.0, 0.0, f0) == 1.0,
          "pspec: metallic=1 -> 1 (smooth)");
    CHECK(sampling_specular_probability(1.0, 0.5, f0) == 1.0,
          "pspec: metallic=1 -> 1 (rough)");
    CHECK(sampling_specular_probability(1.0, 1.0, f0) == 1.0,
          "pspec: metallic=1 -> 1 (fully rough)");
}

/* ------------------------------------------------------------------ */
/* 7. GGX half-vector + PDF                                            */
/* ------------------------------------------------------------------ */

static void test_ggx(void)
{
    Vec3 n = vec3_normalize(vec3(0.2, 0.8, 0.4));

    /* Unit length + +n hemisphere over a dense (alpha, u1, u2) sweep. */
    {
        int unit_ok = 1, hemi_ok = 1;
        double alphas[] = { 0.0, 0.1, 0.5, 1.0 };
        for (size_t a = 0; a < sizeof(alphas) / sizeof(alphas[0]); a++) {
            for (int i = 0; i < 4000; i++) {
                Vec3 h = sampling_ggx_half_vector(n, alphas[a], rnd01(), rnd01());
                if (fabs(vec3_length(h) - 1.0) > 1e-9) unit_ok = 0;
                if (vec3_dot(h, n) < -1e-9) hemi_ok = 0;
            }
        }
        CHECK(unit_ok, "ggx: half-vector unit length");
        CHECK(hemi_ok, "ggx: half-vector in +n hemisphere");
    }

    /* Determinism: identical inputs -> bit-identical output. */
    {
        Vec3 a = sampling_ggx_half_vector(n, 0.3, 0.4, 0.6);
        Vec3 b = sampling_ggx_half_vector(n, 0.3, 0.4, 0.6);
        CHECK(vec_eq(a, b), "ggx: deterministic (same inputs -> identical)");
    }

    /* Closed-form PDF sanity. For alpha = 1 the NDF is D(H) = 1/PI, so
     * p(L) = D * n_dot_h / (4 * h_dot_v). */
    {
        double alpha = 1.0;
        double ndh = 0.5, hdv = 0.25;
        double expect = (1.0 / PI) * ndh / (4.0 * hdv);
        CHECK(fabs(sampling_ggx_pdf(ndh, hdv, alpha) - expect) < 1e-15,
              "ggx pdf: alpha=1 closed form");
    }

    /* Degenerate guards. */
    CHECK(sampling_ggx_pdf(0.5, 0.0, 0.4) == 0.0, "ggx pdf: h_dot_v=0 -> 0");
    CHECK(sampling_ggx_pdf(0.0, 0.5, 0.4) == 0.0, "ggx pdf: n_dot_h=0 -> 0");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_cosine_hemisphere();
    test_uniform_sphere();
    test_basis();
    test_fresnel_schlick();
    test_dielectric();
    test_specular_probability();
    test_ggx();

    printf("test_pathtrace_sampling: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
