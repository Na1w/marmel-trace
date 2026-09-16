# Independent Validation Report — Opt-in Path Tracer (`--pathtrace`)

**Report status:** Implementation gate (PARTIAL). The final full verification is a
later task; this section is written so it can be extended in place.

**Auditor:** independent quality auditor (read-only on `src/`, `tests/`, `Makefile`).
Every result below was re-derived by building and running; no prior report or
coder figure was trusted.

**Workspace:** `/Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer`
**Host:** macOS (aarch64), `/bin/zsh`, Apple `cc` (clang). Default render used
below is 320x180 / 4 spp / depth 4 / seed 1337 unless stated otherwise.
**Scratch artifacts:** written only under `/tmp` (`/tmp/pt_base`, `/tmp/*.bmp`,
`/tmp/build_*.log`, `/tmp/test_serial.log`). Nothing written under `.marmel/`.

---

## 0. Gate verdict: **PASS**

| # | Check | Verdict |
|---|-------|---------|
| A | `make clean && make` and `make threads` warning-free, exit 0 | **PASS** |
| B | `make test` — all pass (1749 passed / 0 failed, 27 binaries) | **PASS** |
| C | Default render byte-identical to pristine pre-path-tracer baseline | **PASS** |
| D | `--pathtrace` succeeds and is distinct from the default render | **PASS** |
| E | `--pathtrace` byte-identical across thread counts 1/2/4/8/16 and vs serial | **PASS** |
| F | `--pathtrace` output is a valid, non-degenerate BMP | **PASS** |
| G | No regressions; default output unchanged by `render.{h,c}` edits | **PASS** |

No invariant violation found. Notes (not defects) are listed in §7.

---

## 1. Baseline provenance (needed for check C)

- `git status --porcelain` shows `src/pathtrace.c`, `src/pathtrace.h`,
  `src/sampling.c`, `src/sampling.h` as **untracked (`??`)**; `git ls-tree HEAD src/`
  confirms they are **absent from HEAD** (`NOT_IN_HEAD`).
- `HEAD` = `61e3f21 "V 0.1"` (2026-09-14), the only commit (no tags/branches).
- The working tree carries many **uncommitted** changes (textures, DOF, PBR,
  refraction, scene format, …) that predate the path tracer; `Makefile` is
  modified only by adding `src/texture.c src/sampling.c src/pathtrace.c` to `SRCS`.
- `git ls-tree HEAD src/ | grep -E "pathtrace|sampling"` → none, so **HEAD is a
  valid pristine pre-path-tracer baseline**. Baseline method (i) was used:

```
rm -rf /tmp/pt_base && mkdir -p /tmp/pt_base
git archive HEAD | tar -x -C /tmp/pt_base
(cd /tmp/pt_base && make)          # BASE_EXIT=0, 0 warnings
```

`src/default_scene_text.h` and `scenes/default.scene` are **unmodified** vs HEAD
(`diff` → IDENTICAL for both), so the baseline renders the same default scene.

---

## 2. Check A — warning-free builds

```
make clean && make
  cc -std=c11 -O2 -Wall -Wextra -Isrc ... -o raytracer      EXIT=0
  grep -ciE "warning|error" /tmp/build_default.log          -> 0

make threads
  cc -std=c11 -O2 -Wall -Wextra -DUSE_PTHREADS -pthread ... EXIT=0
  grep -ciE "warning|error" /tmp/build_threads.log          -> 0
```

Both targets exit 0 with **ZERO warnings** under `-Wall -Wextra` (all 15
translation units + link). **No warning text to report.**

---

## 3. Check B — full test suite

```
make clean && make test      EXIT=0, final line "ALL TESTS PASSED"
grep -cE "^== bin/"          -> 27 test binaries
"passed" sum                 -> 1749 ;  "failed" sum -> 0
```

27 binaries, **1749 assertions passed, 0 failed**. Per-binary pass counts:
adaptive 63, bmp 118, bvh_planes 46, dof 40, emissive_lights 69, glossy 53,
integration_adaptive 40, integration_emissive 14, integration_features 58,
integration_glossy 24, integration_pbr 86, integration_refraction 66,
integration 30, material_presets 140, math 190, noise 61, **pathtrace_sampling 115**,
pbr 77, ppm 59, progress 19, refraction 70, render_threads 4, sampling 102,
softshadow 33, texture 57, tree_params 81, water 34. **PASS.**

> The HEAD baseline suite is much smaller (8 test files, 483 assertions) — the
> larger current suite is expected, not a regression.

---

## 4. Check C — default byte-identity (CRITICAL)

