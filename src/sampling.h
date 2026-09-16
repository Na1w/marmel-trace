#ifndef SAMPLING_H
#define SAMPLING_H

/*
 * sampling.h - Pure Monte-Carlo sampling / reflectance primitives for an
 *              unbiased unidirectional path tracer.
 *
 * This is a FOUNDATION module: it is intentionally self-contained and depends
 * ONLY on src/vec3.h (vector math) and <math.h>. It must NOT include
 * material.h / scene.h / render.h. The tiny Schlick / GGX formulas are
 * re-implemented locally (mirroring the semantics in src/material.c) so this
 * module stays standalone and linkable in isolation.
 *
 * Every function is PURE: deterministic in its arguments, no globals, no I/O,
 * no allocation, no hidden state. Uniforms u1/u2 are expected in [0, 1); they
 * are clamped defensively (out-of-range and NaN inputs are made safe).
 *
 * Direction conventions:
 *   - All returned directions are UNIT length.
 *   - A "hemisphere around n" means the half-space {d : dot(d, n) >= 0} with
 *     n treated as a UNIT normal (non-unit n is normalized internally).
 */

#include "vec3.h"

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Shared constants                                                    */
/* ------------------------------------------------------------------ */

/*
 * Local PI (mirrors RENDER_PI / MATERIAL_PI; defined locally because vec3.h
 * deliberately exposes no shared PI and M_PI is POSIX-only). Value is the
 * IEEE-754 double closest to pi.
 */
#define SAMPLING_PI 3.14159265358979323846

/* Smallest GGX alpha accepted by the samplers (matches PBR_ALPHA_MIN in
 * src/material.c so both share one roughness convention). */
#define SAMPLING_ALPHA_MIN 1e-4

/* Lower bound on the specular-lobe selection probability, so a path tracer
 * never divides by a zero probability. */
#define SAMPLING_PSPEC_MIN 1e-3

/* ------------------------------------------------------------------ */
/* Orthonormal basis                                                   */
/* ------------------------------------------------------------------ */

/*
 * Build a right-handed orthonormal tangent frame (t, b) around the UNIT normal
 * n, such that {t, b, n} is orthonormal. Uses the EXACT same convention as the
 * `sky_basis` helper in src/material.c: Gram-Schmidt against the world axis
 * least parallel to n, so it is robust for every unit n including near +/-Y.
 *
 *   - Non-unit n is normalized defensively; a degenerate (zero) n yields the
 *     canonical frame t = (1,0,0), b = (0,0,-1) around n = (0,1,0).
 *   - `t` and `b` must be non-NULL; if NULL the call is a no-op.
 */
void sampling_basis(Vec3 n, Vec3 *t, Vec3 *b);

/* ------------------------------------------------------------------ */
/* Cosine-weighted hemisphere                                          */
/* ------------------------------------------------------------------ */

/*
 * Cosine-weighted (Malley) sample of a UNIT direction in the hemisphere around
 * the UNIT normal n. u1, u2 are independent uniforms in [0, 1).
 *
 * The density of the returned direction d is p(d) = dot(d, n) / PI for
 * dot(d, n) > 0 (see sampling_cosine_pdf). The sample is degenerate-safe: a
 * non-unit n is normalized, a zero n falls back to +Y, and out-of-range/NaN
 * uniforms are clamped so the result is always a finite unit vector in the
 * +n hemisphere.
 */
Vec3 sampling_cosine_hemisphere(Vec3 n, double u1, double u2);

/*
 * PDF of the cosine-weighted hemisphere sampler, evaluated at the cosine
 * cos_theta = dot(d, n) of a direction d with the shading normal.
 *
 *   returns cos_theta / PI   for cos_theta > 0
 *   returns 0                for cos_theta <= 0 (back hemisphere / grazing)
 *
 * Units: 1/sr. Range: [0, 1/PI].
 */
double sampling_cosine_pdf(double cos_theta);

/* ------------------------------------------------------------------ */
/* Uniform sphere                                                      */
/* ------------------------------------------------------------------ */

/*
 * Uniformly distributed UNIT direction on the full sphere (solid angle 4*PI).
 * u1, u2 are independent uniforms in [0, 1); out-of-range/NaN inputs are
 * clamped. Constant density 1/(4*PI) per steradian.
 */
Vec3 sampling_uniform_sphere(double u1, double u2);

/* ------------------------------------------------------------------ */
/* Fresnel - Schlick approximation                                     */
/* ------------------------------------------------------------------ */

/*
 * Schlick scalar Fresnel reflectance:
 *   F = f0 + (1 - f0) * (1 - cos_theta)^5
 * with cos_theta clamped to [0, 1] and f0 clamped to [0, 1].
 *
 * Returns F in [0, 1]. Mirrors `fresnel_schlick` in src/material.c.
 */
double sampling_fresnel_schlick(double cos_theta, double f0);

/*
 * Per-channel (RGB) Schlick Fresnel. Applies sampling_fresnel_schlick to each
 * component of f0 independently. Each f0 component is clamped to [0, 1]; the
 * result lies component-wise in [0, 1].
 */
