/*
 * render.c - Whitted-style recursive raytracer.
 *
 * Pipeline per pixel:
 *   stratified (or uniform) jittered anti-aliasing samples -> primary rays
 *   (thin-lens depth-of-field when the camera aperture is non-zero)
 *   -> recursive `trace()` (ambient + hard-sun Blinn-Phong, mirror reflection,
 *   Fresnel-weighted refraction with Beer-Lambert tinting)
 *   -> average linear color -> gamma 1/2.2 -> 8-bit quantize.
 *
 * Determinism: all randomness comes from a small integer hash-based PRNG
 * seeded from (x, y, sample index, fixed constant). No rand(), no time().
 *
 * Threading: when compiled with -DUSE_PTHREADS the image is divided into
 * fixed-size tiles and a pool of workers claims tiles from a shared atomic
 * counter (dynamic load balancing). Each worker writes only inside its own
 * tile, so the disjoint regions never race. Without the macro the renderer is
 * a plain single-threaded loop and never references pthread.
 */

#include "render.h"

#include "scene.h"
#include "camera.h"
#include "geometry.h"
#include "material.h"
#include "texture.h"
#include "vec3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdatomic.h>   /* C11: progress guard flag, used in BOTH builds */

#ifdef USE_PTHREADS
#include <pthread.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Wall-clock timing helper                                            */
/* ------------------------------------------------------------------ */

static double render_now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/*
 * Last-render diagnostics (shared by the fixed and adaptive paths). Defined
 * here so the adaptive engine below can update them before the public entry
 * point; the accessors live with render_image().
 */
static double g_render_last_seconds = 0.0;
static unsigned long long g_render_last_total_samples = 0;
static int g_render_last_max_samples = 0;

/* ------------------------------------------------------------------ */
/* Render progress indicator (stderr-only, thread-safe)                */
/* ------------------------------------------------------------------ */

/*
 * Minimum wall-clock gap between two redraws of the meter. Kept comfortably
 * above the ~100 ms floor so that reporting a work unit is a couple of atomic
 * ops + a clock read at most, and never a measurable render cost.
 */
#define RENDER_PROGRESS_MIN_INTERVAL 0.12

/*
 * State of the currently active progress line. A single global line is enough
 * because the renderer is never called re-entrantly, and it lets the legacy
 * entry points (which take no progress argument) report as well as main.c.
 *
 * Concurrency contract:
 *   - `active`, `enabled`, `start`, `last_emit`, `total`, `total_expected` and
 *     `pixels_total` are written only by the single report winner (see
 *     render_progress_update) or, for `active`/`enabled`/`start`/..., by the
 *     caller thread in render_progress_begin()/finish() (before/after the
 *     workers run). They are never concurrently mutated.
 *   - `finished` IS written by a worker (the winner that draws the final 100%
 *     line) and read by every other worker on the fast path, so it is an
 *     atomic flag; that is the only cross-thread mutable field of the struct.
 */
typedef struct {
    int          active;      /* a line has been opened and not finished  */
    int          enabled;     /* progress reporting is on                 */
    atomic_int   finished;    /* final 100% line already emitted (atomic) */
    double       start;       /* CLOCK_MONOTONIC seconds at begin         */
    double       last_emit;   /* time of the last redraw                  */
    long long    total;       /* completed work units                     */
    long long    total_expected;
    long long    pixels_total; /* total pixels, for the Mpx/s readout (0=off) */
} ProgressState;

static ProgressState g_prog = { 0, 1, 1, 0.0, 0.0, 0, 0, 0 };

#ifdef USE_PTHREADS
/*
 * Throttle + single-writer gate, in two parts:
 *
 *   g_prog_last_emit_ns  wall-clock time (CLOCK_MONOTONIC, ns) of the last
 *                        redraw, used to bound the redraw rate to one per
 *                        RENDER_PROGRESS_MIN_INTERVAL regardless of how many
 *                        tiles complete in between.
 *   g_prog_emitting      a lightweight test-and-set lock held ONLY while a
 *                        worker is inside render_progress_emit(). It makes the
 *                        "at most one thread writes to stderr" property
 *                        unconditional: even if a write/flush ever blocks for
 *                        longer than the throttle interval, a second worker
 *                        cannot start a concurrent write and the line can
 *                        never interleave.
 *   g_prog_epoch         bumped whenever a new line is opened, so a worker
 *                        that loses the epoch race discards its (now stale)
 *                        win without writing.
 *
 * A worker first passes the (cheap, atomic) throttle check, then tries to take
 * g_prog_emitting; the loser simply returns. Only the winner refreshes the
 * timestamp and touches stderr.
 */
static atomic_llong g_prog_last_emit_ns = 0;
static atomic_int   g_prog_emitting = 0;
static atomic_llong g_prog_epoch = 0;

/* Current CLOCK_MONOTONIC time in whole nanoseconds (monotonic, no wall jumps). */
static long long render_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}
#endif

int render_progress_enabled(void)
{
    const char *noprog = getenv("RAYTRACER_NO_PROGRESS");
    return (noprog == NULL) ? 1 : 0;
}

void render_progress_init(RenderProgress *p)
{
    if (p == NULL) {
        return;
    }
    p->total = 0;
    p->total_expected = 0;
    p->pixels_total = 0;
}

/* Format `seconds` as MM:SS (clamped at zero). */
static void render_progress_mmss(double seconds, char *buf, size_t n)
{
    long s = (long)(seconds + 0.5);
    if (s < 0) {
        s = 0;
    }
    snprintf(buf, n, "%02ld:%02ld", s / 60, s % 60);
}

/*
 * Draw the meter at `done` / `total_expected`. Called ONLY by the single
 * report winner (see render_progress_update). Writes exclusively to stderr and
 * uses \r to overwrite the previous line, so stdout is never touched.
 */
static void render_progress_emit(ProgressState *st, long long done)
{
    double now = render_now_seconds();
    double elapsed = now - st->start;
    if (elapsed < 0.0) {
        elapsed = 0.0;
    }

    long long total = st->total_expected;
    if (done < 0) {
        done = total;
    }
    /* Monotonicity: a late report winner may carry a `done` smaller than a
     * value another worker already printed (the counters are unique but the
     * winners race), so never move the meter backwards. `st->total` is the
     * last printed count and is only mutated by the single report winner. */
    if (done < st->total) {
        done = st->total;
    }
    if (total > 0 && done > total) {
        done = total;
    }

    int pct = (total > 0) ? (int)((100.0 * (double)done) / (double)total) : 100;
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }

    char el[16];
    char et[16];
    render_progress_mmss(elapsed, el, sizeof el);

    /* Work rate and ETA from the completed fraction; only meaningful once
     * something has finished and the clock has advanced. */
    int have_eta = 0;
    if (total > 0 && done > 0 && elapsed > 1e-6) {
        double frac = (double)done / (double)total;
        double rate = (double)done / elapsed;         /* units/s */
        double eta = elapsed * (1.0 - frac) / frac;   /* seconds */
        if (eta < 0.0) {
            eta = 0.0;
        }
        render_progress_mmss(eta, et, sizeof et);

        /* Prefer a pixel rate when the renderer told us the pixel budget;
         * fall back to the raw row rate for internal callers. The unit is
         * chosen adaptively so the number stays readable at every scale. */
        if (st->pixels_total > 0) {
            double px_done = frac * (double)st->pixels_total;
            double px_rate = px_done / elapsed;
            if (px_rate >= 1.0e6) {
                fprintf(stderr, "\rrender: %3d%% [%s<%s, %.1f Mpx/s]",
                        pct, el, et, px_rate / 1.0e6);
            } else if (px_rate >= 1.0e3) {
                fprintf(stderr, "\rrender: %3d%% [%s<%s, %.1f kpx/s]",
                        pct, el, et, px_rate / 1.0e3);
            } else {
                fprintf(stderr, "\rrender: %3d%% [%s<%s, %.0f px/s]",
                        pct, el, et, px_rate);
            }
        } else {
            fprintf(stderr, "\rrender: %3d%% [%s<%s, %.0f rows/s]",
                    pct, el, et, rate);
        }
        have_eta = 1;
    }

    if (!have_eta) {
        fprintf(stderr, "\rrender: %3d%% [%s<--:--, --]", pct, el);
    }

    fflush(stderr);
    st->last_emit = now;
    st->total = done;

    if (total > 0 && done >= total) {
        atomic_store(&st->finished, 1);
        fputc('\n', stderr);
        fflush(stderr);
    }
}