**Method (i):** build the pristine HEAD tree at `/tmp/pt_base` and compare the
default render byte-for-byte.

```
# current serial build
make clean && make
RAYTRACER_NO_PROGRESS=1 ./raytracer --width 320 --height 180 --samples 4 --depth 4 \
    --out /tmp/cur_serial_default.bmp

# pristine baseline build
(cd /tmp/pt_base && RAYTRACER_NO_PROGRESS=1 ./raytracer --width 320 --height 180 \
    --samples 4 --depth 4 --out /tmp/base_default.bmp)

shasum -a 256 /tmp/base_default.bmp /tmp/cur_serial_default.bmp
cmp /tmp/base_default.bmp /tmp/cur_serial_default.bmp
```

Observed — both files **172854 bytes**:

| render | SHA-256 |
|---|---|
| baseline (`git HEAD`) | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |
| current serial | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |
| current threaded (8) | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |
| `--scene scenes/default.scene` | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |

`cmp` → `BYTE_IDENTICAL_DEFAULT`. Cross-checked at 640x360 / 16 spp / depth 6:
baseline == current == `54e689997dbf85a9ab702b9c8853fa8a9194239d77cb69efa53464b587ba8ad5`.

Additionally, `DEFAULT_SCENE_TEXT` (extracted from `src/default_scene_text.h`) is
**byte-identical** to `scenes/default.scene` (both 3895 bytes; Python compare →
`identical: True`). **PASS.**

---

## 5. Checks D, E, F — `--pathtrace` distinctness, determinism, validity

### D — works and is distinct

```
RAYTRACER_NO_PROGRESS=1 ./raytracer --pathtrace --width 320 --height 180 \
    --samples 4 --depth 4 --out /tmp/pt_serial.bmp
```

Exit 0; stdout reports `Mode: path tracer (depth 4)`, `Wrote /tmp/pt_serial.bmp (172854 bytes)`.

| render | SHA-256 |
|---|---|
| default (no `--pathtrace`) | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |
| `--pathtrace` (serial) | `522d33981397748231a0affcab8e1dd8e0d97184d4333344c63c5e4bc2a397a7` |

Distinct: **164573 of 172800** pixel bytes differ. **PASS.**

### E — determinism

Threaded build (`make threads`), `RAYTRACER_THREADS` selects the worker count
(the binary honours it in `pt_choose_thread_count`; verified non-vacuous because
wall-clock scales: 800x450/16 spp → 53.54 s @1, 27.03 s @2, 6.90 s @16):

```
for t in 1 2 4 8 16; do
  RAYTRACER_NO_PROGRESS=1 RAYTRACER_THREADS=$t ./raytracer --pathtrace --threads \
      --width 320 --height 180 --samples 4 --depth 4 --out /tmp/pt_thr_$t.bmp
done
```

All five outputs → `522d33981397748231a0affcab8e1dd8e0d97184d4333344c63c5e4bc2a397a7`
(identical). The **serial** build's `--pathtrace` output
(`/tmp/pt_serial.bmp`) is the **same** hash, i.e. serial == threaded @1 thread.
Confirmed at a second config (800x450 / 16 spp / depth 6): @1 and @16 both →
`f6b1bef2e366b78d939bf7defce84778d3909d24b06e4a04f379a5c1e57710c5`. **PASS.**

### F — valid, non-degenerate BMP

Header parse of `/tmp/pt_serial.bmp`: magic `BM`, `bfSize=0x0002a336`,
`biWidth=0x140`=320, `biHeight=0x0b4`=180, `biBitCount=0x18`=24, `biSizeImage=0x2a300`
= 172800 = 320*180*3, file size 172854 = 54 + 172800. Pixel stats (payload):
min 0, max 255, mean 228.5, 245 distinct byte values, fully-black pixels 2/57600,
fully-white pixels 14032/57600 (0.244) — neither all-black nor all-white.
`pathtrace_to_byte` clamps NaN→0 and the kernel guards non-finite throughput, so
no NaN can reach the buffer. **PASS.**

---

## 6. Check G — no regressions

- All pre-existing tests still pass (§3).
- `src/render.c` / `src/render.h` are modified vs HEAD, but the default render is
  **byte-identical** to the pristine baseline (§4), i.e. the edits do **not**
  change default output. `grep -nE "pathtrace|sampling_" src/render.c src/render.h`
  → **none**: the path tracer is strictly additive and does not hook the Whitted
  renderer.
- `git status --porcelain src/render.c src/render.h` → ` M` (modified), but the
  byte-identity gate proves no observable default-output change. **PASS.**

