# Research Note — Ray Generation & Intersection Core Math

**Scope:** Primary-ray generation from a perspective camera and the four core
ray–primitive intersection routines (sphere, plane, axis-aligned box, triangle),
plus the ray representation and numerical-robustness pitfalls.

**Explicitly out of scope:** shading/BRDF models, materials, reflection/refraction
recursion, procedural content, anti-aliasing, and file formats.

**Downstream consumers:** `src/vec3.h` / `src/vec3.c` (vector math + ray),
`src/camera.h` / `src/camera.c` (primary rays), `src/geometry.h` / `src/geometry.c`
(sphere / plane / box / triangle intersection + hit records).

---

## 0. Conventions & Assumptions

- Coordinate system is **right-handed** and consistent across the whole codebase.
- All ray directions are **normalized** (`|D| = 1`). This makes `t` a true
  distance in world units, which simplifies epsilon comparisons and depth
  handling later. The intersection math below still shows the general
  non-normalized form where it costs nothing, but the implementation assumes
  unit directions.
- Vectors: **column vectors**, `P(t) = O + t·D`.
- Normals returned by intersection routines are **unit length** and, by
  convention, point **against the incoming ray** for the shaded side
  (`N · D < 0`) unless a routine explicitly documents otherwise (e.g. the box
  routine returns geometric face normals).
- Angles in radians; `fov_y` is the **vertical** field of view in radians.

### 0.1 Vector type (interface contract)

```c
/* src/vec3.h — interface the intersection code relies on */
typedef struct { double x, y, z; } Vec3;

Vec3  v3(double x, double y, double z);
Vec3  v_add(Vec3 a, Vec3 b);
Vec3  v_sub(Vec3 a, Vec3 b);
Vec3  v_mul(Vec3 a, double s);        /* scalar */
Vec3  v_mulv(Vec3 a, Vec3 b);         /* component-wise (hadamard) */
double v_dot(Vec3 a, Vec3 b);
Vec3  v_cross(Vec3 a, Vec3 b);
double v_len(Vec3 a);
double v_len2(Vec3 a);                 /* squared length, avoids sqrt */
Vec3  v_norm(Vec3 a);                  /* returns zero vector if |a| == 0 */
Vec3  v_neg(Vec3 a);

/* Ray: origin + normalized direction */
typedef struct { Vec3 origin, dir; } Ray;

/* Generic hit record shared by all primitives */
typedef struct {
    double t;        /* ray parameter of the hit (>= 0), i.e. distance */
    Vec3   point;    /* O + t*D, filled lazily by the caller if needed */
    Vec3   normal;   /* unit outward (or flipped) normal at the hit */
    int    inside;   /* 1 if the ray origin was inside the primitive */
    int    mat;      /* material index */
} Hit;
```

> **Design note:** keep `Hit.point` computed in one place (the top-level
> `scene_intersect`) as `v_add(O, v_mul(D, t))` rather than duplicating it in
> every primitive routine. Fewer chances of drift.

---

## 1. Perspective Camera Model

### 1.1 Building an orthonormal basis (look-at)

Given:

- `eye` — camera position `E`
- `target` — look-at point `T`
- `world_up` — an up hint vector `U_w` (must **not** be parallel to the view direction)
- `fov_y` — vertical field of view (radians), `aspect = width / height`

Compute the three basis axes:

```
forward = normalize(T − E)                  (this is the "w" axis, pointing into the scene)
right   = normalize(cross(forward, U_w))    (the "u" axis)
up      = cross(right, forward)             (the "v" axis, already unit if the others are)
```

Notes:

- The order `cross(forward, world_up)` vs `cross(world_up, forward)` flips the
  handedness / mirroring of the image. Pick one and keep it consistent: with a
  right-handed system and the convention above, `right` points to the **+x**
  screen direction and `up` to **+y**.
- `up` is computed as `cross(right, forward)` (not `cross(forward, right)`) so
  that `{right, up, forward}` is a proper right-handed orthonormal basis.
- **Degenerate case:** if `forward` is parallel to `world_up`, the cross product
  is the zero vector and normalization fails. Guard it:

```c
Vec3 f = v_norm(v_sub(target, eye));
Vec3 r = v_cross(f, world_up);
if (v_len2(r) < 1e-12) {
    /* pick any vector not parallel to f, e.g. nudge the up hint */
    Vec3 alt = (fabs(f.y) < 0.99) ? v3(0,1,0) : v3(1,0,0);
    r = v_cross(f, alt);
}
r = v_norm(r);
Vec3 u = v_cross(r, f);
```

### 1.2 Viewport: half-extents and corner

The viewport is a rectangle placed one unit in front of the camera (the
"focal distance" is folded into the FOV). Let:

```
h = tan(fov_y / 2)                  half-height of the viewport at distance 1
w = aspect * h                      half-width  (aspect = width / height)
```

Define the **viewport vectors** (full width / full height) and the
**lower-left corner**:

```
horizontal = 2 * w * right          (a.k.a. "viewport_u")
vertical   = 2 * h * up             (a.k.a. "viewport_v")
lower_left = E + forward − w*right − h*up
```

Equivalently, `lower_left = E + forward − 0.5*horizontal − 0.5*vertical`.

Some implementations skip `lower_left` and instead interpolate around the
center directly:

```
P = E + forward + (2u − 1) * w * right + (2v − 1) * h * up
```

Both are equivalent. The `lower_left + u*horizontal + v*vertical` form is
marginally cheaper when `horizontal`/`vertical` are precomputed once, so we use
that below.

### 1.3 Mapping normalized pixel coordinates (u, v) → ray direction

Let `(u, v) ∈ [0,1]²`:

- `u = 0` → left edge, `u = 1` → right edge
- `v = 0` → bottom edge, `v = 1` → top edge  (image-space `v` is "up")

```c
Vec3 camera_ray_dir(const Camera *c, double u, double v) {
    Vec3 on_plane = v_add(c->lower_left,
                          v_add(v_mul(c->horizontal, u),
                                v_mul(c->vertical,   v)));
    return v_norm(v_sub(on_plane, c->eye));   /* normalize => t is distance */
}

Ray camera_ray(const Camera *c, double u, double v) {
    Ray r;
    r.origin = c->eye;
    r.dir    = camera_ray_dir(c, u, v);
    return r;
}
```

**Mapping from integer pixel `(px, py)` (with `py = 0` at the top row) to (u, v):**

```
u = (px + 0.5) / width            /* +0.5 => sample at pixel center */
v = 1.0 − (py + 0.5) / height     /* flip: image row 0 is the TOP, v=1 is top */
```

For anti-aliased supersampling, replace the single center offset `0.5` with
per-sample jittered offsets in `[0,1)` before dividing.

### 1.4 Why `tan(fov_y/2)` and not the FOV itself

The perspective projection makes the viewport half-height at unit distance
equal to `tan(θ/2)` where `θ = fov_y`. So `h = tan(fov_y/2)` and `w = aspect·h`.
A larger `fov_y` widens the view; a smaller value telephotos it. Because the
aspect ratio is applied to the horizontal extent, **vertical FOV is the
authoritative parameter** and horizontal FOV follows from it
(`tan(fov_x/2) = aspect·tan(fov_y/2)`).

---

## 2. Ray Representation & Parametric Form

A ray is `{O, D}` with `D` normalized. Points on the ray:

```
P(t) = O + t·D ,  t ∈ [0, ∞)
```

- `t < 0` is **behind** the origin and must be rejected for primary rays.
- For reflection/refraction secondary rays, offset the origin along the normal
  by an epsilon (see §7) to avoid self-intersection ("shadow acne").
- Because `|D| = 1`, `t` is a Euclidean distance and `t ≈ 1e-4` epsilon values
  have consistent physical meaning.

```c
Vec3 ray_at(Ray r, double t) { return v_add(r.origin, v_mul(r.dir, t)); }
```

---

## 3. Ray–Sphere Intersection

Sphere: center `C`, radius `r > 0`. Point `P` is on the sphere iff
`|P − C|² = r²`.

### 3.1 Derivation

