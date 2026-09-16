/*
 * tests/test_water_pathtrace.c - End-to-end INTEGRATION / REGRESSION test for
 *                               the WATER LOOK of the default scene rendered by
 *                               the DEFAULT (path-traced) renderer.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * A bug in `pt_sample_legacy` (src/pathtrace.c) left the mirror / dielectric
 * lobe WEIGHTS un-normalised when `reflectivity + transparency > 1`. The water
 * preset is 1.0 + 0.85 = 1.85, so every water bounce injected a spurious x1.85
 * energy gain, and `scenes/default.scene` rendered with a near-WHITE pond
 * (blown-out water). The fix divides both lobe weights by the renormalisation
 * factor `s`, restoring energy conservation (see tests/test_water_units.c for
 * the corresponding unit test of the weighting contract).
 *
 * This file guards the USER-VISIBLE SYMPTOM end-to-end: the pond in
 * `scenes/default.scene` must render BLUE-TINTED, NOT blown out to near-white.
 *
 * WHAT IT DOES
 * ------------
 *   1. Loads `scenes/default.scene` (falling back to the byte-identical
 *      embedded default description when the file is not reachable, so the test
 *      is self-contained and CWD-independent) and builds the scene + framing
 *      camera exactly as src/main.c does.
 *   2. Renders it in DEFAULT pathtrace mode (320x180, 4 spp, depth 4, seed
 *      1337) to a TEMP PATH via bmp_write(), then reads the on-disk BMP back
 *      with an INDEPENDENT minimal BMP reader, so the assertions run on the
 *      real produced artifact, not just an in-memory buffer.
 *   3. Renders the reference: `--no-pathtrace --no-adaptive` ==
 *      render_image_ex(adaptive=0), i.e. the pristine legacy Whitted render,
 *      to a second temp path and reads it back too.
 *   4. Identifies the WATER region robustly (NO exact-pixel reliance): the
 *      lower half of the frame (the pond is the foreground; water_level = 0.02
 *      puts the water surface 2 cm above the ground, see scenes/default.scene
 *      lines 13-15) masked to the pixels that are BLUE-DOMINANT in the
 *      reference render. The mask is required to cover a large fraction of the
 *      lower band, so a degenerate mask fails loudly.
 *   5. Asserts, on the WATER region of the pathtrace image:
 *        - it is BLUE-DOMINANT on average (mean blue >= mean red),
 *        - a large majority of water pixels are blue-dominant (blue >= red),
 *        - the near-white (blown-out) fraction is BELOW a sane threshold,
 *          i.e. the pond must NOT regress to the pre-fix near-white look.
 *      A determinism check (two pathtrace renders are byte-identical) and a
 *      reference sanity check (the Whitted water is blue, not white) pin the
 *      surrounding contract.
 *   6. When the built ./raytracer binary is present, repeats the two renders
 *      through the REAL CLI (`--pathtrace` / `--no-pathtrace --no-adaptive`)
 *      and re-applies the same water-region assertions to the CLI BMPs. The
 *      CLI block is SKIPPED (never failed) when the binary has not been built,
 *      so a standalone `make test` stays green; `make && make test` exercises
 *      it. This mirrors tests/test_integration_pathtrace.c.
 *
 * CALIBRATION (320x180 / 4 spp / depth 4, seed 1337, water mask defined above)
 *   fixed  : mean R ~182, mean B ~217, near-white(<245) ~ 4%
 *   broken : mean R ~229, mean B ~244, near-white(<245) ~ 52%
 * The near-white threshold (< 15%) sits far from BOTH values, so the check is
 * stable yet still catches the regression with a wide margin.
 *
 * The Makefile links every C source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own int main(void). C11,
 * -Wall -Wextra clean, no rand(), no clock, deterministic. Kept small so
 * `make test` stays fast (a couple of ~0.3 s renders).
 */

/* Feature-test macros so getpid()/setenv()/unlink() are declared under -std=c11. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"
#include "pathtrace.h"
#include "bmp.h"
#include "vec3.h"

#include <math.h>
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

#define WIDTH     320
#define HEIGHT    180
#define SPP       4        /* --samples */
#define DEPTH     4        /* --depth   */
#define SEED      1337u    /* fixed seed for determinism */

#define BUF_BYTES ((size_t)WIDTH * (size_t)HEIGHT * 3u)