void render_progress_begin(RenderProgress *p)
{
    if (!render_progress_enabled()) {
        return;
    }

    g_prog.active = 1;
    g_prog.enabled = 1;
    atomic_store(&g_prog.finished, 0);
    g_prog.start = render_now_seconds();
    g_prog.last_emit = g_prog.start - RENDER_PROGRESS_MIN_INTERVAL;
    g_prog.total = 0;
    g_prog.total_expected = (p != NULL) ? p->total_expected : 0;
    g_prog.pixels_total = (p != NULL) ? p->pixels_total : 0;

    if (p != NULL) {
        p->total = 0;
    }

#ifdef USE_PTHREADS
    atomic_store(&g_prog_epoch, atomic_load(&g_prog_epoch) + 1);
    atomic_store(&g_prog_last_emit_ns, 0);
    atomic_store(&g_prog_emitting, 0); /* no worker holds the emit lock yet */
#endif

    /* 0% line immediately, so the user sees the meter as soon as work starts. */
    render_progress_emit(&g_prog, 0);
}

void render_progress_update(RenderProgress *p, long long done)
{
    (void)p; /* the reporting line lives in the process-wide g_prog */

    /* Fast path shared by every worker: `active`/`enabled` are written only
     * before the pool starts (begin) or after it joins (finish), so reading
     * them here is race-free. `finished` is atomic because the worker that
     * draws the final line sets it while others are still polling. */
    if (!g_prog.active || !g_prog.enabled ||
        atomic_load_explicit(&g_prog.finished, memory_order_relaxed)) {
        return;
    }
    if (done < 0) {
        render_progress_emit(&g_prog, -1);
        return;
    }

#ifdef USE_PTHREADS
    /*
     * Throttle + single-writer gate. A worker redraws only when the shared
     * timestamp is older than the throttle interval (CAS'ed forward to "now"),
     * and only after winning the test-and-set emit lock; the lock is held
     * across the whole render_progress_emit() call, so two threads can never
     * write to stderr concurrently and the line can never interleave. A forced
     * final line (done < 0) bypasses the throttle so completion is always
     * reported, but still goes through the lock so it stays single-writer.
     */
    long long now_ns = render_now_ns();
    const long long min_ns = (long long)(RENDER_PROGRESS_MIN_INTERVAL * 1.0e9);
    long long epoch = atomic_load(&g_prog_epoch);
    int won = 0;
    for (;;) {
        long long prev = atomic_load(&g_prog_last_emit_ns);
        if (done >= 0 && prev != 0 && now_ns - prev < min_ns) {
            return; /* throttled: not yet time to redraw */
        }
        if (atomic_compare_exchange_weak(&g_prog_last_emit_ns, &prev, now_ns)) {
            won = 1;
            break;
        }
    }
    if (!won || epoch != atomic_load(&g_prog_epoch)) {
        return; /* lost a race or a newer line was opened: skip */
    }
    /* Take the single-writer emit lock; a worker already inside emit() wins
     * and we skip this redraw entirely (the next tile will try again). */
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_prog_emitting, &expected, 1)) {
        return;
    }
    if (atomic_load(&g_prog_epoch) != epoch) {
        atomic_store(&g_prog_emitting, 0);
        return; /* a newer line was opened while we waited for the lock */
    }
    render_progress_emit(&g_prog, done);
    atomic_store(&g_prog_emitting, 0);
#else
    /* Single thread: a plain clock-based throttle is enough. */
    if (render_now_seconds() - g_prog.last_emit < RENDER_PROGRESS_MIN_INTERVAL &&
        done < g_prog.total_expected) {
        return;
    }
    render_progress_emit(&g_prog, done);
#endif
}

void render_progress_finish(RenderProgress *p)
{
    (void)p;
    if (!g_prog.active || !g_prog.enabled) {
        return;
    }
    /* Runs on the caller thread after the worker pool has been joined, so
     * there is no concurrent writer here; the atomic read keeps the flag's
     * access uniform with the workers' fast path. */
    if (!atomic_load_explicit(&g_prog.finished, memory_order_relaxed)) {
        render_progress_emit(&g_prog, -1); /* guarantee a 100% line */
    }
    g_prog.active = 0;
}

/* ------------------------------------------------------------------ */
/* Small integer hash PRNG (deterministic, no libc rand)               */
/* ------------------------------------------------------------------ */

/*
 * 32-bit integer avalanche (a variant of the "wang hash"/murmur finalizer
 * family). Pure function of its input -> reproducible across runs/platforms.
 */
