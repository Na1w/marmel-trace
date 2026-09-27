/*
 * sdf.c - Signed Distance Fields, Domain Warping / Displacement, and CSG.
 *
 * Implements sphere-tracing for analytical shapes modulated by displacement
 * fields (sine, noise, twist, repel) and combined via CSG booleans.
 *
 * Warning-free under -Wall -Wextra, deterministic, C11 standard library only.
 */

#include "sdf.h"
#include "noise.h"
#include <math.h>

#define SDF_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* Analytical Distance Primitives                                     */
/* ------------------------------------------------------------------ */

static double sdf_primitive(SdfShapeKind kind, Vec3 p, Vec3 center, Vec3 param1, double param2)
{
    Vec3 q = vec3_sub(p, center);

    switch (kind) {
    case SDF_SHAPE_BOX: {
        /* param1 = half extents (hx, hy, hz) */
        Vec3 d = vec3(fabs(q.x) - param1.x, fabs(q.y) - param1.y, fabs(q.z) - param1.z);
        double out_x = (d.x > 0.0) ? d.x : 0.0;
        double out_y = (d.y > 0.0) ? d.y : 0.0;
        double out_z = (d.z > 0.0) ? d.z : 0.0;
        double out_dist = sqrt(out_x * out_x + out_y * out_y + out_z * out_z);

        double max_d = d.x;
        if (d.y > max_d) max_d = d.y;
        if (d.z > max_d) max_d = d.z;
        double in_dist = (max_d < 0.0) ? max_d : 0.0;

        return out_dist + in_dist;
    }
    case SDF_SHAPE_SPHERE: {
        /* param2 = radius */
        return vec3_length(q) - param2;
    }
    case SDF_SHAPE_CYLINDER: {
        /* Y-aligned cylinder: param2 = radius, param1.y = half_height */
        double d_rad = sqrt(q.x * q.x + q.z * q.z) - param2;
        double d_y = fabs(q.y) - param1.y;

        double out_r = (d_rad > 0.0) ? d_rad : 0.0;
        double out_y = (d_y > 0.0) ? d_y : 0.0;
        double out_dist = sqrt(out_r * out_r + out_y * out_y);

        double max_d = (d_rad > d_y) ? d_rad : d_y;
        double in_dist = (max_d < 0.0) ? max_d : 0.0;

        return out_dist + in_dist;
    }
    }
    return 1e6;
}

/* ------------------------------------------------------------------ */
/* Domain Warping / Modulators                                        */
/* ------------------------------------------------------------------ */

static Vec3 sdf_apply_displace(Vec3 p, const DisplaceModifier *mod, Vec3 center)
{
    if (!mod || mod->kind == DISPLACE_NONE) {
        return p;
    }

    switch (mod->kind) {
    case DISPLACE_SINE: {
        double w = 2.0 * SDF_PI * mod->frequency;
        double s = sin(w * (p.y - center.y) + mod->phase) * mod->amplitude;
        Vec3 dir = mod->direction;
        if (vec3_length_sq(dir) < 1e-6) dir = vec3(1.0, 0.0, 0.0);
        else dir = vec3_normalize(dir);
        return vec3_sub(p, vec3_scale(dir, s));
    }
    case DISPLACE_TWIST: {
        Vec3 rel = vec3_sub(p, center);
        double angle = -rel.y * mod->strength;
        double cos_a = cos(angle);
        double sin_a = sin(angle);
        double rx = rel.x * cos_a - rel.z * sin_a;
        double rz = rel.x * sin_a + rel.z * cos_a;
        return vec3(center.x + rx, p.y, center.z + rz);
    }
    case DISPLACE_NOISE: {
        double f = mod->frequency;
        double nx = noise_perlin3(p.x * f, p.y * f, p.z * f, mod->seed);
        double ny = noise_perlin3(p.x * f + 17.1, p.y * f + 31.7, p.z * f + 43.3, mod->seed + 101u);
        double nz = noise_perlin3(p.x * f + 59.3, p.y * f + 71.9, p.z * f + 83.1, mod->seed + 202u);
        return vec3_sub(p, vec3_scale(vec3(nx, ny, nz), mod->amplitude));
    }
    case DISPLACE_REPEL: {
        Vec3 d = vec3_sub(p, mod->center);
        double r = vec3_length(d);
        if (r < mod->radius && r > 1e-6) {
            double factor = 1.0 - r / mod->radius;
            double disp = factor * factor * mod->strength;
            return vec3_add(p, vec3_scale(vec3_normalize(d), disp));
        }
        return p;
    }
    default:
        return p;
    }
}

/* ------------------------------------------------------------------ */
/* Combined SDF Evaluation & Normal                                   */
/* ------------------------------------------------------------------ */