Substitute `P(t) = O + t·D`:

```
|O + t·D − C|² = r²
```

Let `L = O − C` (vector from center to origin). Then `O + tD − C = L + tD`:

```
(L + tD) · (L + tD) = r²
L·L + 2t(L·D) + t²(D·D) = r²
(D·D) t² + 2(L·D) t + (L·L − r²) = 0
```

So:

```
a = D·D          (= 1 when D is normalized)
b = 2 (L·D)
c = L·L − r²
discriminant  Δ = b² − 4ac
```

With **normalized D** (`a = 1`) this simplifies to:

```
b = 2 (L·D)          -> use half-b:  hb = L·D
Δ = hb² − (L·L − r²)
t = −hb ± sqrt(Δ)
```

Using the half-b form avoids a factor of 2 and is slightly more accurate. We
present the full form for clarity and give the normalized implementation.

### 3.2 Root selection

- `Δ < 0` → **no hit**.
- `Δ = 0` → tangent, one double root `t = −hb`; accept if `t > t_min`.
- `Δ > 0` → two roots `t0 = −hb − sqrt(Δ)` (near), `t1 = −hb + sqrt(Δ)` (far).

Selection policy (standard):

```
if t0 > t_min:  t_hit = t0                 (near surface, outside)
elif t1 > t_min: t_hit = t1                (origin was inside the sphere)
else:            no valid hit
```

`t_min` is a small epsilon (e.g. `1e-6`) or the current closest-hit `t` when
scanning multiple primitives.

### 3.3 Origin-inside-sphere case

If `c = L·L − r² < 0`, the origin is inside the sphere. Then `hb = L·D` and
the geometry guarantees `Δ > 0` and `t0 < 0 < t1`. The near root is behind the
origin, so we take `t1`. This falls out naturally from the selection policy
above; no special branch is required. We record `inside = (c < 0)`.

### 3.4 Surface normal

```
P_hit = O + t_hit·D
N_out = (P_hit − C) / r          (unit outward normal; radius division is exact)
```

For shading, flip to face the ray:

```
N_shading = (N_out · D < 0) ? N_out : −N_out
```

This yields the **outward** normal when the ray hits from outside and the
**inward** (i.e. still geometric outward, but oriented toward the ray) when the
ray starts inside — which is exactly what a shading routine wants. If the
material needs to know whether the ray is exiting the surface (for refraction),
record `inside` in the hit record and use `N_out` directly.

### 3.5 Pseudocode

```c
int sphere_intersect(Ray ray, Vec3 center, double radius, double t_min,
                     double t_max, Hit *out) {
    Vec3   L  = v_sub(ray.origin, center);
    double hb = v_dot(L, ray.dir);          /* half-b, assumes |dir| = 1 */
    double c  = v_dot(L, L) - radius * radius;
    double disc = hb * hb - c;

    if (disc < 0.0) return 0;               /* miss */

    double sq = sqrt(disc);
    double t  = -hb - sq;                    /* near root */
    if (t < t_min) t = -hb + sq;             /* try far root */
    if (t < t_min || t > t_max) return 0;    /* behind origin or too far */

    out->t      = t;
    out->point  = ray_at(ray, t);
    Vec3 n_out  = v_mul(v_sub(out->point, center), 1.0 / radius);
    out->normal = (v_dot(n_out, ray.dir) < 0.0) ? n_out : v_neg(n_out);
    out->inside = (c < 0.0);
    return 1;
}
```

---

## 4. Ray–Plane Intersection

Infinite plane defined by a point `P0` on it and a normal `N` (need not be
unit, but we normalize it). Plane equation: `(P − P0) · N = 0`.

### 4.1 Derivation

Substitute `P(t) = O + t·D`:

```
(O + tD − P0) · N = 0
(O − P0)·N + t (D·N) = 0
t = ((P0 − O) · N) / (D · N)
```

Equivalent sign conventions exist (`((O − P0)·N)/(D·N)` with a leading minus);
the form above is the common one.

### 4.2 Degenerate parallel case

