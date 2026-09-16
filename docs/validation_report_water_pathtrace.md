# Validation Report - Phase 10: water path-trace energy normalisation (t-128)

Independent QA audit. Read-only inspection: no file under `src/` or `tests/` was modified by this audit. All measurement scripts live under `/tmp`; the workspace was left with the normal (threaded) build.

- Workspace: `/Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer`
- Date: 2026-09-16. Toolchain: clang (`cc`), macOS aarch64.
- Build flags (from `Makefile` line 30): `-std=c11 -O2 -Wall -Wextra`

## Subject under test

Fix in `src/pathtrace.c` inside `pt_sample_legacy`, line 362:

    if (s > 1.0) norm = s;   /* norm stays 1.0 when s <= 1.0 */

with `s = p_mir + p_die` (mirror + dielectric lobe probabilities). Both lobe weights are divided by `norm` (`w = r_mir / (p_mir * norm)` and the dielectric analogue). The water preset has `reflectivity 1.0 + transparency 0.85 = 1.85`, so before the fix the legacy sampler carried a spurious x1.85 per-bounce gain that washed the pond towards white. When `s <= 1` nothing changes (bit-for-bit), so already energy-conserving materials are untouched.

New tests: `tests/test_water_units.c` (t-126), `tests/test_water_pathtrace.c` (t-127).

## Verdict summary

| # | Item | Result |
|---|------|--------|
| 1 | Warning-free `make` and `make serial` under `-Wall -Wextra` | **PASS** |
| 2 | `make test` GREEN from a clean build | **PASS** (31/31 suites) |
| 3 | Legacy byte-identity hash + `src/render.{h,c}` untouched | **PASS** (caveat 3c) |
| 4 | Determinism across thread counts + serial == threaded | **PASS** |
| 5 | Fix behaves as intended (water blue, `r+t<=1` unchanged) | **PASS** |
| 6 | Adversarial: new tests fail on reverted copy | **PASS** |

Final verdict: **APPROVED** (see caveats at the end).

---

## 1. Builds warning-free

Commands: `make clean && make` and `make clean && make serial`. Both exit 0 with zero warning lines under `-Wall -Wextra`:

    $ make clean >/dev/null 2>&1 && make 2>&1 | tee /tmp/b_t.log | grep -ci warning
    0
    MAKE_EXIT=0
    $ make clean >/dev/null 2>&1 && make serial 2>&1 | tee /tmp/b_s.log | grep -ci warning
    0
    SERIAL_EXIT=0

A second `grep -i warning` over each full log also returned zero matches. Compiler lines show the strict flags:

    cc -std=c11 -O2 -Wall -Wextra -DUSE_PTHREADS -pthread -Isrc ... -o bin/...

PASS - both threaded and `serial` build with zero warnings under `-Wall -Wextra`.

## 2. `make test` GREEN from a clean build

Command: `make clean && make && RAYTRACER_NO_PROGRESS=1 make test`. Result: exit 0, `ALL TESTS PASSED`, 31 suites, 0 failed anywhere.

    $ grep -c "^== " /tmp/t.log
    31
    $ grep -E "passed, [0-9]+ failed" /tmp/t.log
    tests/test_adaptive: 67 passed, 0 failed
    test_bmp.c: 118 passed, 0 failed
    tests/test_bvh_planes: 46 passed, 0 failed
    test_dof: 40 passed, 0 failed
    tests/test_emissive_lights: 73 passed, 0 failed
    tests/test_glossy: 53 passed, 0 failed
    tests/test_integration_adaptive: 40 passed, 0 failed
    tests/test_integration_emissive: 14 passed, 0 failed
    test_integration_features: 58 passed, 0 failed
    tests/test_integration_glossy: 24 passed, 0 failed
    tests/test_integration_pathtrace: 44 passed, 0 failed
    test_integration_pbr: 86 passed, 0 failed
    test_integration_refraction: 66 passed, 0 failed
    summary: 30 passed, 0 failed
    test_material_presets: 140 passed, 0 failed
    tests/test_math: 190 passed, 0 failed
    test_noise.c: 61 passed, 0 failed
    test_pathtrace_sampling: 115 passed, 0 failed
    tests/test_pathtrace: 43 passed, 0 failed
    test_pbr: 77 passed, 0 failed
    test_ppm.c: 59 passed, 0 failed
    tests/test_progress: 19 passed, 0 failed
    test_refraction: 70 passed, 0 failed
    tests/test_render_threads: 8 passed, 0 failed
    test_sampling: 102 passed, 0 failed
    test_softshadow: 33 passed, 0 failed
    test_texture: 57 passed, 0 failed
    test_tree_params: 81 passed, 0 failed
    tests/test_water_pathtrace: 23 passed, 0 failed
    tests/test_water_units: 490 passed, 0 failed
    tests/test_water: 34 passed, 0 failed