/*
 * The water region is the LOWER HALF of the frame (the pond is the foreground;
 * water_level = 0.02 in scenes/default.scene lines 13-15). We do NOT hardcode a
 * row boundary beyond the robust half-frame split; the exact water pixels are
 * selected via the reference render (see water_stats()).
 */
#define WATER_Y0  (HEIGHT / 2)

/*
 * A pixel counts as NEAR-WHITE (blown out) when every channel is at or above
 * this level. 245 sits between the fixed (~4% of water pixels) and broken
 * (~52%) regimes, and comfortably below a full 255 clip.
 */
#define NEAR_WHITE_LEVEL 245

/* Near-white fraction of the water region must stay below this (fixed ~4%,
 * pre-fix regression ~52%). 15% is a stable middle ground. */
#define NEAR_WHITE_MAX 0.15

/* Fraction of water-region pixels that must be blue-dominant (blue >= red).
 * The fixed render is ~99%; the floor is generous but still meaningful. */
#define BLUE_DOMINANT_MIN 0.75

/* The reference-derived water mask must cover at least this fraction of the
 * lower band, otherwise the mask is degenerate and the test is meaningless. */
#define MASK_MIN_COVERAGE 0.50

/* ------------------------------------------------------------------ */
/* Scene construction (identical to src/main.c)                        */
/* ------------------------------------------------------------------ */

/*
 * Load the default scene description. Prefers the shipped
 * `scenes/default.scene` file (the task's subject); falls back to the
 * byte-identical embedded default via scene_default_desc() so the test is
 * self-contained and independent of the working directory. Returns 0 on
 * success, non-zero otherwise. `*used_file` records which source won.
 */
static int load_default_desc(SceneDesc *desc, int *used_file)
{
    char errbuf[512];

    *used_file = 0;
    scene_desc_init(desc);

    if (scene_desc_load(desc, "scenes/default.scene", errbuf, sizeof errbuf) == 0 &&
        desc->material_count > 0) {
        *used_file = 1;
        return 0;
    }

    /* Fall back to the embedded default (documented byte-identical to the
     * file). Reset first: a partial load may have allocated state. */
    scene_desc_free(desc);
    scene_desc_init(desc);
    scene_default_desc(desc);
    if (desc->material_count == 0) {
        scene_desc_free(desc);
        return -1;
    }
    return 0;
}

/* Build the Scene + Camera exactly as src/main.c does: parse -> build ->
 * camera from the parsed `camera { }` block with the real output aspect. */
