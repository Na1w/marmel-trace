# Real-World Material Reference Values → Renderer Presets

**Scope:** reference parameter values for a set of physically-motivated materials,
mapped onto concrete `type = <name>` presets for the scene-format material block,
consistent with the existing `sd_water_preset()` / `sd_opaque_preset()` /
`sd_glass_preset()` conventions in `src/scene_desc.c` and the `Material` struct in
`src/material.h`.

**Status:** READ-ONLY research deliverable. No source file was modified. This
document is the specification consumed by the preset-implementation task (t-027)
and cross-checked against the PBR-BRDF research (t-023) and schema extension (t-025).

---

## 0. Conventions learned from the existing code

### 0.1 `Material` field layout (`src/material.h`)

Fields, **in declaration order** (the order used below for C initialiser-style
lists):

| # | Field | Type | Meaning |
|---|-------|------|---------|
| 1 | `albedo` | `Vec3` | base diffuse colour (linear RGB) |
| 2 | `specular` | `Vec3` | specular tint (linear RGB) |
| 3 | `shininess` | `double` | Blinn-Phong exponent (e.g. 16..256) |
| 4 | `reflectivity` | `double` | 0..1 mirror mix |
| 5 | `transparency` | `double` | 0..1 (0 = opaque) |
| 6 | `ior` | `double` | index of refraction |
| 7 | `is_water` | `int` | 1 ⇒ wave-normal perturbation + depth tint |
| 8 | `absorption` | `Vec3` | per-channel Beer-Lambert coefficient |
| 9 | `deep_color` | `Vec3` | colour of the deep transmissive medium |
| 10 | `beer_lambert` | `int` | 1 ⇒ attenuate along transmission path |
| 11 | `texture_kind` | `int` | `TextureKind`: 0 none, 1 checker, 2 stripes |
| 12 | `texture_scale` | `double` | world units per cell (0/≤0 ⇒ default 1) |
| 13 | `texture_color_a` | `Vec3` | first cell / band colour |
| 14 | `texture_color_b` | `Vec3` | second cell / band colour |

The parallel PBR work (t-025) adds, with backward-compatible defaults:

| # | Field | Type | Meaning |
|---|-------|------|---------|
| 15 | `metallic` | `double` | 0..1 conductor mix (binary 0/1 in practice) |
| 16 | `roughness` | `double` | 0..1 microfacet roughness (0 = mirror) |
| 17 | `emissive` | `Vec3` | self-lit linear RGB added to the shaded result |
| 18 | `pbr` | `int` | opt-in gate: 1 ⇒ use the Cook-Torrance path |

> **Byte-identity constraint.** Every new field must zero-initialise so that
> existing materials (and `scenes/default.scene`) are unaffected. `metallic = 0`,
> `roughness = 0`, `emissive = 0 0 0`, `pbr = 0` are the defaults. The canonical
> writer emits the new keys **only when non-default** (mirroring how
> `beer_lambert` is already emitted only when non-zero).

### 0.2 Preset-function convention (`src/scene_desc.c`)

Existing presets are **static, nullary, pure** functions returning a `Material`
by value. Each one:

1. `memset(&m, 0, sizeof m);` — so every unlisted field is a defined zero,
2. assigns only the fields the preset cares about,
3. calls `material_texture_defaults(&m);` last (sets `texture_kind = TEXTURE_NONE`,
   `texture_scale = 1.0`, `texture_color_a = {1,1,1}`, `texture_color_b = {0,0,0}`),
4. `return m;`.

The `type` keyword is dispatched in `sd_apply_material_key()` (around
`src/scene_desc.c:946`): the preset is stored as an integer `p->mat_type`
(1 = water, 2 = opaque, 3 = glass) and *swapped in as the base* at block close
(`src/scene_desc.c:1018`), **before** explicitly-set keys are overlaid. So the
rule is: **preset is the base, explicit keys override it.**

`sd_glass_preset()` and `sd_water_preset()` verified values:

```c
/* sd_water_preset() — type = water */
albedo=(0.05,0.15,0.20) specular=(0.90,0.90,0.90) shininess=256
reflectivity=1.0 transparency=0.85 ior=1.33 is_water=1
absorption=(0.45,0.12,0.06) deep_color=(0.02,0.10,0.16)

/* sd_glass_preset() — type = glass */
albedo=(0.02,0.02,0.02) specular=(1,1,1) shininess=256
reflectivity=0.0 transparency=1.0 ior=1.5 is_water=0 beer_lambert=0
absorption=(0,0,0) deep_color=(0.5,0.5,0.5)
```

### 0.3 Keyword spelling

`type = <name>` is **case-sensitive and lower-case** (`docs/scene_format.md` §2).
Only `water`, `opaque`, `glass` are currently valid; an unknown value is the hard
error `E_BAD_MATERIAL_TYPE` (`src/scene_desc.c:966`). The new preset keywords
therefore use **lower-case ASCII identifiers** (all valid under the
`[A-Za-z_][A-Za-z0-9_]*` identifier rule, §3):

```
gold  copper  silver  aluminum  iron  chrome  brass
plastic  rubber  ceramic  diamond  emissive
```

Note: `aluminum` (US spelling) is used, matching the `aluminium`/`aluminum`
choice in the reference source; use `aluminum` consistently. `chrome` is the
keyword for chromium (the reference source calls it "Chromium").

---

## 1. Sources and how to read the numbers

Two authoritative, widely-used PBR reference tables are cited:

- **[Unity]** Unity Shader Graph, *Metal Reflectance Node* — the canonical
  per-metal **linear F0** table. Source:
  `https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.shadergraph/Documentation~/Metal-Reflectance-Node.md`
  (rendered: `https://docs.unity3d.com/Packages/com.unity.shadergraph@15.0/manual/Metal-Reflectance-Node.html`).
  These are the exact values quoted in the task brief (gold `1.000, 0.766, 0.336`,
  etc.), so this is the primary F0 source.
- **[Filament]** Google Filament, *Physically Based Materials* (`Materials.md.html`).
  Source: `https://raw.githubusercontent.com/google/filament/main/docs/Materials.md.html`
  (rendered: `https://google.github.io/filament/Materials.html`). Provides the
  conductor base-colour table (sRGB), dielectric base colours, the
  metallic-binary rule, and the roughness/reflectance conventions.
- **[IoR]** Wikipedia, *List of refractive indices* — measured IOR values.
  Source: `https://en.wikipedia.org/wiki/List_of_refractive_indices`
  (diamond **2.417** at 589.29 nm; polycarbonate 1.60; etc.).
- **[PBR]** *Physically Based Rendering: From Theory to Implementation* (3rd ed.,
  online `https://pbr-book.org/`) — Schlick Fresnel / microfacet grounding, and
  the F0 = 0.04 default for dielectrics (reflectance ≈ 4%).

**Reading the tables.** All values below are **LINEAR RGB** in `[0,1]` unless
stated. The Unity F0 numbers are already linear. The Filament conductor table is
published in **sRGB** and is quoted here as sRGB alongside its linear conversion
(both given, so the reader can reconcile them). The two sources agree in *hue*
and order of magnitude; they differ in saturation because Unity's table is a set
of artist-facing "metal reflectance" constants (close to measured normal-incidence
F0 for the visible band, slightly desaturated) while Filament's is a
colour-picked base colour. **These values are approximate but physically
grounded** — real metals have wavelength-dependent, angle-dependent complex
IORs (n, k); a single RGB F0 is a three-band simplification. Pick the source that
matches your look; the **Unity F0 values are the ones specified by the task brief
and are recommended as the preset defaults.**

---

## 2. CONDUCTORS (metals)

Physical rule (both sources): a metal has **no diffuse** term; its **`albedo` is
its F0** (the specular colour), the reflected light is tinted by that colour, and
`metallic = 1`. In this renderer's convention we therefore set:

- `metallic = 1`, `roughness` per finish (below),
- `albedo = F0` (so the `(1 - metallic)` diffuse scaling removes the diffuse term,
  and `F0 = mix(vec3(0.04), albedo, metallic)` reduces exactly to `albedo`),
- `specular = F0` too, so that the **existing Blinn-Phong path** (used when
  `pbr = 0`) still tints the highlight with the metal colour and the preset is
  useful even without the PBR gate,
