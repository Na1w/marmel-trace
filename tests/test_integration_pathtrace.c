/*
 * tests/test_integration_pathtrace.c - End-to-end INTEGRATION test for the
 * unbiased path tracer (src/pathtrace.{h,c} pathtrace_render), which is the
 * DEFAULT render mode of src/main.c (opt out with --no-pathtrace).
 *
 * Unlike tests/test_pathtrace_sampling.c (which exercises the estimator
 * primitives), this test drives the FULL production pipeline that src/main.c
 * uses:
 *
 *   scene_default_desc() / scene_desc_load_string() -> scene_build_from_desc()
 *       -> camera built from the parsed `camera { }` block (camera_create with
 *          the real output aspect ratio)
 *       -> pathtrace_render() / render_image_ex() with the same scene+camera
 *       -> inspect / compare the RGB byte buffers
 *
 * Contract locked by this test (NEW CLI semantics):
 *
 *   (1) RENDERER DISTINCTION (library level): at the SAME scene, camera, spp
 *       and depth, pathtrace_render() and the Whitted render_image_ex()
 *       produce DIFFERENT byte buffers (the two renderers are genuinely
 *       distinct, not a no-op alias).
 *   (2) DEFAULT == --pathtrace: the no-flag CLI run IS the path tracer, so the
 *       DEFAULT render and `--pathtrace` are BYTE-IDENTICAL (and reproduce
 *       themselves across re-runs). `--no-adaptive` and
 *       `--no-threads`/`--single-threaded` are ALSO byte-identical to the
 *       default: they change the sampling strategy / schedule, never the
 *       pixels of the (deterministic) path tracer.
 *   (3) LEGACY BASELINE: `--no-pathtrace --no-adaptive` reproduces the pristine
 *       fixed-spp Whitted render, pinned to the SHA-256
 *       f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb at
 *       320x180 / 4 spp / depth 4. Because adaptive sampling is ON by default,
 *       `--no-pathtrace` ALONE runs Whitted + adaptive and yields a DIFFERENT
 *       image (the pristine baseline only reappears with `--no-adaptive` too).
 *   (4) DEFAULT PATH STABILITY (library level): render_image_ex(adaptive=0) and
 *       the plain render_image() are byte-identical across repeats and to each
 *       other, and STILL byte-identical after a path-trace render has run (the
 *       additive module leaves no global state behind).
 *   (5) DETERMINISM: two pathtrace_render() calls are byte-identical, and in a
 *       -DUSE_PTHREADS build RAYTRACER_THREADS=1 vs =4 are byte-identical.
 *   (6) CLI END-TO-END: the built ./raytracer accepts --pathtrace,
 *       --no-pathtrace, --no-adaptive, --no-threads and --single-threaded (all
 *       exit 0) and the byte relations above hold on the produced BMPs.
 *       Skipped (not failed) when the binary has not been built, so a
 *       standalone `make test` stays green.
 *
 * The Makefile links every C source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own int main(void). C11,
 * -Wall -Wextra clean, no rand(). Kept small (160x90, 4 spp, depth 4) so
 * `make test` stays fast (well under a second of rendering).
 */

/* Feature-test macro so setenv()/unsetenv()/getpid()/stat() are declared. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"
#include "pathtrace.h"
#include "vec3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness (same convention as the other integration tests) */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define WIDTH    160
#define HEIGHT   90
#define DEPTH    4
#define SPP      4                 /* --samples (== spp for both modes) */

#define BUF_BYTES ((size_t)WIDTH * (size_t)HEIGHT * 3u)

/*
 * The pristine fixed-spp Whitted baseline. Since adaptive sampling is ON by
 * default, this is reachable ONLY via `--no-pathtrace --no-adaptive`. The
 * digest is the SHA-256 of the BMP produced at the resolution below (a fixed
 * 320x180, 4 spp, depth 4 frame, exactly as the legacy CLI invocation).
 */
#define LEGACY_WIDTH   320
#define LEGACY_HEIGHT  180
#define LEGACY_SPP     4
#define LEGACY_DEPTH   4
#define LEGACY_SHA256  "f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb"


/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Build the BUILT-IN default scene + the camera src/main.c would use: the
 * parsed `camera { }` block with the real output aspect ratio, aperture /
 * focus_distance carried over. Returns 0 on success.
 */
