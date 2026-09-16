/*
 * tests/test_sampling.c - Unit tests for the pure sampling foundation module
 *                         (src/sampling.{h,c}).
 *
 * The Makefile links every test source in tests/ against all project objects
 * except src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure. C11, -Wall -Wextra clean,
 * deterministic (a small LCG, no rand()).
 *
 * Coverage:
 *   1. sampling_basis: orthonormality / right-handedness over many normals
 *      (including near +/-Y and axis-aligned), robustness to non-unit n.
 *   2. sampling_cosine_hemisphere: unit length, +n hemisphere, finite; PDF
 *      matches cos/PI; empirical PDF histogram converges to the analytic one.
 *   3. sampling_uniform_sphere: unit length, finite, both hemispheres hit,
 *      z uniform in [-1, 1); empirical mean ~ 0.
 *   4. Schlick: endpoints F(cos=1)=F0, F(cos=0)=1, clamping, RGB per-channel.
 *   5. Dielectric Fresnel: normal incidence -> ((eta-1)/(eta+1))^2; TIR -> 1;
 *      reciprocity F(cos, eta) == F(cos, 1/eta) for the reflectance at the
 *      matching angles; monotonic in cos_i for eta > 1.
 *   6. sampling_refract: Snell's law consistency (sin_t == eta*sin_i),
 *      unit output, TIR returns 0, normal incidence passes straight through.
 *   7. PBR: specular probability range (0,1], monotonic in metallic/F0 and
 *      decreasing in roughness; GGX half-vector unit length + hemisphere;
 *      GGX PDF integrates to ~1 over the reflected hemisphere.
 */

#include "sampling.h"
#include "vec3.h"

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

#define PI 3.14159265358979323846

/* Deterministic 64-bit LCG mapped to [0, 1). */
static unsigned long long g_rng = 0x9E3779B97F4A7C15ULL;
static double rnd01(void)
{
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((g_rng >> 11) & ((1ULL << 53) - 1)) / (double)(1ULL << 53);
}

static double vec_len(Vec3 v) { return vec3_length(v); }

/* ------------------------------------------------------------------ */
/* 1. Orthonormal basis                                                */
/* ------------------------------------------------------------------ */

static void test_basis(void)
{
    Vec3 normals[] = {
        vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
        vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0),
        vec3(0.0, 0.999, 0.045), vec3(0.001, -0.9999, 0.001),
        vec3(0.577350269, 0.577350269, 0.577350269),
        vec3(-0.3, 0.9, 0.2), vec3(0.6, 0.0, -0.8)
    };
    size_t count = sizeof(normals) / sizeof(normals[0]);

    for (size_t i = 0; i < count; i++) {
        Vec3 n = vec3_normalize(normals[i]);
        Vec3 t, b;
        sampling_basis(n, &t, &b);

        CHECK(fabs(vec_len(t) - 1.0) < 1e-12, "basis: |t| == 1");
        CHECK(fabs(vec_len(b) - 1.0) < 1e-12, "basis: |b| == 1");
        CHECK(fabs(vec3_dot(t, b)) < 1e-12, "basis: t . b == 0");
        CHECK(fabs(vec3_dot(t, n)) < 1e-12, "basis: t . n == 0");
        CHECK(fabs(vec3_dot(b, n)) < 1e-12, "basis: b . n == 0");

        /* Right-handed: cross(t, b) == n. */
        Vec3 c = vec3_cross(t, b);
        CHECK(fabs(c.x - n.x) < 1e-12 && fabs(c.y - n.y) < 1e-12 &&
              fabs(c.z - n.z) < 1e-12, "basis: cross(t,b) == n");
    }

    /* Non-unit normal is normalized internally; NULL pointers are a no-op. */
    Vec3 t, b;
    sampling_basis(vec3(0.0, 5.0, 0.0), &t, &b);
    CHECK(fabs(vec_len(t) - 1.0) < 1e-12 && fabs(vec_len(b) - 1.0) < 1e-12,
          "basis: non-unit n normalized");
    sampling_basis(vec3(0, 1, 0), NULL, &b);   /* must not crash */
    sampling_basis(vec3(0, 1, 0), &t, NULL);   /* must not crash */
    CHECK(1, "basis: NULL args are a no-op");
}