Both new suites pass on the current tree (water_units 490/0, water_pathtrace 23/0). PASS - full suite green, no failures.

## 3. Legacy byte-identity invariant

### 3a. Hash

    $ RAYTRACER_NO_PROGRESS=1 ./raytracer --no-pathtrace --no-adaptive --width 320 --height 180 --samples 4 --depth 4 --out /tmp/v_legacy.bmp
    $ shasum -a 256 /tmp/v_legacy.bmp
    f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb  /tmp/v_legacy.bmp
    $ stat -f "%z bytes" /tmp/v_legacy.bmp
    172854 bytes

The SHA-256 matches the required value exactly. The `serial` build produces the same hash (item 4). Because the fix only renormalises when `s > 1`, the legacy `--no-pathtrace` path is bit-for-bit unchanged.

### 3b. `src/render.{h,c}` modified by this phase?

`git` at HEAD `61e3f21 (V 0.1)` lists these two files as modified versus that ancient commit:

    $ git status --porcelain -- src/render.c src/render.h
     M src/render.c
     M src/render.h
    $ git diff --stat -- src/render.h src/render.c
     src/render.c | 1178 +++...---
     src/render.h |  117 ++-
     2 files changed, 1250 insertions(+), 45 deletions(-)

They are NOT touched by Phase 10. File mtimes place their last write two days before every Phase-10 artefact:

    2026-09-14 23:42 src/render.c
    2026-09-14 21:30 src/render.h
    2026-09-16 20:04 src/pathtrace.c              <- phase 10
    2026-09-16 20:04 tests/test_water_units.c     <- phase 10
    2026-09-16 20:18 tests/test_water_pathtrace.c <- phase 10

`render.{h,c}` were last written 2026-09-14; all Phase-10 files are 2026-09-16. They differ from git HEAD only because earlier phases also edited them.

### 3c. Caveat

`src/pathtrace.c` is untracked (`?? src/pathtrace.c`), so a `git diff` of the fix against HEAD is impossible; the fix was confirmed by reading line 362 directly. Likewise `render.{h,c}` are not clean versus the ancient HEAD commit - they carry earlier-phase edits - so "unmodified" means "not modified by Phase 10", evidenced by mtime.

PASS (hash exact; `render.{h,c}` predate the phase and were not touched by it - see caveat 3c).

## 4. Determinism matrix

The CLI has no numeric `--threads N` flag (`src/main.c` exposes only `--threads` / `--no-threads` booleans). The thread count is selected through the `RAYTRACER_THREADS` environment variable (`src/pathtrace.c` `pt_choose_thread_count`, valid 1..64). Renders at 1, 2, 4, 8 threads:

    $ for t in 1 2 4 8; do RAYTRACER_THREADS=$t RAYTRACER_NO_PROGRESS=1 ./raytracer --scene scenes/default.scene --width 320 --height 180 --samples 4 --depth 4 --out /tmp/det_$t.bmp; done
    t=1 26c55976b5a014d1225b9f8fcc55b54d641c1d623717e314132634239d39dc56
    t=2 26c55976b5a014d1225b9f8fcc55b54d641c1d623717e314132634239d39dc56
    t=4 26c55976b5a014d1225b9f8fcc55b54d641c1d623717e314132634239d39dc56
    t=8 26c55976b5a014d1225b9f8fcc55b54d641c1d623717e314132634239d39dc56

Serial vs threaded build (same scene/params):

    $ make clean && make serial && ./raytracer --scene scenes/default.scene --width 320 --height 180 --samples 4 --depth 4 --out /tmp/det_serial.bmp
    serial 26c55976b5a014d1225b9f8fcc55b54d641c1d623717e314132634239d39dc56

All four thread counts and the serial build produce the identical SHA-256 `26c55976b5a014d1225b9f8fcc55b54d641c1d623717e314132634239d39dc56`. PASS - byte-identical across 1/2/4/8 threads and serial == threaded.

## 5. Fix behaves as intended (independent measurement)

An independent 24-bit BMP reader/analyser (`/tmp/wq.py`, written by this audit) was used. The water-region mask is defined independently of the test suite as the lower half of the frame (rows h/2..h-1) whose reference (legacy) pixels are blue-dominant (b >= r); this yields 25134 of 57600 pixels. Fields: count, meanR, meanG, meanB, near-white fraction (all of r,g,b >= 245), blue-dominant fraction (b >= r).

    $ python3 /tmp/wq.py /tmp/w_leg.bmp /tmp/w_pt.bmp      # current tree
    maskpx 25134 of 57600
    ref (25134, 53.83, 73.12, 73.09, 0.0, 1.0)
    pt  (25134, 182.0, 205.22, 216.98, 0.04, 0.9943)

    $ python3 /tmp/wq.py /tmp/w_leg.bmp /tmp/w_pt_rev.bmp  # reverted copy
    maskpx 25134 of 57600
    ref (25134, 53.83, 73.12, 73.09, 0.0, 1.0)
    pt  (25134, 228.79, 242.05, 244.07, 0.5232, 0.9828)

