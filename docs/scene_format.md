# Scene Description Format — FROZEN SPECIFICATION

**Status:** FROZEN (v1). This document is the authoritative grammar for the
human-editable scene file consumed by the raytracer at startup.
**File extension (recommended):** `.scene`
**Default path (recommended):** `scenes/default.scene`
**Parser entry point (proposed, not implemented here):** `scene_desc_parse`
**Writer entry point (proposed, not implemented here):** `scene_desc_write`
**Applies to:** the C11 raytracer in this repository (`src/scene.c`,
`src/material.h`, `src/geometry.h`, `src/main.c`).

> This is a **specification document only**. It contains no C code and adds no
> third-party dependencies. The format is designed to be parsed with only
> `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>` and `<errno.h>` — all
> C11 standard library.

---

## 1. Rationale & Design Goals

The raytracer currently constructs its scene procedurally inside
`scene_build()` (`src/scene.c`). We want the *same* scene to be describable by
a human-editable text file that the renderer parses at startup.

Design goals, in priority order:

1. **Line-oriented.** Every meaningful token lives on exactly one physical
   line. A parser can be a single loop over `fgets()`/`getline()` plus a small
   number of numeric conversions. No recursive-descent grammar engine, no
   lookahead beyond one line.
2. **Human-editable.** A person can open the file in any editor, add a sphere,
   tweak the sun color, or move a tree, without reading C source.
3. **Trivially parseable in C, zero dependencies.** Only the C11 standard
   library is used. No regex, no YAML/JSON/TOML library, no generated tables.
4. **Diff-friendly.** One attribute per line; stable canonical key order (see
   §9) means a `git diff` after a round-trip shows only real changes.
5. **Forward-compatible.** Unknown-but-well-formed keys can be reported as
   warnings and skipped in a future lenient mode (see §6); the grammar reserves
   namespace for new blocks/keys without breaking existing files.
6. **Deterministic.** Given the same file and the same `--seed`, the parser
   produces a byte-identical scene (same primitive order, same `prim_index`
   assignment), so the BVH and any tie-break are reproducible.

**Non-goals:** expressions, arithmetic, variables, includes, conditionals,
arrays of anonymous primitives, anchors/aliases. If a value is not a literal,
it does not belong in this format.

---

## 2. Lexical Rules

| Concern | Rule |
|---|---|
| **Encoding** | ASCII is required for all keywords, punctuation and numbers. UTF-8 is accepted only inside quoted strings/names; any byte ≥ 0x80 inside a quoted string is passed through verbatim (the parser must not assume a multibyte encoding). Bytes ≥ 0x80 **outside** a quoted string are a lexical error. |
| **Line endings** | Both LF (`\n`) and CRLF (`\r\n`) are accepted. A lone CR (`\r`) is treated as ordinary whitespace. A UTF-8 BOM (`EF BB BF`) at the very start of the file is skipped silently (and may optionally be re-emitted by the writer; see §9). |
| **Comment character** | `#`. A `#` that is the first non-whitespace character on a line comments out the whole line. A `#` appearing after a token begins a trailing comment and runs to end of line. A `#` inside a **double-quoted string** is literal and does **not** start a comment. |
| **Comments allowed** | On their own line (full-line), or trailing after any complete token sequence (key/value line, block header, closing brace). A comment may **not** appear in the middle of a numeric triple or between a key and its `=`. |
| **Whitespace / tabs** | Space (0x20) and horizontal tab (0x09) are equivalent separators. Leading and trailing whitespace on a line is insignificant. Multiple separators collapse to one. Blank-only lines are ignored. |
| **Case sensitivity** | **Keywords and keys are case-sensitive and lower-case** (`camera`, `albedo`, `water_level`). Material names and other identifiers are case-sensitive. `Camera` ≠ `camera`. The writer always emits lower-case keywords. |
| **Max line length** | The parser must accept lines up to **4096 bytes** including the terminator. A longer physical line is a **hard error** (`E_LINE_TOO_LONG`, §6) reporting the 1-based line number. (Rationale: bounds a fixed `char buf[4096]` reader; no dynamic line buffer required.) |
| **Blank lines** | Freely allowed anywhere, including between keys and inside a block. No semantic effect. |
| **Trailing newline** | **Not required.** The final line may or may not end in `\n`; the parser treats a final unterminated line exactly like a terminated one. |
| **Number literals** | A number is an optional sign, digits, optional fraction, optional exponent: `[-+]?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?` or `[-+]?\.[0-9]+(...)`. Parsed with `strtod()`. `NaN`, `inf`, hex (`0x…`) and leading `+`-only tokens are **not** valid numbers. |
| **Identifiers** | A material name or block name is `[A-Za-z_][A-Za-z0-9_]*`, **or** any double-quoted string (`"…"`). See §3. |
| **Quoted strings** | Delimited by `"`. Escape sequences `\"`, `\\`, `\n`, `\t` are recognised; any other backslash sequence is a lexical error. A quoted string must be terminated on the same line (no multi-line strings). |

---

## 3. Syntactic Rules

### 3.1 Statement kinds

A file is a sequence of **top-level statements**, one per line. Exactly two
statement forms exist:

1. **Block header** — opens a block:
   ```
   <keyword> {
   <keyword> <name> {
   ```
2. **Key/value line** — a setting, valid only *inside* a block:
   ```
   <key> = <value>
   ```

A **closing brace** `}` on its own line closes the currently open block.

### 3.2 Block syntax

- A block is `keyword {` … body … `}`. The `{` is **required** and must appear
  on the same line as the keyword. The `}` is **required**, on its own line.
- Named blocks (only `material`) use `material <name> {`.
- Block body lines are indented with **4 spaces** in canonical output
  (indentation is cosmetic; the parser ignores it).
- Blocks **may not nest**. This grammar is **exactly one level deep**: a block
  body contains only `key = value` lines and blank/comment lines. A `{` inside a
  block body is a hard error (`E_NESTED_BLOCK`).
- The top level contains only block headers, `}` (which is an error at top
  level unless a block is open), blank lines and comments.

### 3.3 Key/value syntax

- Form: `key = value`. Whitespace around `=` is optional. `=` is required.
- The key must be a bare lower-case identifier (no quoting of keys).
- A key may appear **at most once per block**; a duplicate key in the same
  block is a hard error (`E_DUPLICATE_KEY`, §6).
- A key/value line outside any block is a hard error (`E_KEY_OUTSIDE_BLOCK`).

### 3.4 Value forms

| Form | Syntax | Example |
|---|---|---|
| Scalar | one number | `shininess = 256` |
| Vec3 | three numbers separated by whitespace | `albedo = 0.05 0.15 0.20` |
| Integer | one number with no fractional/exponent part | `cloud_octaves = 5` |
| Boolean-ish flag | `0` or `1` (integers) | `is_water = 1` |
| Name reference | bare identifier or quoted string | `material = water` |
| String | double-quoted | `name = "pond edge"` |

- **No separators.** `,` and `;` are **not** allowed anywhere. A comma or
  semicolon is a lexical error (`E_UNEXPECTED_CHAR`). Components of a Vec3 are
  separated by whitespace only: `0.25 0.5 1.0`.
- **A Vec3 must have exactly three components on the same line.** Two or four
  components is a hard error (`E_BAD_VEC3`).
- Numbers that are conceptually 1-component (`vfov`, `shininess`, `seed`) must
  have exactly one component.
- Integer-typed keys (`cloud_octaves`, `is_water`, and all `seed`/count keys)
  must be written without a fraction; `5.0` is rejected (`E_BAD_INTEGER`).
  Negative zero is accepted and normalised to `0`.

### 3.5 Nesting

**One level only.** There is no nested block, no array, no repeated block of the
same keyword except for `material` (which may repeat, each with a distinct name)
and the primitive/plant blocks (which may repeat freely). This is stated
explicitly so an implementer does not build a recursive grammar.

---

## 4. Complete Block & Key Reference

Notation: **Type** = `f64` scalar, `vec3` = three f64, `int`, `bool` (0/1),
`name`. **Req** = R (required) / O (optional). Ranges are validated per §6.
"Default" is the value used when the key is omitted **and** the block is
otherwise present.

### 4.1 `camera` — block, at most one