static int build_scene_camera(Scene *scene, Camera *cam, int *used_file)
{
    SceneDesc desc;

    if (load_default_desc(&desc, used_file) != 0)
        return -1;

    memset(scene, 0, sizeof *scene);
    if (scene_build_from_desc(scene, &desc) != 0) {
        scene_desc_free(&desc);
        return -1;
    }

    {
        Vec3 eye, target, up;
        double vfov;

        if (desc.camera.present) {
            eye    = desc.camera.eye;
            target = desc.camera.target;
            up     = desc.camera.up;
            vfov   = desc.camera.vfov_deg;
        } else {
            scene_default_view(&eye, &target, &up, &vfov);
        }
        *cam = camera_create(eye, target, up, vfov,
                             (double)WIDTH / (double)HEIGHT);
        cam->aperture = desc.camera.aperture;
        if (desc.camera.focus_distance > CAMERA_FOCUS_DISTANCE_DERIVED)
            cam->focus_distance = desc.camera.focus_distance;
    }
    scene_desc_free(&desc);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Independent, minimal BMP reader (24-bit, uncompressed)              */
/* ------------------------------------------------------------------ */

static uint32_t rd_u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16le(const unsigned char *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/*
 * Read a 24-bit uncompressed BMP and return a freshly-malloc'd TOP-DOWN,
 * row-major RGB buffer of w*h*3 bytes (matching the renderer's layout). The
 * writer stores rows bottom-up in BGR, so this reader undoes both. Returns
 * NULL on any malformed input. `*out_w` / `*out_h` receive the dimensions.
 */
static unsigned char *bmp_read_rgb(const char *path, int *out_w, int *out_h)
{
    FILE          *f;
    unsigned char  hdr[54];
    unsigned char *row;
    unsigned char *buf;
    uint32_t       off;
    int            w, hgt, abs_h, bpp, y, x;
    size_t         row_size;

    if (path == NULL || out_w == NULL || out_h == NULL)
        return NULL;

    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return NULL; }
    if (hdr[0] != 'B' || hdr[1] != 'M')              { fclose(f); return NULL; }

    off  = rd_u32le(hdr + 10);
    w    = (int)rd_u32le(hdr + 18);
    hgt  = (int)rd_u32le(hdr + 22);
    bpp  = (int)rd_u16le(hdr + 28);
    if (bpp != 24 || w <= 0 || hgt == 0)             { fclose(f); return NULL; }

    abs_h    = (hgt < 0) ? -hgt : hgt;
    row_size = (((size_t)w * 3u + 3u) / 4u) * 4u;

    buf = (unsigned char *)malloc((size_t)w * (size_t)abs_h * 3u);
    row = (unsigned char *)malloc(row_size);
    if (buf == NULL || row == NULL) {
        free(buf); free(row); fclose(f); return NULL;
    }

    for (y = 0; y < abs_h; ++y) {
        /* File row 0 is the BOTTOM row when biHeight > 0. */
        int    dst = (hgt > 0) ? (abs_h - 1 - y) : y;
        if (fseek(f, (long)(off + (uint32_t)y * (uint32_t)row_size),
                  SEEK_SET) != 0 ||
            fread(row, 1, row_size, f) != row_size) {
            free(buf); free(row); fclose(f); return NULL;
        }
        for (x = 0; x < w; ++x) {
            unsigned char b = row[x * 3 + 0];
            unsigned char g = row[x * 3 + 1];
            unsigned char r = row[x * 3 + 2];
            unsigned char *p = buf + ((size_t)dst * (size_t)w + (size_t)x) * 3u;
            p[0] = r; p[1] = g; p[2] = b;
        }
    }

    free(row);
    fclose(f);
    *out_w = w;
    *out_h = abs_h;
    return buf;
}

/* Write `rgb` to `path` and read it straight back (end-to-end on-disk check). */
static unsigned char *bmp_write_read_back(const unsigned char *rgb,
                                          const char *path)
{
    int            w = 0, h = 0;
    unsigned char *disk;

    if (bmp_write(path, rgb, WIDTH, HEIGHT) != 0)
        return NULL;

    disk = bmp_read_rgb(path, &w, &h);
    if (disk == NULL || w != WIDTH || h != HEIGHT) {
        free(disk);
        return NULL;
    }
    return disk;
}

/* ------------------------------------------------------------------ */
/* Water-region statistics                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    double mean_r, mean_g, mean_b;   /* channel means over the region   */
    double blue_frac;                /* fraction with blue >= red       */
    double near_white_frac;          /* fraction blown out to near-white */
    size_t n;                        /* number of region pixels          */
} WaterStats;

/*
 * Compute water-region statistics for `rgb`. The region is the lower half of
 * the frame, restricted to pixels that are BLUE-DOMINANT in `ref` (the legacy
 * Whitted render, whose pond is unambiguously blue/teal) -- a robust,
 * geometry-aware mask that never relies on a single exact pixel.
 *
 * `*coverage` receives the fraction of the lower band the mask covers.
 */
static WaterStats water_stats(const unsigned char *rgb,
                              const unsigned char *ref,
                              double *coverage)
{
    WaterStats st;
    size_t     n = 0, blue = 0, white = 0;
    double     sr = 0.0, sg = 0.0, sb = 0.0;
    int        x, y;

    memset(&st, 0, sizeof st);

    for (y = WATER_Y0; y < HEIGHT; ++y) {
        for (x = 0; x < WIDTH; ++x) {
            size_t               idx = ((size_t)y * (size_t)WIDTH + (size_t)x) * 3u;
            const unsigned char *rp  = ref + idx;
            const unsigned char *pp  = rgb + idx;

            /* Water pixel := blue-dominant in the reference render. */
            if (!(rp[2] >= rp[0]))
                continue;

            n++;
            sr += (double)pp[0];
            sg += (double)pp[1];
            sb += (double)pp[2];

            if (pp[2] >= pp[0]) blue++;
            if (pp[0] >= NEAR_WHITE_LEVEL &&
                pp[1] >= NEAR_WHITE_LEVEL &&
                pp[2] >= NEAR_WHITE_LEVEL) white++;
        }
    }

    st.n = n;
    if (n > 0) {
        st.mean_r          = sr / (double)n;
        st.mean_g          = sg / (double)n;
        st.mean_b          = sb / (double)n;
        st.blue_frac       = (double)blue / (double)n;
        st.near_white_frac = (double)white / (double)n;
    }

    if (coverage != NULL) {
        size_t band = (size_t)(HEIGHT - WATER_Y0) * (size_t)WIDTH;
        *coverage = (band > 0) ? (double)n / (double)band : 0.0;
    }
    return st;
}

