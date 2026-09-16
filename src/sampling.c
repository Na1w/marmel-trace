/*
 * sampling.c - Pure Monte-Carlo sampling / reflectance primitives.
 *
 * Self-contained foundation module: depends ONLY on src/vec3.h and <math.h>.
 * Every function is pure (deterministic in its arguments, no globals, no I/O,
 * no allocation). See src/sampling.h for the full contracts.
 */

#include "sampling.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/* Internal scalar helpers                                             */
/* ------------------------------------------------------------------ */

/* Clamp to [0, 1]; NaN maps to 0. */
static double clamp01(double v)
{
    if (!(v > 0.0)) return 0.0;  /* also catches NaN */
    if (v > 1.0) return 1.0;
    return v;
}

/* Clamp to [lo, hi]; NaN maps to lo. Requires lo <= hi. */
static double clamp_range(double v, double lo, double hi)
{
    if (!(v > lo)) return lo;    /* also catches NaN */
    if (v > hi) return hi;
    return v;
}

/*
 * Clamp a uniform to the open interval [0, 1) by mapping values >= 1 to the
 * largest double strictly below 1 (MATERIAL_RAND01_MAX convention). This keeps
 * the cosine-hemisphere and GGX formulas strictly positive without pushing the
 * sample onto the tangent plane.
 */
#define SAMPLING_RAND01_MAX 0.9999999999999999

static double clamp_uniform(double u)
{
    if (!(u > 0.0)) return 0.0;  /* also catches NaN and negatives */
    if (u > SAMPLING_RAND01_MAX) return SAMPLING_RAND01_MAX;
    return u;
}

/* ------------------------------------------------------------------ */
/* Orthonormal basis                                                   */
/* ------------------------------------------------------------------ */

void sampling_basis(Vec3 n, Vec3 *t, Vec3 *b)
{
    if (!t || !b) {
        return;
    }

    /* Defensively normalize; fall back to +Y for a degenerate normal. */
    Vec3 nn = vec3_normalize(n);
    if (vec3_length_sq(nn) <= 0.0) {
        nn = vec3(0.0, 1.0, 0.0);
    }

    /* Pick the world axis least aligned with nn to avoid a degenerate cross.
     * This is the EXACT convention used by sky_basis() in src/material.c. */
    Vec3 axis;
    if (fabs(nn.x) <= fabs(nn.y) && fabs(nn.x) <= fabs(nn.z)) {
        axis = vec3(1.0, 0.0, 0.0);
    } else if (fabs(nn.y) <= fabs(nn.z)) {
        axis = vec3(0.0, 1.0, 0.0);
    } else {
        axis = vec3(0.0, 0.0, 1.0);
    }

    *t = vec3_normalize(vec3_cross(axis, nn));
    *b = vec3_cross(nn, *t); /* already unit: nn and t are orthonormal */
}

/* ------------------------------------------------------------------ */
/* Cosine-weighted hemisphere                                          */
/* ------------------------------------------------------------------ */

Vec3 sampling_cosine_hemisphere(Vec3 n, double u1, double u2)
{
    Vec3 nn = vec3_normalize(n);
    if (vec3_length_sq(nn) <= 0.0) {
        nn = vec3(0.0, 1.0, 0.0);
    }

    double r1 = clamp_uniform(u1);
    double r2 = clamp_uniform(u2);

    /* Malley's method: r = sqrt(u1) (cosine weighting), phi = 2*PI*u2. */
    double r = sqrt(r1);
    double phi = 2.0 * SAMPLING_PI * r2;
    double x = r * cos(phi);
    double y = r * sin(phi);
    double z = sqrt(fmax(0.0, 1.0 - r1)); /* cosine of the elevation angle */

    Vec3 t, b;
    sampling_basis(nn, &t, &b);

    Vec3 d = vec3_add(
        vec3_add(vec3_scale(t, x), vec3_scale(b, y)),
        vec3_scale(nn, z));
    return vec3_normalize(d);
}

double sampling_cosine_pdf(double cos_theta)
{
    if (!(cos_theta > 0.0)) {    /* also catches NaN */
        return 0.0;
    }
    if (cos_theta > 1.0) {
        cos_theta = 1.0;
    }
    return cos_theta / SAMPLING_PI;
}

/* ------------------------------------------------------------------ */
/* Uniform sphere                                                      */
/* ------------------------------------------------------------------ */

Vec3 sampling_uniform_sphere(double u1, double u2)
{
    double r1 = clamp_uniform(u1);
    double r2 = clamp_uniform(u2);

    /* z = 2*u1 - 1 uniform in [-1, 1); phi = 2*PI*u2 uniform in [0, 2*PI). */
    double z = 2.0 * r1 - 1.0;
    double r = sqrt(fmax(0.0, 1.0 - z * z));
    double phi = 2.0 * SAMPLING_PI * r2;

    return vec3(r * cos(phi), r * sin(phi), z);
}

/* ------------------------------------------------------------------ */
/* Fresnel - Schlick approximation                                     */
/* ------------------------------------------------------------------ */

double sampling_fresnel_schlick(double cos_theta, double f0)
{
    double c = clamp01(cos_theta);
    double f = clamp01(f0);
    double m = 1.0 - c;
    double m2 = m * m;
    double k = m2 * m2 * m; /* (1 - cos_theta)^5 */
    return f + (1.0 - f) * k;
}

