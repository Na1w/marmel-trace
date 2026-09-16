# Render Notes — Final Photorealistic Frame

**Artifact:** `output/scene.bmp`
**Binary:** `./raytracer` (threaded build, `-DUSE_PTHREADS -pthread`)
**Host:** Apple M1 Max, macOS (Darwin 25.6.0, arm64), 10 logical CPUs
**Date of run:** 2026-09-13 17:17:02 CEST (artifact mtime); session 17:15–17:17 CEST
**Predecessor:** this document replaces the pre-hardening notes; the previous
`output/scene.bmp` (SHA-256 `4ba68532…`) was preserved to `/tmp/scene_OLD.bmp`
and analysed like-for-like before being overwritten.

---

## 1. Final settings (what was actually used)

| Parameter | Value |
|---|---|
| Resolution | **1920 x 1080** |
| Samples per pixel | **16** (4x4 stratified jittered grid) |
| Max recursion depth | **6** |
| Scene seed | **1337** |
| Threaded | **Yes** (threaded build — the `make` default) |
| Worker threads used | **10** (`min(sysconf(_SC_NPROCESSORS_ONLN)=10, RENDER_MAX_THREADS=64, ntiles=8100)`) |
| Tile size | **16 x 16** px (dynamic atomic work-stealing) |
| Renderer | **Whitted-style** (this frame predates path-tracing-as-default; reproduce with `--no-pathtrace --no-adaptive`) |
| Output | `output/scene.bmp` (24-bit uncompressed BMP, bottom-up) |

> **Default-behaviour note (current release).** Path tracing (global illumination),
> adaptive sampling and multithreading are now **ON by default**, each with a `--no-*`
> opt-out (`--no-pathtrace`, `--no-adaptive`, `--no-threads`). The frame documented in
> this section was rendered with the legacy **Whitted** renderer and fixed sampling, so
> it is reproduced today with `--no-pathtrace --no-adaptive` (see §1/§8). Adaptive
> sampling applies to the Whitted renderer only and is ignored by the path tracer.

### Exact commands

```sh
make clean
make                  # threaded build is now the DEFAULT (-DUSE_PTHREADS -pthread)
RAYTRACER_NO_PROGRESS=1 ./raytracer --width 1920 --height 1080 \
    --samples 16 --depth 6 --seed 1337 --no-pathtrace --no-adaptive \
    --out output/scene.bmp
```

`make` now builds the threaded binary by default; a serial build is available via
`make serial` (alias `make no-threads`), and `make threads` remains a
backward-compatible alias for the default. `RAYTRACER_NO_PROGRESS=1` silences the
stderr progress meter; it has no effect on the rendered pixels or on the reported
timing (the timer brackets the render call only, not the BMP write).

### Build result

`make` (threaded — the default) completed with **exit code 0 and zero warnings /
zero errors** (`-Wall -Wextra`). The full build log was checked with
`grep -icE "warning|error"` → **0** matches across all 13 compile/link lines
(12 translation units + 1 link).

---

## 2. Timing and throughput

Measured wall-clock (`CLOCK_MONOTONIC` inside the renderer, cross-checked by the
shell `/usr/bin/time -p` wrapper):

| Render | Pixels | Samples | Wall-clock | Samples/s | Pixels/s |
|---|---|---|---|---|---|
| **1920x1080 @ 16 spp (new)** | 2,073,600 | 33,177,600 | **67.33 s** (1 m 7 s) | **492,762** | **30,797** |
| 1920x1080 @ 16 spp (old, pre-hardening) | 2,073,600 | 33,177,600 | 448.22 s | 74,021 | 4,626 |

**Measured speed-up: 448.22 / 67.33 = 6.66x** for the identical workload
(same resolution, spp, depth and seed). This is *not* a threading-only gain: it
combines the tile-based dynamic scheduler with the hardened BVH traversal and
the corrected water paths, which removed the pathological per-sample cost that
dominated the previous build.

Threading effectiveness (from `/usr/bin/time -p`):

```
real 67.69
user 634.38
sys 1.22
```

`user/real = 634.38 / 67.69 = 9.37` on **10 workers → ~93.7% parallel
efficiency**. The previous static-band build achieved `user/real ≈ 4.0` on 8
workers (~50% efficiency), so the dynamic tile scheduler both uses more cores
and balances them better.

### Program stdout summary (final render)

```
Scene: 1286 primitives, 7 materials
Render: 1920x1080, 16 spp, depth 6, seed 1337
Time: 67.33 s (threaded)
Wrote output/scene.bmp (6220854 bytes)
```

### Determinism across thread counts (verified)

The renderer's PRNG is a pure function of `(x, y, sample index, fixed
constant)`, independent of tile/thread scheduling. A 320x180 @ 4 spp probe was
rendered with `RAYTRACER_THREADS=1`, `=8` and `=10`; all three BMPs are
**byte-identical**:

```
f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb  /tmp/t1.bmp
f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb  /tmp/t8.bmp
f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb  /tmp/t10.bmp
```

> Note: the brief mentioned "~1650 primitives"; the seed-1337 build actually
> reports **1286 primitives, 7 materials**. Reported as observed.

---

## 3. Independent verification of `output/scene.bmp`

Verification was performed by a **throwaway program written from scratch in
`/tmp`** (`/tmp/bmp_analyze.c`, ~217 lines) that parses the BMP header and
pixel array itself and does **not** reuse any project code. Header fields were
also cross-checked with `file`, `wc -c`, `stat` and `xxd`.

### 3.1 File size

```
$ file output/scene.bmp
output/scene.bmp: PC bitmap, Windows 3.x format, 1920 x 1080 x 24,
                  image size 6220800, resolution 2835 x 2835 px/m,
                  cbSize 6220854, bits offset 54

$ wc -c output/scene.bmp
 6220854 output/scene.bmp