---

## 7. Notes / limitations (not gate failures)

1. The path-tracer kernel is invoked only via the new `pathtrace_render` entry
   point (`src/main.c` selects it when `--pathtrace` is set); `render.{h,c}` are
   untouched by path-tracing code.
2. `--pathtrace --adaptive` is accepted and both lines print; the adaptive
   statistics remain 0 because adaptive sampling is a Whitted-path feature. This
   is outside the gate scope (documented behaviour, not a defect here).
3. `--threads` is a no-op in the serial build (prints a note), as designed.
4. This gate covers the implementation only; full acceptance (visual/tone-mapping
   fidelity, NEE correctness, performance) is deferred to the later full
   verification task.

---

## 8. Exact command log (condensed)

```
make clean && make                                   # exit 0, 0 warnings
make threads                                         # exit 0, 0 warnings
make clean && make test                              # exit 0, 1749/0, 27 bins

git archive HEAD | tar -x -C /tmp/pt_base && (cd /tmp/pt_base && make)
(cd /tmp/pt_base && ./raytracer --width 320 --height 180 --samples 4 --depth 4 --out /tmp/base_default.bmp)
./raytracer --width 320 --height 180 --samples 4 --depth 4 --out /tmp/cur_serial_default.bmp
RAYTRACER_THREADS=8 ./raytracer --threads --width 320 --height 180 --samples 4 --depth 4 --out /tmp/cur_thr_default.bmp
./raytracer --scene scenes/default.scene --width 320 --height 180 --samples 4 --depth 4 --out /tmp/scene_file_default.bmp
./raytracer --pathtrace --width 320 --height 180 --samples 4 --depth 4 --out /tmp/pt_serial.bmp
for t in 1 2 4 8 16; do RAYTRACER_THREADS=$t ./raytracer --pathtrace --threads --width 320 --height 180 --samples 4 --depth 4 --out /tmp/pt_thr_$t.bmp; done
```

(All render commands prefixed with `RAYTRACER_NO_PROGRESS=1`; hashes via `shasum -a 256`.)

---

# Final independent verification

**Task:** end-to-end verification of the opt-in unbiased path tracer (`--pathtrace`).
**Method:** all results below were re-derived by building and running the real
binary/tests in this workspace; no coder figure was trusted. Scratch artifacts
were written only under `/tmp`; nothing under `.marmel/` was touched. The pristine
pre-feature baseline was obtained with `git worktree add /tmp/ptbase HEAD`
(HEAD = `61e3f215c46959ce6801fbbc3019bae17cf93fd0`, "V 0.1"), built there, and
removed afterwards (`git worktree remove /tmp/ptbase --force`, then
`git worktree prune`; `git worktree list` back to the main tree only).

## 0. Final verdict summary

| # | Check | Result |
|---|-------|--------|
| 1 | Warning-free `make` and `make threads` | **PASS** |
| 2 | Full `make test` suite | **PASS** |
| 3 | Default render byte-identical to pristine baseline | **PASS** |
| 4 | `--pathtrace` distinct + valid non-degenerate BMP | **PASS** |
| 5 | Determinism across thread counts 1/2/4/8 (and vs serial) | **PASS** |
| 6 | NEE noise reduction with higher spp, no fireflies | **PASS** |
| 7 | Russian Roulette unbiasedness (`--depth 2` vs `6`) | **PASS** |
| 8 | Furnace / energy conservation | **PASS** (unit test) |
| 9 | New example scene renders in both modes | **PASS** |
| 10 | Progress meter: stderr, 100%, silenceable, byte-stable | **PASS** |
| 11 | Docs consistency | **PASS** |
| 12 | `scenes/default.scene` == `DEFAULT_SCENE_TEXT`, unmodified | **PASS** |

No invariant violation. Caveats are documented in §13 (none are defects).

## 1. Check 1 — warning-free builds

```
make clean && make          -> EXIT 0 ; grep -ciE "warning|error" /tmp/vf_build_default.log -> 0
make threads                -> EXIT 0 ; grep -ciE "warning|error" /tmp/vf_build_threads.log -> 0
```

All translation units (incl. `src/sampling.c`, `src/pathtrace.c`) compile under
`-std=c11 -O2 -Wall -Wextra`. **Zero warning/error lines in either log; no
warning text to report.** **PASS.**

## 2. Check 2 — full test suite

```
make clean && make test     -> EXIT 0 ; final line "ALL TESTS PASSED"
grep -cE "^== bin/"         -> 29 test binaries
sum of "N passed"           -> 1806 ; sum of "N failed" -> 0
```