static unsigned render_hash_u32(unsigned x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* Mix three 32-bit integers into one well-distributed 32-bit value. */
static unsigned render_hash3(unsigned a, unsigned b, unsigned c)
{
    unsigned h = 0x9e3779b9u;
    h ^= render_hash_u32(a + 0x85ebca6bu);
    h = render_hash_u32(h + b);
    h ^= render_hash_u32(c + 0xc2b2ae35u);
    return render_hash_u32(h);
}

/* Uniform double in [0, 1) from the hash of (a, b, c). */
static double render_rand01(unsigned a, unsigned b, unsigned c)
{
    /* 24 bits of mantissa-scale precision -> [0, 1). */
    return (double)(render_hash3(a, b, c) >> 8) * (1.0 / 16777216.0);
}

/* ------------------------------------------------------------------ */
/* Soft shadows (sun-disk) constants                                   */
/* ------------------------------------------------------------------ */

/*
 * Number of shadow rays cast per lit hit when `sky.sun_radius > 0`. The
 * unoccluded fraction is averaged into the direct-light term, giving penumbral
 * soft shadows. Cost is linear in this count; with sun_radius == 0 the
 * single-ray hard-shadow path is used instead and this constant is unused.
 */
#define SUN_SHADOW_SAMPLES 16

/*
 * Angular radii (degrees) at or below this are treated as a point sun, i.e. a
 * hard shadow. This keeps the default render (sun_radius == 0) on the exact
 * original single-ray code path, byte-for-byte.
 */
#define SUN_RADIUS_EPSILON 1e-9

/*
 * Glossy (roughness-blurred) reflection constants.
 *
 * `GLOSSY_REFLECTION_SAMPLES` recursive rays are importance-sampled over the
 * GGX/Trowbridge-Reitz lobe and averaged with uniform 1/N weights. This path
 * is entered ONLY for opt-in PBR materials with a rough, reflective surface
 * (see the gate in trace_hit), so the default and every non-PBR render pay
 * ZERO extra cost. `GLOSSY_ROUGHNESS_EPSILON` is defined in material.h so the
 * gate here and the sampler short-circuit there can never disagree.
 */
#define GLOSSY_REFLECTION_SAMPLES 16

/*
 * PRNG channel constants for the glossy-reflection jitter. They are chosen
 * disjoint from the sun-disk channels (0x5a17u / 0x7c3du) and the DOF channels
 * (s*2+2 / s*2+3) so the glossy stream is statistically independent of both,
 * leaving soft-shadow and depth-of-field output untouched.
 */
#define GLOSSY_CHANNEL_A 0x9e37u
#define GLOSSY_CHANNEL_B 0x85ebu

/* ------------------------------------------------------------------ */
/* Emissive area lights                                                */
/* ------------------------------------------------------------------ */

/*
 * Number of solid-angle samples taken per emissive area light per lit hit
 * (matches SUN_SHADOW_SAMPLES). Cost per non-emitter hit is
 * O(emissive_light_count * EMISSIVE_LIGHT_SAMPLES) extra shadow rays, bounded
 * by SCENE_MAX_EMISSIVE_LIGHTS (8) * this count.
 */
#define EMISSIVE_LIGHT_SAMPLES 16

/*
 * PRNG channel constants for the emissive-area-light jitter. Chosen disjoint
 * from the sun-disk channels (0x5a17u / 0x7c3du), the glossy channels
 * (0x9e37u / 0x85ebu) and the DOF channels (s*2+2 / s*2+3), and the per-light
 * key salt 0xE311u is disjoint from the glossy depth salt, so the emissive
 * stream is statistically independent of every existing stream. The existing
 * sun-disk / glossy / DOF draws are therefore byte-for-byte unchanged.
 */
#define EMISSIVE_LIGHT_CHANNEL_KEY 0xE311u
#define EMISSIVE_LIGHT_CHANNEL_A   0x4c11u
#define EMISSIVE_LIGHT_CHANNEL_B   0x9d27u

/* ------------------------------------------------------------------ */
/* Recursive Whitted trace                                             */
/* ------------------------------------------------------------------ */

/* Forward declaration: trace_hit() recurses back through trace(). */
static Vec3 trace(const Scene *scene, Ray r, int depth, int max_depth,
                  double time, unsigned seed_key);

/* Local PI so we do not depend on M_PI (POSIX-only). */
#define RENDER_PI 3.14159265358979323846

/*
 * Direct lighting from the scene's emissive PBR primitives, treated as sampled
 * AREA LIGHTS. Called from trace_hit() ONLY when scene->emissive_light_count >
 * 0, so a scene without an emissive PBR material never runs this code and
 * renders byte-identically to the pre-area-light renderer.
 *
 * For each emitter sphere (skipping the one the shading point lies on, so a
 * lamp is not lit by itself) we integrate the rendering equation in SOLID-
 * ANGLE form,
 *
 *     L_o += ∫ f_r(w_i, w_o) · Le · (N · w_i) dω_i
 *
 * with uniform-in-solid-angle samples over the sphere's VISIBLE CAP:
 *
 *     dc            = |C - P|,  w = (C - P)/dc
 *     sin(alpha_max)= R/dc,  cos(alpha_max) = sqrt(1 - (R/dc)^2)
 *     Omega         = 2π·(1 - cos(alpha_max))       (= 1 / pdf)
 *
 * The single-sample estimator is therefore
 *
 *     f_r(w_i, w_o) · Le · (N · w_i) · V(P, Q) / pdf
 *         = f_r(w_i, w_o) · Le · (N · w_i) · V(P, Q) · Omega
 *
 * Le is RADIANCE (distance-independent), so no separate 1/dc^2 or cos(light)
 * factor is added: the inverse-square falloff and the light-side cosine are
 * already folded into the solid-angle measure. We fold Le·Omega into
 * `light_color` and let the existing material_shade_pbr / material_shade_local
 * helpers produce the f_r·(N·w_i) part, exactly mirroring the sun path.
 *
 * All randomness is a pure function of (seed_key, light index, sample index)
 * via the hash PRNG, using channel constants DISJOINT from the sun-disk /
 * glossy / DOF streams, so the result is thread/tile independent and the
 * existing streams are untouched.
 */
static Vec3 emissive_direct(const Scene *scene, const Material *mm,
                            Vec3 P, Vec3 N, Vec3 V, int self_prim,
                            unsigned seed_key)
{
    Vec3 sum = vec3(0.0, 0.0, 0.0);
    Vec3 shadow_o = vec3_add(P, vec3_scale(N, 1e-3)); /* same eps as the sun path */
    int li;

    for (li = 0; li < scene->emissive_light_count; ++li) {
        const EmissiveLight *lt = &scene->emissive_lights[li];
        Vec3 to_c, w, radiance;
        double dc2, dc, cos_mx, solid;
        int si;

        if (lt->prim_index == self_prim) {
            continue; /* no self-illumination: the emitter is already self-lit */
        }

        to_c = vec3_sub(lt->center, P);
        dc2  = vec3_length_sq(to_c);
        dc   = sqrt(dc2);
        if (dc <= lt->radius) {
            continue; /* shading point inside/on the lamp: skip (no 1/0) */
        }

        /* Solid angle of the visible cap, and 1/pdf = Omega. */
        cos_mx = sqrt(1.0 - (lt->radius * lt->radius) / dc2);
        solid  = 2.0 * RENDER_PI * (1.0 - cos_mx);
        if (!(solid > 0.0)) {
            continue; /* degenerate cone */
        }
        w = vec3_scale(to_c, 1.0 / dc);

        for (si = 0; si < EMISSIVE_LIGHT_SAMPLES; ++si) {
            unsigned k = render_hash3(seed_key ^ EMISSIVE_LIGHT_CHANNEL_KEY,
                                      (unsigned)li, (unsigned)si);
            double u1 = render_rand01(k, (unsigned)si, EMISSIVE_LIGHT_CHANNEL_A);
            double u2 = render_rand01(k, (unsigned)si, EMISSIVE_LIGHT_CHANNEL_B);
            Vec3 w_i = light_sphere_sample_dir(w, cos_mx, u1, u2);
            Hit sh;
            int visible;

            if (vec3_dot(N, w_i) <= 0.0) {
                continue; /* sample lands below the shading hemisphere */
            }

            /* Accept when the nearest hit IS the emitter (robust; also
             * handles occluders behind the lamp correctly). */
            visible = !scene_intersect(scene, (Ray){shadow_o, w_i}, 1e-3, 1e30,
                                       &sh)
                   || sh.prim_index == lt->prim_index;
            if (!visible) {
                continue;
            }

            /* Le / pdf, folded with Omega (= 1/pdf). */
            radiance = vec3_scale(lt->emissive, solid);
            sum = vec3_add(sum, mm->pbr
                                    ? material_shade_pbr(mm, N, w_i, V, radiance)
                                    : material_shade_local(mm, N, w_i, V, radiance));
        }
    }

    return vec3_scale(sum, 1.0 / (double)EMISSIVE_LIGHT_SAMPLES);
}

/*
 * Shade an already-intersected hit. `h` MUST be the nearest hit along `r`
 * (as produced by scene_intersect with the same ray and epsilon), which lets
 * callers that have already traced the ray (e.g. the refraction path) reuse
 * that intersection instead of paying for a second one.
 *
 * `seed_key` is a deterministic per-primary-ray key (derived from pixel +
 * sample indices) used to jitter the sun-disk shadow samples. It is a pure
 * function of the pixel and sample, never of the thread or tile, so the output
 * is byte-identical for any thread count.
 */
static Vec3 trace_hit(const Scene *scene, Ray r, int depth, int max_depth,
                      double time, const Hit *h, unsigned seed_key)
{
    if (depth > max_depth) {
        return vec3(0.0, 0.0, 0.0);
    }

    const Material *m = scene_material(scene, h->material_index);
    if (!m) {
        return sky_sample(r.dir, &scene->sky);
    }

    Vec3 P = h->point;
    Vec3 N = h->normal; /* already flipped to oppose the ray */
    if (m->is_water) {
        N = water_normal(P.x, P.z, time); /* wave-perturbed normal */
    } else if (m->bump_strength > 1e-6) {
        N = texture_normal(m, P, N);
    }
    if (vec3_dot(N, r.dir) > 0) {
        N = vec3_neg(N); /* safety re-flip */
    }
    Vec3 V = vec3_neg(r.dir); /* toward the eye */

    /*
     * --- procedural texture: modulate the albedo at the hit point ---
     *
     * The texture is a pure function of the world-space position P, so it is
     * deterministic and thread-independent. We modulate a LOCAL copy of the
     * material and shade with that, leaving the pure material helpers (and
     * their signatures) untouched. The physical fields (ior, transparency,
     * reflectivity, is_water, beer_lambert, absorption, deep_color) are copied
     * verbatim, so reflection/refraction below behaves exactly as before.
     */
    Material m_local = *m;
    m_local.albedo = texture_albedo(m, P);
    const Material *mm = &m_local;

    /*
     * --- direct sunlight: hard or soft (sun-disk) shadow ---
     *
     * With sun_radius <= 0 the sun is point-like and we keep the exact
     * original single hard-shadow ray (byte-identical default render). For a
     * finite sun disk we cast SUN_SHADOW_SAMPLES rays toward deterministic
     * jittered directions on the disk and average the unoccluded fraction into
     * the direct term (penumbral soft shadow). Every jitter value is derived
     * solely from (seed_key, sample index) via the hash PRNG, so the result is
     * independent of the thread/tile schedule.
     */
    Vec3 color = material_ambient(mm, N, &scene->sky);
    Vec3 L = scene->sky.sun_dir;
    Vec3 shadow_o = vec3_add(P, vec3_scale(N, 1e-3));
    double sun_radius = scene->sky.sun_radius;
    if (sun_radius > SUN_RADIUS_EPSILON) {
        double lit = 0.0;
        for (int i = 0; i < SUN_SHADOW_SAMPLES; ++i) {
            unsigned k = seed_key * 0x9e3779b9u + (unsigned)i * 2u;
            double r1 = render_rand01(k, (unsigned)i, 0x5a17u);
            double r2 = render_rand01(k, (unsigned)i, 0x7c3du);
            Vec3 Ld = sky_sun_disk_dir(L, sun_radius, r1, r2);
            Hit sh;
            if (!scene_intersect(scene, (Ray){shadow_o, Ld}, 1e-3, 1e30, &sh)) {
                lit += 1.0;
            }
        }
        if (lit > 0.0) {
            /* Shade with the central sun direction; weight by visibility.
             * Opt-in PBR path when m->pbr is set; the legacy Blinn-Phong
             * statements are kept textually intact in the else branch so the
             * default render stays byte-identical. */
            if (mm->pbr) {
                color = vec3_add(color, vec3_scale(
                    material_shade_pbr(mm, N, L, V, scene->sky.sun_color),
                    lit / (double)SUN_SHADOW_SAMPLES));
            } else {
                color = vec3_add(color, vec3_scale(
                    material_shade_local(mm, N, L, V, scene->sky.sun_color),
                    lit / (double)SUN_SHADOW_SAMPLES));
            }
        }
    } else {
        Hit sh;
        if (!scene_intersect(scene, (Ray){shadow_o, L}, 1e-3, 1e30, &sh)) {
            if (mm->pbr) {
                color = vec3_add(color,
                                 material_shade_pbr(mm, N, L, V, scene->sky.sun_color));
            } else {
                color = vec3_add(color,
                                 material_shade_local(mm, N, L, V, scene->sky.sun_color));
            }
        }
    }

    /*
     * --- emissive PBR primitives as sampled area lights ---
     *
     * STRICT OPT-IN GATING: this block runs ONLY when the scene collected at
     * least one emissive PBR primitive. The list is empty for the built-in
     * default scene and for every scene without an emissive PBR material, so
     * those renders are byte-identical to the pre-area-light renderer (no
     * extra shading, no extra RNG draws).
     */
    if (scene->emissive_light_count > 0) {
        color = vec3_add(color, emissive_direct(scene, mm, P, N, V,
                                                h->prim_index, seed_key));
    }

    /* --- reflection --- */
    Vec3 refl_col = vec3(0.0, 0.0, 0.0);
    if (depth < max_depth) {
        /*
         * Opt-in roughness-blurred (glossy) reflection. It is taken ONLY for
         * PBR materials that actually reflect (pbr != 0 && reflectivity > 0)
         * and are rough (roughness > epsilon); in every other case the legacy
         * sharp-reflection statements run VERBATIM below, keeping the default
         * and all non-PBR / transparent renders byte-identical.
         */
        if (mm->pbr != 0 && m->reflectivity > 0.0 &&
            m->roughness > GLOSSY_ROUGHNESS_EPSILON) {
            const double inv = 1.0 / (double)GLOSSY_REFLECTION_SAMPLES;
            for (int i = 0; i < GLOSSY_REFLECTION_SAMPLES; ++i) {
                /*
                 * Deterministic, thread-independent jitter: a pure function of
                 * (seed_key, depth, sample index), never of the thread or tile.
                 * Folding in `depth` decorrelates successive glossy bounces.
                 */
                unsigned k = seed_key * 0x9e3779b9u
                           + (unsigned)depth * 0x85ebca6bu
                           + (unsigned)i * 2u;
                double r1 = render_rand01(k, (unsigned)i, GLOSSY_CHANNEL_A);
                double r2 = render_rand01(k, (unsigned)i, GLOSSY_CHANNEL_B);
                Vec3 rd = material_sample_glossy_dir(mm, N, r.dir, r1, r2);
                refl_col = vec3_add(refl_col, vec3_scale(
                    trace(scene,
                          (Ray){vec3_add(P, vec3_scale(N, 1e-3)), rd},
                          depth + 1, max_depth, time, seed_key), inv));
            }
        } else {
            Vec3 rd = vec3_reflect(r.dir, N);
            refl_col = trace(scene,
                             (Ray){vec3_add(P, vec3_scale(N, 1e-3)), rd},
                             depth + 1, max_depth, time, seed_key);
        }
    }

    /*
     * --- Fresnel (Schlick) ---
     *
     * Snell's law with eta = n1 / n2, where the incident medium is air when
     * entering (front_face == 1) and the material's ior when exiting
     * (front_face == 0). Evaluating Schlick at the REFRACTED cosine
     * cos(theta_t) instead of the incident cosine removes the non-physical
     * discontinuity at the critical angle when n1 > n2, and lets us detect
     * total internal reflection (sin^2(theta_t) > 1) explicitly.
     */
    double f0 = (1.0 - m->ior) / (1.0 + m->ior);
    f0 = f0 * f0; /* ~0.02 for water; symmetric in n1 <-> n2 */

    double cos_i = vec3_dot(N, V);
    if (cos_i < 0.0) {
        cos_i = 0.0;
    } else if (cos_i > 1.0) {
        cos_i = 1.0;
    }

    double eta = (h->front_face == 1) ? (1.0 / m->ior) : m->ior;

    /* sin^2(theta_t) = eta^2 * (1 - cos^2(theta_i)) */
    double sin2_t = eta * eta * (1.0 - cos_i * cos_i);

    double F;
    int total_internal = 0;
    if (sin2_t > 1.0) {
        /* Total internal reflection: no transmitted ray, reflect everything. */
        F = 1.0;
        total_internal = 1;
    } else {
        double cos_t = sqrt(1.0 - sin2_t);
        F = fresnel_schlick(cos_t, f0);
    }

    if (m->transparency > 0.0) {
        /* --- refraction with Beer-Lambert attenuation --- */
        Vec3 refr_col;
        if (total_internal) {
            refr_col = refl_col; /* 100% of the energy is reflected */
        } else {
            /* vec3_refract() cannot fail here: we already verified
             * sin^2(theta_t) <= 1 above, so no TIR zero-vector can occur. */
            Vec3 rd = vec3_refract(r.dir, N, eta);
            Ray rr = {vec3_add(P, vec3_scale(rd, 1e-3)), rd};
            Hit h2;
            if (scene_intersect(scene, rr, 1e-4, 1e30, &h2)) {
                /* Reuse the exit hit we just computed: trace_hit() shades it
                 * directly, so this ray is intersected exactly once. */
                Vec3 inner = trace_hit(scene, rr, depth + 1, max_depth, time, &h2,
                                       seed_key);
                if (m->is_water || m->beer_lambert) {
                    /* `rr` starts at the entry point, so the exit-hit
                     * parameter h2.t IS the propagation distance inside the
                     * water. Attenuate only while actually inside the medium. */
                    refr_col = water_attenuate(m, inner, h2.t);
                } else {
                    refr_col = inner;
                }
            } else {
                Vec3 inner = sky_sample(rd, &scene->sky);
                if (m->is_water || m->beer_lambert) {
                    /* Ray exits the water body without a further hit: apply a
                     * representative "deep water" path length. */
                    refr_col = water_attenuate(m, inner, 5.0);
                } else {
                    refr_col = inner;
                }
            }
        }
        Vec3 surface = vec3_add(vec3_scale(refl_col, F),
                                vec3_scale(refr_col, 1.0 - F));
        Vec3 out = vec3_lerp(color, surface, m->transparency);
        /* Opt-in self-emission, added ONCE per shaded hit (not per light),
         * independent of transparency/reflectivity. Guarded by pbr and a
         * non-zero emissive so the legacy return is byte-identical. */
        if (mm->pbr && (m->emissive.x != 0.0 || m->emissive.y != 0.0 ||
                        m->emissive.z != 0.0)) {
            out = vec3_add(out, m->emissive);
        }
        return out;
    }

    Vec3 out = vec3_lerp(color, refl_col, m->reflectivity);
    /* Opt-in self-emission, added ONCE per shaded hit (not per light). */
    if (mm->pbr && (m->emissive.x != 0.0 || m->emissive.y != 0.0 ||
                    m->emissive.z != 0.0)) {
        out = vec3_add(out, m->emissive);
    }
    if (depth == 0 && vec3_length_sq(mm->atmosphere_glow) > 1e-6) {
        double cos_v = fmax(0.0, vec3_dot(N, V));
        double limb = pow(1.0 - cos_v, 3.5);
        double sun_dot = vec3_dot(N, scene->sky.sun_dir);
        double day_fac = 0.0;
        if (sun_dot > -0.15) {
            day_fac = (sun_dot + 0.15) / 0.35;
            if (day_fac > 1.0) day_fac = 1.0;
        }
        Vec3 glow_col = mm->atmosphere_glow;
        if (sun_dot > -0.10 && sun_dot < 0.25) {
            double sunset_t = 1.0 - fabs(sun_dot - 0.05) / 0.20;
            if (sunset_t > 0.0) {
                Vec3 sunset_col = vec3(1.0, 0.45, 0.15);
                glow_col = vec3_lerp(glow_col, sunset_col, sunset_t * 0.6);
            }
        }
        Vec3 atmo_term = vec3_scale(glow_col, limb * day_fac * 2.5);
        out = vec3_add(out, atmo_term);
    }
    return out;
}

static Vec3 trace(const Scene *scene, Ray r, int depth, int max_depth, double time,
                  unsigned seed_key)
{
    if (depth > max_depth) {
        return vec3(0.0, 0.0, 0.0);
    }

    Hit h;
    if (!scene_intersect(scene, r, 1e-4, 1e30, &h)) {
        Vec3 sky_col = sky_sample(r.dir, &scene->sky);
        if (scene->fog.density > 0.0) {
            return fog_apply(&scene->fog, &scene->sky, r, 1e30, sky_col);
        }
        return sky_col;
    }

    Vec3 hit_col = trace_hit(scene, r, depth, max_depth, time, &h, seed_key);
    if (scene->fog.density > 0.0) {
        return fog_apply(&scene->fog, &scene->sky, r, h.t, hit_col);
    }
    return hit_col;
}

/* ------------------------------------------------------------------ */
/* Sampling                                                           */
/* ------------------------------------------------------------------ */

/*
 * Pick the (du, dv) offset within a pixel for sample index `s` of `spp`.
 * When spp is a perfect square we use stratified jittered sampling over a
 * g x g grid; otherwise spp uniformly jittered samples. Offsets are in [0,1).
 */
static void sample_offset(int spp, int s, unsigned px, unsigned py,
                          double *du, double *dv)
{
    int g = (int)(sqrt((double)spp) + 0.5);
    if (g > 0 && g * g == spp) {
        int i = s % g;
        int j = s / g;
        double jx = render_rand01(px, py, (unsigned)(s * 2 + 0));
        double jy = render_rand01(px, py, (unsigned)(s * 2 + 1));
        *du = ((double)i + jx) / (double)g;
        *dv = ((double)j + jy) / (double)g;
    } else {
        *du = render_rand01(px, py, (unsigned)(s * 2 + 0));
        *dv = render_rand01(px, py, (unsigned)(s * 2 + 1));
    }
}

/* Gamma-encode one linear channel and quantize to an 8-bit byte. */
static unsigned char to_byte(double c)
{
    if (c < 0.0) {
        c = 0.0;
    } else if (c > 1.0) {
        c = 1.0;
    }
    double v = pow(c, 1.0 / 2.2) * 255.0;
    int iv = (int)(v + 0.5);
    if (iv < 0) {
        iv = 0;
    } else if (iv > 255) {
        iv = 255;
    }
    return (unsigned char)iv;
}

/*
 * Render the pixel rectangle [x0, x1) x [y0, y1) into rgb_out. Every write
 * lands strictly inside the rectangle, so concurrent calls with disjoint
 * rectangles never race on the output buffer.
 *
 * The colour of a pixel is a pure function of (x, y, sample index) through the
 * hash PRNG below -- it never depends on the thread, tile or schedule that
 * produced it -- so the output is byte-identical for any thread count and any
 * work-splitting strategy.
 *
 * When `progress` is non-zero a compact meter (percentage, elapsed, ETA and
 * rate) is drawn to stderr as rows complete, throttled to ~120 ms. Progress
 * goes ONLY to stderr so stdout (used for the BMP path in main.c) is never
 * polluted, and it never touches the pixel buffer.
 */
static void render_region(const Scene *scene, const Camera *cam, int width,
                          int height, int spp, int max_depth,
                          unsigned char *rgb_out, int x0, int x1, int y0, int y1,
                          double time, int progress)
{
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            Vec3 acc = vec3(0.0, 0.0, 0.0);
            for (int s = 0; s < spp; ++s) {
                double du, dv;
                sample_offset(spp, s, (unsigned)x, (unsigned)y, &du, &dv);
                double uu = ((double)x + du) / (double)width;
                double vv = 1.0 - ((double)y + dv) / (double)height;
                /*
                 * Depth of field: draw the thin-lens sample from PRNG
                 * channels DISTINCT from the AA jitter above, which uses
                 * s*2+0 / s*2+1. Channels s*2+2 / s*2+3 are unused, so the
                 * lens sample never collides with the pixel-offset sample and
                 * stays a pure function of (pixel, sample). With aperture 0
                 * camera_ray_dof() returns exactly camera_ray().
                 */
                double lr1 = render_rand01((unsigned)x, (unsigned)y,
                                           (unsigned)(s * 2 + 2));
                double lr2 = render_rand01((unsigned)x, (unsigned)y,
                                           (unsigned)(s * 2 + 3));
                Ray ray = camera_ray_dof(cam, uu, vv, lr1, lr2);
                /*
                 * Deterministic per-primary-ray seed for sun-disk jitter:
                 * a pure function of (pixel, sample index) so the image is
                 * independent of the thread/tile schedule.
                 */
                unsigned seed_key = render_hash3((unsigned)x, (unsigned)y,
                                                 (unsigned)s);
                acc = vec3_add(acc,
                               trace(scene, ray, 0, max_depth, time, seed_key));
            }
            acc = vec3_scale(acc, 1.0 / (double)spp);
            unsigned char *p = &rgb_out[((size_t)y * (size_t)width + (size_t)x) * 3];
            p[0] = to_byte(acc.x);
            p[1] = to_byte(acc.y);
            p[2] = to_byte(acc.z);
        }
        if (progress) {
            render_progress_update(NULL, (long long)(y + 1 - y0));
        }
    }
}

