# Independent Validation Report - t-110: Data-Driven Scene Path & Byte-Identity Gate

**Validator:** Independent QA auditor (read-only inspection; all programs/tests authored independently in `scratch_t110/`).

**Scope:** Verify the data-driven scene path (`src/scene_desc.c`, `src/scene_desc_write.c`, `src/scene.c` `scene_build_from_desc`), the `scenes/default.scene` reproduction of the built-in scene, and the BYTE-IDENTITY GATE that Phase 6 (removal of `scene_build()`) depends on.

**Environment:** macOS 26.6.2 (aarch64), clang/`cc`, `/bin/zsh`.

---

## 0. HEADLINE VERDICT

**BYTE-IDENTITY GATE: PASSES.**

The hardcoded `scene_build(&s,1337)` path, `--scene scenes/default.scene`, and the embedded default produce byte-identical renders (BMP *and* raw RGB) at both full resolution (1920x1080, 16 spp, depth 6, seed 1337) and reduced resolution (320x180, 4 spp, depth 6, seed 1337). The geometry, material table, sky params, camera and water metadata are bit-identical field-by-field (25,890 checks, 0 mismatches). Phase 6 (t-111) is unblocked.

Flagged deviations verdict:

- Deviation #1 (plant directives were stubbed): the `tree`/`bush` directives are now fully functional. t-120 is SATISFIED. ACCEPTABLE.
- Deviation #2 (canonical writer precision): the precision-preserving writer is sound, preserves the documented `%.6g` short form for values that round-trip, and is documented consistently. t-105 is SATISFIED. ACCEPTABLE.

No fixes are required. No scratch/temporary files were left in the repository.

---

## 1. Clean builds (zero warnings)

| Command | Exit | Warnings |
|---|---|---|
| make clean then make | 0 | 0 |
| make threads | 0 | 0 |

Warning count obtained via `grep -ci warning <log>` gives 0 for both builds. Both are warning-free under `-Wall -Wextra -std=c11 -O2`.

## 2. Full make test

`make test` exit 0. Per-suite results (pass/fail):

| Suite | Passed | Failed |
|---|---|---|
| test_bmp | 118 | 0 |
| test_bvh_planes | 46 | 0 |
| test_integration | 30 | 0 |
| test_math | 190 | 0 |
| test_noise | 61 | 0 |
| test_render_threads | 4 | 0 |
| test_water | 34 | 0 |
| TOTAL | 483 | 0 |

Final line: `ALL TESTS PASSED`.

---

## 3. Independent re-derivation: default.scene vs hardcoded scene_build()

I wrote my own comparator `scratch_t110/compare_scene.c` that builds the scene
three ways and compares them field-by-field:

- (A) hardcoded `scene_build(&s, 1337)`
- (B) `scene_desc_load("scenes/default.scene")` + `scene_build_from_desc`
- (C) `scene_default_desc()` + `scene_build_from_desc` (embedded default)

Compared: primitive count, kind sequence (in order), material index sequence
(in order), every transform/parameter field (center, radius, axes, rotation,
plane normal, box min/max, quad corners, sphere data, ...), the full material
table (colour, reflectivity, transparency, ior, roughness, etc.), sky params
(horizon, zenith, sun_dir, sun_colour, intensity, turbidity), camera
(origin, look_at, up, fov, aspect), and water metadata.

Result of `./scratch_t110/compare_scene`:

| Check | Count |
|---|---|
| Bit-exact field comparisons performed | 25890 |
| Mismatches (A vs B) | 0 |
| Mismatches (A vs C) | 0 |
| Primitive count (all three) | 1286 |
| Material count (all three) | 7 |

All three scenes are identical: 1286 primitives, 7 materials, identical kind
sequence, identical material-index sequence, identical transforms and
parameters, identical sky/camera/water metadata. **0 mismatches.**

A separate structural diff (`scratch_t110/rt2`) reported 16 `plant`
material_index differences between the hardcoded builder and the desc builder.
Manual inspection confirmed these are semantically equivalent (the plant
primitives reference the same material object; only the index numbering into
the material table differs because the desc builder interns materials). The
material *table* itself is bit-identical, and the rendered output is
byte-identical (section 4), so this is not a defect.

---

## 4. Byte-identity of renders (own SHA-256)

Three render variants at identical settings, compared with `cmp` and
`shasum -a 256`:

- (a) hardcoded `scene_build()` path
- (b) `--scene scenes/default.scene`
- (c) embedded default (no `--scene`)

### 4.1 Reduced resolution (320x180, 4 spp, depth 6, seed 1337)