29 binaries, **1806 assertions passed, 0 failed.** The four new/extended
path-tracer binaries all pass: `test_sampling` 102, `test_pathtrace_sampling`
115, `test_pathtrace` 39, `test_integration_pathtrace` 18. `test_render_threads`
reports "single-threaded build" (serial build under `make test`). **PASS.**

## 3. Check 3 — default byte-identity (CRITICAL)

```
git worktree add /tmp/ptbase HEAD ; (cd /tmp/ptbase && make)          # BASE exit 0, 0 warnings
(cd /tmp/ptbase && RAYTRACER_NO_PROGRESS=1 ./raytracer --width 320 --height 180 \
    --samples 4 --depth 4 --out /tmp/vf_base_default.bmp)
RAYTRACER_NO_PROGRESS=1 ./raytracer --width 320 --height 180 --samples 4 --depth 4 \
    --out /tmp/vf_default.bmp
RAYTRACER_NO_PROGRESS=1 RAYTRACER_THREADS=8 ./raytracer --threads --width 320 \
    --height 180 --samples 4 --depth 4 --out /tmp/vf_default_thr.bmp
cmp /tmp/vf_base_default.bmp /tmp/vf_default.bmp   -> BYTE_IDENTICAL
```

| render | SHA-256 |
|---|---|
| pristine baseline (worktree @ HEAD) | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |
| current serial default | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |
| current threaded (8) default | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |
| `--scene scenes/default.scene` | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |

All four are the known baseline hash **`f9f4551a…ffcb`** (172854 bytes each);
`cmp` → byte-identical. **PASS.**

## 4. Check 4 — `--pathtrace` distinct and valid

```
RAYTRACER_NO_PROGRESS=1 ./raytracer --pathtrace --width 320 --height 180 \
    --samples 4 --depth 4 --out /tmp/vf_pt.bmp    # exit 0, "Mode: path tracer (depth 4)"
```

| render | SHA-256 |
|---|---|
| default | `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb` |
| `--pathtrace` | `522d33981397748231a0affcab8e1dd8e0d97184d4333344c63c5e4bc2a397a7` |

Hashes differ → distinct image. BMP header parse: `BM`, 320x180, 24-bit,
payload 172800 bytes; pixel stats mean 228.49, min 0, max 255, 2/57600 fully
black, 14032/57600 fully white → neither all-black nor all-white (non-degenerate).
**PASS.**

## 5. Check 5 — determinism matrix

Threaded build (`make threads`, exit 0). 160x90 / 4 spp / depth 4:

```
for t in 1 2 4 8; do RAYTRACER_THREADS=$t ./raytracer --threads --pathtrace \
    --width 160 --height 90 --samples 4 --depth 4 --out /tmp/vf_pt_t$t.bmp; done
```

All four (`t=1,2,4,8`) → `e0dab630e0318d1050cb8d4449d9f490cc2fe9efe89479c410ea1ed34d16092d`
(identical). Second config 640x360 / 8 spp / depth 4: `t=1` and `t=8` both →
`78af5153d8b0a7c7c1d68dd3ace9eb569faa765e904d2e83bb99680cc674d249`.
Thread count is **non-vacuous**: same 640x360/8 spp render takes `real 16.90 s`
at `RAYTRACER_THREADS=1` vs `real 2.32 s` at `=8`. The serial build's
`--pathtrace` equals the threaded `t=1` stream. **PASS.**

## 6. Check 6 — NEE noise reduction

Scene `scenes/example_pathtrace.scene` (dim sky + emissive PBR lamp + area light),
160x90 / depth 6, `--samples 4` vs `--samples 32` (luminance = 0.2126R+0.7152G+0.0722B):

| render | mean L | variance | neighbor roughness | bright outliers (>250) |
|---|---|---|---|---|
| `--samples 4`  | 135.809 | 871.14 | **6.312** | 172 |
| `--samples 32` | 136.232 | 823.20 | **2.966** | 171 |

Higher spp lowers the per-pixel roughness (6.31 → 2.97, −53%) and variance while
the mean is stable (+0.3%). Outlier count is unchanged (~171–172) → **no firefly
explosion**. **PASS.**

## 7. Check 7 — Russian Roulette unbiasedness (sanity)

Same scene, 160x90 / `--samples 64`, `--depth 2` vs `--depth 6`:

| render | mean L | variance | neighbor roughness |
|---|---|---|---|
| `--depth 2` | 133.473 | 994.39 | 2.735 |
| `--depth 6` | 136.260 | 820.50 | 2.475 |

