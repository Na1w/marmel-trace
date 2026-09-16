# Research Note — Procedural Tree Generation (for a from-scratch C raytracer)

**Task:** `t-003` — Research procedural tree generation.
**Consumer:** `src/scene.c` (procedural scene assembly), using primitives from `src/geometry.h`.
**Scope (deliberately tight):** only procedural tree/plant geometry — recursive branching, L-system
branching, branch shape approximation (tapered cylinder / cone / capsule), and leaf/canopy
approximation with concrete parameter ranges. **Not covered:** water, sky, clouds, shading models,
BMP/file formats.

All coordinates and lengths below are in **world units**. Assume a scene where 1 world unit ≈ 1 metre
and the ground plane is `y = 0` (matches the layout in `.marmel/execution_plan.md`).

---

## 0. Design goals and constraints

A raytracer that is being written from scratch in C has three hard constraints that dominate every
choice in this note:

1. **Intersection cost.** Every branch segment added to the scene is a primitive the renderer must
   intersect per ray, per reflection bounce, per pixel, per supersample. The branching recursion
   grows exponentially, so the *primitive count* must be bounded deliberately.
2. **Primitive simplicity.** Reusing one already-implemented primitive (sphere, or a single generic
   cylinder/cone) is far cheaper in engineering time than adding bespoke quad / mesh support.
3. **Determinism.** A tree must be reproducible from a seed so renders are repeatable and diffable.

The recommendation at the end of this note picks the strategy that minimises (1) and (2) while still
looking plausible.

---

## 1. Recursive branching algorithm

### 1.1 Concept

The classic "grow a tree by recursion" model: start from a **trunk segment** with a position,
direction, length and radius. At the end of each segment, spawn `N` child branches. Each child is
rotated away from the parent direction by a **spread angle**, has its length multiplied by a
**length decay**, and its radius multiplied by a **radius decay**. Recurse until a depth limit is
reached; at the limit, place a leaf cluster.

This is the model used by Weber & Penn (1995) "Creation and Rendering of Realistic Trees" and by
countless ray tracers before and since. We only need the pragmatic core, not their full
parametric/statistical machinery.

### 1.2 Per-branch state

```c
/* One branch segment: base -> tip, with a base radius that tapers to a tip radius. */
typedef struct {
    Vec3  base;      /* start point (attachment to parent)      */
    Vec3  dir;       /* unit direction from base to tip         */
    float length;    /* segment length in world units           */
    float r_base;    /* radius at base                          */
    float r_tip;     /* radius at tip (r_tip < r_base)          */
    int   depth;     /* recursion depth (0 = trunk)             */
} Branch;

typedef struct {
    float init_len;      /* trunk length                     */
    float init_radius;   /* trunk base radius                */
    float len_decay;     /* child_len   = parent_len   * k_len   (0<k<1) */
    float rad_decay;     /* child_radius= parent_radius* k_rad   (0<k<1) */
    float taper;         /* r_tip = r_base * taper (0<taper<=1)          */
    int   children;      /* branches spawned per node                    */
    float spread;        /* branching angle in radians (parent -> child) */
    float perturb;       /* max random angular jitter in radians         */
    int   max_depth;     /* 3..5 recommended                             */
    uint32_t seed;       /* per-tree seed for the RNG                    */
} TreeParams;
```

### 1.3 Direction perturbation (the core geometry)

To grow a child branch you need to rotate the parent direction `dir` by `spread` radians around some
axis, then add random jitter. The clean way:

1. Build an orthonormal frame around the parent direction:
   ```
   w = normalize(dir)
   u = normalize(cross( (fabs(w.y) < 0.99 ? (0,1,0) : (1,0,0)), w ))
   v = cross(w, u)                       /* u, v span the plane perpendicular to w */
   ```
2. For child `i` of `n`, distribute azimuth evenly and add jitter:
   ```
   phi   = 2*pi*i/n + rand_uniform(-perturb, +perturb)
   child = normalize( w*cos(spread) + (u*cos(phi) + v*sin(phi))*sin(spread) )
   ```
   Here `spread` is the polar angle away from the parent axis and `phi` is the roll/azimuth.
   Adding `perturb` to `phi` (and optionally a small `±perturb` on `spread`) breaks the otherwise
   suspiciously perfect radial symmetry.

