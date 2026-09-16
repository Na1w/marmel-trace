/*
 * tests/test_progress.c - Regression test for the render progress indicator
 * (src/render.c / src/render.h).
 *
 * The renderer draws a single self-overwriting meter on stderr, e.g.
 *
 *     render:  42% [00:12<00:16, 1.2 Mpx/s]
 *
 * and silences it when RAYTRACER_NO_PROGRESS is set. Progress is strictly
 * diagnostic: it must NEVER touch the pixel buffer, so the rendered bytes are
 * IDENTICAL with and without it (and for any thread count).
 *
 * This test verifies all four properties in-process by temporarily redirecting
 * fd 1 (stdout) and fd 2 (stderr) to tmpfile()s via dup2(), rendering a tiny
 * scene, then reading the captured bytes back:
 *
 *   (a) RAYTRACER_NO_PROGRESS=1  =>  no progress text on stderr;
 *   (b) with progress enabled    =>  the meter is on stderr, and NOTHING is
 *                                    written to stdout;
 *   (c) the meter reaches 100% on completion (a completed render always
 *       reports completion);
 *   (d) the image bytes are byte-identical with progress on and off.
 *
 * RAYTRACER_THREADS is pinned to 4 so that, in the -DUSE_PTHREADS build, the
 * threaded tile counter / single-writer throttle path is actually exercised
 * (it is ignored by the single-threaded build).
 *
 * Kept deliberately tiny (64x36, 1 spp, depth 2) so `make test` stays fast.
 * The Makefile links every test source in tests/ against all project objects
 * except
 * src/main.o, so this file supplies its own main(). C11, -Wall -Wextra clean.
 * No rand(); all heap memory freed.
 */

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
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Test harness (same style as the existing tests)                     */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } } while (0)

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define WIDTH   64
#define HEIGHT  36
#define SAMPLES 1
#define DEPTH   2

#define BUF_BYTES ((size_t)WIDTH * (size_t)HEIGHT * 3u)

/* ------------------------------------------------------------------ */
/* Stream capture (dup2 tmpfiles over fd 1 / fd 2)                     */
/* ------------------------------------------------------------------ */

static FILE *g_cap_out = NULL; /* captured stdout */
static FILE *g_cap_err = NULL; /* captured stderr */
static int   g_saved_out = -1;
static int   g_saved_err = -1;

/* Read an entire stream into a fresh NUL-terminated heap string. */
static char *read_all(FILE *f, size_t *out_len)
{
    size_t cap = 4096, len = 0;
    char  *buf = (char *)malloc(cap + 1);
    if (buf == NULL) {
        if (out_len) { *out_len = 0; }
        return NULL;
    }
    if (f != NULL) {
        rewind(f);
        for (;;) {
            size_t n = fread(buf + len, 1, cap - len, f);
            len += n;
            if (n == 0) { break; }
            if (len == cap) {
                char *nb = (char *)realloc(buf, cap * 2 + 1);
                if (nb == NULL) { break; }
                buf = nb;
                cap *= 2;
            }
        }
    }
    buf[len] = '\0';
    if (out_len) { *out_len = len; }
    return buf;
}

/* Redirect stdout+stderr into fresh tmpfiles. Returns 0 on success. */
static int capture_begin(void)
{
    fflush(stdout);
    fflush(stderr);

    g_cap_out = tmpfile();
    g_cap_err = tmpfile();
    if (g_cap_out == NULL || g_cap_err == NULL) {
        if (g_cap_out) { fclose(g_cap_out); g_cap_out = NULL; }
        if (g_cap_err) { fclose(g_cap_err); g_cap_err = NULL; }
        return -1;
    }

    g_saved_out = dup(STDOUT_FILENO);
    g_saved_err = dup(STDERR_FILENO);
    if (g_saved_out < 0 || g_saved_err < 0 ||
        dup2(fileno(g_cap_out), STDOUT_FILENO) < 0 ||
        dup2(fileno(g_cap_err), STDERR_FILENO) < 0) {
        return -1;
    }
    return 0;
}

