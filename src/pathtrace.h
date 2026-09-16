#ifndef RAYTRACER_PATHTRACE_H
#define RAYTRACER_PATHTRACE_H

/*
 * pathtrace.h - Unbiased UNIDIRECTIONAL path tracer (opt-in render mode).
 *
 * A self-contained, iterative (NO recursion) Monte-Carlo path tracer that
 * reuses the existing scene / material / sampling infrastructure. It is a
 * STRICTLY ADDITIVE module: it does not modify src/render.{h,c}, so the
 * default Whitted render stays byte-identical.
 *
 * The kernel accumulates LINEAR radiance; gamma encoding / quantization and
 * the top-down row-major RGB layout match render_image() so the two modes are
 * visually comparable.
 *
 * Determinism: every random draw is a pure function of (pixel, sample, bounce,
 * fixed channel constant) via a private hash PRNG, NEVER of the thread or tile
 * identity, so the image is byte-identical for any thread count.
 */

#include "vec3.h"
#include "scene.h"
#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Trace one camera ray and return the accumulated LINEAR radiance reaching the
 * eye along it (no gamma encoding).
 *
 *   scene      : the scene to trace (not NULL).
 *   primary    : the camera ray; primary.dir MUST be unit length.
 *   max_depth  : hard bounce cap (>= 0). Each surface interaction is one
 *                bounce; the loop runs b = 0 .. max_depth.
 *   seed_key   : deterministic per-primary-ray key, e.g.
 *                render_hash3(x, y, sample_index). All bounce randomness is
 *                derived from it, so the result is independent of the
 *                thread/tile schedule.
 *
 * The estimator is the standard path-integral sum L = sum_b beta_b * Le_b:
 * it maintains a throughput `beta` (Vec3) and an accumulated `radiance` (Vec3)
 * in a simple iterative `for` loop over bounces (NEE adds direct lighting at
 * each non-specular vertex; Russian Roulette terminates paths past a few
 * bounces). Pure with respect to the scene.
 */
Vec3 pathtrace_radiance(const Scene *scene, Ray primary, int max_depth,
                        unsigned seed_key);

/*
 * Gamma-encode one LINEAR channel and quantize it to an 8-bit byte.
 *
 * Matches the default renderer's tone mapping exactly (clamp to [0, 1],
 * `pow(x, 1/2.2)`, round to nearest, clamp to [0, 255]); NaN is treated as 0.
 * Used by the path-trace render entry point (t-104c) so both render modes share
 * one quantization convention.
 */
unsigned char pathtrace_to_byte(double linear);

/*
 * Render `scene` from `cam` into a TOP-DOWN, row-major RGB buffer of
 * `width*height*3` bytes (pixel (x,y) at rgb[(y*width+x)*3 + 0..2]), exactly
 * matching render_image()'s contract.
 *
 *   samples_per_pixel >= 1  (Monte-Carlo samples per pixel)
 *   max_depth         >= 0  (bounce cap)
 *
 * Returns 0 on success, non-zero on invalid arguments (1 = NULL pointer,
 * 2 = non-positive dimensions, 3 = bad sample/depth counts), i.e. the same
 * failure codes as render_image().
 *
 * Supports BOTH the single-threaded build and the -DUSE_PTHREADS build: the
 * threaded build schedules fixed-size tiles via an atomic claim counter and
 * reports the same stderr progress meter as render_image() (silenced by
 * RAYTRACER_NO_PROGRESS=1). Progress never affects pixel data.
 */
int pathtrace_render(const Scene *scene, const Camera *cam, int width, int height,
                     int samples_per_pixel, int max_depth,
                     unsigned char *rgb_out);

#ifdef __cplusplus
}
#endif

#endif /* RAYTRACER_PATHTRACE_H */
