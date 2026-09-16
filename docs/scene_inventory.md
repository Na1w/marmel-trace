# Scene Inventory — `src/scene.c` (marmel raytracer, C11)

Machine-checkable, exhaustive parameter inventory of the scene produced by
`scene_build()`. Everything below is read directly from the source; every value
cites `file:line`. Goal: the exact same scene must be reconstructable from this
document alone.

## 0. Verification summary

| Quantity | Value | Source |
|---|---|---|
| Total primitives | **1286** | confirmed by running `./raytracer --width 8 --height 8 --samples 1 --depth 1 --out /tmp/inv.bmp` → `Scene: 1286 primitives, 7 materials` |
| Total materials | **7** (`MAT_COUNT`) | `src/scene.c:80`, runtime print |
| Primitives by kind | 1 plane, 1 box, 121 cylinders, 1163 spheres | instrumented build (see §8) |
| Default seed | 1337 | `src/main.c:36` (`DEFAULT_SEED`) |
| `water_level` | 0.02 | `src/scene.c:40, 346` |
| `water_material` | 6 (`MAT_WATER`) | `src/scene.c:360` |

Cross-check of the counts (all confirmed against an instrumented build of the
same sources, compiled outside the workspace):

- cylinders: 36+19+17+15+10+7+10+7 = **121**
- spheres (leaf clusters): 341+184+166+138+94+65+103+72 = **1163**
- plane 1 + box 1 + 121 + 1163 = **1286** ✓
- material histogram: idx0=1, idx1=121, idx2=334, idx3=0, idx4=597, idx5=232, idx6=1 (sum 1286) ✓

> **Note on the default seed.** `main.c` passes `DEFAULT_SEED = 1337`
> (`src/main.c:36, 111`) to `scene_build()`. All numeric results below (seeds,
> variants, per-plant counts, total 1286) are for `seed = 1337`. A different
> seed changes the layout (see §8).

---

## 1. Scene-level state

`Scene` struct (`src/scene.h:29-39`). Fields set by `scene_build`
(`src/scene.c:334-405`):

| Field | Value | Source |
|---|---|---|
| `geo` | initialized via `geometry_init`, then filled (see §8) | `src/scene.c:341` |
| `bvh` | built over `geo` via `bvh_build(&s->geo)` | `src/scene.c:396-397` |
| `materials` | `calloc(MAT_COUNT, sizeof(Material))` filled by `scene_fill_materials` | `src/scene.c:353-358` |
| `material_count` | **7** (`MAT_COUNT`) | `src/scene.c:359` |
| `sky` | `sky_default_params(&s->sky)` then `s->sky.seed = seed` | `src/scene.c:349-350` |
| `water_level` | **0.02** (`SCENE_WATER_LEVEL`) | `src/scene.c:40, 346` |
| `water_material` | **6** (`MAT_WATER`) — the index of the water material | `src/scene.c:360` |

Init order detail: `water_material` is first set to `-1` (`src/scene.c:347`),
then overwritten to `MAT_WATER` (`= 6`) at `src/scene.c:360`. On `scene_free`
it is reset to `-1` and `water_level` to `0.0` (`src/scene.c:428-429`).

`water_material` is **not** read by `render.c`; the renderer detects water via
the per-material `is_water` flag instead (`src/render.c:112, 190, 200`).
`water_level` is only used to place the pond box at build time.

---

## 2. Material table

`MAT_COUNT = 7`. Enum order (`src/scene.c:72-81`), which **defines the index
order** — primitives reference materials by index, so order is significant:

```
MAT_GROUND = 0, MAT_BARK, MAT_LEAF0, MAT_LEAF1, MAT_LEAF2, MAT_LEAF3, MAT_WATER, MAT_COUNT
```

The table is zero-filled first (`memset`, `src/scene.c:134`), so any field not
explicitly assigned below is `0`. Values from `scene_fill_materials`
(`src/scene.c:124-168`); leaf albedos from the `leaf_albedo[4]` array
(`src/scene.c:125-130`).

