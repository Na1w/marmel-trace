/*
 * curve.c - 3D Parametric Curves, Bending, and Fairytale Tree Generation.
 *
 * Implements:
 *   - Cubic Bézier curve evaluation with sinusoidal wobble and 3D noise modulation.
 *   - Numerical unit tangent computation.
 *   - Curved tube / vine generation via chained tapered cylinders.
 *   - Curved beam / bent box generation via parallel-transport frame extrusion.
 *   - Gnarled fairytale tree (sagoträd) generation.
 *
 * Self-contained, warning-free under -Wall -Wextra, deterministic, C11.
 */

#include "curve.h"
#include "noise.h"
#include <math.h>
#include <stdlib.h>

#define CURVE_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* Deterministic hash & PRNG helpers (no global state, thread-safe)    */
/* ------------------------------------------------------------------ */

static inline unsigned curve_hash(unsigned seed, unsigned a, unsigned b)
{
    unsigned h = seed ^ (a * 0x85ebca6bu) ^ (b * 0xc2b2ae35u);
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static inline double curve_rand(unsigned seed, unsigned a, unsigned b)
{
    return (double)(curve_hash(seed, a, b) & 0x0FFFFFFFu) / (double)0x10000000;
}

static inline double curve_rand_range(unsigned seed, unsigned a, unsigned b,
                                      double min_val, double max_val)
{
    return min_val + (max_val - min_val) * curve_rand(seed, a, b);
}

/* ------------------------------------------------------------------ */
/* Curve constructors & modulation                                     */
/* ------------------------------------------------------------------ */

Curve3D curve_bezier(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, double r_start, double r_end)
{
    Curve3D c;
    c.p0 = p0;
    c.p1 = p1;
    c.p2 = p2;
    c.p3 = p3;
    c.radius_start = r_start;
    c.radius_end   = r_end;
    c.wobble_amp   = 0.0;
    c.wobble_freq  = 0.0;
    c.noise_amp    = 0.0;
    c.noise_freq   = 0.0;
    c.seed         = 0;
    return c;
}

void curve_set_wobble(Curve3D *c, double amplitude, double frequency)
{
    if (!c) return;
    c->wobble_amp  = amplitude;
    c->wobble_freq = frequency;
}

void curve_set_noise(Curve3D *c, double amplitude, double frequency, unsigned seed)
{
    if (!c) return;
    c->noise_amp  = amplitude;
    c->noise_freq = frequency;
    c->seed       = seed;
}

double curve_radius(const Curve3D *c, double t)
{
    if (!c) return 0.0;
    if (t <= 0.0) return c->radius_start;
    if (t >= 1.0) return c->radius_end;
    return c->radius_start * (1.0 - t) + c->radius_end * t;
}

Vec3 curve_eval(const Curve3D *c, double t)
{
    if (!c) return vec3(0.0, 0.0, 0.0);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    double u = 1.0 - t;
    double tt = t * t;
    double uu = u * u;
    double uuu = uu * u;
    double ttt = tt * t;

    /* Base cubic Bézier point */
    Vec3 p = vec3_add(
        vec3_add(vec3_scale(c->p0, uuu), vec3_scale(c->p1, 3.0 * uu * t)),
        vec3_add(vec3_scale(c->p2, 3.0 * u * tt), vec3_scale(c->p3, ttt))
    );

    /*
     * Boundary envelope E(t) = sin(pi * t) keeps the base (t=0) and
     * tip (t=1) pinned firmly to the caller's control points.
     */
    double envelope = sin(CURVE_PI * t);
    if (envelope < 0.0) envelope = 0.0;

    /* Sinusoidal harmonic wobble */
    if (c->wobble_amp != 0.0 && c->wobble_freq != 0.0 && envelope > 1e-6) {
        Vec3 dP = vec3_sub(c->p3, c->p0);
        if (vec3_length_sq(dP) < 1e-8) dP = vec3(0.0, 1.0, 0.0);
        Vec3 T = vec3_normalize(dP);
        Vec3 ref = (fabs(T.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        Vec3 U = vec3_normalize(vec3_cross(ref, T));
        Vec3 V = vec3_cross(T, U);

        double phase = 2.0 * CURVE_PI * c->wobble_freq * t;
        double wu = sin(phase) * c->wobble_amp * envelope;
        double wv = cos(phase * 1.37) * (c->wobble_amp * 0.7) * envelope;
        p = vec3_add(p, vec3_add(vec3_scale(U, wu), vec3_scale(V, wv)));
    }

    /* 3D Perlin noise displacement */
    if (c->noise_amp != 0.0 && c->noise_freq != 0.0 && envelope > 1e-6) {
        double s = c->noise_freq;
        double nx = noise_perlin3(p.x * s, p.y * s, p.z * s, c->seed);
        double ny = noise_perlin3(p.x * s + 17.1, p.y * s + 31.7, p.z * s + 43.3, c->seed + 101u);
        double nz = noise_perlin3(p.x * s + 59.3, p.y * s + 71.9, p.z * s + 83.1, c->seed + 202u);
        Vec3 ndisp = vec3(nx, ny, nz);
        p = vec3_add(p, vec3_scale(ndisp, c->noise_amp * envelope));
    }

    return p;
}

Vec3 curve_tangent(const Curve3D *c, double t)
{
    if (!c) return vec3(0.0, 1.0, 0.0);
    double dt = 1e-4;
    Vec3 p_prev, p_next;
    if (t <= dt) {
        p_prev = curve_eval(c, 0.0);
        p_next = curve_eval(c, dt);
    } else if (t >= 1.0 - dt) {
        p_prev = curve_eval(c, 1.0 - dt);
        p_next = curve_eval(c, 1.0);
    } else {
        p_prev = curve_eval(c, t - dt);
        p_next = curve_eval(c, t + dt);
    }

    Vec3 d = vec3_sub(p_next, p_prev);
    if (vec3_length_sq(d) < 1e-12) {
        d = vec3_sub(c->p3, c->p0);
        if (vec3_length_sq(d) < 1e-12) return vec3(0.0, 1.0, 0.0);
    }
    return vec3_normalize(d);
}

/* ------------------------------------------------------------------ */
/* Geometry generation: Tube & Bent Beam                              */
/* ------------------------------------------------------------------ */

int curve_build_tube(Geometry *g, const Curve3D *c, int segments, int material_index)
{
    if (!g || !c || segments < 1) return -1;

    for (int i = 0; i < segments; ++i) {
        double t0 = (double)i / (double)segments;
        double t1 = (double)(i + 1) / (double)segments;

        Vec3 p0 = curve_eval(c, t0);
        Vec3 p1 = curve_eval(c, t1);
        double r0 = curve_radius(c, t0);
        double r1 = curve_radius(c, t1);

        if (geometry_add(g, prim_cylinder(p0, p1, r0, r1, material_index)) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Helper to rotate a vector around a unit axis by angle theta */
static Vec3 rotate_vector_axis(Vec3 v, Vec3 axis, double cos_th, double sin_th)
{
    Vec3 cross_term = vec3_cross(axis, v);
    double dot_term = vec3_dot(axis, v);
    return vec3_add(
        vec3_scale(v, cos_th),
        vec3_add(vec3_scale(cross_term, sin_th),
                 vec3_scale(axis, dot_term * (1.0 - cos_th)))
    );
}

int curve_build_beam(Geometry *g, const Curve3D *c, double half_w, double half_h,
                     int segments, int material_index)
{
    if (!g || !c || segments < 1 || half_w <= 0.0 || half_h <= 0.0) return -1;

    int num_rings = segments + 1;
    Vec3 (*rings)[4] = malloc((size_t)num_rings * sizeof(*rings));
    if (!rings) return -1;

    /* Initialize Bishop / parallel-transport frame at t = 0 */
    Vec3 T = curve_tangent(c, 0.0);
    Vec3 ref = (fabs(T.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    Vec3 U = vec3_normalize(vec3_cross(ref, T));
    Vec3 V = vec3_cross(T, U);

    for (int i = 0; i < num_rings; ++i) {
        double t = (double)i / (double)segments;
        Vec3 p = curve_eval(c, t);

        if (i > 0) {
            Vec3 next_T = curve_tangent(c, t);
            Vec3 rot_axis = vec3_cross(T, next_T);
            double rot_sin = vec3_length(rot_axis);
            if (rot_sin > 1e-7) {
                rot_axis = vec3_scale(rot_axis, 1.0 / rot_sin);
                double rot_cos = vec3_dot(T, next_T);
                if (rot_cos > 1.0) rot_cos = 1.0;
                if (rot_cos < -1.0) rot_cos = -1.0;
                U = rotate_vector_axis(U, rot_axis, rot_cos, rot_sin);
                /* Gram-Schmidt re-orthogonalization */
                U = vec3_sub(U, vec3_scale(next_T, vec3_dot(next_T, U)));
                U = vec3_normalize(U);
                V = vec3_cross(next_T, U);
            }
            T = next_T;
        }

        /* 4 corners of cross-section ring */
        rings[i][0] = vec3_sub(vec3_sub(p, vec3_scale(U, half_w)), vec3_scale(V, half_h));
        rings[i][1] = vec3_sub(vec3_add(p, vec3_scale(U, half_w)), vec3_scale(V, half_h));
        rings[i][2] = vec3_add(vec3_add(p, vec3_scale(U, half_w)), vec3_scale(V, half_h));
        rings[i][3] = vec3_add(vec3_sub(p, vec3_scale(U, half_w)), vec3_scale(V, half_h));
    }

    /* Emit side quads (2 triangles each) */
    for (int i = 0; i < segments; ++i) {
        for (int k = 0; k < 4; ++k) {
            int next_k = (k + 1) % 4;
            Vec3 v00 = rings[i][k];
            Vec3 v10 = rings[i][next_k];
            Vec3 v11 = rings[i + 1][next_k];
            Vec3 v01 = rings[i + 1][k];

            if (geometry_add(g, prim_triangle(v00, v10, v11, material_index)) < 0 ||
                geometry_add(g, prim_triangle(v00, v11, v01, material_index)) < 0) {
                free(rings);
                return -1;
            }
        }
    }

    /* Bottom cap at i = 0 (outward normal points in -T direction) */
    if (geometry_add(g, prim_triangle(rings[0][0], rings[0][3], rings[0][2], material_index)) < 0 ||
        geometry_add(g, prim_triangle(rings[0][0], rings[0][2], rings[0][1], material_index)) < 0) {
        free(rings);
        return -1;
    }

    /* Top cap at i = segments (outward normal points in +T direction) */
    int top_idx = segments;
    if (geometry_add(g, prim_triangle(rings[top_idx][0], rings[top_idx][1], rings[top_idx][2], material_index)) < 0 ||
        geometry_add(g, prim_triangle(rings[top_idx][0], rings[top_idx][2], rings[top_idx][3], material_index)) < 0) {
        free(rings);
        return -1;
    }

    free(rings);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fairytale Tree (Sagoträd) Generator                                */
/* ------------------------------------------------------------------ */

void fairytale_tree_params_default(FairytaleTreeParams *p)
{
    if (!p) return;
    p->bough_count      = 5;
    p->bough_min_t      = 0.35;
    p->bough_max_t      = 0.95;
    p->branch_max_depth = 3;
    p->branch_taper     = 0.65;
    p->branch_len_decay = 0.70;
    p->branch_spread    = 0.62;
    p->curl_strength    = 0.35;
    p->leaf_min         = 14;
    p->leaf_span        = 8;
    p->leaf_scale       = 1.0;
}

static int add_fairytale_leaf_cluster(Geometry *g, int leaf_mat, Vec3 tip,
                                      double branch_len, int depth, unsigned seed,
                                      const FairytaleTreeParams *params)
{
    unsigned d = (unsigned)depth;
    int count = params->leaf_min
              + (int)(curve_rand(seed, d, 0xBEEFu) * (double)params->leaf_span);
    double cluster_r = (branch_len * 1.2 + 0.8) * params->leaf_scale;
    double leaf_scale = params->leaf_scale;

    if (count < 1) count = 1;
    if (cluster_r < 0.6) cluster_r = 0.6;
    if (cluster_r > 3.0) cluster_r = 3.0;

    for (int i = 0; i < count; ++i) {
        unsigned k = (unsigned)i;
        double ox = curve_rand_range(seed, d, 100u + k, -1.0, 1.0);
        double oy = curve_rand_range(seed, d, 200u + k, -0.8, 1.2);
        double oz = curve_rand_range(seed, d, 300u + k, -1.0, 1.0);
        Vec3 off = vec3(ox * cluster_r, oy * cluster_r * 0.9, oz * cluster_r);
        double r = (0.4 + curve_rand(seed, d, 400u + k) * 0.6) * leaf_scale;

        if (geometry_add(g, prim_sphere(vec3_add(tip, off), r, leaf_mat)) < 0)
            return -1;
    }
    return 0;
}

static int grow_fairytale_branch(Geometry *g, int bark_mat, int leaf_mat,
                                 Vec3 base, Vec3 dir, double length, double radius,
                                 int depth, unsigned seed,
                                 const FairytaleTreeParams *params)
{
    unsigned d = (unsigned)depth;
    Vec3 axis = vec3_normalize(dir);
    double r_bottom = radius;
    double r_top = radius * params->branch_taper;

    if (vec3_length_sq(axis) < 1e-12) {
        axis = vec3(0.0, 1.0, 0.0);
    }

    /* Organic 3D curl displacement */
    Vec3 curl_off = vec3(
        noise_perlin3(base.x * 0.8, base.y * 0.8, base.z * 0.8, seed + d * 31u),
        noise_perlin3(base.x * 0.8 + 10.0, base.y * 0.8 + 20.0, base.z * 0.8 + 30.0, seed + d * 47u),
        noise_perlin3(base.x * 0.8 + 50.0, base.y * 0.8 + 60.0, base.z * 0.8 + 70.0, seed + d * 61u)
    );

    Vec3 mid = vec3_add(base, vec3_scale(axis, length * 0.5));
    mid = vec3_add(mid, vec3_scale(curl_off, length * params->curl_strength * 0.6));

    Vec3 tip = vec3_add(base, vec3_scale(axis, length));
    tip = vec3_add(tip, vec3_scale(curl_off, length * params->curl_strength * 0.4));

    /* 2 segments per branch for a gnarled, crooked fairytale look */
    double r_mid = (r_bottom + r_top) * 0.5;
    if (geometry_add(g, prim_cylinder(base, mid, r_bottom, r_mid, bark_mat)) < 0)
        return -1;
    if (geometry_add(g, prim_cylinder(mid, tip, r_mid, r_top, bark_mat)) < 0)
        return -1;

    /* Terminal check: max depth reached or branch radius too small */
    if (depth >= params->branch_max_depth || r_top < 0.03) {
        return add_fairytale_leaf_cluster(g, leaf_mat, tip, length, depth, seed, params);
    }

    /* Reference frame around branch direction */
    Vec3 eff_axis = vec3_normalize(vec3_sub(tip, mid));
    if (vec3_length_sq(eff_axis) < 1e-12) eff_axis = axis;

    Vec3 ref = (fabs(eff_axis.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    Vec3 u = vec3_normalize(vec3_cross(ref, eff_axis));
    Vec3 v = vec3_cross(eff_axis, u);

    int children = (curve_rand(seed, d, 0xABC1u) < 0.45) ? 3 : 2;

    for (int i = 0; i < children; ++i) {
        unsigned k = (unsigned)i;
        double phi = 2.0 * CURVE_PI * (double)i / (double)children
                   + curve_rand_range(seed, d, 100u + k, -0.3, 0.3);
        double spread = params->branch_spread
                   + curve_rand_range(seed, d, 300u + k, -0.15, 0.15);
        double len_jit = 1.0 + curve_rand_range(seed, d, 500u + k, -0.15, 0.15);

        Vec3 child_dir = vec3_normalize(vec3_add(
            vec3_scale(eff_axis, cos(spread)),
            vec3_scale(vec3_add(vec3_scale(u, cos(phi)),
                                vec3_scale(v, sin(phi))), sin(spread))));

        /* Upward bias keeps branches growing naturally towards the canopy */
        child_dir = vec3_normalize(vec3_add(child_dir, vec3(0.0, 0.25, 0.0)));

        unsigned child_seed = curve_hash(seed, d, k + 1u);
        int rc = grow_fairytale_branch(g, bark_mat, leaf_mat, tip, child_dir,
                                       length * params->branch_len_decay * len_jit,
                                       r_top, depth + 1, child_seed, params);
        if (rc != 0) return rc;
    }

    return 0;
}

int curve_grow_fairytale_tree(Geometry *g, const Curve3D *trunk_curve,
                             int bark_mat, int leaf_mat, unsigned seed,
                             const FairytaleTreeParams *params)
{
    FairytaleTreeParams default_params;
    if (!params) {
        fairytale_tree_params_default(&default_params);
        params = &default_params;
    }
    if (!g || !trunk_curve) return -1;

    /* 1. Build gnarled trunk following the curve */
    int trunk_segments = 16;
    if (curve_build_tube(g, trunk_curve, trunk_segments, bark_mat) != 0)
        return -1;

    /* Approximate trunk length */
    double trunk_len = vec3_length(vec3_sub(trunk_curve->p3, trunk_curve->p0));
    if (trunk_len < 1e-4) trunk_len = 1.0;

    /* 2. Spawn major boughs along the trunk */
    int boughs = params->bough_count;
    double t_min = params->bough_min_t;
    double t_max = params->bough_max_t;

    for (int b = 0; b < boughs; ++b) {
        double frac = (boughs > 1) ? (double)b / (double)(boughs - 1) : 0.5;
        double t = t_min + frac * (t_max - t_min);
        Vec3 pos = curve_eval(trunk_curve, t);
        Vec3 tan = curve_tangent(trunk_curve, t);
        double rad = curve_radius(trunk_curve, t);

        /* Outward angle: golden spiral angle ~ 137.5 deg for even canopy distribution */
        double phi = (double)b * 2.399963229728653 + curve_rand_range(seed, (unsigned)b, 11u, -0.2, 0.2);

        Vec3 ref = (fabs(tan.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        Vec3 u = vec3_normalize(vec3_cross(ref, tan));
        Vec3 v = vec3_cross(tan, u);
        Vec3 radial = vec3_normalize(vec3_add(vec3_scale(u, cos(phi)), vec3_scale(v, sin(phi))));

        /* Bough direction combines outward radial vector with upward angle */
        double elev = 0.55 + curve_rand_range(seed, (unsigned)b, 22u, -0.15, 0.15);
        Vec3 bough_dir = vec3_normalize(vec3_add(vec3_scale(radial, cos(elev)), vec3(0.0, sin(elev), 0.0)));

        /* Bough length scales with tree size and tapers higher up */
        double bough_len = trunk_len * (0.35 + 0.15 * (1.0 - t)) * (0.85 + curve_rand(seed, (unsigned)b, 33u) * 0.3);
        double bough_radius = rad * 0.55;

        unsigned bough_seed = curve_hash(seed, (unsigned)b, 44u);
        int rc = grow_fairytale_branch(g, bark_mat, leaf_mat, pos, bough_dir,
                                       bough_len, bough_radius, 1, bough_seed, params);
        if (rc != 0) return rc;
    }

    /* 3. Crown cluster at the very tip of the trunk */
    Vec3 tip_pos = curve_eval(trunk_curve, 1.0);
    Vec3 tip_tan = curve_tangent(trunk_curve, 1.0);
    double tip_rad = curve_radius(trunk_curve, 1.0);
    double crown_len = trunk_len * 0.25;

    int rc = grow_fairytale_branch(g, bark_mat, leaf_mat, tip_pos, tip_tan,
                                   crown_len, tip_rad, 1, curve_hash(seed, 999u, 1u), params);
    if (rc != 0) return rc;

    return 0;
}