/* ------------------------------------------------------------------ */
/* 2. Cosine hemisphere                                                */
/* ------------------------------------------------------------------ */

static void test_cosine_hemisphere(void)
{
    Vec3 n = vec3_normalize(vec3(0.1, 0.9, -0.3));
    int all_hemi = 1, all_unit = 1, all_finite = 1;
    const int N = 20000;

    for (int i = 0; i < N; i++) {
        Vec3 d = sampling_cosine_hemisphere(n, rnd01(), rnd01());
        if (fabs(vec_len(d) - 1.0) > 1e-9) all_unit = 0;
        if (vec3_dot(d, n) < -1e-9) all_hemi = 0;
        if (!isfinite(d.x) || !isfinite(d.y) || !isfinite(d.z)) all_finite = 0;
    }
    CHECK(all_unit, "cosine: all samples unit length");
    CHECK(all_hemi, "cosine: all samples in +n hemisphere");
    CHECK(all_finite, "cosine: all samples finite");

    /* For a cosine-weighted sampler, E[cos]/... : the average PDF weighted
     * estimator of the hemisphere solid angle integral ~ PI / PI... Instead
     * verify the mean of cos_theta ~ 2/3 (analytic for cosine-weighted). */
    /* Recompute mean cosine separately for clarity. */
    double mean_cos = 0.0;
    for (int i = 0; i < N; i++) {
        Vec3 d = sampling_cosine_hemisphere(n, rnd01(), rnd01());
        mean_cos += vec3_dot(d, n);
    }
    mean_cos /= (double)N;
    CHECK(fabs(mean_cos - 2.0 / 3.0) < 5e-3,
          "cosine: mean cos_theta ~ 2/3");

    /* Analytic PDF values. */
    CHECK(fabs(sampling_cosine_pdf(1.0) - 1.0 / PI) < 1e-15,
          "cosine pdf: cos=1 -> 1/PI");
    CHECK(fabs(sampling_cosine_pdf(0.5) - 0.5 / PI) < 1e-15,
          "cosine pdf: cos=0.5 -> 0.5/PI");
    CHECK(sampling_cosine_pdf(0.0) == 0.0, "cosine pdf: cos=0 -> 0");
    CHECK(sampling_cosine_pdf(-0.3) == 0.0, "cosine pdf: back hemisphere -> 0");

    /* Out-of-range / NaN uniforms stay safe. */
    Vec3 d1 = sampling_cosine_hemisphere(n, 2.0, -1.0);
    Vec3 d2 = sampling_cosine_hemisphere(n, NAN, NAN);
    CHECK(isfinite(d1.x) && fabs(vec_len(d1) - 1.0) < 1e-9,
          "cosine: out-of-range u safe");
    CHECK(isfinite(d2.x) && fabs(vec_len(d2) - 1.0) < 1e-9,
          "cosine: NaN u safe");

    /* Empirical PDF check: fraction of samples with cos in a bin ~ integral
     * of p over that bin, i.e. (cos_hi^2 - cos_lo^2). */
    {
        const double lo = 0.4, hi = 0.6;
        int in_bin = 0;
        for (int i = 0; i < N; i++) {
            Vec3 d = sampling_cosine_hemisphere(n, rnd01(), rnd01());
            double c = vec3_dot(d, n);
            if (c >= lo && c < hi) in_bin++;
        }
        double frac = (double)in_bin / (double)N;
        double expect = hi * hi - lo * lo; /* CDF of cosine-weighted in cos */
        CHECK(fabs(frac - expect) < 0.02,
              "cosine: empirical CDF matches analytic");
    }
}

/* ------------------------------------------------------------------ */
/* 3. Uniform sphere                                                   */
/* ------------------------------------------------------------------ */