If `D · N ≈ 0`, the ray is parallel to the plane: either no intersection
(if `(P0 − O)·N ≠ 0`) or the ray lies **in** the plane (infinitely many). Both
are treated as "no hit" for a raytracer. Guard with a tolerance:

```
denom = D · N
if (fabs(denom) < 1e-9) return 0;      /* parallel — reject */
t = ((P0 − O)·N) / denom
if (t < t_min || t > t_max) return 0;  /* behind or clipped */
```

The tolerance must be **absolute** here (`1e-9`), not relative, because `denom`
is a cosine of the angle between unit vectors and is naturally in `[−1,1]`.

### 4.3 Normal orientation

The geometric normal is `±N`. Orient it against the ray:

```
N_hit = (N · D < 0) ? N : −N
```

For a one-sided ground plane (normal `+y`), a ray coming from above yields
`N · D < 0` and `N_hit = +y`. A ray from below yields `−y`.

### 4.4 Pseudocode

```c
int plane_intersect(Ray ray, Vec3 p0, Vec3 n, double t_min, double t_max,
                    Hit *out) {
    n = v_norm(n);
    double denom = v_dot(ray.dir, n);
    if (fabs(denom) < 1e-9) return 0;                 /* parallel */

    double t = v_dot(v_sub(p0, ray.origin), n) / denom;
    if (t < t_min || t > t_max) return 0;

    out->t      = t;
    out->point  = ray_at(ray, t);
    out->normal = (denom < 0.0) ? n : v_neg(n);        /* face the ray */
    out->inside = 0;
    return 1;
}
```

---

## 5. Ray–Axis-Aligned Box (Slab Method)

Box defined by `min` and `max` corners (component-wise `min ≤ max`). The slab
method intersects the ray with each of the three pairs of parallel planes
(x, y, z) and keeps the running overlap interval `[tmin, tmax]`.

### 5.1 Derivation

For each axis `i`, the ray is inside the slab `[min_i, max_i]` for
`t ∈ [t1_i, t2_i]`:

```
t1_i = (min_i − O_i) / D_i
t2_i = (max_i − O_i) / D_i
```

Order them so `t1_i ≤ t2_i`, then intersect intervals:

```
tmin = max(tmin, min(t1_i, t2_i))
tmax = min(tmax, max(t1_i, t2_i))
if (tmax < tmin) -> no hit
```

Initialize `tmin = -inf`, `tmax = +inf`. A hit exists iff the final interval is
non-empty and `tmax ≥ max(tmin, 0)`.

### 5.2 Division-by-zero handling

When `D_i == 0` the ray is parallel to that slab. Using IEEE-754 infinities this
"just works" if you compute `1/D_i` once and multiply:

```
inv = 1.0 / D_i;                 /* ±inf when D_i == 0, 0.0 preserved for sign */
t1 = (min_i - O_i) * inv;        /* ±inf, correct sign */
t2 = (max_i - O_i) * inv;
```

Caveat: `0 * inf = NaN`. That NaN can occur when `O_i == min_i` (or `max_i`)
and `D_i == 0`. A NaN in `min/max` comparisons silently propagates in some
orderings. Two robust options:

1. **Branchless with NaN guard:** keep the multiply form but use `fmin`/`fmax`,
   which return the non-NaN operand, instead of ternary `min`/`max`.
2. **Explicit branch:** if `fabs(D_i) < eps`, check `O_i` against the slab
   directly and either keep the interval unchanged (inside) or reject (outside).

The `fmin`/`fmax` approach is compact and safe:

```c
double inv = 1.0 / d;
double t1  = (bmin - o) * inv;
double t2  = (bmax - o) * inv;
tmin = fmax(tmin, fmin(t1, t2));
tmax = fmin(tmax, fmax(t1, t2));
```

### 5.3 Which face was hit (normal)

The normal is the axis on which the entering plane was set. Track it:

```
if (t1 > tmin_old) { tmin = t1; normal_axis = i; normal_sign = (inv > 0) ? -1 : +1; }
```

where `inv = 1/D_i`; the sign of `inv` tells you whether the ray travels in the
`+i` or `−i` direction, hence which face it entered through. For a mere hit
**test** (shadow/AABB acceleration) you can skip the normal entirely.

