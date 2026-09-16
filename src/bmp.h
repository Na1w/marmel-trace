#ifndef BMP_H
#define BMP_H
#include <stddef.h>

/* Write a 24-bit uncompressed BMP (BITMAPINFOHEADER, 40-byte header).
 * `rgb` points to a TOP-DOWN, row-major RGB buffer: pixel (x,y) at
 *   rgb[(y*width + x)*3 + 0] = R, +1 = G, +2 = B
 * with y = 0 being the TOP row of the image.
 * The writer must flip rows to bottom-up and convert RGB -> BGR as required
 * by the BMP format, and pad each row to a 4-byte boundary with zero bytes.
 * Returns 0 on success, non-zero on failure (with a message on stderr). */
int bmp_write(const char *path, const unsigned char *rgb, int width, int height);

/* Write a binary PPM (P6). `rgb` uses the SAME top-down, row-major RGB
 * layout as bmp_write: pixel (x,y) at rgb[(y*width + x)*3 + 0] = R, +1 = G,
 * +2 = B, with y = 0 being the TOP row.
 * Emits the header "P6\n<width> <height>\n255\n" followed by the raw RGB
 * bytes verbatim -- P6 is natively top-down RGB, so unlike BMP there is NO
 * row flip, NO row padding and NO RGB -> BGR channel swap.
 * Total file size is therefore strlen(header) + width*height*3.
 * Returns 0 on success, non-zero on failure (with a message on stderr). */
int ppm_write(const char *path, const unsigned char *rgb, int width, int height);

#endif