/*
 * Render the full image on the calling thread only. Used by the non-threaded
 * build; the threaded build drains tiles via render_worker() instead.
 */
#ifndef USE_PTHREADS
static void render_serial(const Scene *scene, const Camera *cam, int width,
                          int height, int spp, int max_depth,
                          unsigned char *rgb_out, double time, int progress)
{
    render_region(scene, cam, width, height, spp, max_depth, rgb_out,
                  0, width, 0, height, time, progress);
}
#endif

/* ------------------------------------------------------------------ */
/* Threading (optional)                                                */
/* ------------------------------------------------------------------ */

#ifdef USE_PTHREADS

#include <stdatomic.h>

/*
 * Upper bound on the worker count. The actual count is derived from the online
 * CPU count (see render_image), so this only guards against absurd values.
 */
#define RENDER_MAX_THREADS 64

/* Edge length of a work tile, in pixels. */
#define RENDER_TILE_SIZE 16

/*
 * Shared, read-only render description plus the dynamic work counter. The
 * atomic tile counter is the single point of synchronisation: workers claim
 * the next tile with atomic_fetch_add and never touch each other's pixels.
 */
typedef struct {
    const Scene   *scene;
    const Camera  *cam;
    int            width;
    int            height;
    int            spp;
    int            max_depth;
    unsigned char *rgb_out;
    double         time;
    int            tiles_x;
    int            tiles_y;
    int            progress;   /* non-zero: draw the stderr meter */
    atomic_int     next_tile;  /* index of the next tile to claim */
    atomic_int     tiles_done; /* tiles finished (thread-safe progress) */
} RenderShared;

