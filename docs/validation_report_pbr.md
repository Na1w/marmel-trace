# Independent validation report — PBR material layer

Auditor: independent read-only QA. No source/scene/test/doc file was modified; all scratch work lives under `/tmp`. Every result below was produced by building from source and running my own probes — no prior report or coder figure was trusted.

Environment: macOS (aarch64), `/bin/zsh`, CWD `/Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer`. Compiler: system `cc` (clang). Renders at 320x180/4spp unless noted.

## Baseline statement (check 6)

`HEAD` = `61e3f21 (V 0.1)`. The working tree carries large **uncommitted** changes, and `git archive HEAD` proves HEAD predates not only PBR but also the `texture` keys and the `type = glass` preset (the HEAD binary rejects `type = glass` with *"unknown material type, expected 'water' or 'opaque'"*). HEAD is therefore a valid true pre-PBR baseline for the default render and the water path, but **not** usable for the glass path. I used two baselines:

1. `git HEAD` built to `/tmp/baseline/raytracer` — the true pre-change commit.
2. A **reverted working tree** copied to `/tmp/revert`, whose only edit is `sed 's/    m.pbr = 1;/    m.pbr = 0;/' src/scene_desc.c` (4 preset sites), disabling every PBR preset while leaving all glass/texture/scene code intact. This isolates the PBR change exactly.

Both are valid: (1) is literally the committed pre-change code; (2) is the current tree minus PBR only, so any difference is attributable to PBR.

## Check 1 — clean build (PASS)

    make          -> exit 0 ; grep -ci warning = 0
    make threads  -> exit 0 ; grep -ci warning = 0

`-Wall -Wextra` clean for both the single-threaded and `-DUSE_PTHREADS` builds.

## Check 2 — full suite (PASS)

`make test` -> exit 0, final line `ALL TESTS PASSED`. Per-binary pass counts, all 0 failed:

| binary | passed | binary | passed |
|---|---|---|---|
| test_bmp | 118 | test_pbr | 76 |
| test_bvh_planes | 46 | test_ppm | 59 |
| test_dof | 40 | test_refraction | 70 |
| test_integration | 30 | test_render_threads | 4 |
| test_integration_features | 58 | test_softshadow | 33 |
| test_integration_pbr | 86 | test_texture | 57 |
| test_integration_refraction | 66 | test_tree_params | 81 |
| test_material_presets | 140 | test_water | 34 |
| test_math | 190 | test_noise | 61 |

Total **1249 checks, 0 failed**. `test_pbr` also prints `max hemispherical reflectance = 0.74825 (<= 1 required)` — this uses `albedo = 0.5`; see check 3 for why that figure is not the whole story.

## Check 3 — BRDF correctness (PARTIAL: 3/4 pass, energy claim FAILS)

Estimator: my own `/tmp/est7.c`, `/tmp/est8.c`, `/tmp/est9.c` — deterministic midpoint quadrature of `rho = integral shade_pbr * sin(theta) dtheta dphi` (1200x1200 samples) over the hemisphere, `N=(0,1,0)`, unit light colour.

* **Dielectric F0 ~ 0.04 — PASS.** `fresnel_schlick_rgb(1.0, vec3(0.04))` = `0.040000 0.040000 0.040000`. With `metallic=0, albedo=0`, the pure specular at `roughness=0.2` is `1.989437`, exactly the closed form `F0*D*G/(4*N*L*N*V)`; adding `albedo=0.5` adds exactly `0.159155 = 0.5/pi` (the Lambert term).
* **Roughness sharpens the specular peak — PASS.** Mirror config (N=L=V, metallic=1, albedo=1): L_o = 7.96e6 (r=0), 12732.4 (0.05), 49.74 (0.20), 1.273 (0.50), 0.121 (0.90) — monotonic, D proportional to 1/alpha^2 as documented.
* **metallic=1 removes the diffuse term — PASS.** `metallic=1, albedo=0` returns exactly `0.000000`; `metallic=1, albedo=0.5` returns `24.867960`, which is *pure* specular (scaled by `F0=0.5`: `1.989437 * 0.5/0.04 = 24.868`), with no diffuse contribution.
* **Energy conservation (rho <= 1 over a sweep) — FAIL.** The shipped presets are individually fine (plastic 0.4317, rubber 0.4958, ceramic 0.9735, diamond 0.0333, gold 0.9555, emissive 0.2671 — all <= 1). **However** the model is *not* universally energy-conserving:

      dielectric alb=0.85, roughness sweep 0.1..1.0 : max_rho = 1.307550 (5/20 > 1)
      white dielectric alb=1.0, roughness 0.5        : max_rho = 1.267093 (4/4 > 1)
      example_materials.scene ceramic_red
          (albedo 0.72, roughness 0.55, metallic 0)  : max_rho = 1.019193 (1/4 > 1)

  Root cause: `material_shade_pbr` (src/material.c:197) adds a **full** Lambert diffuse `f_diff = (1-metallic)*albedo/pi` on top of the Cook-Torrance specular, with **no `(1-F)` energy-compensation factor** (kD). For a dielectric the diffuse integrates to `albedo` (here up to 1.0) and the specular lobe adds on top, so rho can exceed 1 whenever `albedo` is high. This contradicts the unqualified claim in three docs (check 9), and one of the **delivered example scenes** (`scenes/example_materials.scene`, `ceramic_red`) already trips it.

