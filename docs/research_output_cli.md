# Research: PPM (P6) Output Integration Report

**Scope:** enable adding a PPM (P6 binary) output format alongside the existing BMP writer in the C11 raytracer.
**Method:** read-only inspection of `src/bmp.h`, `src/bmp.c`, `src/main.c`, `tests/test_bmp.c`,
`tests/test_integration.c`, `Makefile`. No source was modified; this report is the only file written.

---

## 1. `src/bmp.h` / `src/bmp.c` — existing writer contract

### Signature
`src/bmp.h:12`
```c
int bmp_write(const char *path, const unsigned char *rgb, int width, int height);
```

### Image buffer contract (critical for PPM reuse)
From the header comment (`src/bmp.h:4-11`), the buffer is:

- **TOP-DOWN, row-major RGB**.
- Pixel `(x,y)` at `rgb[(y*width + x)*3 + 0] = R`, `+1 = G`, `+2 = B`.
- `y = 0` is the **TOP** row.
- `bmp_write` itself performs the flip to bottom-up and the RGB→BGR swap.

So the same `rgb` pointer/dims passed to `bmp_write` can be passed to a PPM writer with **no
reordering** — P6 is top-down RGB, exactly the buffer's native layout.

### Return / error convention
- `0` on success (`src/bmp.c:147`).
- Non-zero on failure; all failures emit a message to **stderr**:
  - `-2` invalid args: NULL `path`/`rgb`, `width<=0`, `height<=0` (`src/bmp.c:47-55`).
  - `-2` image too large / overflow guard (`src/bmp.c:63-69`).
  - `-1` `malloc` row buffer failure (`src/bmp.c:94-98`).
  - `-1` `fopen(path,"wb")` failure (`src/bmp.c:102-106`).
  - `-1` header `fwrite` failure (`src/bmp.c:110-114`).
  - `-1` pixel-row `fwrite` failure (`src/bmp.c:131-136`).
  - `-1` `fclose` failure (`src/bmp.c:142-144`).

Callers only test `!= 0` (see `src/main.c:431`), so PPM should adopt the same "0 / non-zero + stderr"
convention. Serialization is done with explicit little-endian helpers `put_u16_le` (`src/bmp.c:20`)
and `put_u32_le` (`src/bmp.c:26`); `bmp.c` has **no dependency on other project headers** (only
`bmp.h`, `<stdio.h>`, `<stdlib.h>`) — keep `ppm_write` equally self-contained.

---

## 2. `src/main.c` — CLI, render/write flow, exit codes

### Options struct & defaults
- `Options` struct: `src/main.c:46-56` (fields `width,height,samples,depth,seed,out,scene_path,write_scene_path,want_threads`).
- `#define DEFAULT_OUT "output/scene.bmp"` — `src/main.c:44`.
- `usage()` — `src/main.c:58-80` (documents `--out PATH  output BMP path  (default "output/scene.bmp")` at line 69).

### Parsing loop (`parse_args`, `src/main.c:116-272`)
- Initializes `opt->out = DEFAULT_OUT` at `src/main.c:125`.
- `--help`/`-h` → `usage(stdout); exit(0)` (`src/main.c:136-139`).
- Any arg not starting with `--` → error + `usage(stderr)` + `return -1` (`src/main.c:141-146`).
- Supports both `--opt value` and `--opt=value`: the `=` is split at `src/main.c:148-161`, and
  `take_value()` (`src/main.c:100-113`) pulls the next argv entry when there is no `=`.
- Value-taking option whitelist (`src/main.c:177-184`): `--width --height --samples --depth --seed
  --out --scene --write-scene`. An unknown `--opt` → error + usage + `return -1` (`src/main.c:190-193`).
- `--out` handling (`src/main.c:196-204`):
```c
if (strcmp(name, "--out") == 0) {
    if (val[0] == '\0') {
        fprintf(stderr, "error: --out path must not be empty\n\n");
        usage(stderr, argv[0]);
        return -1;
    }
    opt->out = val;
    continue;
}
```
- `parse_args` returns `-1` on any usage error, `0` otherwise (`src/main.c:272`).