static void *render_worker(void *arg)
{
    RenderShared *sh = (RenderShared *)arg;
    const int ntiles = sh->tiles_x * sh->tiles_y;

    for (;;) {
        int tile = atomic_fetch_add(&sh->next_tile, 1);
        if (tile >= ntiles) {
            break; /* all work claimed */
        }

        int ty = tile / sh->tiles_x;
        int tx = tile - ty * sh->tiles_x;

        int x0 = tx * RENDER_TILE_SIZE;
        int y0 = ty * RENDER_TILE_SIZE;
        int x1 = x0 + RENDER_TILE_SIZE;
        int y1 = y0 + RENDER_TILE_SIZE;
        if (x1 > sh->width) {
            x1 = sh->width; /* last column may be a partial tile */
        }
        if (y1 > sh->height) {
            y1 = sh->height; /* last row may be a partial tile */
        }

        render_region(sh->scene, sh->cam, sh->width, sh->height, sh->spp,
                      sh->max_depth, sh->rgb_out, x0, x1, y0, y1, sh->time, 0);

        /*
         * Thread-safe progress: count the finished tile, then let
         * render_progress_update() decide (via its atomic single-writer gate
         * and ~120 ms throttle) whether THIS worker redraws the meter. At most
         * one thread ever writes to stderr, so the line never interleaves.
         */
        if (sh->progress) {
            int done = atomic_fetch_add(&sh->tiles_done, 1) + 1;
            render_progress_update(NULL, done);
        }
    }
    return NULL;
}

