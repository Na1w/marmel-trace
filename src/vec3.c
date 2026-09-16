/*
 * vec3.c - Non-trivial vector routines.
 *
 * The trivial operations live as static inline functions in vec3.h;
 * everything that needs a branch or a sqrt lives here.
 */

#include "vec3.h"

#define VEC3_EPS 1e-12

Vec3 vec3_div(Vec3 a, double s)
{
    if (s == 0.0) {
        /* Guard: division by zero is never a meaningful vector op here. */
        return vec3(0.0, 0.0, 0.0);
    }
    return vec3(a.x / s, a.y / s, a.z / s);
}

double vec3_length(Vec3 a)
{
    return sqrt(vec3_length_sq(a));
}

Vec3 vec3_normalize(Vec3 a)
{
    double len_sq = vec3_length_sq(a);
    if (len_sq <= VEC3_EPS) {
        /* Degenerate / near-zero vector: return the zero vector. */
        return vec3(0.0, 0.0, 0.0);
    }
    return vec3_scale(a, 1.0 / sqrt(len_sq));
}

/*
 * Snell's law refraction.
 *
 *   i   - incident unit vector (pointing toward the surface)
 *   n   - surface unit normal, pointing against i (same hemisphere as -i)
 *   eta - ratio of indices of refraction n_from / n_to
 *
 * Returns the zero vector on total internal reflection.
 *
 *   cos_i = -dot(i, n)
 *   sin^2_t = eta^2 * (1 - cos^2_i)
 *   if sin^2_t > 1  -> total internal reflection
 *   cos_t = sqrt(1 - sin^2_t)
 *   t = eta * i + (eta * cos_i - cos_t) * n
 */
Vec3 vec3_refract(Vec3 i, Vec3 n, double eta)
{
    double cos_i = -vec3_dot(i, n);
    double sin2_t = eta * eta * (1.0 - cos_i * cos_i);

    if (sin2_t > 1.0) {
        return vec3(0.0, 0.0, 0.0); /* total internal reflection */
    }

    double cos_t = sqrt(1.0 - sin2_t);
    double k = eta * cos_i - cos_t;

    return vec3(eta * i.x + k * n.x,
                eta * i.y + k * n.y,
                eta * i.z + k * n.z);
}