3. Optional **upward bias** so trees grow toward the sky (light) rather than drooping:
   ```
   child = normalize( child + (0, up_bias, 0) )   /* up_bias ~ 0.1..0.3 */
   ```
   A "gravity"/droop variant simply subtracts, or biases toward `-y` for weeping trees.

### 1.4 Recursive function (C-friendly pseudocode)

```c
/* Deterministic per-branch RNG: hash (seed, depth, index) -> float in [0,1).
   Avoids shared mutable RNG state, so traversal order cannot change the tree. */
static float rnd(uint32_t seed, int depth, int idx) {
    uint32_t h = seed * 2654435761u;      /* Knuth multiplicative hash */
    h ^= (uint32_t)depth * 0x9E3779B9u;
    h ^= (uint32_t)idx   * 0x85EBCA6Bu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return (float)(h & 0xFFFFFF) / (float)0x1000000;   /* [0,1) */
}
static float rnd_range(uint32_t seed, int d, int i, float lo, float hi) {
    return lo + (hi - lo) * rnd(seed, d, i);
}

/* Emits branch primitives into the scene and, at the tips, leaf primitives. */
void grow_tree(Scene *sc, Branch parent, const TreeParams *P, int idx) {
    /* 1. Materialise this segment as a tapered cylinder (see Section 3). */
    add_tapered_cylinder(sc, parent.base, parent.dir, parent.length,
                         parent.r_base, parent.r_tip);

    Vec3  tip = v3_add(parent.base, v3_scale(parent.dir, parent.length));
    float child_len = parent.length * P->len_decay;
    float child_rad = parent.r_base * P->rad_decay;

    /* 2. Terminal condition: depth limit OR branch too thin to matter. */
    if (parent.depth >= P->max_depth || child_rad < P->min_radius) {
        place_leaf_cluster(sc, tip, child_len, parent.depth, P, idx);
        return;
    }

    /* 3. Build the orthonormal frame around the parent direction. */
    Vec3 w = v3_normalize(parent.dir);
    Vec3 ref = (fabsf(w.y) < 0.99f) ? v3(0,1,0) : v3(1,0,0);
    Vec3 u = v3_normalize(v3_cross(ref, w));
    Vec3 v = v3_cross(w, u);

    /* 4. Spawn children. */
    for (int i = 0; i < P->children; i++) {
        float phi = 2.0f*PI*(float)i/(float)P->children
                  + rnd_range(P->seed, parent.depth, i, -P->perturb, P->perturb);
        float spread = P->spread
                  + rnd_range(P->seed, parent.depth, i+97, -P->perturb, P->perturb);

        Vec3 child_dir = v3_normalize(v3_add(
            v3_scale(w, cosf(spread)),
            v3_scale(v3_add(v3_scale(u, cosf(phi)), v3_scale(v, sinf(phi))), sinf(spread))));

        /* optional upward bias */
        child_dir = v3_normalize(v3_add(child_dir, v3(0.0f, P->up_bias, 0.0f)));

        Branch child = {
            .base    = tip,
            .dir     = child_dir,
            .length  = child_len,
            .r_base  = child_rad,
            .r_tip   = child_rad * P->taper,
            .depth   = parent.depth + 1,
        };
        grow_tree(sc, child, P, idx * P->children + i);
    }
}

/* Entry point for one tree at ground position p. */
void make_tree(Scene *sc, Vec3 p, const TreeParams *P) {
    Branch trunk = {
        .base = p, .dir = v3(0,1,0),
        .length = P->init_len,
        .r_base = P->init_radius,
        .r_tip  = P->init_radius * P->taper,
        .depth  = 0,
    };
    grow_tree(sc, trunk, P, 0);
}
```

**Primitive count.** With `children = 2` and `max_depth = 5`, the number of segments is
`2^(depth+1) - 1 = 63`. With `children = 3, depth = 4`: `(3^5-1)/2 = 121`. Keep this in mind: a
single tree can easily dominate the scene's primitive budget, so prefer `children = 2..3` and
depth 3–5. A practical cap is to also stop when `r_base` falls below ~0.02 world units (branch too
thin to see), which prunes the deep end of the tree cheaply.