/*
 * Pick the worker count: the online CPU count, clamped to [1, RENDER_MAX_THREADS]
 * and never more than the number of tiles (spawning more threads than there is
 * work for is pure overhead). A positive RAYTRACER_THREADS environment variable
 * overrides the CPU count, which is handy for verifying byte-identity across
 * different thread counts.
 */
static int choose_thread_count(int tiles_x, int tiles_y)
{
    int nthreads;
    const char *env = getenv("RAYTRACER_THREADS");

    if (env != NULL && env[0] != '\0') {
        long v = strtol(env, NULL, 10);
        nthreads = (v >= 1 && v <= RENDER_MAX_THREADS) ? (int)v : 1;
    } else {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = (ncpu >= 1) ? (int)ncpu : 4;
        if (nthreads > RENDER_MAX_THREADS) {
            nthreads = RENDER_MAX_THREADS;
        }
    }

    int ntiles = tiles_x * tiles_y;
    if (nthreads > ntiles) {
        nthreads = ntiles; /* never spawn more threads than there is work */
    }
    if (nthreads < 1) {
        nthreads = 1;
    }
    return nthreads;
}

#endif /* USE_PTHREADS */

/* ------------------------------------------------------------------ */
/* Opt-in adaptive sampling                                            */
/* ------------------------------------------------------------------ */

/*
 * Adaptive refinement spends more samples on noisy / high-contrast pixels and
 * fewer on flat ones, while the DEFAULT fixed-spp path stays byte-identical.
 *
 * Metric (per pixel): the RELATIVE STANDARD ERROR OF THE MEAN LUMINANCE
 *
 *     Y_i  = 0.2126*R_i + 0.7152*G_i + 0.0722*B_i   (Rec.709)
 *     Ybar = (1/n) * sum Y_i
 *     s2   = (1/(n-1)) * sum (Y_i - Ybar)^2         (unbiased, n >= 2)
 *     e    = sqrt(s2 / n) / (Ybar + 1e-4)
 *
 * A pixel is refined while e > tau and n < N_max, in progressive fixed-batch
 * passes (batch = n0 = --samples) separated by global barriers.
 *
 * Determinism: sample index s is GLOBAL and monotonic per pixel -- the k-th
 * sample of a pixel always uses index k regardless of pass or thread, and
 * every jitter/seed is a pure function of (x, y, s) via the existing hash
 * PRNG (see adaptive_sample_offset below). Refinement decisions use only the
 * pixel's own running stats. Within a pass, tiles are disjoint; across passes,
 * a barrier orders them. The per-pixel sample loop stays serial so the
 * summation order is fixed. The result is therefore reproducible for any
 * thread count.
 */
typedef struct {
    const Scene   *scene;
    const Camera  *cam;
    int            width;
    int            height;
    int            max_depth;
    int            n0;       /* base spp (== --samples), >= 1   */
    int            nmax;     /* per-pixel sample cap, >= n0     */
    int            batch;    /* additional samples per pass      */
    double         time;
    double        *sumR;     /* per-pixel linear channel sums    */
    double        *sumG;
    double        *sumB;
    double        *sumY;     /* per-pixel luminance sum          */
    double        *sumY2;    /* per-pixel luminance sum-of-squares */
    int           *nsamp;    /* per-pixel sample count           */
    unsigned char *active;   /* 1 if the pixel is refined this pass */
    long long     *rows_done; /* per-pass completed-row counters (progress) */
    int            pass;     /* current pass index (into rows_done)      */
    int            progress; /* non-zero: draw the stderr meter */
} AdaptiveImage;

/*
 * Sample offset for global sample index `s`. For s < n0 this is EXACTLY the
 * fixed path's stratified (or uniform) offset, so the first n0 samples of a
 * pixel match the fixed render. For s >= n0 the base g x g grid is exhausted,
 * so we fall back to uniform jitter (still a pure function of (x, y, s)).
 */
static void adaptive_sample_offset(int n0, int s, unsigned px, unsigned py,
                                   double *du, double *dv)
{
    if (s < n0) {
        sample_offset(n0, s, px, py, du, dv);
    } else {
        *du = render_rand01(px, py, (unsigned)(s * 2 + 0));
        *dv = render_rand01(px, py, (unsigned)(s * 2 + 1));
    }
}

/*
 * Render the pixels of [x0,x1) x [y0,y1) that are flagged active, taking up to
 * `batch` additional samples each (bounded by the per-pixel cap nmax), and
 * fold them into the running accumulators. The per-pixel sample loop is
 * serial and continues the global index from nsamp[p], so summation order is
 * fixed and independent of the tile schedule.
 */
