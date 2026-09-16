/*
 * tests/test_render_threads.c - Regression test for renderer determinism and
 * single-threaded vs threaded output identity (src/render.c).
 *
 * The threaded renderer was rewritten to use a dynamic thread count
 * (sysconf(_SC_NPROCESSORS_ONLN), capped at 64, fallback 4), 16x16 tiles with
 * an atomic work counter, and an optional RAYTRACER_THREADS env override.
 * Whatever the schedule, the resulting image MUST be byte-identical to the
 * single-threaded image and stable across repeated calls.
 *
 * Strategy (works in BOTH build modes):
 *   - render_image() is called twice with identical inputs and the two
 *     buffers are compared byte-for-byte (determinism).
 *   - When compiled with -DUSE_PTHREADS the test additionally forces a
 *     single worker via the documented RAYTRACER_THREADS=1 override and
 *     compares that buffer against the multi-threaded one: this is a direct
 *     threaded-vs-single-threaded identity check. Without USE_PTHREADS the
 *     renderer is already single-threaded and the determinism check is the
 *     whole story (the test still passes).
 *
 * Kept deliberately small (96x54, 4 spp, depth 4) so `make test` stays fast.
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own main() and returns non-zero
 * on any failure. C11, -Wall -Wextra clean. No rand(). All heap memory freed.
 */

/* Feature-test macro so setenv()/unsetenv() are declared under -std=c11. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"
#include "vec3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test harness (same style as the existing tests)                     */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } } while (0)

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define WIDTH   96
#define HEIGHT  54
#define SAMPLES 4
#define DEPTH   4

#define BUF_BYTES ((size_t)WIDTH * (size_t)HEIGHT * 3u)

/* Count differing bytes between two buffers. */
static size_t count_diff(const unsigned char *a, const unsigned char *b,
                         size_t n)
{
    size_t d = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) ++d;
    }
    return d;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Silence the renderer's stderr progress line for clean test output. */
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    Scene scene;
    SceneDesc desc;
    memset(&scene, 0, sizeof scene);
    scene_desc_init(&desc);
    scene_default_desc(&desc);
    if (scene_build_from_desc(&scene, &desc) != 0) {
        fprintf(stderr, "FAIL: scene_build_from_desc failed\n");
        scene_desc_free(&desc);
        printf("tests/test_render_threads: 0 passed, 1 failed\n");
        return 1;
    }
    scene_desc_free(&desc);

    Camera cam = scene_default_camera(&scene);

    unsigned char *buf1 = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *buf2 = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *buf1t = (unsigned char *)malloc(BUF_BYTES);
    if (!buf1 || !buf2 || !buf1t) {
        fprintf(stderr, "FAIL: allocation failed\n");
        free(buf1); free(buf2); free(buf1t);
        scene_free(&scene);
        printf("tests/test_render_threads: 0 passed, 1 failed\n");
        return 1;
    }

    memset(buf1, 0xAA, BUF_BYTES);
    memset(buf2, 0x55, BUF_BYTES);
    memset(buf1t, 0x00, BUF_BYTES);

    /* --- Call 1: default configuration -------------------------------- */
    int rc1 = render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, buf1);
    CHECK(rc1 == 0, "render_image call 1 returns 0");

    /* --- Call 2: identical inputs -> must be byte-identical ----------- */
    int rc2 = render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, buf2);
    CHECK(rc2 == 0, "render_image call 2 returns 0");

    size_t diff12 = count_diff(buf1, buf2, BUF_BYTES);
    CHECK(diff12 == 0, "two identical render calls are byte-identical");

    /* The image must actually contain rendered content (guard against a
     * trivially-empty buffer making the identity check vacuous). */
    int nonblack = 0;
    for (size_t i = 0; i + 2 < BUF_BYTES; i += 3) {
        if (buf1[i] || buf1[i + 1] || buf1[i + 2]) { ++nonblack; break; }
    }
    CHECK(nonblack, "rendered buffer is not all-black");

#ifdef USE_PTHREADS
    /* --- Threaded build: force a single worker and compare ------------ */
    int have_env = (setenv("RAYTRACER_THREADS", "1", 1) == 0);
    if (have_env) {
        int rc3 = render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, buf1t);
        CHECK(rc3 == 0, "render_image (RAYTRACER_THREADS=1) returns 0");

        size_t diff_1t = count_diff(buf1, buf1t, BUF_BYTES);
        CHECK(diff_1t == 0,
              "threaded output == single-threaded output (byte-identical)");
        if (diff_1t != 0) {
            fprintf(stderr, "  threaded vs single: %zu differing bytes\n", diff_1t);
        }

        /* Restore the default (dynamic) thread count and re-render: this
         * exercises a different schedule and must still match. */
        (void)unsetenv("RAYTRACER_THREADS");
        int rc4 = render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, buf1t);
        CHECK(rc4 == 0, "render_image (dynamic threads) returns 0");
        size_t diff_dyn = count_diff(buf1, buf1t, BUF_BYTES);
        CHECK(diff_dyn == 0, "dynamic-thread re-render matches the first render");

        printf("tests/test_render_threads: threaded build; RAYTRACER_THREADS=1 "
               "vs dynamic: %zu differing bytes\n", diff_1t);
    } else {
        fprintf(stderr, "  note: setenv failed; skipped explicit thread-count "
                        "comparison\n");
    }
#else
    printf("tests/test_render_threads: single-threaded build; determinism across "
           "two calls verified\n");
#endif

    free(buf1);
    free(buf2);
    free(buf1t);
    scene_free(&scene);

    printf("tests/test_render_threads: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