The camera framing. If the block is absent, `scene_default_view()` values are
used (see §8 for the built-in fallback and the full mapping).

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `eye` | vec3 | world | any finite | `-18 6 22` | O |
| `target` | vec3 | world | any finite | `0 5 -20` | O |
| `up` | vec3 | world | non-zero | `0 1 0` | O |
| `vfov` | f64 | degrees | `(0, 180)` | `40` | O |
| `aperture` | f64 | world | `>= 0` | `0` (pinhole) | O |
| `focus_distance` | f64 | world | `>= 0` | `0` → derive `|target - eye|` | O |

- `up` must be non-zero after normalisation, and must not be parallel to
  `target - eye`; otherwise hard error (`E_DEGENERATE_CAMERA`).

**Depth of field (`aperture` / `focus_distance`):** a thin-lens camera model.
`aperture` is the **radius of the lens disk** in world units. When it is `0`
(the default) the camera is a perfect pinhole and primary rays are generated
exactly as before. When it is greater than `0`, each primary ray's origin is
jittered to a uniform point on the lens disk of radius `aperture`, and the ray
is aimed at the point where the ideal pinhole ray crosses the **focal plane** at
distance `focus_distance` in front of the eye. Geometry at that distance stays
perfectly sharp; nearer/farther geometry is blurred in proportion to the
aperture — classic depth of field. The jitter is fully deterministic: it derives
only from the pixel index, the sample index, and two dedicated hash-PRNG
channels **distinct from the anti-aliasing jitter channels** (no `rand()`, no
clock), so repeated renders are byte-identical. `focus_distance` is a world
distance from the eye; when it is omitted (or `0`) the camera derives it as the
distance to the `target` (`|target - eye|`), i.e. the look-at point is in focus
by default. The canonical writer emits both keys **only when they differ from
their defaults**, so the built-in scene emits no `aperture`/`focus_distance`
lines and `scenes/default.scene` remains byte-identical.
- **Aspect ratio is NOT stored in the file.** The camera aspect is always the
  render's true output aspect `width/height` from the CLI, exactly as
  `main.c` does today (`camera_create(eye, target, up, vfov, width/height)`).
  This is a deliberate decision: the file describes *geometry/framing*, the CLI
  describes *output resolution*. A file key `aspect` is reserved but has no
  effect in v1 and must be reported as unknown (warning, §6).

### 4.2 `sky` — block, at most one

Maps 1:1 onto `SkyParams` (`src/material.h`). Field names are the **real**
struct field names. If the block is absent, `sky_default_params()` fills every
field (see §8). The `seed` field here is the *cloud noise* seed.

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `sun_dir` | vec3 | unit direction | non-zero (normalised on load) | `0.45 0.75 -0.50` (normalised) | O |
| `sun_color` | vec3 | linear RGB | ≥ 0, may exceed 1 | `1.00 0.95 0.85` | O |
| `horizon_color` | vec3 | linear RGB | ≥ 0 | `0.75 0.85 1.00` | O |
| `zenith_color` | vec3 | linear RGB | ≥ 0 | `0.35 0.55 0.95` | O |
| `gradient_gamma` | f64 | — | `(0, 4]` | `0.6` | O |
| `sun_glow_exponent` | f64 | — | `> 0` | `350` | O |
| `sun_glow_strength` | f64 | — | `[0, ∞)` | `0.8` | O |
| `cloud_height` | f64 | world | `> 0` | `120` | O |
| `cloud_scale` | f64 | 1/world | `> 0` | `0.0025` | O |
| `cloud_coverage` | f64 | — | `[0, 1]` | `0.5` | O |
| `cloud_softness` | f64 | — | `[0, 1]` | `0.12` | O |
| `cloud_sharpness` | f64 | — | `> 0` | `1.5` | O |
| `cloud_octaves` | int | — | `[1, 12]` | `5` | O |
| `seed` | int | — | `[0, 2^32-1]` | the CLI `--seed` value | O |
| `sun_radius` | f64 | degrees | `>= 0` | `0` (hard shadow) | O |
| `star_intensity` | f64 | — | `[0, ∞)` | `0.0` (off) | O |
| `star_density` | f64 | — | `> 0` | `250.0` | O |
| `nebula_intensity` | f64 | — | `[0, ∞)` | `0.0` (off) | O |
| `nebula_dir` | vec3 | direction | non-zero | `-0.1059 -0.2060 0.9728` | O |
| `galaxy_intensity` | f64 | — | `[0, ∞)` | `0.0` (off) | O |
| `galaxy_dir` | vec3 | direction | non-zero | `-0.35 0.45 0.82` | O |
| `galaxy_tilt` | f64 | degrees | `[0, 90]` | `50.0` (inclination) | O |
| `galaxy_roll` | f64 | degrees | any | `-38.0` (orientation) | O |

**Soft shadows (`sun_radius`):** the angular radius of the sun disk. When it
is greater than `0`, the renderer samples the direct sunlight over a cone of
this half-angle around `sun_dir`, casting a fixed number of shadow rays per lit
hit and averaging the unoccluded fraction — producing a soft (penumbral)
shadow. The default `0` denotes a point-like sun and keeps the original single
hard-shadow ray (and therefore the byte-identical default render). Sampling is
fully deterministic: the jitter derives only from the pixel and sample indices
via the renderer's hash PRNG (no `rand()`, no clock). The canonical writer
emits this key **only when it differs from its default** (`0`), so the built-in
scene emits no `sun_radius` line and `scenes/default.scene` remains
byte-identical.

**Seed interaction:** if the `sky.seed` key is omitted, the parser sets it to
the CLI `--seed` value (mirroring `s->sky.seed = seed;` in `scene_build`). If
present, the file value wins and the CLI seed does **not** override it. The writer always emits the resolved value so round-trips are stable.

### 4.2.1 `fog` — block, at most one

Atmospheric fog, ground mist, and volumetric smoke. When omitted (or `density = 0`),
fog is disabled with zero performance overhead and byte-identical output to legacy
renders.

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `density` | f64 | 1/world | `[0, ∞)` | `0.0` (disabled) | O |
| `color` | vec3 | linear RGB | `≥ 0` | `0.70 0.75 0.80` | O |
| `height` | f64 | world | any finite | `0.0` | O |
| `height_falloff` | f64 | 1/world | `[0, ∞)` | `0.0` (uniform) | O |
| `inscatter_strength` | f64 | — | `[0, ∞)` | `0.50` | O |
| `sun_anisotropy` | f64 | — | `(-1, 1)` | `0.70` (forward Mie) | O |
| `noise_scale` | f64 | 1/world | `[0, ∞)` | `0.0` (analytic) | O |
| `noise_amount` | f64 | — | `[0, 1]` | `0.0` | O |

* **Uniform distance fog:** when `height_falloff = 0`, transmittance attenuates exponentially with line-of-sight distance: $T(d) = e^{-\text{density} \cdot d}$.
* **Exponential height fog:** when `height_falloff > 0`, density decays exponentially with height: $\rho(y) = \text{density} \cdot e^{-\lambda (y - \text{height})}$. Integrated analytically along every ray in closed form ($O(1)$ constant time).
* **Sun inscattering:** forward Mie scattering (Henyey-Greenstein phase function) causes the fog to glow warmly when viewed in the direction of the sun disk.
* **Procedural turbulence:** when `noise_amount > 0` and `noise_scale > 0`, 3D fBm noise modulates the optical density to create drifting mist banks and billowing smoke plumes.

### 4.3 `material <name>` — repeatable named block

Declares a material in the scene material table. Maps onto `Material`
(`src/material.h`). Each block **must** carry a unique `<name>`.

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `albedo` | vec3 | linear RGB | `[0, ∞)` | `0 0 0` | O |
| `specular` | vec3 | linear RGB | `[0, ∞)` | `0 0 0` | O |
| `shininess` | f64 | Blinn-Phong exponent | `≥ 0` (0 ⇒ no specular) | `0` | O |
| `reflectivity` | f64 | mix weight | `[0, 1]` | `0` | O |
| `transparency` | f64 | mix weight | `[0, 1]` | `0` | O |
| `ior` | f64 | index of refraction | `≥ 1.0` | `1.0` | O |
| `is_water` | bool | flag | `0` or `1` | `0` | O |
| `beer_lambert` | bool | flag | `0` or `1` | `0` | O |
| `absorption` | vec3 | per-channel Beer-Lambert | `≥ 0` | `0 0 0` | O |
| `deep_color` | vec3 | linear RGB | `≥ 0` | `0 0 0` | O |
| `metallic` | f64 | conductor mix | `[0, 1]` (0 = dielectric, 1 = metal) | `0` | O |
| `roughness` | f64 | perceptual microfacet roughness | `[0, 1]` (0 = mirror) | `0` | O |
| `emissive` | vec3 | self-emission (linear RGB) | `[0, ∞)` | `0 0 0` | O |
| `pbr` | bool | opt-in microfacet gate | `0` or `1` | `0` | O |
| `texture` | name | procedural pattern | `none`, `checker` or `stripes` | `none` | O |
| `texture_scale` | f64 | world units per cell | `> 0` | `1` | O |
| `texture_color_a` | vec3 | linear RGB | `[0, ∞)` | `1 1 1` | O |
| `texture_color_b` | vec3 | linear RGB | `[0, ∞)` | `0 0 0` | O |
| `type` | name | shorthand | `water`, `opaque`, `glass`, `gold`, `copper`, `silver`, `aluminum`, `iron`, `chrome`, `brass`, `plastic`, `rubber`, `ceramic`, `diamond` or `emissive` | — | O |

