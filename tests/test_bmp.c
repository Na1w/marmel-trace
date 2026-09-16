/*
 * tests/test_bmp.c - Unit tests for the 24-bit BMP writer (src/bmp.c).
 *
 * The Makefile links every test source in tests/ against all project objects EXCEPT
 * src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure.
 *
 * Coverage:
 *   1. Argument validation (NULL path/rgb, non-positive dimensions).
 *   2. BITMAPFILEHEADER / BITMAPINFOHEADER field values.
 *   3. Row padding to 4-byte boundaries, with zero pad bytes.
 *   4. Bottom-up row order and RGB -> BGR channel swap.
 *   5. Full round-trip content check on a padded, non-square image.
 *   6. Header/size consistency for a range of odd/non-square sizes.
 *
 * C11, -Wall -Wextra clean. No rand(). All resources freed, temp files removed.
 */

#include "bmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

#define CHECK_NEAR(a, b, eps, msg) CHECK(fabs((a) - (b)) <= (eps), msg)

/* ------------------------------------------------------------------ */
/* Temp files (fixed names in the system temp dir, removed at the end) */
/* ------------------------------------------------------------------ */

static const char *const TMP_FILES[] = {
    "/tmp/rt_test_bmp_1.bmp",
    "/tmp/rt_test_bmp_2.bmp",
    "/tmp/rt_test_bmp_3.bmp",
    "/tmp/rt_test_bmp_4.bmp"
};
#define NTMP ((int)(sizeof TMP_FILES / sizeof TMP_FILES[0]))

/* ------------------------------------------------------------------ */
/* Little-endian readers + layout helpers                              */
/* ------------------------------------------------------------------ */

static unsigned rd_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int rd_i32(const unsigned char *p)
{
    return (int)rd_u32(p);
}

