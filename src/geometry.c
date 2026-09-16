/*
 * geometry.c - Geometry primitives and ray/primitive intersection.
 *
 * Implements ray tests for sphere, plane, axis-aligned box, triangle and
 * finite capped tapered cylinder (truncated cone). Depends only on vec3.h.
 * No I/O, no global state, C11, warning-free under -Wall -Wextra.
 *
 * Numerical notes:
 *   - Rays are normalized internally so `t` is a Euclidean distance.
 *   - Sphere and tapered-cylinder quadratics are computed in the stable
 *     "half-b" form (b' = b/2), which avoids the 2*a factor and keeps the
 *     discriminant well conditioned.
 *   - Epsilons are absolute because all direction/normal vectors are unit.
 */

#include "geometry.h"

#include <math.h>
#include <stdlib.h>

/* Parallel-ray guard for the plane test (cosine of unit vectors). */
#define GEO_PARALLEL_EPS 1e-12
/* Möller–Trumbore determinant guard (parallel ray / degenerate triangle). */
#define GEO_TRI_EPS      1e-9
/* Barycentric tolerance to avoid gaps along shared triangle edges. */
#define GEO_BARY_EPS     1e-9
/* Below this axis length a cylinder is treated as a point (no hit). */
#define GEO_AXIS_EPS     1e-12
/* Half extent of the finite AABB used to represent an infinite plane. */
#define GEO_PLANE_EXTENT 1e4

/* ------------------------------------------------------------------ */
/* Small local helpers                                                 */
/* ------------------------------------------------------------------ */

/* Pick the smallest root of a*t^2 + b*t + c = 0 (stable half-b form) that
 * lies inside [tmin, tmax]. Returns 1 and writes *t_out on success. */
static int geo_quad_nearest(double a, double half_b, double c,
                            double tmin, double tmax, double *t_out)
{
    if (fabs(a) < 1e-15) {
        /* Degenerate quadratic (linear): solve half_b*t + c = 0. */
        if (fabs(half_b) < 1e-15) return 0;
        double t = -c / (2.0 * half_b);
        if (t < tmin || t > tmax) return 0;
        *t_out = t;
        return 1;
    }

    double disc = half_b * half_b - a * c;
    if (disc < 0.0) return 0;

    double sq = sqrt(disc);
    double t0 = (-half_b - sq) / a;
    double t1 = (-half_b + sq) / a;
    if (t0 > t1) {
        double tmp = t0;
        t0 = t1;
        t1 = tmp;
    }

    if (t0 >= tmin && t0 <= tmax) {
        *t_out = t0;
        return 1;
    }
    if (t1 >= tmin && t1 <= tmax) {
        *t_out = t1;
        return 1;
    }
    return 0;
}

/* Store a hit, orienting the unit normal to oppose the ray.
 * Note: prim_index is intentionally NOT touched here; geometry_intersect
 * seeds it before dispatching, and primitive_intersect preserves it. */
