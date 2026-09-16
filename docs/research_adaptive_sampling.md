# Research Note — Opt-in Adaptive Sampling for the Whitted Raytracer

**Scope:** READ-ONLY design/research note. No source file was modified. This document
specifies an **opt-in** adaptive sampler for the C11, zero-dependency Whitted raytracer
whose default (flag-absent) render must stay **byte-identical** to today for any thread
count and any `--samples N`.

**Target limitation (README):** *"No adaptive sampling / denoising — a fixed per-pixel
sample count is used everywhere. (Adaptive sampling / denoising is deferred as out of
scope.)"* — `README.md:644–645`.

**Sources inspected (local repo only):**
- `src/render.c` (full read; line numbers cited below)
- `src/render.h` (public `render_image` contract)
- `src/main.c` (CLI parsing, `Options`, render call)
- `src/scene_desc.h` (scene model — confirms **no** `samples`/adaptive scene key exists)
- `src/camera.h` (`camera_ray_dof`, `aperture`/`focus_distance`)
- `README.md` (§ threading / tile scheduling, § limitations)
- `docs/render_notes.md` (§ determinism, § tile threading, § throughput)
- `tests/test_render_threads.c` (existing determinism test pattern to extend)

---

## 1. How per-pixel supersampling works today

### 1.1 The sample loop and averaging — `src/render.c`

`render_region()` (`src/render.c:390`) is the single per-pixel workhorse. For each pixel
`(x, y)` it runs a fixed loop `for (int s = 0; s < spp; ++s)` (`src/render.c:401`) and
averages into a linear accumulator:

```c
Vec3 acc = vec3(0.0, 0.0, 0.0);
for (int s = 0; s < spp; ++s) {
    double du, dv;
    sample_offset(spp, s, (unsigned)x, (unsigned)y, &du, &dv);   /* render.c:402 */
    double uu = ((double)x + du) / (double)width;
    double vv = 1.0 - ((double)y + dv) / (double)height;
    double lr1 = render_rand01((unsigned)x, (unsigned)y, (unsigned)(s*2+2)); /* lens */
    double lr2 = render_rand01((unsigned)x, (unsigned)y, (unsigned)(s*2+3));
    Ray ray = camera_ray_dof(cam, uu, vv, lr1, lr2);
    unsigned seed_key = render_hash3((unsigned)x, (unsigned)y, (unsigned)s); /* render.c:423 */
    acc = vec3_add(acc, trace(scene, ray, 0, max_depth, time, seed_key));
}
acc = vec3_scale(acc, 1.0 / (double)spp);                       /* render.c:428 */
```

Averaging is a plain arithmetic mean of the `spp` linear-radiance samples; the result is
then gamma-encoded and quantized once per pixel by `to_byte()` (`src/render.c:359`):
`clamp(c,0,1) → pow(c, 1/2.2) * 255 → round → byte`. There is **no** per-sample gamma,
and the mean is taken **before** gamma (correct, linear-space averaging).

### 1.2 Per-sample deterministic jitter / RNG derivation

All randomness is a pure integer hash PRNG, no `rand()`/`time()`:

- `render_hash_u32(x)` (`src/render.c:53`) — murmur/wang-style 32-bit finalizer.
- `render_hash3(a,b,c)` (`src/render.c:70`) — mixes three 32-bit ints.
- `render_rand01(a,b,c)` (`src/render.c:80`) — uniform double in `[0,1)` from the hash.

Per-sample channels are keyed on **(pixel x, pixel y, sample index s)** only:

| Quantity | Derivation | Location |
|---|---|---|
| Pixel AA jitter `(du,dv)` | `render_rand01(x, y, s*2+0)` / `(s*2+1)` | `src/render.c:348–354` |
| Thin-lens DOF sample `(lr1,lr2)` | `render_rand01(x, y, s*2+2)` / `(s*2+3)` | `src/render.c:413–415` |
| Sun-disk shadow jitter seed | `seed_key = render_hash3(x, y, s)` | `src/render.c:423` |

