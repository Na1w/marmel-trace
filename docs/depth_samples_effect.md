# `--samples` and `--depth` in the path tracer — empirical effect report

**Task:** t-119
**Scope:** verify end-to-end that `--samples` (Monte-Carlo samples per pixel) and
`--depth` (bounce cap) actually control the path tracer; fix a real bug if one exists.
**Result:** **both flags work correctly. No bug found. No source change made.**
The user's observation is a *flag confusion* plus a *diminishing-returns* effect
(see §6/§7).

---

## 1. Build

```
$ make clean && make
BUILD_EXIT=0
NO WARNINGS/ERRORS          # grep -iE "warning|error" over the full log: empty
```

```
$ make serial
SERIAL_EXIT=0
SERIAL: NO WARNINGS/ERRORS
```

Toolchain: `cc` = Apple clang on macOS aarch64, flags
`-std=c11 -O2 -Wall -Wextra -DUSE_PTHREADS -pthread` (threaded default),
`make serial` = `THREADS=0` (no pthread). Both builds are warning-free.

```
$ make test
TEST_EXIT=0
... summary: 30 passed, 0 failed ... ALL TESTS PASSED
```
(All 30 test binaries green, including `test_pathtrace`, `test_pathtrace_sampling`,
`test_integration_pathtrace`, `test_render_threads`.)

---

## 2. Method / tooling

* Renders were written as **PPM (P6)** via `--out *.ppm`, so pixels are uncompressed
  and can be compared byte-for-byte and per-pixel.
* `RAYTRACER_NO_PROGRESS=1` used throughout so the stderr progress meter never
  perturbs the runs.
* Metric helper `tools_t119_metrics.py` (added by this task, ~120 lines, pure Python
  stdlib):
  * `noise <a.ppm>` → **adjacent-pixel MAD**: mean absolute luma difference between
    horizontally adjacent pixels. Random Monte-Carlo noise inflates this value;
    more samples → lower value.
  * `diff <a.ppm> <b.ppm>` → **mean absolute pixel difference** (per RGB channel).
  * `stats <a.ppm>` → mean luminance.
  * `sha256` of the raw file.

All commands below were run from the repo root with
`RAYTRACER_NO_PROGRESS=1` exported.

---

## 3. `--samples` effect (SPP = Monte-Carlo samples per pixel)

Fixed geometry: `--width 160 --height 90 --depth 4`. Only `--samples` varied.

| scene | `--samples` | SHA-256 (first 32) | adjacent-pixel MAD ↓ | mean luma |
|---|---|---|---|---|
| default (embedded) | 4  | `a8b12035fd6983f7cc4a82d4d4579a08` | **11.2712** | 228.7355 |
| default (embedded) | 64 | `45defe2ab8fec5eda8e297ffb05827f6` | **2.7441**  | 233.0974 |
| `scenes/example_pathtrace.scene` | 4  | `dbb56fd0fc41593bdbcba5db9f6dba1a90` | **6.3181** | 136.1796 |
| `scenes/example_pathtrace.scene` | 64 | `d36a53d3a4532335d4f488ba19b382892f` | **2.4603** | 136.5239 |

Full SHAs:
```
a8b12035fd6983f7cc4a82d4d4579a088691efc77b5cafea47941199b87b0db8  def_s4.ppm
45defe2ab8fec5eda8e297ffb05827f63630fd6ada2ecacb564a70d3502af7fb  def_s64.ppm
dbb56fd0fc41593bdbcba5db9f6dba1a90f1f3174196fdff6e7a54f0dc34b75c  ex_s4.ppm
d36a53d3a4532335d4f488ba19b382892feb8e82145987049320c38c50fba1ef  ex_s64.ppm
```

**SHA-256 differ** in both scenes (4 spp ≠ 64 spp). **Noise falls**:
* default scene: 11.2712 → 2.7441 (×0.243, ~4.1× less noisy)
* example_pathtrace: 6.3181 → 2.4603 (×0.389, ~2.6× less noisy)

Mean absolute difference between the two renders:
* default scene: **6.9197** per channel
* example_pathtrace: **4.0376** per channel

Full SPP sweep (`--depth 4`), adjacent-pixel MAD (noise proxy), mean luma in brackets:

| spp | default scene | example_pathtrace |
|----:|---:|---:|
| 1   | 34.3683 (212.50) | 11.4940 (134.97) |
| 2   | 19.4182 (223.69) |  8.5721 (135.79) |
| 4   | 11.2712 (228.74) |  6.3181 (136.18) |
| 8   |  7.3697 (230.91) |  4.7386 (136.45) |
| 16  |  4.8223 (232.18) |  3.6554 (136.45) |
| 32  |  3.4923 (232.79) |  2.9261 (136.49) |
| 64  |  2.7441 (233.10) |  2.4603 (136.52) |
| 128 |  2.2828 (233.26) |  2.1317 (136.52) |

Monotone decreasing noise, converging mean luma → `--samples` is the quality/noise
knob and it is plumbed end-to-end. **`--samples` WORKS.**

---

## 4. `--depth` effect (bounce cap)

Fixed geometry: `--width 160 --height 90 --samples 64`. Only `--depth` varied.

| scene | `--depth` | SHA-256 (first 32) | mean luma ↑ |
|---|---|---|---|
| default | 0 | `637aca21e7911cf2b1c4ff2ff0bc114e` | 154.0171 |
| default | 1 | `cd6b450d492dea31ff9832aa77d294ed` | 229.3842 |
| default | 4 | `45defe2ab8fec5eda8e297ffb05827f6` | 233.0974 |
| example_pathtrace | 0 | `6b76c0d7e3e2bc920f0252cbd8fe7c4d` |  91.2476 |
| example_pathtrace | 1 | `13aa288fe6b06e117a24841c5567ac3b` | 130.5462 |
| example_pathtrace | 4 | `d36a53d3a4532335d4f488ba19b382892` | 136.5239 |

Full SHAs:
```
637aca21e7911cf2b1c4ff2ff0bc114e605e971e685473889f1a37f2c18e997f  def_d0.ppm
cd6b450d492dea31ff9832aa77d294eda3fa06c5aa54683b02310e9d6b45589a  def_d1.ppm
45defe2ab8fec5eda8e297ffb05827f63630fd6ada2ecacb564a70d3502af7fb  def_d4.ppm
6b76c0d7e3e2bc920f0252cbd8fe7c4d3813bc9dea8d23406e1b8f6b5140d2b5  ex_d0.ppm
13aa288fe6b06e117a24841c5567ac3bd470c6edbd9689f62e5a84c752c25968  ex_d1.ppm
d36a53d3a4532335d4f488ba19b382892feb8e82145987049320c38c50fba1ef  ex_d4.ppm
```

**SHA-256 differ at every depth.** Mean absolute pixel difference between levels:

| pair | default scene | example_pathtrace |
|---|---:|---:|
| depth 0 → 1 | **75.0466** | **39.6929** |
| depth 1 → 4 | **3.1769**  | **5.9987**  |
| depth 0 → 4 | **78.2234** | **45.6916** |

Global illumination **increases with depth**: mean luminance rises monotonically
(154.02 → 229.38 → 233.10 for the default scene; 91.25 → 130.55 → 136.52 for
example_pathtrace), i.e. deeper paths pick up more indirect light and colour bleed.

Full depth sweep (default scene, `--samples 64`):

| depth | SHA-256 (first 16) | mean luma | adjacent-MAD |
|---:|---|---:|---:|
| 0  | `637aca21e7911cf2` | 154.0171 | 1.4660 |
| 1  | `cd6b450d492dea31` | 229.3842 | 4.4649 |
| 2  | `7999e48bb6809aa5` | 230.1802 | 4.1422 |
| 3  | `74412e8164c90592` | 231.9129 | 3.1718 |
| 4  | `45defe2ab8fec5ed` | 233.0974 | 2.7441 |
| 8  | `27f3eab849d3055d` | 233.9783 | 2.1579 |
| 16 | `2adea73752e34027` | 234.1162 | 2.0646 |
| 64 | `94822d084ea188e8` | 234.1267 | 2.0583 |

Depth-increment MAD (default scene, samples 64) — the diminishing-returns curve:

| increment | mean luma | MAD vs previous depth |
|---|---:|---:|
| 0 | 154.0171 | — |
| 0→1 | 229.3842 | **75.0466** |
| 1→2 | 230.1802 | 0.6798 |
| 2→3 | 231.9129 | 1.4427 |
| 3→4 | 233.0974 | 1.0544 |
| 4→5 | 233.5196 | 0.3848 |
| 5→6 | 233.7566 | 0.2320 |
| 6→8 | 233.9783 | 0.2136 |
| 8→16 | 234.1162 | 0.1284 |
| 16→64 | 234.1267 | **0.0089** |

**`--depth` WORKS.** Every depth value produces a distinct image, and indirect
light accumulates monotonically.

---

## 5. Plumbing audit (source read, nothing assumed)

| stage | file:line | finding |
|---|---|---|
| parse `--depth` | `src/main.c:363-369` | `strcmp(name,"--depth")` → `num < 0` rejected → `opt->depth = (int)num`. No clamp, no override. |
| parse `--samples` | `src/main.c:356-362` | `num < 1` rejected → `opt->samples = (int)num`. No clamp, no override. |
| default | `src/main.c:146-147` | `opt->samples = DEFAULT_SAMPLES (16)`, `opt->depth = DEFAULT_DEPTH (6)`. |
| call site | `src/main.c:602-605` | `pathtrace_render(&scene,&cam,opt.width,opt.height,opt.samples,opt.depth,rgb)` — positional order matches the header exactly (`samples_per_pixel` then `max_depth`). |
| entry validation | `src/pathtrace.c:956` | `if (samples_per_pixel < 1 || max_depth < 0) return 3;` — only rejects invalid input. |
| shared struct | `src/pathtrace.c:973-974` | `shared.spp = samples_per_pixel; shared.max_depth = max_depth;` — forwarded verbatim. |
| worker | `src/pathtrace.c:876-877` | `pt_render_region(..., sh->spp, sh->max_depth, ...)`. |
| region | `src/pathtrace.c:804` | `pt_render_pixel(..., spp, max_depth, ...)`. |
| per-pixel loop | `src/pathtrace.c:750` | `for (int s = 0; s < spp; ++s)` — the SPP loop, uses the forwarded `spp`. |
| per-pixel trace | `src/pathtrace.c:777` | `pathtrace_radiance(scene, ray, max_depth, seed_key)`. |
| serial loop | `src/pathtrace.c:1035-1036` | `pt_render_pixel(scene, cam, width, height, samples_per_pixel, max_depth, rgb_out, x, y)` — direct. |
| kernel loop | `src/pathtrace.c:579` | `for (int b = 0; b <= max_depth; b++)` — the bounce loop. |
| kernel clamp | `src/pathtrace.c:558-560` | `if (max_depth < 0) max_depth = 0;` — only normalises negatives (unreachable from the CLI, which already rejects them). |

* **No hard-coded depth** anywhere in `pathtrace.c` (grep for `PT_MAX_DEPTH` / `MAX_DEPTH` / numeric bounce bounds returns nothing).
* **No argument-order swap** — the `(samples_per_pixel, max_depth)` order is consistent at every hop.
* **No shadowed variable** — `max_depth` is a plain parameter at each level; the only assignment is the defensive `< 0` clamp.
* `scene_desc.c` contains a `max_depth` key, but it is `PLANT_MAX_DEPTH` for **tree plants** (L-system depth), unrelated to path-tracer bounces. `scenes/example_pathtrace.scene` contains no `depth`/`samples` keys at all (only in comment text).

**Plumbing is intact end-to-end; nothing drops or overrides either value.**

### Corroborating invariants

* **Determinism / thread-invariance** (default scene, samples 64, depth 4):
  `RAYTRACER_THREADS=1/2/4/8` all produced SHA `45defe2ab8fec5ed…` — byte-identical.
* **Serial == threaded**: `make serial` render of the same parameters produced the
  identical SHA `45defe2ab8fec5eda8e297ffb05827f63630fd6ada2ecacb564a70d3502af7fb`.
  So `max_depth`/`spp` reach the kernel identically in both builds (i.e. `pt_render_region`
  vs the serial row loop agree).
* **`--opt value` == `--opt=value`**: `--samples=64 --depth=4` produced the same SHA as
  the space-separated form.
* **Default depth honoured**: no `--depth` ≡ `--depth 6` (both SHA `c460b7968adb9892…`).

---

## 6. Fix