static size_t row_size_for(int width)
{
    return (((size_t)width * 3u + 3u) / 4u) * 4u;
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

/* Verify every fixed header field plus the on-disk file size. */
static void check_header(const unsigned char *buf, long size, int width, int height,
                         const char *tag)
{
    size_t row_size   = row_size_for(width);
    uint32_t pixel_size = (uint32_t)(row_size * (size_t)height);
    char msg[192];

    snprintf(msg, sizeof msg, "%s: signature byte 0 == 'B'", tag);
    CHECK(buf[0] == 0x42, msg);
    snprintf(msg, sizeof msg, "%s: signature byte 1 == 'M'", tag);
    CHECK(buf[1] == 0x4D, msg);

    snprintf(msg, sizeof msg, "%s: bfSize == on-disk file size", tag);
    CHECK(rd_u32(buf + 2) == (uint32_t)size, msg);

    snprintf(msg, sizeof msg, "%s: bfOffBits == 54", tag);
    CHECK(rd_u32(buf + 10) == 54u, msg);

    snprintf(msg, sizeof msg, "%s: biSize == 40", tag);
    CHECK(rd_u32(buf + 14) == 40u, msg);

    snprintf(msg, sizeof msg, "%s: biWidth", tag);
    CHECK(rd_i32(buf + 18) == width, msg);

    snprintf(msg, sizeof msg, "%s: biHeight", tag);
    CHECK(rd_i32(buf + 22) == height, msg);

    snprintf(msg, sizeof msg, "%s: biPlanes == 1", tag);
    CHECK(rd_u16(buf + 26) == 1u, msg);

    snprintf(msg, sizeof msg, "%s: biBitCount == 24", tag);
    CHECK(rd_u16(buf + 28) == 24u, msg);

    snprintf(msg, sizeof msg, "%s: biCompression == BI_RGB (0)", tag);
    CHECK(rd_u32(buf + 30) == 0u, msg);

    snprintf(msg, sizeof msg, "%s: biSizeImage == rowSize*height", tag);
    CHECK(rd_u32(buf + 34) == pixel_size, msg);

    snprintf(msg, sizeof msg, "%s: file size == 54 + rowSize*height", tag);
    CHECK(size == (long)(54u + pixel_size), msg);
}

/* Count non-zero padding bytes across every row of the pixel array. */
static int count_nonzero_padding(const unsigned char *buf, int width, int height)
{
    size_t row_size = row_size_for(width);
    size_t pad = row_size - (size_t)width * 3u;
    int bad = 0;
    int r;
    size_t k;

    for (r = 0; r < height; ++r) {
        size_t base = 54u + (size_t)r * row_size + (size_t)width * 3u;
        for (k = 0; k < pad; ++k) {
            if (buf[base + k] != 0u) {
                bad++;
            }
        }
    }
    return bad;
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

/* ------------------------------------------------------------------ */
/* Test 1: reject invalid arguments                                    */
/* ------------------------------------------------------------------ */

static void test_invalid_arguments(void)
{
    unsigned char rgb[4 * 4 * 3];
    const char *path = TMP_FILES[0];

    fill_pattern(rgb, 4, 4);

    CHECK(bmp_write(NULL, rgb, 4, 4) != 0, "bmp_write(NULL path) rejected");
    CHECK(bmp_write(path, NULL, 4, 4) != 0, "bmp_write(NULL rgb) rejected");
    CHECK(bmp_write(path, rgb, 0, 4) != 0, "bmp_write(width=0) rejected");
    CHECK(bmp_write(path, rgb, 4, 0) != 0, "bmp_write(height=0) rejected");
    CHECK(bmp_write(path, rgb, -1, 4) != 0, "bmp_write(width=-1) rejected");

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
/* Test 2: magic + header fields (4x3)                                 */
/* ------------------------------------------------------------------ */

static void test_header_fields(void)
{
    const char *path = TMP_FILES[0];
    unsigned char rgb[4 * 3 * 3];
    unsigned char *buf;
    long size = 0;

    fill_pattern(rgb, 4, 3);
    CHECK(bmp_write(path, rgb, 4, 3) == 0, "bmp_write 4x3 succeeds");

    buf = read_whole_file(path, &size);
    CHECK(buf != NULL, "read back 4x3 file");
    if (buf != NULL) {
        check_header(buf, size, 4, 3, "4x3");
        CHECK(rd_u32(buf + 34) == 12u * 3u, "4x3 biSizeImage == 12*3");
        free(buf);
    }
}

/* ------------------------------------------------------------------ */
/* Test 3: row padding                                                 */
/* ------------------------------------------------------------------ */

static void test_row_padding(void)
{
    const char *path = TMP_FILES[1];
    static const int widths[3] = { 4, 5, 3 };
    const int height = 5;
    int i;

    for (i = 0; i < 3; ++i) {
        int w = widths[i];
        size_t row_size = row_size_for(w);
        unsigned char *rgb = (unsigned char *)malloc((size_t)w * (size_t)height * 3u);
        unsigned char *buf;
        long size = 0;
        char tag[32];
        char msg[192];

        CHECK(rgb != NULL, "alloc padding test buffer");
        if (rgb == NULL) {
            continue;
        }
        fill_pattern(rgb, w, height);
        snprintf(tag, sizeof tag, "pad w=%d", w);

        snprintf(msg, sizeof msg, "%s: write succeeds", tag);
        CHECK(bmp_write(path, rgb, w, height) == 0, msg);

        buf = read_whole_file(path, &size);
        snprintf(msg, sizeof msg, "%s: read back", tag);
        CHECK(buf != NULL, msg);
        if (buf != NULL) {
            snprintf(msg, sizeof msg, "%s: file size == 54 + rowSize*height", tag);
            CHECK(size == (long)(54u + row_size * (size_t)height), msg);
            snprintf(msg, sizeof msg, "%s: padding bytes are zero", tag);
            CHECK(count_nonzero_padding(buf, w, height) == 0, msg);
            snprintf(msg, sizeof msg, "%s: biSizeImage", tag);
            CHECK(rd_u32(buf + 34) == (uint32_t)(row_size * (size_t)height), msg);
            free(buf);
        }
        free(rgb);
    }

    /* Explicit expected row sizes. */
    CHECK(row_size_for(4) == 12u, "rowSize(width=4) == 12 (no padding)");
    CHECK(row_size_for(5) == 16u, "rowSize(width=5) == 16 (1 pad byte)");
    CHECK(row_size_for(3) == 12u, "rowSize(width=3) == 12 (3 pad bytes)");
}

/* ------------------------------------------------------------------ */
/* Test 4: bottom-up row order + BGR channel swap                      */
/* ------------------------------------------------------------------ */

static void test_bottom_up_bgr(void)
{
    const char *path = TMP_FILES[2];
    const int w = 3, h = 2;
    /* Source is TOP-DOWN RGB. Top row (y=0), then bottom row (y=1). */
    static const unsigned char src[3 * 2 * 3] = {
        255u,   0u,   0u,   /* (0,0) red     */
          0u, 255u,   0u,   /* (1,0) green   */
          0u,   0u, 255u,   /* (2,0) blue    */
        255u, 255u,   0u,   /* (0,1) yellow  <- bottom-left */
        255u, 255u, 255u,   /* (1,1) white   */
          0u,   0u,   0u    /* (2,1) black   */
    };
    size_t row_size = row_size_for(w);
    unsigned char *buf;
    long size = 0;
    size_t top_off = 54u + row_size * (size_t)(h - 1);

    CHECK(bmp_write(path, src, w, h) == 0, "bmp_write 3x2 succeeds");
    buf = read_whole_file(path, &size);
    CHECK(buf != NULL, "read back 3x2 file");
    if (buf == NULL) {
        return;
    }

    /* First pixel in the file == source BOTTOM-LEFT (yellow) in B,G,R. */
    CHECK(buf[54] == 0u && buf[55] == 255u && buf[56] == 255u,
          "first file pixel is bottom-left (255,255,0) as B,G,R");
    /* R/B swap explicitly: a naive RGB writer would store R first (255). */
    CHECK(buf[54] == 0u && buf[54] != src[0],
          "red/blue channels are swapped (not naive RGB order)");

    /* File row 0 must be the source BOTTOM row (y=1), in BGR. */
    CHECK(buf[57] == 255u && buf[58] == 255u && buf[59] == 255u,
          "file row0 pixel1 == white in BGR");
    CHECK(buf[60] == 0u && buf[61] == 0u && buf[62] == 0u,
          "file row0 pixel2 == black");

    /* Top-left source pixel (255,0,0) -> B,G,R (0,0,255) at 54+rowSize*(h-1). */
    CHECK(buf[top_off + 0] == 0u && buf[top_off + 1] == 0u && buf[top_off + 2] == 255u,
          "top-left (255,0,0) at 54+rowSize*(h-1) stored as B,G,R 0,0,255");
    CHECK(buf[top_off + 3] == 0u && buf[top_off + 4] == 255u && buf[top_off + 5] == 0u,
          "top-middle (0,255,0) stored as B,G,R 0,255,0");
    CHECK(buf[top_off + 6] == 255u && buf[top_off + 7] == 0u && buf[top_off + 8] == 0u,
          "top-right (0,0,255) stored as B,G,R 255,0,0");

    free(buf);
}

/* ------------------------------------------------------------------ */
/* Test 5: full round-trip on a larger padded image (17x13)            */
/* ------------------------------------------------------------------ */

static void test_roundtrip(void)
{
    const char *path = TMP_FILES[3];
    const int w = 17, h = 13;
    size_t row_size = row_size_for(w);
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
    CHECK(bmp_write(path, rgb, w, h) == 0, "bmp_write 17x13 succeeds");

    buf = read_whole_file(path, &size);
    CHECK(buf != NULL, "read back 17x13 file");
    if (buf != NULL) {
        CHECK(size == (long)(54u + row_size * (size_t)h), "17x13 file size");
        for (y = 0; y < h; ++y) {
            for (x = 0; x < w; ++x) {
                size_t si = ((size_t)y * (size_t)w + (size_t)x) * 3u;
                size_t fi = 54u + (size_t)(h - 1 - y) * row_size + (size_t)x * 3u;
                unsigned char R = rgb[si + 0];
                unsigned char G = rgb[si + 1];
                unsigned char B = rgb[si + 2];
                if (buf[fi + 0] != B || buf[fi + 1] != G || buf[fi + 2] != R) {
                    mismatches++;
                }
            }
        }
        CHECK(mismatches == 0, "all 17x13 pixels round-trip exactly (BGR, bottom-up, padded)");
        CHECK(count_nonzero_padding(buf, w, h) == 0, "17x13 padding bytes are zero");
        free(buf);
    }
    free(rgb);
}

/* ------------------------------------------------------------------ */
/* Test 6: non-square / odd sizes                                      */
/* ------------------------------------------------------------------ */

static void test_odd_sizes(void)
{
    const char *path = TMP_FILES[1];
    static const int dims[][2] = {
        { 1, 1 }, { 7, 1 }, { 1, 7 }, { 33, 9 }
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
        CHECK(bmp_write(path, rgb, w, h) == 0, "bmp_write odd size succeeds");

        buf = read_whole_file(path, &size);
        CHECK(buf != NULL, "read back odd size file");
        if (buf != NULL) {
            check_header(buf, size, w, h, tag);
            CHECK(count_nonzero_padding(buf, w, h) == 0, "odd size padding is zero");
            free(buf);
        }
    }
    free(rgb);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_invalid_arguments();
    test_header_fields();
    test_row_padding();
    test_bottom_up_bgr();
    test_roundtrip();
    test_odd_sizes();

    /* Clean up every temp file we may have created. */
    {
        int i;
        for (i = 0; i < NTMP; ++i) {
            remove(TMP_FILES[i]);
        }
    }

    printf("test_bmp.c: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
