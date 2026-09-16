/*
 * tests/test_integration.c - End-to-end integration test for the raytracer.
 *
 * Pipeline under test:
 *   scene_default_desc -> scene_build_from_desc -> scene_default_view
 *     -> camera_create -> render_image -> bmp_write -> independent on-disk
 *        BMP validation.
 *
 * The Makefile links every test source in tests/ against all objects EXCEPT
 * src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on failure.
 *
 * C11, -Wall -Wextra clean. No rand(), no globals other than the harness
 * counters. All heap memory is freed and the temp file is removed.
 */

#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"
#include "bmp.h"
#include "vec3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Small statistics helpers                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    double min, max, mean, stddev;
} ChannelStats;

static ChannelStats channel_stats(const unsigned char *rgb, int w, int h, int channel)
{
    ChannelStats st;
    size_t n = (size_t)w * (size_t)h;
    size_t i;
    double sum = 0.0, sumsq = 0.0;
    unsigned char lo = 255u, hi = 0u;

    for (i = 0; i < n; ++i) {
        unsigned char v = rgb[i * 3u + (size_t)channel];
        double d = (double)v;
        sum += d;
        sumsq += d * d;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }

    st.min = (double)lo;
    st.max = (double)hi;
    st.mean = sum / (double)n;
    {
        double var = sumsq / (double)n - st.mean * st.mean;
        st.stddev = (var > 0.0) ? sqrt(var) : 0.0;
    }
    return st;
}

static double luminance(const unsigned char *p)
{
    return 0.2126 * (double)p[0] + 0.7152 * (double)p[1] + 0.0722 * (double)p[2];
}

static void luminance_stats(const unsigned char *rgb, int w, int h,
                            double *out_mean, double *out_sd)
{
    size_t n = (size_t)w * (size_t)h;
    size_t i;
    double sum = 0.0, sumsq = 0.0;

    for (i = 0; i < n; ++i) {
        double l = luminance(rgb + i * 3u);
        sum += l;
        sumsq += l * l;
    }
    *out_mean = sum / (double)n;
    {
        double var = sumsq / (double)n - (*out_mean) * (*out_mean);
        *out_sd = (var > 0.0) ? sqrt(var) : 0.0;
    }
}

/* Mean luminance over rows [y0, y1). */
static double mean_luminance_region(const unsigned char *rgb, int w, int y0, int y1)
{
    double sum = 0.0;
    size_t count = 0;
    int x, y;

    for (y = y0; y < y1; ++y) {
        for (x = 0; x < w; ++x) {
            const unsigned char *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
            sum += luminance(p);
            count++;
        }
    }
    return (count > 0) ? sum / (double)count : 0.0;
}

/* Count distinct 24-bit colours using a 2^24-bit set (2 MB). */
static int count_distinct_colors(const unsigned char *rgb, size_t npix,
                                 unsigned char *set)
{
    size_t i;
    int distinct = 0;

    for (i = 0; i < npix; ++i) {
        unsigned color = ((unsigned)rgb[i * 3u + 0u] << 16) |
                         ((unsigned)rgb[i * 3u + 1u] << 8)  |
                         ((unsigned)rgb[i * 3u + 2u]);
        size_t byte = (size_t)color >> 3;
        unsigned char mask = (unsigned char)(1u << (color & 7u));

        if ((set[byte] & mask) == 0) {
            set[byte] |= mask;
            distinct++;
        }
    }
    return distinct;
}

/* ------------------------------------------------------------------ */
/* Raw little-endian BMP field readers                                 */
/* ------------------------------------------------------------------ */