static int build_default_scene(Scene *scene, Camera *cam)
{
    SceneDesc desc;

    scene_desc_init(&desc);
    scene_default_desc(&desc);
    if (desc.material_count == 0) {
        scene_desc_free(&desc);
        return -1;
    }

    memset(scene, 0, sizeof *scene);
    if (scene_build_from_desc(scene, &desc) != 0) {
        scene_desc_free(&desc);
        return -1;
    }

    {
        const CameraDesc *cd = &desc.camera;
        if (cd->present) {
            *cam = camera_create(cd->eye, cd->target, cd->up, cd->vfov_deg,
                                 (double)WIDTH / (double)HEIGHT);
            cam->aperture = cd->aperture;
            if (cd->focus_distance > CAMERA_FOCUS_DISTANCE_DERIVED)
                cam->focus_distance = cd->focus_distance;
        } else {
            *cam = scene_default_camera(scene);
        }
    }
    scene_desc_free(&desc);
    return 0;
}

/* Number of differing bytes between two equal-sized RGB buffers. */
static size_t count_diff(const unsigned char *a, const unsigned char *b)
{
    size_t d = 0;
    for (size_t i = 0; i < BUF_BYTES; ++i) {
        if (a[i] != b[i]) ++d;
    }
    return d;
}

/* Build a RenderParams the way src/main.c does (0 == "use the default"). */
static void make_params(RenderParams *p, int adaptive)
{
    p->samples_per_pixel = SPP;
    p->max_depth         = DEPTH;
    p->adaptive          = adaptive;
    p->adaptive_max_spp  = 0;   /* 0 => engine default 4*n0 */
    p->adaptive_tau      = 0.0; /* 0 => engine default 0.02 */
}

/* File size in bytes, or -1 if it does not exist. */
static long file_size(const char *path)
{
    struct stat st;
    if (path == NULL || stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

/* Read a whole file into a malloc'd buffer; NULL on failure. */
static unsigned char *read_file(const char *path, size_t *out_len)
{
    FILE          *f;
    unsigned char *buf;
    long           n;
    size_t         got;

    if (path == NULL || out_len == NULL) return NULL;

    f = fopen(path, "rb");
    if (f == NULL) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);

    buf = (unsigned char *)malloc((size_t)n + 1u);
    if (buf == NULL) { fclose(f); return NULL; }

    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }

    *out_len = (size_t)n;
    return buf;
}

/* 1 when the two files exist and are byte-identical, 0 otherwise. */
static int files_equal(const char *a, const char *b)
{
    size_t         la = 0, lb = 0;
    unsigned char *ba = read_file(a, &la);
    unsigned char *bb = read_file(b, &lb);
    int            eq = (ba != NULL && bb != NULL && la == lb &&
                         (la == 0u || memcmp(ba, bb, la) == 0));

    free(ba);
    free(bb);
    return eq;
}

/* Run a shell command; return the process exit status, or <0 on failure. */
static int run_cmd(const char *cmd)
{
    int status = system(cmd);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -2;
}