/* Restore the original streams and hand back both captured strings. */
static void capture_end(char **out_buf, size_t *out_len,
                        char **err_buf, size_t *err_len)
{
    fflush(stdout);
    fflush(stderr);

    if (g_saved_out >= 0) { dup2(g_saved_out, STDOUT_FILENO); close(g_saved_out); }
    if (g_saved_err >= 0) { dup2(g_saved_err, STDERR_FILENO); close(g_saved_err); }
    g_saved_out = -1;
    g_saved_err = -1;

    *out_buf = read_all(g_cap_out, out_len);
    *err_buf = read_all(g_cap_err, err_len);

    if (g_cap_out != NULL) { fclose(g_cap_out); g_cap_out = NULL; }
    if (g_cap_err != NULL) { fclose(g_cap_err); g_cap_err = NULL; }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    Scene scene;
    SceneDesc desc;
    memset(&scene, 0, sizeof scene);
    scene_desc_init(&desc);
    scene_default_desc(&desc);
    if (scene_build_from_desc(&scene, &desc) != 0) {
        fprintf(stderr, "FAIL: scene_build_from_desc failed\n");
        scene_desc_free(&desc);
        printf("tests/test_progress: 0 passed, 1 failed\n");
        return 1;
    }
    scene_desc_free(&desc);

    Camera cam = scene_default_camera(&scene);

    unsigned char *buf_prog = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *buf_sil  = (unsigned char *)malloc(BUF_BYTES);
    if (buf_prog == NULL || buf_sil == NULL) {
        fprintf(stderr, "FAIL: allocation failed\n");
        free(buf_prog); free(buf_sil);
        scene_free(&scene);
        printf("tests/test_progress: 0 passed, 1 failed\n");
        return 1;
    }

    /* Force a multi-worker schedule where it is honoured (threaded build). */
    (void)setenv("RAYTRACER_THREADS", "4", 1);

    /* --- Render #1: progress ENABLED (default). --------------------- */
    (void)unsetenv("RAYTRACER_NO_PROGRESS");
    CHECK(render_progress_enabled() != 0,
          "progress should be enabled by default");

    char  *out_prog = NULL, *err_prog = NULL;
    size_t out_prog_len = 0, err_prog_len = 0;
    int    cap_ok = (capture_begin() == 0);
    int    rc = -1;
    if (cap_ok) {
        rc = render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, buf_prog);
    }
    capture_end(&out_prog, &out_prog_len, &err_prog, &err_prog_len);

    CHECK(cap_ok, "stream capture could not be set up");
    CHECK(rc == 0, "render with progress failed");

    /* (b) progress text must be on stderr... */
    CHECK(err_prog_len > 0, "expected progress output on stderr");
    CHECK(err_prog != NULL && strstr(err_prog, "render:") != NULL,
          "stderr should contain a 'render:' progress line");
    CHECK(err_prog != NULL && strstr(err_prog, "%") != NULL,
          "progress line should contain a percentage");
    CHECK(err_prog != NULL && (strstr(err_prog, "Mpx/s") != NULL ||
                               strstr(err_prog, "kpx/s") != NULL ||
                               strstr(err_prog, "px/s") != NULL),
          "progress line should contain a rate (px/s)");
    CHECK(err_prog != NULL && strchr(err_prog, '\r') != NULL,
          "progress line should use carriage returns to redraw");

    /* (c) the meter must report completion (100%). */
    CHECK(err_prog != NULL && strstr(err_prog, "100%") != NULL,
          "progress meter should reach 100%");

    /* (b) ...and NOTHING on stdout. render_image() writes only to stderr, so
     * any byte on stdout would mean the meter leaked onto the wrong stream. */
    CHECK(out_prog_len == 0, "progress must not write to stdout");
    CHECK(out_prog == NULL || strstr(out_prog, "render:") == NULL,
          "stdout must not contain the progress meter");

    /* --- Render #2: progress SILENCED. ----------------------------- */
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);
    CHECK(render_progress_enabled() == 0,
          "RAYTRACER_NO_PROGRESS should disable progress");

    char  *out_sil = NULL, *err_sil = NULL;
    size_t out_sil_len = 0, err_sil_len = 0;
    cap_ok = (capture_begin() == 0);
    rc = -1;
    if (cap_ok) {
        rc = render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, buf_sil);
    }
    capture_end(&out_sil, &out_sil_len, &err_sil, &err_sil_len);

    CHECK(cap_ok, "stream capture (silenced) could not be set up");
    CHECK(rc == 0, "silenced render failed");
    /* (a) no progress text at all when silenced. */
    CHECK(err_sil_len == 0, "RAYTRACER_NO_PROGRESS must emit no stderr output");
    CHECK(err_sil == NULL || strstr(err_sil, "render:") == NULL,
          "silenced run must not contain a progress line");
    CHECK(out_sil_len == 0, "silenced run must emit nothing on stdout");

    /* (d) byte-identical pixels with and without progress. */
    CHECK(memcmp(buf_prog, buf_sil, BUF_BYTES) == 0,
          "rendered bytes must be identical with and without progress");

    /* Determinism sanity: repeat the silenced render and compare. */
    (void)render_image(&scene, &cam, WIDTH, HEIGHT, SAMPLES, DEPTH, buf_prog);
    CHECK(memcmp(buf_prog, buf_sil, BUF_BYTES) == 0,
          "repeated renders must be deterministic");

    /* --- Cleanup --------------------------------------------------- */
    (void)unsetenv("RAYTRACER_NO_PROGRESS");
    (void)unsetenv("RAYTRACER_THREADS");
    free(out_prog); free(err_prog);
    free(out_sil);  free(err_sil);
    free(buf_prog); free(buf_sil);
    scene_free(&scene);

    printf("tests/test_progress: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
