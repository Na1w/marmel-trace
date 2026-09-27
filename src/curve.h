#ifndef RAYTRACER_CURVE_H
#define RAYTRACER_CURVE_H

/*
 * curve.h - 3D Parametric Curves, Bending, and Fairytale Tree Generation.
 *
 * This module implements:
 *   - 3D cubic Bézier curves with procedural harmonic wobble and 3D noise
 *     displacement for organic, gnarled shapes.
 *   - Smooth curved cylinder tubes (pipes / vines / bent trunks).
 *   - Extruded curved beams (bent/twisted rectangular boxes).
 *   - Procedural fairytale tree generation (sagoträd) whose gnarled, twisted
 *     trunk follows a 3D curve with gnarly boughs, sub-branches and leaf clusters.
 *
 * Depends only on vec3.h, geometry.h, and noise.h. C11, warning-free.
 */

#include "geometry.h"
#include "vec3.h"

/*
 * 3D parametric Bézier curve with radius profile and organic modulation.
 */
typedef struct {
    Vec3     p0, p1, p2, p3;   /* 4 cubic control points in world space */
    double   radius_start;     /* trunk/tube radius at t = 0 */
    double   radius_end;       /* trunk/tube radius at t = 1 */
    double   wobble_amp;       /* amplitude of wave wobble (0 = none) */
    double   wobble_freq;      /* frequency of wave wobble */
    double   noise_amp;        /* amplitude of 3D noise perturbation */
    double   noise_freq;       /* frequency of 3D noise */
    unsigned seed;             /* procedural seed for noise/jitter */
} Curve3D;

/* Construct a smooth Bézier curve with linear radius taper */
Curve3D curve_bezier(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, double r_start, double r_end);

/* Add sinusoidal wobble modulation along the curve */
void curve_set_wobble(Curve3D *c, double amplitude, double frequency);

/* Add 3D organic noise displacement along the curve */
void curve_set_noise(Curve3D *c, double amplitude, double frequency, unsigned seed);

/* Evaluate position on curve at t in [0.0, 1.0] (includes wobble & noise) */
Vec3 curve_eval(const Curve3D *c, double t);

/* Evaluate unit tangent on curve at t in [0.0, 1.0] */
Vec3 curve_tangent(const Curve3D *c, double t);

/* Evaluate radius on curve at t in [0.0, 1.0] */
double curve_radius(const Curve3D *c, double t);

/* ------------------------------------------------------------------ */
/* Geometry generation along curves                                   */
/* ------------------------------------------------------------------ */

/*
 * Build a smooth curved cylinder (tube / vine) along `c` into `g`.
 * Discretized into `segments` seamlessly connected tapered cylinders.
 * Returns 0 on success, non-zero on failure.
 */
int curve_build_tube(Geometry *g, const Curve3D *c, int segments, int material_index);

/*
 * Build a curved rectangular beam (bent box) along `c` into `g`.
 * Extrudes a rectangle of half-dimensions (half_w, half_h) along the curve's
 * parallel-transport frame, generating (segments * 8 + 4) triangles.
 * Returns 0 on success, non-zero on failure.
 */
int curve_build_beam(Geometry *g, const Curve3D *c, double half_w, double half_h,
                     int segments, int material_index);

/* ------------------------------------------------------------------ */
/* Fairytale Tree (Sagoträd) Generator                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int    bough_count;        /* number of major boughs along trunk (e.g. 5) */
    double bough_min_t;        /* start parameter on trunk for boughs (0.35) */
    double bough_max_t;        /* end parameter on trunk for boughs (0.95) */
    int    branch_max_depth;   /* recursion depth for bough splitting (e.g. 3) */
    double branch_taper;       /* radius taper factor per branch (e.g. 0.65) */
    double branch_len_decay;   /* length decay factor per branch (e.g. 0.70) */
    double branch_spread;      /* angle spread in radians (e.g. 0.60 ~ 34 deg) */
    double curl_strength;      /* organic curl / twist strength */
    int    leaf_min;           /* minimum leaf spheres per tip (e.g. 16) */
    int    leaf_span;          /* leaf sphere count variation span (e.g. 8) */
    double leaf_scale;         /* size multiplier for leaf clusters */
} FairytaleTreeParams;

/* Populate default parameters for a lush, gnarled fairytale tree */
void fairytale_tree_params_default(FairytaleTreeParams *p);

/*
 * Grow a fairytale tree (sagoträd) into `g`:
 *   - The main gnarled trunk follows `trunk_curve` from t=0 to t=1.
 *   - Major gnarled boughs branch outward from the curve, recursively forking
 *     and twisting into organic canopy branches.
 *   - Branch tips blossom into lush leaf sphere clusters.
 *
 * Returns 0 on success, non-zero on failure.
 */
int curve_grow_fairytale_tree(Geometry *g, const Curve3D *trunk_curve,
                             int bark_mat, int leaf_mat, unsigned seed,
                             const FairytaleTreeParams *params);

#endif /* RAYTRACER_CURVE_H */
