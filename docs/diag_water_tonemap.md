# Diagnostic C — Pond/water near-white: tone-mapping / exposure hypothesis

**Task:** t-123 · **Mode:** bounded diagnosis, no source change
**Workspace:** `/Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer`

## Verdict

> ### HYPOTHESIS C: **REFUTED**
>
> The path tracer's `pathtrace_to_byte` and the Whitted renderer's `to_byte` are
> **byte-for-byte identical for all finite inputs** (verified empirically, not
> just by reading), and **no exposure or additional gamma constant exists
> anywhere in `src/`**. The two render modes therefore apply the *same*
> tone-mapping operator. The water is near-white in pathtrace mode because the
> **linear radiance reaching the tone map is far higher in the water region of
> the path tracer** — the difference is upstream (radiance computation), not in
> the tone map / exposure.
>
> Decisive empirical evidence: the **sky region is bit-identical** between the
> two modes (mean channel diff `+0.0`), while only the **water region** diverges
> by ~`+180` per channel. A tone-map/exposure mismatch would shift *both*
> regions globally; it does not.

---

## 1. Function bodies (verified line numbers)

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

## 2. Empirical equivalence test of the two tone maps

Rather than only reading the code, both bodies were copied **verbatim** into
`output/diagC_tonemap_equiv.c`, compiled (`cc -O2 … -lm`) and swept:

```
finite-input sweep [-0.5,1.5] x2e6+1: mismatches=0
x=0          pt=  0 whitted=  0
x=0.001      pt= 11 whitted= 11
x=0.01       pt= 31 whitted= 31
x=0.05       pt= 65 whitted= 65
x=0.1        pt= 90 whitted= 90
x=0.25       pt=136 whitted=136
x=0.5        pt=186 whitted=186
x=0.75       pt=224 whitted=224
x=1          pt=255 whitted=255
x=1.5        pt=255 whitted=255
x=1e+09      pt=255 whitted=255
x=NaN pt=  0 whitted=255 (documented divergence)
```

* **2,000,001 finite samples** across `[-0.5, 1.5]` → **0 mismatches**.
* The only divergence is **NaN**: `pathtrace_to_byte` returns `0`
  (`!(c >= 0.0)` guard, line 444), while `to_byte` falls through `c < 0.0`
  (false for NaN) → `pow(NaN,1/2.2)` → implementation-defined `(int)` cast.
  This cannot brighten the water (it would *darken* a NaN channel to 0 in the
  path tracer, the opposite of the observed behaviour) and is irrelevant here.

The tone-map is provably a **single shared operator**: clamp to `[0,1]` →
`pow(c, 1/2.2)` → `×255` → round → clamp int. Same exponent, same scale, same
clamps.

## 3. Exposure / gamma constant search

```
$ grep -rn -i "exposure" src/
(no matches)
```

No exposure multiplier, no brightness/scale constant, and no second gamma stage
exists in either pipeline. The only gamma in both renderers is the shared
`pow(c, 1.0 / 2.2)` inside the two tone-map functions above. (The
`gradient_gamma = 0.6` seen in `src/material.c:68,358` is a **sky-gradient**
parameter inside `sky_sample`, shared by both renderers — not a display gamma
or exposure.) Both call sites also average samples identically before tone
mapping:

* `src/pathtrace.c:749-785` — `acc = vec3_add(acc, pathtrace_radiance(...))` in
  the `s`-loop, then `acc = vec3_scale(acc, 1.0 / (double)spp)` at line 780,
  then `pathtrace_to_byte(acc.{x,y,z})` at lines 783-785.
* `src/render.c:892-932` — `acc = vec3_add(acc, trace(...))` at lines 925-926,
  then `acc = vec3_scale(acc, 1.0 / (double)spp)` at line 928, then
  `to_byte(acc.{x,y,z})` at lines 930-932.

Same mean-by-`spp`, same operator ⇒ **no tone-map/exposure mismatch is possible
between the two paths.**

## 4. Empirical render comparison

Reproduction (did **not** touch `output/scene.bmp`):

```
./raytracer --out output/diagC_pt.bmp     --width 320 --height 180 --samples 8 --no-progress
./raytracer --no-pathtrace --no-adaptive  --out output/diagC_whitted.bmp \
            --width 320 --height 180 --samples 8 --no-progress
```

Region split (top = sky/ground, bottom = water), analysed with
`output/diagC_analyze.py`:

```
--- pathtrace ---
top(sky/gnd)   n= 28800  mean=( 212.4, 227.1, 240.7)  near-white= 11.93%  any255= 19.60%
bottom(water)  n= 28800  mean=( 229.7, 244.4, 235.7)  near-white= 45.03%  any255= 77.98%
--- whitted ---
top(sky/gnd)   n= 28800  mean=( 209.1, 222.4, 237.8)  near-white= 11.93%  any255= 19.60%
bottom(water)  n= 28800  mean=(  64.0,  83.4,  76.8)  near-white=  0.00%  any255=  0.00%
```

Per-row-band breakdown (`output/diagC_bands.py`):

```
row-band  | pathtrace mean R,G,B   nw% | whitted mean R,G,B   nw% | dR,dG,dB
[  0, 15) |  211.7  226.5  250.8   6.1 |  211.7  226.5  250.8   6.1 |   +0.0   +0.0   +0.0
[ 15, 30) |  220.1  232.5  252.2  22.4 |  220.1  232.5  252.2  22.4 |   +0.0   +0.0   +0.0
[ 30, 45) |  222.3  233.6  252.0  18.8 |  222.3  233.6  252.0  18.8 |   +0.0   +0.0   +0.0
[ 45, 60) |  224.8  235.6  252.5  24.2 |  224.8  235.6  252.5  24.2 |   +0.0   +0.0   +0.0
[ 60, 75) |  216.6  230.2  250.1   0.0 |  216.1  229.5  249.7   0.0 |   +0.5   +0.7   +0.4
[ 75, 90) |  179.2  204.4  186.7   0.0 |  159.5  176.5  169.5   0.0 |  +19.7  +27.9  +17.2
[ 90,105) |  187.0  214.6  163.6  11.6 |  114.1  136.5   98.4   0.0 |  +72.8  +78.0  +65.2
[105,120) |  237.1  249.7  246.1  54.8 |   53.8   74.8   75.1   0.0 | +183.3 +174.9 +171.0
[120,135) |  238.9  250.4  248.9  56.6 |   53.4   72.2   71.7   0.0 | +185.5 +178.2 +177.2
[135,150) |  238.4  250.4  251.1  51.1 |   53.8   72.1   71.6   0.0 | +184.6 +178.3 +179.4
[150,165) |  237.2  250.5  252.4  45.2 |   54.1   72.3   71.8   0.0 | +183.1 +178.2 +180.7
[165,180) |  239.4  251.0  252.3  50.8 |   54.7   72.8   72.2   0.0 | +184.8 +178.2 +180.0
```

### Interpretation

* **Sky / upper bands (rows 0-75): bit-identical** (`dR=dG=dB=0.0` through row
  60, then ≤ 0.7). Both modes produce the *same* bytes for the same scene
  radiance — exactly what identical tone maps + shared `sky_sample` predict.
  The ~12% near-white in the top half is the **intended bright sky**, present
  in *both* modes, not a bug.
* **Water (rows 90-180): diverges by ~+180 per channel.** Whitted water sits at
  `≈(53-55, 72-75, 71-75)` — matching the documented corrected deep-water
  target of `≈53,72,72` (`docs/render_notes.md`). The path tracer's water
  instead lands at `≈(237-239, 250, 246-252)`, i.e. **linear radiance ≈ 0.9-1.0**
  fed into the tone map, which correctly encodes it to near-white.
* Because the *same* tone map produces **identical** bytes in the sky, a global
  exposure/clipping difference is **excluded**. If exposure or clamping differed,
  the sky would have shifted too. The water-only, ~+180 jump is the signature of
  an **upstream radiance difference** in the water BRDF/attenuation path of the
  path tracer (`water_attenuate` / dielectric reflection-refraction), not a
  tone-map defect.

## 5. Conclusion

| Claim under Hypothesis C | Result |
|---|---|
| Tone maps differ (gamma/exposure/clamp) | **FALSE** — 0 mismatches over 2e6+ finite samples |
| An exposure constant exists somewhere | **FALSE** — `grep -rn -i exposure src/` → no matches |
| Difference is consistent with a pure tone-map/exposure issue | **FALSE** — sky is bit-identical; only water diverges (~+180) |

The near-white water is a **water-region radiance** problem in the path tracer
(the linear colour reaching the shared tone map is ≈0.9-1.0 instead of the
Whitted ≈0.05), **not** a tone-mapping / exposure mismatch. Hypothesis C is
**REFUTED**; the defect must be sought upstream in the path tracer's water
shading (dielectric/attenuation/Fresnel) code.

**No source files were modified.** Diagnostic artefacts written under `output/`:
`diagC_pt.bmp`, `diagC_whitted.bmp`, `diagC_analyze.py`, `diagC_bands.py`,
`diagC_tonemap_equiv.c` (and its binary). `output/scene.bmp` was untouched.
