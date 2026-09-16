# Research Note — 24-bit BMP File Format (for a from-scratch C raytracer)

**Task:** `t-006` — Research the 24-bit BMP binary file format and how to write it portably from C.
**Consumer:** `src/bmp.h` / `src/bmp.c`, exposing
`int bmp_write(const char *path, const unsigned char *rgb, int width, int height);`
**Scope (deliberately tight):** only the BMP binary layout and a portable writer. **Not covered:**
build tooling, rendering, shading, image *reading*.

All multi-byte integers in a BMP are stored **little-endian** (least-significant byte first).
The BMP is a Windows DIB (Device-Independent Bitmap); the layout below is the canonical
`BITMAPFILEHEADER` + `BITMAPINFOHEADER` (a.k.a. `BITMAPINFOHEADER`/`BMP v3`) form, which is the
simplest and most universally readable variant for a 24-bpp true-colour image.

---

## 0. TL;DR (the whole file in six lines)

```
offset 0   : BITMAPFILEHEADER  (14 bytes)   bfType='BM', bfSize, 0, 0, bfOffBits=54
offset 14  : BITMAPINFOHEADER  (40 bytes)   biSize=40, w, h, planes=1, bpp=24, comp=0, ...
offset 54  : pixel data                     bottom-up rows, BGR triples, 4-byte-padded rows
rowSize    = ((width * 3 + 3) / 4) * 4
pixelBytes = rowSize * height
bfSize     = 54 + pixelBytes
```

---

## 1. BITMAPFILEHEADER — 14 bytes

Reference: Microsoft Learn, `BITMAPFILEHEADER` (wingdi.h).

| Offset | Size (bytes) | Field        | C type  | Value for our writer                    |
|-------:|-------------:|--------------|---------|-----------------------------------------|
| 0      | 2            | `bfType`     | `WORD`  | `0x4D42` (bytes `0x42 0x4D` = ASCII `'B'`,`'M'`) |
| 2      | 4            | `bfSize`     | `DWORD` | `54 + rowSize * height` (whole file)    |
| 6      | 2            | `bfReserved1`| `WORD`  | `0`                                     |
| 8      | 2            | `bfReserved2`| `WORD`  | `0`                                     |
| 10     | 4            | `bfOffBits`  | `DWORD` | `54` (14 + 40, start of pixel data)     |
| **14** |              | **total**    |         |                                         |

Notes (verified against MS Learn):

- `bfType` **must** be `0x4d42` — "the ASCII string `BM`". Because the field is a little-endian
  `WORD`, the two bytes on disk are `0x42 ('B')` then `0x4D ('M')`.
- `bfSize` is "the size, in bytes, of the bitmap file" — i.e. the *entire* file including both
  headers and the pixel array. It must match the real on-disk file size.
- `bfReserved1` and `bfReserved2` are reserved and **must be zero**.
- `bfOffBits` is the offset from the start of the file to the pixel bits. With a 40-byte info
  header and no colour table, it is exactly `14 + 40 = 54`.

**WORD** = 2-byte unsigned, **DWORD** = 4-byte unsigned, **LONG** = 4-byte signed (Windows
conventions; we write them explicitly as little-endian byte sequences — see §5).

---

## 2. BITMAPINFOHEADER — 40 bytes (starts at file offset 14)

Reference: Microsoft Learn, `BITMAPINFOHEADER` (wingdi.h).

| Offset (file) | Offset (struct) | Size | Field              | C type  | Value for our writer |
|--------------:|----------------:|-----:|--------------------|---------|----------------------|
| 14            | 0               | 4    | `biSize`           | `DWORD` | `40` (size of *this* header) |
| 18            | 4               | 4    | `biWidth`          | `LONG`  | `width` (pixels)     |
| 22            | 8               | 4    | `biHeight`         | `LONG`  | `height` (positive ⇒ bottom-up) |
| 26            | 12              | 2    | `biPlanes`         | `WORD`  | `1`                  |
| 28            | 14              | 2    | `biBitCount`       | `WORD`  | `24`                 |
| 30            | 16              | 4    | `biCompression`    | `DWORD` | `0` (`BI_RGB`, uncompressed) |
| 34            | 20              | 4    | `biSizeImage`      | `DWORD` | `rowSize * height` (may be `0` for BI_RGB, but set it correctly) |
| 38            | 24              | 4    | `biXPelsPerMeter`  | `LONG`  | `2835` (≈72 DPI) or `0` |
| 42            | 28              | 4    | `biYPelsPerMeter`  | `LONG`  | `2835` (≈72 DPI) or `0` |
| 46            | 32              | 4    | `biClrUsed`        | `DWORD` | `0` (no colour table) |
| 50            | 36              | 4    | `biClrImportant`   | `DWORD` | `0` (all colours important) |
| **54**        | **40**          |      | **end of header**  |         |                      |