Mean luminance differs by only **+2.1%** (deeper = slightly brighter, consistent
with additional GI from further bounces) — **not systematically darker/brighter
beyond noise**. Deeper depth also reduces variance/roughness. RR does not bias
the estimator downward. **PASS.**

## 8. Check 8 — furnace / energy sanity

`tests/test_pathtrace.c` contains a dedicated furnace case
(`FURNACE_SCENE`, `test_furnace()`): a uniform bright sky (no sun, glow off) over
a pure-white Lambertian ground, asserting every radiance sample is finite and in
`[0, sky + 5% tol]`, that the white ground reflects real light, and that the mean
radiance never exceeds the incident sky radiance. This test passes as part of the
29-binary suite (§2). **PASS** (covered by unit test, no separate scene needed).

## 9. Check 9 — new example scene

```
RAYTRACER_NO_PROGRESS=1 ./raytracer --scene scenes/example_pathtrace.scene \
    --pathtrace --width 160 --height 90 --samples 4 --depth 4 --out /tmp/vf_example_pt.bmp   # exit 0
RAYTRACER_NO_PROGRESS=1 ./raytracer --scene scenes/example_pathtrace.scene \
    --width 160 --height 90 --samples 4 --depth 4 --out /tmp/vf_example_wt.bmp               # exit 0
```

Both exit 0; both are valid 160x90 24-bit BMPs (43254 bytes). Pixel stats:
pathtrace mean 135.77, var 992.5, 2/14400 black, 172/14400 white; Whitted mean
142.95, var 1483.2, 0 black, 171 white → both non-degenerate. SHA-256:
pathtrace `725914c60bc5fe1c1c3d819c6940ef67bfee56a5762e112a9e4bd40bd69f376e`,
Whitted `d6a7e0fe9020197e009d6773f726450b83adef401b4abaf701b2fade221b9b53`
(distinct, as expected). **PASS.**

## 10. Check 10 — progress meter

Serial build, 64x36 / 2 spp / depth 4:
- stdout contains the summary (`Progress: on (stderr)`, `Wrote …`); the meter
  `render:   0% …` / `render: 100% [00:00<00:00, …]` is written to **stderr only**
  (stdout had zero `render:` lines). Reaches **100%**.
- `RAYTRACER_NO_PROGRESS=1` → stderr **0 bytes**.
- Bytes identical with/without progress: `5167affe7644dd876aee74afedbce87b8e9c4fb82b275e4fd173fa44dea887b8`.

Threaded build (`make threads`), 64x36 / 2 spp / depth 4, default **and**
`--pathtrace`:
- Default: meter on stderr, reaches `100%`; no-progress stderr 0 bytes; both
  renders → `5167affe7644dd876aee74afedbce87b8e9c4fb82b275e4fd173fa44dea887b8`.
- `--pathtrace`: meter on stderr, reaches `100%`; no-progress stderr 0 bytes;
  both renders → `9f9abe09003f9c1b130b46c70c6de6cce86ef7051b03d1bb3b6f02b53c52870a`.

Progress never alters pixel data in either build/mode. **PASS.**

## 11. Check 11 — docs consistency

- `README.md` describes `--pathtrace` as an **opt-in** unbiased unidirectional
  path tracer; options table (line 130) states bounces up to `--depth`, `--samples`
  paths/pixel, and that without the flag the output is **byte-identical**.
  Gallery (line 496) lists `scenes/example_pathtrace.scene` and gives the
  `--pathtrace … --samples 256 --depth 6 --out output/pathtrace.bmp` command.
- `docs/render_notes.md` §6.10 "Opt-in unbiased path tracer (`--pathtrace`) — how
  it works" documents the byte-identity guarantee, iterative bounce loop, NEE, and
  reused BVH infrastructure; references `scenes/example_pathtrace.scene`.
- `docs/scene_format.md` line 1014 documents `--pathtrace` (opt-in, interacts with
  `--depth`/`--samples`, byte-identical default).

Flag name (`--pathtrace`), opt-in semantics, default byte-identity, and the
`--depth`/`--samples` interaction are all stated accurately; the gallery
references the new scene. **PASS.**

## 12. Check 12 — default scene invariant

```
git diff --quiet scenes/default.scene   -> EMPTY_DIFF (unchanged vs HEAD)
DEFAULT_SCENE_TEXT vs scenes/default.scene: both 3895 bytes, identical: True
```

The file was not regenerated and matches the embedded `DEFAULT_SCENE_TEXT`
byte-for-byte. **PASS.**

## 13. Residual caveats (documented, not failures)

