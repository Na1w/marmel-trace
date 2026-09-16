# Validation Report - Phase 8 (Independent Audit)

Auditor: independent quality auditor (read-only on src/, tests/, Makefile).
Workspace: /Users/fredrikandersson/Experiments/marmel-0.8.0/raytracer
Host: macOS arm64, Apple clang 17.0.0, 10 online CPUs.
All scratch programs were written under /tmp and compiled directly
against the workspace sources; no repo test was reused for checks C-F.

## Overall verdict: PASS WITH NOTES

Every mandated contract held under independent reproduction. The two
NOT-VERIFIED items are (a) the authors' "2.01x over the pre-change
threaded build" figure, which cannot be reproduced without rebuilding the
old code (out of scope), and (b) ThreadSanitizer, whose runtime is
unusable on this host. Neither affects a change under audit.

## Check A - Clean builds, zero warnings

Commands (fresh, from CWD):

    make clean && make
    make clean && make threads

Observed:

- single: exit 0; `grep -ci warning` = 0; `grep -ci error` = 0
- threads: exit 0; `grep -ci warning` = 0; `grep -ci error` = 0

Result: PASS (0 warnings, 0 errors, both targets).
Logs: /tmp/b_single.log, /tmp/b_thr.log.

## Check B - Full test suite

Commands:

    make clean && make test
    make clean && make test \
      CFLAGS="-std=c11 -O2 -Wall -Wextra -DUSE_PTHREADS -pthread" \
      LDFLAGS="-pthread"

(The `make threads` target only builds the `raytracer` binary, not the
test binaries, so the threaded test run used the CFLAGS/LDFLAGS override
above, exactly as instructed.)

Observed - single-threaded build (exit 0, "ALL TESTS PASSED"):

| suite                  | passed | failed |
|------------------------|--------|--------|
| test_bmp.c             | 118    | 0      |
| tests/test_bvh_planes  | 46     | 0      |
| summary (integration)  | 30     | 0      |
| tests/test_math        | 190    | 0      |
| test_noise.c           | 61     | 0      |
| tests/test_render_threads | 4   | 0      |
| tests/test_water       | 34     | 0      |

Observed - threaded build (exit 0, "ALL TESTS PASSED"):

| suite                  | passed | failed |
|------------------------|--------|--------|
| test_bmp.c             | 118    | 0      |
| tests/test_bvh_planes  | 46     | 0      |
| summary (integration)  | 30     | 0      |
| tests/test_math        | 190    | 0      |
| test_noise.c           | 61     | 0      |
| tests/test_render_threads | 8   | 0      |
| tests/test_water       | 34     | 0      |

`test_render_threads` (threaded) printed:
`threaded build; RAYTRACER_THREADS=1 vs dynamic: 0 differing bytes`.

Result: PASS. Logs: /tmp/test_fresh.log, /tmp/test_thr2.log (0 warnings).

## Check C - BVH vs geometry_intersect equivalence

Own throwaway program: /tmp/vbvh.c (plus /tmp/vex.c, /tmp/vex2.c,
/tmp/vex3.c, /tmp/vex4.c for the extreme-coordinate probe). Compiled:

    cc -std=c11 -O2 -Wall -Wextra -Isrc /tmp/vbvh.c \
       src/vec3.c src/geometry.c src/bvh.c -lm -o /tmp/vbvh

Mixed scene: 200 primitives (6 planes + spheres + boxes + cylinders +
triangles), 8000 random rays, |coord| <= 60.
Comparison: t within 1e-9 relative; prim_index/material_index/front_face
exact; hit/miss agreement.

Observed output:

    [mixed] prims=200 nodes=127 depth=11
    [mixed] rays=8000 ghit=7420 bhit=7420 mis=0
    [all-plane] prims=5 nodes=0
    [all-plane] rays=4000 mismatches(total)=0
    [no-plane] prims=150 nodes=101
    [no-plane] rays=4000 mismatches(total)=0
    [single-plane] prims=1 nodes=0
    [single-plane] rays=2000 mismatches(total)=0
    [near-plane] go=1 gp=1 gt=1.000000 | bo=1 bp=1 bt=1.000000
    [tie-plane] go=1 gp=2 gt=5.000000000000
    [tie-plane] bo=1 bp=2 bt=5.000000000000 TIE-OK
    [tie-sphere] go=1 gp=0 | bo=1 bp=0 TIE-OK
    [extreme] bounds min=(1e+16,-1,-1) max=(1e+16,1,1) degenerate=0
    [extreme] go=1 gt=4.000000 | bo=1
    TOTAL rays=18000 ghit=14135 bhit=14135 mis=0
    t=0 p=0 m=0 f=0
    EXIT=0

