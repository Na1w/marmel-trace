# Research: Refraction gaps for general (non-water) refractive / glass materials

READ-ONLY investigation of the existing Whitted refraction path and the exact
gap preventing *general* transmissive materials (glass, gems). Proposes a
minimal, byte-identity-safe design. Line numbers are as found in the workspace.

## 1. `src/render.c` — transmission block in `trace_hit`

`trace_hit` is declared at `src/render.c:124`. The path is generic **except**
for two `m->is_water` gates.

**1.1 Normal selection** — `src/render.c:138-140`, the only normal gate:
```c
if (m->is_water) {
    N = water_normal(P.x, P.z, time); /* wave-perturbed normal */
}
```
A non-water material keeps `h->normal` (already flipped at L135). That is
exactly what glass wants: no wave perturbation.

**1.2 Reflection** — `src/render.c:204`: `Vec3 rd = vec3_reflect(r.dir, N);`
then recursive `trace(...)` (L205-207). Material-agnostic.

**1.3 Fresnel / Schlick with `eta`, `sin2_t`, TIR** — already generic, uses
only `m->ior` and `h->front_face`:
- L226-228: `f0 = ((1 - ior)/(1 + ior))^2`
- L230: `double eta = (h->front_face == 1) ? (1.0 / m->ior) : m->ior;`
- L233: `double sin2_t = eta * eta * (1.0 - cos_i * cos_i);`
- L237-239: TIR `if (sin2_t > 1.0) { F = 1.0; total_internal = 1; }`
- L241-243: else `cos_t = sqrt(1.0 - sin2_t); F = fresnel_schlick(cos_t, f0);`

**1.4 Refraction + inner reuse**:
- L246: `if (m->transparency > 0.0) {`
- L247-249: TIR → `refr_col = refl_col;`
- L254: `Vec3 rd = vec3_refract(r.dir, N, eta);`
- L255-256: `Ray rr = {vec3_add(P, vec3_scale(rd, 1e-3)), rd};`
- L257: `if (scene_intersect(scene, rr, 1e-4, 1e30, &h2)) {`
- L260-261: inner `trace_hit(scene, rr, depth+1, max_depth, time, &h2, seed_key)`
  — the exit hit is reused (intersected exactly once).

**1.5 THE GAP — the two `is_water` attenuation gates.**

Gate A (inner hit found), `src/render.c:262-269`:
```c
262:                 if (m->is_water) {
266:                     refr_col = water_attenuate(m, inner, h2.t);
267:                 } else {
268:                     refr_col = inner;
269:                 }
```
Gate B (ray escapes, no further hit), `src/render.c:271-278`:
```c
271:                 Vec3 inner = sky_sample(rd, &scene->sky);
272:                 if (m->is_water) {
275:                     refr_col = water_attenuate(m, inner, 5.0);
276:                 } else {
277:                     refr_col = inner;
278:                 }
```
So `m->is_water` selects `water_attenuate(...)` (distance `h2.t` inside, fixed
`5.0` on escape); otherwise `refr_col = inner` — the raw transmitted colour with
**no medium absorption**. A general glass material (`is_water = 0`) has its
`absorption` / `deep_color` parsed and stored but **silently ignored** at render
time. That is the whole gap: no Beer-Lambert attenuation without water, and no
way to attenuate while keeping the un-perturbed normal.

**1.6 Compositing** — L281-282 `surface = F*refl_col + (1-F)*refr_col;`, L283
`return vec3_lerp(color, surface, m->transparency);`. Generic; unaffected.

## 2. `src/material.h` — `Material` + declarations

`Material` at `src/material.h:52-72`, fields in order: `albedo` (L54),
`specular` (L55), `shininess` (L56), `reflectivity` (L57), `transparency` (L58),
`ior` (L59), `is_water` (L60), `absorption` (L61), `deep_color` (L62),
`texture_kind` (L65), `texture_scale` (L66), `texture_color_a` (L67),
`texture_color_b` (L68).