/*
 * Apply the water-region contract to one pathtrace image given its reference.
 * `label` names the render path for diagnostics. Returns nothing; results go
 * through CHECK().
 */
static void assert_water_region(const unsigned char *pt,
                                const unsigned char *ref,
                                const char *label)
{
    double     coverage = 0.0;
    WaterStats w  = water_stats(pt,  ref, &coverage);
    WaterStats rw = water_stats(ref, ref, NULL);

    CHECK(w.n > 0, "water region is non-empty");
    CHECK(coverage > MASK_MIN_COVERAGE,
          "water mask covers a large fraction of the lower band");

    /* (a) The pond must be BLUE-DOMINANT on average. */
    CHECK(w.mean_b >= w.mean_r,
          "pathtrace water region is blue-dominant (mean blue >= mean red)");
    /* (b) ... and for a large majority of individual water pixels. */
    CHECK(w.blue_frac >= BLUE_DOMINANT_MIN,
          "a large majority of water pixels are blue-dominant (blue >= red)");
    /* (c) The blown-out (near-white) fraction must stay BELOW the threshold:
     *     this is the check that catches the pre-fix x1.85 over-brightening. */
    CHECK(w.near_white_frac < NEAR_WHITE_MAX,
          "near-white fraction of the water region is below the threshold "
          "(no regression to a blown-out pond)");

    /* Reference sanity: the Whitted water is blue and NOT blown out. */
    CHECK(rw.mean_b >= rw.mean_r,
          "reference water region is blue-dominant (mask is sane)");
    CHECK(rw.near_white_frac < 0.05,
          "reference water region is not near-white");

    printf("%s: water region n=%zu coverage=%.1f%% "
           "meanRGB=(%.1f,%.1f,%.1f) blue=%.1f%% near-white=%.1f%%\n",
           label, w.n, coverage * 100.0,
           w.mean_r, w.mean_g, w.mean_b,
           w.blue_frac * 100.0, w.near_white_frac * 100.0);
}

/* ------------------------------------------------------------------ */
/* (1) Library-level end-to-end regression (always runs)               */
/* ------------------------------------------------------------------ */

static void test_water_library_e2e(void)
{
    Scene          scene;
    Camera         cam;
    int            used_file = 0;
    char           pt_path[128], ref_path[128];
    long           pid = (long)getpid();
    unsigned char *pt_buf = NULL, *pt_buf2 = NULL;
    unsigned char *ref_buf = NULL;
    unsigned char *pt_disk = NULL, *ref_disk = NULL;
    RenderParams   refp;

    if (build_scene_camera(&scene, &cam, &used_file) != 0) {
        CHECK(0, "default scene + camera build");
        return;
    }
    CHECK(scene.geo.count > 0, "default scene has geometry");
    printf("water_pathtrace: scene source = %s\n",
           used_file ? "scenes/default.scene" : "<embedded default>");

    pt_buf  = (unsigned char *)malloc(BUF_BYTES);
    pt_buf2 = (unsigned char *)malloc(BUF_BYTES);
    ref_buf = (unsigned char *)malloc(BUF_BYTES);
    if (pt_buf == NULL || pt_buf2 == NULL || ref_buf == NULL) {
        free(pt_buf); free(pt_buf2); free(ref_buf);
        scene_free(&scene);
        CHECK(0, "render buffers allocated");
        return;
    }

    /* --- DEFAULT pathtrace render, twice (determinism). --- */
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH, pt_buf) == 0,
          "pathtrace_render (default mode) succeeds end-to-end");
    CHECK(pathtrace_render(&scene, &cam, WIDTH, HEIGHT, SPP, DEPTH, pt_buf2) == 0,
          "pathtrace_render (repeat) succeeds end-to-end");
    CHECK(memcmp(pt_buf, pt_buf2, BUF_BYTES) == 0,
          "two pathtrace renders are byte-identical (deterministic)");

    /* --- Reference: --no-pathtrace --no-adaptive == render_image_ex(0). --- */
    refp.samples_per_pixel = SPP;
    refp.max_depth         = DEPTH;
    refp.adaptive          = 0;
    refp.adaptive_max_spp  = 0;
    refp.adaptive_tau      = 0.0;
    CHECK(render_image_ex(&scene, &cam, WIDTH, HEIGHT, &refp, ref_buf) == 0,
          "reference Whitted render (adaptive=0) succeeds end-to-end");

    /* --- Write to temp paths and read the ON-DISK BMPs back. --- */
    (void)snprintf(pt_path,  sizeof pt_path,  "/tmp/rt_water_pt_%ld.bmp",  pid);
    (void)snprintf(ref_path, sizeof ref_path, "/tmp/rt_water_ref_%ld.bmp", pid);

    pt_disk  = bmp_write_read_back(pt_buf,  pt_path);
    ref_disk = bmp_write_read_back(ref_buf, ref_path);
    if (pt_disk == NULL || ref_disk == NULL) {
        free(pt_buf); free(pt_buf2); free(ref_buf);
        free(pt_disk); free(ref_disk);
        scene_free(&scene);
        CHECK(0, "renders written to disk and read back");
        return;
    }
    /* The on-disk artifact must reproduce the in-memory render exactly. */
    CHECK(memcmp(pt_disk, pt_buf, BUF_BYTES) == 0,
          "on-disk pathtrace BMP round-trips to the rendered buffer");
    CHECK(memcmp(ref_disk, ref_buf, BUF_BYTES) == 0,
          "on-disk reference BMP round-trips to the rendered buffer");

    /* --- The regression contract, on the actual produced image. --- */
    assert_water_region(pt_disk, ref_disk, "library");

    (void)unlink(pt_path);
    (void)unlink(ref_path);
    free(pt_buf); free(pt_buf2); free(ref_buf);
    free(pt_disk); free(ref_disk);
    scene_free(&scene);
}