**`type` shorthand.** `type = <name>` is a convenience that, *before* any other
key in the same block is applied, sets a named material preset. Explicit keys
later in the block override the preset, so `type = gold` followed by
`roughness = 0.30` yields a brushed gold. The valid keywords are:

*Core presets (unchanged):*
- `type = water` — the water preset: `albedo 0.05 0.15 0.20`,
  `specular 0.90 0.90 0.90`, `shininess 256`, `reflectivity 1`,
  `transparency 0.85`, `ior 1.33`, `is_water 1`, `absorption 0.45 0.12 0.06`,
  `deep_color 0.02 0.10 0.16` (exactly the current `MAT_WATER` values).
- `type = opaque` — the all-zero/`ior 1` opaque default.
- `type = glass` — a general clear refractive preset: `albedo 0.02 0.02 0.02`,
  `specular 1 1 1`, `shininess 256`, `reflectivity 0`, `transparency 1`,
  `ior 1.5`, `is_water 0`, `beer_lambert 0`, `absorption 0 0 0`,
  `deep_color 0.5 0.5 0.5`.

*Named real-world presets (values from `docs/research_material_reference.md`).*
All carry `pbr = 1`, `reflectivity = 0`, `is_water = 0`, `beer_lambert = 0`,
zero `absorption`, and the default (untextured) texture fields:

- `type = gold` — polished gold conductor: `albedo = specular = 1.000 0.766 0.336`
  (linear F0), `shininess 256`, `metallic 1`, `roughness 0.05`, `ior 1`.
- `type = copper` — polished copper: `albedo = specular = 0.955 0.637 0.538`,
  `metallic 1`, `roughness 0.05`.
- `type = silver` — polished silver: `albedo = specular = 0.972 0.960 0.915`,
  `metallic 1`, `roughness 0.03`.
- `type = aluminum` — polished aluminium: `albedo = specular = 0.913 0.921 0.925`,
  `metallic 1`, `roughness 0.05`.
- `type = iron` — polished iron/steel: `albedo = specular = 0.560 0.570 0.580`,
  `metallic 1`, `roughness 0.10`.
- `type = chrome` — polished chromium: `albedo = specular = 0.550 0.556 0.554`,
  `metallic 1`, `roughness 0.03`.
- `type = brass` — polished brass: `albedo = specular = 0.910 0.778 0.423`,
  `metallic 1`, `roughness 0.08`.
- `type = plastic` — glossy plastic dielectric: `albedo 0.30 0.05 0.06`,
  `specular 0.05 0.05 0.05`, `shininess 64`, `metallic 0`, `roughness 0.10`,
  `ior 1.46`.
- `type = rubber` — matte rubber dielectric: `albedo 0.05 0.05 0.05`,
  `specular 0.04 0.04 0.04`, `shininess 8`, `metallic 0`, `roughness 0.90`,
  `ior 1.50`.
- `type = ceramic` — glazed ceramic dielectric: `albedo 0.85 0.85 0.82`,
  `specular 0.05 0.05 0.05`, `shininess 128`, `metallic 0`, `roughness 0.20`,
  `ior 1.60`.
- `type = diamond` — clear refractive gem: glass-like transmissive conventions
  with `albedo 0.02 0.02 0.02`, `specular 1 1 1`, `shininess 256`,
  `transparency 1`, `ior 2.417`, `is_water 0`, `beer_lambert 0`,
  `absorption 0 0 0`, `deep_color 0.5 0.5 0.5`, `metallic 0`, `roughness 0.0`.
- `type = emissive` — warm-white lamp emitter: `albedo 0 0 0`,
  `specular 0 0 0`, `shininess 0`, `metallic 0`, `roughness 0.5`,
  `emissive 1.0 0.85 0.65`, `pbr 1`. A pure emitter adds its `emissive` colour
  once per shaded hit; scale `emissive` above `1` (e.g. `4.0 3.4 2.6`) for a
  stronger lamp.

Any other `type` value is a hard error (`E_BAD_MATERIAL_TYPE`); the diagnostic
lists all valid keywords. This gives a terse way to write
`material water { type = water }`, `material glass { type = glass }` or
`material mirror { type = silver }`.

**General refraction (`type = glass` / `beer_lambert`).** The refraction path is
material-generic: Fresnel/Schlick (from `ior` and the surface front-face), total
internal reflection, and `vec3_refract` all work for any transmissive material.
The `is_water` flag additionally perturbs the surface normal with animated waves
(`water_normal`) — a behaviour glass must **not** have. To get Beer-Lambert
medium absorption *without* that wave perturbation, set the independent
`beer_lambert = 1` flag. Attenuation uses `absorption` (per-channel coefficient)
and `deep_color` along the transmitted ray: `T_c = exp(-absorption_c · depth)`
and `out_c = inner_c·T_c + deep_color_c·(1 - T_c)`, so `absorption = 0 0 0`
makes the flag a strict no-op (transmitted colour returned verbatim) and
increasing `absorption` tint the medium toward `deep_color`. The canonical
writer emits `beer_lambert` **only when non-zero**; the built-in materials leave
it at `0`, so `scenes/default.scene` stays byte-identical.

**Opt-in physically-based (PBR) layer (`metallic` / `roughness` / `emissive` /
`pbr`).** These four keys describe an optional physically-based microfacet
material layer (metallic/roughness workflow, see `docs/research_pbr_shading.md`).
`metallic` selects the conductor mix (`0` = dielectric, `1` = metal);
`roughness` is the perceptual microfacet roughness (`0` = mirror); `emissive`
is a linear self-emission colour added once per shaded hit (it may exceed `1`);
and `pbr` is the **opt-in gate** — `pbr = 1` enables the microfacet shading path,
while `pbr = 0` (the default) keeps the legacy Blinn-Phong behaviour. All four
default to zero (`metallic 0`, `roughness 0`, `emissive 0 0 0`, `pbr 0`), which
is exactly the zero-initialised `Material` state, so any material that does not
set them behaves — and renders — identically to before. Like `beer_lambert`, the
canonical writer emits each of these keys **only when non-default** (between
`deep_color` and the `texture*` keys), so the built-in materials emit none of
them and `scenes/default.scene` stays byte-identical.

When `pbr = 1`, the direct-sun term uses the metallic/roughness Cook-Torrance
model: `F0 = mix(vec3(0.04), albedo, metallic)` (per channel), a GGX
(Trowbridge-Reitz) normal distribution with `alpha = max(roughness², 1e-4)`, a
Smith-Schlick-GGX visibility term with `k = (roughness + 1)² / 8`, a Schlick
Fresnel term evaluated at `N·V`, a `(1 − metallic)`-weighted π-consistent
Lambert diffuse lobe (`albedo / π`), and `L_o = light_color · (N·L) · (f_diff +
f_spec)`. The model is energy-conserving — the reflected radiance never exceeds
the incident light (the diffuse lobe is weighted by `kD = (1 − F)(1 − metallic)`,
with `F` the Schlick Fresnel term) — and `metallic` and `roughness` are
**clamped to `[0, 1]` at shading time** (the parser stores them verbatim — a value
outside the range is not rejected, it simply behaves as its clamped endpoint;
`emissive` is deliberately unclamped so it may exceed `1`). `emissive` is applied
**only when `pbr = 1`** and the colour is non-zero; it is added once per shaded
hit (not per light, and independent of `reflectivity`/`transparency`).