/* ------------------------------------------------------------------ */
/* Minimal SHA-256 (FIPS 180-4), self-contained so the test suite stays */
/* dependency-free (no OpenSSL / no shelling out to shasum).           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t      state[8];
    uint64_t      bitlen;
    unsigned char buf[64];
    size_t        buflen;
} Sha256;

static uint32_t sha256_rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_init(Sha256 *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(c->state, iv, sizeof iv);
    c->bitlen = 0;
    c->buflen = 0;
}

static void sha256_block(Sha256 *c, const unsigned char *p)
{
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    int      i;

    for (i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)p[i * 4 + 0] << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8)  |
               ((uint32_t)p[i * 4 + 3]);
    }
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g  = c->state[6]; h = c->state[7];

    for (i = 0; i < 64; ++i) {
        uint32_t S1    = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t ch    = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0    = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t maj   = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = cc; cc = b; b = a; a = temp1 + temp2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g;  c->state[7] += h;
}

static void sha256_update(Sha256 *c, const unsigned char *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t take = sizeof c->buf - c->buflen;
        if (take > len - i) take = len - i;
        memcpy(c->buf + c->buflen, data + i, take);
        c->buflen += take;
        i         += take;
        if (c->buflen == sizeof c->buf) {
            sha256_block(c, c->buf);
            c->bitlen += 512u;
            c->buflen  = 0;
        }
    }
}

static void sha256_final(Sha256 *c, unsigned char out[32])
{
    uint64_t bits = c->bitlen + (uint64_t)c->buflen * 8u;
    unsigned char pad = 0x80u;
    unsigned char zero = 0x00u;
    int i;

    sha256_update(c, &pad, 1u);
    while (c->buflen != 56u) sha256_update(c, &zero, 1u);
    for (i = 7; i >= 0; --i) {
        unsigned char byte = (unsigned char)((bits >> (i * 8)) & 0xffu);
        sha256_update(c, &byte, 1u);
    }
    for (i = 0; i < 8; ++i) {
        out[i * 4 + 0] = (unsigned char)(c->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->state[i]);
    }
}

/* Lower-case hex SHA-256 of a file, or 0 on failure. */
static int sha256_file_hex(const char *path, char out_hex[65])
{
    unsigned char *buf;
    size_t         len = 0;
    Sha256         c;
    unsigned char  dig[32];
    int            i;

    buf = read_file(path, &len);
    if (buf == NULL) return 0;

    sha256_init(&c);
    sha256_update(&c, buf, len);
    sha256_final(&c, dig);
    free(buf);

    for (i = 0; i < 32; ++i) {
        static const char hexd[] = "0123456789abcdef";
        out_hex[i * 2 + 0] = hexd[dig[i] >> 4];
        out_hex[i * 2 + 1] = hexd[dig[i] & 0x0f];
    }
    out_hex[64] = '\0';
    return 1;
}

/* ------------------------------------------------------------------ */
/* (1) pathtrace_render produces a DIFFERENT image than Whitted        */
/*     render_image_ex (the two RENDERERS are genuinely distinct).     */
/* ------------------------------------------------------------------ */

static void test_pathtrace_differs_e2e(void)
{
    Scene  scene;
    Camera cam;

    if (build_default_scene(&scene, &cam) != 0) {
        CHECK(0, "default scene builds via scene_default_desc");
        return;
    }
    CHECK(scene.geo.count > 0, "default scene has geometry");

    unsigned char *whitted = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *path    = (unsigned char *)malloc(BUF_BYTES);
    if (whitted == NULL || path == NULL) {
        free(whitted); free(path); scene_free(&scene);
        CHECK(0, "buffers allocated");
        return;
    }
    memset(whitted, 0xAA, BUF_BYTES);
    memset(path,    0x55, BUF_BYTES);

    /* Default Whitted path: render_image_ex with adaptive OFF. */
    RenderParams off;
    make_params(&off, 0);
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &off, whitted) == 0,
          "Whitted render (adaptive=0) succeeds end-to-end");

    /* Path-traced mode: SAME scene, camera, spp and depth. */
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH, path) == 0,
          "pathtrace_render succeeds end-to-end");

    /* The two RENDERERS must NOT be the same image. (Note: the CLI DEFAULT is
     * the path tracer, so this is a library-level renderer comparison, not a
     * default-vs-opt-in CLI comparison.) */
    size_t dbytes = count_diff(whitted, path);
    CHECK(dbytes > 0,
          "pathtrace_render output DIFFERS from the Whitted render output");
    /* ... and by a substantial margin (a real, distinct image, not one pixel).
     * Measured ~64% of bytes differ; a 1% floor is a robust, machine-safe
     * lower bound. */
    CHECK(dbytes > BUF_BYTES / 100u,
          "pathtrace_render differs on more than 1% of the bytes (distinct image)");

    printf("integration_pathtrace: whitted vs pathtrace: %zu/%zu bytes differ "
           "(%.1f%%)\n", dbytes, BUF_BYTES,
           100.0 * (double)dbytes / (double)BUF_BYTES);

    free(whitted); free(path);
    scene_free(&scene);
}

/* ------------------------------------------------------------------ */
/* (2) The fixed-spp Whitted path is byte-identical + unaffected by    */
/*     a preceding path-trace render (library-level stability).        */
/* ------------------------------------------------------------------ */