| idx | name | albedo (r,g,b) | specular | shininess | reflectivity | transparency | ior | is_water | absorption | deep_color |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | ground | 0.30, 0.42, 0.16 | 0.05, 0.05, 0.05 | 8.0 | 0.0 | 0.0 | 1.0 | 0 | 0,0,0 | 0,0,0 |
| 1 | bark | 0.26, 0.18, 0.11 | 0.04, 0.04, 0.04 | 6.0 | 0.0 | 0.0 | 1.0 | 0 | 0,0,0 | 0,0,0 |
| 2 | leaf0 | 0.16, 0.38, 0.12 | 0.03, 0.04, 0.03 | 10.0 | 0.0 | 0.0 | 1.0 | 0 | 0,0,0 | 0,0,0 |
| 3 | leaf1 | 0.22, 0.45, 0.15 | 0.03, 0.04, 0.03 | 10.0 | 0.0 | 0.0 | 1.0 | 0 | 0,0,0 | 0,0,0 |
| 4 | leaf2 | 0.30, 0.50, 0.18 | 0.03, 0.04, 0.03 | 10.0 | 0.0 | 0.0 | 1.0 | 0 | 0,0,0 | 0,0,0 |
| 5 | leaf3 | 0.12, 0.30, 0.10 | 0.03, 0.04, 0.03 | 10.0 | 0.0 | 0.0 | 1.0 | 0 | 0,0,0 | 0,0,0 |
| 6 | water | 0.05, 0.15, 0.20 | 0.90, 0.90, 0.90 | 256.0 | 1.0 | 0.85 | 1.33 | 1 | 0.45, 0.12, 0.06 | 0.02, 0.10, 0.16 |

Per-line citations: ground `src/scene.c:137-141`; bark `src/scene.c:144-148`;
leaves `src/scene.c:151-157` (albedos `src/scene.c:126-129`); water
`src/scene.c:160-168`.

`Material` field order (`src/material.h:26-38`):
`albedo, specular, shininess, reflectivity, transparency, ior, is_water,
absorption, deep_color`. Only `MAT_WATER` sets `transparency`, `is_water`,
`absorption`, `deep_color`; all other materials leave them at 0.

---

## 3. The ground

- **Kind:** `PRIM_PLANE` (infinite plane), built by `prim_plane(point, normal, material_index)`.
- **Point:** `(0.0, 0.0, 0.0)`; **normal:** `(0.0, 1.0, 0.0)` (unit, +Y up).
- **Material index:** `MAT_GROUND` = **0**.
- **Added order:** **first** (`geometry_add` call at `src/scene.c:363-367`), so
  `prim_index = 0`.
- Plane AABB for the BVH is a large finite box (e.g. ±1e4, `src/geometry.h`
  `primitive_bounds` contract).

---

## 4. The water body

- **Kind:** `PRIM_BOX` (axis-aligned box), built by `prim_box(center, half, material_index)`.
- **Center:** `(SCENE_POND_CX, SCENE_WATER_LEVEL - SCENE_POND_HALF_Y, SCENE_POND_CZ)` = `(0.0, -0.01, -25.0)`.
- **Half extents:** `(SCENE_POND_HALF_X, SCENE_POND_HALF_Y, SCENE_POND_HALF_Z)` = `(45.0, 0.03, 45.0)`.
  → footprint x ∈ [-45, 45], z ∈ [-70, 20]; y ∈ [-0.04, 0.02]. Top face sits
  exactly at `water_level = 0.02`.
- **Material index:** `MAT_WATER` = **6**.
- **Added order:** **second** (`src/scene.c:369-377`), so `prim_index = 1`.

Constants: `SCENE_POND_CX/CZ/HALF_X/HALF_Y/HALF_Z` at `src/scene.c:43-47`;
`SCENE_WATER_LEVEL` at `src/scene.c:40`.

---

## 5. Procedural trees and bushes

### 5.1 Placement table

`TreeSpec { double x, z; double trunk_len; double trunk_radius; }`
(`src/scene.c:293-298`). Position is `(x, 0.0, z)`, initial direction
`(0, 1, 0)` (straight up). `trunk_radius` also selects the recursion depth
(see §5.3).

Trees — `scene_trees[]` (`src/scene.c:302-312`), added **in array order**:

| # | x | z | trunk_len | trunk_radius | intended depth |
|---|---|---|---|---|---|
| 0 | 50.0 | -80.0 | 3.4 | 0.20 | full depth-4 |
| 1 | 30.0 | -105.0 | 2.8 | 0.07 | depth-3 |
| 2 | -35.0 | -100.0 | 2.7 | 0.07 | depth-3 |
| 3 | 70.0 | -60.0 | 2.6 | 0.07 | depth-3 |
| 4 | 15.0 | -115.0 | 2.2 | 0.05 | depth-2 |
| 5 | -60.0 | -85.0 | 2.2 | 0.05 | depth-2 |

Bushes — `scene_bushes[]` (`src/scene.c:314-317`):

| # | x | z | trunk_len | trunk_radius |
|---|---|---|---|---|
| 0 | -48.0 | -55.0 | 0.8 | 0.05 |
| 1 | 55.0 | -100.0 | 0.7 | 0.05 |

### 5.2 Per-plant seed and leaf-material derivation

For tree `i` (`src/scene.c:383-390`):
```
tree_seed = scene_hash(seed, 0x7, i)
variant   = clamp((int)(scene_rand(tree_seed, 0, 0x51) * 4.0), 0, 3)
```
For bush `i` (`src/scene.c:393-400`):
```
bush_seed = scene_hash(seed, 0xB, i)
variant   = clamp((int)(scene_rand(bush_seed, 0, 0x51) * 4.0), 0, 3)
```
Leaf material index (`scene_add_plant`, `src/scene.c:322`):
`leaf_mat = MAT_LEAF0 + (variant % 4)` → one of {2,3,4,5}.

`scene_hash(seed, depth, idx)` (`src/scene.c:87-99`) is a pure 32-bit hash
(all arithmetic mod 2^32):
```
h  = seed * 2654435761
h ^= (depth + 1) * 0x9E3779B9
h ^= (idx   + 1) * 0x85EBCA6B
h ^= h >> 15
h *= 0x2C1B3C6D
h ^= h >> 12
h *= 0x297A2D39
h ^= h >> 15
```
`scene_rand(seed,depth,idx) = (scene_hash(...) & 0xFFFFFF) / 16777216.0` → [0,1)
(`src/scene.c:101-104`).
`scene_rand_range(seed,depth,idx,lo,hi) = lo + (hi-lo)*scene_rand(...)` → [lo,hi)
(`src/scene.c:107-110`).

**Resolved values for `seed = 1337`** (verified against the instrumented build):

| plant | pos (x,z) | gen seed | variant | leaf_mat |
|---|---|---|---|---|
| tree 0 | 50, -80 | 3128892807 | 2 | 4 (leaf2) |
| tree 1 | 30, -105 | 1448886631 | 2 | 4 (leaf2) |
| tree 2 | -35, -100 | 1959102491 | 0 | 2 (leaf0) |
| tree 3 | 70, -60 | 2945246587 | 3 | 5 (leaf3) |
| tree 4 | 15, -115 | 1816962403 | 3 | 5 (leaf3) |
| tree 5 | -60, -85 | 4003524602 | 0 | 2 (leaf0) |
| bush 0 | -48, -55 | 4181885975 | 0 | 2 (leaf0) |
| bush 1 | 55, -100 | 4072799993 | 2 | 4 (leaf2) |

### 5.3 Generator: `grow_tree` (`src/scene.c:226-289`)

Constants (`src/scene.c:50-64`):

| name | value | meaning |
|---|---|---|
| `SCENE_TREE_MAX_DEPTH` | 4 | max recursion depth |
| `SCENE_MIN_BRANCH_RADIUS` | 0.02 | terminal radius threshold |
| `SCENE_TAPER` | 0.7 | `r_top = 0.7 * r_bottom` |
| `SCENE_LEN_DECAY` | 0.72 | `child_len = parent_len * 0.72` |
| `SCENE_SPREAD` | 33° (0.57595865 rad) | nominal branch angle from parent axis |
| `SCENE_PERTURB` | 7° (0.12217305 rad) | ± jitter on angle and azimuth |
| `SCENE_UP_BIAS` | 0.12 | +Y pull added to each child direction |
| `SCENE_THIRD_CHILD_CHANCE` | 0.25 | probability of 3-way fork |
| `SCENE_LEAF_MIN` | 15 | min leaf spheres per tip |
| `SCENE_LEAF_SPAN` | 5 | extra spheres: count ∈ [15,19] |
| `SCENE_MAX_SEGMENTS` | 4096 | global safety cap on `geo.count` |