- `shininess` high (mirror-like Blinn-Phong fallback; `pow`-exponent of ~256 for
  polished, lower for brushed),
- `reflectivity = 0` (the PBR specular term already provides the reflection; a
  non-zero `reflectivity` would double-count),
- `ior = 1` (unused for metals; keep the neutral default),
- `transparency = 0`, `is_water = 0`, `beer_lambert = 0`, zero absorption/deep.

### 2.1 Conductor F0 table (linear RGB)

| Material | Keyword | F0 linear RGB [Unity] | Filament base colour (sRGB) | linear of Filament | Suggested `roughness` |
|----------|---------|------------------------|------------------------------|--------------------|------------------------|
| Gold | `gold` | `1.000 0.766 0.336` | `1.00 0.85 0.57` | `1.000 0.692 0.285` | 0.05 polished / 0.30 brushed |
| Copper | `copper` | `0.955 0.637 0.538` | `0.97 0.74 0.62` | `0.933 0.507 0.342` | 0.05 polished / 0.35 brushed |
| Silver | `silver` | `0.972 0.960 0.915` | `0.97 0.96 0.91` | `0.933 0.911 0.807` | 0.03 polished / 0.25 brushed |
| Aluminum | `aluminum` | `0.913 0.921 0.925` | `0.91 0.92 0.92` | `0.807 0.828 0.828` | 0.05 polished / 0.30 brushed |
| Iron/steel | `iron` | `0.560 0.570 0.580` | `0.77 0.78 0.78` | `0.554 0.570 0.570` | 0.10 polished / 0.40 brushed |
| Chrome | `chrome` | `0.550 0.556 0.554` | (Chromium, same as Unity) | — | 0.03 polished / 0.20 brushed |
| Brass | `brass` | `0.910 0.778 0.423` | `0.98 0.90 0.59` | `0.955 0.787 0.307` | 0.08 polished / 0.30 brushed |

> Unity's Iron F0 `0.560 0.570 0.580` is noticeably darker than Filament's
> colour-picked iron `0.77 0.78 0.78` (linear `0.554 0.570 0.570`) — note that
> Filament's *linear* value is very close to Unity's, so the two sources actually
> agree well once the sRGB→linear conversion is applied to Filament. The sRGB
> column is what differs; use the linear column.

**Roughness guidance (both sources):** `roughness` is `1 - glossiness`, in
`[0,1]`; 0 = perfectly smooth mirror, 1 = fully diffuse-like blur. Practical
bands: **polished ≈ 0.03–0.15**, **satin ≈ 0.15–0.30**, **brushed ≈ 0.30–0.45**,
**rough-cast ≈ 0.45–0.65**. The presets below choose a sensible polished default
per metal; authors override `roughness` for a brushed look.

### 2.2 Conductor preset struct values

Recommended polished defaults (`roughness` = polished value; `pbr = 1`):

```c
/* type = gold */
/* albedo */ (1.000, 0.766, 0.336),  /* specular */ (1.000, 0.766, 0.336),
/* shininess */ 256.0, /* reflectivity */ 0.0, /* transparency */ 0.0,
/* ior */ 1.0, /* is_water */ 0, /* absorption */ (0,0,0),
/* deep_color */ (0,0,0), /* beer_lambert */ 0,
/* texture */ NONE, 1.0, (1,1,1), (0,0,0),
/* metallic */ 1.0, /* roughness */ 0.05, /* emissive */ (0,0,0), /* pbr */ 1

/* type = copper */
albedo = specular = (0.955, 0.637, 0.538); shininess 256; metallic 1.0; roughness 0.05; pbr 1

/* type = silver */
albedo = specular = (0.972, 0.960, 0.915); shininess 256; metallic 1.0; roughness 0.03; pbr 1

/* type = aluminum */
albedo = specular = (0.913, 0.921, 0.925); shininess 256; metallic 1.0; roughness 0.05; pbr 1

/* type = iron */
albedo = specular = (0.560, 0.570, 0.580); shininess 256; metallic 1.0; roughness 0.10; pbr 1

/* type = chrome */
albedo = specular = (0.550, 0.556, 0.554); shininess 256; metallic 1.0; roughness 0.03; pbr 1

/* type = brass */
albedo = specular = (0.910, 0.778, 0.423); shininess 256; metallic 1.0; roughness 0.08; pbr 1
```