/* ------------------------------------------------------------------ */
/* (2) CLI end-to-end regression (skipped when ./raytracer is absent)  */
/* ------------------------------------------------------------------ */

/* Run a shell command; return the process exit status, or <0 on failure. */
static int run_cmd(const char *cmd)
{
    int status = system(cmd);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -2;
}

static void test_water_cli_e2e(void)
{
    char           pt_path[128], ref_path[128];
    char           cmd[1024];
    long           pid = (long)getpid();
    struct stat    st;
    unsigned char *pt_disk = NULL, *ref_disk = NULL;
    int            w = 0, h = 0;

    if (stat("./raytracer", &st) != 0) {
        printf("water_pathtrace: CLI check skipped (./raytracer not built)\n");
        return;
    }

    (void)snprintf(pt_path,  sizeof pt_path,  "/tmp/rt_wcli_pt_%ld.bmp",  pid);
    (void)snprintf(ref_path, sizeof ref_path, "/tmp/rt_wcli_ref_%ld.bmp", pid);

    /* DEFAULT pathtrace mode. */
    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --scene scenes/default.scene "
        "--width %d --height %d --samples %d --depth %d --seed %u --out %s "
        ">/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, SEED, pt_path);
    CHECK(run_cmd(cmd) == 0, "CLI default pathtrace exits 0");

    /* Reference: legacy Whitted, fixed spp (adaptive off). */
    (void)snprintf(cmd, sizeof cmd,
        "RAYTRACER_NO_PROGRESS=1 ./raytracer --scene scenes/default.scene "
        "--no-pathtrace --no-adaptive "
        "--width %d --height %d --samples %d --depth %d --seed %u --out %s "
        ">/dev/null 2>&1",
        WIDTH, HEIGHT, SPP, DEPTH, SEED, ref_path);
    CHECK(run_cmd(cmd) == 0, "CLI --no-pathtrace --no-adaptive exits 0");

    pt_disk  = bmp_read_rgb(pt_path,  &w, &h);
    ref_disk = bmp_read_rgb(ref_path, &w, &h);
    if (pt_disk == NULL || ref_disk == NULL || w != WIDTH || h != HEIGHT) {
        free(pt_disk); free(ref_disk);
        (void)unlink(pt_path);
        (void)unlink(ref_path);
        CHECK(0, "CLI renders read back at the expected size");
        return;
    }

    assert_water_region(pt_disk, ref_disk, "cli");

    (void)unlink(pt_path);
    (void)unlink(ref_path);
    free(pt_disk);
    free(ref_disk);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    test_water_library_e2e();
    test_water_cli_e2e();

    printf("tests/test_water_pathtrace: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
