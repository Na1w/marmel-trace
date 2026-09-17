/*
 * texture.c - Procedural, position-modulated material textures.
 *
 * Implements the contract documented in texture.h. Pure functions only:
 * no globals, no dynamic allocation, no I/O, no rand()/clock()/time().
 * The output is a deterministic function of (material fields, world point).
 *
 * Depends only on texture.h (which pulls in vec3.h + material.h) and <math.h>.
 */

#include "texture.h"
#include "noise.h"

#include <math.h>
#include <stddef.h>

/* Local PI so we do not depend on M_PI (POSIX-only). */
#define TEXTURE_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline double clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static inline double smoothstep01(double edge0, double edge1, double x)
{
    double t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

/* Effective cell size: a non-positive/NaN scale falls back to 1.0 so a
 * zero-initialised material still produces a well-defined pattern. */
static double texture_scale_or_default(const Material *m)
{
    double s = m->texture_scale;
    if (!(s > 0.0)) { /* false for 0, negative and NaN */
        return 1.0;
    }
    return s;
}

/* Component-wise product (kept local so texture.c stays independent of any
 * helper that might be added to vec3.h later). */
static Vec3 texture_mul(Vec3 a, Vec3 b)
{
    return vec3(a.x * b.x, a.y * b.y, a.z * b.z);
}

/* 3D checkerboard parity at `p` scaled by `inv_scale`: 0 or 1. */
static int texture_checker_parity(Vec3 p, double inv_scale)
{
    /* floor() of a finite product is deterministic and platform-stable. */
    double fx = floor(p.x * inv_scale);
    double fy = floor(p.y * inv_scale);
    double fz = floor(p.z * inv_scale);

    /* Reduce each cell index to a parity bit before summing so the result
     * never overflows an int for very large coordinates. */
    int px = (int)(((long long)fx) & 1);
    int py = (int)(((long long)fy) & 1);
    int pz = (int)(((long long)fz) & 1);

    return (px + py + pz) & 1;
}

static Vec3 texture_planet_earth(const Material *m, Vec3 p)
{
    double s = 1.0 / texture_scale_or_default(m);
    Vec3 q = vec3_scale(p, s);

    /* Continents elevation field (7 octaves fBm) */
    double elev = noise_fbm3(q.x * 1.6, q.y * 1.6, q.z * 1.6, 7, 2.0, 0.5, 42u);

    /* Surface colors */
    Vec3 ocean_deep    = vec3(0.02, 0.05, 0.20);
    Vec3 ocean_shallow = vec3(0.04, 0.18, 0.35);
    Vec3 land_low      = vec3(0.12, 0.22, 0.08); /* lush plains/forests */
    Vec3 land_mid      = vec3(0.26, 0.24, 0.14); /* savannah/hills */
    Vec3 land_arid     = vec3(0.42, 0.35, 0.20); /* desert/rock */
    Vec3 snow          = vec3(0.88, 0.90, 0.94); /* alpine peaks/ice */
    Vec3 cloud_col     = vec3(0.92, 0.94, 0.96); /* bright clouds */

    Vec3 surf;
    if (elev < 0.0) {
        double t = clamp01((elev + 0.25) / 0.25);
        surf = vec3_lerp(ocean_deep, ocean_shallow, t);
    } else {
        double t = clamp01(elev / 0.45);
        if (t < 0.35) {
            surf = vec3_lerp(land_low, land_mid, t / 0.35);
        } else if (t < 0.70) {
            surf = vec3_lerp(land_mid, land_arid, (t - 0.35) / 0.35);
        } else {
            surf = vec3_lerp(land_arid, snow, (t - 0.70) / 0.30);
        }
    }

    /* Swirling cloud deck with domain warping */
    double warp = noise_fbm3(q.x * 2.2 + 7.3, q.y * 2.2 + 7.3, q.z * 2.2 + 7.3, 4, 2.0, 0.5, 101u);
    double c_noise = noise_fbm3(q.x * 2.6 + warp * 0.4,
                                q.y * 2.6 + warp * 0.4,
                                q.z * 2.6 + warp * 0.4, 6, 2.0, 0.5, 999u);
    double c_cov = smoothstep01(0.02, 0.38, c_noise);

    Vec3 result = vec3_lerp(surf, cloud_col, c_cov * 0.88);
    return texture_mul(m->albedo, result);
}

static Vec3 texture_planet_moon(const Material *m, Vec3 p)
{
    double s = 1.0 / texture_scale_or_default(m);
    Vec3 q = vec3_scale(p, s);

    /* Large-scale Maria vs Highlands (5 octaves fBm) */
    double maria = noise_fbm3(q.x * 1.2, q.y * 1.2, q.z * 1.2, 5, 2.0, 0.5, 77u);

    /* Medium-scale crater relief (6 octaves fBm) */
    double crater = noise_fbm3(q.x * 4.5, q.y * 4.5, q.z * 4.5, 6, 2.0, 0.5, 555u);

    /* Micro-scale regolith grain (4 octaves fBm) */
    double micro = noise_fbm3(q.x * 16.0, q.y * 16.0, q.z * 16.0, 4, 2.0, 0.5, 888u);

    Vec3 maria_col    = vec3(0.075, 0.075, 0.072); /* dark basalt */
    Vec3 highland_col = vec3(0.165, 0.160, 0.150); /* bright anorthosite */
    Vec3 ejecta_col   = vec3(0.250, 0.245, 0.235); /* fresh crater rays */

    double m_blend = clamp01((maria + 0.12) / 0.38);
    Vec3 base = vec3_lerp(maria_col, highland_col, m_blend);

    /* Modulate by craters and micro grain */
    double detail = 1.0 + 0.32 * crater + 0.14 * micro;
    Vec3 result = vec3_scale(base, detail);

    /* Bright impact rays where crater noise is high */
    if (crater > 0.40) {
        double ray_t = clamp01((crater - 0.40) / 0.25);
        result = vec3_lerp(result, ejecta_col, ray_t * 0.75);
    }
    return texture_mul(m->albedo, result);
}

static Vec3 texture_noise_blend(const Material *m, Vec3 p)
{
    double s = 1.0 / texture_scale_or_default(m);
    Vec3 q = vec3_scale(p, s);
    double n = noise_fbm3(q.x, q.y, q.z, 6, 2.0, 0.5, 1337u);
    double t = clamp01(0.5 * (n + 1.0));
    Vec3 col = vec3_lerp(m->texture_color_a, m->texture_color_b, t);
    return texture_mul(m->albedo, col);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Vec3 texture_albedo(const Material *m, Vec3 p)
{
    double inv_scale;
    int    parity;

    if (m == NULL) {
        return vec3(0.0, 0.0, 0.0);
    }

    switch (m->texture_kind) {
    case TEXTURE_CHECKER:
        inv_scale = 1.0 / texture_scale_or_default(m);
        parity = texture_checker_parity(p, inv_scale);
        return texture_mul(m->albedo,
                           parity ? m->texture_color_a : m->texture_color_b);

    case TEXTURE_STRIPES: {
        double s = texture_scale_or_default(m);
        double wave = 0.5 * (1.0 + sin(p.y * s * (2.0 * TEXTURE_PI)));
        return texture_mul(m->albedo,
                           (wave >= 0.5) ? m->texture_color_a
                                         : m->texture_color_b);
    }

    case TEXTURE_PLANET_EARTH:
        return texture_planet_earth(m, p);

    case TEXTURE_PLANET_MOON:
        return texture_planet_moon(m, p);

    case TEXTURE_NOISE:
        return texture_noise_blend(m, p);

    case TEXTURE_NONE:
    default:
        /* Bit-for-bit identity: untextured materials are untouched. */
        return m->albedo;
    }
}

Vec3 texture_specular(const Material *m, Vec3 p)
{
    (void)p; /* reserved: no position dependence yet */
    if (m == NULL) {
        return vec3(0.0, 0.0, 0.0);
    }
    return m->specular;
}

static double texture_height(const Material *m, Vec3 p)
{
    double s = m->bump_scale > 0.0 ? m->bump_scale : 1.0;
    Vec3 q = vec3_scale(p, s);

    switch (m->texture_kind) {
    case TEXTURE_PLANET_MOON: {
        double c1 = noise_fbm3(q.x * 2.5, q.y * 2.5, q.z * 2.5, 5, 2.0, 0.5, 555u);
        double c2 = noise_fbm3(q.x * 8.0, q.y * 8.0, q.z * 8.0, 4, 2.0, 0.5, 888u);
        return c1 * 0.7 + c2 * 0.3;
    }
    case TEXTURE_PLANET_EARTH: {
        double e = noise_fbm3(q.x * 1.6, q.y * 1.6, q.z * 1.6, 6, 2.0, 0.5, 42u);
        if (e < 0.0) return 0.0;
        return e;
    }
    default:
        return noise_fbm3(q.x, q.y, q.z, 5, 2.0, 0.5, 1337u);
    }
}

Vec3 texture_normal(const Material *m, Vec3 p, Vec3 n)
{
    if (m == NULL || m->bump_strength <= 1e-6) {
        return n;
    }

    /* Tangent basis orthonormal to n */
    Vec3 up = (fabs(n.y) < 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    Vec3 t1 = vec3_normalize(vec3_cross(n, up));
    Vec3 t2 = vec3_cross(n, t1);

    double scale = m->bump_scale > 0.0 ? m->bump_scale : 1.0;
    double eps = 0.005 / scale;
    if (eps < 1e-7) eps = 1e-7;

    Vec3 p_f1 = vec3_add(p, vec3_scale(t1, eps));
    Vec3 p_b1 = vec3_sub(p, vec3_scale(t1, eps));
    Vec3 p_f2 = vec3_add(p, vec3_scale(t2, eps));
    Vec3 p_b2 = vec3_sub(p, vec3_scale(t2, eps));

    double h_f1 = texture_height(m, p_f1);
    double h_b1 = texture_height(m, p_b1);
    double h_f2 = texture_height(m, p_f2);
    double h_b2 = texture_height(m, p_b2);

    double dh1 = (h_f1 - h_b1) / (2.0 * eps);
    double dh2 = (h_f2 - h_b2) / (2.0 * eps);

    Vec3 grad = vec3_add(vec3_scale(t1, dh1), vec3_scale(t2, dh2));
    Vec3 perturbed = vec3_sub(n, vec3_scale(grad, m->bump_strength));
    Vec3 result = vec3_normalize(perturbed);
    if (vec3_length_sq(result) < 1e-12) {
        return n;
    }
    return result;
}