static void test_default_unchanged_e2e(void)
{
    Scene  scene;
    Camera cam;

    if (build_default_scene(&scene, &cam) != 0) {
        CHECK(0, "default scene builds via scene_default_desc");
        return;
    }

    unsigned char *ex_a   = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *ex_b   = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *plain  = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *ex_aft = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *pt_tmp = (unsigned char *)malloc(BUF_BYTES);
    if (ex_a == NULL || ex_b == NULL || plain == NULL ||
        ex_aft == NULL || pt_tmp == NULL) {
        free(ex_a); free(ex_b); free(plain); free(ex_aft); free(pt_tmp);
        scene_free(&scene);
        CHECK(0, "buffers allocated");
        return;
    }

    RenderParams off;
    make_params(&off, 0);

    /* The default entry point, twice: reproducible. */
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &off, ex_a) == 0,
          "default render_image_ex #1 succeeds");
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &off, ex_b) == 0,
          "default render_image_ex #2 succeeds");
    CHECK(count_diff(ex_a, ex_b) == 0,
          "two default renders are BYTE-IDENTICAL (deterministic)");

    /* The plain render_image() path == the adaptive=0 entry point. */
    CHECK(render_image(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH, plain) == 0,
          "plain render_image succeeds on the default scene");
    CHECK(count_diff(plain, ex_a) == 0,
          "render_image == render_image_ex(adaptive=0) (default untouched)");

    /* Diagnostics still report the fixed n0 budget on the default path. */
    CHECK(render_last_total_samples() ==
              (unsigned long long)WIDTH * (unsigned long long)HEIGHT *
                  (unsigned long long)SPP,
          "default path reports exactly spp samples per pixel");
    CHECK(render_last_max_samples() == SPP,
          "default path reports spp as the max per-pixel count");

    /* Run the path tracer in between: it must leave NO global state that
     * perturbs the default render (the module is strictly additive). */
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH,
                           pt_tmp) == 0,
          "interleaved pathtrace_render succeeds");
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &off, ex_aft) == 0,
          "default render after a path trace succeeds");
    CHECK(count_diff(ex_aft, ex_a) == 0,
          "default output is BYTE-IDENTICAL after a path-trace render");

    printf("integration_pathtrace: default path reproducible + unaffected by "
           "the path tracer (total=%llu, max/px=%d)\n",
           render_last_total_samples(), render_last_max_samples());

    free(ex_a); free(ex_b); free(plain); free(ex_aft); free(pt_tmp);
    scene_free(&scene);
}

/* ------------------------------------------------------------------ */
/* (3) Determinism of the path tracer (and thread-count independence)  */
/* ------------------------------------------------------------------ */

static void test_pathtrace_determinism_e2e(void)
{
    Scene  scene;
    Camera cam;

    if (build_default_scene(&scene, &cam) != 0) {
        CHECK(0, "default scene builds via scene_default_desc");
        return;
    }

    unsigned char *a = (unsigned char *)malloc(BUF_BYTES);
    unsigned char *b = (unsigned char *)malloc(BUF_BYTES);
    if (a == NULL || b == NULL) {
        free(a); free(b); scene_free(&scene);
        CHECK(0, "buffers allocated");
        return;
    }

    /* Two identical path-trace renders must agree byte-for-byte. */
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH, a) == 0,
          "pathtrace_render #1 succeeds");
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH, b) == 0,
          "pathtrace_render #2 succeeds");
    CHECK(count_diff(a, b) == 0,
          "two pathtrace renders are BYTE-IDENTICAL (deterministic)");

#ifdef USE_PTHREADS
    /*
     * Threaded build: the schedule must not affect any pixel. Pin the worker
     * count to 1 and then 4 via the documented RAYTRACER_THREADS override and
     * require byte-identical buffers.
     */
    {
        int ok1 = (setenv("RAYTRACER_THREADS", "1", 1) == 0);
        CHECK(ok1, "setenv(RAYTRACER_THREADS=1) succeeds");
        if (ok1) {
            CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH,
                                   a) == 0,
                  "pathtrace_render (RAYTRACER_THREADS=1) succeeds");
        }

        int ok4 = (setenv("RAYTRACER_THREADS", "4", 1) == 0);
        CHECK(ok4, "setenv(RAYTRACER_THREADS=4) succeeds");
        if (ok4) {
            CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH,
                                   b) == 0,
                  "pathtrace_render (RAYTRACER_THREADS=4) succeeds");
        }

        CHECK(count_diff(a, b) == 0,
              "pathtrace is BYTE-IDENTICAL for RAYTRACER_THREADS=1 vs 4");

        (void)unsetenv("RAYTRACER_THREADS");
    }
    printf("integration_pathtrace: threaded build; RAYTRACER_THREADS=1 vs 4 "
           "byte-identical\n");