static void adaptive_region(AdaptiveImage *im, int x0, int x1, int y0, int y1)
{
    const int width  = im->width;
    const int height = im->height;
    const int n0     = im->n0;

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            size_t p = (size_t)y * (size_t)width + (size_t)x;
            if (!im->active[p]) {
                continue;
            }

            int n   = im->nsamp[p];
            int cap = im->nmax - n;
            int b   = im->batch;
            if (b > cap) {
                b = cap;
            }
            if (b <= 0) {
                continue; /* already at the cap */
            }

            double sr = 0.0, sg = 0.0, sb = 0.0, sy = 0.0, sy2 = 0.0;
            for (int k = 0; k < b; ++k) {
                int s = n + k;
                double du, dv;
                adaptive_sample_offset(n0, s, (unsigned)x, (unsigned)y, &du, &dv);
                double uu = ((double)x + du) / (double)width;
                double vv = 1.0 - ((double)y + dv) / (double)height;
                double lr1 = render_rand01((unsigned)x, (unsigned)y,
                                           (unsigned)(s * 2 + 2));
                double lr2 = render_rand01((unsigned)x, (unsigned)y,
                                           (unsigned)(s * 2 + 3));
                Ray ray = camera_ray_dof(im->cam, uu, vv, lr1, lr2);
                unsigned seed_key = render_hash3((unsigned)x, (unsigned)y,
                                                 (unsigned)s);
                Vec3 c = trace(im->scene, ray, 0, im->max_depth, im->time,
                               seed_key);
                sr += c.x;
                sg += c.y;
                sb += c.z;
                double Y = 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
                sy  += Y;
                sy2 += Y * Y;
            }

            im->sumR[p]  += sr;
            im->sumG[p]  += sg;
            im->sumB[p]  += sb;
            im->sumY[p]  += sy;
            im->sumY2[p] += sy2;
            im->nsamp[p]  = n + b;
        }
        if (im->progress && x0 == 0) {
            /* Count each row once (only the leftmost tile column) so the
             * per-pass total is exactly `height` in both builds. Overall
             * completion spans all passes: pass * height + rows done. */
            long long done = __atomic_add_fetch(&im->rows_done[im->pass], 1,
                                                __ATOMIC_RELAXED);
            render_progress_update(NULL,
                                   (long long)im->pass * (long long)height + done);
        }
    }
}

#ifdef USE_PTHREADS
/* Per-pass work description: the adaptive image plus a tile counter. */
typedef struct {
    AdaptiveImage *im;
    int            tiles_x;
    atomic_int     next_tile;
} AdaptivePassShared;

static void *adaptive_worker(void *arg)
{
    AdaptivePassShared *ps = (AdaptivePassShared *)arg;
    AdaptiveImage *im = ps->im;
    const int tiles_y = (im->height + RENDER_TILE_SIZE - 1) / RENDER_TILE_SIZE;
    const int ntiles  = ps->tiles_x * tiles_y;

    for (;;) {
        int tile = atomic_fetch_add(&ps->next_tile, 1);
        if (tile >= ntiles) {
            break;
        }
        int ty = tile / ps->tiles_x;
        int tx = tile - ty * ps->tiles_x;
        int x0 = tx * RENDER_TILE_SIZE;
        int y0 = ty * RENDER_TILE_SIZE;
        int x1 = x0 + RENDER_TILE_SIZE;
        int y1 = y0 + RENDER_TILE_SIZE;
        if (x1 > im->width)  { x1 = im->width; }
        if (y1 > im->height) { y1 = im->height; }
        adaptive_region(im, x0, x1, y0, y1);
    }
    return NULL;
}
#endif /* USE_PTHREADS */

/*
 * Run one refinement pass over the whole image, joining all workers before
 * returning. That join IS the global barrier: the active set for the next pass
 * is a deterministic function of the finished pass. `pass` is the index of the
 * pass being run; it selects the per-pass progress row counter.
 */
static void adaptive_run_pass(AdaptiveImage *im, int pass)
{
    im->pass = pass;
    if (im->progress && im->rows_done != NULL) {
        __atomic_store_n(&im->rows_done[pass], 0, __ATOMIC_RELAXED);
    }
#ifdef USE_PTHREADS
    int tiles_x = (im->width + RENDER_TILE_SIZE - 1) / RENDER_TILE_SIZE;
    int tiles_y = (im->height + RENDER_TILE_SIZE - 1) / RENDER_TILE_SIZE;
    int nthreads = choose_thread_count(tiles_x, tiles_y);

    AdaptivePassShared ps;
    ps.im = im;
    ps.tiles_x = tiles_x;
    atomic_init(&ps.next_tile, 0);

    if (nthreads <= 1) {
        adaptive_worker(&ps);
        return;
    }

    pthread_t *tids = (pthread_t *)malloc((size_t)nthreads * sizeof(pthread_t));
    if (tids == NULL) {
        adaptive_worker(&ps);
        return;
    }

    int started = 0;
    for (int i = 0; i < nthreads; ++i) {
        if (pthread_create(&tids[i], NULL, adaptive_worker, &ps) == 0) {
            started++;
        } else {
            adaptive_worker(&ps); /* drain remaining tiles inline */
            break;
        }
    }
    for (int i = 0; i < started; ++i) {
        pthread_join(tids[i], NULL);
    }
    free(tids);
#else
    adaptive_region(im, 0, im->width, 0, im->height);
#endif
}

/*
 * Recompute the active set from each pixel's OWN running statistics and return
 * the number of pixels still flagged for refinement. Pure per-pixel; no
 * neighbourhood dependency, so the decision is schedule-independent.
 */
static int adaptive_compute_active(AdaptiveImage *im, double tau, int nmax)
{
    size_t npixels = (size_t)im->width * (size_t)im->height;
    int count = 0;

    for (size_t p = 0; p < npixels; ++p) {
        int n = im->nsamp[p];
        int flag = 0;

        if (n < nmax) {
            if (n < 2) {
                /* Cannot estimate variance from a single sample: refine. */
                flag = 1;
            } else {
                double nf   = (double)n;
                double ybar = im->sumY[p] / nf;
                double var  = (im->sumY2[p] - im->sumY[p] * ybar) / (nf - 1.0);
                if (var < 0.0) {
                    var = 0.0; /* guard against tiny negative round-off */
                }
                double se = sqrt(var / nf);
                double e  = se / (ybar + 1e-4);
                if (e > tau) {
                    flag = 1;
                }
            }
        }

        im->active[p] = (unsigned char)flag;
        count += flag;
    }
    return count;
}

/* Average the accumulated samples, then gamma-encode + quantize once. */
static void adaptive_resolve(const AdaptiveImage *im, unsigned char *rgb_out)
{
    size_t npixels = (size_t)im->width * (size_t)im->height;
    for (size_t p = 0; p < npixels; ++p) {
        int n = im->nsamp[p];
        double inv = 1.0 / (double)n;
        rgb_out[p * 3 + 0] = to_byte(im->sumR[p] * inv);
        rgb_out[p * 3 + 1] = to_byte(im->sumG[p] * inv);
        rgb_out[p * 3 + 2] = to_byte(im->sumB[p] * inv);
    }
}

/*
 * Adaptive driver: pass 0 renders every pixel with n0 samples; then, while any
 * pixel still exceeds the relative-error tolerance and is below the cap, run
 * another fixed batch. Finally resolve to bytes.
 */