- Current (fixed): water mean (182.0, 205.2, 217.0) - blue-tinted (B > G > R), near-white 4.0%, blue-dominant 99.43%. Not blown out.
- Pre-fix (reverted): water mean (228.8, 242.1, 244.1) - nearly saturated white, near-white 52.32%, blue-dominant 98.28%. The reported "~50% near-white / blown-out".

The fix moves water from ~52% near-white to 4% near-white and restores a clear blue tint, matching the intended behaviour and test calibration.

### 5b. Materials with `reflectivity + transparency <= 1` unchanged

By construction `norm` stays exactly 1.0 whenever `s <= 1.0`, so the weights `w = r/(p*norm)` equal the pre-fix code. Confirmed with controlled renders on the current tree vs the reverted copy:

    scene_le1         (glass_mat r=0.3 t=0.4 -> s=0.7)  cur=f3ded4dce03eed2f... rev=f3ded4dce03eed2f...
    example_glass     (glass preset r=0   t=1   -> s=1.0) cur=30ea815984f6a7d0... rev=30ea815984f6a7d0...
    example_pathtrace (no r+t>1 surfaces)                cur=cfefca8056b4afa3... rev=cfefca8056b4afa3...

Full hash of controlled scene `scene_le1` (160x90, 4 spp, depth 4): f3ded4dce03eed2f96fcc0b40cb0c230bea8e8c15bef7d44fcb1795624fd2f39 - identical between fixed and reverted trees. So `r + t <= 1` materials are byte-for-byte unchanged.

PASS - water is blue-tinted and not blown out (near-white 52% -> 4%), and `r + t <= 1` materials are unchanged.

## 6. Adversarial check (revert in a COPY, workspace untouched)

The workspace `src/pathtrace.c` was never modified. A full copy of the tree was made at `/tmp/rtrevert` and only there was line 362 changed to `norm = 1.0` (`if (s > 1.0) norm = 1.0; /* REVERTED-FOR-AUDIT */`), then rebuilt:

    $ (cd /tmp/rtrevert && RAYTRACER_NO_PROGRESS=1 make test) ; echo $?
    2

The new suites fail on the reverted copy:

    FAIL: near-white fraction of the water region is below the threshold (no regression to a blown-out pond) (tests/test_water_pathtrace.c:405)
    tests/test_water_pathtrace: 21 passed, 2 failed

    FAIL: water: mirror weight == r_mir / (p_mir * s) (tests/test_water_units.c:253): 1.8500000000000003 != 1.0000000000000002
    FAIL: water: dielectric weight == r_die / (p_die * s) (tests/test_water_units.c:259): 1.8500000000000001 != 1
    FAIL: water: p_mir*w_mir + p_die*w_die == 1 (energy conserved) (tests/test_water_units.c:263): 1.8500000000000001 != 1
    ...
    FAIL: prng: exact lobe weight matches contract (tests/test_water_units.c:417): 1.8500000000000003 != 1.0000000000000002
    tests/test_water_units: 418 passed, 72 failed

`make test` exits 2 with 74 total failures (2 in test_water_pathtrace, 72 in test_water_units). On the current tree the same suites pass (23/0 and 490/0, item 2). This proves the tests genuinely detect the bug rather than passing vacuously.

PASS - reverting the fix in a copy makes the new tests fail; they pass on the current tree.

---

## Caveats

1. `src/render.{h,c}` are not clean versus git HEAD (61e3f21): they carry edits from earlier phases. Their "unmodified by Phase 10" status rests on mtime evidence (2026-09-14 vs 2026-09-16), not on a clean `git status`.
2. `src/pathtrace.c` and the two new test files are untracked in git, so a `git diff`-based review of the change is not possible; the fix was verified by reading the source directly.
3. The thread count is controlled by the `RAYTRACER_THREADS` environment variable, not a numeric CLI flag (`--threads`/`--no-threads` are boolean).
4. The water-region mask for item 5 is an independent heuristic (lower half + reference blue-dominant), not derived from the test suite, so it is an independent corroboration rather than a copy of the tests.

## Final verdict

APPROVED. The Phase-10 energy-normalisation fix is present, builds warning-free in both threaded and serial configurations, leaves the full 31-suite test run green, preserves the legacy byte-identity hash exactly, is deterministic across thread counts and between serial/threaded builds, measurably fixes the blown-out water (near-white 52% -> 4%, blue-tinted), leaves `r + t <= 1` materials byte-identical, and is genuinely covered by the new tests (they fail when the fix is reverted).