**No fix applied.** No stage ignores `max_depth` or `spp`; there is no bug to fix in
`src/pathtrace.c` or `src/main.c`, and `src/render.{h,c}` were not touched.

The only artefact added by this task is the read-only metric helper
`tools_t119_metrics.py` (used to produce the numbers above).

---

## 7. Conclusion

**Both flags work.** With exact commands:

```
# samples: same geometry, only --samples varies -> SHAs differ, noise drops
./raytracer --pathtrace --width 160 --height 90 --depth 4 --samples 4  --out /tmp/t119/def_s4.ppm
./raytracer --pathtrace --width 160 --height 90 --depth 4 --samples 64 --out /tmp/t119/def_s64.ppm
#   def_s4  a8b12035fd6983f7cc4a82d4d4579a088691efc77b5cafea47941199b87b0db8  noise MAD 11.2712
#   def_s64 45defe2ab8fec5eda8e297ffb05827f63630fd6ada2ecacb564a70d3502af7fb  noise MAD  2.7441

# depth: same geometry, only --depth varies -> SHAs differ, GI rises
./raytracer --pathtrace --width 160 --height 90 --samples 64 --depth 0 --out /tmp/t119/def_d0.ppm
./raytracer --pathtrace --width 160 --height 90 --samples 64 --depth 1 --out /tmp/t119/def_d1.ppm
./raytracer --pathtrace --width 160 --height 90 --samples 64 --depth 4 --out /tmp/t119/def_d4.ppm
#   def_d0 637aca21e7911cf2b1c4ff2ff0bc114e605e971e685473889f1a37f2c18e997f  luma 154.02
#   def_d1 cd6b450d492dea31ff9832aa77d294eda3fa06c5aa54683b02310e9d6b45589a  luma 229.38  (MAD 75.05 vs d0)
#   def_d4 45defe2ab8fec5eda8e297ffb05827f63630fd6ada2ecacb564a70d3502af7fb  luma 233.10  (MAD  3.18 vs d1)
```

* **`--samples N`** is the Monte-Carlo **samples-per-pixel (SPP)** knob — the
  **noise/quality** control. It is the `for (int s = 0; s < spp; ++s)` loop in
  `pt_render_pixel`. Increasing it lowers variance; it is *the* knob for a cleaner
  image. Verified: SHA changes and adjacent-pixel MAD falls 34.37 → 2.28 across
  spp 1 → 128.
* **`--depth N`** is the **maximum number of bounces per path** — the
  **global-illumination / indirect-light** control. It is the
  `for (int b = 0; b <= max_depth; b++)` loop in `pathtrace_radiance`. It changes
  *what is visible* (indirect light, colour bleeding), **not** the sample count.
  Verified: SHA changes at every depth and mean luminance rises monotonically
  (154.02 → 234.13).

### Why the user thought `--depth` "seems to have no effect at all"

Both effects are real, but the *perceptual* effect of `--depth` saturates very
quickly, and the user was almost certainly comparing **already-deep** renders:

* The 0 → 1 step dominates: MAD 75.05 (default) / 39.69 (example) and a large
  luminance jump — depth 1 already supplies the bulk of direct + first-bounce light.
* Beyond depth 1 the increments are tiny: 1→4 = MAD 3.18 (default) / 6.00 (example);
  4→5 = 0.38; 8→16 = 0.13; **16→64 = 0.009**.
* Russian Roulette starts at bounce 2 (`PT_RR_START_BOUNCE 2`), so with the default
  depth 6 most paths die before reaching the cap anyway. Changing `--depth` from,
  say, 6 to 8 therefore moves only a handful of samples — **visually
  indistinguishable** at normal viewing, even though the files differ byte-for-byte.

So `--depth` is not broken: its visual signature is concentrated at small depths
(0/1/2) and its high-end increments are sub-perceptual. Meanwhile `--samples`
changes visible noise immediately, which is why it "works" to the eye and
`--depth` "seems" not to.

**Recommendation for the user:** use `--samples N` to control Monte-Carlo SPP
(noise/quality); use `--depth N` to control bounce depth (indirect light) and
compare `--depth 0` vs `--depth 1` vs `--depth 4` to see its effect clearly.
`--depth` beyond ~6 with the default scene yields no visible change because
Russian Roulette already terminates most paths.

**No source bug exists. No fix was required.**