static int render_image_adaptive(const Scene *scene, const Camera *cam,
                                 int width, int height, int n0, int max_depth,
                                 int nmax, double tau, unsigned char *rgb_out)
{
    if (nmax < n0) {
        nmax = n0;
    }
    if (!(tau > 0.0)) {
        tau = 0.02;
    }

    size_t npixels = (size_t)width * (size_t)height;

    AdaptiveImage im;
    im.scene = scene;
    im.cam = cam;
    im.width = width;
    im.height = height;
    im.max_depth = max_depth;
    im.n0 = n0;
    im.nmax = nmax;
    im.batch = n0;
    im.time = 0.0;
    im.sumR  = (double *)calloc(npixels, sizeof(double));
    im.sumG  = (double *)calloc(npixels, sizeof(double));
    im.sumB  = (double *)calloc(npixels, sizeof(double));
    im.sumY  = (double *)calloc(npixels, sizeof(double));
    im.sumY2 = (double *)calloc(npixels, sizeof(double));
    im.nsamp = (int *)calloc(npixels, sizeof(int));
    im.active = (unsigned char *)calloc(npixels, sizeof(unsigned char));
    im.rows_done = NULL;
    im.pass = 0;
    im.progress = 0;

    if (im.sumR == NULL || im.sumG == NULL || im.sumB == NULL ||
        im.sumY == NULL || im.sumY2 == NULL || im.nsamp == NULL ||
        im.active == NULL) {
        free(im.sumR); free(im.sumG); free(im.sumB);
        free(im.sumY); free(im.sumY2); free(im.nsamp); free(im.active);
        return 4;
    }

    /*
     * Progress for the adaptive path. A refinement pass is always run at least
     * once (even when every pixel is inactive), so the number of passes is
     * bounded by 1 + ceil((nmax - n0) / batch) = 1 + (nmax - n0) / n0. Each
     * pass touches exactly `height` rows (see adaptive_region), so the overall
     * meter is `pass * height + rows done in this pass` out of
     * `max_passes * height`. This is a documented, deterministic UPPER BOUND:
     * refinement may converge early, in which case the final 100% line is
     * emitted at the end of the render (render_progress_finish).
     */
    int max_passes = 1;
    if (nmax > n0 && n0 > 0) {
        max_passes += (nmax - n0 + n0 - 1) / n0;
    }
    im.progress = render_progress_enabled();
    if (im.progress) {
        im.rows_done = (long long *)calloc((size_t)max_passes, sizeof(long long));
        if (im.rows_done == NULL) {
            im.progress = 0; /* progress is optional: never fail the render */
        }
    }

    RenderProgress pr;
    pr.total = 0;
    pr.total_expected = (long long)max_passes * (long long)height;
    pr.pixels_total = (long long)npixels;
    render_progress_begin(&pr);

    double t0 = render_now_seconds();

    /* Pass 0: every pixel, samples s = 0 .. n0-1. */
    for (size_t p = 0; p < npixels; ++p) {
        im.active[p] = 1;
    }
    adaptive_run_pass(&im, 0);

    /* Refinement passes: batch n0 for every flagged pixel, global barriers. */
    int pass = 1;
    for (;;) {
        int remaining = adaptive_compute_active(&im, tau, nmax);
        if (remaining == 0) {
            break;
        }
        if (pass >= max_passes) {
            break; /* defensive: never index past rows_done[] */
        }
        adaptive_run_pass(&im, pass);
        pass++;
    }

    render_progress_finish(&pr);

    adaptive_resolve(&im, rgb_out);

    /* Diagnostics: total and maximum per-pixel sample counts. */
    unsigned long long total = 0;
    int maxn = 0;
    for (size_t p = 0; p < npixels; ++p) {
        total += (unsigned long long)im.nsamp[p];
        if (im.nsamp[p] > maxn) {
            maxn = im.nsamp[p];
        }
    }
    g_render_last_total_samples = total;
    g_render_last_max_samples = maxn;

    g_render_last_seconds = render_now_seconds() - t0;

    free(im.sumR); free(im.sumG); free(im.sumB);
    free(im.sumY); free(im.sumY2); free(im.nsamp); free(im.active);
    free(im.rows_done);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

double render_last_seconds(void)
{
    return g_render_last_seconds;
}

unsigned long long render_last_total_samples(void)
{
    return g_render_last_total_samples;
}

int render_last_max_samples(void)
{
    return g_render_last_max_samples;
}

int render_image(const Scene *scene, const Camera *cam, int width, int height,
                 int samples_per_pixel, int max_depth, unsigned char *rgb_out)
{
    if (scene == NULL || cam == NULL || rgb_out == NULL) {
        return 1;
    }
    if (width <= 0 || height <= 0) {
        return 2;
    }
    if (samples_per_pixel < 1 || max_depth < 0) {
        return 3;
    }

    /* Time is fixed at 0 for a deterministic static frame. */
    const double time = 0.0;

    /* Diagnostics for the fixed path (mirrors the adaptive counters). */
    g_render_last_total_samples =
        (unsigned long long)width * (unsigned long long)height *
        (unsigned long long)samples_per_pixel;
    g_render_last_max_samples = samples_per_pixel;

    /* Progress is stderr-only and easily silenced via RAYTRACER_NO_PROGRESS. */
    int progress = render_progress_enabled();

#ifdef USE_PTHREADS
    /* Tile grid covering the image (tiles may be partial at the edges). */
    int tiles_x = (width + RENDER_TILE_SIZE - 1) / RENDER_TILE_SIZE;
    int tiles_y = (height + RENDER_TILE_SIZE - 1) / RENDER_TILE_SIZE;
    int nthreads = choose_thread_count(tiles_x, tiles_y);

    RenderShared shared;
    shared.scene = scene;
    shared.cam = cam;
    shared.width = width;
    shared.height = height;
    shared.spp = samples_per_pixel;
    shared.max_depth = max_depth;
    shared.rgb_out = rgb_out;
    shared.time = time;
    shared.tiles_x = tiles_x;
    shared.tiles_y = tiles_y;
    shared.progress = progress;
    atomic_init(&shared.next_tile, 0);
    atomic_init(&shared.tiles_done, 0);

    /* Whole render == tiles_x * tiles_y tiles; the pixel budget drives Mpx/s. */
    RenderProgress pr;
    pr.total = 0;
    pr.total_expected = (long long)tiles_x * (long long)tiles_y;
    pr.pixels_total = (long long)width * (long long)height;
    render_progress_begin(&pr);

    double t0 = render_now_seconds();

    if (nthreads <= 1) {
        /* Trivial case: no point paying for thread creation. */
        render_worker(&shared);
    } else {
        pthread_t *tids = (pthread_t *)malloc((size_t)nthreads * sizeof(pthread_t));
        if (tids == NULL) {
            /* Allocation failure: render everything on the calling thread. */
            render_worker(&shared);
        } else {
            int started = 0;
            for (int i = 0; i < nthreads; ++i) {
                if (pthread_create(&tids[i], NULL, render_worker, &shared) == 0) {
                    started++;
                } else {
                    /* Creation failed: run on the calling thread so no tiles
                     * are left unrendered, then stop spawning more. */
                    render_worker(&shared);
                    break;
                }
            }
            /* If every create failed, `started` is 0 and the loop above
             * already drained all tiles inline. */
            for (int i = 0; i < started; ++i) {
                pthread_join(tids[i], NULL);
            }
            free(tids);
        }
    }

    render_progress_finish(&pr);

    g_render_last_seconds = render_now_seconds() - t0;
#else
    RenderProgress pr;
    pr.total = 0;
    pr.total_expected = height;
    pr.pixels_total = (long long)width * (long long)height;
    render_progress_begin(&pr);

    double t0 = render_now_seconds();
    render_serial(scene, cam, width, height, samples_per_pixel, max_depth,
                  rgb_out, time, progress);

    render_progress_finish(&pr);
    g_render_last_seconds = render_now_seconds() - t0;
#endif

    return 0;
}

int render_image_ex(const Scene *scene, const Camera *cam, int width, int height,
                    const RenderParams *params, unsigned char *rgb_out)
{
    if (params == NULL) {
        return 1;
    }
    if (scene == NULL || cam == NULL || rgb_out == NULL) {
        return 1;
    }
    if (width <= 0 || height <= 0) {
        return 2;
    }
    if (params->samples_per_pixel < 1 || params->max_depth < 0) {
        return 3;
    }

    /*
     * GATING: with adaptive OFF this is the existing fixed path VERBATIM
     * (delegated to render_image), so the output is byte-identical to today
     * for any thread count and any --samples.
     */
    if (!params->adaptive) {
        return render_image(scene, cam, width, height, params->samples_per_pixel,
                            params->max_depth, rgb_out);
    }

    int n0 = params->samples_per_pixel;
    int nmax = params->adaptive_max_spp;
    if (nmax <= 0) {
        nmax = 4 * n0; /* default cap: N_max = 4 * n0 */
    }
    double tau = params->adaptive_tau;
    if (!(tau > 0.0)) {
        tau = 0.02; /* default tolerance */
    }

    return render_image_adaptive(scene, cam, width, height, n0, params->max_depth,
                                 nmax, tau, rgb_out);
}
