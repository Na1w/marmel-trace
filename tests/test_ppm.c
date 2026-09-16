/*
 * tests/test_ppm.c - Unit tests for the binary PPM (P6) writer (src/bmp.c).
 *
 * The Makefile links every test source in tests/ against all project objects EXCEPT
 * src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure.
 *
 * Coverage:
 *   1. Argument validation (NULL path/rgb, non-positive dimensions), and that
 *      no file is created by rejected calls.
 *   2. Exact header bytes: "P6\n<width> <height>\n255\n".
 *   3. Exact file size == header length + width*height*3 (no row padding).
 *   4. Verbatim, top-down RGB pixel order (NO row flip, NO RGB -> BGR swap).
 *   5. Odd / non-square sizes.
 *   6. Full round-trip content check on a larger non-square image.
 *
 * C11, -Wall -Wextra clean. No rand(). All resources freed, temp files removed.
 */

#include "bmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Temp files (fixed names in the system temp dir, removed at the end) */
/* ------------------------------------------------------------------ */

static const char *const TMP_FILES[] = {
    "/tmp/rt_test_ppm_1.ppm",
    "/tmp/rt_test_ppm_2.ppm",
    "/tmp/rt_test_ppm_3.ppm",
    "/tmp/rt_test_ppm_4.ppm"
};
#define NTMP ((int)(sizeof TMP_FILES / sizeof TMP_FILES[0]))

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Length of the canonical P6 header for the given dimensions. */
static size_t header_len(int width, int height)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "P6\n%d %d\n255\n", width, height);
    return (n > 0) ? (size_t)n : 0u;
}

/* Read an entire file into a heap buffer. Returns NULL on error. */
static unsigned char *read_whole_file(const char *path, long *out_size)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *buf;
    long sz;

    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)sz + 1u);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_size = sz;
    return buf;
}

static void fill_pattern(unsigned char *rgb, int width, int height)
{
    int x, y;
    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            size_t i = ((size_t)y * (size_t)width + (size_t)x) * 3u;
            rgb[i + 0] = (unsigned char)((x * 7 + y * 3) & 0xFF);
            rgb[i + 1] = (unsigned char)((x * 11 + y * 5) & 0xFF);
            rgb[i + 2] = (unsigned char)((x * 13 + y * 17) & 0xFF);
        }
    }
}

/* Verify the exact header bytes and the on-disk file size. */
static void check_header(const unsigned char *buf, long size, int width, int height,
                         const char *tag)
{
    char expected[64];
    size_t hlen;
    char msg[192];

    hlen = (size_t)snprintf(expected, sizeof expected, "P6\n%d %d\n255\n",
                            width, height);
    snprintf(msg, sizeof msg, "%s: magic is 'P','6'", tag);
    CHECK(buf[0] == 'P' && buf[1] == '6', msg);

    snprintf(msg, sizeof msg, "%s: header == \"P6\\n%d %d\\n255\\n\"", tag,
             width, height);
    CHECK(memcmp(buf, expected, hlen) == 0, msg);

    snprintf(msg, sizeof msg, "%s: file size == header + width*height*3", tag);
    CHECK(size == (long)(hlen + (size_t)width * (size_t)height * 3u), msg);
}

/* ------------------------------------------------------------------ */
/* Test 1: reject invalid arguments                                    */
/* ------------------------------------------------------------------ */