Declarations: `double fresnel_schlick(double, double)`; `Vec3 sky_sample(Vec3,
const SkyParams*)`; `Vec3 water_normal(double x, double z, double time)`;
`Vec3 water_attenuate(const Material*, Vec3 inner, double depth)`;
`void material_texture_defaults(Material*)`.

Note the `is_water` comment (L59): *"1 => wave-normal perturbation + depth
tint"* — the flag currently conflates two independent behaviours. A new
`beer_lambert` flag must mean **attenuation only, no normal perturbation**.

## 3. `src/material.c` — water helpers

- `water_normal` (`src/material.c:332`): sum of 4 directional sine waves
  (`WATER_WAVES`, L317-323) with analytic derivatives plus a small fBm ripple
  (central differences, L353-366), returns `vec3_normalize(vec3(-dhdx, 1, -dhdz))`.
  Pure **normal perturbation** — unrelated to attenuation; glass simply never
  calls it (`is_water = 0`).
- `water_attenuate` (`src/material.c:399`): per-channel transmittance
  `T_c = exp(-absorption_c * depth)` via static `water_transmittance` (L384-398;
  `T = 1` when `absorption_c == 0`), then the blend
  `out_c = inner_c * T_c + deep_color_c * (1 - T_c)`. `depth -> 0` ⇒ `inner`,
  `depth -> inf` ⇒ `deep_color`. It reads only `m->absorption` / `m->deep_color`,
  is NULL-safe, and is already material-generic — **reusable as-is for glass**;
  no new function needed. (`water_depth_tint` at L428 is a deprecated wrapper.)

## 4. `src/scene_desc.c` — parsing

- `MAT_*` enum (`src/scene_desc.c:473-490`): `MAT_ALBEDO`…`MAT_DEEP_COLOR`
  (bits 0-8), `MAT_TYPE` (bit 9), `MAT_TEXTURE_KIND`/`MAT_TEX_SCALE`/
  `MAT_TEX_COLOR_A`/`MAT_TEX_COLOR_B` (bits 10-13). **Bit 14 is free.**
- `MAT_KEYS` (`src/scene_desc.c:492-505`): already-parseable generic keys are
  `albedo`, `specular`, `shininess`, `reflectivity`, `transparency`, `ior`,
  `is_water` (KT_BOOL), `absorption`, `deep_color`, `texture_scale`,
  `texture_color_a`, `texture_color_b`. `type` and `texture` are enum-ish and
  handled specially, not via `MAT_KEYS`.