### 5.4 Pseudocode (hit test + optional normal)

```c
int box_intersect(Ray ray, Vec3 bmin, Vec3 bmax, double t_min, double t_max,
                  Hit *out) {
    double tmin = t_min, tmax = t_max;
    int axis = -1;
    double o[3] = { ray.origin.x, ray.origin.y, ray.origin.z };
    double d[3] = { ray.dir.x,    ray.dir.y,    ray.dir.z    };
    double lo[3] = { bmin.x, bmin.y, bmin.z };
    double hi[3] = { bmax.x, bmax.y, bmax.z };

    for (int i = 0; i < 3; ++i) {
        double inv = 1.0 / d[i];
        double t1  = (lo[i] - o[i]) * inv;
        double t2  = (hi[i] - o[i]) * inv;
        double tn  = fmin(t1, t2);
        double tf  = fmax(t1, t2);
        if (tn > tmin) { tmin = tn; axis = i; }
        tmax = fmin(tmax, tf);
        if (tmax < tmin) return 0;        /* intervals disjoint -> miss */
    }

    if (tmin < t_min || tmin > t_max) return 0;

    if (out) {
        out->t     = tmin;
        out->point = ray_at(ray, tmin);
        Vec3 n = v3(0, 0, 0);
        if (axis >= 0) {
            double inv = 1.0 / d[axis];
            double sign = (inv > 0.0) ? -1.0 : 1.0;
            if (axis == 0) n.x = sign;
            else if (axis == 1) n.y = sign;
            else            n.z = sign;
        } else {
            n = v_neg(ray.dir);           /* origin inside the box */
        }
        out->normal = n;                   /* geometric face normal */
        out->inside = (axis < 0);
    }
    return 1;
}
```

> **Note:** when the ray origin is inside the box, `tmin` stays `< t_min` (or the
> initial value) and `axis` may be `-1`; the ray exits through `tmax`. If you
> need the exit point (e.g. to leave a volume), return `tmax` in that case. For
> typical solid-object rendering the entry test above suffices.

---

## 6. Ray–Triangle Intersection (Möller–Trumbore)

Triangle with vertices `V0, V1, V2`. Points on the triangle:

```
P = V0 + u·(V1 − V0) + v·(V2 − V0),   with  u ≥ 0, v ≥ 0, u + v ≤ 1
```

Edge vectors:

```
e1 = V1 − V0
e2 = V2 − V0
```

### 6.1 Derivation

Set `O + t·D = V0 + u·e1 + v·e2` and solve for `(t, u, v)` using Cramer's rule.
Let `pvec = D × e2` and `det = e1 · pvec`:

```
det = e1 · (D × e2)                    determinant
tvec = O − V0
u    = (tvec · pvec) / det
qvec = tvec × e1
v    = (D · qvec) / det
t    = (e2 · qvec) / det
```

This is the **Möller–Trumbore** formulation. It is division-light and avoids
explicitly constructing the triangle's plane.

### 6.2 Epsilon tolerances & backface handling

- If `|det| < EPS` (`EPS ≈ 1e-9`), the ray is parallel to the triangle plane
  (or the triangle is degenerate, e.g. zero area) → reject.
- **Backface culling:** if you want to render only front faces, require
  `det > EPS` (winding-dependent). For a two-sided raytracer (the common case),
  use `fabs(det) > EPS` and test `u`, `v` in `[0,1]`.
- Barycentric bounds (with a small tolerance `BARY_EPS ≈ 1e-9` to avoid gaps at
  shared edges):

```
u < -BARY_EPS  -> miss
v < -BARY_EPS  -> miss
u + v > 1 + BARY_EPS -> miss
```

- `t` must satisfy `t > t_min` (and `< t_max` if clipping).

### 6.3 Normal

Geometric (flat) normal:

```
N = normalize(cross(e1, e2))
```

Then orient against the ray:

```
if (N · D > 0) N = −N;      /* make it face the incoming ray */
```

For smooth shading (Phong interpolation), use the per-vertex normals `n0,n1,n2`:

```
N_smooth = normalize( (1 − u − v)·n0 + u·n1 + v·n2 )
```

where `(1 − u − v)`, `u`, `v` are the barycentric weights of `V0`, `V1`, `V2`.
Re-orient that against the ray as well.

### 6.4 Pseudocode

```c
#define TRI_EPS   1e-9
#define BARY_EPS  1e-9

int triangle_intersect(Ray ray, Vec3 v0, Vec3 v1, Vec3 v2,
                       double t_min, double t_max, Hit *out) {
    Vec3 e1 = v_sub(v1, v0);
    Vec3 e2 = v_sub(v2, v0);

    Vec3 pvec = v_cross(ray.dir, e2);
    double det = v_dot(e1, pvec);

    /* Two-sided test: use fabs(det). Front-face-only: require det > TRI_EPS. */
    if (fabs(det) < TRI_EPS) return 0;        /* parallel / degenerate */

    double inv_det = 1.0 / det;
    Vec3 tvec = v_sub(ray.origin, v0);
    double u = v_dot(tvec, pvec) * inv_det;
    if (u < -BARY_EPS || u > 1.0 + BARY_EPS) return 0;

    Vec3 qvec = v_cross(tvec, e1);
    double v = v_dot(ray.dir, qvec) * inv_det;
    if (v < -BARY_EPS || u + v > 1.0 + BARY_EPS) return 0;

    double t = v_dot(e2, qvec) * inv_det;
    if (t < t_min || t > t_max) return 0;

    out->t     = t;
    out->point = ray_at(ray, t);
    Vec3 n = v_norm(v_cross(e1, e2));
    out->normal = (v_dot(n, ray.dir) < 0.0) ? n : v_neg(n);
    out->inside = 0;
    return 1;
}
```

> **Alternative:** the "double-sided" variant flips the sign of `det` and
> `tvec` when `det < 0` so that the same `u,v` tests work for backfaces without
> `fabs`. This is the branch-free Möller–Trumbore variant:
>
> ```c
> if (det > -TRI_EPS && det < TRI_EPS) return 0;
> double inv = 1.0 / det;
> ...  /* u = dot(tvec,pvec)*inv; v = dot(dir,qvec)*inv; ... */
> if (u < 0 || u > 1 || v < 0 || u + v > 1) return 0;
> ```
> Note the exact-zero comparison is not robust for backfaces; keep the `fabs`
> form unless profiling shows the triangle routine is the bottleneck.

---

## 7. Pitfalls / Numerical Robustness

1. **Non-normalized directions.** If `D` is not unit length, `t` is no longer a
   distance, the sphere `a` coefficient is no longer 1, and epsilon comparisons
   become scale-dependent. Always normalize `D` at ray construction (primary
   rays) and after computing reflection/refraction directions (secondary rays).

2. **Shadow/acne self-intersection.** A ray leaving a surface can immediately
   re-hit the same primitive due to floating-point error. Offset the new origin
   along the surface normal:
   `O' = P_hit + ε·N` (with `ε ≈ 1e-4 … 1e-3` scaled to scene size), or use a
   `t_min` epsilon in the intersection test. Prefer `t_min` when the primitive
   is curved (sphere), and origin offset when the geometry is flat (plane,
   triangle) and near-coplanar primitives are possible.

3. **`0 * inf = NaN` in the slab method.** Handled with `fmin`/`fmax` (§5.2).
   Never use a hand-rolled `a < b ? a : b` macro for slab tests if it can see a
   NaN operand.

4. **Parallel-ray tolerance is absolute, not relative.** `D·N` and the slab
   `D_i` are naturally bounded quantities (`D` is unit). Use absolute epsilons
   (`1e-9`) — relative comparisons can misfire near zero.

5. **Sphere root cancellation.** For spheres very far from the origin, computing
   `t = -hb ± sqrt(disc)` can suffer catastrophic cancellation in the far root.
   The numerically stable form (already implicit in the near-root-first policy)
   is:
   ```
   q  = -hb - copysign(sqrt(disc), hb)
   t0 = q                       (near)
   t1 = c / q                   (far, via Vieta: t0*t1 = c)
   ```
   This preserves precision when `hb` and `sqrt(disc)` are nearly equal.