### Render + write flow (`main`, `src/main.c:313-459`)
- `if (parse_args(argc, argv, &opt) != 0) return 2;` — **usage error → exit 2** (`src/main.c:329-330`).
- `--write-scene` early exit path: writes and `return 0` / `return 1` on failure (`src/main.c:337-351`).
- Scene load failures (`scene_desc_load`, `scene_desc_load_string`, `scene_build_from_desc`) → `return 1` (`src/main.c:355-389`).
- Camera (`camera_create`) `src/main.c:395`; buffer alloc with overflow guard `src/main.c:398-411` → `return 1` on failure.
- Render (`src/main.c:415-421`):
```c
rc = render_image(&scene, &cam, opt.width, opt.height,
                  opt.samples, opt.depth, rgb);
if (rc != 0) {
    fprintf(stderr, "error: rendering failed (code %d)\n", rc);
    free(rgb); scene_free(&scene); return 1;
}
```
- **The write site** (`src/main.c:423-436`):
```c
/* 6. Create the output directory, then write the BMP. */
if (ensure_parent_dir(opt.out) != 0) {
    free(rgb); scene_free(&scene); return 1;
}

if (bmp_write(opt.out, rgb, opt.width, opt.height) != 0) {
    fprintf(stderr, "error: failed to write '%s'\n", opt.out);
    free(rgb); scene_free(&scene); return 1;
}
```
- `ensure_parent_dir()` (`src/main.c:280-311`) parses up to the last `/`, `mkdir(...,0755)`, ignores `EEXIST`;
  returns `0`/`-1` with stderr diagnostics.
- Summary uses `stat()` for on-disk size (`src/main.c:439-456`), then `return 0` (`src/main.c:459`).

### Exit-code convention (confirmed)
| Code | Meaning |
|------|---------|
| `0` | success (`src/main.c:459`, `:351`) |
| `2` | CLI usage error, only from `parse_args` failing (`src/main.c:330`) |
| `1` | load / build / render / write failure (`src/main.c:347,361,375,380,389,403,411,421,428,435`) |

All diagnostics go to **stderr** via `fprintf(stderr, ...)`; the success summary uses `printf`.
Errors deliberately do **not** print the exit code, so PPM path should mirror the existing
`"error: failed to write '%s'\n"` message style.

---

## 3. Proposed PPM integration

### 3a. New function `ppm_write` in `src/bmp.{h,c}`

Keep it in the existing module so **no Makefile change is required** (see §5).

**`src/bmp.h`** — add after line 12:
```c
/* Write a binary PPM (P6). `rgb` uses the SAME top-down, row-major RGB
 * layout as bmp_write (no flip, no channel swap). Emits:
 *   "P6\n<width> <height>\n255\n"  followed by width*height*3 raw RGB bytes.
 * Returns 0 on success, non-zero on failure (message on stderr). */
int ppm_write(const char *path, const unsigned char *rgb, int width, int height);
```

**`src/bmp.c`** — add a new function (no new includes needed; `<stdio.h>`/`<stdlib.h>` already present):
```c
int ppm_write(const char *path, const unsigned char *rgb, int width, int height)
{
    char   header[64];
    int    hlen;
    size_t pixel_size;
    FILE  *fp;

    if (path == NULL || rgb == NULL || width <= 0 || height <= 0) {
        fprintf(stderr, "ppm_write: invalid arguments "
                        "(path=%p, rgb=%p, width=%d, height=%d)\n",
                (const void *)path, (const void *)rgb, width, height);
        return -2;
    }

    /* P6 magic, decimal dims, maxval 255; header length is bounded (<= ~33). */
    hlen = snprintf(header, sizeof header, "P6\n%d %d\n255\n", width, height);
    if (hlen <= 0 || (size_t)hlen >= sizeof header) {
        fprintf(stderr, "ppm_write: header formatting failed\n");
        return -2;
    }

    pixel_size = (size_t)width * (size_t)height * 3u;

    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "ppm_write: cannot open '%s' for writing\n", path);
        return -1;
    }

    if (fwrite(header, 1, (size_t)hlen, fp) != (size_t)hlen) {
        fprintf(stderr, "ppm_write: failed to write header to '%s'\n", path);
        fclose(fp);
        return -1;
    }

    /* Buffer is already top-down RGB, so it is written verbatim. */
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
```
Notes:
- The P6 header may legally use a single whitespace/newline between tokens; `"P6\n<w> <h>\n255\n"`
  is the requested canonical form and what most tools (ImageMagick, Netpbm) emit/accept.
- No row padding and no bottom-up flip: total file size = `strlen(header) + width*height*3`.
- Overflow is bounded by `main`'s existing `npixels > SIZE_MAX/3` guard (`src/main.c:399-403`);
  the `size_t` multiply here is safe for the same reason, but a defensive `pixel_size / 3 !=
  (size_t)width * (size_t)height` check may be added to match `bmp_write`'s style.

### 3b. CLI dispatch — **RECOMMENDED: extension-based**