static unsigned rd_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned rd_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int rd_i32(const unsigned char *p)
{
    return (int)rd_u32(p);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    const int W = 160, H = 120;       /* primary resolution   */
    const int W2 = 80, H2 = 60;       /* secondary resolution */
    const char *path = "/tmp/rt_integration.bmp";
    const char *bad_path = "/nonexistent_dir_xyz/out.bmp";

    Scene scene;
    SceneDesc desc;
    Camera cam;
    Vec3 eye, target, up;
    double vfov = 0.0;
    double lum_mean = 0.0, lum_sd = 0.0;
    double top_mean, bot_mean;
    double overall_min, overall_max;
    unsigned char *rgb = NULL;
    unsigned char *rgb2 = NULL;
    unsigned char *small = NULL;
    unsigned char *color_set = NULL;
    int rc;

    /* --- 1. build the scene (zero-initialised) from the default desc --- */
    memset(&scene, 0, sizeof scene);
    scene_desc_init(&desc);
    scene_default_desc(&desc);
    rc = scene_build_from_desc(&scene, &desc);
    scene_desc_free(&desc);
    CHECK(rc == 0, "scene_build_from_desc(&scene, &default_desc) returns 0");

    /* --- 2. content checks --- */
    CHECK(scene.geo.count > 0, "scene.geo.count > 0");
    CHECK(scene.material_count > 0, "scene.material_count > 0");

    /* --- 3. framing + camera --- */
    scene_default_view(&eye, &target, &up, &vfov);
    cam = camera_create(eye, target, up, vfov, (double)W / (double)H);

    /* --- allocate buffers --- */
    rgb = (unsigned char *)malloc((size_t)W * (size_t)H * 3u);
    rgb2 = (unsigned char *)malloc((size_t)W * (size_t)H * 3u);
    small = (unsigned char *)malloc((size_t)W2 * (size_t)H2 * 3u);
    color_set = (unsigned char *)calloc((size_t)1 << 21, 1u); /* 2 MB, 2^24 bits */
    CHECK(rgb != NULL && rgb2 != NULL && small != NULL && color_set != NULL,
          "all working buffers allocated");
    if (rgb == NULL || rgb2 == NULL || small == NULL || color_set == NULL)
        goto cleanup;

    /* --- 4. render at 160x120 --- */
    rc = render_image(&scene, &cam, W, H, 4, 5, rgb);
    CHECK(rc == 0, "render_image(160x120, spp=4, depth=5) returns 0");
    if (rc != 0)
        goto cleanup;

    /* --- 5. non-degenerate content --- */
    {
        ChannelStats rs = channel_stats(rgb, W, H, 0);
        ChannelStats gs = channel_stats(rgb, W, H, 1);
        ChannelStats bs = channel_stats(rgb, W, H, 2);
        int distinct;
        size_t npix = (size_t)W * (size_t)H;
        size_t i;
        int all_same = 1;

        printf("channel stats R: min=%.0f max=%.0f mean=%.2f sd=%.2f\n",
               rs.min, rs.max, rs.mean, rs.stddev);
        printf("channel stats G: min=%.0f max=%.0f mean=%.2f sd=%.2f\n",
               gs.min, gs.max, gs.mean, gs.stddev);
        printf("channel stats B: min=%.0f max=%.0f mean=%.2f sd=%.2f\n",
               bs.min, bs.max, bs.mean, bs.stddev);

        luminance_stats(rgb, W, H, &lum_mean, &lum_sd);
        printf("luminance      : mean=%.2f sd=%.2f\n", lum_mean, lum_sd);
        CHECK(lum_sd > 5.0, "luminance stddev > 5.0 (image is not flat)");

        overall_min = rs.min;
        if (gs.min < overall_min) overall_min = gs.min;
        if (bs.min < overall_min) overall_min = bs.min;
        overall_max = rs.max;
        if (gs.max > overall_max) overall_max = gs.max;
        if (bs.max > overall_max) overall_max = bs.max;

        CHECK(overall_max > 200.0, "bright regions present (max channel > 200)");
        CHECK(overall_min < 100.0, "dark regions present (min channel < 100)");

        distinct = count_distinct_colors(rgb, npix, color_set);
        printf("distinct colours: %d\n", distinct);
        CHECK(distinct > 500, "more than 500 distinct pixel colours");

        /* Every byte is inherently 0..255 (no NaN possible); prove the buffer
         * was actually written by requiring it to be non-uniform. */
        for (i = 1; i < npix * 3u; ++i) {
            if (rgb[i] != rgb[0]) { all_same = 0; break; }
        }
        CHECK(!all_same, "buffer fully written (bytes are not all identical)");
    }

    /* --- 6. plausibility: sky (top) brighter than ground/water (bottom) --- */
    top_mean = mean_luminance_region(rgb, W, 0, H / 3);
    bot_mean = mean_luminance_region(rgb, W, 2 * H / 3, H);
    printf("plausibility   : top-third mean lum=%.2f  bottom-third mean lum=%.2f\n",
           top_mean, bot_mean);
    CHECK(top_mean > bot_mean, "top third is brighter than bottom third");

    /* --- 7. determinism --- */
    rc = render_image(&scene, &cam, W, H, 4, 5, rgb2);
    CHECK(rc == 0, "second render returns 0");
    if (rc == 0)
        CHECK(memcmp(rgb, rgb2, (size_t)W * (size_t)H * 3u) == 0,
              "two renders are byte-identical (deterministic)");

    /* --- 8. resolution independence (80x60) --- */
    {
        Camera cam2 = camera_create(eye, target, up, vfov, (double)W2 / (double)H2);
        double s_mean = 0.0, s_sd = 0.0;

        rc = render_image(&scene, &cam2, W2, H2, 4, 5, small);
        CHECK(rc == 0, "render_image(80x60) returns 0");
        if (rc == 0) {
            luminance_stats(small, W2, H2, &s_mean, &s_sd);
            printf("80x60 luminance: mean=%.2f sd=%.2f\n", s_mean, s_sd);
            CHECK(s_sd > 5.0, "80x60 render is non-degenerate (sd > 5.0)");
        }
    }

    /* --- 9. write the BMP --- */
    remove(path);
    rc = bmp_write(path, rgb, W, H);
    CHECK(rc == 0, "bmp_write('/tmp/rt_integration.bmp') returns 0");

    /* --- 10. independent on-disk validation --- */
    if (rc == 0) {
        FILE *fp = fopen(path, "rb");
        CHECK(fp != NULL, "reopen BMP file for independent validation");
        if (fp != NULL) {
            long fsz;
            unsigned char *buf = NULL;
            size_t got = 0;

            if (fseek(fp, 0, SEEK_END) == 0) {
                fsz = ftell(fp);
                rewind(fp);
                if (fsz > 0) {
                    buf = (unsigned char *)malloc((size_t)fsz);
                    if (buf != NULL)
                        got = fread(buf, 1, (size_t)fsz, fp);
                }
            }
            fclose(fp);

            CHECK(buf != NULL && got == (size_t)fsz,
                  "read entire BMP file from disk");
            if (buf != NULL && got == (size_t)fsz) {
                const size_t rowSize = 480; /* ((160*3+3)/4)*4 */
                const size_t expected = 54u + rowSize * 120u; /* 57654 */
                unsigned bfSize = rd_u32(buf + 2);
                unsigned bfOffBits = rd_u32(buf + 10);
                int biWidth = rd_i32(buf + 18);
                int biHeight = rd_i32(buf + 22);
                unsigned biBitCount = rd_u16(buf + 28);
                unsigned biCompression = rd_u32(buf + 30);
                unsigned biSizeImage = rd_u32(buf + 34);
                size_t i;
                size_t pd_off = 54u;
                size_t check_bytes = 1000u;
                int seen[256];
                int distinct_bytes = 0;

                printf("bmp: filesize=%ld bfSize=%u offBits=%u w=%d h=%d bits=%u comp=%u sizeImage=%u\n",
                       fsz, bfSize, bfOffBits, biWidth, biHeight, biBitCount,
                       biCompression, biSizeImage);

                CHECK(buf[0] == 'B' && buf[1] == 'M', "BMP magic bytes are 'BM'");
                CHECK((size_t)bfSize == (size_t)fsz,
                      "bfSize equals real on-disk file size");
                CHECK(bfOffBits == 54u, "bfOffBits == 54");
                CHECK(biWidth == 160, "biWidth == 160");
                CHECK(biHeight == 120, "biHeight == 120");
                CHECK(biBitCount == 24u, "biBitCount == 24");
                CHECK(biCompression == 0u, "biCompression == BI_RGB (0)");
                CHECK((size_t)biSizeImage == rowSize * 120u,
                      "biSizeImage == rowSize * height");
                CHECK((size_t)fsz == expected,
                      "on-disk size == 54 + rowSize*height");
                CHECK((size_t)fsz > pd_off, "pixel data region exists");

                if (pd_off + check_bytes > (size_t)fsz)
                    check_bytes = (size_t)fsz - pd_off;

                memset(seen, 0, sizeof seen);
                for (i = 0; i < check_bytes; ++i) {
                    unsigned char b = buf[pd_off + i];
                    if (!seen[b]) { seen[b] = 1; distinct_bytes++; }
                }
                printf("bmp: distinct byte values in first %zu pixel bytes: %d\n",
                       check_bytes, distinct_bytes);
                CHECK(distinct_bytes > 2,
                      "pixel data not all-zero and not all-0xFF");
            }
            free(buf);
        }
    }

    /* --- 11. bmp_write failure handling --- */
    rc = bmp_write(bad_path, rgb, W, H);
    CHECK(rc != 0, "bmp_write fails for a nonexistent directory");

    /* --- 12. cleanup --- */
cleanup:
    free(rgb);
    free(rgb2);
    free(small);
    free(color_set);
    scene_free(&scene);
    remove(path);

    printf("\nsummary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