Vec3 sampling_fresnel_schlick_rgb(double cos_theta, Vec3 f0)
{
    return vec3(sampling_fresnel_schlick(cos_theta, f0.x),
                sampling_fresnel_schlick(cos_theta, f0.y),
                sampling_fresnel_schlick(cos_theta, f0.z));
}

/* ------------------------------------------------------------------ */
/* Fresnel - dielectric (exact, for reflect/refract)                   */
/* ------------------------------------------------------------------ */

double sampling_fresnel_dielectric(double cos_i, double eta)
{
    double c = clamp01(cos_i);
    double e = eta > 0.0 ? eta : 0.0; /* guard non-positive ratio */

    /* sin^2_t = eta^2 * (1 - cos^2_i); >= 1 means total internal reflection. */
    double sin2_t = e * e * (1.0 - c * c);
    if (sin2_t >= 1.0) {
        return 1.0; /* total internal reflection */
    }

    double cos_t = sqrt(1.0 - sin2_t);

    /* Unpolarised Fresnel: average of s- and p-polarised reflectances. */
    double rs = (e * c - cos_t) / (e * c + cos_t);
    double rp = (e * cos_t - c) / (e * cos_t + c);

    double f = 0.5 * (rs * rs + rp * rp);
    return clamp01(f);
}

int sampling_refract(Vec3 incident, Vec3 n, double eta, Vec3 *out_transmitted)
{
    if (!out_transmitted) {
        return 0;
    }

    Vec3 i = vec3_normalize(incident);
    Vec3 nn = vec3_normalize(n);
    if (vec3_length_sq(i) <= 0.0 || vec3_length_sq(nn) <= 0.0) {
        return 0; /* degenerate input: no meaningful refraction */
    }

    double e = eta > 0.0 ? eta : 0.0;
    double cos_i = -vec3_dot(i, nn);
    double sin2_t = e * e * (1.0 - cos_i * cos_i);

    if (sin2_t >= 1.0) {
        return 0; /* total internal reflection: no transmitted direction */
    }

    double cos_t = sqrt(1.0 - sin2_t);
    double k = e * cos_i - cos_t;

    Vec3 t = vec3(e * i.x + k * nn.x,
                  e * i.y + k * nn.y,
                  e * i.z + k * nn.z);
    *out_transmitted = vec3_normalize(t);
    return 1;
}

/* ------------------------------------------------------------------ */
/* PBR lobe selection                                                  */
/* ------------------------------------------------------------------ */

double sampling_specular_probability(double metallic, double roughness,
                                     double f0_scalar)
{
    double m = clamp01(metallic);
    double r = clamp01(roughness);
    double f0 = clamp01(f0_scalar);

    /* Base: conductors are specular-only; dielectrics weighted by F0. */
    double p = f0 + (1.0 - f0) * m;

    /* Rough dielectrics scatter more diffusely -> lower specular weight. */
    p *= 1.0 - 0.5 * r * (1.0 - m);

    return clamp_range(p, SAMPLING_PSPEC_MIN, 1.0);
}

/* GGX / Trowbridge-Reitz normal distribution D(NdotH, a), a = alpha > 0.
 * denom = NdotH^2 * (a^2 - 1) + 1 is >= a^2 > 0, so there is no divide-by-zero. */
static double d_ggx(double ndh, double a)
{
    double a2 = a * a;
    double d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (SAMPLING_PI * d * d);
}

Vec3 sampling_ggx_half_vector(Vec3 n, double alpha, double u1, double u2)
{
    Vec3 nn = vec3_normalize(n);
    if (vec3_length_sq(nn) <= 0.0) {
        nn = vec3(0.0, 1.0, 0.0);
    }

    double a = alpha > SAMPLING_ALPHA_MIN ? alpha : SAMPLING_ALPHA_MIN;
    double a2 = a * a;

    double r1 = clamp_uniform(u1);
    double r2 = clamp_uniform(u2);

    /* Karis/UE4 NDF importance sampling: cosTheta in (0, 1]. */
    double cos_theta = sqrt((1.0 - r1) / (1.0 + (a2 - 1.0) * r1));
    double sin_theta = sqrt(fmax(0.0, 1.0 - cos_theta * cos_theta));
    double phi = 2.0 * SAMPLING_PI * r2;

    Vec3 t, b;
    sampling_basis(nn, &t, &b);

    Vec3 h = vec3_add(
        vec3_add(vec3_scale(t, sin_theta * cos(phi)),
                 vec3_scale(b, sin_theta * sin(phi))),
        vec3_scale(nn, cos_theta));
    return vec3_normalize(h);
}

double sampling_ggx_pdf(double n_dot_h, double h_dot_v, double alpha)
{
    if (!(h_dot_v > 0.0) || !(n_dot_h > 0.0)) { /* also catches NaN */
        return 0.0;
    }

    double a = alpha > SAMPLING_ALPHA_MIN ? alpha : SAMPLING_ALPHA_MIN;
    double ndh = n_dot_h > 1.0 ? 1.0 : n_dot_h;

    double D = d_ggx(ndh, a);
    return D * ndh / (4.0 * h_dot_v);
}
