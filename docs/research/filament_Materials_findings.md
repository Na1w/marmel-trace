# Filament "Physically Based Materials" — verbatim extraction

## Source

- URL fetched: `https://raw.githubusercontent.com/google/filament/main/docs/Materials.md.html`
- Rendered page: `https://google.github.io/filament/Materials.html`
- HTTP status: `200` (fetch SUCCEEDED)
- Local copy: `docs/research/filament_Materials.md.html` (147,515 bytes, 3,302 lines)
- Raw section: lines 245–285 (`### Base color`)

> IMPORTANT STRUCTURAL NOTE: On the current `main` branch, the two base-color tables do **NOT**
> have the columns `Material | sRGB | Linear` as assumed in the request. Their actual columns are:
> `Metal | sRGB | Hexadecimal | Color`. There is **no "Linear" column** in either table.
> The values shown under "sRGB" are given as normalized triples (0..1).

---

## 1. Base color table for CONDUCTORS (metals)

Table caption reference: `[Table [baseColorsConductors]: baseColor for common metals]`
Header: `Metal | sRGB | Hexadecimal | Color`

| Metal | sRGB | Hexadecimal |
|---|---|---|
| Silver | 0.97, 0.96, 0.91 | #f7f4e8 |
| Aluminum | 0.91, 0.92, 0.92 | #e8eaea |
| Titanium | 0.76, 0.73, 0.69 | #c1baaf |
| Iron | 0.77, 0.78, 0.78 | #c4c6c6 |
| Platinum | 0.83, 0.81, 0.78 | #d3cec6 |
| Gold | 1.00, 0.85, 0.57 | #ffd891 |
| Brass | 0.98, 0.90, 0.59 | #f9e596 |
| Copper | 0.97, 0.74, 0.62 | #f7bc9e |

- Metals listed: Silver, Aluminum, Titanium, Iron, Platinum, Gold, Brass, Copper (8 total).
- NOT present: Chromium, Nickel, Cobalt.
- There is NO "Linear" column; therefore no linear triples are reported on the page for metals.
- Accompanying prose: "Real-world values are typically found in the range $[170..255]$ if the
  value is encoded between 0 and 255, or in the range $[0.66..1.0]$ between 0 and 1."

---

## 2. Base color table for DIELECTRICS (non-metals)

Table caption reference: `[Table [baseColorsDielectrics]: baseColor for common non-metals]`
Header: `Metal | sRGB | Hexadecimal | Color`

| Metal | sRGB | Hexadecimal |
|---|---|---|
| Coal | 0.19, 0.19, 0.19 | #323232 |
| Rubber | 0.21, 0.21, 0.21 | #353535 |
| Mud | 0.33, 0.24, 0.19 | #553d31 |
| Wood | 0.53, 0.36, 0.24 | #875c3c |
| Vegetation | 0.48, 0.51, 0.31 | #7b824e |
| Brick | 0.58, 0.49, 0.46 | #947d75 |
| Sand | 0.69, 0.66, 0.52 | #b1a884 |
| Concrete | 0.75, 0.75, 0.73 | #c0bfbb |

- Materials listed: Coal, Rubber, Mud, Wood, Vegetation, Brick, Sand, Concrete (8 total).
- NOT present in this table: Water, Plastic, Glass, Diamond, Iron, Copper, Gold, Aluminum, Silver.
  These names do NOT appear in the dielectric base-color table at all. (Rubber IS present, value
  0.21, 0.21, 0.21.)
- There is NO "Linear" column.
- Accompanying prose: "Real-world values are typically found in the range $[10..240]$ if the value
  is encoded between 0 and 255, or in the range $[0.04..0.94]$ between 0 and 1."

---

## 3. Default dielectric reflectance / F0 and metallic/roughness conventions

From the `### Reflectance` section (lines ~339–370):

- "the default value of 0.5 corresponds to a reflectance of 4%. Values below 0.35 (2% reflectance)
  should be avoided as no real-world materials have such low reflectance."
- Default value row of table `[Table [commonMatReflectance]: Reflectance of common materials]`:
  `Default value | 4% | 1.5 | 0.5`
  (columns: Material | Reflectance | IOR | Linear value)

So the default dielectric Fresnel reflectance at normal incidence (F0) is **4%**, expressed as a
normalized `reflectance` of **0.5**, which corresponds to IOR **1.5** (linear value 0.5).

### Reflectance of common materials table (verbatim, lines 356–371)

| Material | Reflectance | IOR | Linear value |
|---|---|---|---|
| Water | 2% | 1.33 | 0.35 |
| Fabric | 4% to 5.6% | 1.5 to 1.62 | 0.5 to 0.59 |
| Common liquids | 2% to 4% | 1.33 to 1.5 | 0.35 to 0.5 |
| Common gemstones | 5% to 16% | 1.58 to 2.33 | 0.56 to 1.0 |
| Plastics, glass | 4% to 5% | 1.5 to 1.58 | 0.5 to 0.56 |
| Other dielectric materials | 2% to 5% | 1.33 to 1.58 | 0.35 to 0.56 |
| Eyes | 2.5% | 1.38 | 0.39 |
| Skin | 2.8% | 1.4 | 0.42 |
| Hair | 4.6% | 1.55 | 0.54 |
| Teeth | 5.8% | 1.63 | 0.6 |
| Default value | 4% | 1.5 | 0.5 |

### Metallic / roughness conventions (verbatim)

- `metallic` (table `[standardProperties]`): "Whether a surface appears to be dielectric (0.0) or
  conductor (1.0). Often used as a binary value (0 or 1)". Range `[0..1]`, note "Should be 0 or 1".
- `roughness` (table `[standardProperties]`): "Perceived smoothness (1.0) or roughness (0.0) of a
  surface. Smooth surfaces exhibit sharp reflections". Range `[0..1]`.
- From `### Roughness`: "When roughness is set to 0, the surface is perfectly smooth and highly
  glossy. ... This property is often called _glossiness_ in other engines and tools, and is simply
  the opposite of the roughness (roughness = 1 - glossiness)."
- From `### Metallic`: "Non-metallic surfaces have chromatic diffuse reflection and achromatic
  specular reflection ... Metallic surfaces do not have any diffuse reflection and chromatic
  specular reflection (reflected light takes on the color of the surfaced as defined by baseColor)."
- `baseColor` (standardProperties): "Diffuse albedo for non-metallic surfaces, and specular color
  for metallic surfaces". Type float4, range [0..1], "Pre-multiplied linear RGB".

---

## Other tables where Water / Plastic / Glass / Diamond DO appear

These materials are NOT in the base-color dielectric table but appear in adjacent reference tables:

- Index of refraction `[Table [commonMatIOR]: Index of refraction of common materials]` (line ~685):
  - `Water | 1.33`
  - `Plastics, glass | 1.5 to 1.58`
- Dispersion `[Table [commonMatDispersion]: Dispersion of common materials]` (lines 825–827):
  - `Diamond | 55 | 0.36`
  - `Crown Glass | 59 | 0.33`
  - `Water | 55 | 0.36`