1. **Sun NEE is first-bounce-only** — the sky model has only a smooth sun glow,
   no discrete sun disk, so direct light is estimated at the first vertex only.
2. **`--pathtrace --adaptive`** is accepted (both `Mode:` and `Adaptive:` lines
   print) but adaptive statistics stay 0 (`used 0 samples, max/px=0`); the adaptive
   sampler is Whitted-only.
3. **`--threads` is a no-op in the serial build** — prints
   `note: --threads ignored; rebuild with 'make threads' for a threaded build`.
4. **Cosmetic:** in `--pathtrace` mode the summary prints `Time: 0.00 s` because
   `pathtrace_render` does not populate the renderer's timing global
   (`render_last_seconds()` stays 0). Rendering is genuinely fast/serial or
   threaded as selected; only the displayed time is unset. Not part of the
   acceptance checklist.

---

VERDICT: APPROVED

---

## 14. Defaults inversion + flip fix verification

Independent end-to-end verification of the new quality-by-default CLI behaviour
(path tracer, adaptive sampling and multithreading ON by default; `--no-*` opt-outs)
and of the path-tracer vertical-flip fix. Read-only audit of the tree; this section
is append-only and does not modify any source file.

Environment: macOS (aarch64), /bin/zsh, CWD = repo root. Binary built with the
default -Wall -Wextra C11 flags. All renders use small sizes for speed.

### 14.1 Check 1 - Warning-free builds (PASS)

```
make clean && make                 # default == threaded; exit 0; warnings=0
make clean && make serial          # serial opt-out;      exit 0; warnings=0
make threads                       # alias of default;    exit 0; pthread refs=2
make no-threads                    # alias of serial;     exit 0; pthread refs=0
grep -ciE "warning|error" build_default.log -> 0
nm raytracer | grep -i pthread     # default build: U _pthread_create, U _pthread_join
nm raytracer | grep -i pthread     # serial build:  0 matches
```
Default binary links pthread_create/pthread_join (threaded); serial build has zero
pthread symbols. Both builds are warning-free under -Wall -Wextra. **PASS.**

### 14.2 Check 2 - Full test suite (PASS)

```
make clean && make        && make test    # threaded build first, then tests
make clean && make serial && make test    # serial build first, then tests
```
Both runs: exit 0, "ALL TESTS PASSED", 29 test binaries, 1848 assertions passed,
0 failed, 0 skipped. The binary was built BEFORE `make test` in both runs, so the
CLI end-to-end assertions executed (no self-skip): integration_pathtrace = 44
passed, including 22 CLI assertions (the binary-presence guard was satisfied).

Per-binary passed counts (identical for both builds): adaptive 67, bmp 118,
bvh_planes 46, dof 40, emissive_lights 73, glossy 53, integration_adaptive 40,
integration_emissive 14, integration_features 58, integration_glossy 24,
integration_pathtrace 44, integration_pbr 86, integration_refraction 66,
integration 30, material_presets 140, math 190, noise 61, pathtrace_sampling 115,
pathtrace 43, pbr 77, ppm 59, progress 19, refraction 70, render_threads 8,
sampling 102, softshadow 33, texture 57, tree_params 81, water 34.

Caveat (not a failure): the `test` target links tests against LIB_OBJS without
forwarding THREADS=0, so after `make serial` the test .c files are compiled with
-DUSE_PTHREADS -pthread while linked to serial objects; the threaded-only branches
still pass. Running `make test THREADS=0` explicitly gives render_threads=4
("single-threaded build") and integration_pathtrace=39 (RAYTRACER_THREADS check
skipped) - both green. **PASS.**
%s\n

### 14.3 Check 3 - DEFAULT semantics (PASS)

```
./raytracer --width 160 --height 90 --samples 4 --depth 4 --out /tmp/v_def.bmp
./raytracer --pathtrace --width 160 --height 90 --samples 4 --depth 4 --out /tmp/v_pt.bmp
sha256 /tmp/v_def.bmp = 8ea8eab208147f57bf0aad00dae3b69102c633ec9aab06f3e5abf9a3b5185e5d
sha256 /tmp/v_pt.bmp  = 8ea8eab208147f57bf0aad00dae3b69102c633ec9aab06f3e5abf9a3b5185e5d  (IDENTICAL)
```
Default render hash == `--pathtrace` hash. `Mode:` line by default reads
"Mode: path tracer (global illumination, depth 4)". **PASS.**

### 14.4 Check 4 - --no-pathtrace semantics (PASS)