Total: 18000 rays, 14135 hits, 14135 hits, 0 mismatches.
All of: plane-nearer-than-tree, all-plane (nodes=0), no-plane, single
plane, and both exact-tie cases matched. PASS.

Extreme-coordinate probe (pre-existing primitive_bounds limitation):

    /tmp/vex2.c: C=1e16, sphere at (1e16,5,0)
      bounds min=(1e+16,4,-1) max=(1e+16,6,1)   (X collapses, Y/Z fine)
      ray origin x=1e+16 (C+0.5 rounded to C), go=1 gt=4 bo=1
    /tmp/vex3.c: sphere (1e16,0,0) r=3, 20000 random rays
      go=1035 bo=1035 mismatches=0
    /tmp/vex4.c: C=1e17 / 1e18, X bounds collapse to a single value,
      geometry_intersect and bvh_intersect still agree (go=1 gt=4 bo=1)

Root cause is in src/geometry.c primitive_bounds (sphere/plane AABB
arithmetic at |coord| >= ~2^52 loses the X extent: min.x == max.x).
This lives entirely in geometry.c and is therefore PRE-EXISTING and NOT
introduced by the BVH change. The BVH inherits the same degenerate AABB,
but because a degenerate slab is still tested consistently and the ray
that hits also has its X collapsed, the two agree (0 mismatches over
20000 rays). No discrepancy attributable to the BVH was found.

## Check D - Fresnel / TIR correctness (independent)

Own program /tmp/vfres.c, replicating render.c's formula (f0 =
((1-ior)/(1+ior))^2; eta = 1/ior entering, ior exiting; sin2_t =
eta^2(1-cos^2); if >1 F=1 else F=fresnel_schlick(sqrt(1-sin2_t),f0)),
linking the real src/material.c fresnel_schlick and src/noise.c:

    cc -std=c11 -O2 -Wall -Wextra -Isrc /tmp/vfres.c \
       src/vec3.c src/material.c src/noise.c -lm -o /tmp/vfres

Observed: ior=1.33 theta_c=48.7535 deg.

EXIT (inside -> air), theta_i / cos_i / F:

    0.000   1.00000  0.02005931
    5.000   0.99619  0.02005931
   10.000   0.98481  0.02005933
   15.000   0.96593  0.02006015
   20.000   0.93969  0.02007470
   25.000   0.90631  0.02021081
   30.000   0.86603  0.02107825
   35.000   0.81915  0.02546351
   40.000   0.76604  0.04534741
   45.000   0.70711  0.14284886
   50.000   0.64279  1.00000000
   ...
   85.000   0.08716  1.00000000
   F(48)=0.44980787  F(49)=1.00000000

ENTER (air -> inside): F rises smoothly from 0.02006 (0 deg)
to 0.02298 (75 deg). Physical, no TIR (n1 < n2).
No discontinuity at theta_c: F(48)=0.45, F(48.7)=0.82,
F(thc-eps) approaches 1 continuously.

## Check E - water_attenuate correctness (independent)

Own program /tmp/vwat.c + /tmp/vmono.c against the real src/material.c:

    cc -std=c11 -O2 -Wall -Wextra -Isrc /tmp/vwat.c \
       src/vec3.c src/material.c src/noise.c -lm -o /tmp/vwat
    cc ... /tmp/vmono.c ... -o /tmp/vmono

Observed (/tmp/vwat, absorption=(0.4,0.5,0.6),
deep_color=(0.05,0.2,0.35), inner=(0.9,0.8,0.7)):

    depth0 dmax_vs_inner=0.000e+00
    depth1e9 dmax_vs_deep=0.000e+00
    monotone formula max_err=0.000e+00
    zero-absorb dmax_vs_inner=0.000e+00
    neg dmax=0.000e+00 nan dmax=0.000e+00
    extreme finite=1,1,1
    deep-absorb finite=1,1,1 val=0.000e+00
    zero-absorb depth0 exact=1
    m depth0 exact=1