static void test_uniform_sphere(void)
{
    int all_unit = 1, pos_z = 0, neg_z = 0;
    double mean = 0.0;
    const int N = 20000;

    for (int i = 0; i < N; i++) {
        Vec3 d = sampling_uniform_sphere(rnd01(), rnd01());
        if (fabs(vec_len(d) - 1.0) > 1e-9) all_unit = 0;
        if (!isfinite(d.x) || !isfinite(d.y) || !isfinite(d.z)) all_unit = 0;
        if (d.z >= 0.0) pos_z++; else neg_z++;
        mean += d.z;
    }
    CHECK(all_unit, "sphere: all samples unit + finite");
    CHECK(pos_z > 0 && neg_z > 0, "sphere: covers both hemispheres");
    CHECK(fabs(mean / (double)N) < 0.02, "sphere: mean z ~ 0 (uniform)");

    /* z-distribution uniformity: fraction with z < 0 ~ 0.5. */
    CHECK(fabs((double)pos_z / (double)N - 0.5) < 0.02,
          "sphere: P(z>0) ~ 0.5");
}

/* ------------------------------------------------------------------ */
/* 4. Schlick Fresnel                                                  */
/* ------------------------------------------------------------------ */

static void test_fresnel_schlick(void)
{
    CHECK(fabs(sampling_fresnel_schlick(1.0, 0.04) - 0.04) < 1e-15,
          "schlick: F(cos=1) == F0");
    CHECK(fabs(sampling_fresnel_schlick(0.0, 0.04) - 1.0) < 1e-15,
          "schlick: F(cos=0) == 1");
    /* Clamping of cos_theta. */
    CHECK(fabs(sampling_fresnel_schlick(-5.0, 0.04) - 1.0) < 1e-15,
          "schlick: cos < 0 clamped to 0");
    CHECK(fabs(sampling_fresnel_schlick(5.0, 0.04) - 0.04) < 1e-15,
          "schlick: cos > 1 clamped to 1");
    /* Monotonic decreasing in cos. */
    CHECK(sampling_fresnel_schlick(0.2, 0.04) >
          sampling_fresnel_schlick(0.8, 0.04),
          "schlick: decreasing in cos");

    /* RGB per channel. */
    Vec3 f0 = vec3(0.04, 0.5, 1.0);
    Vec3 f1 = sampling_fresnel_schlick_rgb(1.0, f0);
    CHECK(fabs(f1.x - 0.04) < 1e-15 && fabs(f1.y - 0.5) < 1e-15 &&
          fabs(f1.z - 1.0) < 1e-15, "schlick rgb: F(cos=1) == F0");
    Vec3 fz = sampling_fresnel_schlick_rgb(0.0, f0);
    CHECK(fabs(fz.x - 1.0) < 1e-15 && fabs(fz.y - 1.0) < 1e-15 &&
          fabs(fz.z - 1.0) < 1e-15, "schlick rgb: F(cos=0) == 1");
}

/* ------------------------------------------------------------------ */
/* 5. Dielectric Fresnel                                               */
/* ------------------------------------------------------------------ */

static void test_fresnel_dielectric(void)
{
    /* Normal incidence, entering denser medium: ((eta-1)/(eta+1))^2. */
    double eta = 1.5;
    double expect = (eta - 1.0) / (eta + 1.0);
    expect *= expect;
    CHECK(fabs(sampling_fresnel_dielectric(1.0, eta) - expect) < 1e-12,
          "dielectric: normal incidence closed form");

    /* Grazing incidence -> 1 for any eta. */
    CHECK(fabs(sampling_fresnel_dielectric(0.0, eta) - 1.0) < 1e-12,
          "dielectric: grazing -> 1");

    /* Total internal reflection when leaving a denser medium: eta = 1.5 > 1
     * with a steep angle beyond the critical angle (cos_i < 1/eta). */
    CHECK(fabs(sampling_fresnel_dielectric(0.3, 1.5) - 1.0) < 1e-12,
          "dielectric: TIR -> 1");

    /* Reciprocity: R(cos_i, eta) == R(cos_t, 1/eta) with Snell's law. */
    double ci = 0.9, e = 1.5;
    double sin2t = e * e * (1.0 - ci * ci);
    double ct = sqrt(1.0 - sin2t);
    double r_fwd = sampling_fresnel_dielectric(ci, e);
    double r_bwd = sampling_fresnel_dielectric(ct, 1.0 / e);
    CHECK(fabs(r_fwd - r_bwd) < 1e-12, "dielectric: reciprocity");

    /* Monotonic increasing toward grazing (decreasing in cos_i). */
    CHECK(sampling_fresnel_dielectric(0.2, e) >
          sampling_fresnel_dielectric(0.9, e),
          "dielectric: monotonic in cos_i");

    /* Range check over a sweep. */
    int in_range = 1;
    for (int i = 0; i <= 100; i++) {
        double c = (double)i / 100.0;
        double f = sampling_fresnel_dielectric(c, 1.33);
        if (!(f >= 0.0 && f <= 1.0)) in_range = 0;
    }
    CHECK(in_range, "dielectric: reflectance in [0,1]");
}