All of the above share: `reflectivity 0`, `transparency 0`, `ior 1`, `is_water 0`,
`beer_lambert 0`, `absorption 0 0 0`, `deep_color 0 0 0`, texture defaults,
`emissive 0 0 0`.

---

## 3. DIELECTRICS

Physical rule (Filament): a dielectric has **chromatic diffuse** and
**achromatic specular**. Its specular F0 is a flat `≈ 0.04` at normal incidence
(a "reflectance" of 4 %); `metallic = 0`. In this renderer the flat F0 comes from
the PBR path's `mix(vec3(0.04), albedo, 0) = vec3(0.04)`, so a dielectric preset
sets `metallic = 0`, `specular` small/neutral, and drives colour through
`albedo`. Filament warns that values below ~2 % reflectance are non-physical; keep
the default 4 %.

### 3.1 Plastic

| Parameter | Value | Justification |
|-----------|-------|---------------|
| `ior` | `1.46` | Typical polymer IOR (e.g. PET/PS/PMMA range 1.46–1.60) |
| `albedo` | tinted, e.g. `0.30 0.05 0.06` (deep red) | Plastic is dye-tinted; pick any saturated colour |
| `specular` | `0.05 0.05 0.05` | ≈ F0 4 % specular tint for the Blinn-Phong fallback |
| `shininess` | `64.0` | smooth plastic highlight (medium gloss) |
| `roughness` | `0.10` | moulded/polished plastic, low roughness |
| `metallic` | `0.0` | dielectric |
| `reflectivity` | `0.0` | no mirror mix (specular comes from BRDF) |
| `transparency` | `0.0` | opaque plastic |
| `pbr` | `1` | enable the microfacet path |

### 3.2 Rubber

| Parameter | Value | Justification |
|-----------|-------|---------------|
| `ior` | `1.50` | Typical elastomer IOR |
| `albedo` | `0.05 0.05 0.05` (dark) | Filament rubber base colour `0.21 0.21 0.21` sRGB ≈ `0.036` linear; use a dark near-black |
| `specular` | `0.04 0.04 0.04` | 4 % dielectric F0 |
| `shininess` | `8.0` | broad, weak Blinn-Phong highlight (matte) |
| `roughness` | `0.90` | matte, near-fully-rough |
| `metallic` | `0.0` | dielectric |
| `reflectivity` | `0.0` | — |
| `transparency` | `0.0` | — |
| `pbr` | `1` | — |

### 3.3 Ceramic

| Parameter | Value | Justification |
|-----------|-------|---------------|
| `ior` | `1.60` | Glazed ceramic/porcelain IOR ≈ 1.5–1.7; 1.60 mid-range |
| `albedo` | `0.85 0.85 0.82` (light) | Light, near-white ceramic body |
| `specular` | `0.05 0.05 0.05` | 4 % F0 |
| `shininess` | `128.0` | tight glaze highlight |
| `roughness` | `0.20` | glazed but not mirror; medium-low |
| `metallic` | `0.0` | dielectric |
| `reflectivity` | `0.0` | — |
| `transparency` | `0.0` | — |
| `pbr` | `1` | — |

### 3.4 Diamond

Diamond is a transparent dielectric, so it reuses the **glass / `beer_lambert`**
conventions from `sd_glass_preset()` (§0.2) with the measured diamond IOR.

| Parameter | Value | Justification |
|-----------|-------|---------------|
| `ior` | `2.417` | Measured diamond IOR at 589.29 nm [IoR] |
| `albedo` | `0.02 0.02 0.02` | Matches the glass preset (near-zero diffuse) |
| `specular` | `1 1 1` | Matches the glass preset |
| `shininess` | `256.0` | Matches the glass preset |
| `reflectivity` | `0.0` | Matches the glass preset (no mirror mix) |
| `transparency` | `1.0` | Fully transmissive |
| `is_water` | `0` | Must be 0: no wave-normal perturbation for a gem |
| `beer_lambert` | `0` | Leave attenuation opt-in (strict no-op at absorption 0) |
| `absorption` | `0 0 0` | Clear |
| `deep_color` | `0.5 0.5 0.5` | Neutral medium (matches glass) |
| `roughness` | `0.0` | Faceted, mirror-smooth surface |
| `metallic` | `0.0` | dielectric |
| `pbr` | `1` | — |

