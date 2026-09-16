# Tone-mapping / exposure comparison: `pathtrace_to_byte` vs Whitted `to_byte`

Scope: compare the two linear-radiance → 8-bit-channel functions only.
No verdict, no fix proposals.

## 1. Function bodies (exact line numbers)

### Path tracer — `src/pathtrace.c:441-457`

```c
441: unsigned char pathtrace_to_byte(double linear)
442: {
443:     double c = linear;
444:     if (!(c >= 0.0)) {
445:         c = 0.0; /* also catches NaN and negatives */
446:     } else if (c > 1.0) {
447:         c = 1.0;
448:     }
449:     double v = pow(c, 1.0 / 2.2) * 255.0;
450:     int iv = (int)(v + 0.5);
451:     if (iv < 0) {
452:         iv = 0;
453:     } else if (iv > 255) {
454:         iv = 255;
455:     }
456:     return (unsigned char)iv;
457: }
```

### Whitted renderer — `src/render.c:860-875` (file-static `to_byte`)

```c
860: static unsigned char to_byte(double c)
861: {
862:     if (c < 0.0) {
863:         c = 0.0;
864:     } else if (c > 1.0) {
865:         c = 1.0;
866:     }
867:     double v = pow(c, 1.0 / 2.2) * 255.0;
868:     int iv = (int)(v + 0.5);
869:     if (iv < 0) {
870:         iv = 0;
871:     } else if (iv > 255) {
872:         iv = 255;
873:     }
874:     return (unsigned char)iv;
875: }
```

## 2. Side-by-side comparison

| Property | `pathtrace_to_byte` (pathtrace.c:441) | `to_byte` (render.c:860) |
|---|---|---|
| Input range accepted | any `double` (linear radiance) | any `double` (linear radiance) |
| Exposure / scaling constant | none | none |
| Gamma correction | yes, `pow(c, 1.0/2.2)` | yes, `pow(c, 1.0/2.2)` |
| Gamma exponent | `1/2.2` ≈ 0.454545 | `1/2.2` ≈ 0.454545 |
| Input clamp | to `[0,1]`; negatives **and NaN** → 0, `>1` → 1 | to `[0,1]`; negatives → 0, `>1` → 1 |
| Post-scale quantize | `(int)(v + 0.5)`, then clamp `[0,255]` | `(int)(v + 0.5)`, then clamp `[0,255]` |
| White point | linear `c == 1.0` → 255 | linear `c == 1.0` → 255 |
| Samples averaged before tone map | `spp` (see §4) | `spp` (see §4) |

**Differences affecting brightness / clipping:** NONE observed between the two
functions themselves. Identical exponent, identical scale (`* 255.0`), identical
clamp bounds and rounding. The only textual difference is the NaN guard in
`pathtrace_to_byte` (line 444 `!(c >= 0.0)` vs line 862 `c < 0.0`): for NaN,
path tracer yields 0, Whitted's `c < 0.0` is false so NaN falls through to
`pow(NaN, 1/2.2)` → NaN → `(int)(NaN + 0.5)` (implementation-defined) — this is
a NaN-handling divergence, not a brightness/clipping difference for finite input.

## 3. Exposure search

`grep -rn "exposure" src/` → **no matches**. Neither path applies an exposure
multiplier or any brightness constant; both rely on scene radiance already
landing in `[0,1]` before quantization.

## 4. Radiance accumulation at each call site

Both paths average `spp` samples identically before tone mapping:

- Path tracer — `src/pathtrace.c:749-785` (`pt_render_pixel`):
  `acc = vec3_add(acc, pathtrace_radiance(...))` in the `s`-loop (line 777),
  then `acc = vec3_scale(acc, 1.0 / (double)spp)` (line 780), then
  `pathtrace_to_byte(acc.{x,y,z})` (lines 783-785).

- Whitted — `src/render.c:892-932` (`render_region`):
  `Vec3 acc = vec3(0,0,0)` (line 899); `acc = vec3_add(acc, trace(...))` in the
  `s`-loop (lines 925-926), then `acc = vec3_scale(acc, 1.0 / (double)spp)`
  (line 928), then `to_byte(acc.{x,y,z})` (lines 930-932).

Both divide the accumulated sum by `spp` via the same `1.0 / (double)spp`
factor, so the mean linear radiance fed into each tone map is scaled identically.

## 5. Summary of any brightness/clipping divergence

- Tone-map functions: **byte-for-byte equivalent for finite inputs** (same
  clamp → `pow(·, 1/2.2)` → `×255` → round → clamp).
- No exposure constant in either path.
- Accumulation: identical mean-by-`spp` before tone mapping.
- Only divergence is NaN handling (path tracer guards it, Whitted does not).
  Any observed brightness/clipping difference between the two paths must
  therefore originate upstream (per-sample radiance / lighting estimators),
  not in these two tone-map functions.
