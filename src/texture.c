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
#include "noise.h"
#include "bmp.h"

#include <math.h>
#include <stdlib.h>
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
