#ifndef RAYTRACER_RENDER_H
#define RAYTRACER_RENDER_H

/*
 * render.h - Whitted-style recursive raytracer entry point.
 *
 * Turns a Scene + Camera into a TOP-DOWN, row-major RGB byte buffer.
 * Pure with respect to the scene (const inputs); the only side effects are
 * the caller-supplied output buffer and an optional progress line on stderr.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "scene.h"
#include "camera.h"

/*
 * Render the scene to a TOP-DOWN, row-major RGB buffer of `width*height*3`
 * bytes (pixel (x,y) at rgb[(y*width + x)*3 + 0..2] = R,G,B; y = 0 is the top
 * row).
 *
 *   samples_per_pixel  >= 1  (anti-aliasing samples per pixel)
 *   max_depth          >= 0  (recursion depth limit)
 *
 * Returns 0 on success, non-zero on failure (e.g. invalid arguments).
 */
int render_image(const Scene *scene, const Camera *cam, int width, int height,
                 int samples_per_pixel, int max_depth, unsigned char *rgb_out);

/*
 * Render parameters for the OPT-IN adaptive entry point render_image_ex().
 *
 * The struct is a strict superset of render_image()'s argument list: with
 * `adaptive == 0` the result is BYTE-IDENTICAL to render_image() for any
 * thread count and any `samples_per_pixel`.
 *
 *   samples_per_pixel  base sample count n0, >= 1 (the value of --samples)
 *   max_depth          recursion depth limit, >= 0
 *   adaptive           0 = fixed (default), non-zero = adaptive refinement
 *   adaptive_max_spp   cap N_max (>= n0); <= 0 selects the default 4 * n0
 *   adaptive_tau       relative-error tolerance tau; <= 0 selects 0.02
 *
 * Adaptive sampling refines a pixel while its relative standard error of the
 * mean luminance exceeds `adaptive_tau`, spending more samples on noisy /
 * high-contrast pixels and fewer on flat ones, up to `adaptive_max_spp`.
 */
typedef struct {
    int    samples_per_pixel;
    int    max_depth;
    int    adaptive;
    int    adaptive_max_spp;
    double adaptive_tau;
} RenderParams;

/*
 * Render with explicit parameters. `params` must not be NULL. Returns 0 on
 * success, non-zero on failure (same codes as render_image()).
 *
 * When `params->adaptive == 0` this delegates to render_image() verbatim, so
 * the fixed-spp path (arithmetic, summation order, gamma/quantize) is
 * untouched and byte-identical to today.
 */
int render_image_ex(const Scene *scene, const Camera *cam, int width, int height,
                    const RenderParams *params, unsigned char *rgb_out);

/*
 * Render time, in seconds, of the most recent render_image / render_image_ex
 * call (single-threaded or wall-clock if threaded). Diagnostic only.
 */
double render_last_seconds(void);

/*
 * Total primary samples taken by the most recent render (sum over all pixels).
 * Diagnostic only; useful for verifying that adaptive refinement engaged.
 */
unsigned long long render_last_total_samples(void);

/*
 * Largest per-pixel sample count in the most recent render (== samples_per_pixel
 * for the fixed path). Diagnostic only; useful for verifying the N_max cap.
 */
int render_last_max_samples(void);

/*
 * ---------------------------------------------------------------------------
 * Render progress indicator (stderr-only, thread-safe, opt-out)
 * ---------------------------------------------------------------------------
 *
 * The renderer draws a single, self-overwriting progress line on stderr, e.g.
 *
 *     render:  42% [00:12<00:16, 1.2 Mpx/s]
 *
 * consisting of the completion percentage, elapsed wall-clock time, estimated
 * time remaining (ETA) and throughput. The line is redrawn at most every
 * ~120 ms (throttled) and is always finished with a newline, so it never
 * garbles stdout (which main.c uses for the BMP/PPM path) and never interleaves
 * with itself in the threaded build.
 *
 * Progress is ENABLED by default and silenced by setting the environment
 * variable RAYTRACER_NO_PROGRESS to any non-NULL value (main.c also exposes a
 * matching --no-progress flag which does the same). It is strictly diagnostic:
 * it never touches pixel data, so the rendered bytes are IDENTICAL with and
 * without progress, for any thread count.
 */

/* Non-zero when the progress meter is enabled (i.e. RAYTRACER_NO_PROGRESS is
 * unset). Public so main.c can print a matching "Progress: off" summary line. */
int render_progress_enabled(void);

/*
 * Live progress state shared between a render and the thread that reports it.
 * `total_expected` is the upper bound of the whole render in work units (rows
 * for the fixed path; rows summed over all adaptive passes), and `pixels_total`
 * is the pixel budget used for the Mpx/s readout (0 to report rows/s instead).
 * A zero-initialised struct (see render_progress_init) is valid in both builds.
 */
typedef struct {
    long long total;          /* completed work units so far            */
    long long total_expected; /* upper bound of the whole render        */
    long long pixels_total;   /* pixels in the image (0 = rows/s mode)  */
} RenderProgress;

/* Initialise `p` to a stopped, zeroed state (both builds). */
void render_progress_init(RenderProgress *p);

/*
 * Open the renderer's stderr progress line: reset the counter, enable
 * reporting for the next render, and print a 0% line. When `p` is NULL (or the
 * build has no threads) a process-wide state is used instead, so the legacy
 * render_image()/render_image_ex() entry points report without any caller
 * plumbing. Safe to call when progress is disabled (it becomes a no-op).
 */
void render_progress_begin(RenderProgress *p);

/*
 * Report progress: redraw the meter when at least ~120 ms have elapsed since
 * the last redraw (so it is cheap enough to call on every work unit), and
 * always draw the 100% line once when `done == total_expected` (or `done` is
 * negative). Thread-safe; `done` is a monotonic count of completed units.
 */
void render_progress_update(RenderProgress *p, long long done);

/*
 * Finalise the meter (guaranteeing a 100% line even for empty/aborted renders)
 * and terminate it with a newline. No-op when progress is disabled.
 */
void render_progress_finish(RenderProgress *p);

#ifdef __cplusplus
}
#endif

#endif /* RAYTRACER_RENDER_H */