**Recommendation: select by output-file extension (`.ppm` → PPM, otherwise BMP).** Rationale:
- Zero new options; keeps `--out PATH` semantics and the default `output/scene.bmp` unchanged.
- Matches user expectation (`--out foo.ppm` just works) and needs no documentation churn beyond the
  `--out` help text.
- A `--format bmp|ppm` flag is strictly more complex (new option, new validation, must reconcile
  with extension when both are present). If a flag is ever wanted it can be layered on later
  without breaking the extension rule (flag overrides extension).

**Exact edits in `src/main.c`:**

1. **Add a small helper** (place near `ensure_parent_dir`, before `main`), plus include already-present
   `<string.h>` (`src/main.c:27`):
```c
/* Case-insensitive check that `path` ends with `ext` (e.g. ".ppm"). */
static int has_suffix(const char *path, const char *ext)
{
    size_t plen, elen, i;
    if (path == NULL || ext == NULL) return 0;
    plen = strlen(path);
    elen = strlen(ext);
    if (plen < elen) return 0;
    for (i = 0; i < elen; ++i) {
        char a = path[plen - elen + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}
```

2. **Replace the single `bmp_write` call** (`src/main.c:431-436`) with a dispatch:
```c
{
    int ok = has_suffix(opt.out, ".ppm")
                 ? ppm_write(opt.out, rgb, opt.width, opt.height)
                 : bmp_write(opt.out, rgb, opt.width, opt.height);
    if (ok != 0) {
        fprintf(stderr, "error: failed to write '%s'\n", opt.out);
        free(rgb);
        scene_free(&scene);
        return 1;
    }
}
```
This preserves the exit-code contract (write failure → `return 1`) and the existing stderr message.

3. **Update the `--out` help text** in `usage()` (`src/main.c:69`):
```c
"  --out PATH     output image path (BMP; use .ppm for P6)  (default \"%s\")\n"
```

4. **No change needed** to the option whitelist (`src/main.c:177-184`) or the `--out` block
   (`src/main.c:196-204`) — the extension is inspected only at write time.

*(Alternative if a flag is mandated: add `--format` to the whitelist at `src/main.c:177-184`, add a
`const char *format;` field to `Options` (`src/main.c:46-56`), default it in `parse_args`
(`src/main.c:125`), add a `--format` branch alongside `--out` at `src/main.c:196`, validate it is
`bmp`/`ppm`, and let it override the extension rule in step 2. Not recommended.)*

---

## 4. Existing output tests (patterns to mirror for PPM)

### `tests/test_bmp.c`
Harness: `CHECK` macro (`tests/test_bmp.c:33`), `g_pass`/`g_fail`, `main` returns `g_fail ? 1 : 0`
(`tests/test_bmp.c:398`). Temp files: `TMP_FILES[]` (`tests/test_bmp.c:44-49`), removed at end.
Helpers: `read_whole_file` (`:80`), `row_size_for` (`:74`), `rd_u16/rd_u32/rd_i32` (`:57/:62/:69`),
`fill_pattern` (`:175`), `count_nonzero_padding` (`:156`), `check_header` (`:112`).

Test functions and what they assert:
- `test_invalid_arguments` (`:192`) — NULL path/rgb, zero/negative dims rejected **and no file created**
  (`:199-208`).
- `test_header_fields` (`:219`) — magic `'B','M'` (`:120-122`), `bfSize == on-disk size` (`:125`),
  `bfOffBits==54` (`:128`), `biSize==40` (`:131`), `biWidth/biHeight` (`:134/:137`),
  `biPlanes==1` (`:140`), `biBitCount==24` (`:143`), `biCompression==0` (`:146`),
  `biSizeImage==rowSize*h` (`:149`), `size==54+rowSize*h` (`:152`).
- `test_row_padding` (`:242`) — widths {4,5,3}: exact file size, **all pad bytes zero**
  (`:275`), `biSizeImage` (`:277`); explicit `row_size_for` values (`:284-286`).
- `test_bottom_up_bgr` (`:293`) — first file pixel is **bottom-left** in BGR (`:319`), R/B swap
  proven (`:322`), file row 0 == source bottom row (`:326-328`), top-left at
  `54+rowSize*(h-1)` (`:332-336`).
- `test_roundtrip` (`:346`) — 17x13: exact size (`:367`) and **all pixels round-trip**
  (BGR, bottom-up, padded) with zero mismatches (`:380`), padding zero (`:381`).
- `test_odd_sizes` (`:384`) — {1x1, 7x1, 1x7, 33x9}: header + zero padding per size.