`sample_offset()` (`src/render.c:341`) picks **stratified jittered** samples over a `g×g`
grid when `spp` is a perfect square (`g = round(sqrt(spp))`, `g*g == spp`), otherwise
**uniform jitter**. With `spp = 16` (default, `src/main.c:46`) this is the documented
4×4 stratified grid (`docs/render_notes.md:18`).

Crucially, every channel is a **pure function of `(x, y, s)`** — never of thread, tile,
or ordering. This is the invariant the adaptive scheme must preserve.

### 1.3 Tile threading (schedule independence) — `src/render.c`

Under `-DUSE_PTHREADS` the image is split into **16×16 px tiles**
(`RENDER_TILE_SIZE 16`, `src/render.c:479`). A pool of workers claims tiles from a shared
`atomic_int next_tile` via `atomic_fetch_add` (`render_worker`, `src/render.c:500–530`).
Each worker calls `render_region` on its own disjoint rectangle, so no output-buffer race
exists. Worker count = `min(sysconf(_SC_NPROCESSORS_ONLN), 64, ntiles)`
(`choose_thread_count`, `src/render.c:533`), overridable by `RAYTRACER_THREADS`
(`README.md:165–173`). Because pixel colour never depends on the schedule, output is
byte-identical across thread counts (verified in `docs/render_notes.md:86–89` and
`tests/test_render_threads.c`).

`render_image()` (`src/render.c:577`) is the public entry; without `USE_PTHREADS` it calls
`render_serial()` (`src/render.c:457`).

### 1.4 CLI parsing and scene key

- `Options.samples` is an `int`, default `DEFAULT_SAMPLES = 16` (`src/main.c:46,50`).
- `--samples N` is parsed at `src/main.c:249–255` (validated `>= 1`); it is listed in the
  generic value-taking option set at `src/main.c:181`.
- `render_image(&scene, &cam, opt.width, opt.height, opt.samples, opt.depth, rgb)` at
  `src/main.c:471–472`.
- **There is no `samples` scene key.** `grep` over `src/scene_desc.{h,c}` finds no
  `samples`/`spp` field. The scene model exposes `camera`, `sky`, materials, prims and
  plants only (`src/scene_desc.h:230–233`). So sample count is **CLI-only** today.

---

## 2. Adaptive metric and refinement strategy

### 2.1 Recommended metric — per-pixel relative standard error of luminance

Use the **relative standard error of the mean (RSEM)** of per-sample **luminance**. It is
per-pixel local (no neighbourhood dependency), cheap (running sum + sum-of-squares), and
directly estimates the residual noise that matters after averaging.

For pixel `p`, after `n` samples with per-sample linear radiance `L_i = (R_i,G_i,B_i)`,
define luminance with Rec.709 weights:

```
Y_i   = 0.2126*R_i + 0.7152*G_i + 0.0722*B_i
Ybar  = (1/n) * Σ Y_i
s²    = (1/(n-1)) * Σ (Y_i - Ybar)²          (unbiased sample variance, n >= 2)
SE    = sqrt(s² / n)                          (standard error of the mean)
e     = SE / (Ybar + eps)                     (relative error; eps = 1e-4)
```

Refine pixel `p` while `e > tau` and `n < N_max`.

- `tau` (tolerance) default `0.02` (2% relative noise). Exposed as `--adaptive-tau` (opt).
- `eps = 1e-4` guards division by ~black.
- `N_max = adaptive_max_factor * n0` with `adaptive_max_factor = 4` → e.g. 16 spp base
  refines up to 64 spp; exposed as `--adaptive-max N` (opt).

**Why luminance RSEM rather than raw variance:** a fixed variance threshold spends samples
on bright, already-clean regions; dividing by `Ybar` makes the criterion perceptually
proportional and matches the README's "noisy/high-contrast pixels" intent. **Why not
contrast-only:** contrast between neighbours needs a full first-pass buffer and is
ambiguous on thin features (a 1-px edge has a high gradient on both sides even where each
side is already converged). RSEM is self-contained per pixel.