Algorithm (per call; `depth` starts at 0, `axis = normalize(dir)`, and if
`|dir|² < 1e-12` the axis is forced to `(0,1,0)`):

1. If `geo.count >= 4096`, stop (return 0).
2. `r_bottom = radius`, `r_top = radius * 0.7`, `top = base + axis*length`.
3. Append a **tapered cylinder**: `prim_cylinder(base, top, r_bottom, r_top, MAT_BARK=1)`.
4. **Terminal test:** if `depth >= 4` **or** `r_top < 0.02` → append a leaf
   cluster at `top` (see §5.4) and return.
5. Otherwise build an orthonormal frame around `axis`:
   `ref = (|axis.y| < 0.99) ? (0,1,0) : (1,0,0)`, `u = normalize(ref × axis)`,
   `v = axis × u`.
6. `children = 3` if `scene_rand(seed, depth, 0xABC1) < 0.25`, else `2`.
7. For each child `i` in `[0, children)`:
   ```
   phi    = 2π*i/children + rand_range(seed, depth, 100+i, -7°, +7°)
   spread = 33°            + rand_range(seed, depth, 300+i, -7°, +7°)
   len_jit= 1.0            + rand_range(seed, depth, 500+i, -0.12, +0.12)
   child_dir = normalize( axis*cos(spread)
                        + (u*cos(phi) + v*sin(phi)) * sin(spread) )
   child_dir = normalize( child_dir + (0, 0.12, 0) )      # upward bias
   child_seed = scene_hash(seed, depth, i + 1)
   recurse( base=top, dir=child_dir,
            length = length * 0.72 * len_jit, radius = r_top,
            depth = depth + 1, seed = child_seed )
   ```
   Radius is continuous across joints (children start at the parent's tip
   radius `r_top`). `bark_mat` is `MAT_BARK = 1` for every segment.

Recursion terminates by depth limit (4) or thinness (`r_top < 0.02`), which is
why the initial radius selects the effective depth.

### 5.4 Generator: `add_leaf_cluster` (`src/scene.c:180-209`)

Called at each terminal tip. `d = depth`:
```
count     = 15 + (int)(scene_rand(seed, d, 0xBEEF) * 5)      # 15..19
cluster_r = clamp(branch_len*1.1 + 0.7, 0.7, 2.2)
leaf_scale= clamp(cluster_r/1.6, 0.4, 1.0)
for i in 0..count-1:  k = i
    ox = rand_range(seed,d,100+k, -1,+1)
    oy = rand_range(seed,d,200+k, -1,+1)
    oz = rand_range(seed,d,300+k, -1,+1)
    off = (ox*cluster_r*1.15, oy*cluster_r*0.85, oz*cluster_r*1.15)
    r   = (0.5 + scene_rand(seed,d,400+k)*0.9) * leaf_scale     # r ∈ [0.5,1.4]*leaf_scale
    add prim_sphere(tip + off, r, leaf_mat)
```
`branch_len` is the terminal segment's length; the cluster size therefore
shrinks for shallower/smaller plants (bushes get smaller foliage).

### 5.5 Per-plant primitive counts (seed 1337, measured)

All cylinders carry material 1; all leaf spheres carry the plant's `leaf_mat`.

| plant | cylinders | leaf spheres | total added | internal nodes | 3-way forks | tips | prim_index range |
|---|---|---|---|---|---|---|---|
| tree 0 (50,-80) | 36 | 341 | **377** | 16 | 3 | 20 | 2–378 |
| tree 1 (30,-105) | 19 | 184 | **203** | 8 | 2 | 11 | 379–581 |
| tree 2 (-35,-100) | 17 | 166 | **183** | 7 | 2 | 10 | 582–764 |
| tree 3 (70,-60) | 15 | 138 | **153** | 7 | 0 | 8 | 765–917 |
| tree 4 (15,-115) | 10 | 94 | **104** | 4 | 1 | 6 | 918–1021 |
| tree 5 (-60,-85) | 7 | 65 | **72** | 3 | 0 | 4 | 1022–1093 |
| bush 0 (-48,-55) | 10 | 103 | **113** | 4 | 1 | 6 | 1094–1206 |
| bush 1 (55,-100) | 7 | 72 | **79** | 3 | 0 | 4 | 1207–1285 |
| **total** | **121** | **1163** | **1284** | | | | |

