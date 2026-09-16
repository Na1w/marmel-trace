#ifndef RAYTRACER_VEC3_H
#define RAYTRACER_VEC3_H

/*
 * vec3.h - Canonical vector / ray math interface for the raytracer.
 *
 * Double precision. Header-only for trivial operations (static inline),
 * with the slightly heavier routines implemented in vec3.c.
 *
 * Conventions:
 *   - Ray.dir is assumed normalized, so the ray parameter t is a distance.
 *   - Functions never allocate, read input, or touch global state.
 */

#include <math.h>

typedef struct {
    double x, y, z;
} Vec3;

typedef struct {
    Vec3 origin;
    Vec3 dir; /* assumed normalized */
} Ray;

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static inline Vec3 vec3(double x, double y, double z)
{
    Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

/* ------------------------------------------------------------------ */
/* Basic arithmetic                                                    */
/* ------------------------------------------------------------------ */

static inline Vec3 vec3_add(Vec3 a, Vec3 b)
{
    return vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static inline Vec3 vec3_sub(Vec3 a, Vec3 b)
{
    return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static inline Vec3 vec3_neg(Vec3 a)
{
    return vec3(-a.x, -a.y, -a.z);
}

static inline Vec3 vec3_scale(Vec3 a, double s)
{
    return vec3(a.x * s, a.y * s, a.z * s);
}

static inline Vec3 vec3_mul(Vec3 a, Vec3 b)
{
    return vec3(a.x * b.x, a.y * b.y, a.z * b.z);
}

/* ------------------------------------------------------------------ */
/* Products and norms                                                  */
/* ------------------------------------------------------------------ */

static inline double vec3_dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline Vec3 vec3_cross(Vec3 a, Vec3 b)
{
    return vec3(a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x);
}

static inline double vec3_length_sq(Vec3 a)
{
    return vec3_dot(a, a);
}

/* ------------------------------------------------------------------ */
/* Interpolation / geometry helpers                                    */
/* ------------------------------------------------------------------ */

static inline Vec3 vec3_lerp(Vec3 a, Vec3 b, double t)
{
    return vec3(a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t);
}

/* Reflect incident vector i about normal n (n assumed normalized):
 *   i - 2*(i.n)*n
 */
static inline Vec3 vec3_reflect(Vec3 i, Vec3 n)
{
    double d = vec3_dot(i, n);
    return vec3(i.x - 2.0 * d * n.x,
                i.y - 2.0 * d * n.y,
                i.z - 2.0 * d * n.z);
}

/* Point on ray at parameter t: r.origin + t*r.dir */
static inline Vec3 vec3_at(Ray r, double t)
{
    return vec3(r.origin.x + t * r.dir.x,
                r.origin.y + t * r.dir.y,
                r.origin.z + t * r.dir.z);
}

/* Component-wise min/max (handy for clamping colors). */
static inline Vec3 vec3_min(Vec3 a, Vec3 b)
{
    return vec3(a.x < b.x ? a.x : b.x,
                a.y < b.y ? a.y : b.y,
                a.z < b.z ? a.z : b.z);
}

static inline Vec3 vec3_max(Vec3 a, Vec3 b)
{
    return vec3(a.x > b.x ? a.x : b.x,
                a.y > b.y ? a.y : b.y,
                a.z > b.z ? a.z : b.z);
}

/* ------------------------------------------------------------------ */
/* Implemented in vec3.c                                               */
/* ------------------------------------------------------------------ */

Vec3 vec3_div(Vec3 a, double s);               /* scalar divide; zero if s == 0 */
double vec3_length(Vec3 a);
Vec3 vec3_normalize(Vec3 a);                   /* zero vector if length ~ 0 */
Vec3 vec3_refract(Vec3 i, Vec3 n, double eta); /* zero vector on total internal reflection */

#endif /* RAYTRACER_VEC3_H */