/* ------------------------------------------------------------------ */
/* 6. Refraction                                                       */
/* ------------------------------------------------------------------ */

static void test_refract(void)
{
    /* Convention: `n` faces the incident side, so dot(incident, n) <= 0.
     * Normal incidence straight through: incident points +Y toward the
     * surface, n faces back toward the incident side (-Y). */
    Vec3 in = vec3(0.0, 1.0, 0.0);
    Vec3 n = vec3(0.0, -1.0, 0.0);
    Vec3 out;
    int ok = sampling_refract(in, n, 1.0 / 1.5, &out);
    CHECK(ok == 1, "refract: normal incidence succeeds");
    CHECK(fabs(vec_len(out) - 1.0) < 1e-12, "refract: unit output");
    CHECK(fabs(out.x) < 1e-12 && fabs(out.z) < 1e-12 && fabs(out.y - 1.0) < 1e-12,
          "refract: normal incidence passes straight");

    /* Snell's law consistency over many angles. */
    int snell_ok = 1, unit_ok = 1;
    for (int i = 1; i < 60; i++) {
        double ang = (double)i / 60.0 * (PI * 0.49);
        Vec3 d = vec3(sin(ang), cos(ang), 0.0); /* unit, dot(d,n) = -cos(ang) */
        Vec3 nn = vec3(0.0, -1.0, 0.0);
        double eta = 1.0 / 1.5;
        Vec3 t;
        if (sampling_refract(d, nn, eta, &t) == 1) {
            double cos_i = -vec3_dot(d, nn);
            double sin_i = sqrt(1.0 - cos_i * cos_i);
            double cos_t = -vec3_dot(t, nn);
            double sin_t = sqrt(fmax(0.0, 1.0 - cos_t * cos_t));
            if (fabs(sin_t - eta * sin_i) > 1e-9) snell_ok = 0;
            if (fabs(vec_len(t) - 1.0) > 1e-12) unit_ok = 0;
        }
    }
    CHECK(snell_ok, "refract: satisfies Snell's law");
    CHECK(unit_ok, "refract: transmitted always unit");

    /* TIR: leaving a dense medium (eta = 1.5 > 1) beyond the critical angle
     * (sin_i = sin(1 rad) = 0.84 > 1/1.5 = 0.667) -> 0. */
    Vec3 d_tir = vec3(sin(1.0), cos(1.0), 0.0);
    Vec3 t_tir;
    CHECK(sampling_refract(d_tir, vec3(0.0, -1.0, 0.0), 1.5, &t_tir) == 0,
          "refract: TIR returns 0");

    /* NULL output pointer is failure, no crash. */
    CHECK(sampling_refract(in, n, 1.0, NULL) == 0, "refract: NULL out -> 0");
}

/* ------------------------------------------------------------------ */
/* 7. PBR lobe selection + GGX                                         */
/* ------------------------------------------------------------------ */