### 2.1 Positive vs negative `biHeight`

Verified from MS Learn: "For uncompressed RGB bitmaps, if `biHeight` is positive, the bitmap is a
**bottom-up** DIB with the origin at the lower left corner. If `biHeight` is negative, the bitmap is
a **top-down** DIB with the origin at the upper left corner."

| `biHeight` sign | Row order in file | First row written | Compatibility |
|-----------------|-------------------|-------------------|---------------|
| **Positive** (e.g. `height`) | **bottom-up** | the *bottom* image row | Widest (oldest/most tools). **Use this.** |
| Negative (e.g. `-height`) | top-down | the *top* image row | Allowed for uncompressed RGB, but not accepted by every legacy reader. |

We choose **positive `biHeight`** (bottom-up), because it is the most portable and it is what the
original 24-bpp BMPs from Windows 3.0 used. That forces the writer to emit rows in reverse order
(see §5 pseudocode).

### 2.2 Field value choices and why

- `biSize = 40`: identifies this as a `BITMAPINFOHEADER` (the 12-byte `BITMAPCOREHEADER` and the
  108/124-byte V4/V5 headers exist, but 40 is the safe common denominator for 24-bpp).
- `biPlanes = 1`: "This value must be set to 1."
- `biBitCount = 24`: 24 bits per pixel = 3 bytes per pixel, no palette.
- `biCompression = 0` = `BI_RGB`: uncompressed RGB. (Other values like `BI_BITFIELDS`, `BI_RLE8`
  do not apply to plain 24-bpp true colour.)
- `biSizeImage`: MS Learn says it "can be set to 0 for uncompressed RGB bitmaps", but setting the
  exact padded byte count (`rowSize * height`) is strictly more correct and is accepted everywhere.
- `biClrUsed = 0`: for 24-bpp there is no palette; 0 is the conventional value.
- `biXPelsPerMeter` / `biYPelsPerMeter`: device resolution. `0` is legal (means "unknown"). A
  neutral, widely used value for ~72 DPI is `2835` px/m (72 / 0.0254 ≈ 2834.6, rounded).

Because `biBitCount > 8` and `biCompression == BI_RGB`, there is **no colour table** between the
info header and the pixel array. That is what makes `bfOffBits == 54`.

---

## 3. Pixel data layout

### 3.1 Byte order per pixel: BGR, not RGB

24-bpp pixels are three consecutive bytes. The Windows `RGBTRIPLE` structure is defined (MS Learn)
as:

```c
typedef struct tagRGBTRIPLE {
    BYTE rgbtBlue;    /* blue first  */
    BYTE rgbtGreen;   /* green second */
    BYTE rgbtRed;     /* red last    */
} RGBTRIPLE;
```

So each pixel on disk is **Blue, Green, Red** — i.e. **BGR**. If your renderer produces an
RGB buffer (`rgb[0]=R, rgb[1]=G, rgb[2]=B`), the writer must **swap the first and third bytes** of
every pixel. Getting this wrong yields the classic "red and blue channels swapped" image (a red
apple looks blue).

### 3.2 Row order

With positive `biHeight` the pixel array is **bottom-up**: the first row physically written to the
file is the **last (bottom) row of the image**, and the last row written is the top image row.
Equivalently, the pixel data is the image's rows in reverse vertical order. Within a row, pixels go
**left-to-right**.

(With a negative `biHeight` the order is top-down and no flip is needed — but we use positive height
for portability, so we flip.)

### 3.3 Row padding to a 4-byte boundary