Plane (1) + box (1) + 1284 = **1286**. (Note: bush 0 and bush 1 have
`trunk_len` 0.8/0.7 but the same radius 0.05 as trees 4/5, so they reach
similar depths yet produce fewer cylinders because the shorter `branch_len`
shrinks the leaf clusters, not the branch count.)

---

## 6. Sky

`sky_default_params()` fills the struct (`src/material.c:51-74`); `scene_build`
then overrides the seed (`src/scene.c:349-350`). Field order per
`src/material.h:47-63`.

| field | value | source |
|---|---|---|
| `sun_dir` | `normalize(0.45, 0.75, -0.50)` = **(0.446663, 0.744438, -0.496292)** | `src/material.c:58` |
| `sun_color` | (1.00, 0.95, 0.85) | `src/material.c:60` |
| `horizon_color` | (0.75, 0.85, 1.00) | `src/material.c:61` |
| `zenith_color` | (0.35, 0.55, 0.95) | `src/material.c:62` |
| `gradient_gamma` | 0.6 | `src/material.c:63` |
| `sun_glow_exponent` | 350.0 | `src/material.c:64` |
| `sun_glow_strength` | 0.8 | `src/material.c:65` |
| `cloud_height` | 120.0 | `src/material.c:67` |
| `cloud_scale` | 0.0025 | `src/material.c:68` |
| `cloud_coverage` | 0.5 | `src/material.c:69` |
| `cloud_softness` | 0.12 | `src/material.c:70` |
| `cloud_sharpness` | 1.5 | `src/material.c:71` |
| `cloud_octaves` | 5 | `src/material.c:72` |
| `seed` | default 1337, overridden to the `scene_build` seed | `src/material.c:73`, `src/scene.c:350` |

There is **no explicit lacunarity/gain field** in `SkyParams`; the cloud fBm
call uses hardcoded `lacunarity = 2.0`, `gain = 0.5`
(`noise_fbm2(px*cloud_scale, pz*cloud_scale, cloud_octaves, 2.0, 0.5, seed)`,
`src/material.c:198-201`). Cloud layer is only evaluated for `dir.y > 0.03`
and faded by `smoothstep(0.0, 0.15, dir.y)` (`src/material.c:185-206`).

How the sky is consumed: `render.c` uses `scene->sky` for background rays
(`sky_sample`, `src/render.c:107, 199, 225`), for ambient
(`material_ambient`, `src/render.c:121`) and for the sun direction/colour of
the hard shadow ray (`L = scene->sky.sun_dir`, `src/render.c:122, 127`).

---

## 7. Camera

`scene_default_view()` (`src/scene.c:457-463`):

| output | value |
|---|---|
| eye | (-18.0, 6.0, 22.0) |
| target | (0.0, 5.0, -20.0) |
| up | (0.0, 1.0, 0.0) |
| vfov_deg | 40.0 |

`scene_default_camera()` (`src/scene.c:465-474`) ignores the scene
(`(void)s;`), calls `scene_default_view`, and builds
`camera_create(eye, target, up, 40.0, 16.0/9.0)` — i.e. **aspect = 16/9**
(hardcoded). `main.c` does **not** use this function; it calls
`scene_default_view` itself and rebuilds the camera with the **actual output
aspect** `width/height` (`src/main.c:306-307`). So the effective aspect is
whatever the `--width/--height` produce, defaulting to 16:9.

---

## 8. Ordering, counts, and the role of `seed`

### 8.1 Append order (determines `prim_index`)

`geometry_add` returns the sequential index; `prim_index` in a `Hit` is that
index (`src/geometry.h`, `Hit.prim_index`). The BVH resolves equal-`t` ties by
**lowest `prim_index`** (`src/bvh.c:482-543`), matching the linear scan in
`geometry_intersect`. Therefore this exact order matters:

1. `prim_index 0` — ground plane (`src/scene.c:363`)
2. `prim_index 1` — water box (`src/scene.c:371`)
3. `prim_index 2..378` — tree 0 (377 prims)
4. `379..581` — tree 1 (203)
5. `582..764` — tree 2 (183)
6. `765..917` — tree 3 (153)
7. `918..1021` — tree 4 (104)
8. `1022..1093` — tree 5 (72)
9. `1094..1206` — bush 0 (113)
10. `1207..1285` — bush 1 (79)