**Glossy (roughness-blurred) reflections.** The recursive mirror reflection is
driven by `reflectivity` and, for a PBR material, is *no longer* a sharp Whitted
ray: when **all** of `pbr != 0`, `reflectivity > 0` and
`roughness > GLOSSY_ROUGHNESS_EPSILON` (`1e-6`) hold, the renderer casts 16
importance-sampled GGX rays (using the same `alpha = max(roughness², 1e-4)`
convention as the direct-sun term) and averages them, so `roughness` blurs the
recursive environment reflection. A `roughness ≈ 0` metal (`<= 1e-6`) falls back
to the **exact** legacy single mirror ray, and the non-PBR Blinn-Phong path is
otherwise unchanged. This behaviour is driven entirely by the existing
`pbr`/`reflectivity`/`roughness` keys — **no new scene key is required** — and is
opt-in, so a material with `roughness = 0` (or `pbr = 0`) renders byte-identically
to before.

**Emissive materials as area lights.** An emissive PBR primitive also acts as a
sampled **area light**: at scene-build time up to `SCENE_MAX_EMISSIVE_LIGHTS` (8)
emissive PBR **spheres** (in file order) are collected, and the renderer samples
each lamp's visible cap as a solid-angle cone (16 samples, using the lamp's
`emissive` colour) to illuminate and colour-tint other geometry with distance
falloff. A scene whose materials are not emissive (or not PBR) has a zero light
count, so the feature is disabled and the render is byte-identical. This, too, is
driven entirely by the existing `pbr`/`emissive` keys — **no new scene key is
required**. (Only emissive PBR *spheres* are collected; other primitive types are
not area lights.)

**Referencing a material.** Primitives and plants refer to a material by its
declared `<name>` via `material = <name>` (a name value, §3.4). Names are
case-sensitive. Resolution rules are in §5.

**Procedural textures.** `texture` selects a procedural, position-modulated
albedo (implemented in `src/texture.c`, applied per hit in `src/render.c`).
There is no UV parameterisation, so the patterns are functions of the
**world-space hit position**:

- `texture = none` — the albedo is used verbatim (the default; no-op).
- `texture = checker` — a 3D checkerboard: cells are `floor(p / texture_scale)`
  and the parity of `floor(p.x/s) + floor(p.y/s) + floor(p.z/s)` selects
  `texture_color_a` or `texture_color_b`.
- `texture = stripes` — sinusoidal bands along world Y: `0.5·(1+sin(2π·p.y/s))`
  thresholded at `0.5` selects `texture_color_a` or `texture_color_b`.

The selected pattern colour is **multiplied by the base `albedo`**, so the base
colour still tints the texture. `texture_scale` is the world-space cell size
(`1` by default; a non-positive value falls back to `1`). Any other `texture`
value is a hard error (`E_BAD_TEXTURE_KIND`). The default values
(`texture=none`, `texture_scale=1`, `texture_color_a=1 1 1`,
`texture_color_b=0 0 0`) are a no-op and are **not** emitted by the canonical
writer (§9.2), so untextured materials round-trip with no `texture*` lines.

### 4.4 `sphere` — repeatable primitive block

Constructed via `prim_sphere(center, radius, material_index)`.

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `center` | vec3 | world | finite | — | **R** |
| `radius` | f64 | world | `> 0` | — | **R** |
| `material` | name | — | must resolve | — | **R** |

### 4.5 `plane` — repeatable primitive block

Constructed via `prim_plane(point, normal, material_index)` (infinite plane).

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `point` | vec3 | world | finite | — | **R** |
| `normal` | vec3 | direction | non-zero (normalised on load) | — | **R** |
| `material` | name | — | must resolve | — | **R** |

### 4.6 `box` — repeatable primitive block

Constructed via `prim_box(center, half, material_index)` (axis-aligned box).

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `center` | vec3 | world | finite | — | **R** |
| `half` | vec3 | world half-extents | each `> 0` | — | **R** |
| `material` | name | — | must resolve | — | **R** |

### 4.7 `triangle` — repeatable primitive block

Constructed via `prim_triangle(a, b, c, material_index)`.

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `a` | vec3 | world | finite | — | **R** |
| `b` | vec3 | world | finite | — | **R** |
| `c` | vec3 | world | finite | — | **R** |
| `material` | name | — | must resolve | — | **R** |

Degenerate triangles (`a`,`b`,`c` collinear or coincident) are accepted at parse
time but reported as a warning (`W_DEGENERATE_TRIANGLE`, §6); they simply never
register a hit.

### 4.8 `cylinder` — repeatable primitive block

Constructed via `prim_cylinder(base, top, r_bottom, r_top, material_index)`
(finite, capped, tapered frustum).

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `base` | vec3 | world | finite | — | **R** |
| `top` | vec3 | world | finite | — | **R** |
| `r_bottom` | f64 | world | `> 0` | — | **R** |
| `r_top` | f64 | world | `> 0` | — | **R** |
| `material` | name | — | must resolve | — | **R** |

`base == top` (zero length) is a warning (`W_DEGENERATE_CYLINDER`, §6).

### 4.9 `tree` — repeatable procedural block

Grows a full tree using the same recursive routine as `grow_tree()` in
`src/scene.c`. This is a **high-level directive**: it expands to many
`PRIM_CYLINDER` branch segments plus leaf-cluster `PRIM_SPHERE`s.

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `position` | vec3 | world (x, y=ignored, z) | finite; `y` is forced to `0` (ground) | — | **R** |
| `height` | f64 | world (trunk length) | `> 0` | — | **R** |
| `radius` | f64 | world (trunk radius) | `> 0` | `0.20` | O |
| `seed` | int | — | `[0, 2^32-1]` | derived from CLI seed (§5.5) | O |
| `material_bark` | name | — | must resolve | `bark` | O |
| `material_leaf` | name | — | must resolve | auto leaf variant (§5.5) | O |
| `leaf_variant` | int | — | `[0, 3]` | auto from seed | O |
| `max_depth` | int | levels | `>= 1` | `4` (`SCENE_TREE_MAX_DEPTH`) | O |
| `min_branch_radius` | f64 | world | `> 0` | `0.02` (`SCENE_MIN_BRANCH_RADIUS`) | O |
| `taper` | f64 | ratio | `> 0` | `0.7` (`SCENE_TAPER`) | O |
| `len_decay` | f64 | ratio | `> 0` | `0.72` (`SCENE_LEN_DECAY`) | O |
| `spread_deg` | f64 | degrees | finite | `33` (`SCENE_SPREAD`) | O |
| `perturb_deg` | f64 | degrees | finite | `7` (`SCENE_PERTURB`) | O |
| `up_bias` | f64 | — | finite | `0.12` (`SCENE_UP_BIAS`) | O |
| `third_child_chance` | f64 | probability | `[0, 1]` | `0.25` (`SCENE_THIRD_CHILD_CHANCE`) | O |
| `leaf_min` | int | spheres/tip | `>= 1` | `15` (`SCENE_LEAF_MIN`) | O |
| `leaf_span` | int | spheres/tip | `>= 0` | `5` (`SCENE_LEAF_SPAN`) | O |
| `foliage` | ident | — | `spheres`, `leaves`, `needles` | `spheres` (legacy) | O |
| `type` | ident | — | `deciduous`, `conifer` (`spruce`), `bush` | `deciduous` | O |

**`radius` also selects recursion depth**, exactly as documented in `scene.c`:
the recursion stops when a branch tip radius drops below `min_branch_radius`
(0.02 by default). `radius ≈ 0.20` → depth 4 (full tree); `≈ 0.07` → depth 3;
`≈ 0.05` → depth 2. The writer emits the `radius` that reproduces the source
tree.

**Generator parameters (file-controllable).** The optional keys above mirror
the procedural constants of `grow_tree()` / `add_leaf_cluster()` in
`src/scene.c`. Each is a per-plant override:

* `type` — `deciduous` (branching crown), `conifer` / `spruce` (tiered whorls of
  horizontal drooping branches and central tapered trunk), or `bush`.
* `foliage` — `spheres` (legacy sphere clusters), `leaves` (3D folded diamond
  triangles with crease), or `needles` (dense conifer needle fronds).
* `max_depth` — maximum branch recursion depth (levels); a terminal tip at the
  limit gets foliage.
* `min_branch_radius` — a branch whose tip radius falls below this also becomes
  terminal (secondary depth control, combined with `radius`/`taper`).
