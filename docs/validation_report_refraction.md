# Independent Validation Report — General Refraction / Glass Material Support

Auditor: independent QA (read-only; no source/scene/test file was modified).
Scratch artifacts were written only under `/tmp` (`/tmp/audit`, `/tmp/base_rt`,
`/tmp/work_norefract`). Nothing was written under `.marmel/`.

Workspace: `/Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer`
Git state: single commit `61e3f21 "V 0.1"` (HEAD) — it predates *all* in-progress
work (textures, DOF, soft shadows, refraction), i.e. the working tree has many
uncommitted changes. This is why the baseline strategy below is used.

## Summary of VERDICTS

| # | Check | Verdict |
|---|-------|---------|
| 1 | Clean `make` / `make threads`, zero warnings under `-Wall -Wextra` | PASS |
| 2 | `make test` — all tests pass, incl. both new binaries | PASS |
| 3 | `type = glass` preset field values | PASS |
| 4 | `beer_lambert` decoupling (0 vs 1, absorption ignored at 0) | PASS |
| 5a | Default render byte-identical to pre-change build | PASS |
| 5b | `scenes/default.scene` == `DEFAULT_SCENE_TEXT` | PASS |
| 5c | `type = water` unchanged (fields + render) | PASS |
| 6 | `scenes/example_glass.scene` parses + non-trivial + glass alters output | PASS |
| 7 | Determinism (single-threaded and threaded) | PASS |

**Overall VERDICT: PASS.** No discrepancy found. Details and evidence follow.

---

## 1. Clean build — PASS

```
$ make clean && make
... cc -std=c11 -O2 -Wall -Wextra -Isrc -MMD -MP -c src/*.c ...
... cc ... -o raytracer
$ grep -icE "warning|error" /tmp/build_default.log   # -> 0

$ make threads
... cc -std=c11 -O2 -Wall -Wextra -DUSE_PTHREADS -pthread -Isrc ...
$ grep -icE "warning|error" /tmp/build_threads.log   # -> 0
```

Both the default and the `-DUSE_PTHREADS` builds compile **all** translation
units with zero warnings and zero errors under `-Wall -Wextra`. Final clean
rebuild at the end of the audit (`make clean && make`) also produced 0 warnings.

## 2. Full test suite — PASS

```
$ make clean && make test
... ALL TESTS PASSED
$ echo $?        # -> 0
```

Per-binary results (exit code 0 for every binary; parsed from the run log):

| Binary | Result |
|--------|--------|
| test_bmp | 118 passed, 0 failed |
| test_bvh_planes | 46 passed, 0 failed |
| test_dof | 40 passed, 0 failed |
| test_integration | 30 passed, 0 failed |
| test_integration_features | 58 passed, 0 failed |
| **test_integration_refraction** | **66 passed, 0 failed** |
| test_math | 190 passed, 0 failed |
| test_noise | 61 passed, 0 failed |
| test_ppm | 59 passed, 0 failed |
| **test_refraction** | **70 passed, 0 failed** |
| test_render_threads | 4 passed, 0 failed |
| test_softshadow | 33 passed, 0 failed |
| test_texture | 57 passed, 0 failed |
| test_tree_params | 81 passed, 0 failed |
| test_water | 34 passed, 0 failed |

Both new binaries were also run individually (`./bin/test_refraction`,
`./bin/test_integration_refraction`): exit 0 each. `test_integration_refraction`
self-reports `refraction render: 820 distinct colours`, `beer_lambert 1 vs 0:
DIFFER`, `beer_lambert=0 absorption A vs 0: IDENTICAL`.

## 3. `type = glass` preset semantics — PASS

Independently parsed `scenes/example_glass.scene` with a scratch program
(`/tmp/audit/parse_glass.c`, linked against the project objects) calling
`scene_desc_load()`. Observed for the bare `type = glass` material
(`glass_clear`), matching `sd_glass_preset()` exactly:

