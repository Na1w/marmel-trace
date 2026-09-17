/*
 * texture.c - Procedural, position-modulated material textures.
 *
 * Implements the contract documented in texture.h. Pure functions only:
 * no globals, no dynamic allocation during sampling, no I/O, no rand()/clock()/time().
 * The output is a deterministic function of (material fields, world point, UV).
 *
 * Depends only on texture.h (which pulls in vec3.h + material.h), bmp.h, <math.h> and <stdlib.h>.
 */

#include "texture.h"
#include "bmp.h"

#include <math.h>
#include <stdlib.h>
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

/* Exact sRGB byte to linear RGB [0, 1] conversion. */
static inline double srgb_byte_to_linear(unsigned char byte_val)
{
    double c = (double)byte_val / 255.0;
    return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
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
/* ImageTexture lifecycle & sampling                                   */
/* ------------------------------------------------------------------ */

ImageTexture *texture_image_create(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    ImageTexture *tex = (ImageTexture *)malloc(sizeof(ImageTexture));
    if (!tex) return NULL;
    tex->width = width;
    tex->height = height;
    tex->rgb = (unsigned char *)calloc((size_t)width * (size_t)height * 3u, 1);
    if (!tex->rgb) {
        free(tex);
        return NULL;
    }
    return tex;
}

ImageTexture *texture_image_load_bmp(const char *path)
{
    if (!path) return NULL;
    unsigned char *rgb = NULL;
    int w = 0, h = 0;
    if (bmp_read(path, &rgb, &w, &h) != 0 || !rgb) {
        return NULL;
    }
    ImageTexture *tex = (ImageTexture *)malloc(sizeof(ImageTexture));
    if (!tex) {
        free(rgb);
        return NULL;
    }
    tex->width = w;
    tex->height = h;
    tex->rgb = rgb;
    return tex;
}

void texture_image_free(ImageTexture *tex)
{
    if (!tex) return;
    free(tex->rgb);
    free(tex);
}

Vec3 texture_image_sample(const ImageTexture *tex, double u, double v)
{
    if (!tex || !tex->rgb || tex->width <= 0 || tex->height <= 0) {
        return vec3(1.0, 1.0, 1.0);
    }

    /* Wrap UV into [0, 1) */
    u = u - floor(u);
    v = v - floor(v);

    /* Map UV to continuous pixel space.
     * v = 1.0 is top (row 0), v = 0.0 is bottom (row height - 1) */
    double px = u * (double)tex->width - 0.5;
    double py = (1.0 - v) * (double)tex->height - 0.5;

    int x0 = (int)floor(px);
    int y0 = (int)floor(py);
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    double fx = px - (double)x0;
    double fy = py - (double)y0;

    int w = tex->width;
    int h = tex->height;

    x0 = ((x0 % w) + w) % w;
    x1 = ((x1 % w) + w) % w;
    y0 = ((y0 % h) + h) % h;
    y1 = ((y1 % h) + h) % h;

    const unsigned char *p00 = &tex->rgb[((size_t)y0 * (size_t)w + (size_t)x0) * 3u];
    const unsigned char *p10 = &tex->rgb[((size_t)y0 * (size_t)w + (size_t)x1) * 3u];
    const unsigned char *p01 = &tex->rgb[((size_t)y1 * (size_t)w + (size_t)x0) * 3u];
    const unsigned char *p11 = &tex->rgb[((size_t)y1 * (size_t)w + (size_t)x1) * 3u];

    Vec3 c00 = vec3(srgb_byte_to_linear(p00[0]), srgb_byte_to_linear(p00[1]), srgb_byte_to_linear(p00[2]));
    Vec3 c10 = vec3(srgb_byte_to_linear(p10[0]), srgb_byte_to_linear(p10[1]), srgb_byte_to_linear(p10[2]));
    Vec3 c01 = vec3(srgb_byte_to_linear(p01[0]), srgb_byte_to_linear(p01[1]), srgb_byte_to_linear(p01[2]));
    Vec3 c11 = vec3(srgb_byte_to_linear(p11[0]), srgb_byte_to_linear(p11[1]), srgb_byte_to_linear(p11[2]));

    double w00 = (1.0 - fx) * (1.0 - fy);
    double w10 = fx * (1.0 - fy);
    double w01 = (1.0 - fx) * fy;
    double w11 = fx * fy;

    return vec3(
        w00 * c00.x + w10 * c10.x + w01 * c01.x + w11 * c11.x,
        w00 * c00.y + w10 * c10.y + w01 * c01.y + w11 * c11.y,
        w00 * c00.z + w10 * c10.z + w01 * c01.z + w11 * c11.z
    );
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Vec3 texture_albedo_uv(const Material *m, Vec3 p, double u, double v)
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

    case TEXTURE_UV_CHECKER: {
        double s = texture_scale_or_default(m);
        double su = u * s;
        double sv = v * s;
        int pu = (int)(((long long)floor(su)) & 1);
        int pv = (int)(((long long)floor(sv)) & 1);
        parity = (pu + pv) & 1;
        return texture_mul(m->albedo,
                           parity ? m->texture_color_a : m->texture_color_b);
    }

    case TEXTURE_IMAGE: {
        if (m->texture_image != NULL) {
            double s = (m->texture_scale > 0.0) ? m->texture_scale : 1.0;
            Vec3 tex_col = texture_image_sample((const ImageTexture *)m->texture_image, u * s, v * s);
            return texture_mul(m->albedo, tex_col);
        }
        return m->albedo;
    }

    case TEXTURE_NONE:
    default:
        /* Bit-for-bit identity: untextured materials are untouched. */
        return m->albedo;
    }
}

Vec3 texture_albedo(const Material *m, Vec3 p)
{
    return texture_albedo_uv(m, p, 0.0, 0.0);
}

Vec3 texture_specular(const Material *m, Vec3 p)
{
    (void)p; /* reserved: no position dependence yet */
    if (m == NULL) {
        return vec3(0.0, 0.0, 0.0);
    }
    return m->specular;
}