```

- On-disk size: **6,220,854 bytes** (5.93 MiB)
- `bfSize` (u32 LE @ offset 2): **6,220,854** → **PASS (equal to on-disk size)**
- Raw header bytes at offset 2: `36 ec 5e 00` → `0x005EEC36` = **6,220,854** ✓
- `rowSize = ((1920*3 + 3)/4)*4 = 5760` (width 1920 is a multiple of 4 → no padding)
- `54 + rowSize*height = 54 + 5760*1080 = 6,220,854` → **PASS (exact match)**

### 3.2 Header fields

Raw header bytes (`xxd -l 54`):

```
00000000: 424d 36ec 5e00 0000 0000 3600 0000 2800
00000010: 0000 8007 0000 3804 0000 0100 1800 0000
00000020: 0000 00ec 5e00 130b 0000 130b 0000 0000
00000030: 0000 0000 0000
```

| Field | Expected | Observed | Result |
|---|---|---|---|
| magic | `42 4d` ("BM") | `42 4d` | PASS |
| `bfSize` (@2) | file size | 6,220,854 | PASS |
| `bfOffBits` (@10) | 54 | 54 | PASS |
| `biSize` (@14) | 40 | 40 | PASS |
| `biWidth` (@18) | 1920 | 1920 | PASS |
| `biHeight` (@22) | 1080 | 1080 (positive → bottom-up) | PASS |
| `biPlanes` (@26) | 1 | 1 | PASS |
| `biBitCount` (@28) | 24 | 24 | PASS |
| `biCompression` (@30) | 0 (BI_RGB) | 0 | PASS |
| `biSizeImage` (@34) | 6,220,800 | 6,220,800 (= 5760*1080) | PASS |

`file` independently reports: `1920 x 1080 x 24`, `cbSize 6220854`,
`bits offset 54`. All checks pass.

---

## 4. Image statistics (new render)

### 4.1 Per-channel statistics (8-bit, 0–255)

| Channel | Min | Max | Mean | Stddev |
|---|---|---|---|---|
| R | 25 | 255 | 136.339 | 80.463 |
| G | 33 | 255 | 152.671 | 77.035 |
| B | 27 | 255 | 156.983 | 88.371 |
| **Luminance** (0.2126R+0.7152G+0.0722B) | — | — | **149.510** | **78.393** |

- **Distinct colours: 7,848** (out of 16.7 M possible)
- Non-degenerate check (lum stddev > 10 **and** distinct > 5000): **PASS**
  (stddev 78.39, distinct 7,848)

### 4.2 Difference vs the previous (pre-hardening) image

The old image was analysed with the same `/tmp` tool before being overwritten;
its measured values reproduce the previously documented ones exactly
(R 164.905±57.458, G 178.988±54.732, B 187.586±65.315, 22,497 colours), which
validates the comparison.

| Metric | Old | New | Delta |
|---|---|---|---|
| R mean | 164.905 | 136.339 | **−28.566** |
| G mean | 178.988 | 152.671 | **−26.317** |
| B mean | 187.586 | 156.983 | **−30.603** |
| R stddev | 57.458 | 80.463 | +23.005 |
| G stddev | 54.732 | 77.035 | +22.303 |
| B stddev | 65.315 | 88.371 | +23.056 |
| Luminance mean | 176.615 | 149.510 | **−27.105** |
| Luminance stddev | 55.808 | 78.393 | **+22.585** |
| Distinct colours | 22,497 | 7,848 | **−14,649 (−65.1%)** |

Interpretation (measurement-driven):

- The **overall frame is ~27 luminance units darker** and the **contrast
  (stddev) is ~40% higher**. The extra spread comes from the water region
  dropping sharply (see §4.4/§5) while the sky is unchanged.
- The **distinct-colour count fell by 65%** to 7,848. The old image's sky/water
  contained many near-white, high-value colours; the corrected water now
  converges to a compact deep-teal target instead of spraying a broad band of
  bright, washed-out blues. The sky and foliage counts are essentially
  unchanged, so the reduction is almost entirely in the water.

### 4.3 Regional split (sky / water / ground), foliage line at display row 473

| Region | Pixels | Share | Mean RGB |
|---|---|---|---|
| Sky (above the foliage line) | 908,160 | 43.796% | 218.1, 230.8, 250.6 |
| Water (below the line, teal/blue) | 986,065 | 47.553% | **61.4, 80.4, 80.8** |
| Ground/foliage (below the line, green) | 179,054 | 8.635% | 134.3, 155.0, 101.9 |
| Warm/bark (R>G, anywhere) | 321 | 0.015% | — |

Old image, same classifier: water mean **121.9, 136.1, 145.7** (47.322%);
sky 218.1, 230.8, 250.6 (43.796%); ground 132.0, 152.6, 100.4 (8.866%).

→ The **sky region is unchanged** (identical mean and identical 908,160-pixel
count) and the **ground/foliage region is essentially unchanged** (mean within
~2.3 per channel; count differs by ~2.6%, i.e. a few thousand pixels reclassified
at the water/ground boundary). The **water region darkened from
(121.9, 136.1, 145.7) to (61.4, 80.4, 80.8)** — roughly half the value and now
clearly *teal* (G≈B ≫ R, B−R ≈ +19.4) instead of the old washed-out pale blue
(B−R ≈ +23.8 but with a much higher floor).

### 4.4 Sixteen horizontal bands (band 0 = **top** of the displayed image)

| Band | y-range | meanR | meanG | meanB | meanLum | mean(B−R) |
|---|---|---|---|---|---|---|
| 0 | [0..67) top (sky) | 210.9 | 225.9 | 250.7 | 224.49 | +39.75 |
| 1 | [67..135) sky | 216.5 | 230.1 | 251.9 | 228.81 | +35.45 |
| 2 | [135..202) sky | 222.6 | 233.9 | 252.2 | 232.79 | +29.63 |
| 3 | [202..270) sky | 222.1 | 233.5 | 251.9 | 232.39 | +29.89 |
| 4 | [270..337) sky | 225.2 | 235.9 | 252.7 | 234.82 | +27.50 |
| 5 | [337..405) sky | 220.0 | 232.3 | 251.4 | 231.04 | +31.41 |
| 6 | [405..472) sky/horizon | 209.7 | 224.2 | 243.5 | 222.50 | +33.80 |
| 7 | [472..540) foliage line | 144.6 | 162.4 | 146.6 | 157.44 | **+2.09** |
| 8 | [540..607) shoreline green | 126.2 | 147.0 | 99.1 | 139.09 | **−27.05** |
| 9 | [607..675) water | 61.7 | 84.8 | 81.4 | **79.62** | +19.71 |
| 10 | [675..742) water | 52.7 | 72.2 | 72.1 | **68.06** | +19.43 |
| 11 | [742..810) water | 53.6 | 72.1 | 71.5 | 68.09 | +17.89 |
| 12 | [810..877) water | 53.8 | 72.1 | 71.6 | 68.16 | +17.79 |
| 13 | [877..945) water | 53.8 | 72.1 | 71.7 | 68.22 | +17.91 |
| 14 | [945..1012) water | 54.7 | 72.7 | 72.0 | 68.79 | +17.39 |
| 15 | [1012..1080) bottom (water) | 54.5 | 72.7 | 72.1 | 68.77 | +17.64 |

Observations:

- The **top six bands are bright and bluish** (luminance 221–234, B−R +27…+40).
- **Band 7** is the foliage transition (mean 144.6, 162.4, 146.6; B−R ≈ +2.1).
- **Band 8** is the only band with **negative (B−R) = −27.05** — the shoreline
  strip where green terrain dominates and blue is suppressed.
- **Bands 9–15 (the water) are now dark and flat**: luminance falls to
  **~68** and stays there, with mean ≈ **53, 72, 72** (deep teal). In the old
  image these bands ran ~145 → ~115 in luminance and 132,149,154 → 105,118,125
  in colour. The water no longer brightens monotonically toward the bottom: it
  **converges to a stable deep-water colour** after band 9, exactly the
  behaviour the corrected `water_attenuate` is designed to produce.

### 4.5 9x16 mean-RGB grid (row 0 = top of image)

```
row 0: 228,237,251 | 221,233,252 | 210,224,249 | 200,219,250 | 198,219,251 | 197,218,252 | 196,218,252 | 197,218,252 | 208,224,250 | 223,233,251 | 226,235,252 | 224,234,251 | 233,241,253 | 228,237,253 | 216,229,250 | 203,221,250
row 1: 233,240,251 | 230,238,252 | 216,229,250 | 219,231,251 | 208,224,250 | 201,221,252 | 201,221,252 | 208,224,251 | 242,246,254 | 255,255,255 | 249,251,254 | 243,247,254 | 227,237,252 | 213,228,252 | 207,225,252 | 204,223,252
row 2: 225,236,253 | 224,235,252 | 216,229,250 | 214,228,250 | 211,227,251 | 207,225,252 | 207,225,253 | 210,227,252 | 231,239,252 | 240,245,253 | 235,242,253 | 230,239,252 | 231,239,252 | 236,243,254 | 236,243,254 | 227,237,252
row 3: 202,217,231 | 217,231,251 | 216,230,252 | 218,231,251 | 219,231,251 | 217,230,251 | 215,230,252 | 215,230,251 | 222,234,252 | 210,221,235 | 159,174,180 | 214,230,252 | 215,230,252 | 211,225,245 | 216,231,252 | 216,230,252
row 4:  76, 96, 64 | 149,169,130 | 154,174,136 | 155,174,136 | 155,174,137 | 122,141,110 | 142,161,127 | 102,121, 88 | 153,173,139 | 129,149,120 | 107,125, 90 | 157,175,140 | 152,171,134 |  78, 97, 67 | 154,172,137 | 157,176,140
row 5:  65, 87, 78 |  66, 87, 80 |  63, 84, 79 |  61, 83, 79 |  59, 81, 79 |  55, 77, 77 |  54, 77, 79 |  50, 72, 74 |  55, 76, 77 |  55, 77, 79 |  52, 75, 77 |  55, 78, 80 |  61, 81, 78 |  66, 87, 79 |  76, 96, 82 |  83,103, 85
row 6:  52, 71, 69 |  54, 73, 72 |  54, 72, 72 |  53, 72, 72 |  53, 72, 72 |  52, 71, 71 |  52, 71, 72 |  52, 71, 70 |  55, 73, 72 |  57, 74, 72 |  54, 72, 71 |  56, 73, 72 |  54, 72, 72 |  52, 71, 70 |  54, 72, 72 |  53, 73, 73
row 7:  53, 71, 70 |  55, 73, 72 |  53, 72, 72 |  54, 73, 72 |  53, 72, 72 |  52, 72, 72 |  53, 72, 72 |  53, 72, 72 |  55, 73, 72 |  56, 73, 72 |  56, 73, 72 |  54, 72, 71 |  54, 72, 71 |  53, 71, 71 |  53, 71, 71 |  53, 71, 72
row 8:  57, 74, 72 |  57, 74, 72 |  55, 73, 72 |  54, 72, 72 |  52, 71, 71 |  52, 71, 72 |  52, 72, 72 |  53, 72, 72 |  55, 73, 72 |  58, 75, 73 |  59, 75, 74 |  56, 73, 73 |  55, 73, 72 |  54, 72, 72 |  53, 72, 72 |  52, 72, 72
```

Rows 0–3 (sky) are unchanged from the previous render and row 4 (foliage) is
essentially unchanged (a few cells shifted by 1–8 units); **rows 5–8 are now
uniformly dark teal (~53, 72, 72)** where they were previously pale blue
(~120–145, 135–160, 130–167).

### 4.6 Classification grid (same 9x16 layout)

`S` = sky · `C` = cloud · `G` = green/foliage · `R` = warm/brown
· `W` = water/bluish · `.` = other

```
row 0: S S S S S S S S S S S S S S S S
row 1: S S S S S S S S C C C C S S S S
row 2: S S S S S S S S S S S S S S S S
row 3: S S S S S S S S S S S S S S S S
row 4: G G G G G G G G G G G G G G G G
row 5: G G W W W W W W G G G W G G G G
row 6: G G G W W W W W G G G G W G W W
row 7: G G W W W W W W G G G W W W W W
row 8: G G G W W W W W G G G G W G W W
```

The grid reads as a coherent outdoor composition: sky (rows 0–3), a solid
green foliage line (row 4), then a mixed shoreline/ground row (row 5) giving
way to water (rows 6–8). The dark teal water now sits close enough in value to
the dark foliage that some cells fall on either side of the `G`/`W` classifier
boundary — an honest consequence of the darker, lower-contrast water.

### 4.7 Pixel-level colour categories (full resolution, 2,073,600 px)

| Category | Pixels | Share |
|---|---|---|
| Very dark (lum < 60) | 51,860 | 2.501% |
| Green foliage (G > R+15, G > B+15) | 179,565 | 8.660% |
| Warm / bark (R > G) | 0 | 0.000% |
| Cloud (lum > 245) | 154,883 | 7.469% |
| Sky (bluish, bright) | 1,298,003 | 62.597% |
| Other (mostly dark water/ripple) | 389,289 | 18.774% |

Overlapping dominance masks: blue-dominant (B>R, B≥G) **1,349,211 (65.066%)**;
green-dominant (G>R, G>B) **620,347 (29.916%)**; warm-dominant (R>G, R>B)
**321 (0.015%)**.

The **"other" share grew from 0.47% to 18.77%** because the darkened water no
longer satisfies the "bright bluish" sky test; those pixels are counted as
"other" rather than being forced into a category. Green foliage (8.66%) and
cloud (7.47%) are unchanged.

### 4.8 Composition checks

| Check | Result |
|---|---|
| Top band bright & bluish (B ≥ R, lum > 80) | **PASS** (B−R = +39.75, lum 224.49) |
| Bottom bands darker than sky | **PASS** (68.77 < 224.49) |
| Clearly GREEN regions present | **PASS** (row 4 fully green; 8.66% of pixels) |
| Clearly BROWN/bark regions present | **FAIL** (see §5 — bark still not visible) |
| Non-degenerate (lum stddev > 10, distinct > 5000) | **PASS** (78.39, 7,848) |

---

## 5. Qualitative assessment (measurement-driven only)

- **Sky (rows 0–3, top ~44% of the frame).** Bright (lum 221–234) and
  distinctly bluish (B−R between +27 and +40), essentially identical to the
  previous render. Cloud cells appear as near-white patches (luminance 240–255)
  in rows 1–2, columns 8–11; the brightest cell is `[row 1 col 9] = 255,255,255`
  (a clipped cloud highlight). Cloud coverage (lum > 245) is **7.47%**.
- **Foliage line (row 4, ~11% of frame height).** The only warm/green band:
  mean luminance drops to 157 and (B−R) falls to **+2.09**. Row 4's cells are
  uniformly green (mean ≈ 130, 152, 120), with darker edge cells
  (`76,96,64` and `78,97,67`) reading as tree silhouettes at the frame borders.
  Green foliage is **8.66%** of all pixels. The clipped highlight at
  `[row 3 col 10] = 159,174,180` is the horizon glint.
- **Shoreline strip (band 8, y 540–607).** Mean **126.2, 147.0, 99.1**
  (B−R = −27.05): the single band where green terrain dominates. Unchanged from
  the previous render.
- **Water (bands 9–15, bottom ~44%).** All cells are dark and teal
  (R ≈ 53, G ≈ 72, B ≈ 72; B−R ≈ +18), with luminance converging to **~68**
  and staying flat. This is the corrected Beer-Lambert behaviour: the refracted
  ray is attenuated by `exp(-absorption_c · depth)` with absorption
  (0.45, 0.12, 0.06), so red is absorbed far faster than blue/green, and the
  result blends toward `deep_color = (0.02, 0.10, 0.16)` as depth grows. The
  water now reads as **deep, dark teal** rather than washing out toward the
  bright sky — a visible, intended change from the previous image.
- **Dark silhouettes.** 2.50% of pixels are very dark (lum < 60); in the old
  image this was 3.01%, so slightly fewer pixels are now near-black (the old
  water's brightest reflections no longer clip).

Overall the frame remains a coherent outdoor composition — bright bluish sky
with clouds on top, a green foliage/horizon line across the middle, and a large
dark-teal water surface filling the lower half — now with markedly stronger
tonal separation between sky and water.

**Honest caveat on "brown":** the composition check for *brown/bark* still
**fails**, exactly as before. Across the entire 1920x1080 frame only
**321 pixels (0.015%)** have R > G, and the maximum (R−G) anywhere is **+3**;
no pixel has R > G+20. The tree trunks/branches contribute essentially **no
visible brown** — they are occluded by, or blended into, the dark green foliage
silhouettes at this camera framing and distance. The trees are present (the
green foliage and dark silhouette pixels), just not as distinct brown trunks.
This is reported as a framing limitation, not hidden.

---

## 6. Recent improvements and their measurable effect

The following hardening changes were made since the previous render; each is
summarised with the measured effect it produced.

### 6.1 Corrected Schlick Fresnel with total internal reflection (TIR)

`src/render.c` now evaluates Schlick's approximation at the **refracted**
cosine `cos(theta_t)` instead of the incident cosine, and computes
`sin²(theta_t) = eta²·(1 − cos²(theta_i))` explicitly. When `sin²(theta_t) > 1`
the path is flagged as total internal reflection and the full reflected energy
is used (`F = 1`), with `vec3_refract()` guaranteed not to fail.

*Measured effect:* the previous code produced a non-physical discontinuity at
the critical angle (the water's glancing reflections and the near-horizon
region were over-bright). The new water's near-horizon band (band 9) is
**79.62** luminance versus **144.82** before — a 45% reduction — and the water
no longer brightens toward the viewer. This is the single largest contributor
to the frame's lower mean luminance (−27.1) and higher contrast (+22.6 sd).

### 6.2 Corrected water attenuation via `water_attenuate`

`src/material.c` gained `water_attenuate()`, which applies true per-channel
Beer-Lambert transmittance `T_c = exp(−absorption_c · depth)` and blends the
inner colour toward `deep_color`:

```
out_c = inner_c · T_c + deep_color_c · (1 − T_c)
```

`src/render.c` now uses the **actual exit-hit parameter `h2.t`** as the
propagation distance (the ray is intersected exactly once and the hit reused),
instead of the previous fixed multiplicative tint. The old
`water_depth_tint()` is retained only as a thin deprecated wrapper.

*Measured effect:* the water region changed from mean **(121.9, 136.1, 145.7)**
to **(61.4, 80.4, 80.8)** — roughly **50% darker** — and is now distinctly
**teal** (B−R ≈ +19.4 with a much lower floor) instead of washed-out pale blue.
The bottom bands converge to a stable deep-water colour (~53, 72, 72) rather
than continuing to brighten. This is the dominant driver of the −65% drop in
distinct-colour count (22,497 → 7,848), since the broad spray of bright
near-white water colours has been replaced by a compact dark-teal target.

### 6.3 Reduced `water_normal` finite-difference step

`src/material.c` reduced the fBm ripple central-difference step from
`0.5 / freq` (~1.43 m, a ~2.86 m span that low-passed away all high-frequency
detail) to a fixed **`e = 0.02` m**. The unit test `tests/test_water.c` confirms
the finer response: **1600/1600** samples show `|dn(0.02 m)| > 1e-5`, with
`max|dn(0.02m)| = 0.00907` and `max|dn(1m)| = 0.12627`.

*Measured effect:* finer ripple structure in the wave normals, which feeds the
Fresnel weighting above and produces the slight cell-to-cell variation visible
in the water rows of the grid (e.g. row 5 spans 50–83 in R across columns).

### 6.4 Tile-based dynamic multithreading

`src/render.c` now divides the image into **16×16 px tiles** and lets a pool of
workers claim them from a shared `atomic_int` counter
(`atomic_fetch_add`). The worker count is
`min(sysconf(_SC_NPROCESSORS_ONLN), RENDER_MAX_THREADS=64, ntiles)` and can be
overridden with `RAYTRACER_THREADS`. Each worker writes only inside its own
tile, so disjoint regions never race, and the output is **byte-identical for
any thread count** (verified in §2). The BVH traversal was also made heap-free
and plane handling corrected (`src/bvh.c`), removing the per-sample cost spike
that previously dominated.

*Measured effect:* on this 10-CPU host the renderer now uses **10 workers** and
achieves `user/real = 9.37` (**~93.7% efficiency**), versus the previous
static-band build's `user/real ≈ 4.0` on 8 workers (~50%). Combined with the
cheaper traversal, the full frame went from **448.22 s to 67.33 s — a measured
6.66x speed-up** at identical settings. (Note: this is the *combined* effect of
scheduling + BVH + shading fixes; a pure scheduler-only A/B was not isolated.)

### 6.5 Opt-in physically-based (PBR) material layer — how it works

The `Material` struct (`src/material.h`) gained four fields for an opt-in
metallic/roughness layer: `metallic` (0..1, default `0`), `roughness` (0..1,
default `0`), `emissive` (linear RGB, default `0 0 0`) and an integer `pbr` gate
(default `0`). Every field zero-initialises to its legacy-neutral default, so
existing materials — and `scenes/default.scene` — are unaffected.

**The `pbr` opt-in gate.** `pbr = 0` (the default) keeps the direct-sun term as
the unchanged Blinn–Phong expression. In `src/render.c` the two call sites
(`material_shade_local`) are kept **textually intact inside an `else` branch**;
the only operation added to the legacy path is an integer comparison of `m->pbr`,
which performs no floating-point work. Consequently the default render — and
this frame — is **byte-identical** to before. All new floating-point work is
confined to `material_shade_pbr`, reachable only when `pbr == 1`.

**The microfacet model (`material_shade_pbr`, `src/material.c`).** With
`N`, `L`, `V` the unit surface normal, light and view directions and
`Lc` the incoming light colour, `pbr = 1` replaces the Blinn–Phong lobe with a
Cook-Torrance microfacet BRDF:

- **GGX / Trowbridge-Reitz** normal distribution `D`, with perceptual roughness
  remapped to `alpha = max(roughness², 1e-4)` (the `1e-4` floor keeps `D` finite
  at the mirror limit, where it peaks at `1/π`).
- **Smith-Schlick-GGX** visibility `G`, using perceptual roughness
  `k = (roughness + 1)² / 8` (clamped to `[0, 1]`), for the light and view terms.
- **Schlick Fresnel** `F = fresnel_schlick_rgb(N·V, F0)`, with
  `F0 = mix(vec3(0.04), albedo, metallic)` per channel — a flat 4 % dielectric
  floor that rises to the full `albedo` for a pure conductor.
- **π-consistent Lambert** diffuse `f_diff = (1 − metallic) · albedo / π`, so a
  pure metal (`metallic = 1`) has no diffuse lobe.
- The returned radiance is `L_o = Lc · max(N·L, 0) · (f_diff + D·G·F / (4·N·L·N·V))`.

The model is **energy-conserving**: for `metallic ∈ [0, 1]` and `F0 ∈ [0, 1]`
the combined BRDF integrated over the hemisphere never exceeds 1 — the
`(1 − metallic)` diffuse weight and the `1/π` factor keep the reflected radiance
bounded by the incident `Lc` scaled by `N·L`. (`tests/test_pbr.c` asserts a
maximum hemispherical reflectance of 0.748 ≤ 1.) Like the legacy path,
`material_shade_pbr` is pure — no globals, no I/O — so the renderer's thread/tile
determinism and byte-identity-across-thread-counts guarantee are preserved.

**Emissive term.** `emissive` (linear RGB, allowed to exceed 1) is added **once
per shaded hit**, not per light, and is independent of `reflectivity` /
`transparency`. In `src/render.c` it is appended at the end of `trace_hit`,
guarded by both `pbr = 1` and a non-zero colour, so a zero `emissive` (or
`pbr = 0`) leaves the return expression byte-identical. The `type = emissive`
preset is a warm-white lamp (`emissive 1.0 0.85 0.65`); scale it above 1 for a
brighter source.

**Named presets.** Fifteen lower-case `type = <name>` keywords are recognised
(`src/scene_desc.c`): the legacy `water`, `opaque`, `glass`, the seven polished
conductors `gold`, `copper`, `silver`, `aluminum`, `iron`, `chrome`, `brass`,
the four dielectrics `plastic`, `rubber`, `ceramic`, `diamond`, and the
`emissive` lamp. Each preset is the *base* value set and explicit keys in the
same block override it (e.g. `type = gold` + `roughness = 0.30` = brushed gold).
The canonical writer round-trips a material that exactly equals a preset back to
`type = <name>`. See `docs/scene_format.md` §4.3 and
`docs/research_material_reference.md` for the full value tables.

*Measured effect on this frame:* **none** — the PBR layer is opt-in and the
default scene sets `pbr = 0` on every material, so `output/scene.bmp` is
unchanged (byte-identical). The feature is exercised by
`scenes/example_materials.scene` and `scenes/example_presets.scene`, and by
`tests/test_pbr.c`, `tests/test_material_presets.c` and
`tests/test_integration_pbr.c`.

### 6.6 Glossy (roughness-blurred) reflections — how it works

Earlier revisions noted that the recursive mirror reflection (`reflectivity`)
was a sharp Whitted ray that `roughness` did **not** blur. That gap is now
closed by an opt-in glossy path in `src/material.{h,c}` and `src/render.c`.

**Gate.** The reflection block in `trace_hit` takes the glossy path only when
**all** of

- `pbr != 0` (the metallic/roughness BRDF is active),
- `reflectivity > 0` (the mirror pass is switched on), and
- `roughness > GLOSSY_ROUGHNESS_EPSILON` (`1e-6`),

hold. Otherwise it casts the legacy single `reflect(dir, N)` mirror ray. Because
the default scene sets `pbr = 0` (and `reflectivity = 0`) everywhere, the extra
floating-point work is never reached and this frame is byte-identical.

**Sampler (`material_sample_glossy_dir`).** When the gate is open the renderer
casts `GLOSSY_REFLECTION_SAMPLES` = **16** recursive rays, each importance
sampled from the material's GGX lobe and averaged. The lobe width is the single
shared convention `material_roughness_to_alpha(roughness) = max(clamp01(roughness)²,
PBR_ALPHA_MIN)`, i.e. `alpha = max(roughness², 1e-4)` — exactly the `alpha` the
PBR shading path uses for its GGX normal distribution, so the highlight and the
reflection blur stay consistent. A `roughness ≈ 0` metal (`<= 1e-6`) is treated
as a perfect delta mirror and takes the exact legacy path, guaranteeing the
mirror fallback is not merely "close" but identical.

**Determinism.** The 16 lobe samples reuse the renderer's integer-hash PRNG
(seeded from the pixel/sample indices and dedicated channels), so the glossy
result is a pure function of the inputs — no `rand()`, no clock — and remains
byte-identical across thread counts and runs.

*Measured effect on this frame:* **none** — the default scene keeps every
material at `pbr = 0`, so the reflection path is the unchanged sharp mirror. The
feature is exercised by `scenes/example_glossy.scene` (a gold/chrome/copper
roughness ramp at `reflectivity = 0.8`) and by `tests/test_glossy.c` /
`tests/test_integration_glossy.c`.

### 6.7 Emissive PBR primitives as area lights — how it works

Earlier revisions noted that `emissive` made a surface self-lit but did **not**
illuminate other geometry. Emissive PBR primitives now act as sampled **area
lights**, implemented across `src/scene.{h,c}`, `src/material.{h,c}` and
`src/render.c`.

**Collection (`scene_collect_emissive_lights`).** At scene-build time, up to
`SCENE_MAX_EMISSIVE_LIGHTS` = **8** emissive PBR *spheres*, in file order, are
promoted to a compact `EmissiveLight` list (`Scene.emissive_lights` /
`Scene.emissive_light_count`). A scene with no emissive PBR material has
`emissive_light_count == 0`, and the renderer's emissive block is gated on
`scene->emissive_light_count > 0`, so such scenes render byte-identically to
before.

**Sampling (`light_sphere_sample_dir`).** For each lit hit, the renderer samples
every collected lamp's **visible cap** as a cone of directions: with `w` the
direction from the shading point to the lamp centre and
`cos(alpha_max) = sqrt(1 - (R/dc)²)` the cosine of the cone half-angle subtended
by the sphere, it draws `EMISSIVE_LIGHT_SAMPLES` = **16** uniform-in-solid-angle
directions

```
cos_alpha = 1 - u1 * (1 - cos_alpha_max)
w_i       = normalize(cos_alpha * w + sin_alpha * (cos(phi)*t + sin(phi)*b))
```

casts one shadow ray per sample, and shades each unoccluded sample with the
lamp's `emissive` colour via `material_shade_local` / `material_shade_pbr`. The
result (`emissive_direct` in `src/render.c`) therefore falls off with distance
and with the lamp's apparent (solid-angle) size, and — because the light is
coloured — tints (colour-bleeds onto) the receiving surfaces. Degenerate inputs
(`u1 <= 0`, `cos_alpha_max` outside `[0, 1]`, zero `w`) are handled safely.

**Determinism and threading.** The cone samples derive only from the pixel/sample
indices through the renderer's hash PRNG, so the direct term is deterministic and
byte-identical across thread counts.

*Measured effect on this frame:* **none** — the default scene has no emissive PBR
material, so `emissive_light_count == 0` and the block is skipped. The feature is
exercised by `scenes/example_emitters.scene` (a warm amber and a cool blue lamp
lighting a dim dusk scene) and by `tests/test_emissive_lights.c` /
`tests/test_integration_emissive.c`.

### 6.8 Adaptive sampling (ON by default; opt out with `--no-adaptive`) — how it works

The renderer gained a second entry point `render_image_ex` (declared in
`src/render.h`) driven by a `RenderParams { samples_per_pixel, max_depth,
adaptive, adaptive_max_spp, adaptive_tau }` struct. Adaptive sampling is
**enabled by default** and selected from the CLI via `--adaptive` (now a redundant
no-op) / `--no-adaptive` (the opt-out); see `docs/scene_format.md` §10. The existing
`render_image` and the fixed per-pixel sample loop are untouched. Adaptive sampling
applies to the **Whitted renderer only** — the path tracer ignores it.

**Byte-identity guarantee.** When `params->adaptive == 0` (i.e. `--no-adaptive`),
`render_image_ex` delegates **verbatim** to `render_image`, so the fixed path
(arithmetic, summation order, gamma/quantize) is identical to before and the render is
byte-identical for any thread count and any `samples_per_pixel`. Combining
`--no-pathtrace --no-adaptive` therefore reproduces the exact legacy fixed-spp Whitted
output of previous releases.

**Metric.** Adaptive refinement accumulates samples and stops a pixel once the
**relative standard error of the mean luminance** falls below the tolerance
`tau`: luminance is Rec. 709 (`0.2126 R + 0.7152 G + 0.0722 B`), and the running
mean/standard error are compared against `tau * mean`. `tau` defaults to `0.02`
(when `adaptive_tau <= 0`) and the cap `N_max` defaults to `4 * n0` (when
`adaptive_max_spp <= 0`, with `n0 = samples_per_pixel`). Flat pixels converge
after the base count `n0`; noisy / high-contrast pixels keep refining up to
`N_max`. The diagnostics `render_last_total_samples()` and
`render_last_max_samples()` expose the summed and largest per-pixel counts so the
refinement and the cap can be verified.

**Determinism.** The refinement decisions and the extra samples all use the same
integer-hash PRNG as the fixed path, so an adaptive render is reproducible and
byte-identical across thread counts.

*Measured effect on this frame:* **none** — this frame was rendered with
`--no-adaptive`, so `render_image_ex` took the fixed path. The feature is exercised
by `scenes/example_adaptive.scene` (flat floor/sky regions kept at the base count
while overlapping spheres, thin needles/blades, hard shadows and strong depth of
field are refined) and by `tests/test_adaptive.c` /
`tests/test_integration_adaptive.c`.

### 6.9 Render progress indicator — detailed behaviour

The renderer draws a single, self-overwriting progress line on **stderr**,
implemented entirely in `src/render.{h,c}` and wired up by `src/main.c`.

**Format and location.** The line looks like

```
render:  42% [00:12<00:16, 1.2 Mpx/s]
```

i.e. completion **percentage**, **elapsed** time (`MM:SS`), estimated **time
remaining** (ETA, `MM:SS`), and **throughput**. It is written **exclusively to
stderr**, so stdout — which `main.c` uses for the BMP/PPM path and the summary —
is never polluted. Each redraw begins with a carriage return `\r`, so the line
overwrites itself in place; the throughput unit is chosen adaptively
(`Mpx/s` ≥ 1e6, `kpx/s` ≥ 1e3, else `px/s`, falling back to `rows/s` for internal
callers that did not supply a pixel budget).

**Throttle.** Redraws are bounded to at most one per
`RENDER_PROGRESS_MIN_INTERVAL` = **~120 ms** (`0.12 s`), measured on
`CLOCK_MONOTONIC`, so reporting a work unit costs at most a couple of atomic
operations plus one clock read and is never a measurable render cost.

**Monotonicity and the final line.** A late report winner may carry a completed
count smaller than one already printed (the counters are unique but the winners
race), so the meter never moves backwards: the printed count is clamped up to the
last printed value, and to the expected total. A `0%` line is emitted immediately
at the start, and completion **always** emits a final `100%` line followed by a
newline (a `done < 0` "finish" report bypasses the throttle), so the terminal is
left on a clean, complete line regardless of timing.

**Both builds.** Progress is enabled by default and works in **both** the
single-threaded and the `-DUSE_PTHREADS` builds:

- *Single-threaded:* a plain clock-based throttle on the completed-row counter is
  sufficient.
- *Threaded:* progress is derived from an **atomic completed-tiles counter**
  (`atomic_fetch_add` on `tiles_done` as each tile finishes). A single-writer CAS
  gate (`g_prog_emitting`, a test-and-set lock held only while a worker is inside
  `render_progress_emit`) makes the "at most one thread writes to stderr"
  property unconditional, so the line can never interleave even if a write blocks
  longer than the throttle interval. An epoch counter lets a worker that loses
  the epoch race discard its stale win without writing.

**Silencing.** Setting the environment variable `RAYTRACER_NO_PROGRESS` to any
non-NULL value (`RAYTRACER_NO_PROGRESS=1`) disables the meter in **both** builds;
`main.c` also exposes a matching `--no-progress` flag that sets the same variable.
When silenced, no progress text is written to stderr at all.

**No effect on output.** The meter is strictly diagnostic: it never reads or
writes the pixel buffer, and the renderer's arithmetic, summation order and
determinism are unchanged. Progress therefore never affects the pixel data, the
byte-identity guarantee, or the reported timing (the timer brackets the render
call, not the progress I/O).

*Measured effect on this frame:* the progress meter was **silenced** for the
recorded run (`RAYTRACER_NO_PROGRESS=1`, see §1/§8) so the log stayed clean; it
does not change `output/scene.bmp`. The behaviour is covered by
`tests/test_progress.c` and `tests/test_render_threads.c`.

### 6.10 Unbiased path tracer (`--pathtrace`) — the DEFAULT renderer

The renderer's **default** rendering mode is an **unbiased, unidirectional path
tracer** that replaces the Whitted-style `trace_hit` recursion with a Monte-Carlo
integration of the light-transport equation. It is selected from the CLI via
`--pathtrace` (now a redundant no-op) / `--no-pathtrace` (the opt-out); see
`docs/scene_format.md` §10. Without `--no-pathtrace` the path tracer runs; passing
`--no-pathtrace` restores the existing recursive renderer.

**Byte-identity guarantee.** When `--no-pathtrace` is given, the renderer takes the
existing Whitted path verbatim (arithmetic, summation order, gamma/quantize). With
`--no-pathtrace --no-adaptive` the legacy fixed-spp Whitted output of previous releases
is reproduced **byte-identically**. Selecting the path tracer does not alter the legacy
path.

**Orientation (vertical-flip bug fixed).** The path tracer's output orientation now
matches the Whitted renderer: the image is right-side up, **top = sky, bottom =
ground**. The earlier vertical-flip discrepancy is fixed; no user action is required
beyond rebuilding.

**Iterative bounce loop.** Rather than recursing, the path tracer runs an
explicit **iterative loop** over bounces. It carries a per-path *throughput*
(initially 1) and accumulates `throughput * radiance` at each hit, marching the
ray to the next bounce until either the loop reaches the CLI `--depth` limit
(default `6`) or the path is terminated. This keeps stack usage constant and
independent of `--depth`.

**Material sampling.** At each bounce the outgoing direction is chosen from the
material's BSDF:

- **diffuse** → **cosine-weighted hemisphere** sampling (the estimator divides by
  the cosine-weighted pdf, so the diffuse albedo is carried directly in the
  throughput).
- **mirror / glass / water** → **Schlick-Fresnel** reflect/refract split using the
  material's `ior` / `transparency`; the reflected and refracted branches are
  weighted by the Fresnel term, and the water/glass medium applies the same
  Beer–Lambert transmission as the legacy path.
- **pbr** → a **mixed diffuse + specular lobe**, choosing between the two in
  proportion to `metallic`/`roughness`, with the specular direction importance
  sampled from the material's **GGX** lobe (the same
  `material_roughness_to_alpha` convention as the glossy reflection path, §6.6).

**Next Event Estimation (NEE).** To reduce variance the path tracer casts an
explicit **shadow ray toward the sun** at each bounce and adds its direct
contribution, and also samples the collected **emissive primitives as area
lights** at every bounce (the same `scene_collect_emissive_lights` set used by the
legacy area-light term, §6.7). The sun NEE is applied on the **first bounce only**
because the sky model is a smooth sun *glow* with no discrete sun disk to sample
at deeper vertices; emissive area lights are sampled at every bounce.

**Russian Roulette.** Paths are terminated stochastically after roughly 3–4
bounces using **Russian Roulette**: the continuation probability is derived from
the current throughput, and the surviving throughput is divided by that
probability. This bounds path length without introducing bias — the estimator
remains **unbiased**.

**Determinism and thread safety.** Every random decision (direction sampling,
Fresnel choice, Russian Roulette) comes from the renderer's **hash-based PRNG
seeded per pixel and per sample** — the same integer-hash scheme as the fixed
path — with no global mutable state. As a result the path-traced output is
**deterministic for any thread count**, preserving the byte-identity-across-threads
guarantee documented in §2.

**Reused infrastructure.** The path tracer reuses the existing **BVH** (binned
SAH), the **camera** (including thin-lens depth of field), the **pthread tile
parallelism** (dynamic tile scheduling; the default build is threaded), and the
**tone mapping / gamma**
pipeline. It works in **both** the serial (`make serial` / `make no-threads`) and the
threaded (`make`, the default) builds; the worker count is set by `RAYTRACER_THREADS` or
auto-detected. In the serial build `--threads` is a no-op, exactly as for the
legacy path, and `--no-threads` forces single-threaded rendering in either build.

**Progress meter.** The stderr progress indicator (§6.9) works in path-trace mode
too, reporting percentage/rate/ETA, and is silenced by `RAYTRACER_NO_PROGRESS=1` or
`--no-progress`.

*Measured effect on this frame:* **none** — this frame was rendered with
`--no-pathtrace`, so the Whitted-style renderer ran and `output/scene.bmp` is
unchanged (byte-identical). The feature is exercised by
`scenes/example_pathtrace.scene` and by the path-trace tests under `tests/`.

---

## 7. Known limitations

- **Shadows were hard in this frame.** The renderer now supports sun-disk
  sampling for soft shadows via the `sky.sun_radius` key (see
  `docs/scene_format.md` §4.2), but the default scene used for this render keeps
  `sun_radius = 0`, so this frame still uses a single binary shadow ray toward
  the directional sun. Soft shadows are opt-in per scene.
- **Global illumination / path tracing — now the DEFAULT renderer.** The renderer
  path-traces by default, adding general diffuse inter-reflection and coloured bounce
  light bounded by `--depth`. The legacy **Whitted** shading path — ambient (hemisphere
  approximation from the sky parameters) + one sun direct term (hard or sun-disk soft;
  Blinn–Phong, or the Cook-Torrance microfacet model when a material sets `pbr = 1` — see
  §6.5) + the emissive area-light term (§6.7, only when the scene has emissive PBR
  spheres) + recursive mirror reflection and Fresnel refraction, with no diffuse
  inter-reflection, no ambient occlusion and no path-traced bounce lighting — is now the
  **opt-out**, selected with `--no-pathtrace`. Arbitrary user-declared area lights beyond
  the collected emissive spheres remain unavailable. *(Arbitrary user-declared area
  lights are deferred as out of scope.)*
- **PBR is direct-light only.** The opt-in metallic/roughness path (§6.5) shades
  the direct sun term and adds `emissive` once per hit; it has no image-based
  environment lighting and no multiple-scattering/energy-compensation term. The
  diffuse lobe now uses `kD = (1 − F)(1 − metallic)`, so the model **is**
  energy-conserving (§6.5). The recursive mirror reflection (`reflectivity`) is a
  sharp Whitted reflection **by default**, but an opt-in glossy path (§6.6) now
  blurs it with `roughness` when `pbr = 1`, `reflectivity > 0` and
  `roughness > 1e-6`; a `roughness ≈ 0` metal keeps the exact mirror.
  `emissive` makes a surface self-lit **and**, in the opt-in area-light path
  (§6.7), also illuminates other geometry: emissive PBR spheres are collected
  (up to 8) and cone-sampled as coloured area lights. *(Image-based environment
  lighting remains deferred as out of scope.)*
- **Procedural textures only; flat in this frame.** Materials can now carry a
  world-space procedural texture (`texture = checker|stripes` with
  `texture_scale`/`texture_color_a`/`texture_color_b`; see `docs/scene_format.md`
  §4.3), but the default scene leaves `texture = none`, so this frame's materials
  are flat per-primitive albedos. There are still no UV parameterisation and no
  image/bitmap textures.
- **Sky/cloud model is a hand-tuned approximation.** The sky is a zenith→horizon
  colour gradient plus a power-law sun glow; clouds are an fBm noise field
  sampled on a flat layer at `cloud_height = 120`, thresholded by
  `coverage`/`softness`/`sharpness` (5 octaves). It is not physically based and
  does not conserve energy; cloud highlights clip to 255 (see the
  `255,255,255` cell).
- **Water is a thin bounded box, not volumetric water.** The pond is a box whose
  top face sits at `y = 0.02` (2 cm above the ground to avoid z-fighting) with
  half-extents 45 x 0.03 x 45. Attenuation now uses the true refracted path
  length through the box, but the medium is still a single homogeneous slab, and
  waves remain a normal perturbation only (no displaced geometry).
- **Bark/trunk visibility.** As documented in §5, brown bark is still
  effectively invisible at the default framing (max R−G = 3, 321 pixels with
  R > G). The camera distance / foliage density occludes the trunks.
- **Performance characteristics.** Cost is proportional to
  `width * height * samples_per_pixel` (plus a small, roughly constant per-sample
  BVH traversal cost). Measured throughput is **~493k samples/s on 10 threads**
  (~30.8k pixels/s at 16 spp); a 1920x1080 @ 16 spp frame takes **~67 s**.
  Time scales linearly with resolution and with sample count. This frame used the
  **fixed** Whitted path (`--no-pathtrace --no-adaptive`); with the default adaptive
  sampler (§6.8, Whitted only) the *actual* sample
  count varies per pixel between `n0` and `N_max`, so cost is bounded by
  `width * height * N_max` but is typically lower where flat regions converge
  early. The stderr progress meter (§6.9) reports the completion rate and ETA and
  has no effect on cost or output.
- **Single static frame.** The renderer fixes `time = 0`, so the wave normal
  perturbation is evaluated at a single instant; there is no animation or motion
  blur. (Depth of field *is* supported via the thin-lens `camera.aperture` /
  `camera.focus_distance` keys, but this frame uses the default pinhole camera.)

---

## 8. Reproduction

```sh
make clean && make            # threaded build is the default
RAYTRACER_NO_PROGRESS=1 ./raytracer --width 1920 --height 1080 \
    --samples 16 --depth 6 --seed 1337 --no-pathtrace --no-adaptive \
    --out output/scene.bmp
```

`--no-pathtrace --no-adaptive` reproduces the exact legacy fixed-spp Whitted frame
documented in §1 (byte-identical to previous releases). Omit both flags for the new
default: path tracing (global illumination) with adaptive sampling, threaded.

The scene build and the renderer's per-pixel sampling are both deterministic
pure functions of the seed and pixel coordinates (no `rand()`, no `time()`), so
the frame is reproducible bit-for-bit across runs and across thread counts.
`make test` (all `tests/*.c`) passes with **exit code 0** after this render.