Observed (/tmp/vmono):

    monotone_toward_deep=1
    limit_eq_deep=1

depth 0 returns EXACTLY inner (0.0 error, bit-identical).
depth 1e9 returns EXACTLY deep_color (0.0 error, better than 1e-12).
Convergence monotone; zero absorption is identity; negative and NaN
depth both clamp to identity (dmax 0.0); extreme finite absorption and
inner stay finite. PASS.

## Check F - water_normal unit length + sub-cm response

Own program /tmp/vnorm.c over a 41x41x4 grid of (x,z,time):

    cc -std=c11 -O2 -Wall -Wextra -Isrc /tmp/vnorm.c \
       src/vec3.c src/material.c src/noise.c -lm -o /tmp/vnorm

Observed:

    grid=6724 nonfinite=0 max|n|-1=3.331e-16
    dx=0.02: resp>1e-5=6724/6724 max|dn|=0.00874334 min|dn|=3.511e-05

Max |n|-1 deviation = 3.33e-16, far below 1e-9; all finite; every one of
6724 grid points changed by > 1e-5 when x shifted by 0.02 m, min 3.5e-5,
max 8.7e-3. PASS. (The 0.02 m step is confirmed in source:
src/material.c line ~283, `const double e = 0.02;`, replacing 0.5/freq
~1.43 m.)

## Check G - Threading: byte-identity and determinism

Both binaries built fresh; /tmp/rt_single (from `make`) and
/tmp/rt_threads (from `make threads`). Render command:

    /tmp/rt_single --width 320 --height 180 --samples 8 \
       --depth 5 --seed 1337 --out /tmp/img_single.bmp
    /tmp/rt_threads --threads --width 320 --height 180 --samples 8 \
       --depth 5 --seed 1337 --out /tmp/img_thr.bmp

Observed: single 6.64 s, threaded 0.89 s; both wrote 172854 bytes.
    cmp /tmp/img_single.bmp /tmp/img_thr.bmp -> CMP_IDENTICAL
    sha256 both = b2066d887c230cabb8f8b3575baa6bd71c221b7d0d9eb166dc6eb6692e8b2892

RAYTRACER_THREADS override (1,2,8,64), same settings:

    n=1  6.69 s  n=2  3.38 s  n=8  0.92 s  n=64 0.87 s
    all four sha256 = b2066d88...92  (byte-identical)

Determinism across repeated runs (160x90, 4 spp, depth 4): 3 runs,
distinct SHA-256 count = 1.

Default thread count: sysconf(_SC_NPROCESSORS_ONLN) = 10 (confirmed via
sysctl -n hw.ncpu = 10 and getconf _NPROCESSORS_ONLN = 10).

Tile scheduling inspection (src/render.c lines 355-528):
- RENDER_MAX_THREADS 64, RENDER_TILE_SIZE 16 (16x16 tiles).
- choose_thread_count(): env override (1..64 else 1) else
  sysconf(_SC_NPROCESSORS_ONLN), fallback 4, clamp to [1,64], then
  min(nthreads, ntiles).
- render_worker(): claims tiles via atomic_fetch_add(&next_tile,1);
  computes x0..x1,y0..y1 clipped to image; calls render_region.
- render_region() writes only p[0..2] for (x,y) inside [x0,x1)x[y0,y1).
- Only shared mutable state = atomic_int next_tile (plus read-only
  scene/cam/params/rgb_out). No two workers share a pixel.

Race conclusion: each worker writes a disjoint tile region; the single
atomic counter is the only synchronisation point. No data race is
apparent by inspection. ThreadSanitizer could NOT be used: a
-fsanitize=thread binary segfaults at startup on this host (rc=139) -
dynamic race check is therefore NOT-VERIFIED; byte-identity across
thread counts is the empirical evidence instead.

## Check H - Speed-up sanity check

960x540, 8 spp, depth 5, seed 1337, wall-clock (shell `time`):

    /tmp/rt_single  -> 59.08 s (renderer-internal), 59.086 s total
    /tmp/rt_threads ->  7.90 s (renderer-internal),  7.906 s total
    measured speed-up = 59.086 / 7.906 = 7.47x