## Check 4 — schema / writer (PASS)

Probes `/tmp/probe.c`, `/tmp/probe2.c`, `/tmp/probe3.c` (linked against the real object files) loaded scenes and dumped every `Material` field:

* `metallic = 0.7`, `roughness = 0.42`, `emissive = 0.5 0.6 0.7`, `pbr = 1` all parse to exactly those values.
* A material with none of the keys defaults to `metallic 0, roughness 0, emissive 0 0 0, pbr 0`.
* `type = gold` + `roughness = 0.30` keeps the gold preset (albedo `1,0.766,0.336`, `metallic 1`, `pbr 1`) and overrides only `roughness -> 0.3`.
* Writer emits the four keys **only when non-default**: `type = gold`/`diamond` round-trip back to the bare `type = <name>` shorthand; a material with only `albedo` (and even one with explicit `metallic 0 / roughness 0 / emissive 0 0 0 / pbr 0`) emits **neither** the PBR keys nor a `type` line.
* Round-trip: reloading the written file reproduces **byte-equal `Material` structs** (`memcmp == 0` for all materials).

## Check 5 — named presets (PASS)

`/tmp/probe2.c` parsed each of the 12 new keywords as `type = <name>` and dumped the full struct; every value matches `docs/research_material_reference.md`:

    gold     albedo 1.000 0.766 0.336  metallic 1 rough 0.05 pbr 1
    copper   albedo 0.955 0.637 0.538  metallic 1 rough 0.05 pbr 1
    silver   albedo 0.972 0.960 0.915  metallic 1 rough 0.03 pbr 1
    aluminum albedo 0.913 0.921 0.925  metallic 1 rough 0.05 pbr 1
    iron     albedo 0.560 0.570 0.580  metallic 1 rough 0.10 pbr 1
    chrome   albedo 0.550 0.556 0.554  metallic 1 rough 0.03 pbr 1
    brass    albedo 0.910 0.778 0.423  metallic 1 rough 0.08 pbr 1
    plastic  albedo 0.30 0.05 0.06  spec 0.05 ior 1.46 shin 64  rough 0.10
    rubber   albedo 0.05 0.05 0.05  spec 0.04 ior 1.50 shin 8   rough 0.90
    ceramic  albedo 0.85 0.85 0.82  spec 0.05 ior 1.60 shin 128 rough 0.20
    diamond  albedo 0.02 0.02 0.02  transp 1 ior 2.417 deep 0.5  rough 0.0
    emissive albedo 0 0 0  emissive 1.0 0.85 0.65  rough 0.5  pbr 1

All conductors share `specular = albedo`, `shininess 256`, `reflectivity 0`, `ior 1`, `is_water 0`, `beer_lambert 0`. Preset-with-override falls back to the preset for non-overridden keys (verified). An **unknown type is a hard error**:

    material m { type = unobtainium }
    -> error: unknown material type, expected one of 'water', 'opaque',
       'glass', 'gold', ... 'emissive'   [rc=1]

## Check 6 — byte-identity regressions (PASS)