### 1.5 Parameter meanings, with formulas

| Symbol | Meaning | Typical value |
|---|---|---|
| `init_len` | trunk segment length | 2.0–4.0 |
| `init_radius` | trunk base radius | 0.10–0.25 |
| `len_decay` | length ratio child/parent | 0.65–0.80 |
| `rad_decay` | radius ratio child/parent | 0.55–0.72 |
| `taper` | tip/base radius within one segment | 0.85–0.95 |
| `children` | branches per node | 2–3 |
| `spread` | polar branch angle | 25°–40° |
| `perturb` | random angular jitter | 5°–15° |
| `up_bias` | upward pull | 0.05–0.30 |
| `max_depth` | recursion depth | 3–5 |

Useful derived relations:

- **Total tree height** ≈ `init_len * (1 - len_decay^(max_depth+1)) / (1 - len_decay)`
  (geometric series of segment lengths along the trunk direction, before spread shortens the
  projected height).
- **Radius continuity.** To make child base radius match the parent tip radius, set
  `rad_decay = taper`; otherwise a small step appears at joints. Matching them gives the smoothest
  silhouette.
- **Volume-ish conservation.** A common trick is `rad_decay = children^(-1/2.2)` so that the
  cross-sectional area of children roughly equals that of the parent — produces naturally thinning
  branches. For `children = 2`, that is `rad_decay ≈ 0.73`.

### 1.6 Determinism

`rnd(seed, depth, idx)` above is a pure function of `(seed, depth, branch-index)`, so the tree is
identical regardless of traversal order or threading. `idx` is propagated as
`idx * children + i`, giving a unique integer path for every node. Vary `seed` to get a different
tree; keep it fixed to reproduce one.

---

## 2. L-system basics (alternative to hand-rolled recursion)

### 2.1 Definition

An L-system (Lindenmayer system) is a parallel rewriting grammar:

- **Alphabet:** symbols, e.g. `F`, `X`, `+`, `-`, `[`, `]`.
- **Axiom:** the initial string, e.g. `X`.
- **Production rules:** each symbol expands to a string, e.g. `X → F[+X][-X]FX`, `F → FF`.
- **Interpretation:** a *turtle* walks the resulting string and emits geometry.

Unlike the recursive function in Section 1, L-systems are **parallel** — in each iteration *every*
symbol is replaced simultaneously. The string length grows exponentially with iteration count, which
is exactly the branching explosion (and the same cost concern).

### 2.2 The canonical plant example

From the Wikipedia L-system article (verified source, see §9):

```
axiom : X
rules : (X → F+[[X]-X]-F[-FX]+X), (F → FF)
```

and the classic binary-tree form:

```
axiom : 0
rules : (1 → 11), (0 → 1[0]0)
```

### 2.3 Turtle interpretation (2D; trivially lifted to 3D)

The turtle keeps a position, a heading, and a **stack** for branching. Common symbols:

| Symbol | Turtle command |
|---|---|
| `F` | move forward by `step` **with the pen down** → emit a segment |
| `f` | move forward **without** drawing |
| `+` | turn left by angle `δ` (rotate heading about the up axis) |
| `-` | turn right by angle `δ` |
| `[` | push current turtle state onto the stack |
| `]` | pop state from the stack (return to the branch point) |
| `X` | no-op placeholder (controls branching only) |

