/* bmp.c -- self-contained 24-bit uncompressed BMP writer.
 *
 * Emits a canonical BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (40 bytes)
 * followed by bottom-up, 4-byte-padded BGR pixel rows.
 *
 * No dependency on any other project header.  All multi-byte fields are
 * serialised explicitly as little-endian byte sequences, so the output is
 * correct regardless of host endianness, struct padding or type widths.
 */

#include "bmp.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* Little-endian serialisation helpers (write at a fixed address).           */
/* ------------------------------------------------------------------------- */

static void put_u16_le(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)( v       & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_u32_le(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)( v        & 0xFFu);
    p[1] = (unsigned char)((v >> 8)  & 0xFFu);
    p[2] = (unsigned char)((v >> 16) & 0xFFu);
    p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

int bmp_write(const char *path, const unsigned char *rgb, int width, int height)
{
    unsigned char header[54];
    unsigned char *row;
    size_t row_size;      /* padded bytes per row (multiple of 4) */
    size_t pixel_size;    /* total pixel-array bytes             */
    size_t pad;           /* 0..3 trailing pad bytes per row     */
    size_t x, k;
    int y;
    FILE *fp;

    /* --- argument validation --- */
    if (path == NULL || rgb == NULL || width <= 0 || height <= 0) {
        fprintf(stderr, "bmp_write: invalid arguments "
                        "(path=%p, rgb=%p, width=%d, height=%d)\n",
                (const void *)path, (const void *)rgb, width, height);
        return -2;
    }

    /* --- geometry, computed in size_t to avoid overflow --- */
    row_size   = (((size_t)width * 3u + 3u) / 4u) * 4u;
    pixel_size = row_size * (size_t)height;
    pad        = row_size - (size_t)width * 3u;

    /* Guard against overflowing the 32-bit DWORD fields. bfSize must fit in a
     * uint32_t, so the whole file (54 + pixel_size) must be <= 0xFFFFFFFF. */
    if (pixel_size / row_size != (size_t)height ||
        pixel_size > (size_t)0xFFFFFFFFu - 54u) {
        fprintf(stderr, "bmp_write: image too large "
                        "(width=%d, height=%d)\n", width, height);
        return -2;
    }

    /* --- build the 54-byte header (BITMAPFILEHEADER + BITMAPINFOHEADER) --- */
    /* BITMAPFILEHEADER (14 bytes) */
    header[0] = 'B';
    header[1] = 'M';
    put_u32_le(header + 2,  (unsigned)(54u + pixel_size)); /* bfSize        */
    put_u16_le(header + 6,  0u);                           /* bfReserved1   */
    put_u16_le(header + 8,  0u);                           /* bfReserved2   */
    put_u32_le(header + 10, 54u);                          /* bfOffBits     */

    /* BITMAPINFOHEADER (40 bytes, starts at offset 14) */
    put_u32_le(header + 14, 40u);                          /* biSize            */
    put_u32_le(header + 18, (unsigned)width);              /* biWidth           */
    put_u32_le(header + 22, (unsigned)height);             /* biHeight (>0: bottom-up) */
    put_u16_le(header + 26, 1u);                           /* biPlanes          */
    put_u16_le(header + 28, 24u);                          /* biBitCount        */
    put_u32_le(header + 30, 0u);                           /* biCompression = BI_RGB */
    put_u32_le(header + 34, (unsigned)pixel_size);         /* biSizeImage       */
    put_u32_le(header + 38, 2835u);                        /* biXPelsPerMeter   */
    put_u32_le(header + 42, 2835u);                        /* biYPelsPerMeter   */
    put_u32_le(header + 46, 0u);                           /* biClrUsed         */
    put_u32_le(header + 50, 0u);                           /* biClrImportant    */

    /* --- allocate the padded row buffer --- */
    row = (unsigned char *)malloc(row_size);
    if (row == NULL) {
        fprintf(stderr, "bmp_write: out of memory "
                        "(row_size=%zu)\n", row_size);
        return -1;
    }

    /* --- open the output file in binary mode --- */
    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "bmp_write: cannot open '%s' for writing\n", path);
        free(row);
        return -1;
    }

    /* --- write header --- */
    if (fwrite(header, 1, sizeof header, fp) != sizeof header) {
        fprintf(stderr, "bmp_write: failed to write header to '%s'\n", path);
        free(row);
        fclose(fp);
        return -1;
    }

    /* --- write pixel rows: bottom-up, RGB -> BGR, zero padding --- */
    for (y = height - 1; y >= 0; --y) {
        const unsigned char *src =
            rgb + (size_t)y * (size_t)width * 3u;

        for (x = 0; x < (size_t)width; ++x) {
            /* source is R,G,B -> file must be B,G,R */
            row[x * 3u + 0u] = src[x * 3u + 2u];  /* Blue  */
            row[x * 3u + 1u] = src[x * 3u + 1u];  /* Green */
            row[x * 3u + 2u] = src[x * 3u + 0u];  /* Red   */
        }
        for (k = 0; k < pad; ++k)
            row[(size_t)width * 3u + k] = 0u;

        if (fwrite(row, 1, row_size, fp) != row_size) {
            fprintf(stderr, "bmp_write: failed to write pixel row to '%s'\n",
                    path);
            free(row);
            fclose(fp);
            return -1;
        }
    }

    free(row);

    if (fclose(fp) != 0) {
        fprintf(stderr, "bmp_write: failed to close '%s'\n", path);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* PPM (P6) writer                                                           */
/* ------------------------------------------------------------------------- */

int ppm_write(const char *path, const unsigned char *rgb, int width, int height)
{
    char   header[64];
    int    hlen;
    size_t pixel_size;
    FILE  *fp;

    /* --- argument validation (mirrors bmp_write) --- */
    if (path == NULL || rgb == NULL || width <= 0 || height <= 0) {
        fprintf(stderr, "ppm_write: invalid arguments "
                        "(path=%p, rgb=%p, width=%d, height=%d)\n",
                (const void *)path, (const void *)rgb, width, height);
        return -2;
    }

    /* --- build the header: "P6\n<width> <height>\n255\n" --- */
    hlen = snprintf(header, sizeof header, "P6\n%d %d\n255\n", width, height);
    if (hlen <= 0 || (size_t)hlen >= sizeof header) {
        fprintf(stderr, "ppm_write: header formatting failed "
                        "(width=%d, height=%d)\n", width, height);
        return -2;
    }

    /* --- pixel bytes, computed in size_t to avoid overflow --- */
    pixel_size = (size_t)width * (size_t)height * 3u;
    if (pixel_size / 3u != (size_t)width * (size_t)height) {
        fprintf(stderr, "ppm_write: image too large (width=%d, height=%d)\n",
                width, height);
        return -2;
    }

    /* --- open the output file in binary mode --- */
    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "ppm_write: cannot open '%s' for writing\n", path);
        return -1;
    }

    /* --- write header --- */
    if (fwrite(header, 1, (size_t)hlen, fp) != (size_t)hlen) {
        fprintf(stderr, "ppm_write: failed to write header to '%s'\n", path);
        fclose(fp);
        return -1;
    }

    /* --- write pixels verbatim: the buffer is already top-down RGB --- */
    if (fwrite(rgb, 1, pixel_size, fp) != pixel_size) {
        fprintf(stderr, "ppm_write: failed to write pixels to '%s'\n", path);
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "ppm_write: failed to close '%s'\n", path);
        return -1;
    }

    return 0;
}