user/real on threaded = 68.68 s user / 7.906 s real = 8.69x on 10 CPUs
(~87% efficiency) - plausible for a 10-core host.

Full default render (1280x720, 16 spp, depth 6, seed 1337):

    /tmp/rt_threads -> 31.53 s (internal), 31.539 s total,
       user 275.36 s / real 31.539 s = 8.73x CPU utilisation

Assessment of authors' claims:
- "6.37x vs the old 448 s full render": the old 448.22 s figure is for
  1920x1080 @ 16 spp, NOT the 1280x720 default. I reproduced the NEW
  side of that workload: 1920x1080 @ 16 spp, depth 6 -> 70.62 s
  (internal) / 70.64 s total. That matches the authors' "70.36 s"
  (within ~0.4%). 448.22 / 70.62 = 6.35x, CONSISTENT with 6.37x.
  (The old 448 s baseline itself was not rebuilt - out of scope - but
  the new-code number reproduces to <1%.)
- "2.01x over the pre-change threaded build at 960x540": NOT-VERIFIED.
  The pre-change threaded binary was not available and rebuilding the
  old code is explicitly out of scope. My 960x540 measurements
  (single 59.09 s, threaded 7.91 s) are internally consistent but do
  not isolate the scheduler-only delta, so the 2.01x claim is
  neither confirmed nor refuted.

## Check I - Source-tree hygiene

Observed (ls -la, find, grep):