- `sd_apply_material_key` (`src/scene_desc.c:909`): special-cases `type`
  (L913-931) — `"water"` → `p->mat_type = 1` (L927), `"opaque"` →
  `p->mat_type = 2` (L929), else hard error (L931: *"unknown material type,
  expected 'water' or 'opaque'"*). `texture` is the second special case
  (L938-963). Everything else falls through to `sd_apply_key` (L969).
  `Parser.mat_type` documented at `src/scene_desc.c:143` (0 none / 1 water / 2 opaque).
- `sd_water_preset` (L511-527): albedo `.05 .15 .20`, specular `.90 .90 .90`,
  shininess 256, reflectivity 1.0, transparency 0.85, ior 1.33, is_water 1,
  absorption `.45 .12 .06`, deep_color `.02 .10 .16`.
- `sd_opaque_preset` (L530-541): all-zero, `ior = 1.0`, not water.
- `sd_mat_begin` (L545-551): starts from `sd_opaque_preset()`, `mat_type = 0`.
- `sd_close_material` (L979-994): `final` = selected preset (water/opaque) or
  the accumulated `p->mat`; if a `type` was given, `sd_overlay_explicit(&final,
  &p->mat, b->seen)` (L985-986, impl L893-907) copies **only explicitly-set**
  keys over the preset. So `type = glass { ior = 1.7 }` gets preset defaults +
  overridden `ior` for free.

**Conclusion:** all ten physical keys already parse. Parser work for glass = a
third `type` name + one new generic key.

## 5. `src/scene_desc_write.c` — canonical emission

- `mat_is_opaque_preset` (L292-299) and `mat_is_water_preset` (L302-311) are
  exact field-by-field equality tests (plus `mat_texture_is_default`, L281). The
  opaque test requires `is_water == 0` and zero absorption/deep_color — a
  `beer_lambert` flag must be added to **both** predicates' default side so a
  non-default flag cannot match a preset.
- `emit_material` (L424-448): emits `type = water` / `type = opaque` when a
  preset matches (L432-435); otherwise emits every physical key
  **unconditionally** (L437-445), then `emit_material_texture`.
- `emit_material_texture` (L395-421) is the existing **conditional-emission**
  template (emits nothing when all fields are default). A `beer_lambert` key
  should follow it: emit `w_key_int(w, "beer_lambert", 1)` **only when
  non-zero**.
- Adding `type = glass`: add `mat_is_glass_preset` (mirroring L302-311) and a
  branch in `emit_material` **after** the water/opaque checks so their behaviour
  is unchanged.

## 6. Byte-identity constraints — every site to guard

Invariants: (1) `type = water` keeps exact behaviour; (2) `scenes/default.scene`
stays byte-identical to `DEFAULT_SCENE_TEXT` (`src/default_scene_text.h`);
(3) the default (`no --scene`) render stays byte-identical.

| Site | File:line | Guard |
|---|---|---|
| Water preset values | `src/scene_desc.c:511-527` | do not touch |
| Water render gates | `src/render.c:138,262,272` | change predicate only; keep `water_attenuate` args (`h2.t`, `5.0`) |
| Opaque preset | `src/scene_desc.c:530-541` | keep `beer_lambert = 0` |
| `sd_mat_begin` | `src/scene_desc.c:545-551` | starts from opaque preset ⇒ flag 0 automatically |
| `sd_close_material` overlay | `src/scene_desc.c:979-994` | new key applied only when `seen & MAT_BEER_LAMBERT` |
| `sd_overlay_explicit` | `src/scene_desc.c:893-907` | add `if (seen & MAT_BEER_LAMBERT) dst->beer_lambert = src->beer_lambert;` |
| `mat_is_opaque_preset` | `src/scene_desc_write.c:292-299` | add `m->beer_lambert == 0` |
| `mat_is_water_preset` | `src/scene_desc_write.c:302-311` | add `m->beer_lambert == 0` |
| `emit_material` per-key branch | `src/scene_desc_write.c:437-445` | emit `beer_lambert` only when non-zero |
| built-in material table | `src/scene.c:156-201` | `memset(...,0,...)` (L156) zeroes the new field |
| `scene_default_desc` | `src/scene.c:624-680` | built-ins all-zero/water ⇒ nothing extra emitted |
| `default_scene_text.h` | whole file | must not be regenerated/edited |
| `scenes/default.scene` | whole file | must not be edited |

Because the flag defaults to 0 and is emitted only when non-zero, the default
scene emits no new line and a `type = water` block still emits exactly
`type = water`.

## 7. Proposed design

**7.1 New `type = glass` preset** — add `sd_glass_preset()` in
`src/scene_desc.c` (mirroring `sd_water_preset`, L511):

| Field | Value | Rationale |
|---|---|---|
| `albedo` | `0.02 0.02 0.02` | near-black, no diffuse body colour |
| `specular` | `1.0 1.0 1.0` | bright highlight |
| `shininess` | `256.0` | tight highlight |
| `reflectivity` | `0.0` | energy via Fresnel/transmission, not mirror mix |
| `transparency` | `1.0` | fully transmissive |
| `ior` | `1.5` | typical crown glass |
| `is_water` | `0` | **no** wave-normal perturbation |
| `absorption` | `0 0 0` | clear by default (opt-in tint) |
| `deep_color` | `0.5 0.5 0.5` | neutral medium colour |
| texture | `material_texture_defaults()` | untextured |

`is_water = 0` keeps the geometric normal (L138 gate false) — the non-perturbing
behaviour glass needs.

**7.2 New per-material flag `beer_lambert`** — a plain POD int:
- Struct: add `int beer_lambert;` to `Material` (`src/material.h`, after L62);
  zero-init keeps every existing material non-attenuating.
- Parser: add `MAT_BEER_LAMBERT = 1u << 14` (`src/scene_desc.c:490`); a
  `{ "beer_lambert", KT_BOOL, MAT_BEER_LAMBERT, offsetof(Material, beer_lambert) }`
  row in `MAT_KEYS` (after L501); the overlay line in `sd_overlay_explicit`
  (L893-907).
- Writer: emit `w_key_int(w, "beer_lambert", 1)` only when non-zero in the
  explicit branch of `emit_material` (L437-445); add `m->beer_lambert == 0` to
  both preset predicates (L292-311).

**7.3 Exact `render.c` gate change** — replace both `m->is_water` predicates
with `if (m->is_water || m->beer_lambert) {`, **keeping arguments identical**:
- `src/render.c:262` → `if (m->is_water || m->beer_lambert) {` (L266
  `water_attenuate(m, inner, h2.t)` unchanged)
- `src/render.c:272` → `if (m->is_water || m->beer_lambert) {` (L275
  `water_attenuate(m, inner, 5.0)` unchanged)

The `is_water` normal gate at L138 is left untouched, so `beer_lambert`
materials keep the geometric normal while still attenuating. Reusing
`water_attenuate` needs no new function.

**Byte-identity proof for `beer_lambert = 0`:** `m->is_water || m->beer_lambert`
reduces to `m->is_water`, so both branches take the identical `else`
(`refr_col = inner`, L268/L277). Water behaviour is unchanged; a glass material
with default `absorption = 0` / `deep_color` returns `inner` verbatim anyway
(`T_c = 1`), so even `beer_lambert = 1` with zero absorption is a strict no-op.

**7.4 Example usage:**
```
material glass { type = glass }
material tinted_glass {
    type = glass
    absorption = 0.3 0.1 0.05
    deep_color = 0.1 0.2 0.25
    beer_lambert = 1
}
```

## Proposed implementation checklist

1. `src/material.h` — add `int beer_lambert;` to `Material` (after L62); note
   attenuation-only semantics (no normal perturbation).
2. `src/scene_desc.c` — add `MAT_BEER_LAMBERT = 1u << 14` (after L489); add the
   `beer_lambert` row to `MAT_KEYS` (after L501); add the `sd_overlay_explicit`
   line (L893-907); add `sd_glass_preset()` near L511 (§7.1 values); accept
   `"glass"` → `p->mat_type = 3` in `sd_apply_material_key` (L913-931, update
   error text L931); map `mat_type == 3` to `sd_glass_preset()` in
   `sd_close_material` (L979-994).
3. `src/render.c` — change the predicates at L262 and L272 to
   `if (m->is_water || m->beer_lambert) {` (arguments unchanged).
4. `src/scene_desc_write.c` — add `m->beer_lambert == 0` to
   `mat_is_opaque_preset` (L292-299) and `mat_is_water_preset` (L302-311); add
   `mat_is_glass_preset`; add an `else if (mat_is_glass_preset(m)) w_key_name(w,
   "type", "glass");` branch in `emit_material` (after L435); emit
   `beer_lambert` only when non-zero (L437-445).
5. `docs/scene_format.md` — document the `glass` preset and `beer_lambert` key
   (§4.3 rows + §9.2 preset table).
6. Verify (no source edits): `scenes/default.scene` byte-equal to
   `DEFAULT_SCENE_TEXT`; default render matches the pre-change baseline;
   `type = water` output unchanged.
7. Do **not** edit `scenes/default.scene` or `src/default_scene_text.h`.

MISSION COMPLETE
