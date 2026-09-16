# Final Independent Validation Report — task t-114

**Validator:** independent QA auditor (read-only; all evidence produced by the
auditor's own scratch programs under `scratch_t114/`, now deleted).
**Workspace:** `/Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer`
**Host:** Apple silicon macOS (Darwin 25.6.0), `clang`, 10 logical CPUs.
**Scope:** re-verify the two prior defects D1 (dead hardcoded scene builder /
not-embedded default) and D2 (stale `output/scene.bmp` / non-reproducible
`docs/render_notes.md`), plus full end-to-end acceptance.

---

## 1. Clean build from scratch — ZERO warnings

```
make clean && make            # single-threaded
  rc=0 ; grep -icE "warning|error" -> 0
make clean && make threads    # -DUSE_PTHREADS -pthread
  rc=0 ; grep -icE "warning|error" -> 0
```

12 translation units + 1 link, `-std=c11 -O2 -Wall -Wextra`.
**PASS** — zero warnings, zero errors, exit 0 for both targets.

## 2. Full test suite — green (both builds)

`make test` builds one binary per `tests/*.c` and runs them:

| Test | passed | failed |
|---|---|---|
| test_bmp | 118 | 0 |
| test_bvh_planes | 46 | 0 |
| test_integration | 30 | 0 |
| test_math | 190 | 0 |
| test_noise | 61 | 0 |
| test_render_threads | 4 | 0 |
| test_water | 34 | 0 |
| **total** | **483** | **0** |

`ALL TESTS PASSED`, exit 0, in **both** the `make` and `make threads` builds.

> Note (not a defect): `make test` compiles the test TUs without extra flags,
> so they are always single-threaded; in the `threads` tree the shared objects
> carry `-DUSE_PTHREADS`. `test_render_threads` prints "single-threaded build;
> determinism across two calls verified" and still passes. Thread-count
> determinism is independently confirmed in section 5.

---

## 3. D1 — hardcoded scene builder removed; default truly embedded

**3a. No bare `scene_build` symbol/definition/call remains.**

```
grep -rn "scene_build" src/ tests/ Makefile
  -> only "scene_build_from_desc" occurrences; no bare scene_build
nm raytracer | grep -i scene_build
  -> 0000000100004e0c T _scene_build_from_desc      (only)
strings -a raytracer | grep -c scene_build
  -> 0
```

**3b. `scene_desc_load_string()` genuinely live; `default_scene_text.h` included.**

```
nm raytracer | grep -E "scene_desc_load_string|scene_default_desc"
  T _scene_default_desc
  T _scene_desc_load
  T _scene_desc_load_string
grep -n "scene_desc_load_string\|scene_default_desc" src/main.c
  341:        scene_default_desc(&desc);          (--write-scene path)
  369:        if (scene_desc_load_string(&desc, DEFAULT_SCENE_TEXT, "<embedded>",...
```

`src/main.c:35` `#include "default_scene_text.h"`; `src/main.c:386` calls
`scene_build_from_desc(&scene, &desc)` — the single construction path.

**3c. Embedded text present in the binary AND byte-identical to the file.**
A throwaway extractor (`scratch_t114/extract_default.c`) compiled the header and
dumped `DEFAULT_SCENE_TEXT` (minus NUL):

```
strings -a raytracer | grep -F "# scenes/default.scene"      -> found
cmp header_text.txt scenes/default.scene                     -> IDENTICAL
shasum -a 256 scenes/default.scene header_text.txt
  15421d01b61b8d419c3a5eed032a301612aa0c7de52ee2a92354c0995bcedc03  scenes/default.scene
  15421d01b61b8d419c3a5eed032a301612aa0c7de52ee2a92354c0995bcedc03  header_text.txt
```

**3d. No dead/unreachable code, no misleading comments.** `clang -Wall -Wextra
-fsyntax-only` on all TUs emits no `-Wunused-function`. `scene_trees`/
`scene_bushes` are used (scene.c:606/630); the only remaining deprecated symbol
is `water_depth_tint` (public API, documented as a deprecated wrapper, no
callers). No comment references a `scene_build()` that does not exist.

**3e. `--write-scene` body identity.** `./raytracer --write-scene X` exits 0 and
emits text whose **non-comment lines are byte-identical** to `scenes/default.scene`
(the only difference is the leading comment banner):

```
grep -v '^#' ws.scene | grep -v '^$' > ws_body.txt
grep -v '^#' default.scene | grep -v '^$' > def_body.txt
cmp ws_body.txt def_body.txt -> IDENTICAL
```

This matches the README wording exactly ("its body is exactly what `--write-scene`
emits"). D1 fully RESOLVED.

---

## 4. D2 — `output/scene.bmp` and `docs/render_notes.md` independently reproduced

**4a. Header / size / hash of the shipped artifact.**

```
file output/scene.bmp -> PC bitmap, 1920 x 1080 x 24, image size 6220800,
                         resolution 2835 x 2835 px/m, cbSize 6220854, bits offset 54
wc -c output/scene.bmp -> 6220854
shasum -a 256 output/scene.bmp -> 752d5f7c5818beb907f465fdebe20091bd8baa968452faaab417b5697f45ce28
```

Header bytes at offset 2 = 36 ec 5e 00 (6220854); width 8007 0000 (1920);
height 3804 0000 (1080); resolution 130b (2835). All match docs/render_notes
section 3.2 exactly.

**4b. Independent re-render == shipped artifact (byte identity).**

```
RAYTRACER_NO_PROGRESS=1 ./raytracer --width 1920 --height 1080 \
    --samples 16 --depth 6 --seed 1337 --out scratch_t114/render_default.bmp
  -> Time: 67.43 s (threaded); Wrote 6220854 bytes
shasum -a 256 scratch_t114/render_default.bmp output/scene.bmp
  752d5f7c...  scratch_t114/render_default.bmp
  752d5f7c...  output/scene.bmp
cmp scratch_t114/render_default.bmp output/scene.bmp -> BYTE IDENTICAL
```

**4c. --scene render == built-in default (byte identity).**

```
RAYTRACER_NO_PROGRESS=1 ./raytracer --scene scenes/default.scene --width 1920 \
    --height 1080 --samples 16 --depth 6 --seed 1337 --out scratch_t114/render_scene.bmp
shasum -a 256 scratch_t114/render_scene.bmp -> 752d5f7c...  (same)
cmp scratch_t114/render_scene.bmp scratch_t114/render_default.bmp -> IDENTICAL
```

**4d. 1280x720 reference hash.**

```
./raytracer --width 1280 --height 720 --samples 16 --depth 6 --seed 1337 \
    --out scratch_t114/ref1280.bmp   -> 2764854 bytes, 30.03 s
shasum -a 256 scratch_t114/ref1280.bmp
  8537ed5d7518c0d2ef8c198d70dd38104dd365973ce7c0fb9afe6380c280ff2d  == documented
```

**4e. Thread-count determinism.** `RAYTRACER_THREADS=1/8/10` on a 320x180x4
render all produced hash `f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb`,
matching docs/render_notes section 2 exactly. `RAYTRACER_THREADS=999` and `=abc`
fall back to 1 thread (both complete normally, per README contract).

**4f. Every render_notes statistic recomputed with my own Python decoder.**
I wrote an independent BMP reader (`scratch_t114/*.py`) and reproduced:

- Header (file/bfSize/offBits/width/height/bpp/compression/resolution): MATCH.
- Global per-channel stats (section 4.1): R 136.339/80.463 (min 25, max 255), G 152.671/77.035 (min 33, max 255), B 156.983/88.371 (min 27, max 255), Luminance 149.510/78.393, distinct colours 7848 -- EXACT match to render_notes section 4.1 (recomputed over all 2073600 px).
- Region split (section 4.3): sky 908160, water 986065, ground 179054,
  with means sky (218.1,230.8,250.6), water (61.4,80.4,80.8), ground (134.3,155.0,101.9)
  -- EXACT match (ground = below-line G>R+15 & G>B+15; water = below-line rest).
- Category counts (section 4.7): dark 51860, green 179565, warm 0, cloud 154883,
  sky 1298003 (= b>=g after priority exclusions), other 389289 -- EXACT.
- Bark visibility: 321 pixels with R>G (max R-G=3) -- MATCH.
- Threading user/real: measured 9.39 (35.58s user / 3.79s real) vs doc 9.37 (~93.7%).

- 16-band table (section 4.4): all 16 rows reproduced to the displayed precision (band 0 R210.9/G225.9/B250.7/lum224.49/B-R+39.75; band 7 +2.09; band 8 B-R-27.05; bands 9-15 water ~53,72,72 lum~68) -- EXACT.
- 9x16 mean-RGB grid (section 4.5): all 144 cells reproduced (rounded) -- EXACT.
- Classification grid (section 4.6): reproduced cell-for-cell with the priority order cloud (lum>245) > warm (R>G) > green-dominance (G>R and G>B) > sky (bluish, lum>200) > water. Note: section 4.6 uses green *dominance* while the section 4.7 numeric category uses G>R+15 and G>B+15; both are internally consistent with their own legends, and the 4.6 grid is fully reproducible.
- Dominance masks (section 4.7): blue-dominant 1349211, green-dominant 620347, warm-dominant 321 -- EXACT.

---

## 5. README documented workflow (verbatim)

Every command in `README.md` was executed as written:

- `make clean && make threads` -> rc=0, zero warnings (section 1).
- `./raytracer --help` -> exit 0; the printed usage block matches the README usage block line-for-line.
- `./raytracer --bogus` -> exit 2, `error: unknown option '--bogus'` followed by the usage block (matches README).
- `./raytracer --scene missing.scene` -> exit 1, `error: cannot load scene 'missing.scene': missing.scene:1: error: cannot open: No such file or directory (near '<eof>')` -- matches the README's documented error text exactly.
- `./raytracer --scene scenes/example_sunset.scene --out ...` -> rc=0.
- The README worked example (scene text, lines 205-316) was written to a file and rendered -> rc=0, `Scene: 551 primitives, 4 materials`. The default scene reports `Scene: 1286 primitives, 7 materials` (matches render_notes).
- `RAYTRACER_THREADS` honoured (1/8/10 -> byte-identical; 999/abc -> fall back to 1 thread). The threaded build always renders threaded.

**No divergence from the documented workflow.**

---

## 6. Source-tree hygiene

`find` (excluding `.git` and the scratch dir) for `*.orig`, `*.bak`, `*~`, `*.tmp`, `scratch_*`, `core` -> NONE. No stray temp files. `.gitignore` contains only `.marmel`. The scratch directory `scratch_t114/` used for this audit was removed entirely before finalising.

---

## 7. Leak checks (`leaks -atExit --`; ASAN known-broken on this host)

```
leaks -atExit -- ./raytracer --width 64 --height 48 --samples 2 --depth 3 --seed 1 --out /tmp/l1.bmp
  -> Process 0 leaks; exit 0
leaks -atExit -- ./raytracer --scene scenes/default.scene --width 64 --height 48 --samples 2 --depth 3 --seed 1 --out /tmp/l2.bmp
  -> Process 0 leaks; exit 0
leaks -atExit -- ./bin/test_integration   -> 0 leaks
leaks -atExit -- ./bin/test_water         -> 0 leaks
```

No leaks on the built-in-default path, the `--scene` path, or the test binaries.

---

## 8. Overall verdict

**PROJECT COMPLETE AND CORRECT.** Both prior defects are genuinely fixed and independently reproduced: D1 (dead hardcoded builder / non-embedded default) and D2 (stale artifact / non-reproducible notes) are RESOLVED. The build is warning-free, the full suite is green (483/0), the shipped `output/scene.bmp` is byte-identical to an independent re-render at the documented settings and every figure in `docs/render_notes.md` is reproducible from the actual artifact. No defect remains; no REPLAN required.
