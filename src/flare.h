#ifndef RAYTRACER_FLARE_H
#define RAYTRACER_FLARE_H

/*
 * flare.h - Anamorphic lens flare / horizontal streak filter.
 *
 * Simulates the cinematic horizontal streaks characteristic of anamorphic
 * camera lenses when exposed to bright light sources (the sun, emissive lamps,
 * and high-specular highlights).
 *
 * Algorithm:
 *   1. Luminance & peak extraction: isolates pixels exceeding a bright threshold.
 *   2. Dual-scale bidirectional exponential IIR filter along each scanline (O(N) linear time,
 *      energy-conserving, zero-phase symmetric decay).
 *   3. Color modulation with an anamorphic tint (classic sci-fi cyan/blue by default).
 *   4. Additive / soft-saturating composite back into the 24-bit RGB framebuffer.
 *
 * Thread-safety:
 *   flare_apply() modifies the caller's RGB framebuffer in-place. Rows are processed
 *   independently; uses temporary row buffers only. No global state, no rand(), no I/O.
 */

#include "vec3.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int    enabled;          /* 1 = enabled, 0 = disabled */
    double intensity;        /* flare brightness multiplier [0.0, 10.0] (default 0.6) */
    double threshold;        /* minimum luminance to trigger flare [0.0, 1.0] (default 0.85) */
    double streak_length;    /* streak half-width as fraction of image width [0.01, 1.0] (default 0.25) */
    Vec3   tint;             /* RGB tint of the streak (default: vec3(0.15, 0.45, 1.0)) */
} FlareParams;

#define FLARE_DEFAULT_ENABLED       0
#define FLARE_DEFAULT_INTENSITY     0.6
#define FLARE_DEFAULT_THRESHOLD     0.85
#define FLARE_DEFAULT_STREAK_LENGTH 0.25

/* Initialize flare parameters to documented defaults (disabled by default). */
void flare_default_params(FlareParams *f);

/*
 * Apply anamorphic horizontal streak lens flare to a top-down RGB image buffer
 * of `width * height * 3` bytes. Modifies `rgb` in-place.
 *
 * Returns 0 on success, non-zero on invalid arguments.
 */
int flare_apply(unsigned char *rgb, int width, int height, const FlareParams *params);

#ifdef __cplusplus
}
#endif

#endif /* RAYTRACER_FLARE_H */