**PPM analogues to add** (e.g. `tests/test_ppm.c`, auto-discovered — see §5):
- invalid args rejected, no file created;
- exact header bytes `P6\n<w> <h>\n255\n`;
- **exact file size** = `strlen(header) + w*h*3` (no padding);
- pixel data equals source `rgb` **verbatim** (top-down, RGB order — *no* flip, *no* BGR swap),
  which is the inverse of `test_bottom_up_bgr`'s assertions;
- odd sizes {1x1, 7x1, 1x7, 33x9} and a padded-size image (17x13) with zero byte mismatches.

### `tests/test_integration.c`
Single `int main(void)` (`:163`); renders 160x120 (`:212`) and validates content: channel stats
(`channel_stats` `:41`), luminance `sd > 5.0` (`:235`), bright/dark present (`:244-245`),
`>500` distinct colours (`:249`), non-uniform buffer (`:256`), top-third brighter than bottom
(`:264`), determinism (`:268-270`), 80x60 non-degenerate (`:279-283`).
On-disk BMP validation (`:290-362`): reopen file, then assert magic `'B','M'` (`:334`),
`bfSize==fsz` (`:335`), `bfOffBits==54` (`:337`), `biWidth==160` (`:338`), `biHeight==120` (`:339`),
`biBitCount==24` (`:340`), `biCompression==0` (`:341`), `biSizeImage==rowSize*120` (`:342`),
`fsz==54+rowSize*120` (`:344`), pixel region exists (`:346`), `>2` distinct byte values (`:358`).
Write-failure path: `bmp_write(bad_path,...)` to `/nonexistent_dir_xyz/out.bmp` must return non-zero
(`:367`). Cleanup at `cleanup:` label (`:369`).

**PPM integration analogue:** add a `/tmp/rt_integration.ppm` write, reopen, assert header
`P6\n160 120\n255\n`, exact size `len(header) + 160*120*3`, and that pixel bytes are non-degenerate
(`>2` distinct values), plus a failure test against a nonexistent directory.

### `tests/run_integration.sh` (shell smoke test)
`tests/run_integration.sh:13` sets `OUT="$TMPDIR/rt_it.bmp"`, `EXPECTED_SIZE=57654` (`:14`),
renders with `--out "$OUT"` (`:24-27`), then checks non-empty (`:33`), **exact size** (`:40`),
and magic `BM` (`:46`). A PPM variant would assert size `= 11 + 160*120*3 = 57611` (header
`"P6\n160 120\n255\n"` is 15 bytes → actually `15 + 57600 = 57615`; compute with
`printf 'P6\n160 120\n255\n' | wc -c` at test time rather than hardcoding) and magic `P6`.

---

## 5. Makefile — confirm no change needed

- `SRCS` (line 15) already contains **`src/bmp.c`**:
  `SRCS = src/vec3.c src/camera.c src/bmp.c src/noise.c src/geometry.c src/bvh.c src/material.c src/scene.c src/scene_desc.c src/scene_desc_write.c src/render.c`
- `OBJS = $(SRCS:.c=.o) src/main.o`; `LIB_OBJS = $(filter-out src/main.o,$(OBJS))`.
- Tests are auto-discovered: `TEST_SRCS := $(wildcard tests/*.c)` and `bin/%: tests/%.c $(LIB_OBJS)`.

**Conclusion (CONFIRMED):** placing `ppm_write` inside the existing `src/bmp.c` requires **no
Makefile edit** — `bmp.c` is already compiled into both the `raytracer` binary and every test
binary via `LIB_OBJS`. A new test file (e.g. `tests/test_ppm.c`) is likewise picked up automatically
by `$(wildcard tests/*.c)` and run by `make test`. Adding a *new* `src/*.c` module would instead
require appending it to `SRCS` — avoid that by keeping PPM in `bmp.c`.

---

## Quick reference: exact edit sites

| File | Line(s) | Edit |
|------|---------|------|
| `src/bmp.h` | after 12 | declare `int ppm_write(const char *path, const unsigned char *rgb, int width, int height);` |
| `src/bmp.c` | after 147 | add `ppm_write` (needs only existing `<stdio.h>`/`<stdlib.h>`) |
| `src/main.c` | near 311 | add `has_suffix()` helper (uses existing `<string.h>`) |
| `src/main.c` | 431-436 | replace `bmp_write(...)` call with extension dispatch |
| `src/main.c` | 69 | update `--out` help text |
| `Makefile` | — | **no change** (bmp.c already in `SRCS`; tests auto-discovered) |
| `tests/` | new `test_ppm.c` | auto-discovered by `$(wildcard tests/*.c)` |

MISSION COMPLETE