> Authors wanting a *tinted* gem can set `beer_lambert = 1` plus a non-zero
> `absorption` and a `deep_color`, exactly as documented for general refraction
> in `docs/scene_format.md` §4.3. The clear preset keeps attenuation off.

### 3.5 Dielectric preset struct values

```c
/* type = plastic */
albedo (0.30,0.05,0.06) specular (0.05,0.05,0.05) shininess 64.0
reflectivity 0.0 transparency 0.0 ior 1.46 is_water 0
absorption (0,0,0) deep_color (0,0,0) beer_lambert 0
texture defaults; metallic 0.0 roughness 0.10 emissive (0,0,0) pbr 1

/* type = rubber */
albedo (0.05,0.05,0.05) specular (0.04,0.04,0.04) shininess 8.0
reflectivity 0.0 transparency 0.0 ior 1.50 is_water 0
absorption (0,0,0) deep_color (0,0,0) beer_lambert 0
texture defaults; metallic 0.0 roughness 0.90 emissive (0,0,0) pbr 1

/* type = ceramic */
albedo (0.85,0.85,0.82) specular (0.05,0.05,0.05) shininess 128.0
reflectivity 0.0 transparency 0.0 ior 1.60 is_water 0
absorption (0,0,0) deep_color (0,0,0) beer_lambert 0
texture defaults; metallic 0.0 roughness 0.20 emissive (0,0,0) pbr 1

/* type = diamond */
albedo (0.02,0.02,0.02) specular (1,1,1) shininess 256.0
reflectivity 0.0 transparency 1.0 ior 2.417 is_water 0
absorption (0,0,0) deep_color (0.5,0.5,0.5) beer_lambert 0
texture defaults; metallic 0.0 roughness 0.0 emissive (0,0,0) pbr 1
```

---

## 4. EMISSIVE

An emitter is a surface that adds a self-lit colour to the shaded result
(`emissive`), independent of illumination. Because the renderer's `emissive` term
is a **linear RGB addition** (tone-mapped downstream), a warm-white lamp is best
expressed as a slightly-orange white with a strength above 1 for a visibly glowing
surface.

| Parameter | Value | Justification |
|-----------|-------|---------------|
| `emissive` | `1.0 0.85 0.65` (warm white) | Blackbody ~2700–3000 K reads as warm white (R > G > B) |
| `albedo` | `0.0 0.0 0.0` | A pure emitter has no diffuse response |
| `specular` | `0 0 0` | no highlight needed |
| `shininess` | `0.0` | no specular (0 ⇒ no Blinn-Phong highlight) |
| `roughness` | `0.5` | neutral (irrelevant for a pure emitter) |
| `metallic` | `0.0` | dielectric |
| `reflectivity` | `0.0` | — |
| `transparency` | `0.0` | — |
| `ior` | `1.0` | neutral default |
| `pbr` | `1` | keep the emitter in the PBR path |

> **Strength convention.** If the emitter must be brighter than the sky/sun to
> read as a light source, scale `emissive` above 1 (linear radiance is allowed to
> exceed 1 and is tone-mapped downstream, exactly like `sky.sun_color`). A value
> of `(1.0, 0.85, 0.65)` gives a gentle glow; `(4.0, 3.4, 2.6)` gives a strong
> lamp. The preset default below uses a mid value; authors override per scene.

```c
/* type = emissive */
albedo (0,0,0) specular (0,0,0) shininess 0.0
reflectivity 0.0 transparency 0.0 ior 1.0 is_water 0
absorption (0,0,0) deep_color (0,0,0) beer_lambert 0
texture defaults; metallic 0.0 roughness 0.5 emissive (1.0,0.85,0.65) pbr 1
```

---

## 5. Purity and determinism