- Repo is NOT a git repository (no .git, no .gitignore). No stray
  *.orig/*.bak/*~/*.tmp files found.
- Build artifacts present: src/*.o (10), src/*.d (10), raytracer,
  output/scene.bmp. These are normal `make` outputs (there is no
  .gitignore because there is no git repo); noted but not anomalies.
- grep -rn '/tmp' src tests Makefile: only test-code references
  (/tmp/rt_integration.bmp, /tmp/rt_test_bmp_*.bmp, TMPDIR default).
  No /tmp path committed into src/ or the Makefile.
- .marmel/ contains only internal state: .session_frozen.json,
  .session_journal.json, .session_transcript.json, execution_plan.md,
  marmel.log. This audit wrote nothing into .marmel/.
- docs/ contains research notes plus this report. No anomaly.
Result: PASS (no stray artifacts; only expected build outputs).

## Summary table

| Check | Command (abridged) | Observed | Result |
|-------|--------------------|----------|--------|
| A single | `make clean && make` | rc 0, 0 warn, 0 err | PASS |
| A threads | `make clean && make threads` | rc 0, 0 warn, 0 err | PASS |
| B single | `make clean && make test` | rc 0, all suites 0 failed | PASS |
| B threads | `make test CFLAGS=...-DUSE_PTHREADS` | rc 0, 0 warn, 0 failed | PASS |
| C | /tmp/vbvh 18000 rays | 0 mismatches (t/p/m/f) | PASS |
| C extreme | /tmp/vex2..4 | agrees; limit pre-existing in geometry.c | PASS |
| D | /tmp/vfres ior 1.33 | F<1 below 48.75 deg, ==1 above | PASS |
| E | /tmp/vwat + /tmp/vmono | 0 err; identity/limit/monotone OK | PASS |
| F | /tmp/vnorm 6724 pts | max dev 3.33e-16; all respond | PASS |
| G | cmp + sha256, thread 1/2/8/64 | byte-identical | PASS |
| G determinism | 3 runs | 1 distinct SHA-256 | PASS |
| G races | inspect + TSan | disjoint tiles; TSan unusable | PASS (inspect) / NOT-VERIFIED (TSan) |
| H 960x540 | time single vs threaded | 59.09 s vs 7.91 s = 7.47x | PASS |
| H 6.37x | 1920x1080@16 -> 70.62 s | 448.22/70.62 = 6.35x | CONSISTENT |
| H 2.01x | old binary unavailable | - | NOT-VERIFIED |
| I | ls/find/grep | no stray artifacts | PASS |

## Claims CONFIRMED independently

1. BVH stack is a fixed C-stack array (BVH_STACK_FIXED=64) with a
   malloc/realloc fallback on pathological depth (src/bvh.c line 48,
   lines 462-617). Confirmed by reading source and by 18000-ray
   equivalence (no stack corruption / no overflow).
2. Interior traversal pushes the FARTHER child first, the NEARER child
   last (src/bvh.c ~585-600). Confirmed by source inspection.
3. Planes removed from the tree; held in b->planes[]/plane_count and
   tested in bvh_intersect (src/bvh.c 374-401, 483-500). Confirmed by
   source inspection and by all-plane geometry yielding node_count=0
   while still returning correct hits.
4. bvh_intersect is semantically identical to geometry_intersect for t
   (1e-9 rel), prim_index, material_index and front_face, including the
   lowest-prim_index tie-break, over 18000 rays (mixed / all-plane /
   no-plane / single-plane / both exact-tie cases): 0 mismatches.
5. Refraction path intersects the scene once (render.c lines 88-98,
   186-189); the normal and hit are reused rather than re-tracing the
   scene. Confirmed by source inspection.
6. Exiting Fresnel uses the refracted angle: sin2_t = eta^2(1-cos^2);
   if > 1 then TIR => F = 1.0, else cos_t = sqrt(1 - sin2_t). Confirmed
   by /tmp/vfres: F < 1 below theta_c = 48.7535 deg, F == 1.0 above, no
   discontinuity; entering case stays physical (no TIR).
7. water_attenuate implements inner_c*T_c + deep_color_c*(1-T_c) with
   T_c = exp(-absorption_c*depth): depth 0 => exactly inner, depth 1e9
   => exactly deep_color, monotone, zero-absorption identity, NaN/neg
   depth identity, all finite. Confirmed by /tmp/vwat + /tmp/vmono.
8. water_normal finite-difference step is 0.02 m (material.c ~line 283),
   down from ~1.43 m. Confirmed by source and /tmp/vnorm: |n|-1 max dev
   3.33e-16, all 6724 grid points respond > 1e-5 to a 0.02 m x shift.
9. Threading: dynamic count via
   sysconf(_SC_NPROCESSORS_ONLN)
   (cap 64, fallback 4) and 16x16 tile
   dynamic scheduling with atomic_int,
   replacing the 8-thread cap / static
   bands. Confirmed by source and by
   RAYTRACER_THREADS 1/2/8/64 all
   byte-identical.
10. New threaded binary renders
    1920x1080 @ 16 spp, depth 6 in
    70.62 s => 448.22/70.62 = 6.35x,
    matching the authors 6.37x.

## Residual uncertainties / NOT-VERIFIED

- "2.01x over the pre-change threaded
  build at 960x540": NOT-VERIFIED.
  The old binary was not available and
  rebuilding the pre-change code is out
  of scope. My 960x540 numbers (single
  59.09 s, threaded 7.91 s = 7.47x) are
  self-consistent but do not isolate the
  scheduler-only delta.
- ThreadSanitizer race check:
  NOT-VERIFIED. A -fsanitize=thread
  binary segfaults at startup on this
  host (rc=139), so no dynamic race
  detection was possible. Byte-identity
  across thread counts is the empirical
  substitute; no race is apparent by
  source inspection.
- The old 448.22 s baseline was not
  rebuilt (out of scope). Only the NEW
  side (70.62 s) was reproduced.
- primitive_bounds precision limitation
  at |coord| >= ~2^52: reasoned to be
  pre-existing in src/geometry.c (the
  degenerate AABB originates in
  primitive_bounds, not the BVH) but was
  not diffed against a pre-change build.

## Overall verdict: PASS WITH NOTES

Every mandated contract from the
review-driven changes held under
independent reproduction. Checks A-I
all pass except two NOT-VERIFIED items
that do not affect any change under
audit: (1) the authors 2.01x vs the
pre-change threaded build could not be
isolated without rebuilding the old
code, which is out of scope; and (2)
ThreadSanitizer is unusable on this
host (startup segfault, rc=139), so the
race check rests on source inspection
plus byte-identical output across
thread counts. The BVH/geometry
equivalence (0 mismatches over 18000
rays, including the lowest-prim_index
tie-break and all-plane / no-plane /
single-plane cases), the TIR Fresnel
behaviour, the water_attenuate formula,
the 0.02 m water_normal step, the
deterministic tiled threading, and the
measured 6.35x full-render speed-up
(consistent with the claimed 6.37x) were
all independently confirmed. No
discrepancy attributable to the audited
changes was found.