Each row (scan line / stride) must be padded so its length is a multiple of 4 bytes. Wikipedia's BMP
article: "The size of each row is rounded up to a multiple of 4 bytes (a 32-bit DWORD) by padding."
Padding bytes are appended at the **end of each row**, and their value is unspecified ("not
necessarily 0"), though zero is conventional and safest.

For 24-bpp (3 bytes/pixel) the padded row size is:

```
rowSize = ((width * 3 + 3) / 4) * 4;      /* integer division */
padding = rowSize - (width * 3);          /* 0, 1, 2, or 3 bytes */
```

Worked examples:

| width | width*3 | rowSize | padding |
|------:|--------:|--------:|--------:|
| 1     | 3       | 4       | 1       |
| 2     | 6       | 8       | 2       |
| 3     | 9       | 12      | 3       |
| 4     | 12      | 12      | 0       |
| 5     | 15      | 16      | 1       |
| 100   | 300     | 300     | 0       |
| 101   | 303     | 304     | 1       |

Note the MS Learn general stride formula `stride = (((biWidth * biBitCount) + 31) & ~31) >> 3`
is algebraically identical to `((width*3 + 3) / 4) * 4` for `biBitCount = 24`.

---

## 4. Total size computation

```
rowSize     = ((width * 3 + 3) / 4) * 4;   /* padded bytes per row         */
pixelBytes  = rowSize * height;            /* total pixel-array bytes      */
bfOffBits   = 14 + 40;                     /* = 54, no colour table        */
bfSize      = bfOffBits + pixelBytes;      /* = 54 + rowSize * height      */
biSizeImage = pixelBytes;
```

Example: 800 × 600 image.
`rowSize = ((800*3 + 3)/4)*4 = ((2403)/4)*4 = 600*4 = 2400`. Since 800 is a multiple of 4 there is
no padding. `pixelBytes = 2400 * 600 = 1,440,000`. `bfSize = 54 + 1,440,000 = 1,440,054`.

Example: 801 × 600 image.
`rowSize = ((801*3 + 3)/4)*4 = ((2406)/4)*4 = 601*4 = 2404` (1 pad byte/row).
`pixelBytes = 2404 * 600 = 1,442,400`. `bfSize = 1,442,454`.

> `bfSize` must equal the actual number of bytes written to the file. A mismatch is the single most
> common cause of "file won't open" reports.

---

## 5. Portable C writing

### 5.1 Why relying on `struct` layout / padding is unsafe

Naively one might do:

```c
struct { WORD bfType; DWORD bfSize; WORD r1, r2; DWORD bfOffBits; } fh;
fwrite(&fh, sizeof fh, 1, fp);
```

This is **not portable** and is a classic bug source:

1. **Compiler padding.** A C compiler may insert padding between members to satisfy alignment
   (e.g. a `DWORD` at an odd offset). `sizeof(struct)` is then larger than the 14 bytes the format
   requires, and the *offsets* of members no longer match the file offsets. The format is a packed
   byte layout; the compiler is not obligated to make the struct packed.
2. **Endianness.** On a big-endian host, a multi-byte `WORD`/`DWORD` written with `fwrite` is
   stored big-endian, but the BMP requires little-endian. The file would be unreadable on
   little-endian machines (and vice versa).
3. **`int`/`long` width.** `int` is 32 bits on most 64-bit platforms but is not guaranteed; using
   `long` for a 32-bit field is wrong on LP64 (Linux/macOS) where `long` is 64 bits.

Therefore the writer should serialise **field by field into a byte buffer**, controlling both order
and width explicitly.

### 5.2 Approach (a) — explicit little-endian helpers (recommended)

Write each field as a known number of little-endian bytes. Two convenient forms:

```c
#include <stdint.h>
#include <stdio.h>

/* Append `n` little-endian bytes of `v` to buf at *pos, advancing *pos. */
static void put_u16_le(unsigned char *buf, size_t *pos, uint16_t v)
{
    buf[(*pos)++] = (unsigned char)( v        & 0xFF);
    buf[(*pos)++] = (unsigned char)((v >> 8)  & 0xFF);
}

static void put_u32_le(unsigned char *buf, size_t *pos, uint32_t v)
{
    buf[(*pos)++] = (unsigned char)( v        & 0xFF);
    buf[(*pos)++] = (unsigned char)((v >> 8)  & 0xFF);
    buf[(*pos)++] = (unsigned char)((v >> 16) & 0xFF);
    buf[(*pos)++] = (unsigned char)((v >> 24) & 0xFF);
}
```

The shifts-and-mask idiom is correct on **any** host endianness: it extracts the numeric value and
emits it least-significant-byte first. No union tricks, no host-endianness assumptions.

If you prefer streaming directly to the file, the same logic can call `fputc` repeatedly (2 or 4
calls per field) instead of filling a buffer — slightly slower but avoids allocating a header
buffer. Either is fine; the buffer approach below keeps the whole header contiguous and easy to
reason about.

### 5.3 Approach (b) — packed structs (compiler-specific; not recommended)

An alternative is to defeat padding with a packing directive:

```c
#pragma pack(push, 1)
typedef struct {
    uint16_t bfType; uint32_t bfSize; uint16_t r1, r2; uint32_t bfOffBits;
} BmpFileHeader;
#pragma pack(pop)

/* GCC/Clang only: */
struct __attribute__((packed)) BmpFileHeaderGcc { /* ... */ };
```

**Caveats:**

- `#pragma pack` / `__attribute__((packed))` are **compiler-specific extensions**, not standard C.
  MSVC supports `#pragma pack`; GCC/Clang support both; other compilers vary.
- Taking the address of a member of a packed struct can cause **unaligned access** faults on strict
  architectures (ARM, SPARC, some MIPS).
- You **still** have the **endianness problem**: packing removes padding but does not reorder bytes,
  so the fields are wrong on a big-endian host.
- The struct must use fixed-width `uint16_t`/`uint32_t` (from `<stdint.h>`), never `int`/`long`.

For a small, self-contained writer, approach (a) is strictly simpler and fully portable. Use (b) only
if you already depend on the compiler and never target big-endian hosts.

### 5.4 Complete pseudocode — `bmp_write`

Signature (matches the downstream `src/bmp.h`):

```c
int bmp_write(const char *path,
              const unsigned char *rgb,   /* top-down, RGB, width*height*3 bytes */
              int width, int height);
```

Contract:
- `rgb` is row-major, **top row first**, and each pixel is `R,G,B` (3 bytes).
- Returns `0` on success, non-zero on failure (e.g. `-1` for I/O error, `-2` for bad arguments).
- Writes a bottom-up 24-bpp BMP with 4-byte-padded rows.

```c
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

/* ---- little-endian serialisation helpers (host-endianness independent) ---- */
static void put_u16_le(unsigned char *b, size_t *p, uint16_t v)
{
    b[(*p)++] = (unsigned char)( v       & 0xFF);
    b[(*p)++] = (unsigned char)((v >> 8) & 0xFF);
}
static void put_u32_le(unsigned char *b, size_t *p, uint32_t v)
{
    b[(*p)++] = (unsigned char)( v        & 0xFF);
    b[(*p)++] = (unsigned char)((v >> 8)  & 0xFF);
    b[(*p)++] = (unsigned char)((v >> 16) & 0xFF);
    b[(*p)++] = (unsigned char)((v >> 24) & 0xFF);
}

int bmp_write(const char *path, const unsigned char *rgb, int width, int height)
{
    if (path == NULL || rgb == NULL || width <= 0 || height <= 0)
        return -2;

    /* --- geometry --- */
    /* rowSize: padded bytes per row (multiple of 4) */
    long row_size   = (((long)width * 3L + 3L) / 4L) * 4L;
    long pixel_size = row_size * (long)height;
    long pad        = row_size - (long)width * 3L;   /* 0..3 */

    /* --- size sanity checks (avoid silent overflow on huge images) --- */
    if (row_size < 0 || pixel_size < 0 ||
        pixel_size > (long)0xFFFFFFFFL - 54L)
        return -2;

    uint32_t bf_size      = (uint32_t)(54L + pixel_size);
    uint32_t bf_off_bits  = 54u;
    uint32_t bi_size_img  = (uint32_t)pixel_size;

    /* --- 1. build the 54-byte header in a byte buffer --- */
    unsigned char hdr[54];
    size_t p = 0;

    /* BITMAPFILEHEADER (14 bytes) */
    put_u16_le(hdr, &p, 0x4D42);        /* bfType  = 'B','M' */
    put_u32_le(hdr, &p, bf_size);       /* bfSize  = whole file */
    put_u16_le(hdr, &p, 0);             /* bfReserved1 */
    put_u16_le(hdr, &p, 0);             /* bfReserved2 */
    put_u32_le(hdr, &p, bf_off_bits);   /* bfOffBits = 54 */

    /* BITMAPINFOHEADER (40 bytes) */
    put_u32_le(hdr, &p, 40u);                       /* biSize */
    put_u32_le(hdr, &p, (uint32_t)width);           /* biWidth  (positive) */
    put_u32_le(hdr, &p, (uint32_t)height);          /* biHeight (positive => bottom-up) */
    put_u16_le(hdr, &p, 1u);                        /* biPlanes = 1 */
    put_u16_le(hdr, &p, 24u);                       /* biBitCount = 24 */
    put_u32_le(hdr, &p, 0u);                        /* biCompression = BI_RGB */
    put_u32_le(hdr, &p, bi_size_img);               /* biSizeImage = row_size*height */
    put_u32_le(hdr, &p, 2835u);                     /* biXPelsPerMeter (~72 dpi) */
    put_u32_le(hdr, &p, 2835u);                     /* biYPelsPerMeter (~72 dpi) */
    put_u32_le(hdr, &p, 0u);                        /* biClrUsed = 0 */
    put_u32_le(hdr, &p, 0u);                        /* biClrImportant = 0 */
    /* p == 54 here */

    /* --- 2. open file (binary mode; "wb" is portable and no-op on POSIX) --- */
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    /* --- 3. write header --- */
    if (fwrite(hdr, 1, 54, fp) != 54) { fclose(fp); return -1; }

    /* --- 4. write pixel rows, bottom-up, BGR, padded --- */
    unsigned char *row = (unsigned char *)malloc((size_t)row_size);
    if (!row) { fclose(fp); return -1; }

    for (int y = height - 1; y >= 0; --y) {        /* bottom row first */
        const unsigned char *src = rgb + (size_t)y * (size_t)width * 3u;
        long x;
        for (x = 0; x < width; ++x) {
            /* src is R,G,B -> file must be B,G,R */
            row[x * 3 + 0] = src[x * 3 + 2];       /* Blue  <- src[2] */
            row[x * 3 + 1] = src[x * 3 + 1];       /* Green <- src[1] */
            row[x * 3 + 2] = src[x * 3 + 0];       /* Red   <- src[0] */
        }
        for (long k = 0; k < pad; ++k)             /* zero pad to 4-byte boundary */
            row[(long)width * 3 + k] = 0;

        if (fwrite(row, 1, (size_t)row_size, fp) != (size_t)row_size) {
            free(row); fclose(fp); return -1;
        }
    }

    free(row);

    /* --- 5. close; on success the file size must equal bf_size --- */
    if (fclose(fp) != 0) return -1;
    return 0;
}
```

**Pixel-swap summary:** with `src` laid out `R,G,B`, the file byte order becomes `B,G,R`, i.e. the
first and third source bytes are swapped:

```c
row[3x + 0] = src[3x + 2];   /* B */
row[3x + 1] = src[3x + 1];   /* G */
row[3x + 2] = src[3x + 0];   /* R */
```

### 5.5 Alternative: stream fields directly with `fputc`

If you would rather not allocate a header buffer, the helpers can write straight to `FILE*`:

```c
static int fput_u16_le(FILE *fp, uint16_t v) {
    return fputc(v & 0xFF, fp) != EOF && fputc((v >> 8) & 0xFF, fp) != EOF;
}
static int fput_u32_le(FILE *fp, uint32_t v) {
    return fput_u16_le(fp, (uint16_t)(v & 0xFFFF)) &&
           fput_u16_le(fp, (uint16_t)(v >> 16));
}
```

Same output, no intermediate buffer, slightly more `fputc` calls. Both are correct; pick one and
keep it consistent with `src/bmp.c`'s style.

---

## 6. Validation checklist

After `bmp_write` produces a file, verify the following. Several can be done with standard tools.

1. **Magic bytes.** The first two bytes must be `0x42 0x4D` (`BM`).
   `xxd -l 2 out.bmp` → `00000000: 424d ...`
2. **`bfSize` equals the real file size.** Read the 4 bytes at offset 2 (little-endian) and compare
   with the actual size on disk:
   `stat -f%z out.bmp` (macOS) / `stat -c%s out.bmp` (Linux) vs. `xxd -s 2 -l 4 out.bmp`.
3. **`bfOffBits == 54`.** Bytes at offset 10 should be `36 00 00 00`.
4. **`biSize == 40`.** Bytes at offset 14 should be `28 00 00 00`.
5. **Header values.** At offset 18: `biWidth` = expected width. At offset 22: `biHeight` =
   expected height (positive). At offset 26: `01 00` (planes). At offset 28: `18 00`
   (= 24 bpp). At offset 30: `00 00 00 00` (`BI_RGB`).
6. **`biSizeImage == bfSize - 54`** and equals `rowSize * height`.
7. **Pixel count / file length.** File size on disk must equal `54 + rowSize * height`, where
   `rowSize = ((width*3 + 3)/4)*4`. Equivalently, `(filesize - 54) / height == rowSize`.
8. **First pixel sanity (bottom-up + BGR).** The first 3 pixel bytes (offset 54) are the
   **bottom-left** pixel, in **B,G,R** order. If you render a test image whose bottom-left pixel is
   known (e.g. pure red `255,0,0`), those bytes must read `00 00 FF`.
9. **Round-trip open.** Open the file in an independent viewer (macOS Preview, GIMP, an image
   library, or `python3 -c "from PIL import Image; Image.open('out.bmp').verify()"`). A vertically
   flipped image ⇒ row-order bug; swapped red/blue ⇒ BGR bug; skewed rows ⇒ padding bug.
10. **Padding bytes are zero** (not required, but a good invariant). For widths where
    `width*3 % 4 != 0`, the trailing `pad` bytes of each row should be `0x00`.

A tiny independent verifier in C or Python that reads back the headers and checks 1–7 catches
essentially every writer bug.

---

## 7. Common mistakes

1. **Red/blue swapped (RGB written instead of BGR).** 24-bpp BMP stores `B,G,R` per pixel. The
   image looks colour-inverted on the R/B channels. Fix: swap bytes 0 and 2 of each pixel.
2. **Image upside-down.** Rows must be written **bottom-up** when `biHeight` is positive. Writing
   top-down with a positive height flips the image vertically. (Alternative: use negative
   `biHeight` and write top-down — but that is less portable.)
3. **Missing row padding.** Forgetting `rowSize = ((width*3 + 3)/4)*4` shifts every subsequent row
   by 1–3 bytes, producing a diagonal skew. Padding must be applied to **every** row, including the
   last.
4. **Wrong `bfSize` / `biSizeImage`.** Setting `bfSize` to `54 + width*height*3` (unpadded) is wrong
   whenever `width*3 % 4 != 0`. Always use the padded `pixel_size`.
5. **`bfOffBits` not `54`.** If you (incorrectly) emit a colour table, or compute the offset wrong,
   readers start reading pixels from the wrong place. For 24-bpp `BI_RGB` there is no colour table,
   so it is exactly 54.
6. **Struct padding / `sizeof(struct)` used as the header size.** The `BITMAPFILEHEADER` is 14
   bytes and the `BITMAPINFOHEADER` is 40 bytes *on disk*; a C compiler may make `sizeof` larger.
   Never `fwrite` a whole struct without packing + endian control.
7. **Endianness.** Writing `WORD`/`DWORD` directly on a big-endian host produces a big-endian file.
   Use explicit little-endian serialisation.
8. **`int`/`long` width assumptions.** Use `uint16_t`/`uint32_t` (`<stdint.h>`) for 16/32-bit
   fields. `long` is 64 bits on LP64 (macOS/Linux).
9. **Sign error on `biHeight`.** A negative `biHeight` means top-down; if you intended bottom-up but
   stored the magnitude as negative (or vice versa), the image flips. Keep it positive.
10. **Text-mode file open on Windows.** `fopen(path, "w")` translates `\n` → `\r\n`, corrupting
    binary data. Always use `"wb"`. (On POSIX `"wb"` is equivalent to `"w"`, so use `"wb"` everywhere.)
11. **`biPlanes` not `1`.** Must be 1; some readers reject anything else.
12. **Forgetting that `biSizeImage` counts padded bytes**, not raw `width*height*3`.
13. **Overflow on large images.** `width * 3 * height` can overflow 32-bit `int`. Compute sizes in
    `long`/`size_t` and reject results that exceed `0xFFFFFFFF - 54` (BMP `bfSize` is a 32-bit
    field).

---

## 8. Sources

- Microsoft Learn — `BITMAPFILEHEADER` structure (wingdi.h):
  <https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapfileheader>
  (field list; `bfType` must be `0x4d42` = "BM"; `bfSize`; reserved must be zero; `bfOffBits`).
- Microsoft Learn — `BITMAPINFOHEADER` structure (wingdi.h):
  <https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader>
  (field list and sizes; `biPlanes` must be 1; `biSizeImage` may be 0 for uncompressed RGB;
  positive `biHeight` = bottom-up, negative = top-down; stride formula).
- Microsoft Learn — `RGBTRIPLE` structure (wingdi.h):
  <https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-rgbtriple>
  (`rgbtBlue`, `rgbtGreen`, `rgbtRed` ⇒ BGR byte order).
- Microsoft Learn — Bitmap Storage:
  <https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-storage>
  (bottom-up vs top-down scan-line ordering; worked `Redbrick.bmp` hex dump showing `42 4d …`).
- Wikipedia — BMP file format:
  <https://en.wikipedia.org/wiki/BMP_file_format>
  (all integers little-endian; `0x42 0x4D` header; rows rounded up to a multiple of 4 bytes;
  colour-table entry order blue, green, red).