static void test_pbr(void)
{
    /* Range (0, 1]. */
    int in_range = 1;
    for (int i = 0; i <= 10; i++) {
        for (int j = 0; j <= 10; j++) {
            double p = sampling_specular_probability(i / 10.0, j / 10.0, 0.04);
            if (!(p > 0.0 && p <= 1.0)) in_range = 0;
        }
    }
    CHECK(in_range, "pspec: in (0,1] over sweep");

    /* metallic = 1 -> exactly 1.0. */
    CHECK(fabs(sampling_specular_probability(1.0, 0.5, 0.04) - 1.0) < 1e-15,
          "pspec: metallic=1 -> 1");
    /* metallic = 0, roughness = 0 -> exactly F0. */
    CHECK(fabs(sampling_specular_probability(0.0, 0.0, 0.04) - 0.04) < 1e-15,
          "pspec: dielectric smooth -> F0");

    /* Monotonic increasing in metallic, decreasing in roughness. */
    CHECK(sampling_specular_probability(0.8, 0.5, 0.04) >
          sampling_specular_probability(0.2, 0.5, 0.04),
          "pspec: monotonic in metallic");
    CHECK(sampling_specular_probability(0.0, 0.1, 0.04) >
          sampling_specular_probability(0.0, 0.9, 0.04),
          "pspec: decreasing in roughness");

    /* GGX half-vector: unit, +n hemisphere, finite. */
    Vec3 n = vec3_normalize(vec3(0.2, 0.8, 0.4));
    int unit_ok = 1, hemi_ok = 1;
    for (int i = 0; i < 5000; i++) {
        Vec3 h = sampling_ggx_half_vector(n, 0.3, rnd01(), rnd01());
        if (fabs(vec_len(h) - 1.0) > 1e-9) unit_ok = 0;
        if (vec3_dot(h, n) < -1e-9) hemi_ok = 0;
    }
    CHECK(unit_ok, "ggx: half-vector unit length");
    CHECK(hemi_ok, "ggx: half-vector in +n hemisphere");

    /* alpha below the minimum is still safe. */
    Vec3 h0 = sampling_ggx_half_vector(n, 0.0, 0.5, 0.5);
    CHECK(fabs(vec_len(h0) - 1.0) < 1e-9, "ggx: alpha=0 safe");

    /* GGX PDF normalisation. The standard reflection PDF
     *   p(L) = D(H) * (N.H) / (4 * (H.V))
     * integrates to 1 over the FULL sphere of outgoing directions L (the map
     * L -> H = normalize(V+L) covers the sphere; p returns 0 where N.H <= 0).
     * Monte-Carlo: E[ p(L) * 4*PI ] over uniform-sphere samples ~ 1. */
    {
        Vec3 N = vec3(0.0, 1.0, 0.0);
        Vec3 V = vec3_normalize(vec3(0.4, 0.7, 0.2)); /* view direction */
        double alpha = 0.4;
        double sum = 0.0;
        int M = 400000;
        for (int i = 0; i < M; i++) {
            Vec3 L = sampling_uniform_sphere(rnd01(), rnd01());
            Vec3 H = vec3_normalize(vec3_add(V, L));
            double ndh = vec3_dot(N, H);
            double hdv = vec3_dot(H, V);
            double p = sampling_ggx_pdf(ndh, hdv, alpha);
            sum += p * 4.0 * PI; /* uniform-sphere pdf is 1/(4*PI) */
        }
        double integral = sum / (double)M;
        CHECK(fabs(integral - 1.0) < 0.03,
              "ggx pdf: integrates to ~1 over the sphere");
    }

    /* PDF degenerate guards. */
    CHECK(sampling_ggx_pdf(0.5, 0.0, 0.4) == 0.0, "ggx pdf: h_dot_v=0 -> 0");
    CHECK(sampling_ggx_pdf(0.0, 0.5, 0.4) == 0.0, "ggx pdf: n_dot_h=0 -> 0");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_basis();
    test_cosine_hemisphere();
    test_uniform_sphere();
    test_fresnel_schlick();
    test_fresnel_dielectric();
    test_refract();
    test_pbr();

    printf("test_sampling: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