static void geo_store_hit(Hit *out, double t, Vec3 point, Vec3 unit_geo_normal,
                          Ray r, int material_index)
{
    int front = vec3_dot(r.dir, unit_geo_normal) < 0.0;
    Vec3 n = front ? unit_geo_normal : vec3_neg(unit_geo_normal);

    out->t = t;
    out->point = point;
    out->normal = n;
    out->material_index = material_index;
    out->front_face = front ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Geometry lifecycle                                                  */
/* ------------------------------------------------------------------ */

void geometry_init(Geometry *g)
{
    if (!g) return;
    g->prims = NULL;
    g->count = 0;
    g->capacity = 0;
}

void geometry_free(Geometry *g)
{
    if (!g) return;
    free(g->prims);
    g->prims = NULL;
    g->count = 0;
    g->capacity = 0;
}

int geometry_add(Geometry *g, Primitive p)
{
    if (!g) return -1;

    if (g->count >= g->capacity) {
        int new_cap = (g->capacity > 0) ? g->capacity * 2 : 8;
        Primitive *grown = (Primitive *)realloc(g->prims,
                                                (size_t)new_cap * sizeof(Primitive));
        if (!grown) return -1;
        g->prims = grown;
        g->capacity = new_cap;
    }

    g->prims[g->count] = p;
    return g->count++;
}

/* ------------------------------------------------------------------ */
/* Convenience constructors                                            */
/* ------------------------------------------------------------------ */

static Primitive prim_zero(PrimKind kind, int material_index)
{
    Primitive p;
    p.kind = kind;
    p.material_index = material_index;
    p.center = vec3(0.0, 0.0, 0.0);
    p.axis = vec3(0.0, 0.0, 0.0);
    p.half = vec3(0.0, 0.0, 0.0);
    p.a = vec3(0.0, 0.0, 0.0);
    p.b = vec3(0.0, 0.0, 0.0);
    p.c = vec3(0.0, 0.0, 0.0);
    p.radius = 0.0;
    p.radius2 = 0.0;
    return p;
}

Primitive prim_sphere(Vec3 center, double radius, int material_index)
{
    Primitive p = prim_zero(PRIM_SPHERE, material_index);
    p.center = center;
    p.radius = radius;
    return p;
}

Primitive prim_plane(Vec3 point, Vec3 normal, int material_index)
{
    Primitive p = prim_zero(PRIM_PLANE, material_index);
    p.center = point;
    p.axis = vec3_normalize(normal); /* unit normal */
    return p;
}

Primitive prim_box(Vec3 center, Vec3 half, int material_index)
{
    Primitive p = prim_zero(PRIM_BOX, material_index);
    p.center = center;
    p.half = half;
    return p;
}

Primitive prim_triangle(Vec3 a, Vec3 b, Vec3 c, int material_index)
{
    Primitive p = prim_zero(PRIM_TRIANGLE, material_index);
    p.a = a;
    p.b = b;
    p.c = c;
    return p;
}

Primitive prim_cylinder(Vec3 base, Vec3 top, double r_bottom, double r_top,
                        int material_index)
{
    Primitive p = prim_zero(PRIM_CYLINDER, material_index);
    p.a = base;
    p.b = top;
    p.radius = r_bottom;
    p.radius2 = r_top;
    return p;
}

/* ------------------------------------------------------------------ */
/* Individual intersection routines                                    */
/* ------------------------------------------------------------------ */

/* Ray-sphere. Handles the origin-inside case and tangent hits. */
static int intersect_sphere(const Primitive *p, Ray r, double tmin, double tmax,
                            Hit *out)
{
    if (p->radius <= 0.0) return 0;

    Vec3 l = vec3_sub(r.origin, p->center);
    double half_b = vec3_dot(l, r.dir);          /* |dir| == 1 */
    double c = vec3_length_sq(l) - p->radius * p->radius;

    double t;
    if (!geo_quad_nearest(1.0, half_b, c, tmin, tmax, &t)) return 0;

    Vec3 point = vec3_at(r, t);
    Vec3 n = vec3_scale(vec3_sub(point, p->center), 1.0 / p->radius);
    geo_store_hit(out, t, point, n, r, p->material_index);
    return 1;
}

/* Ray-plane. Rejects near-parallel rays. */
static int intersect_plane(const Primitive *p, Ray r, double tmin, double tmax,
                           Hit *out)
{
    Vec3 n = vec3_normalize(p->axis); /* robust if caller bypassed constructor */
    double denom = vec3_dot(r.dir, n);
    if (fabs(denom) < GEO_PARALLEL_EPS) return 0; /* parallel / in-plane */

    double t = vec3_dot(vec3_sub(p->center, r.origin), n) / denom;
    if (t < tmin || t > tmax) return 0;

    Vec3 point = vec3_at(r, t);
    geo_store_hit(out, t, point, n, r, p->material_index);
    return 1;
}

/* Ray-AABB, slab method. Computes the entry-face normal. */
static int intersect_box(const Primitive *p, Ray r, double tmin, double tmax,
                         Hit *out)
{
    Vec3 bmin = vec3_sub(p->center, p->half);
    Vec3 bmax = vec3_add(p->center, p->half);

    double o[3] = { r.origin.x, r.origin.y, r.origin.z };
    double d[3] = { r.dir.x, r.dir.y, r.dir.z };
    double lo[3] = { bmin.x, bmin.y, bmin.z };
    double hi[3] = { bmax.x, bmax.y, bmax.z };

    double tn = -INFINITY;
    double tf = INFINITY;
    int entry_axis = -1, entry_sign = 0;
    int exit_axis = -1, exit_sign = 0;

    for (int i = 0; i < 3; ++i) {
        double inv = 1.0 / d[i]; /* +/-inf when d[i] == 0 */
        double t1 = (lo[i] - o[i]) * inv;
        double t2 = (hi[i] - o[i]) * inv;

        /* fmin/fmax propagate the non-NaN operand for the 0*inf case. */
        double t_near = fmin(t1, t2);
        double t_far = fmax(t1, t2);

        if (t_near > tn) {
            tn = t_near;
            entry_axis = i;
            entry_sign = (inv > 0.0) ? -1 : 1;
        }
        if (t_far < tf) {
            tf = t_far;
            exit_axis = i;
            exit_sign = (inv > 0.0) ? 1 : -1;
        }
        if (tf < tn) return 0; /* disjoint intervals -> miss */
    }

    /* Choose the nearest surface: entry if in front of the origin, otherwise
     * the exit face (origin is inside the box). */
    double t_hit;
    int axis, axis_sign;
    if (tn >= tmin && tn <= tmax) {
        t_hit = tn;
        axis = entry_axis;
        axis_sign = entry_sign;
    } else if (tf >= tmin && tf <= tmax) {
        t_hit = tf;
        axis = exit_axis;
        axis_sign = exit_sign;
    } else {
        return 0;
    }

    Vec3 point = vec3_at(r, t_hit);
    Vec3 n;
    if (axis < 0) {
        /* Degenerate: no face identified, face back along the ray. */
        n = vec3_neg(r.dir);
    } else {
        n = vec3(0.0, 0.0, 0.0);
        if (axis == 0) n.x = (double)axis_sign;
        else if (axis == 1) n.y = (double)axis_sign;
        else n.z = (double)axis_sign;
    }
    geo_store_hit(out, t_hit, point, n, r, p->material_index);
    return 1;
}

/* Ray-triangle, Möller–Trumbore, two-sided. */
static int intersect_triangle(const Primitive *p, Ray r, double tmin, double tmax,
                              Hit *out)
{
    Vec3 e1 = vec3_sub(p->b, p->a);
    Vec3 e2 = vec3_sub(p->c, p->a);

    Vec3 pvec = vec3_cross(r.dir, e2);
    double det = vec3_dot(e1, pvec);
    if (fabs(det) < GEO_TRI_EPS) return 0; /* parallel or degenerate */

    double inv_det = 1.0 / det;
    Vec3 tvec = vec3_sub(r.origin, p->a);
    double u = vec3_dot(tvec, pvec) * inv_det;
    if (u < -GEO_BARY_EPS || u > 1.0 + GEO_BARY_EPS) return 0;

    Vec3 qvec = vec3_cross(tvec, e1);
    double v = vec3_dot(r.dir, qvec) * inv_det;
    if (v < -GEO_BARY_EPS || u + v > 1.0 + GEO_BARY_EPS) return 0;

    double t = vec3_dot(e2, qvec) * inv_det;
    if (t < tmin || t > tmax) return 0;

    Vec3 n = vec3_cross(e1, e2);
    double nlen = vec3_length(n);
    if (nlen < 1e-15) return 0; /* degenerate triangle */
    n = vec3_scale(n, 1.0 / nlen);

    Vec3 point = vec3_at(r, t);
    geo_store_hit(out, t, point, n, r, p->material_index);
    return 1;
}

/* Ray-tapered-cylinder (truncated cone): lateral surface + two caps.
 *
 * Derivation follows docs/research_trees.md §3.2. With axis A = b - a,
 * h = |A|, â = A/h, k = (r1 - r0)/h and the ray in the base frame, the
 * lateral surface satisfies |X_perp|^2 = r(s)^2, giving the quadratic
 *   a t^2 + b' t + c = 0  with  a = |D_perp|^2 - ra^2,
 *   b' = (D_perp·d0_perp) - rb*ra,  c = |d0_perp|^2 - rb^2,
 * where ra = k*Od, rb = r0 + k*d0d and s = (d0d + t*Od)/h.
 */
static int intersect_cylinder(const Primitive *p, Ray r, double tmin, double tmax,
                              Hit *out)
{
    Vec3 A = vec3_sub(p->b, p->a);
    double h = vec3_length(A);
    if (h < GEO_AXIS_EPS) return 0; /* degenerate: zero-length axis */

    Vec3 axis = vec3_scale(A, 1.0 / h);
    double r0 = p->radius;
    double r1 = p->radius2;
    double k = (r1 - r0) / h;

    Vec3 d0 = vec3_sub(r.origin, p->a);
    double Od = vec3_dot(r.dir, axis);
    double d0d = vec3_dot(d0, axis);

    Vec3 D_perp = vec3_sub(r.dir, vec3_scale(axis, Od));
    Vec3 d0_perp = vec3_sub(d0, vec3_scale(axis, d0d));

    double ra = k * Od;
    double rb = r0 + k * d0d;

    double qa = vec3_length_sq(D_perp) - ra * ra;
    double qb = vec3_dot(D_perp, d0_perp) - rb * ra;
    double qc = vec3_length_sq(d0_perp) - rb * rb;

    int found = 0;
    double best_t = 0.0;
    Vec3 best_n = vec3(0.0, 0.0, 0.0);

    /* --- Lateral surface --- */
    if (fabs(qa) > 1e-15) {
        double disc = qb * qb - qa * qc;
        if (disc >= 0.0) {
            double sq = sqrt(disc);
            double roots[2];
            roots[0] = (-qb - sq) / qa;
            roots[1] = (-qb + sq) / qa;
            if (roots[0] > roots[1]) {
                double tmp = roots[0];
                roots[0] = roots[1];
                roots[1] = tmp;
            }
            for (int i = 0; i < 2; ++i) {
                double t = roots[i];
                if (t < tmin || t > tmax) continue;
                double s = (d0d + t * Od) / h;
                if (s < 0.0 || s > 1.0) continue;

                Vec3 point = vec3_at(r, t);
                Vec3 axis_pt = vec3_add(p->a, vec3_scale(axis, s * h));
                Vec3 radial = vec3_sub(point, axis_pt);
                double rlen = vec3_length(radial);
                Vec3 radial_dir = (rlen > 1e-15) ? vec3_scale(radial, 1.0 / rlen)
                                                 : vec3(0.0, 0.0, 0.0);
                /* Outward cone normal: radial * h - axis * (r1 - r0). */
                Vec3 n = vec3_sub(vec3_scale(radial_dir, h),
                                  vec3_scale(axis, r1 - r0));
                n = vec3_normalize(n);
                if (vec3_length_sq(n) < 0.5) n = radial_dir; /* safety */

                if (!found || t < best_t) {
                    found = 1;
                    best_t = t;
                    best_n = n;
                }
            }
        }
    } else if (fabs(qb) > 1e-15) {
        /* Linear degeneracy (e.g. ray parallel to a cone's rulings). */
        double t = -qc / (2.0 * qb);
        if (t >= tmin && t <= tmax) {
            double s = (d0d + t * Od) / h;
            if (s >= 0.0 && s <= 1.0) {
                Vec3 point = vec3_at(r, t);
                Vec3 axis_pt = vec3_add(p->a, vec3_scale(axis, s * h));
                Vec3 radial = vec3_sub(point, axis_pt);
                Vec3 radial_dir = vec3_normalize(radial);
                Vec3 n = vec3_sub(vec3_scale(radial_dir, h),
                                  vec3_scale(axis, r1 - r0));
                n = vec3_normalize(n);
                if (vec3_length_sq(n) < 0.5) n = radial_dir;
                found = 1;
                best_t = t;
                best_n = n;
            }
        }
    }

    /* --- Caps: two axis-perpendicular discs at s = 0 and s = 1 --- */
    if (fabs(Od) > 1e-15) {
        for (int cap = 0; cap < 2; ++cap) {
            double s_cap = (cap == 0) ? 0.0 : 1.0;
            double r_cap = (cap == 0) ? r0 : r1;
            if (r_cap <= 0.0) continue;

            Vec3 cap_center = (cap == 0) ? p->a : p->b;
            double t = ((s_cap * h) - d0d) / Od;
            if (t < tmin || t > tmax) continue;

            Vec3 point = vec3_at(r, t);
            Vec3 radial = vec3_sub(point, cap_center);
            if (vec3_length_sq(radial) > r_cap * r_cap) continue;

            Vec3 n = (cap == 0) ? vec3_neg(axis) : axis; /* outward */
            if (!found || t < best_t) {
                found = 1;
                best_t = t;
                best_n = n;
            }
        }
    }

    if (!found) return 0;

    Vec3 point = vec3_at(r, best_t);
    geo_store_hit(out, best_t, point, best_n, r, p->material_index);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public intersection dispatch                                        */
/* ------------------------------------------------------------------ */

int primitive_intersect(const Primitive *p, Ray r, double tmin, double tmax, Hit *out)
{
    if (!p || !out) return 0;

    /* Normalize the direction once so `t` is a distance. */
    double len_sq = vec3_length_sq(r.dir);
    if (len_sq > 0.0 && fabs(len_sq - 1.0) > 1e-12) {
        r.dir = vec3_normalize(r.dir);
    }

    switch (p->kind) {
    case PRIM_SPHERE:   return intersect_sphere(p, r, tmin, tmax, out);
    case PRIM_PLANE:    return intersect_plane(p, r, tmin, tmax, out);
    case PRIM_BOX:      return intersect_box(p, r, tmin, tmax, out);
    case PRIM_TRIANGLE: return intersect_triangle(p, r, tmin, tmax, out);
    case PRIM_CYLINDER: return intersect_cylinder(p, r, tmin, tmax, out);
    default:            return 0;
    }
}

int geometry_intersect(const Geometry *g, Ray r, double tmin, double tmax, Hit *out)
{
    if (!g || !out || g->count <= 0 || !g->prims) return 0;

    int found = 0;
    double closest = tmax;
    Hit best;

    for (int i = 0; i < g->count; ++i) {
        Hit h;
        h.prim_index = i; /* seed so the primitive can stamp its index */
        if (primitive_intersect(&g->prims[i], r, tmin, closest, &h)) {
            if (!found || h.t < best.t) {
                best = h;
                closest = h.t;
                found = 1;
            }
        }
    }

    if (!found) return 0;
    *out = best;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Bounding boxes                                                      */
/* ------------------------------------------------------------------ */

static void bounds_sphere(const Primitive *p, Vec3 *out_min, Vec3 *out_max)
{
    Vec3 ext = vec3(p->radius, p->radius, p->radius);
    *out_min = vec3_sub(p->center, ext);
    *out_max = vec3_add(p->center, ext);
}

static void bounds_plane(const Primitive *p, Vec3 *out_min, Vec3 *out_max)
{
    (void)p;
    double e = GEO_PLANE_EXTENT;
    *out_min = vec3(-e, -e, -e);
    *out_max = vec3(e, e, e);
}

static void bounds_box(const Primitive *p, Vec3 *out_min, Vec3 *out_max)
{
    Vec3 h = vec3(fabs(p->half.x), fabs(p->half.y), fabs(p->half.z));
    *out_min = vec3_sub(p->center, h);
    *out_max = vec3_add(p->center, h);
}

static void bounds_triangle(const Primitive *p, Vec3 *out_min, Vec3 *out_max)
{
    *out_min = vec3_min(p->a, vec3_min(p->b, p->c));
    *out_max = vec3_max(p->a, vec3_max(p->b, p->c));
}

static void bounds_cylinder(const Primitive *p, Vec3 *out_min, Vec3 *out_max)
{
    double rmax = (p->radius > p->radius2) ? p->radius : p->radius2;
    if (rmax < 0.0) rmax = 0.0;
    Vec3 ext = vec3(rmax, rmax, rmax);
    *out_min = vec3_sub(vec3_min(p->a, p->b), ext);
    *out_max = vec3_add(vec3_max(p->a, p->b), ext);
}

void primitive_bounds(const Primitive *p, Vec3 *out_min, Vec3 *out_max)
{
    if (!p || !out_min || !out_max) return;

    switch (p->kind) {
    case PRIM_SPHERE:   bounds_sphere(p, out_min, out_max);   break;
    case PRIM_PLANE:    bounds_plane(p, out_min, out_max);    break;
    case PRIM_BOX:      bounds_box(p, out_min, out_max);      break;
    case PRIM_TRIANGLE: bounds_triangle(p, out_min, out_max); break;
    case PRIM_CYLINDER: bounds_cylinder(p, out_min, out_max); break;
    default:
        *out_min = vec3(0.0, 0.0, 0.0);
        *out_max = vec3(0.0, 0.0, 0.0);
        break;
    }
}