**Optional secondary contrast term (between passes):** after pass 0, compute the local
luminance contrast `C(p) = max over 4-neighbours q of |Ybar(p) − Ybar(q)|`. Flag a pixel
if `e > tau` **OR** `C(p) > tau_c` (e.g. `tau_c = 0.05`). This is a deterministic function
of the completed pass-0 buffer, evaluated after a global barrier, so it stays
schedule-independent. It is a refinement, not required for the minimal recipe.

### 2.2 Refinement strategy — progressive passes with a cap (two-tier)

A **progressive, fixed-batch** scheme (not per-pixel recursive splitting) keeps tile
parallelism intact:

1. **Pass 0 (coarse):** render every pixel with `n0 = base spp` (the value of
   `--samples`, default 16). Accumulate `sum_Y` and `sum_Y2` per pixel (float/double
   buffers, allocated only in adaptive mode).
2. **Barrier:** all tiles finish; compute `e(p)` for every pixel. The "active set" is now a
   deterministic function of the whole pass-0 image.
3. **Pass k ≥ 1:** render a **batch** of `B` additional samples (e.g. `B = n0`) for every
   pixel still flagged, **continuing the sample index from `n`** (see §3). Update stats;
   recompute `e`. Stop when no pixel is flagged or `n == N_max`, or a max pass count is
   reached.
4. **Resolve:** average `sum_Y / n` (per channel: keep three `sum` accumulators, or three
   `sum` + three `sumsq` for per-channel variance) → gamma → `to_byte`.

Using whole-image passes means the per-pixel "continue?" decision depends only on that
pixel's own statistics, so no cross-pixel dependency is introduced and tile scheduling
remains irrelevant. Batching (`B = n0`) bounds the number of barriers; a per-pixel
`while` loop inside `render_region` (splitting until convergence) would also be
deterministic but makes the cost profile uneven and complicates progress reporting —
prefer the batched variant.

**Memory:** per-pixel `sum[3]` + `sumsq` luminance (or `sumsq[3]`), i.e. 4–6 `double`s
per pixel. For 1280×720 that is ~35–44 MB if `double`; use `float` accumulators to halve
it (~22 MB) at negligible precision cost for these magnitudes, or store only luminance
`sum_Y`/`sum_Y2` plus three channel `sum`s. Allocated only when adaptive is ON.

---

## 3. Determinism strategy (schedule independence)

The existing guarantee rests on: *pixel colour is a pure function of `(x, y, sample
index)`*. The adaptive sampler preserves it exactly by three rules:

1. **Sample indices are global and monotonic per pixel.** A pixel's `k`-th sample always
   uses index `s = k`, regardless of which pass or thread produced it. `sample_offset`,
   the DOF jitter, and `seed_key` all key on `(x, y, s)` (`src/render.c:348–354, 413–415,
   423`), so sample `k` is bit-identical whether produced in pass 0 or pass 3.
2. **Refinement decisions use only the pixel's own accumulated statistics.** `e(p)`
   depends on `(sum_Y, sum_Y2, n)` for that pixel, which depend only on the samples
   already taken — never on another pixel or on thread ordering. (If the optional
   neighbour-contrast term is used, it reads a *completed, immutable* pass-0 buffer after
   a global barrier.)
3. **Global barriers between passes.** Pass boundaries are synchronization points, so the
   active set for pass `k+1` is a deterministic function of pass `k`'s finished image.
   Within a pass, tiles are disjoint → no races; across passes, barriers order them.

Consequently the adaptive result is byte-identical for any `RAYTRACER_THREADS` value and
any tile schedule, and reproducible run-to-run. This is the same argument already
documented in `README.md:563–567` and `docs/render_notes.md:430–438`; the adaptive scheme
is a strict extension of it.

**Float non-associativity caveat:** the accumulator order *within a pixel* is
`for s in 0..n-1` (sequential per pixel), so the summation order is fixed and independent
of threading. Do **not** parallelize the per-pixel inner loop or reduce across threads,
or the summation order could change and break bit-identity.

---

## 4. Opt-in gating (default OFF ⇒ byte-identical)

### 4.1 Guarantee mechanism

Keep the **fixed path untouched**. Introduce adaptive sampling as a *separate* code path
selected by a runtime flag, and make the existing entry point delegate with the flag
cleared:

- Keep `render_image(const Scene*, const Camera*, int w, int h, int spp, int max_depth,
  unsigned char *rgb_out)` **exactly as-is** — it calls the new engine with adaptive OFF
  (`src/render.h` unchanged for existing callers/tests such as
  `tests/test_render_threads.c`).
- Add a params-carrying entry point, e.g.

  ```c
  typedef struct {
      int    samples_per_pixel;   /* base n0, >= 1            */
      int    max_depth;           /* >= 0                     */
      int    adaptive;            /* 0 = fixed (default)      */
      int    adaptive_max_spp;    /* cap N_max (>= n0)        */
      double adaptive_tau;        /* relative-error tolerance */
      int    denoise;             /* 0 = off (default)        */
  } RenderParams;

  int render_image_ex(const Scene *scene, const Camera *cam, int width, int height,
                      const RenderParams *params, unsigned char *rgb_out);
  ```

  `render_image(...)` becomes a thin wrapper: `RenderParams p = {spp, max_depth, 0, spp,
  0.0, 0}; return render_image_ex(..., &p, ...);`.

- `render_region()` is **not modified** in the fixed path. The adaptive pass uses a new
  function (e.g. `render_region_adaptive`) that reuses the *same* `sample_offset` /
  `render_rand01` / `seed_key` code and the *same* per-sample trace call, differing only
  in the accumulation buffers and the loop bound.

With `adaptive == 0`, `render_image_ex` executes the current code path verbatim: same
loop, same sample count, same order, same `to_byte` — hence byte-identical output for any
thread count and any `--samples`.

### 4.2 CLI flag — `src/main.c`

- Add `int adaptive;` (default 0) and `int adaptive_max; double adaptive_tau;` to
  `Options` (`src/main.c:49–57`).
- Register `--adaptive` in the **no-value** branch next to `--threads`
  (`src/main.c:165–172`), and `--adaptive-max`/`--adaptive-tau` in the value branch
  (`src/main.c:181`), with the same `parse_long`/`strtod` validation style.
- Add usage lines near `src/main.c:67–75`.
- Call `render_image_ex(...)` at `src/main.c:471` with the assembled `RenderParams`;
  when `opt.adaptive == 0` the params reproduce today's behaviour exactly.
- Optionally print `adaptive: on (n0=16, max=64, tau=0.02)` in the summary
  (`src/main.c:506–509`) — **only** when adaptive is on, so the default summary is
  unchanged.

### 4.3 Optional scene key (deferred / needs parser work)

There is **no** existing `samples` scene key, so a scene-level switch is a *new* key and
requires parser + model + writer changes (`src/scene_desc.{h,c}` and
`src/scene_desc_write.c` for round-trip fidelity). Recommended shape if added:

```
render {
    adaptive = 1
    adaptive_max = 64
    adaptive_tau = 0.02
}
```

Precedence: CLI flag overrides scene key; absent in both ⇒ OFF. **Minimal recipe: CLI
only.** Treat the scene key as an optional follow-up, not part of the core deliverable,
because the CLI flag alone satisfies the requirement and keeps the change bounded.

---

## 5. Denoising consideration

**Recommendation: adaptive sampling alone is sufficient for the documented limitation;
denoising should remain a separate, optional, default-OFF post-pass.**

Rationale, bounded to zero-dependency options:

- The metric in §2 directly targets residual *variance* (RSEM). Where the criterion is
  met, the mean already has relative error `<= tau`; remaining error is below threshold,
  not visible banding. No filter is needed for the stated goal ("spend more samples on
  noisy pixels").
- A denoiser adds risk to the byte-identity contract and to determinism (filters that
  depend on traversal order or use RNG are non-deterministic). If offered, it must be:
  - **deterministic**: a fixed stencil, no RNG, no data-dependent iteration order;
  - **linear-space**: applied to the float radiance buffer *before* gamma/`to_byte`;
  - **edge-aware and bounded**: a 5×5 **cross-bilateral** filter with a luminance-similarity
    weight `w = exp(-|Y_p - Y_q| / sigma_l) * exp(-r²/(2 sigma_s²))`, or a 3×3
    luminance-guided median. Fixed neighbourhood ⇒ schedule-independent.
- Keep `--denoise` a distinct flag (default OFF). Never enable it implicitly when
  `--adaptive` is set, or `--adaptive` alone would stop being a pure sampling change and
  byte-identity reasoning would get muddier.

Net: implement adaptive sampling first; treat denoising as an independent, optional
follow-up.

---

## 6. Concrete change list, implementation outline, and test strategy

### 6.1 Files / functions to change

| File | Change | Why |
|---|---|---|
| `src/render.h` | Add `RenderParams` struct + `render_image_ex()` declaration. Leave `render_image()` signature unchanged. | New opt-in entry point without breaking existing callers/tests. |
| `src/render.c` | Add `render_region_adaptive()` (+ helpers `rsem_step()`); add global-barrier pass driver in `render_image_ex()`; keep `render_region()`, `sample_offset()`, `to_byte()`, hashing untouched. | Adaptive passes reuse existing deterministic sampling; fixed path byte-identical. |
| `src/main.c` | Extend `Options`; parse `--adaptive`, `--adaptive-max`, `--adaptive-tau`; call `render_image_ex`; conditional summary line. | Opt-in gating, default OFF. |
| `src/scene_desc.h` / `src/scene_desc.c` (+ `scene_desc_write.c`) | **Optional / deferred:** add a `render { }` block with `adaptive` keys. | Only if a scene-level switch is wanted; not required. |
| `tests/test_adaptive.c` (new) | New deterministic test (below). | Lock the contract. |
| `README.md` | Update the limitation bullet (`README.md:644–645`) and the CLI table (`README.md:98`) to document `--adaptive`. | Docs only; no code effect. |

Functions **not** to touch: `render_hash_u32/hash3/rand01` (`src/render.c:53–84`),
`sample_offset` (`:341`), `to_byte` (`:359`), `trace`/`trace_hit` (`:111–332`),
`camera_ray_dof` (`src/camera.c`).

### 6.2 Bounded implementation outline

```
render_image_ex(scene, cam, w, h, params, rgb):
    validate(params->samples_per_pixel >= 1, max_depth >= 0)
    if !params->adaptive:
        return <existing fixed path>            # byte-identical to today

    n0   = params->samples_per_pixel
    Nmax = max(params->adaptive_max_spp, n0)
    tau  = params->adaptive_tau (> 0)
    # per-pixel accumulators (float/double), allocated once
    sumR,sumG,sumB,sumY,sumY2 : w*h   # zeroed
    n : w*h (implicitly n0 for pass 0)

    # pass 0: every pixel, samples s = 0 .. n0-1
    render_pass(active=all, sample_range=[0,n0))

    loop:
        compute e[p] = RSEM(sumY[p], sumY2[p], n[p])   # pure per-pixel
        active = { p : e[p] > tau and n[p] < Nmax }
        if active empty: break
        for p in active: batch B = min(n0, Nmax - n[p])
        render_pass(active, sample_range=[n[p], n[p]+B))   # s continues monotonically
        for p in active: n[p] += B

    # resolve: linear mean per channel -> gamma -> to_byte
    for each pixel p:
        rgb[..] = (to_byte(sumR/n), to_byte(sumG/n), to_byte(sumB/n))
```

`render_pass(active, range)` uses the **same** `sample_offset`, DOF jitter channels and
`seed_key = render_hash3(x,y,s)` as the fixed path, with the tile/atomic work-queue from
`render_worker` (`src/render.c:500`). Between passes, join all workers (global barrier).
For non-`USE_PTHREADS`, the same pass driver runs serially via `render_serial`-style
loop.

### 6.3 Deterministic test strategy (`tests/test_adaptive.c`)

Reuse the harness style of `tests/test_render_threads.c` (own `main()`, `CHECK`, links
against all objects except `main.o`, small image e.g. 64×36, depth 4, `RAYTRACER_NO_PROGRESS=1`).

1. **Default byte-identity (the hard contract).** For a fixed `(w, h, spp, depth)`, assert
   `render_image(...)` output == `render_image_ex(..., adaptive=0)` output, byte-for-byte.
   This proves the wrapper and the new engine agree on the fixed path.
2. **Determinism.** Call `render_image_ex(..., adaptive=1)` twice with identical inputs →
   byte-identical buffers (mirrors `test_render_threads.c:count_diff`).
3. **Thread-schedule independence.** Under `USE_PTHREADS`, render adaptive with
   `RAYTRACER_THREADS=1` and with `RAYTRACER_THREADS=4` (or unset) → byte-identical
   (`setenv`/`unsetenv` as in `tests/test_render_threads.c`).
4. **Adaptivity actually engages (high-contrast scene).** Build a scene with a hard
   silhouette / high-frequency feature. Assert the adaptive image **differs** from the
   fixed-`n0` image (non-zero diff count) and that the adaptive image has **lower
   measured noise**: e.g. render a high-spp reference, then compare
   `mean |adaptive − reference| < mean |fixed − reference|` over the frame, or assert the
   flagged-pixel count is > 0 and concentrated near the silhouette.
5. **Cap respected.** Assert no pixel exceeds `Nmax` samples — expose a diagnostic counter
   (e.g. `render_last_total_samples()` or a per-pixel max in a debug build), or verify via
   a small wrapper that total samples lie in `[n0*Npx, Nmax*Npx]`.
6. **Flat-region economy (optional).** On a constant-colour scene (uniform background),
   assert the adaptive run flags ~0 pixels and takes ~`n0` samples/pixel (cost ≈ fixed).

Also extend `make test` (Makefile auto-globs `tests/*.c`, `Makefile:26–28`) — no Makefile
change needed.

### 6.4 Risks / pitfalls

- **Bit-identity leakage:** any change to `render_region`'s fixed loop, to `to_byte`, or
  to the hash functions can perturb the default image. Keep them untouched; route the
  fixed path through the *existing* code.
- **Accumulation order:** never parallelize the per-pixel sample loop; keep `for s` serial
  per pixel for bit-reproducibility.
- **Numerical variance via sum/sumsq:** for large `n` and bright pixels, `ΣY²` can lose
  precision; Welford's online algorithm is the safe alternative if artifacts appear.
  Given bounded radiance here, `double` sum/sumsq is acceptable; document the choice.
- **Progress reporting:** the threaded path prints nothing (`src/render.c:646`); the
  adaptive multi-pass driver should likewise emit at most a stderr-only summary to avoid
  polluting stdout (used for BMP/PPM in `main.c`).
- **DOF channel collisions:** keep the DOF lens samples on channels `s*2+2 / s*2+3` so
  they never collide with the AA jitter (`s*2+0 / s*2+1`) — the existing comment
  (`src/render.c:405–411`) already documents this invariant; preserve it in the adaptive
  loop.

---

## 7. Summary

- **Metric:** per-pixel **relative standard error of the mean luminance**
  `e = sqrt(s²/n) / (Ybar + 1e-4)`, Rec.709 luminance; refine while `e > tau` (default
  0.02) up to `N_max = 4·n0`, using progressive fixed-batch passes with global barriers.
- **Gating:** new `--adaptive` CLI flag (default **OFF**), plus optional `--adaptive-max`
  and `--adaptive-tau`; a scene `render { }` key is an optional deferred extension.
  Fixed path unchanged ⇒ byte-identical default for any thread count and `--samples`.
- **Determinism:** sample index `s` is global/monotonic per pixel; every jitter/seed
  derives from `(x, y, s)` via the existing hash PRNG; per-pixel decisions use only that
  pixel's own stats; passes separated by global barriers ⇒ schedule-independent,
  reproducible.
- **Denoising:** not required; keep any deterministic cross-bilateral filter as a separate
  default-OFF `--denoise` pass applied in linear space before gamma.
- **Primary edits:** `src/render.{h,c}` (new `render_image_ex` + adaptive pass driver,
  fixed path untouched), `src/main.c` (flag parsing + call), optional
  `src/scene_desc.{h,c}`, new `tests/test_adaptive.c`, README docs.

MISSION COMPLETE (t-036)