```
material 'glass_clear':
  albedo       = 0.02 0.02 0.02
  specular     = 1 1 1
  shininess    = 256
  reflectivity = 0
  transparency = 1
  ior          = 1.5
  is_water     = 0
  beer_lambert = 0
  absorption   = 0 0 0
  deep_color   = 0.5 0.5 0.5
  texture_kind = 0
```

Explicit-key override also verified independently: `type = glass` + `ior = 2.4`
yields ior 2.4 with all other preset fields intact (`glass_diamond`).
Additional scratch edge checks (`/tmp/audit/edge.c`):
- `type = diamond` -> rc=1, `error: unknown material type, expected 'water',
  'opaque' or 'glass'`.
- `beer_lambert = 2` -> rc=1, `error: expected 0 or 1`.
- `type = glass` + `transparency = 0.5` -> transparency 0.5, rest preset intact.
- `type = glass` + `is_water = 1` -> is_water 1 (explicit key wins).

## 4. `beer_lambert` decoupling — PASS

Three scratch scenes identical except for the flag/absorption
(`/tmp/audit/{A,B,C}.scene`; same glass sphere in front of a striped wall):

- A: `beer_lambert = 0`, `absorption = 1.3 0.2 1.1`
- B: `beer_lambert = 1`, `absorption = 1.3 0.2 1.1`
- C: `beer_lambert = 0`, `absorption = 0 0 0`

Rendered at `--width 200 --height 120 --samples 4`:

```
A.bmp  md5 e299908f8c446cf0f5bba0009d6246cc
B.bmp  md5 55a871600d79f0e9f31f173988c8af41
C.bmp  md5 e299908f8c446cf0f5bba0009d6246cc

cmp A.bmp B.bmp -> DIFFER   (beer_lambert=1 with nonzero absorption changes output)
cmp A.bmp C.bmp -> IDENTICAL (beer_lambert=0 ignores nonzero absorption)
cmp B.bmp C.bmp -> DIFFER
```

This confirms both required properties: enabling `beer_lambert` with nonzero
absorption changes the image, and `beer_lambert = 0` with nonzero absorption is
byte-identical to zero absorption.

## 5. Byte-identity regressions — PASS

### 5a. Default render vs pre-change build — PASS

Baseline used and why it is valid: HEAD (`61e3f21`) predates the refraction work
*and* unrelated in-progress work, so it is not a clean "pre-refraction working
tree". Two baselines were therefore used:

1. **HEAD tree** (`git archive HEAD | tar -x -C /tmp/base_rt`, built there).
2. **Surgically reverted working tree**: a copy of the working tree
   (`/tmp/work_norefract`) with only the two gates reverted
   (`sed 's/if (m->is_water || m->beer_lambert) {/if (m->is_water) {/g'
   src/render.c`) — i.e. the exact pre-refraction refraction code path, with all
   other features retained.

```
# working tree vs HEAD, default render
work_default.bmp md5 be83bf960e8e70d10fceac314a82e26c
base_default.bmp md5 be83bf960e8e70d10fceac314a82e26c
cmp -> IDENTICAL

# working tree vs reverted-gate working tree, default render
cmp /tmp/work_default.bmp /tmp/noref_default.bmp -> IDENTICAL
```

Both baselines agree, so the refraction change is byte-neutral for the default
scene at `--width 160 --height 90 --samples 4`.

### 5b. `scenes/default.scene` vs `DEFAULT_SCENE_TEXT` — PASS

Extracted the embedded string with a scratch program
(`/tmp/audit/extract_embed.c`, `fwrite(DEFAULT_SCENE_TEXT, 1, sizeof-1)`) and
compared to the file:

```
embedded.scene  3895 bytes  md5 d86ce8952a464887ccb9e721b15fad4e
scenes/default.scene 3895 bytes  md5 d86ce8952a464887ccb9e721b15fad4e
cmp -> BYTE IDENTICAL
```