static void test_invalid_arguments(void)
{
    unsigned char rgb[4 * 4 * 3];
    const char *path = TMP_FILES[0];

    fill_pattern(rgb, 4, 4);

    CHECK(ppm_write(NULL, rgb, 4, 4) != 0, "ppm_write(NULL path) rejected");
    CHECK(ppm_write(path, NULL, 4, 4) != 0, "ppm_write(NULL rgb) rejected");
    CHECK(ppm_write(path, rgb, 0, 4) != 0, "ppm_write(width=0) rejected");
    CHECK(ppm_write(path, rgb, 4, 0) != 0, "ppm_write(height=0) rejected");
    CHECK(ppm_write(path, rgb, -1, 4) != 0, "ppm_write(width=-1) rejected");

    /* None of the rejected calls should have created a file. */
    {
        FILE *fp = fopen(path, "rb");
        CHECK(fp == NULL, "no file created by rejected calls");
        if (fp != NULL) {
            fclose(fp);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Test 2: header bytes + file size (4x3)                              */
/* ------------------------------------------------------------------ */

static void test_header_fields(void)
{
    const char *path = TMP_FILES[0];
    unsigned char rgb[4 * 3 * 3];
    unsigned char *buf;
    long size = 0;

    fill_pattern(rgb, 4, 3);
    CHECK(ppm_write(path, rgb, 4, 3) == 0, "ppm_write 4x3 succeeds");

    buf = read_whole_file(path, &size);
    CHECK(buf != NULL, "read back 4x3 file");
    if (buf != NULL) {
        check_header(buf, size, 4, 3, "4x3");
        /* Explicit canonical header for 4x3. */
        CHECK(size == (long)(header_len(4, 3) + 4u * 3u * 3u),
              "4x3 file size == header + 36");
        free(buf);
    }
}

/* ------------------------------------------------------------------ */
/* Test 3: verbatim top-down RGB (no flip, no BGR swap)                */
/* ------------------------------------------------------------------ */

static void test_verbatim_rgb(void)
{
    const char *path = TMP_FILES[1];
    const int w = 3, h = 2;
    /* Source is TOP-DOWN RGB. Top row (y=0), then bottom row (y=1). */
    static const unsigned char src[3 * 2 * 3] = {
        255u,   0u,   0u,   /* (0,0) red     */
          0u, 255u,   0u,   /* (1,0) green   */
          0u,   0u, 255u,   /* (2,0) blue    */
        255u, 255u,   0u,   /* (0,1) yellow  */
        255u, 255u, 255u,   /* (1,1) white   */
          0u,   0u,   0u    /* (2,1) black   */
    };
    size_t hlen = header_len(w, h);
    unsigned char *buf;
    long size = 0;

    CHECK(ppm_write(path, src, w, h) == 0, "ppm_write 3x2 succeeds");
    buf = read_whole_file(path, &size);
    CHECK(buf != NULL, "read back 3x2 file");
    if (buf == NULL) {
        return;
    }

    /* File size has no row padding: header + w*h*3. */
    CHECK(size == (long)(hlen + (size_t)w * (size_t)h * 3u),
          "3x2 file size == header + 18 (no padding)");

    /* First pixel in the file == source TOP-LEFT in R,G,B (NOT B,G,R). */
    CHECK(buf[hlen + 0] == 255u && buf[hlen + 1] == 0u && buf[hlen + 2] == 0u,
          "first file pixel is top-left (255,0,0) as R,G,B");
    /* R channel is stored first: a BMP-style BGR writer would store 0 here. */
    CHECK(buf[hlen + 0] == src[0] && buf[hlen + 0] != src[2],
          "channels are NOT swapped (R,G,B order preserved)");

    /* Row order is preserved (top row first), no bottom-up flip. */
    CHECK(buf[hlen + 3] == 0u && buf[hlen + 4] == 255u && buf[hlen + 5] == 0u,
          "file pixel 1 == top-middle green in R,G,B");
    CHECK(buf[hlen + 6] == 0u && buf[hlen + 7] == 0u && buf[hlen + 8] == 255u,
          "file pixel 2 == top-right blue in R,G,B");

    /* Second file row == source bottom row. */
    CHECK(buf[hlen + 9]  == 255u && buf[hlen + 10] == 255u && buf[hlen + 11] == 0u,
          "file pixel 3 == bottom-left yellow in R,G,B");
    CHECK(buf[hlen + 12] == 255u && buf[hlen + 13] == 255u && buf[hlen + 14] == 255u,
          "file pixel 4 == white");
    CHECK(buf[hlen + 15] == 0u && buf[hlen + 16] == 0u && buf[hlen + 17] == 0u,
          "file pixel 5 == black");

    /* The whole pixel payload must be byte-identical to the source buffer. */
    CHECK(memcmp(buf + hlen, src, sizeof src) == 0,
          "pixel payload is verbatim top-down RGB");

    free(buf);
}

/* ------------------------------------------------------------------ */
/* Test 4: odd / non-square sizes                                      */
/* ------------------------------------------------------------------ */

static void test_odd_sizes(void)
{
    const char *path = TMP_FILES[2];
    static const int dims[][2] = {
        { 1, 1 }, { 7, 1 }, { 1, 7 }, { 33, 9 }, { 5, 3 }
    };
    const int n = (int)(sizeof dims / sizeof dims[0]);
    unsigned char *rgb = (unsigned char *)malloc(33u * 9u * 3u);
    int i;

    CHECK(rgb != NULL, "alloc odd-size buffer");
    if (rgb == NULL) {
        return;
    }

    for (i = 0; i < n; ++i) {
        int w = dims[i][0];
        int h = dims[i][1];
        unsigned char *buf;
        long size = 0;
        char tag[32];

        fill_pattern(rgb, w, h);
        snprintf(tag, sizeof tag, "size %dx%d", w, h);
        CHECK(ppm_write(path, rgb, w, h) == 0, "ppm_write odd size succeeds");

        buf = read_whole_file(path, &size);
        CHECK(buf != NULL, "read back odd size file");
        if (buf != NULL) {
            check_header(buf, size, w, h, tag);
            CHECK(memcmp(buf + header_len(w, h), rgb,
                         (size_t)w * (size_t)h * 3u) == 0,
                  "odd size payload is verbatim RGB");
            free(buf);
        }
    }
    free(rgb);
}

/* ------------------------------------------------------------------ */
/* Test 5: full round-trip on a larger non-square image (17x13)        */
/* ------------------------------------------------------------------ */

static void test_roundtrip(void)
{
    const char *path = TMP_FILES[3];
    const int w = 17, h = 13;
    size_t hlen = header_len(w, h);
    unsigned char *rgb = (unsigned char *)malloc((size_t)w * (size_t)h * 3u);
    unsigned char *buf;
    long size = 0;
    int x, y;
    long mismatches = 0;

    CHECK(rgb != NULL, "alloc 17x13 buffer");
    if (rgb == NULL) {
        return;
    }
    fill_pattern(rgb, w, h);
    CHECK(ppm_write(path, rgb, w, h) == 0, "ppm_write 17x13 succeeds");

    buf = read_whole_file(path, &size);
    CHECK(buf != NULL, "read back 17x13 file");
    if (buf != NULL) {
        CHECK(size == (long)(hlen + (size_t)w * (size_t)h * 3u), "17x13 file size");
        for (y = 0; y < h; ++y) {
            for (x = 0; x < w; ++x) {
                size_t si = ((size_t)y * (size_t)w + (size_t)x) * 3u;
                size_t fi = hlen + si;   /* same order, no flip/padding */
                if (buf[fi + 0] != rgb[si + 0] ||
                    buf[fi + 1] != rgb[si + 1] ||
                    buf[fi + 2] != rgb[si + 2]) {
                    mismatches++;
                }
            }
        }
        CHECK(mismatches == 0,
              "all 17x13 pixels round-trip exactly (top-down RGB, no padding)");
        free(buf);
    }
    free(rgb);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_invalid_arguments();
    test_header_fields();
    test_verbatim_rgb();
    test_odd_sizes();
    test_roundtrip();

    /* Clean up every temp file we may have created. */
    {
        int i;
        for (i = 0; i < NTMP; ++i) {
            remove(TMP_FILES[i]);
        }
    }

    printf("test_ppm.c: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
