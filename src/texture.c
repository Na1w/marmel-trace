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

#include <math.h>
#include <stddef.h>

/* Local PI so we do not depend on M_PI (POSIX-only). */
#define TEXTURE_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

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
        /*
         * Sinusoidal bands along world Y. `0.5 * (1 + sin(...))` is in [0, 1];
         * thresholding at 0.5 yields a clean two-colour band with no
         * half-intensity blend, so the two colours stay exactly representable
         * (which also makes the unit tests robust to FP noise).
         */
        double s = texture_scale_or_default(m);
        double wave = 0.5 * (1.0 + sin(p.y * s * (2.0 * TEXTURE_PI)));
        return texture_mul(m->albedo,
                           (wave >= 0.5) ? m->texture_color_a
                                         : m->texture_color_b);
    }

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