double sdf_eval(const SdfData *sdf, Vec3 p)
{
    Vec3 warped_p = p;
    if (sdf->displace_count > 0) {
        for (int i = 0; i < sdf->displace_count; i++) {
            warped_p = sdf_apply_displace(warped_p, &sdf->displaces[i], sdf->center_a);
        }
    } else if (sdf->displace.kind != DISPLACE_NONE) {
        warped_p = sdf_apply_displace(p, &sdf->displace, sdf->center_a);
    }
    double da = sdf_primitive(sdf->shape_a, warped_p, sdf->center_a, sdf->param1_a, sdf->param2_a);

    if (sdf->op == SDF_OP_NONE) {
        return da;
    }

    double db = sdf_primitive(sdf->shape_b, warped_p, sdf->center_b, sdf->param1_b, sdf->param2_b);

    switch (sdf->op) {
    case SDF_OP_UNION:
        return (da < db) ? da : db;
    case SDF_OP_DIFFERENCE:
        return (da > -db) ? da : -db;
    case SDF_OP_INTERSECTION:
        return (da > db) ? da : db;
    default:
        return da;
    }
}

Vec3 sdf_normal(const SdfData *sdf, Vec3 p)
{
    const double h = 1e-4;
    double dx = sdf_eval(sdf, vec3(p.x + h, p.y, p.z)) - sdf_eval(sdf, vec3(p.x - h, p.y, p.z));
    double dy = sdf_eval(sdf, vec3(p.x, p.y + h, p.z)) - sdf_eval(sdf, vec3(p.x, p.y - h, p.z));
    double dz = sdf_eval(sdf, vec3(p.x, p.y, p.z + h)) - sdf_eval(sdf, vec3(p.x, p.y, p.z - h));

    Vec3 n = vec3(dx, dy, dz);
    double len = vec3_length(n);
    if (len > 1e-12) {
        return vec3_scale(n, 1.0 / len);
    }
    return vec3(0.0, 1.0, 0.0);
}

/* ------------------------------------------------------------------ */
/* Conservative Bounding Box Calculation                              */
/* ------------------------------------------------------------------ */

static void get_primitive_bounds(SdfShapeKind kind, Vec3 center, Vec3 param1, double param2,
                                 Vec3 *bmin, Vec3 *bmax)
{
    *bmin = center;
    *bmax = center;
    switch (kind) {
    case SDF_SHAPE_BOX:
        *bmin = vec3_sub(center, param1);
        *bmax = vec3_add(center, param1);
        break;
    case SDF_SHAPE_SPHERE:
        *bmin = vec3_sub(center, vec3(param2, param2, param2));
        *bmax = vec3_add(center, vec3(param2, param2, param2));
        break;
    case SDF_SHAPE_CYLINDER:
        *bmin = vec3_sub(center, vec3(param2, param1.y, param2));
        *bmax = vec3_add(center, vec3(param2, param1.y, param2));
        break;
    default:
        break;
    }
}

void sdf_bounds(const SdfData *sdf, Vec3 *out_min, Vec3 *out_max)
{
    Vec3 min_a, max_a;
    get_primitive_bounds(sdf->shape_a, sdf->center_a, sdf->param1_a, sdf->param2_a, &min_a, &max_a);

    if (sdf->op == SDF_OP_UNION) {
        Vec3 min_b, max_b;
        get_primitive_bounds(sdf->shape_b, sdf->center_b, sdf->param1_b, sdf->param2_b, &min_b, &max_b);
        min_a.x = fmin(min_a.x, min_b.x);
        min_a.y = fmin(min_a.y, min_b.y);
        min_a.z = fmin(min_a.z, min_b.z);
        max_a.x = fmax(max_a.x, max_b.x);
        max_a.y = fmax(max_a.y, max_b.y);
        max_a.z = fmax(max_a.z, max_b.z);
    }

    /* Expand for displacement */
    double disp_pad = 0.05;
    int num_mods = sdf->displace_count;
    const DisplaceModifier *mods = sdf->displaces;
    DisplaceModifier single_mod;
    if (num_mods == 0 && sdf->displace.kind != DISPLACE_NONE) {
        single_mod = sdf->displace;
        mods = &single_mod;
        num_mods = 1;
    }

    for (int i = 0; i < num_mods; i++) {
        const DisplaceModifier *mod = &mods[i];
        if (mod->kind == DISPLACE_SINE) {
            disp_pad += mod->amplitude;
        } else if (mod->kind == DISPLACE_NOISE) {
            disp_pad += mod->amplitude * 1.74;
        } else if (mod->kind == DISPLACE_REPEL) {
            disp_pad += mod->strength;
        } else if (mod->kind == DISPLACE_TWIST) {
            /* In twist around Y, corners sweep out a circular cylinder */
            double hx = fmax(fabs(min_a.x - sdf->center_a.x), fabs(max_a.x - sdf->center_a.x));
            double hz = fmax(fabs(min_a.z - sdf->center_a.z), fabs(max_a.z - sdf->center_a.z));
            double max_r = sqrt(hx * hx + hz * hz);
            min_a.x = sdf->center_a.x - max_r;
            max_a.x = sdf->center_a.x + max_r;
            min_a.z = sdf->center_a.z - max_r;
            max_a.z = sdf->center_a.z + max_r;
        }
    }

    out_min->x = min_a.x - disp_pad;
    out_min->y = min_a.y - disp_pad;
    out_min->z = min_a.z - disp_pad;
    out_max->x = max_a.x + disp_pad;
    out_max->y = max_a.y + disp_pad;
    out_max->z = max_a.z + disp_pad;
}