```
./raytracer --no-pathtrace --width 160 --height 90 --samples 4 --depth 4 --out /tmp/v_wh.bmp
sha256 /tmp/v_wh.bmp = 419d5d72ff00c51c794509f8dbece04dc67ff0ff8109e23f70cb516192890496
```
Differs from the default hash; `Mode:` reports "Mode: Whitted (legacy, depth 4)".
Adaptive is ON by default, so the summary also shows "used 87364 samples, max/px=16".
**PASS.**

### 14.5 Check 5 - Legacy byte-identity (PASS)

```
# SERIAL build:
./raytracer --no-pathtrace --no-adaptive --width 320 --height 180 --samples 4 --depth 4 --out /tmp/v_legacy.bmp
sha256 /tmp/v_legacy.bmp = f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb
expected                = f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb  (EXACT MATCH, 172854 bytes)
# --no-pathtrace ALONE (adaptive ON) at 320x180:
sha256 /tmp/v_wh_adapt320.bmp = dfb30615b20575843993d178a821339dbf3c98cd5fd0ad2443f73567de52cdc6  (DIFFERENT)
```
The pristine fixed-spp Whitted render is reproduced byte-for-byte with
`--no-pathtrace --no-adaptive`; `--no-pathtrace` alone yields a different hash because
adaptive sampling is active. **PASS.**

### 14.6 Check 6 - Opt-out / backward-compat flags (PASS)

```
--no-threads --single-threaded --no-adaptive --no-pathtrace --pathtrace --adaptive --threads
  -> each exits 0
--no-threads=1 --no-adaptive=1 --no-pathtrace=1 --threads=1
  -> each exits 2 (value form rejected)
```
All opt-out flags accepted; value-assignment forms rejected with nonzero exit.
Backward-compat `--pathtrace`/`--adaptive`/`--threads` still accepted (no-ops). **PASS.**
%s\n

### 14.7 Check 7 - Vertical-flip correctness (PASS)

Method: decode BMP top-to-bottom, compute per-row mean luminance; compare the
path-tracer image against the Whitted image band-by-band. Raw evidence:

```
scenes/example_pathtrace.scene @ 160x90/8spp/depth4:
  TOP band (logical top rows) RGB: pt = (143.2,143.2,143.9) == wh = (143.2,143.2,143.9)
  top-30%% mean-abs-diff: pt-top vs wh-top = 0.00 ; pt-top vs wh-bottom = 30.64
  sky-band(top) vs whitted-top   correlation = +0.7702
  sky-band(top) vs whitted-bottom correlation = -0.3851
  top-3 rows: pt [141.3,141.7,142.0] == wh [141.3,141.7,142.0]

scenes/default.scene @ 160x90/8spp/depth4:
  TOP band RGB: pt = (213.6,227.9,251.3) (blue sky) ; top-3 rows pt [224.1,224.7,224.9] == wh same
  top-30%% mean-abs-diff: pt-top vs wh-top = 0.05 ; pt-top vs wh-bottom = 162.11
  sky-band(top) correlation: unflipped = +0.9630 ; flipped = -0.3551
  BOTTOM band: pt RGB = (237.2,249.3,251.8) (bright ground) ; wh RGB = (54.6,72.7,72.1) (dark ground)
```
The path-tracer TOP rows are the SKY (brighter/cool) and BOTTOM rows are the GROUND,
matching the Whitted vertical structure; the row-profile correlation strongly favors
the unflipped orientation (0.96 vs -0.36). Root cause confirmed fixed in source:
`src/pathtrace.c:769  double vv = 1.0 - ((double)y + jy)/(double)height;` matches
`src/render.c:904` and `src/render.c:1171`. **PASS.**

### 14.8 Check 8 - Determinism matrix (PASS)

```
# Threaded build, 160x90/4spp/depth4:
default (pathtrace)  RAYTRACER_THREADS=1/2/4/8 -> 8ea8eab208147f57bf0aad00dae3b69102c633ec9aab06f3e5abf9a3b5185e5d (all 4 identical)
--no-pathtrace       RAYTRACER_THREADS=1/2/4/8 -> 419d5d72ff00c51c794509f8dbece04dc67ff0ff8109e23f70cb516192890496 (all 4 identical)

# Non-vacuous scaling, 400x225/16spp/depth5 real time:
  threads=1 -> 13.92 s
  threads=4 ->  3.56 s
  threads=8 ->  2.16 s
  all hashes = 95dabf3c0401cb51fe38fc3e48e2293bf30013293a6454161a98741f9fd9feda
```
Byte-identical output across thread counts in both modes; wall time scales down
with threads, proving the scaling is non-vacuous. **PASS.**