* `taper` — child-segment top radius = `taper * bottom radius`.
* `len_decay` — child length = `len_decay * parent length` (before jitter).
* `spread_deg` / `perturb_deg` — base branch angle and the ± jitter applied to
  the angle and the child azimuth, in degrees.
* `up_bias` — upward pull added to each child direction (anti-droop).
* `third_child_chance` — probability a node forks three ways (else two).
* `leaf_min` / `leaf_span` — a terminal tip gets
  `leaf_min .. leaf_min + leaf_span - 1` leaf elements.

**`spruce` / `conifer` block headers.** Blocks can also be introduced directly
with `spruce { ... }` or `conifer { ... }`, which default `type = conifer` and
`foliage = needles`.

When a key is omitted the legacy `SCENE_*` constant is used, so a scene with
none of these keys generates byte-identical geometry to the pre-parameterised
generator.

### 4.10 `bush` — repeatable procedural block

Identical to `tree` but with the bush defaults of `src/scene.c` (shorter trunk,
thinner radius → shallower recursion). Same keys and semantics as §4.9,
including all generator-parameter keys.

| Key | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `position` | vec3 | world | `y` forced to `0` | — | **R** |
| `height` | f64 | world | `> 0` | — | **R** |
| `radius` | f64 | world | `> 0` | `0.05` | O |
| `seed` | int | — | `[0, 2^32-1]` | derived (§5.5) | O |
| `material_bark` | name | — | must resolve | `bark` | O |
| `material_leaf` | name | — | must resolve | auto leaf variant | O |
| `leaf_variant` | int | — | `[0, 3]` | auto from seed | O |
| `max_depth` | int | levels | `>= 1` | `4` | O |
| `min_branch_radius` | f64 | world | `> 0` | `0.02` | O |
| `taper` | f64 | ratio | `> 0` | `0.7` | O |
| `len_decay` | f64 | ratio | `> 0` | `0.72` | O |
| `spread_deg` | f64 | degrees | finite | `33` | O |
| `perturb_deg` | f64 | degrees | finite | `7` | O |
| `up_bias` | f64 | — | finite | `0.12` | O |
| `third_child_chance` | f64 | probability | `[0, 1]` | `0.25` | O |
| `leaf_min` | int | spheres/tip | `>= 1` | `15` | O |
| `leaf_span` | int | spheres/tip | `>= 0` | `5` | O |

### 4.11 Top-level (global) settings

These are **not** blocks; they are top-level statements (no braces). In
canonical output they are emitted first, before any block.

| Statement | Type | Units | Range | Default | Req |
|---|---|---|---|---|---|
| `water_level = <f64>` | f64 | world (y of water surface) | finite | `0.02` (`SCENE_WATER_LEVEL`) | O |
| `water_material = <name>` | name | — | must resolve, or `none` | the water-preset material if any, else `-1` | O |
| `water_enabled = <bool>` | bool | — | `0`/`1` | `1` if a water material exists | O |