Rendering the embedded default and `scenes/default.scene` also produced
byte-identical output (`/tmp/audit/def_embed.bmp` == `/tmp/audit/def_file.bmp`).
Note: `--write-scene` emits its own one-line canonical header comment
(`# scene description (canonical)`) instead of the file's 10-line comment block;
the *body* is identical. This is expected writer behaviour, not a discrepancy,
and does not affect the required embedded-vs-file byte-identity.

### 5c. `type = water` unchanged — PASS

Parsed a water scene (`/tmp/audit/parse_water.c` on `scenes/example_sunset.scene`):

```
'pond': is_water=1 beer_lambert=0 transparency=0.8 ior=1.33 refl=1
```

Rendering `scenes/example_sunset.scene` (a water-using scene) at
`--width 160 --height 90 --samples 4`:

```
work_water.bmp   md5 6df1fa1c92e6026c4be58d1efe8081e8
noref_water.bmp  md5 6df1fa1c92e6026c4be58d1efe8081e8
cmp -> IDENTICAL
```

Water still sets `is_water == 1`, `beer_lambert == 0`, and renders identically
to the pre-refraction baseline.

## 6. `scenes/example_glass.scene` — PASS

Parses cleanly (rc=0, no error) into 7 materials and 7 primitives. Rendered at
`--width 320 --height 180 --samples 8`:

```
Scene: 7 primitives, 7 materials
Wrote /tmp/audit/glass.bmp (172854 bytes)   # 320x180x3 + 54-byte header
```

Built a scratch copy with the three glass spheres removed
(`/tmp/audit/glass_noglass.scene`, 4 primitives) and rendered identically:

```
glass.bmp    distinct colours: 4316
noglass.bmp  distinct colours: 1995
cmp glass.bmp noglass.bmp -> DIFFER
```

The glass objects materially alter the output and the image is non-trivial.

## 7. Determinism — PASS

```
# single-threaded, same scene twice
det1.bmp == det2.bmp            (glass scene, 200x120/4)  -> ST DETERMINISTIC
dd1.bmp == dd2.bmp              (default scene, 160x90/4) -> ST DEFAULT DETERMINISTIC

# threaded build (make threads)
tdet1.bmp == tdet2.bmp          -> THREADED DETERMINISTIC
tdet1.bmp == det1.bmp           -> THREADED == SINGLE-THREADED
```

Threaded output is byte-identical both across runs and to the single-threaded
output.

---

## Implementation spot-checks (read-only source inspection)

- `src/material.h`: `int beer_lambert;` present on `Material` (after `deep_color`),
  with a comment documenting the `is_water` decoupling.
- `src/scene_desc.c`: `MAT_BEER_LAMBERT = 1u << 14` and the `beer_lambert`
  `KT_BOOL` KeySpec (`offsetof(Material, beer_lambert)`) present; `type = glass`
  sets `mat_type = 3`; `sd_glass_preset()` matches the observed values; the
  overlay `sd_overlay_explicit()` copies `MAT_BEER_LAMBERT` so explicit keys
  override the preset.
- `src/scene_desc_write.c`: `mat_is_glass_preset()` matches `sd_glass_preset()`
  field-for-field; a glass-equal material emits exactly `type = glass`;
  `beer_lambert` is emitted only when non-zero, positioned between `is_water`
  and `absorption`.
- `src/render.c`: exactly two gates are `if (m->is_water || m->beer_lambert)`
  (lines 262 and 272); the water-normal gate at line 138 remains the plain
  `if (m->is_water)`, so `beer_lambert` does **not** enable wave perturbation.

## Conclusion

Every claim in the implementation brief was independently reproduced from
source. All builds are warning-free, the full suite passes, the glass preset and
`beer_lambert` semantics are correct, byte-identity of the default scene, the
embedded text, and water behaviour is preserved, the new scene renders
non-trivially, and output is deterministic. **Overall VERDICT: PASS.**