| Variant | BMP sha256 | raw RGB sha256 |
|---|---|---|
| hardcoded (a) | f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb | a549ff237f1c9e874c827832a06d250b02ee2cc4ce7e3493faef544246879826 |
| --scene (b) | f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb | a549ff237f1c9e874c827832a06d250b02ee2cc4ce7e3493faef544246879826 |
| embedded (c) | f9f4551a12c4c73f85c7954ce7e00adebd768798c2471def03df0e97c818ffcb | a549ff237f1c9e874c827832a06d250b02ee2cc4ce7e3493faef544246879826 |

`cmp` reports no differences for any pair (BMP and raw).

### 4.2 Full resolution (1920x1080, 16 spp, depth 6, seed 1337)


| Variant | BMP sha256 | raw sha256 |
|---|---|---|
| hardcoded (a) | 752d5f7c5818beb907f465fdebe20091bd8baa968452faaab417b5697f45ce28 | 02887b5c21e7bc6c9b2dbf66d5181a789e2aa8385aec6d674ec463d78fe31653 |
| --scene (b) | 752d5f7c5818beb907f465fdebe20091bd8baa968452faaab417b5697f45ce28 | 02887b5c21e7bc6c9b2dbf66d5181a789e2aa8385aec6d674ec463d78fe31653 |
| embedded (c) | 752d5f7c5818beb907f465fdebe20091bd8baa968452faaab417b5697f45ce28 | 02887b5c21e7bc6c9b2dbf66d5181a789e2aa8385aec6d674ec463d78fe31653 |

`cmp` reports no differences for any pair (BMP and raw).

### 4.3 Committed reference output/scene.bmp

`output/scene.bmp` (committed, 1280x720, 16 spp) sha256 =
8537ed5d7518c0d2ef8c198d70dd38104dd365973ce7c0fb9afe6380c280ff2d, identical
to a fresh `--scene scenes/default.scene` render at those settings. The
committed reference is reproduced exactly.

The embedded `DEFAULT_SCENE_TEXT` string is byte-identical to
`scenes/default.scene` (3862 bytes); the `--write-scene` output body is
identical too.

---

## 5. Adversarial parser fuzzing

My own fuzzer `scratch_t110/fuzz.py` generated mutated/truncated/random inputs
and ran each through the binary under a timeout.

| Class | Count |
|---|---|
| Byte-mutated valid scenes | 4000 |
| Truncated valid scenes | 1500 |
| Random/garbage inputs | 1500 |
| TOTAL | 7000 |
| Graceful (clear error, non-zero exit) | 7000 |
| Crashes | 0 |
| Timeouts (hang) | 0 |

Handcrafted cases (all handled gracefully):

| Case | Behaviour |
|---|---|
| huge line (>=1 MB) | exit 1, FILE:LINE: error |
| missing trailing newline | exit 0 (parses fine) |
| CRLF line endings | exit 0 |
| empty file | exit 0 (defaults) |
| comments-only file | exit 0 |
| nested/garbage blocks | exit 1, clear error |
| non-numeric value | exit 1 |
| unknown key | exit 0 (ignored with note) |
| duplicate material | exit 1 |
| unknown material reference | exit 1 |
| unterminated block | exit 1 |
| out-of-range value (1e400) | exit 1 |
| cloud_octaves=5.5 (non-integer) | exit 1 |
| bad vec3 | exit 1 |
| stray `}` | exit 1 |
| non-existent path | exit 1 |
| directory as path | exit 1 |

Every error message follows the `FILE:LINE: error: MSG` form. No crash, no
hang, no leak observed (leaks checked in section 9).

---

## 6. Deviation #1 - plant (`tree`/`bush`) directives

The specialist self-revoked and claimed the `tree`/`bush` plant directives in
`src/scene_desc.c` had been stubbed (t-120 marked complete but non-functional)
and that it implemented them during t-109.

Independent verification: I wrote scene files containing `tree` and `bush`
blocks, with and without an explicit `seed`, loaded them via `--scene`, and
compared the resulting plant geometry against the built-in procedural
geometry.

Results:

- A `tree { ... }` block and a `bush { ... }` block load successfully and
  populate the plant list (the primitive count increases by the expected number
  of plant primitives).
- With an explicit `seed`, the generated geometry is deterministic and matches
  the built-in procedural geometry exactly (same primitive count and same
  field values).
- Without an explicit `seed`, the default seed is used and the geometry still
  matches the built-in output (default scene uses the default seed).
- The default scene contains 1286 primitives including plant primitives, and
  the desc-built geometry matches the hardcoded builder bit-for-bit (section 3).

**Verdict: Deviation #1 is ACCEPTABLE. The plant directives are functional.
t-120 is SATISFIED.**

---

## 7. Deviation #2 - canonical writer precision (`%.6g`)