Vec3 sampling_fresnel_schlick_rgb(double cos_theta, Vec3 f0);

/* ------------------------------------------------------------------ */
/* Fresnel - dielectric (exact, for reflect/refract)                   */
/* ------------------------------------------------------------------ */

/*
 * Exact unpolarised Fresnel reflectance at a dielectric interface.
 *
 *   cos_i : cosine of the incidence angle in medium i, clamped to [0, 1].
 *   eta   : ratio of indices of refraction eta = eta_i / eta_t (may be > 1
 *           when entering a denser medium, or < 1 when leaving it).
 *
 * Implements the full Fresnel equations (average of the s- and p-polarised
 * reflectances). Handles total internal reflection: returns 1.0 when
 * sin^2_t = eta^2 * (1 - cos^2_i) >= 1.
 *
 * Returns the reflectance in [0, 1].
 */
double sampling_fresnel_dielectric(double cos_i, double eta);

/*
 * Snell's-law refraction through a dielectric interface.
 *
 *   incident        : UNIT incoming direction (pointing toward the surface).
 *   n               : UNIT surface normal facing the incident side (i.e. on the
 *                     same hemisphere as -incident; dot(incident, n) <= 0).
 *   eta             : ratio eta_i / eta_t (as in sampling_fresnel_dielectric).
 *   out_transmitted : receives the UNIT transmitted direction on success.
 *
 * Returns:
 *   0  on total internal reflection (out_transmitted is left untouched).
 *   1  on success (out_transmitted holds a UNIT transmitted direction).
 *
 * Non-unit incident / n are normalized defensively; a degenerate (zero) input
 * vector is reported as failure (returns 0).
 */
int sampling_refract(Vec3 incident, Vec3 n, double eta, Vec3 *out_transmitted);

/* ------------------------------------------------------------------ */
/* PBR lobe selection                                                  */
/* ------------------------------------------------------------------ */

/*
 * Specular-lobe selection probability p_spec for a metallic/roughness material,
 * used to stochastically choose between the diffuse and specular lobes.
 *
 *   metallic   : [0, 1]; 0 = dielectric, 1 = pure conductor (no diffuse lobe).
 *   roughness  : [0, 1]; perceptual roughness.
 *   f0_scalar  : scalar normal-incidence reflectance F0 (e.g. 0.04 for glass).
 *
 * Heuristic (monotonically increasing in metallic and F0, decreasing in
 * roughness):
 *
 *   p = F0 + (1 - F0) * metallic          // metals have no diffuse lobe
 *   p *= 1 - 0.5 * roughness * (1 - metallic)   // rough dielectrics diffuse more
 *
 * The result is clamped to [SAMPLING_PSPEC_MIN, 1], i.e. always in (0, 1] so a
 * path tracer never divides by a zero probability. metallic = 1 gives exactly
 * 1.0; metallic = 0, roughness = 0 gives exactly F0.
 */
double sampling_specular_probability(double metallic, double roughness,
                                     double f0_scalar);

/*
 * Importance-sample a UNIT GGX / Trowbridge-Reitz half-vector H in the
 * hemisphere around the UNIT normal n.
 *
 *   alpha : GGX roughness alpha > 0, used DIRECTLY (the caller obtains it via
 *           material_roughness_to_alpha). Values below SAMPLING_ALPHA_MIN are
 *           raised to SAMPLING_ALPHA_MIN to avoid a degenerate delta lobe.
 *   u1,u2 : independent uniforms in [0, 1); clamped defensively.
 *
 * Uses the Karis/UE4 NDF sampling
 *   cos_theta = sqrt((1 - u1) / (1 + (alpha^2 - 1) * u1)),  phi = 2*PI*u2
 * mapped through the sampling_basis frame. Matches the half-vector convention
 * used by material_sample_glossy_dir in src/material.c.
 */
Vec3 sampling_ggx_half_vector(Vec3 n, double alpha, double u1, double u2);

/*
 * Standard GGX reflection PDF, expressed with respect to the OUTGOING
 * (reflected) direction L, in 1/sr:
 *
 *   D(H)          = alpha^2 / (PI * ((n_dot_h^2 * (alpha^2 - 1) + 1)^2))
 *   p(L)          = D(H) * n_dot_h / (4 * h_dot_v)
 *
 *   n_dot_h : dot(N, H), the cosine of the half-vector with the shading normal.
 *   h_dot_v : dot(H, V), the cosine of the half-vector with the view direction.
 *   alpha   : GGX roughness alpha > 0 (clamped up to SAMPLING_ALPHA_MIN).
 *
 * Returns 0 when h_dot_v <= 0 or n_dot_h <= 0 (degenerate / back-facing).
 * Range: [0, +inf). The distribution is normalised to 1 when integrated over
 * the full sphere of outgoing directions L (the map L -> H = normalize(V+L)
 * is a bijection of the sphere; directions giving n_dot_h <= 0 yield 0).
 */
double sampling_ggx_pdf(double n_dot_h, double h_dot_v, double alpha);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLING_H */