/* ------------------------------------------------------------------ */
/* Ray Intersection via Sphere Tracing                                */
/* ------------------------------------------------------------------ */

static int ray_box_intersect(Ray ray, Vec3 bmin, Vec3 bmax, double *t_enter, double *t_exit)
{
    double t0 = -1e30, t1 = 1e30;

    /* X slab */
    if (fabs(ray.dir.x) > 1e-12) {
        double inv_d = 1.0 / ray.dir.x;
        double ta = (bmin.x - ray.origin.x) * inv_d;
        double tb = (bmax.x - ray.origin.x) * inv_d;
        if (ta > tb) { double tmp = ta; ta = tb; tb = tmp; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return 0;
    } else {
        if (ray.origin.x < bmin.x || ray.origin.x > bmax.x) return 0;
    }

    /* Y slab */
    if (fabs(ray.dir.y) > 1e-12) {
        double inv_d = 1.0 / ray.dir.y;
        double ta = (bmin.y - ray.origin.y) * inv_d;
        double tb = (bmax.y - ray.origin.y) * inv_d;
        if (ta > tb) { double tmp = ta; ta = tb; tb = tmp; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return 0;
    } else {
        if (ray.origin.y < bmin.y || ray.origin.y > bmax.y) return 0;
    }

    /* Z slab */
    if (fabs(ray.dir.z) > 1e-12) {
        double inv_d = 1.0 / ray.dir.z;
        double ta = (bmin.z - ray.origin.z) * inv_d;
        double tb = (bmax.z - ray.origin.z) * inv_d;
        if (ta > tb) { double tmp = ta; ta = tb; tb = tmp; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return 0;
    } else {
        if (ray.origin.z < bmin.z || ray.origin.z > bmax.z) return 0;
    }

    *t_enter = t0;
    *t_exit  = t1;
    return 1;
}

int sdf_intersect(const SdfData *sdf, Ray ray, double tmin, double tmax,
                  double *out_t, Vec3 *out_point, Vec3 *out_normal)
{
    Vec3 bmin, bmax;
    sdf_bounds(sdf, &bmin, &bmax);

    double t_enter, t_exit;
    if (!ray_box_intersect(ray, bmin, bmax, &t_enter, &t_exit)) {
        return 0;
    }

    if (t_exit < tmin || t_enter > tmax) {
        return 0;
    }

    double t = (t_enter > tmin) ? t_enter : tmin;
    double t_end = (t_exit < tmax) ? t_exit : tmax;
    const double eps = 1e-4;
    const int max_steps = 96;

    for (int step = 0; step < max_steps && t <= t_end; ++step) {
        Vec3 p = vec3_add(ray.origin, vec3_scale(ray.dir, t));
        double d = sdf_eval(sdf, p);

        if (d < eps) {
            *out_t = t;
            *out_point = p;
            *out_normal = sdf_normal(sdf, p);
            return 1;
        }

        /* 0.6 step relaxation factor ensures convergence without overstepping in warped space */
        double step_size = d * 0.6;
        if (step_size < 1e-3) step_size = 1e-3;
        t += step_size;
    }

    return 0;
}

int sdf_occluded(const SdfData *sdf, Ray ray, double tmin, double tmax)
{
    Vec3 bmin, bmax;
    sdf_bounds(sdf, &bmin, &bmax);

    double t_enter, t_exit;
    if (!ray_box_intersect(ray, bmin, bmax, &t_enter, &t_exit)) {
        return 0;
    }

    if (t_exit < tmin || t_enter > tmax) {
        return 0;
    }

    double t = (t_enter > tmin) ? t_enter : tmin;
    double t_end = (t_exit < tmax) ? t_exit : tmax;
    const double eps = 1e-4;
    const int max_steps = 96;

    for (int step = 0; step < max_steps && t <= t_end; ++step) {
        Vec3 p = vec3_add(ray.origin, vec3_scale(ray.dir, t));
        double d = sdf_eval(sdf, p);

        if (d < eps) {
            return 1;
        }

        double step_size = d * 0.6;
        if (step_size < 1e-3) step_size = 1e-3;
        t += step_size;
    }

    return 0;
}