* **6a Default render.** `./raytracer --width 320 --height 180 --samples 4` (no `--scene`) is **byte-identical** to both baselines (`cmp` clean) for the new, HEAD and reverted builds.
* **6b Default scene text.** Compiled `DEFAULT_SCENE_TEXT` to a file and `diff`ed: **byte-identical** to `scenes/default.scene` (both 3895 bytes). `--write-scene` body matches `scenes/default.scene` from `water_level` onward; only the leading descriptive comment block differs (expected — the writer emits its own canonical header).
* **6c water / glass refraction.** Minimal `type = water` (with `water_enabled`) and `type = glass` scenes render **byte-identical** to the reverted baseline. (HEAD could not be used for glass — it cannot parse the keyword — which is itself proof HEAD is pre-change.)

## Check 7 — example scenes (PASS)

`scenes/example_materials.scene` and `scenes/example_presets.scene` parse with **no errors/warnings** and render non-trivially (43 KB BMPs). Effect probes (compare against `/tmp` variants, PPM byte MAD):

* flatten the roughness gradient (all 0.40): differs from original, MAD 0.387, 2214 bytes differing by > 2 — the gradient is real.
* set the lamp `emissive = 0 0 0`: differs, MAD 3.331, 1194 bytes > 2 — the emitter genuinely lights its own surface.

## Check 8 — determinism (PASS)

* single-threaded: two runs of `example_materials.scene` byte-identical.
* threaded (`--threads`): two runs of `example_presets.scene` byte-identical.
* single-threaded output == threaded output (byte-identical).

## Check 9 — docs consistency (PARTIAL: 1 inaccurate claim)

`README.md`, `docs/scene_format.md`, `docs/render_notes.md` section 6.5 describe the keys, the `pbr` gate, the `mix(0.04, albedo, metallic)` F0, the `(1-metallic)` diffuse weight, the emissive semantics and the 15 presets accurately — all independently reproduced above. The prior-flagged **README threading staleness is now fixed** (README correctly describes 16x16 dynamic tile scheduling and the `RAYTRACER_THREADS` override).

**Inaccurate claim (contradicts the code):** all three docs assert energy conservation *without qualification*:

* `README.md`: "an energy-conserving metallic/roughness Cook-Torrance microfacet model";
* `docs/scene_format.md`: "The model is energy-conserving (the reflected radiance never exceeds the incident light)";
* `docs/render_notes.md` section 6.5: "the combined BRDF integrated over the hemisphere never exceeds 1".

Check 3 shows this is false for dielectrics with high albedo (rho up to 1.31), including the delivered `scenes/example_materials.scene` (`ceramic_red`, rho=1.019). The claim holds only for conductors (metallic=1) and low-albedo dielectrics.

## Summary

| # | Check | Result |
|---|---|---|
| 1 | clean `make` / `make threads` | PASS |
| 2 | `make test` (1249 checks, 0 fail) | PASS |
| 3 | BRDF: F0, roughness, metallic | PASS |
| 3 | BRDF: energy conservation <= 1 over sweep | FAIL |
| 4 | schema / writer / round-trip | PASS |
| 5 | 12 named presets, override, unknown error | PASS |
| 6 | default / default.scene / water / glass byte-identity | PASS |
| 7 | example scenes parse + effects visible | PASS |
| 8 | determinism single + threaded | PASS |
| 9 | docs consistency | PASS except energy claim |

## VERDICT: REJECTED

All functional wiring, parsing, writer, presets, byte-identity regressions, determinism and the test suite pass. The single blocking defect is the **energy-conservation claim**: `material_shade_pbr` is **not** energy-conserving for dielectrics — its Lambert term is added at full weight with no `(1-F)` compensation, so directional-hemispherical reflectance exceeds 1 for albedo above roughly 0.6 (measured up to 1.3076; the delivered `example_materials.scene` `ceramic_red` reaches 1.0192). This contradicts the unqualified claim repeated in `README.md`, `docs/scene_format.md` and `docs/render_notes.md` section 6.5.

Actionable fix (either):

* (a) add the standard energy compensation `kD = (1 - F)*(1 - metallic)` (or a `1 - F` diffuse weight) in `material_shade_pbr`, restoring rho <= 1; **or**
* (b) if the current behaviour is intended, correct all three docs to state the model is energy-conserving for conductors and low-albedo dielectrics only, and that high-albedo dielectrics may exceed unity.
