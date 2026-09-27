/*
 * flare.c - Anamorphic lens flare / horizontal streak filter implementation.
 */

#include "flare.h"

#include <stdlib.h>
#include <math.h>

void flare_default_params(FlareParams *f)
{
    if (f == NULL) {
        return;
    }
    f->enabled       = FLARE_DEFAULT_ENABLED;
    f->intensity     = FLARE_DEFAULT_INTENSITY;
    f->threshold     = FLARE_DEFAULT_THRESHOLD;
    f->streak_length = FLARE_DEFAULT_STREAK_LENGTH;
    f->tint          = vec3(0.15, 0.45, 1.0); /* classic anamorphic cyan-blue */
}

/*
 * 1D bidirectional exponential IIR filter along an array of `width` floats.
 * Zero-phase, symmetric decay with energy normalization:
 *   sum_{k=-inf}^{+inf} alpha^|k| = (1 + alpha) / (1 - alpha)
 * so multiplying by (1 - alpha) / (1 + alpha) conserves total energy.
 */
static void flare_filter_channel(const float *src, float *dst, int width,
                                 float radius, float weight,
                                 float *pass_l, float *pass_r)
{
    if (radius < 1.0f) {
        radius = 1.0f;
    }
    float alpha = expf(-1.0f / radius);
    /* Scale so a typical bright emitter generates a vivid cinematic streak */
    float norm = weight * 0.045f;

    /* Forward pass (left-to-right) */
    float acc = 0.0f;
    for (int x = 0; x < width; ++x) {
        acc = src[x] + alpha * acc;
        pass_l[x] = acc;
    }

    /* Backward pass (right-to-left) */
    acc = 0.0f;
    for (int x = width - 1; x >= 0; --x) {
        acc = src[x] + alpha * acc;
        pass_r[x] = acc;
    }

    /* Combine symmetric impulse response */
    for (int x = 0; x < width; ++x) {
        float val = (pass_l[x] + pass_r[x] - src[x]) * norm;
        dst[x] += val;
    }
}

int flare_apply(unsigned char *rgb, int width, int height, const FlareParams *params)
{
    if (rgb == NULL || width <= 0 || height <= 0 || params == NULL) {
        return 1;
    }
    if (!params->enabled || params->intensity <= 0.0) {
        return 0;
    }

    double intensity = params->intensity;
    double thresh = params->threshold;
    if (thresh < 0.0) thresh = 0.0;
    if (thresh > 0.999) thresh = 0.999;

    double len_frac = params->streak_length;
    if (len_frac < 0.005) len_frac = 0.005;
    if (len_frac > 1.0) len_frac = 1.0;

    Vec3 tint = params->tint;
    if (vec3_length_sq(tint) < 1e-6) {
        tint = vec3(0.15, 0.45, 1.0);
    }

    /* Allocate working line buffers */
    size_t w = (size_t)width;
    float *src_r = (float *)malloc(w * sizeof(float));
    float *src_g = (float *)malloc(w * sizeof(float));
    float *src_b = (float *)malloc(w * sizeof(float));
    float *str_r = (float *)malloc(w * sizeof(float));
    float *str_g = (float *)malloc(w * sizeof(float));
    float *str_b = (float *)malloc(w * sizeof(float));
    float *pass_l = (float *)malloc(w * sizeof(float));
    float *pass_r = (float *)malloc(w * sizeof(float));

    if (!src_r || !src_g || !src_b || !str_r || !str_g || !str_b || !pass_l || !pass_r) {
        free(src_r); free(src_g); free(src_b);
        free(str_r); free(str_g); free(str_b);
        free(pass_l); free(pass_r);
        return 2;
    }

    /* Radii for dual-scale streak (core + extended tail) */
    float r1 = (float)(len_frac * (double)width * 0.12);
    if (r1 < 2.0f) r1 = 2.0f;
    float r2 = (float)(len_frac * (double)width);
    if (r2 < 6.0f) r2 = 6.0f;

    for (int y = 0; y < height; ++y) {
        /* 1. Extract highlight energy on this scanline */
        int has_highlights = 0;
        for (int x = 0; x < width; ++x) {
            size_t idx = ((size_t)y * w + (size_t)x) * 3;
            double r = (double)rgb[idx + 0] / 255.0;
            double g = (double)rgb[idx + 1] / 255.0;
            double b = (double)rgb[idx + 2] / 255.0;

            /* Metric: combination of luminance and peak channel */
            double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            double max_ch = r > g ? (r > b ? r : b) : (g > b ? g : b);
            double metric = 0.5 * (lum + max_ch);

            if (metric > thresh) {
                double excess = (metric - thresh) / (1.0 - thresh);
                double energy = excess * intensity;

                /* Blend original highlight tint with anamorphic flare color */
                src_r[x] = (float)((0.25 * r + 0.75 * tint.x) * energy);
                src_g[x] = (float)((0.25 * g + 0.75 * tint.y) * energy);
                src_b[x] = (float)((0.25 * b + 0.75 * tint.z) * energy);
                has_highlights = 1;
            } else {
                src_r[x] = 0.0f;
                src_g[x] = 0.0f;
                src_b[x] = 0.0f;
            }

            str_r[x] = 0.0f;
            str_g[x] = 0.0f;
            str_b[x] = 0.0f;
        }

        if (!has_highlights) {
            continue; /* Scanline has no bright highlights; skip blur pass */
        }

        /* 2. Dual-scale exponential streak filter */
        /* Scale 1: intense core flare (weight 0.45) */
        flare_filter_channel(src_r, str_r, width, r1, 0.45f, pass_l, pass_r);
        flare_filter_channel(src_g, str_g, width, r1, 0.45f, pass_l, pass_r);
        flare_filter_channel(src_b, str_b, width, r1, 0.45f, pass_l, pass_r);

        /* Scale 2: wide cinematic streak (weight 0.55) */
        flare_filter_channel(src_r, str_r, width, r2, 0.55f, pass_l, pass_r);
        flare_filter_channel(src_g, str_g, width, r2, 0.55f, pass_l, pass_r);
        flare_filter_channel(src_b, str_b, width, r2, 0.55f, pass_l, pass_r);

        /* 3. Additive composite back into RGB buffer */
        for (int x = 0; x < width; ++x) {
            size_t idx = ((size_t)y * w + (size_t)x) * 3;
            int r_out = (int)((float)rgb[idx + 0] + str_r[x] * 255.0f + 0.5f);
            int g_out = (int)((float)rgb[idx + 1] + str_g[x] * 255.0f + 0.5f);
            int b_out = (int)((float)rgb[idx + 2] + str_b[x] * 255.0f + 0.5f);

            rgb[idx + 0] = (unsigned char)(r_out > 255 ? 255 : (r_out < 0 ? 0 : r_out));
            rgb[idx + 1] = (unsigned char)(g_out > 255 ? 255 : (g_out < 0 ? 0 : g_out));
            rgb[idx + 2] = (unsigned char)(b_out > 255 ? 255 : (b_out < 0 ? 0 : b_out));
        }
    }

    free(src_r); free(src_g); free(src_b);
    free(str_r); free(str_g); free(str_b);
    free(pass_l); free(pass_r);
    return 0;
}