In **3D** the turtle carries a full orientation (three orthonormal vectors: heading `H`, left `L`,
up `U`). `+`/`-` rotate `H` and `L` about `U`; additional symbols (`&`, `^`, `/`, `\`) pitch and
roll about the other axes. The turtle-graphics article confirms the standard 3D extension uses an
"up" vector to choose the rotation plane and a roll command for the third degree of freedom.

### 2.4 String → geometry (pseudocode)

```c
void interpret(const char *s, float step, float delta, float radius, Scene *sc) {
    Vec3  pos = origin, H = v3(0,1,0), U = v3(1,0,0);   /* 2D case: ignore U */
    Stack st = stack_new();
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case 'F': {
            Vec3 next = v3_add(pos, v3_scale(H, step));
            add_tapered_cylinder(sc, pos, H, step, radius, radius*0.9f);
            pos = next;
            break;
        }
        case '+': H = rot_axis(H, U, +delta); break;
        case '-': H = rot_axis(H, U, -delta); break;
        case '[': stack_push(&st, pos, H, radius); break;
        case ']': stack_pop (&st, &pos, &H, &radius); break;
        default: break;   /* X and other no-ops */
        }
    }
}
```

A very common practical tweak: **shrink the radius when pushing** (`radius *= 0.7f` inside `[`), so
the taper is produced by the L-system itself rather than by per-segment geometry.

### 2.5 Recursion vs L-system — which to use here?

| Aspect | Recursive function (§1) | L-system (§2) |
|---|---|---|
| Implementation effort | Low; direct C recursion | Medium; string rewrite + stack interpreter |
| Memory | O(depth) stack | O(string length), grows exponentially |
| Parameter tweaking | Direct numeric fields | Edit grammar strings |
| Randomness | Trivial via hashed RNG | Needs stochastic rules or symbol-level jitter |
| Leaf placement | Natural (at terminal call) | Needs a distinct leaf symbol `L` + interpreter case |
| Best fit | **This raytracer** | When you want hand-authored, artistic grammars |

**Recommendation:** use the **recursive function** (§1) for this project. It produces the same class
of forms with less machinery, integrates directly with `src/scene.c`, and makes seed-based variation
trivial. Mention the L-system in a code comment as the conceptual ancestor, but do not build a string
rewriter unless the goal is specifically artistic grammar authoring.

---

## 3. Branch geometry approximation

### 3.1 The primitives available

For a from-scratch raytracer the realistic candidates, in order of increasing fidelity and cost:

1. **Sphere** — already needed everywhere (sky, water droplets, leaf clusters). Ray-sphere is the
   simplest intersection and the one you implement first.
2. **Capsule** (a cylinder with hemispherical caps) — sphere tests plus a bounded-cylinder test.
3. **Tapered cylinder / truncated cone** — a single quadric-style intersection; the most accurate
   branch shape for the least extra code beyond a cylinder.
4. **Triangle mesh** — most flexible, most code (Möller–Trumbore + mesh management); overkill here.

### 3.2 Tapered cylinder (truncated cone) intersection

A branch segment is the region between a base point `P0` with radius `r0` and a tip point `P1` with
radius `r1`. Let:

```
A  = P1 - P0          h  = |A|          â = A / h
Δ0 = O - P0           (O = ray origin, D = ray direction, normalized)
Od  = D · â           Δ0d = Δ0 · â
D⊥  = D  - Od  · â    Δ0⊥ = Δ0 - Δ0d · â
```

The radius varies linearly along the axis: `r(s) = r0 + s*(r1 - r0)` where `s ∈ [0,1]` is the
normalised axial position, `s(t) = (Δ0d + t*Od) / h`. Substituting the ray `X(t) = O + t*D` into
`|X⊥|² = r(s)²` gives a quadratic `a t² + b t + c = 0`:

```
k    = (r1 - r0) / h
r_b  = r0 + k * Δ0d
r_a  = k * Od

a = |D⊥|²      - r_a²
b = 2 * ( Δ0⊥ · D⊥ - r_b * r_a )
c = |Δ0⊥|²     - r_b²
```

Then:

```
disc = b*b - 4*a*c
if (disc < 0) no hit
t0 = (-b - sqrt(disc)) / (2a)
t1 = (-b + sqrt(disc)) / (2a)
for each t in {t0, t1} (ascending, t > eps):
    s = (Δ0d + t*Od) / h
    if (0 <= s <= 1): accept  ->  hit point X = O + t*D
```

**Normal at the hit:** with `s` as above,
```
radial = X - (P0 + s*h*â)          /* points outward from the axis */
axis_component = â * ((r0 - r1)/h) /* tilt of the cone surface       */
n = normalize( radial + axis_component * ??? )   /* see note below  */
```
A robust and simpler formulation: the outward normal of a cone surface is
`n = normalize( radial_dir * h - â * (r1 - r0) * s_sign )` — but for a **thin, nearly cylindrical
branch** (`|r1-r0| << h`) you can approximate `n ≈ normalize(radial)`, i.e. treat it as a cylinder.
This approximation is visually indistinguishable on branches and saves the cone-normal derivation.
For a **pure cylinder** (`r0 == r1`), `k = 0`, so `r_a = 0` and the quadratic collapses to the
familiar ray-cylinder test.

**Caps.** The truncated cone has two flat disc caps. For a tree, you can usually **omit the caps**:
adjacent segments overlap slightly at joints and the caps are hidden. If you see light leaking at
joints, either add disc-cap tests (ray-plane intersected with a radius check) or — simplest —
extend each segment a tiny bit (`length * 1.02`) so consecutive cones interpenetrate.

### 3.3 Capsule alternative (simplest of all)

A capsule is the set of points within distance `r` of a line segment `P0..P1`. Intersection =
ray-vs-bounded-cylinder **plus** two ray-vs-sphere caps; take the nearest valid `t`.

- Ray-sphere cap at `P0` with radius `r0`: standard quadratic
  `t² + 2 t (Δ0·D) + (Δ0·Δ0 - r0²) = 0` (with `|D|=1`), accept only if the hit projects *behind* the
  base plane.
- The cylinder body uses the same quadratic as §3.2 with `r0 = r1 = r`.

Capsules are attractive because they reuse the sphere code you already have, and they have **no
orientation ambiguity at joints** (rounded ends blend nicely). The cost is that a capsule is
fatter at the joints than a tapered cone, so deep branches look slightly "beaded" unless the radii
shrink fast.

### 3.4 Practical trade-off table

| Approach | Code cost | Intersection cost | Look | Notes |
|---|---|---|---|---|
| Sphere-only branches (chain of spheres) | Lowest (reuse ray-sphere) | 1 quadric per sphere | Beaded/lumpy | Only acceptable for very small twigs |
| **Capsule** | Low (sphere + bounded cylinder) | ~3 tests per segment | Good, rounded joints | Best simplicity/quality ratio |
| **Tapered cylinder (truncated cone)** | Medium (one quadratic + clip) | 1 quadric per segment | **Best**, natural taper | Recommended if you want the classic look |
| Axis-aligned box / cone hack | Low | Cheap | Poor, faceted | Avoid; orientation breaks it |
| Triangle mesh | High | BVH needed | Best | Out of scope |

**Axis-aligned vs general primitive.** An axis-aligned cylinder/cone is only correct when the branch
happens to point along a coordinate axis. Branches point in arbitrary directions, so you *must*
either (a) implement the **general oriented** quadratic above, or (b) transform the ray into the
branch's local frame, intersect an axis-aligned primitive, then transform `t` back. Option (b) is
mathematically equivalent and sometimes easier to reuse, but it costs a per-primitive basis and two
matrix-vector ops per ray. For a small scene, (a) is straightforward and faster. **Do not** try to
approximate a slanted branch with an axis-aligned cone — it will visibly fail.

**Recommendation:** implement the **general tapered cylinder** (§3.2) once, with the
`n ≈ normalize(radial)` normal approximation. It is a single quadratic and gives proper taper,
which is the single biggest visual cue that "these are branches, not pipes."

---

## 4. Leaf / canopy approximation

Branches alone read as a dead tree. Leaves are what make it look alive. Three strategies, from
cheapest to richest.

### 4.1 (a) Leaf quads / billboards at branch tips

At each terminal branch, place a small quad (two triangles) or a single "billboard" polygon
oriented to face the camera.

- **Pros:** cheap geometry; can carry a leaf texture/alpha; looks good with enough of them.
- **Cons:** requires **ray-triangle** intersection (Möller–Trumbore) plus alpha/texture sampling;
  billboards that face the camera are *view-dependent*, which is awkward in a raytracer (the
  billboard orientation changes per ray, and reflected rays see a different orientation). Needs a
  leaf texture or procedural leaf shape to avoid looking like flat paper.
- **Verdict:** good for a mature renderer, but it pulls in triangle + texture infrastructure this
  project does not otherwise need.

### 4.2 (b) Small spheres (leaf clusters) at branch tips

At each terminal branch, place one or a few small **spheres** representing a clump of leaves.

- **Pros:** **reuses ray-sphere**, which is already implemented for the rest of the scene; no new
  intersection code; no textures; naturally 3D and view-independent; reflection/refraction "just
  works"; can be made slightly translucent or given a noisy normal for a foliage feel.
- **Cons:** at low counts they look like berries; need a moderate number per tip and a green
  material to read as foliage. Cost is `#tips × #spheres_per_tip` primitives.
- **Verdict:** **the pragmatic sweet spot** for a from-scratch C raytracer.

### 4.3 (c) Single translucent canopy sphere with noise-based alpha

Wrap the whole crown in one large sphere whose material is a **translucent green** with
density/opacity modulated by 3D noise (value/Perlin noise, already needed for clouds).

- **Pros:** extremely cheap — **one** primitive per tree. Gives a soft, volumetric crown silhouette.
- **Cons:** you must implement **translucency / participating-media style attenuation** (or at least
  a noise-thresholded opacity so some rays pass through); the noise must be sampled in 3D and mapped
  to alpha; hard to make it not look like a "green fog ball"; shadows and secondary rays through it
  are fiddly.
- **Verdict:** elegant in principle, but requires shading machinery (translucency, alpha) beyond
  plain opaque surfaces. Good as an *enhancement*, not as the first implementation.

### 4.4 Comparison

| Strategy | New code needed | Primitive cost | Quality | View-dependence |
|---|---|---|---|---|
| (a) Leaf quads/billboards | Triangle intersection + texture/alpha | Medium–high | High (with texture) | Yes (billboards) |
| **(b) Leaf spheres at tips** | **None (reuse ray-sphere)** | Medium | Good | No |
| (c) Translucent canopy sphere | Translucency + 3D noise alpha | Very low (1) | Medium | No |

### 4.5 Recommended leaf approach

**Use (b): small spheres at branch tips.** Specifically:

- At each terminal branch, place **1 primary cluster sphere** of radius
  `r_cluster ≈ 0.6 × remaining_branch_length`, plus optionally 2–4 smaller offset spheres
  (radius `0.4 × r_cluster`) jittered around the tip by a hashed RNG for a less spherical look.
- Give clusters a green material; if you want variation, slightly randomise the green per cluster
  (hue/lightness jitter) using the same hashed RNG.
- For a "fluffy" edge, allow the cluster spheres to **overlap the last branch segment** so foliage
  hides the twig ends.

This reuses the ray-sphere code, needs zero new primitives, is deterministic, and reads clearly as
foliage at the render resolutions a project like this targets. If time allows later, strategy (c) can
be layered *on top* as a cheap soft-halo, but it should not be the first thing built.

---

## 5. Concrete parameter ranges (world units)

Assuming the camera/ground scale from the project layout (1 unit ≈ 1 m), a plausible deciduous tree:

| Parameter | Range | Suggested default | Notes |
|---|---|---|---|
| Trunk height (`init_len`) | 2.0 – 4.0 | 3.0 | first segment only |
| Trunk base radius (`init_radius`) | 0.10 – 0.25 | 0.15 | trunk thickness |
| Length decay (`len_decay`) | 0.65 – 0.80 | 0.72 | child/parent length |
| Radius decay (`rad_decay`) | 0.55 – 0.72 | 0.65 | child/parent radius |
| Segment taper (`taper`) | 0.85 – 0.95 | 0.90 | tip/base within a segment |
| Children per node | 2 – 3 | 2 | 2 = sparse, 3 = bushy |
| Spread angle (`spread`) | 25° – 40° | 32° | polar branch angle |
| Jitter (`perturb`) | 5° – 15° | 10° | random per-child angular noise |
| Up bias (`up_bias`) | 0.05 – 0.30 | 0.15 | upward growth pull |
| Recursion depth (`max_depth`) | **3 – 5** | 4 | 5+ explodes primitive count |
| Min radius cutoff | 0.02 – 0.04 | 0.03 | prune twigs below this |
| Leaf cluster radius | 0.15 – 0.45 | 0.25 | per tip |
| Leaf spheres per tip | 1 – 5 | 3 | 1 + 2 jittered |
| Total tree height | 4.0 – 7.0 | ~5.5 | derived from the series above |
| Total tree width | 2.5 – 5.0 | ~3.5 | roughly `2 × height × sin(spread)` |

**Sanity checks.**

- Height grows with `max_depth` and `len_decay`; if a depth-5 tree is under ~4 units, raise
  `len_decay` toward 0.80.
- If branches look too sparse, raise `children` to 3 *or* `max_depth` to 5, but watch primitive
  count: `children=3, depth=5` is `(3^6-1)/2 = 364` segments per tree — likely too many for many
  trees in one scene.
- If the crown is a thin umbrella, increase `spread` (wider) or `up_bias` (taller/narrower).
- If it looks like a bush, lower `init_len` relative to `init_radius`.

### 5.1 Suggested presets

```c
/* Tall, sparse, oak-like */
TreeParams oak = { .init_len=3.5f, .init_radius=0.20f, .len_decay=0.75f,
                   .rad_decay=0.68f, .taper=0.92f, .children=2, .spread=RAD(30),
                   .perturb=RAD(12), .up_bias=0.20f, .max_depth=5, .min_radius=0.03f };

/* Short, wide, bushy */
TreeParams bush = { .init_len=1.8f, .init_radius=0.14f, .len_decay=0.70f,
                    .rad_decay=0.62f, .taper=0.88f, .children=3, .spread=RAD(38),
                    .perturb=RAD(15), .up_bias=0.08f, .max_depth=4, .min_radius=0.03f };

/* Slim conifer-ish (small spread, strong up bias) */
TreeParams conifer = { .init_len=3.0f, .init_radius=0.16f, .len_decay=0.78f,
                       .rad_decay=0.70f, .taper=0.94f, .children=3, .spread=RAD(22),
                       .perturb=RAD(6), .up_bias=0.35f, .max_depth=5, .min_radius=0.03f };
```

### 5.2 Seed-based variety

Vary each tree by passing a different `seed` into `TreeParams`, then:

```c
void scatter_trees(Scene *sc, int count, uint32_t base_seed) {
    for (int i = 0; i < count; i++) {
        uint32_t s = base_seed + 1013904223u * (uint32_t)i;   /* decorrelated seeds */
        TreeParams P = oak;            /* start from a preset */
        P.seed       = s;
        P.init_len  *= rnd_range(s, 0, 0, 0.85f, 1.15f);      /* ±15% size        */
        P.init_rad  *= rnd_range(s, 0, 1, 0.90f, 1.10f);
        P.spread     = RAD(rnd_range(s, 0, 2, 26.0f, 38.0f)); /* angle variety    */
        P.up_bias    = rnd_range(s, 0, 3, 0.05f, 0.30f);
        /* position: jittered grid or Poisson-ish scatter on the ground plane */
        Vec3 p = v3(/* x */, 0.0f, /* z */);
        make_tree(sc, p, &P);
    }
}
```

Because the branch RNG is a pure hash of `(seed, depth, idx)`, changing only `seed` gives a
completely different but fully reproducible tree. Perturbing the *top-level* parameters per tree
(size, spread, bias) gives a forest that does not look cloned, while keeping each tree internally
coherent.

---

## 6. Recommended simplest approach

**For the first working version, do this — nothing more:**

1. **Recursive branching function** exactly as in §1.4:
   `children = 2`, `max_depth = 4`, `len_decay = 0.72`, `rad_decay = 0.65`, `taper = 0.90`,
   `spread = 32°`, `perturb = 10°`, `up_bias = 0.15`, trunk `len = 3.0`, `radius = 0.15`.
   Stop early when `r_base < 0.03`.
2. **Branches as tapered cylinders** (truncated cones) using the single quadratic in §3.2, with the
   `n ≈ normalize(radial)` normal approximation. Skip caps; overlap segments by ~2% to hide joints.
3. **Leaves as small spheres** at terminal branches: one sphere of radius `0.25` plus two jittered
   spheres of radius `0.10`, green material.
4. **Variety via seed**: hash-based RNG `rnd(seed, depth, idx)`; perturb size/spread/bias per tree.

This yields plausible trees with **zero new intersection primitives beyond the sphere you already
have and one general tapered-cylinder quadratic**, is fully deterministic, and scales to a small
forest. The L-system (§2), leaf quads (§4.1) and translucent canopy (§4.3) are documented
alternatives to revisit only if the simple version proves insufficient.

**Expected primitive budget** for one depth-4, 2-child tree: 31 branch segments + ~16 tips × 3 leaf
spheres ≈ **79 primitives**. A forest of 8 trees ≈ **630 primitives** — a reasonable scene load for
a from-scratch raytracer without acceleration structures, and manageable even with naive O(n)
intersection per ray at modest resolution.

---

## 7. Integration notes for `src/scene.c` / `src/geometry.h`

- Add a `TAPERED_CYLINDER` (or `CONE`) primitive kind to `src/geometry.h` alongside sphere/plane/box.
  Store: base point, axis direction (unit), length, `r_base`, `r_tip`, material index.
- Reuse the existing `Vec3` ops (`dot`, `cross`, `normalize`, `add`, `sub`, `scale`) — the pseudocode
  above assumes exactly this API.
- The leaf cluster spheres are ordinary spheres, so no new primitive kind is needed for leaves.
- Keep the RNG in `src/noise.h` or a small `rng.h`; do **not** use `rand()` (non-portable seeding and
  shared state break determinism).
- Because `grow_tree` appends primitives to a dynamic array in `Scene`, reserve capacity
  (e.g. `scene_reserve(sc, 200 * num_trees)`) to avoid repeated reallocation.

---

## 8. Summary of key formulas

- Child direction: `child = normalize( w*cos(spread) + (u*cos(phi) + v*sin(phi))*sin(spread) )`,
  with `u,v` an orthonormal basis of the plane perpendicular to the parent direction `w`.
- Azimuth distribution: `phi_i = 2π i / n + jitter`.
- Length series: `L_k = init_len · len_decay^k`; total height `= init_len · (1 - len_decay^(D+1)) / (1 - len_decay)`.
- Radius series: `R_k = init_radius · rad_decay^k`; area-matching suggests `rad_decay ≈ children^(-1/2.2)`.
- Tapered-cylinder ray test: `a t² + b t + c = 0` with
  `a = |D⊥|² - r_a²`, `b = 2(Δ0⊥·D⊥ - r_b r_a)`, `c = |Δ0⊥|² - r_b²`,
  `k = (r1-r0)/h`, `r_b = r0 + k·Δ0d`, `r_a = k·Od`; accept if `s = (Δ0d + t·Od)/h ∈ [0,1]`.
- Capsule: bounded-cylinder quadratic plus two sphere-cap quadratics; nearest valid `t`.

---

## 9. Sources

- L-system — Wikipedia (axioms, production rules, the plant example
  `X → F+[[X]-X]-F[-FX]+X`, `F → FF`, and `0 → 1[0]0`; turtle interpretation).
  https://en.wikipedia.org/wiki/L-system  (retrieved for this note)
- Turtle graphics — Wikipedia (turtle = location + orientation + pen; 3D extension uses an "up"
  vector and roll; forward/turn commands).
  https://en.wikipedia.org/wiki/Turtle_graphics
- Algorithmic botany — Wikipedia (L-systems as the standard plant-modeling formalism;
  Prusinkiewicz & Lindenmayer, *The Algorithmic Beauty of Plants*).
  https://en.wikipedia.org/wiki/Algorithmic_botany
- Weber, J. & Penn, J. (1995), "Creation and Rendering of Realistic Trees", SIGGRAPH '95 —
  the canonical source for the recursive/parametric branching model used in §1
  (length/radius decay, branching angle, per-level randomness).
  https://dl.acm.org/doi/10.1145/218380.218427
- Ray–cone / ray–cylinder quadratic derivation (tapered-cylinder test in §3.2), standard form as
  used in raytracing references (e.g. Scratchapixel "Ray-Cone Intersection", Ray Tracing Gems).
  https://www.scratchapixel.com/lessons/3d-basic-rendering/minimal-ray-tracer-rendering-simple-shapes/ray-cones.html