**Every preset above is a pure function** — no randomness, no global state, no
I/O, no dynamic allocation, no dependence on the seed or time. Each is a nullary
`static Material sd_<name>_preset(void)` that fills a stack `Material` from
compile-time constants and returns it by value, exactly like the existing
`sd_water_preset()` / `sd_opaque_preset()` / `sd_glass_preset()`. Two calls in the
same run (or across runs) yield bit-identical results. This preserves the
renderer's reproducibility guarantee and lets `scenes/default.scene` remain
byte-identical (no preset is applied unless an author writes `type = <name>`).

> Implementation note (for t-027): extend the `type` dispatch at
> `src/scene_desc.c:959-966` with the new names, add matching
> `sd_<name>_preset()` functions next to the existing ones, and add the
> corresponding detection predicates in `src/scene_desc_write.c` (mirroring
> `mat_is_water_preset()` / `mat_is_glass_preset()`) so a preset material
> round-trips as `type = <name>`.

---

## 6. Preset summary table

| `type =` keyword | class | albedo (=F0 for metals) | specular | shininess | metallic | roughness | ior | transp. | emissive | pbr |
|------------------|-------|-------------------------|----------|-----------|----------|-----------|-----|---------|----------|-----|
| `gold` | conductor | 1.000 0.766 0.336 | = albedo | 256 | 1 | 0.05 | 1.0 | 0 | 0 | 1 |
| `copper` | conductor | 0.955 0.637 0.538 | = albedo | 256 | 1 | 0.05 | 1.0 | 0 | 0 | 1 |
| `silver` | conductor | 0.972 0.960 0.915 | = albedo | 256 | 1 | 0.03 | 1.0 | 0 | 0 | 1 |
| `aluminum` | conductor | 0.913 0.921 0.925 | = albedo | 256 | 1 | 0.05 | 1.0 | 0 | 0 | 1 |
| `iron` | conductor | 0.560 0.570 0.580 | = albedo | 256 | 1 | 0.10 | 1.0 | 0 | 0 | 1 |
| `chrome` | conductor | 0.550 0.556 0.554 | = albedo | 256 | 1 | 0.03 | 1.0 | 0 | 0 | 1 |
| `brass` | conductor | 0.910 0.778 0.423 | = albedo | 256 | 1 | 0.08 | 1.0 | 0 | 0 | 1 |
| `plastic` | dielectric | 0.30 0.05 0.06 (tint) | 0.05 | 64 | 0 | 0.10 | 1.46 | 0 | 0 | 1 |
| `rubber` | dielectric | 0.05 0.05 0.05 | 0.04 | 8 | 0 | 0.90 | 1.50 | 0 | 0 | 1 |
| `ceramic` | dielectric | 0.85 0.85 0.82 | 0.05 | 128 | 0 | 0.20 | 1.60 | 0 | 0 | 1 |
| `diamond` | refractive | 0.02 0.02 0.02 | 1 1 1 | 256 | 0 | 0.00 | 2.417 | 1.0 | 0 | 1 |
| `emissive` | emitter | 0 0 0 | 0 0 0 | 0 | 0 | 0.50 | 1.0 | 0 | 1.0 0.85 0.65 | 1 |

Common to all presets: `reflectivity = 0`, `is_water = 0`, `beer_lambert = 0`,
`absorption = 0 0 0`, `deep_color = 0 0 0` (except `diamond`:
`deep_color = 0.5 0.5 0.5`), and texture defaults
(`texture = none`, `texture_scale = 1`, `texture_color_a = 1 1 1`,
`texture_color_b = 0 0 0`).

---

## 7. Reference URLs

- Unity Shader Graph — Metal Reflectance Node (linear F0 table):
  https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.shadergraph/Documentation~/Metal-Reflectance-Node.md
- Google Filament — Physically Based Materials (conductor/dielectric base colours,
  metallic rule, roughness & reflectance conventions):
  https://raw.githubusercontent.com/google/filament/main/docs/Materials.md.html
  (rendered: https://google.github.io/filament/Materials.html)
- Wikipedia — List of refractive indices (diamond 2.417; polymer/glass IORs):
  https://en.wikipedia.org/wiki/List_of_refractive_indices
- *Physically Based Rendering: From Theory to Implementation* (3rd ed. online):
  https://pbr-book.org/