> **New field (not in the C `Scene` struct):** `water_enabled` has **no
> counterpart in `src/scene.h`** — it is introduced by this format as a
> convenience gate (when `0`, the parser treats the scene as having no water
> regardless of `water_material`). An implementer adding it must extend the
> `Scene` struct (or the parser's local state) explicitly; do not assume it
> already exists. The other two globals map to real `Scene` fields:
> `water_level` → `Scene.water_level`, `water_material` → `Scene.water_material`.

- `water_material = none` (or a file with no water material at all) sets
  `scene.water_material = -1`.
- **Renderer nuance (from `docs/scene_inventory.md` §1):** `render.c` does *not*
  read `scene.water_material`; it detects water per-material via the `is_water`
  flag. Therefore the practical effect of the `is_water` key is what enables
  wave-normal perturbation and depth tint, and `water_material` is essentially
  bookkeeping metadata. The parser still records it (for round-trip fidelity and
  future use) but authors should set `is_water = 1` on the water material to get
  the water look.
- `water_level` is the y of the pond top face and is used only at build time to
  place the pond box. The pond box (if the file declares one) must be authored
  with its top face at this value; see §8.

---

## 5. Semantics

### 5.1 Material name resolution

1. All `material <name> { … }` blocks are parsed **in file order** and appended
   to the scene material table. A material's table index is its **0-based
   position among material blocks in the file** (`material_index`).
2. A primitive/plant/global `material = <name>` reference is resolved to that
   index by **exact, case-sensitive name match**.
3. The material table is exactly the set of materials declared in the file. No
   implicit materials are added. (The built-in fallback of §8 is only used
   when *no file is supplied at all*, not to fill gaps in a supplied file.)

### 5.2 Duplicate material names

Declaring two `material` blocks with the same `<name>` is a **hard error**
(`E_DUPLICATE_MATERIAL`, §6). The parser reports the line of the second
declaration. (Rationale: silent shadowing would make name resolution ambiguous
and break the round-trip guarantee.)

### 5.3 Unknown material reference

If a primitive/plant/global references a name that was never declared, it is a
**hard error** (`E_UNKNOWN_MATERIAL`, §6) naming the offending token and line.
There is no silent fallback to a default material, because that would render a
wrong image without the user knowing. (If the renderer is later run *without*
any scene file, the built-in `scene_build()` scene of §8 is used
unchanged — that is the only "default" behaviour.)

### 5.4 Primitive order and `prim_index`

**Primitive ORDER in the file determines `prim_index`.** This is a hard
requirement:

- Primitives are appended to `Geometry` in the exact order their blocks appear
  in the file, top to bottom.
- A `tree`/`bush` directive expands, at its position in the file, to all of its
  branch cylinders and leaf spheres, in the deterministic order produced by
  `grow_tree()` (parent segment first, then children depth-first).
- Therefore the `prim_index` recorded in a `Hit` and used by the BVH tie-break
  is a stable function of file order. Two files that differ only in whitespace
  or comments produce identical `prim_index` assignments.

### 5.5 `--seed` interaction

- The CLI `--seed N` (default 1337, `unsigned`) is the **root seed**.
- For a `tree`/`bush` directive **without** an explicit `seed` key, the
  directive seed is derived exactly as in `scene_build()`:
  `directive_seed = scene_hash(root_seed, group, index)` where `group = 0x7`
  for trees, `group = 0xB` for bushes, and `index` is the 0-based position of
  that directive among same-group directives in the file.
- For a directive **with** an explicit `seed` key, that value is used directly
  and the CLI `--seed` does **not** affect that directive's growth.
- When `sky.seed` is omitted, it is set to the CLI `--seed` (§4.2).
- Result: a file with no `seed` keys is fully reproducible from `--seed`; a
  file with explicit seeds is reproducible independent of `--seed`.

### 5.6 Determinism

`scene_desc_parse` is a pure function of (file contents, CLI seed). It uses no
`rand()`, no time, no globals. Re-parsing the same bytes yields identical
geometry, identical material table and identical `prim_index` order.

---

## 6. Error Semantics — Exhaustive

**Global rules (apply to every error below):**

- Every diagnostic message MUST include the **1-based physical line number** and
  the **offending token** (or `<eof>`), in the form:
  `scene.scene:LINE: error: MESSAGE (near 'TOKEN')`.
- Malformed input must **NEVER crash, hang, or leak**. The parser must be a
  bounded loop: it reads at most one line at a time, never recurses, never
  allocates unboundedly, and frees everything it allocated before returning on
  any error path.
- Exit behaviour matches `main.c` conventions: a **hard error** causes
  `scene_desc_parse` to return non-zero, `main` prints the message to `stderr`
  and exits **1** (scene-build failure), exactly like `scene_build()` failing
  today. CLI-usage errors remain exit **2**.

| Condition | Severity | Parser action |
|---|---|---|
| **Unknown block keyword** (top-level token not in the block set) | **Hard error** `E_UNKNOWN_BLOCK` | Stop, report line + token. |
| **Unknown key** inside a known block | **Warning** `W_UNKNOWN_KEY` | Print warning, ignore that line, continue. (Forward-compat: reserved keys like `aspect` are tolerated.) |
| **Missing value** (`key =` with nothing after, or `key` with no `=`) | **Hard error** `E_MISSING_VALUE` | Stop, report line + key. |
| **Malformed number** (`1.2.3`, `--4`, `abc`, `0x10`, `nan`, `inf`) | **Hard error** `E_BAD_NUMBER` | Stop, report line + token. |
| **Wrong component count** for a vec3 (2 or 4 numbers) | **Hard error** `E_BAD_VEC3` | Stop, report line + token. |
| **Fractional value for an integer key** (`cloud_octaves = 5.0`) | **Hard error** `E_BAD_INTEGER` | Stop, report line + token. |
| **Unterminated block** (EOF reached with a block still open) | **Hard error** `E_UNTERMINATED_BLOCK` | Stop, report the line of the opening `{` and its keyword. |
| **Unmatched `}`** (close with no open block) | **Hard error** `E_UNEXPECTED_BRACE` | Stop, report line. |
| **Nested `{`** inside a block | **Hard error** `E_NESTED_BLOCK` | Stop, report line. |
| **Duplicate material name** | **Hard error** `E_DUPLICATE_MATERIAL` | Stop, report line + name. |
| **Duplicate key** within one block | **Hard error** `E_DUPLICATE_KEY` | Stop, report line + key. |
| **Unknown material reference** | **Hard error** `E_UNKNOWN_MATERIAL` | Stop, report line + name. |
| **Value out of range** (e.g. `reflectivity = 2`, `radius = -1`, `ior = 0.5`, `vfov = 200`, `cloud_octaves = 99`) | **Hard error** `E_OUT_OF_RANGE` | Stop, report line, key, value and the allowed range. |
| **Degenerate but well-formed geometry** (collinear triangle, `base==top` cylinder) | **Warning** `W_DEGENERATE_*` | Print warning, keep the primitive. |
| **Missing required key** in a primitive/plant block | **Hard error** `E_MISSING_REQUIRED` | Stop, report the line of the block header + the missing key name. |
| **Empty file** (0 bytes, or only blank/comment lines) | **Not an error** | Return success with the **built-in default scene** (§8 fallback) and print a note. Equivalent to "no file". |
| **I/O failure** (file missing, permission denied, read error) | **Hard error** `E_IO` | Report `strerror(errno)`; exit **1**. A missing default scene file is **not** fatal — `main` falls back to the built-in scene and prints a note. |
| **Line longer than 4096 bytes** | **Hard error** `E_LINE_TOO_LONG` | Stop, report line number. |
| **Byte ≥ 0x80 outside a quoted string** | **Hard error** `E_UNEXPECTED_CHAR` | Stop, report line + byte. |
| **Unterminated / bad escape in quoted string** | **Hard error** `E_BAD_STRING` | Stop, report line. |
| **`,` or `;` anywhere** | **Hard error** `E_UNEXPECTED_CHAR` | Stop, report line + token. |
| **Block/primitive count exceeds a safety cap** (e.g. > 1e6 primitives after expansion) | **Hard error** `E_TOO_MANY_PRIMITIVES` | Stop before allocating past the cap (mirrors `SCENE_MAX_SEGMENTS`). |

**Warning vs. error policy:** anything that would silently change the *rendered
image* in a way the author did not intend is a **hard error**. Only genuinely
forward-compatible unknowns (unknown keys) and cosmetically-degenerate geometry
are warnings. Unknown **blocks** are hard errors because a misspelled block
keyword would otherwise drop an entire intended object.

---

## 7. Worked Example — Complete Valid Scene

Every line below is valid under this spec and exercises every block type.
Paste this into `scenes/default.scene`.

```text
# scenes/default.scene — fully commented reference scene.
# Comments start with '#'; blank lines are ignored.
# This file describes the built-in outdoor scene (see §8 mapping table).

# ---- Global settings (top level, no braces) --------------------------
water_level    = 0.02
water_material = water
water_enabled  = 1

# ---- Camera ----------------------------------------------------------
camera {
    eye    = -18.0 6.0 22.0
    target = 0.0 5.0 -20.0
    up     = 0.0 1.0 0.0
    vfov   = 40.0            # vertical FOV in degrees; aspect comes from --width/--height
}

# ---- Sky / atmosphere (SkyParams) ------------------------------------
sky {
    sun_dir           = 0.45 0.75 -0.50   # normalised on load
    sun_color         = 1.00 0.95 0.85
    horizon_color     = 0.75 0.85 1.00
    zenith_color      = 0.35 0.55 0.95
    gradient_gamma    = 0.6
    sun_glow_exponent = 350.0
    sun_glow_strength = 0.8
    cloud_height      = 120.0
    cloud_scale       = 0.0025
    cloud_coverage    = 0.5
    cloud_softness    = 0.12
    cloud_sharpness   = 1.5
    cloud_octaves     = 5
    # seed omitted on purpose -> takes the CLI --seed value
}

# ---- Materials -------------------------------------------------------
material ground {
    albedo       = 0.30 0.42 0.16
    specular     = 0.05 0.05 0.05
    shininess    = 8.0
    reflectivity = 0.0
    ior          = 1.0
}

material bark {
    albedo       = 0.26 0.18 0.11
    specular     = 0.04 0.04 0.04
    shininess    = 6.0
    reflectivity = 0.0
    ior          = 1.0
}

material leaf0 {
    albedo    = 0.16 0.38 0.12
    specular  = 0.03 0.04 0.03
    shininess = 10.0
    ior       = 1.0
}
material leaf1 {
    albedo    = 0.22 0.45 0.15
    specular  = 0.03 0.04 0.03
    shininess = 10.0
    ior       = 1.0
}
material leaf2 {
    albedo    = 0.30 0.50 0.18
    specular  = 0.03 0.04 0.03
    shininess = 10.0
    ior       = 1.0
}
material leaf3 {
    albedo    = 0.12 0.30 0.10
    specular  = 0.03 0.04 0.03
    shininess = 10.0
    ior       = 1.0
}

# Water via the shorthand preset, then override one field:
material water {
    type     = water          # sets albedo/specular/shininess/reflectivity/...
    deep_color = 0.02 0.10 0.16   # explicit override, same as preset here
}

# ---- Primitive geometry ----------------------------------------------
# Infinite grass plane at y = 0.
plane {
    point    = 0.0 0.0 0.0
    normal   = 0.0 1.0 0.0
    material = ground
}

# Pond: a very flat box whose TOP face sits at water_level (0.02).
# center.y = water_level - half.y = 0.02 - 0.03 = -0.01
box {
    center   = 0.0 -0.01 -25.0
    half     = 45.0 0.03 45.0
    material = water
}

# A plain sphere, a triangle and a tapered cylinder (all prim_* constructors):
sphere {
    center   = 8.0 1.5 -8.0
    radius   = 1.5
    material = ground
}

triangle {
    a        = 0.0 0.0 0.0
    b        = 2.0 0.0 0.0
    c        = 0.0 2.0 0.0
    material = bark
}

cylinder {
    base     = 3.0 0.0 3.0
    top      = 3.0 4.0 3.0
    r_bottom = 0.3
    r_top    = 0.2
    material = bark
}

# ---- Procedural plants -----------------------------------------------
# Large full-depth tree on the far shore (radius 0.20 -> depth 4).
tree {
    position = 50.0 0.0 -80.0
    height   = 3.4
    radius   = 0.20
    # seed omitted -> derived from --seed; leaf variant auto-selected
}

# Medium depth-3 tree.
tree {
    position = 30.0 0.0 -105.0
    height   = 2.8
    radius   = 0.07
}

# Small depth-2 tree with explicit seed and material overrides.
tree {
    position      = 15.0 0.0 -115.0
    height        = 2.2
    radius        = 0.05
    seed          = 424242
    material_bark = bark
    material_leaf = leaf2
}

# Bushes.
bush {
    position = -48.0 0.0 -55.0
    height   = 0.8
    radius   = 0.05
}
bush {
    position = 55.0 0.0 -100.0
    height   = 0.7
    radius   = 0.05
}
```

---

## 8. Mapping Table — Current Hardcoded Scene → Format

Derived directly from `src/scene.c` (no `docs/scene_inventory.md` existed at
authoring time). Every hardcoded item and its file expression:

| # | Source item (`src/scene.c`) | Value | File expression |
|---|---|---|---|
| 1 | `scene_default_view` eye | `(-18, 6, 22)` | `camera { eye = -18.0 6.0 22.0 }` |
| 2 | `scene_default_view` target | `(0, 5, -20)` | `camera { target = 0.0 5.0 -20.0 }` |
| 3 | `scene_default_view` up | `(0, 1, 0)` | `camera { up = 0.0 1.0 0.0 }` |
| 4 | `scene_default_view` vfov | `40.0` | `camera { vfov = 40.0 }` |
| 5 | camera aspect (16/9 hardcoded, overridden by `main.c`) | `w/h` | **not in file**; always from `--width/--height` |
| 6 | `sky_default_params` `sun_dir` | `normalize(0.45,0.75,-0.50)` | `sky { sun_dir = 0.45 0.75 -0.50 }` |
| 7 | sky `sun_color` | `(1.00,0.95,0.85)` | `sky { sun_color = 1.00 0.95 0.85 }` |
| 8 | sky `horizon_color` | `(0.75,0.85,1.00)` | `sky { horizon_color = 0.75 0.85 1.00 }` |
| 9 | sky `zenith_color` | `(0.35,0.55,0.95)` | `sky { zenith_color = 0.35 0.55 0.95 }` |
| 10 | sky `gradient_gamma` | `0.6` | `sky { gradient_gamma = 0.6 }` |
| 11 | sky `sun_glow_exponent` | `350.0` | `sky { sun_glow_exponent = 350.0 }` |
| 12 | sky `sun_glow_strength` | `0.8` | `sky { sun_glow_strength = 0.8 }` |
| 13 | sky `cloud_height` | `120.0` | `sky { cloud_height = 120.0 }` |
| 14 | sky `cloud_scale` | `0.0025` | `sky { cloud_scale = 0.0025 }` |
| 15 | sky `cloud_coverage` | `0.5` | `sky { cloud_coverage = 0.5 }` |
| 16 | sky `cloud_softness` | `0.12` | `sky { cloud_softness = 0.12 }` |
| 17 | sky `cloud_sharpness` | `1.5` | `sky { cloud_sharpness = 1.5 }` |
| 18 | sky `cloud_octaves` | `5` | `sky { cloud_octaves = 5 }` |
| 19 | sky `seed` (set to CLI seed) | `--seed` | `sky { seed = <N> }` (omit to inherit `--seed`) |
| 20 | `MAT_GROUND` material | albedo `.30 .42 .16`, spec `.05 .05 .05`, shin `8`, refl `0`, ior `1` | `material ground { … }` |
| 21 | `MAT_BARK` material | albedo `.26 .18 .11`, spec `.04 .04 .04`, shin `6`, refl `0`, ior `1` | `material bark { … }` |
| 22 | `MAT_LEAF0` | albedo `.16 .38 .12`, spec `.03 .04 .03`, shin `10`, ior `1` | `material leaf0 { … }` |
| 23 | `MAT_LEAF1` | albedo `.22 .45 .15` | `material leaf1 { … }` |
| 24 | `MAT_LEAF2` | albedo `.30 .50 .18` | `material leaf2 { … }` |
| 25 | `MAT_LEAF3` | albedo `.12 .30 .10` | `material leaf3 { … }` |
| 26 | `MAT_WATER` material | albedo `.05 .15 .20`, spec `.90 .90 .90`, shin `256`, refl `1`, transp `.85`, ior `1.33`, is_water `1`, abs `.45 .12 .06`, deep `.02 .10 .16` | `material water { type = water }` |
| 26b | `type = glass` preset (general refraction) | albedo `.02 .02 .02`, spec `1 1 1`, shin `256`, refl `0`, transp `1`, ior `1.5`, is_water `0`, beer_lambert `0`, abs `0 0 0`, deep `.5 .5 .5` | `material glass { type = glass }` |
| 27 | Ground plane | `prim_plane((0,0,0), (0,1,0), MAT_GROUND)` | `plane { point = 0 0 0  normal = 0 1 0  material = ground }` |
| 28 | Water pond box | `prim_box((0, -0.01, -25), (45, 0.03, 45), MAT_WATER)`; top face at `water_level=0.02` | `box { center = 0 -0.01 -25  half = 45 0.03 45  material = water }` |
| 29 | `SCENE_WATER_LEVEL` | `0.02` | `water_level = 0.02` |
| 30 | `scene.water_material` | `MAT_WATER` | `water_material = water` |
| 31 | Tree 0 (`scene_trees[0]`) | `(50,-80)`, len `3.4`, r `0.20` | `tree { position = 50 0 -80  height = 3.4  radius = 0.20 }` |
| 32 | Tree 1 | `(30,-105)`, len `2.8`, r `0.07` | `tree { position = 30 0 -105  height = 2.8  radius = 0.07 }` |
| 33 | Tree 2 | `(-35,-100)`, len `2.7`, r `0.07` | `tree { position = -35 0 -100  height = 2.7  radius = 0.07 }` |
| 34 | Tree 3 | `(70,-60)`, len `2.6`, r `0.07` | `tree { position = 70 0 -60  height = 2.6  radius = 0.07 }` |
| 35 | Tree 4 | `(15,-115)`, len `2.2`, r `0.05` | `tree { position = 15 0 -115  height = 2.2  radius = 0.05 }` |
| 36 | Tree 5 | `(-60,-85)`, len `2.2`, r `0.05` | `tree { position = -60 0 -85  height = 2.2  radius = 0.05 }` |
| 37 | Bush 0 (`scene_bushes[0]`) | `(-48,-55)`, len `0.8`, r `0.05` | `bush { position = -48 0 -55  height = 0.8  radius = 0.05 }` |
| 38 | Bush 1 | `(55,-100)`, len `0.7`, r `0.05` | `bush { position = 55 0 -100  height = 0.7  radius = 0.05 }` |
| 39 | Tree/bush seed derivation | `scene_hash(seed, 0x7/0xB, i)` | derived automatically when `seed` omitted (§5.5) |
| 40 | Tree/bush leaf variant | `floor(rand*4)` from directive seed | derived automatically when `leaf_variant` omitted |
| 41 | Tree/bush generator parameters | `SCENE_*` constants in `src/scene.c` (`max_depth=4`, `min_branch_radius=0.02`, `taper=0.7`, `len_decay=0.72`, `spread_deg=33`, `perturb_deg=7`, `up_bias=0.12`, `third_child_chance=0.25`, `leaf_min=15`, `leaf_span=5`) | per-plant override when the key is present (§4.9/§4.10); omitted keys use the constant |

**Built-in fallback.** If the renderer is invoked with **no scene file**, or
with an empty file, it reproduces the current `scene_build()` output exactly
(items 1–40 above). This is the only "default scene" behaviour; a *supplied*
file is authoritative and is never silently back-filled.

---

## 9. Round-Trip Guarantee & Canonical Formatting

**Guarantee.** The writer `scene_desc_write` MUST be able to emit a canonical
file such that `scene_desc_parse(scene_desc_write(scene))` yields a scene that
is **identical** to the input scene: same material table (same order, same
values), same primitive array (same order → same `prim_index`), same
`water_level`/`water_material`, same camera and sky. "Identical" is bit-for-bit
on all `double` fields and on primitive ordering.

To make the round-trip **stable** (write∘parse∘write is a fixed point), the
canonical formatting is frozen as follows:

1. **Statement order (top level):**
   1. header comment (single `#` line, no timestamp),
   2. globals: `water_level`, `water_material`, `water_enabled`,
   3. `camera` block,
   4. `sky` block,
   5. all `material` blocks in table order,
   6. all primitives in `prim_index` order,
   7. all `tree`/`bush` directives in file order.
2. **Key order within a block:** the order the keys are listed in §4 (not the
   order they were read). `material` blocks emit `type` first if the material
   equals a preset (`water`, `opaque`, `glass`, `gold`, `copper`, `silver`,
   `aluminum`, `iron`, `chrome`, `brass`, `plastic`, `rubber`, `ceramic`,
   `diamond` or `emissive`), otherwise all explicit keys
   in §4.3 order, followed by the `texture*` keys. In the explicit branch the
   `beer_lambert` flag is emitted **only when non-zero** (between `is_water`
   and `absorption`), so a material with the default `beer_lambert = 0` emits
   no such line and a material equal to the `glass` preset emits exactly
   `type = glass`. The four opt-in PBR keys (`metallic`, `roughness`,
   `emissive`, `pbr`) are likewise emitted **only when they differ from their
   zero defaults**, in that order between `deep_color` and the `texture*` keys;
   a material with no PBR layer emits none of them. The four `texture*` keys are emitted **only when they differ
   from their defaults** (`texture=none`, `texture_scale=1`,
   `texture_color_a=1 1 1`, `texture_color_b=0 0 0`), so an untextured material
   emits no `texture*` line at all (this keeps `scenes/default.scene`
   byte-identical). When emitted, `texture` comes first, then only the
   non-default `texture_scale` / `texture_color_a` / `texture_color_b` keys.
   Likewise, the `sky` block emits `sun_radius` **only when it differs from its
   default** (`0`, a point-like sun / hard shadow), so the built-in scene emits
   no `sun_radius` line and `scenes/default.scene` stays byte-identical.
   Similarly, the `camera` block emits `aperture` and `focus_distance` **only
   when they differ from their defaults** (`0` pinhole and `0` →
   derive `|target - eye|`), so the built-in scene emits neither line and
   `scenes/default.scene` stays byte-identical.
   Likewise, each `tree`/`bush` block emits its optional generator parameters
   (`max_depth`, `min_branch_radius`, `taper`, `len_decay`, `spread_deg`,
   `perturb_deg`, `up_bias`, `third_child_chance`, `leaf_min`, `leaf_span`)
   **only when the file carried the corresponding key**, in the §4.9/§4.10
   order after `leaf_variant`; an omitted key resolves to the legacy `SCENE_*`
   default at build time, so the built-in scene emits none of them and
   `scenes/default.scene` stays byte-identical.
3. **Indentation:** block body lines are indented with exactly **4 spaces**.
   One space around `=`; one space between vec3 components. Closing `}` at
   column 0.
4. **Number formatting:** every floating-point value is emitted with
   `"%.6g"` whenever that is enough to recover the identical `double`; for a
   value that `%.6g` would not round-trip bit-exactly (e.g. a normalised
   `sun_dir`), the writer emits the **shortest** `"%.*g"` form (up to 17
   significant digits) that does. This preserves the short, readable canonical
   form for typical values while making the round-trip guarantee in this
   section *bit-exact* on every `double`. Integers (`cloud_octaves`,
   `is_water`, all `seed`/count fields) are emitted as base-10 with no sign for
   non-negatives. `-0` is normalised to `0`. The parser accepts wider forms
   (§2).
5. **Vec3 formatting:** exactly three components, space-separated, on the value
   side of one line.
6. **Names:** emitted unquoted when they match `[A-Za-z_][A-Za-z0-9_]*`,
   otherwise double-quoted with `\"`/`\\` escapes.
7. **Booleans:** emitted as `0`/`1`.
8. **Comments:** the writer emits only its own header comment and never
   preserves input comments (comments are not part of the scene model). Blank
   lines: the writer emits one blank line between top-level blocks for
   readability; the parser ignores them, so this does not affect the round-trip.
9. **Trailing newline:** the writer ends the file with a single `\n`.
10. **Encoding:** the writer emits pure ASCII (no BOM). Quoted names containing
    non-ASCII are emitted verbatim.

**Idempotence check (normative):** `write(parse(write(S))) == write(S)` for
every valid scene `S`, byte-for-byte.

---

## 10. CLI Flags Relevant to Rendering

The scene file describes *geometry/framing/materials*; the CLI describes *output
resolution* and the **rendering** behaviours. **Path tracing (global illumination),
adaptive sampling and multithreading are ON by default**, each with a `--no-*` opt-out.
The scene format itself is unchanged by these flags — no new scene key is required. All
of them are defined in `src/main.c` and accepted in both `--opt value` and
`--opt=value` forms.

| Flag | Argument | Meaning | Default |
|---|---|---|---|
| `--adaptive` | — | Adaptive sampling is **ON by default**; this flag is accepted for compatibility (a no-op). The adaptive sampler (`render_image_ex`) refines a pixel while its relative standard error of the mean luminance (Rec. 709) exceeds `--adaptive-tau`, up to `--adaptive-max` samples, and stops early on flat/low-variance pixels. Applies to the **Whitted renderer only** (ignored by the path tracer). | on |
| `--no-adaptive` | — | Disable adaptive sampling and use the fixed per-pixel sample count. Combine with `--no-pathtrace` to reproduce the exact legacy fixed-spp Whitted output. | off |
| `--adaptive-max` | `N` | Maximum samples per pixel in adaptive mode (must be ≥ 1). `0` selects the default. | `4 × --samples` |
| `--adaptive-tau` | `T` | Relative-error tolerance `tau` in adaptive mode (must be > 0). A smaller `tau` refines more. `0` selects the default. | `0.02` |
| `--pathtrace` | — | The **unbiased path tracer** (unidirectional global illumination) is **ON by default**; this flag is accepted for compatibility (a no-op). Runs an iterative bounce loop up to `--depth` and accumulates `--samples` paths per pixel, so it interacts with both `--depth` (bounce limit) and `--samples` (paths/pixel). | on |
| `--no-pathtrace` | — | Select the legacy **Whitted-style renderer** instead of the path tracer. Combine with `--no-adaptive` to reproduce the exact **byte-identical** fixed-spp output of previous releases. | off |
| `--threads` | — | Multithreading is **ON by default** in the threaded build; this flag is accepted for compatibility (a no-op). | on |
| `--no-threads` / `--single-threaded` | — | Force **single-threaded** rendering at runtime. | off |
| `--no-progress` | — | Silence the stderr render progress meter (same effect as setting the environment variable `RAYTRACER_NO_PROGRESS`). | off |

**Default-ON with `--no-*` opt-outs.** Path tracing, adaptive sampling and multithreading
run by default; the matching `--pathtrace` / `--adaptive` / `--threads` flags are still
accepted but are redundant no-ops. Passing `--no-pathtrace` runs the Whitted-style
renderer unchanged (byte-identical), and `--no-adaptive` makes `render_image_ex` delegate
verbatim to the fixed path; together they reproduce the exact legacy fixed-spp Whitted
output. Adaptive sampling applies to the Whitted renderer only and is ignored by the path
tracer. The progress meter writes only to stderr and never touches pixel data. The
remaining rendering options (`--width`, `--height`, `--samples`, `--depth`, `--seed`,
`--out`, `--scene`, `--write-scene`) are documented in the project `README.md`.

**Emissive PBR materials as area lights.** This rendering behaviour is *not*
controlled by a CLI flag or a new scene key: it is driven entirely by the
existing material parameters documented in §4.3. A primitive whose material has
`pbr = 1` and a non-zero `emissive` colour is both self-lit and, when it is a
**sphere**, promoted at build time to a sampled **area light** (up to
`SCENE_MAX_EMISSIVE_LIGHTS` = 8, in file order) that illuminates and colour-tints
other geometry. A scene with no such material has a zero emissive-light count, so
the feature is disabled and the render is byte-identical. Likewise, the
roughness-blurred ("glossy") reflection behaviour is driven entirely by the
existing `pbr`/`reflectivity`/`roughness` keys (§4.3) — no new key or flag.

---

## Appendix A — Reserved Names & Forward Compatibility

- **Reserved but inert in v1:** `camera { aspect = … }` (ignored → warning).
  A future nested `tree`/`bush` sub-block remains reserved (would require
  nesting); per-plant generator tuning is instead exposed through the flat
  optional keys of §4.9/§4.10 (`max_depth`, `taper`, `spread_deg`, …).
- **Reserved error codes** for future lenient mode: unknown *keys* are warnings
  today; a future `--strict` flag may promote them to errors. The set of block
  keywords is closed and case-sensitive; adding blocks is a **minor** spec
  revision that must keep existing files valid.
- **Reserved globals:** `water_enabled` is defined (a **new field**, see §4.11 —
  not present in the C `Scene` struct); future scene-level flags
  (`world_up`, `background = <name>`) are reserved names and must be reported as
  unknown keys (warning) by a v1 parser.

## Appendix B — Grammar Summary (EBNF, informational)

```ebnf
file        = { blank | comment | global | block } , EOF ;
global      = ( "water_level" | "water_material" | "water_enabled" )
              "=" value , EOL ;
block       = block_header , EOL ,
              { blank | comment | keyval } ,
              "}" , EOL ;
block_header= "camera" | "sky"
            | "material" name
            | "sphere" | "plane" | "box" | "triangle" | "cylinder"
            | "tree" | "bush" , "{" ;
keyval      = key , "=" , value , EOL ;
value       = number | number number number | integer | name | string | "0" | "1" ;
comment     = "#" , { any-char-except-EOL } ;
```

No recursion. Nesting is disallowed. One statement per physical line.

*End of frozen specification.*