### 14.9 Check 9 - Adaptive applies in Whitted mode (PASS)

```
--no-pathtrace               160x90 -> 419d5d72ff00c51c794509f8dbece04dc67ff0ff8109e23f70cb516192890496 (adaptive ON)
--no-pathtrace --no-adaptive 160x90 -> 3eec91dabfc6b03cf39136e212bf0f61c8761e3b7ef4f70a9b082059a78c0a48 (differs)
--no-pathtrace --no-adaptive 320x180 -> f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb == legacy fixed-spp
```
Adaptive sampling is active in Whitted mode by default; `--no-adaptive` restores the
exact fixed-spp legacy render. **PASS.**
%s\n

### 14.10 Check 10 - Example scene + docs consistency (PASS)

```
./raytracer scenes/example_pathtrace.scene --width 160 --height 90 --out /tmp/v_ex_pt.bmp   -> exit 0
./raytracer --no-pathtrace scenes/example_pathtrace.scene --width 160 --height 90 --out /tmp/v_ex_wh.bmp -> exit 0
```
Docs accurate:
- README.md lines 3-6: default render is the unbiased path tracer with adaptive
  sampling and multithreading all ON; each opt-out (--no-pathtrace, --no-adaptive,
  --no-threads) documented. Options table lines 130-147 lists all flags. Build section
  lines 116-124 documents `make`=threaded default, `make serial`/`make no-threads`,
  `make threads` alias, and THREADS=0.
- docs/scene_format.md section 10 (~lines 1010-1027): table documents --adaptive/
  --no-adaptive/--pathtrace/--no-pathtrace/--threads/--no-threads/--single-threaded/
  --no-progress and states default-ON plus the --no-* opt-outs.
- docs/render_notes.md section 6.10 ("Unbiased path tracer (--pathtrace) - the DEFAULT
  renderer") documents the default renderer, the byte-identity guarantee, the
  "Orientation (vertical-flip bug fixed)" top=sky/bottom=ground note, NEE, RR and
  determinism; opt-out semantics also referenced at lines 24,29,31,719,778,785,806,846,864,868.
**PASS.**

### 14.11 Check 11 - scenes/default.scene invariant (PASS)

```
git diff --quiet scenes/default.scene   -> exit 0 (EMPTY diff)
scenes/default.scene = 3895 bytes, sha256 15421d01b61b8d419c3a5eed032a301612aa0c7de52ee2a92354c0995bcedc03
compiled DEFAULT_SCENE_TEXT (sizeof-1) = 3895 bytes, cmp -> BYTE_IDENTICAL_EMBEDDED, same sha256
```
The scene file is unmodified vs HEAD and byte-identical to the embedded DEFAULT_SCENE_TEXT.
(Note: `--write-scene` emits a different 3379-byte canonical form; that is the writer
canonicalisation, not the embedded literal, and is out of scope for this invariant.) **PASS.**

### 14.12 Residual caveats (documented, not failures)

1. `make serial && make test` compiles the test .c files with -DUSE_PTHREADS because the
   `test` target does not forward THREADS=0; the threaded-only branches still pass. Use
   `make test THREADS=0` to exercise the pure single-threaded test paths (also green).
2. `--write-scene` output (3379 bytes canonical) differs from the embedded scene literal
   (3895 bytes); the on-disk scenes/default.scene matches the embedded literal, so the
   invariant holds.

### 14.13 Summary

| # | Check | Result |
|---|-------|--------|
| 1 | Warning-free builds (default threaded + serial) | PASS |
| 2 | Full test suite (29 binaries, 1848 assertions) | PASS |
| 3 | DEFAULT == --pathtrace | PASS |
| 4 | --no-pathtrace differs, Mode=Whitted | PASS |
| 5 | Legacy byte-identity f9f4551a... | PASS |
| 6 | Opt-out / backward-compat flags | PASS |
| 7 | Vertical-flip correctness (top=sky) | PASS |
| 8 | Determinism matrix + non-vacuous scaling | PASS |
| 9 | Adaptive applies in Whitted mode | PASS |
| 10 | Example scene + docs consistency | PASS |
| 11 | scenes/default.scene invariant | PASS |

All 11 checklist items verified with raw evidence. The quality-by-default inversion
(path tracer + adaptive + multithreading ON, `--no-*` opt-outs) is correct, the legacy
fixed-spp Whitted render is reproduced byte-for-byte, and the path-tracer vertical-flip
fix is confirmed both in source and in rendered output.

VERDICT: APPROVED