Within each plant, primitives are appended **depth-first pre-order**: the
cylinder for a node is appended before recursing into children; a terminal
node appends its cylinder first, then its leaf spheres in `i` order.

Total = **1286**. BVH is built afterwards over the whole geometry
(`src/scene.c:396`).

### 8.2 What depends on `seed`

Seed-dependent:
- `sky.seed` (cloud pattern) — `src/scene.c:350`.
- Per-plant generator seed `scene_hash(seed, 0x7|0xB, i)` → all branch angles,
  azimuths, fork counts, length jitter, leaf cluster counts/offsets/radii.
- Leaf material `variant` derived from the per-plant seed.

Seed-**in**dependent:
- The number of plants (6 trees + 2 bushes) and their `(x, z, trunk_len,
  trunk_radius)`.
- The ground plane and water box geometry/materials.
- All material values, `water_level`, `material_count`.
- The camera (`scene_default_view` / `scene_default_camera`).
- `sky_default_params` except the seed.
- The per-plant **topology** is only indirectly seed-dependent: depth is fixed
  by `trunk_radius`, but the *count* of cylinders/tips depends on the 3-way
  fork decisions, so total primitive count can vary with seed.

### 8.3 Observed count

Running the shipped binary:
```
$ ./raytracer --width 8 --height 8 --samples 1 --depth 1 --out /tmp/inv.bmp
Scene: 1286 primitives, 7 materials
```
Matches the instrumented build exactly.

---

## 9. What is NOT expressible as a simple primitive

The following are procedural/derived and require higher-level directives in a
scene file rather than raw primitive records:

1. **Trees and bushes** (§5). Each plant is a recursive generator producing
   72–377 primitives, not a fixed list. A file format needs a directive like
   `plant { type=tree|bush, position=(x,z), trunk_len, trunk_radius,
   seed, leaf_variant }` plus the global growth parameters
   (`max_depth=4`, `min_branch_radius=0.02`, `taper=0.7`, `len_decay=0.72`,
   `spread=33°`, `perturb=7°`, `up_bias=0.12`, `third_child_chance=0.25`,
   `leaf_min=15`, `leaf_span=5`). Reproducing the identical layout requires the
   exact `scene_hash`/`scene_rand` PRNG and the depth-first append order, so the
   generator must be part of the runtime, not pre-baked, unless the expanded
   primitive list is exported.
2. **Per-plant leaf material selection** is derived (`MAT_LEAF0 + variant%4`,
   `variant` from a hash), not authored per primitive.
3. **Cloud coverage pattern** is procedural fBm driven by `sky.seed`
   (`src/material.c:198-201`) — expressible as parameters, not geometry.
4. **Water wave normal** is a runtime function with **hardcoded** wave
   parameters and a **hardcoded seed 1337** (`src/material.c`, `water_normal`:
   `WATER_WAVES[]`, `freq=0.35`, `namp=0.06`, `e=0.02`, 4-octave fBm,
   `seed=1337u`). This seed is independent of the scene seed and is not stored
   in `SkyParams`/`Material`, so it cannot currently be set from a scene file.
5. **`sun_dir` is stored normalized**, not as authored Euler angles — a file
   format should either store the unit vector or the pre-normalisation
   `(0.45, 0.75, -0.50)` plus the normalisation rule.
6. **`is_water` behaviour** (wave perturbation, Beer-Lambert `water_attenuate`,
   deep-colour convergence) is renderer logic keyed off the material flag
   (`src/render.c:112, 190-205`), not a per-primitive attribute.

---

## Appendix — quick rebuild recipe

To reproduce byte-for-byte with `seed = 1337`:
1. 7 materials in the exact order of §2.
2. Add plane (0), then box (1), then 6 trees then 2 bushes in §5.1 order,
   using the generator in §5.3–5.4 with the seeds/leaf-materials of §5.2.
3. Set `water_level = 0.02`, `water_material = 6`, `sky.seed = 1337`, sky
   defaults from §6, camera from §7.
4. Expect **1286 primitives / 7 materials**.