#else
    printf("integration_pathtrace: single-threaded build; RAYTRACER_THREADS "
           "check skipped\n");
#endif

    free(a); free(b);
    scene_free(&scene);
}

/* ------------------------------------------------------------------ */
/* (4) CLI end-to-end: DEFAULT == --pathtrace, and the opt-out flags    */
/* ------------------------------------------------------------------ */

static void test_pathtrace_cli_e2e(void)
{
    char def_path[128], def2_path[128];
    char pt_path[128], pt2_path[128];
    char nopt_path[128], legacy_path[128];
    char na_path[128], nt_path[128], st_path[128];
    char cmd[1024];
    char hex[65];
    long pid = (long)getpid();
    struct stat st;

    /* The CLI check needs the built binary; skip (never fail) if absent, so a
     * standalone `make test` (which does not depend on ./raytracer) stays
     * green. `make clean && make && make test` always exercises it. */
    if (stat("./raytracer", &st) != 0) {
        printf("integration_pathtrace: CLI check skipped (./raytracer not "
               "built)\n");
        return;
    }

    (void)snprintf(def_path,    sizeof def_path,    "/tmp/rt_def_%ld.bmp",    pid);
    (void)snprintf(def2_path,   sizeof def2_path,   "/tmp/rt_def2_%ld.bmp",   pid);
    (void)snprintf(pt_path,     sizeof pt_path,     "/tmp/rt_pt_%ld.bmp",     pid);
    (void)snprintf(pt2_path,    sizeof pt2_path,    "/tmp/rt_pt2_%ld.bmp",    pid);
    (void)snprintf(nopt_path,   sizeof nopt_path,   "/tmp/rt_nopt_%ld.bmp",   pid);
    (void)snprintf(legacy_path, sizeof legacy_path, "/tmp/rt_legacy_%ld.bmp", pid);
    (void)snprintf(na_path,     sizeof na_path,     "/tmp/rt_na_%ld.bmp",     pid);
    (void)snprintf(nt_path,     sizeof nt_path,     "/tmp/rt_nt_%ld.bmp",     pid);
    (void)snprintf(st_path,     sizeof st_path,     "/tmp/rt_st_%ld.bmp",     pid);

    /* --- the two renders at the SAME (test) size ---------------------- */

    /* DEFAULT (no flag): path tracer + adaptive + threads. */
    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --width %d --height %d "
        "--samples %d --depth %d --out %s >/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, def_path);
    CHECK(run_cmd(cmd) == 0, "CLI default (no flag) exits 0");

    /* Explicit --pathtrace: must equal the default. */
    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --pathtrace --width %d "
        "--height %d --samples %d --depth %d --out %s >/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, pt_path);
    CHECK(run_cmd(cmd) == 0, "CLI --pathtrace exits 0");

    /* --no-pathtrace: the Whitted renderer (adaptive stays ON), distinct. */
    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --no-pathtrace --width %d "
        "--height %d --samples %d --depth %d --out %s >/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, nopt_path);
    CHECK(run_cmd(cmd) == 0, "CLI --no-pathtrace exits 0");

    /* --- re-runs (reproducibility) ------------------------------------ */

    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --width %d --height %d "
        "--samples %d --depth %d --out %s >/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, def2_path);
    CHECK(run_cmd(cmd) == 0, "CLI default re-run exits 0");

    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --pathtrace --width %d "
        "--height %d --samples %d --depth %d --out %s >/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, pt2_path);
    CHECK(run_cmd(cmd) == 0, "CLI --pathtrace re-run exits 0");

    /* --- opt-out flags are all ACCEPTED (exit 0) ---------------------- */

    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --no-adaptive --width %d "
        "--height %d --samples %d --depth %d --out %s >/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, na_path);
    CHECK(run_cmd(cmd) == 0, "CLI --no-adaptive exits 0");

    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --no-threads --width %d "
        "--height %d --samples %d --depth %d --out %s >/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, nt_path);
    CHECK(run_cmd(cmd) == 0, "CLI --no-threads exits 0");

    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --single-threaded --width %d "
        "--height %d --samples %d --depth %d --out %s >/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, st_path);
    CHECK(run_cmd(cmd) == 0, "CLI --single-threaded (alias) exits 0");

    /* --- non-empty outputs (BMP header is 54 bytes) ------------------- */
    CHECK(file_size(def_path) > 54, "CLI default wrote a non-empty image");
    CHECK(file_size(pt_path)  > 54, "CLI --pathtrace wrote a non-empty image");
    CHECK(file_size(nopt_path) > 54, "CLI --no-pathtrace wrote a non-empty image");

    /* --- the NEW default-vs-opt-out byte relations -------------------- */

    /* DEFAULT == --pathtrace (the default IS the path tracer). */
    CHECK(files_equal(def_path, pt_path),
          "CLI DEFAULT output == --pathtrace output (byte-identical)");

    /* Default is reproducible; --pathtrace is reproducible. */
    CHECK(files_equal(def_path, def2_path),
          "CLI default output is BYTE-IDENTICAL across re-runs");
    CHECK(files_equal(pt_path, pt2_path),
          "CLI --pathtrace output is BYTE-IDENTICAL across re-runs");

    /* --no-pathtrace (Whitted renderer) DIFFERS from the default. */
    CHECK(!files_equal(nopt_path, def_path),
          "CLI --no-pathtrace output DIFFERS from the CLI default output");

    /* --no-adaptive / --no-threads / --single-threaded change only the
     * sampling strategy or the thread schedule, never the (deterministic)
     * path-tracer pixels: all equal the default. */
    CHECK(files_equal(na_path, def_path),
          "CLI --no-adaptive output == default (path tracer is deterministic)");
    CHECK(files_equal(nt_path, def_path),
          "CLI --no-threads output == default (byte-identical)");
    CHECK(files_equal(st_path, def_path),
          "CLI --single-threaded output == default (byte-identical)");

    /* --- the pristine LEGACY fixed-spp Whitted baseline --------------- */

    /* The legacy frame is a fixed 320x180 / 4 spp / depth 4 Whitted render
     * with adaptive OFF, so it requires BOTH --no-pathtrace AND
     * --no-adaptive. Pin it to the exact SHA-256. */
    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --no-pathtrace --no-adaptive "
        "--width %d --height %d --samples %d --depth %d --out %s "
        ">/dev/null 2>&1",
        LEGACY_WIDTH, LEGACY_HEIGHT, LEGACY_SPP, LEGACY_DEPTH, legacy_path);
    CHECK(run_cmd(cmd) == 0, "CLI --no-pathtrace --no-adaptive exits 0");
    CHECK(file_size(legacy_path) > 54,
          "CLI legacy (--no-pathtrace --no-adaptive) wrote a non-empty image");

    if (sha256_file_hex(legacy_path, hex)) {
        printf("integration_pathtrace: CLI legacy SHA-256 = %s\n", hex);
        CHECK(strcmp(hex, LEGACY_SHA256) == 0,
              "CLI --no-pathtrace --no-adaptive reproduces the pristine legacy "
              "baseline SHA-256");
    } else {
        CHECK(0, "CLI legacy output hashes successfully");
    }

    printf("integration_pathtrace: CLI pt=%ld B def=%ld B nopt=%ld B "
           "legacy=%ld B (default==pathtrace, nopt!=default)\n",
           file_size(pt_path), file_size(def_path), file_size(nopt_path),
           file_size(legacy_path));

    (void)unlink(def_path);
    (void)unlink(def2_path);
    (void)unlink(pt_path);
    (void)unlink(pt2_path);
    (void)unlink(nopt_path);
    (void)unlink(legacy_path);
    (void)unlink(na_path);
    (void)unlink(nt_path);
    (void)unlink(st_path);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    test_pathtrace_differs_e2e();
    test_default_unchanged_e2e();
    test_pathtrace_determinism_e2e();
    test_pathtrace_cli_e2e();

    printf("tests/test_integration_pathtrace: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}