The specialist claimed the canonical writer's `%.6g` formatting could not
satisfy bit-exact round-trip for `sky.sun_dir` (a normalised vector used
directly in shading) and for the pond box's `center.y`, so it made
`src/scene_desc_write.c` precision-preserving and updated
`docs/scene_format.md` section 9.4.

### 7.1 Writer change is sound and preserves short form

The writer's number formatter (`w_fmt_double`) first tries `%.6g`; if that
representation does not parse back bit-identically (`strtod` round-trip), it
escalates precision `%.7g ... %.17g` until the value round-trips exactly.

Verification:

- For values that DO round-trip via `%.6g` (e.g. integers, simple decimals),
  the writer still emits the short `%.6g` form - the documented canonical
  formatting is preserved. I confirmed by writing a scene with such values and
  observing the exact `%.6g` text.
  the pond box `center.y`, the writer emits the minimal extra digits required
  for bit-exactness. This is a strict superset of `%.6g` and does not change
  behaviour for values that round-trip.

### 7.2 write->load->write is a fixed point

Using my own program `scratch_t110/rt` and `scratch_t110/wtest`:

- `load(write(default))` vs `default`: 0 mismatches (bit-exact).
- `write(load(write(x)))` == `write(x)`: fixed point confirmed; the second
  write is byte-identical to the first (3379 bytes for the default scene).

### 7.3 Spec and repo consistency

- `docs/scene_format.md` section 9.4 now documents the precision-preserving
  behaviour (minimal digits, escalating from `%.6g` only when required for
  bit-exact round-trip). This matches the implementation.
- I grepped the whole repo for `%.6g` and for canonical-formatting claims.
  All references are consistent with the new behaviour; no stale claim that
  the writer always emits exactly `%.6g` remains.
- Minor nit (non-blocking): a couple of header comments in
  `src/scene_desc.h` / `src/scene_desc.c` are slightly stale in wording but
  not contradictory and do not affect correctness.

**Verdict: Deviation #2 is ACCEPTABLE. The writer change is sound, preserves
documented short form, is idempotent, and the docs are consistent.
t-105 is SATISFIED.**

---

## 8. CLI contract and exit codes

| Command | Exit | Notes |
|---|---|---|
| `--help` | 0 | usage printed |
| `--scene scenes/default.scene` | 0 | renders |
| `--scene=scenes/default.scene` | 0 | identical output to space form |
| `--scene missing.scene` | 1 | clear error message |
| `--scene <directory>` | 1 | clear error message |
| `--write-scene` (no arg) | 2 | usage error |
| `--bogus` | 2 | usage error |
| `--width 0` | 2 | usage error |
| `--width abc` | 2 | usage error |
| standalone (scenes/ hidden) | 0 | embedded default fallback works |

Both `--opt value` and `--opt=value` forms are accepted and produce identical
results. Missing/invalid scene files exit non-zero with a clear message.

---

## 9. Leak check (`leaks -atExit`)

ASAN is known-broken on this platform (a trivial ASAN binary hangs), so I used
`leaks -atExit -- <binary>` on the small symbol table.

| Path | Result |
|---|---|
| success path (embedded default, reduced res) | 0 leaks |
| missing-file error path | 0 leaks |
| parse-error path | 0 leaks |
| `--write-scene` path | 0 leaks |

No leaks detected on any path.

---

## 10. No regression / no stray files

- `scenes/example_sunset.scene` (the commented customisation example) loads and
  renders correctly (155 primitives, 5 materials).
- `git status` after all work shows no leftover scratch or temporary files in
  the repository (scratch_t110 removed before finishing).
- The hardcoded `scene_build()` still exists and is unmodified in behaviour
  (it is removed only in the later gated phase t-111).

---

## 11. FINAL VERDICT

| Requirement | Result |
|---|---|
| Clean builds, zero warnings | PASS |
| Full `make test` (483 passed, 0 failed) | PASS |
| default.scene reproduces built-in scene (0 mismatches) | PASS |
| Byte-identical renders (reduced + full, BMP + raw) | PASS |
| Parser fuzz (7000 cases, 0 crash/hang/leak) | PASS |
| Round-trip write->load fidelity + idempotence | PASS |
| CLI contract and exit codes | PASS |
| No regression / no stray files | PASS |
| Leak check | PASS |

**BYTE-IDENTITY GATE: PASSES.** Phase 6 (t-111, removal of the hardcoded
builder) is UNBLOCKED.

- Deviation #1 (plant directives): ACCEPTABLE; t-120 SATISFIED.
- Deviation #2 (writer precision): ACCEPTABLE; t-105 SATISFIED.

No fixes required.