6. **Degenerate triangle / zero area.** `det ≈ 0` catches most cases, but a
   sliver triangle with tiny but non-zero area can produce wild barycentrics.
   If triangles are generated procedurally, skip triangles whose edge cross
   product length is below a threshold at build time.

7. **`t_min` vs `t_max` semantics.** Define them once, globally:
   `t_min` is the smallest acceptable `t` (usually a small epsilon or the
   current closest hit), `t_max` is the farthest (usually `+inf` for primary
   rays, or a finite distance for shadow rays). Every routine must honor both.

8. **Normalizing the normal twice.** `(P_hit − C)/r` is already unit for a
   sphere; re-normalizing is harmless but wasteful. `cross(e1,e2)` for a
   triangle is *not* unit and **must** be normalized.

9. **Inside/outside bookkeeping.** Decide once whether `Hit.inside` means "ray
   origin was inside the primitive". It matters for refraction (choosing the
   IOR ratio) and for flipping normals. Record it in every routine.

10. **Aspect-ratio mistakes.** The most common camera bug is applying `aspect`
    to the wrong axis. `h = tan(fov_y/2)` (vertical), `w = aspect * h`
    (horizontal). If the image is stretched, swap them.

11. **Row vs column indexing.** `u` grows with the pixel column, `v` grows
    *upward* while image rows grow downward. The `v = 1 − (py+0.5)/height` flip
    is mandatory; forgetting it renders the scene upside-down.

12. **`double` vs `float`.** Use `double` throughout the core math. `float`
    loses enough precision that thin geometry and grazing rays flicker, and the
    cost difference in a from-scratch C raytracer is negligible.

---

## 8. Quick Reference Summary

| Primitive | Solve | Key guard | Normal |
|---|---|---|---|
| Sphere | `t² + 2(L·D)t + (L·L − r²) = 0`, `L = O − C` | `Δ = (L·D)² − (L·L − r²) ≥ 0`; take near root `> t_min` else far | `(P − C)/r`, flip toward ray; `inside = (L·L < r²)` |
| Plane | `t = ((P0 − O)·N)/(D·N)` | `\|D·N\| > 1e-9` (parallel ⇒ miss) | `±N` toward ray |
| AABB | slab: `t1_i=(min_i−O_i)/D_i`, `t2_i=(max_i−O_i)/D_i`; `tmin=max(...)`, `tmax=min(...)` | `tmax ≥ tmin`; `fmin/fmax` for `0·inf` NaN | axis of entering slab, sign from `1/D_i` |
| Triangle | Möller–Trumbore: `det=e1·(D×e2)`, `u=(tvec·pvec)/det`, `v=(D·qvec)/det`, `t=(e2·qvec)/det` | `\|det\| > 1e-9`; `u,v ≥ 0`, `u+v ≤ 1` | `normalize(e1×e2)`, flip toward ray |

---

## 9. References (standard, well-established sources)

These formulas are textbook-standard; no live retrieval was required. Canonical
references for verification:

- Möller, T. & Trumbore, B. (1997). *Fast, Minimum Storage Ray/Triangle
  Intersection*. Journal of Graphics Tools, 2(1), 21–28.
  DOI: 10.1080/10867651.1997.10487468
- Möller, T. & Haines, E. *Real-Time Rendering* / *Real-Time Collision
  Detection* (slab method, barycentric intersection).
- Shirley, P. *Ray Tracing in One Weekend* series (camera model, sphere
  intersection, orthonormal basis construction).
  https://raytracing.github.io/
- Pharr, M., Jakob, W., Humphreys, G. *Physically Based Rendering: From Theory
  to Implementation* (robust ray–primitive intersection, epsilon handling,
  quadratic root stability). https://pbr-book.org/
- Ericson, C. *Real-Time Collision Detection* (slab method, AABB, degenerate
  cases).

---

*End of note. Scope intentionally limited to ray generation and the four core
intersection routines; shading, materials, and I/O are covered by separate
research notes.*
