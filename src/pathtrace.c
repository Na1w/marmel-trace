/*
 * pathtrace.c - Iterative (NO recursion) unidirectional path-trace kernel.
 *
 * This module implements the core Monte-Carlo kernel of the opt-in path tracer.
 * It is STRICTLY ADDITIVE: it does not modify src/render.{h,c}, and it does not
 * yet implement Next Event Estimation (t-104b) or the public render entry point
 * (t-104c). The kernel here is the unbiased path-integral estimator
 *
 *     L = sum_b  beta_b * Le_b
 *
 * evaluated with a plain `for` loop over bounces: the path is carried by a
 * (throughput, direction, origin) triple instead of a call stack, so there is
 * no recursion and the depth is bounded by `max_depth`.
 *
 * Determinism: every random draw is a pure function of
 * (seed_key, bounce index, fixed channel constant) through a private copy of
 * the renderer's hash PRNG. Nothing depends on the thread, tile or schedule, so
 * the result is bit-reproducible for any parallel decomposition.
 *
 * All colours handled here are LINEAR (no gamma encoding); tone mapping is the
 * `pathtrace_to_byte` helper below.
 */

#include "pathtrace.h"

#include "geometry.h"
#include "material.h"
#include "render.h"
#include "sampling.h"
#include "texture.h"
#include "vec3.h"

#include <math.h>

/*
 * Threading support (t-104c2). The pthread/atomic headers are pulled in ONLY
 * for the -DUSE_PTHREADS build, mirroring render.c, so the default build never
 * references pthread. <stdlib.h>/<unistd.h> are needed by the worker pool
 * (malloc/getenv/strtol, sysconf); <stddef.h> for size_t/NULL.
 */
#ifdef USE_PTHREADS
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

/* Ray epsilon / far bound, matching the renderer's intersection calls. */
#define PT_RAY_EPS 1e-4
#define PT_RAY_MAX 1e30

/* First bounce at which Russian Roulette may kill the path (unbiased). */
#define PT_RR_START_BOUNCE 3

/* Representative propagation distance used when a ray escapes a medium without
 * a further hit (mirrors the Whitted renderer's deep-water fallback). */
#define PT_MEDIUM_FALLBACK_DEPTH 5.0

/* Smallest dielectric reflect/refract selection probability, so neither branch
 * becomes unreachable (keeps the single-path estimator well conditioned). */
#define PT_GLASS_P_MIN 0.05
#define PT_GLASS_P_MAX 0.95

/* ------------------------------------------------------------------ */
/* Private deterministic hash PRNG                                     */
/* ------------------------------------------------------------------ */

/*
 * The three functions below are a VERBATIM copy of render_hash_u32 /
 * render_hash3 / render_rand01 (src/render.c:369-394). They are duplicated
 * (rather than exposed) so this module stays additive and the renderer's own
 * streams are untouched; the algorithm is bit-identical to the renderer's.
 */

/* 32-bit integer avalanche (wang-hash / murmur finalizer family). */
static unsigned pt_hash_u32(unsigned x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* Mix three 32-bit integers into one well-distributed 32-bit value. */
static unsigned pt_hash3(unsigned a, unsigned b, unsigned c)
{
    unsigned h = 0x9e3779b9u;
    h ^= pt_hash_u32(a + 0x85ebca6bu);
    h = pt_hash_u32(h + b);
    h ^= pt_hash_u32(c + 0xc2b2ae35u);
    return pt_hash_u32(h);
}

/* Uniform double in [0, 1) from the hash of (a, b, c). */
static double pt_rand01(unsigned a, unsigned b, unsigned c)
{
    /* 24 bits of mantissa-scale precision -> [0, 1). */
    return (double)(pt_hash3(a, b, c) >> 8) * (1.0 / 16777216.0);
}

/*
 * Disjoint per-bounce channel constants. These are deliberately distinct from
 * every existing stream in the tree (sun disk 0x5a17/0x7c3d, glossy
 * 0x9e37/0x85eb, emissive 0xE311/0x4c11/0x9d27, DOF s*2+1/s*2+2) so the path
 * tracer can never accidentally alias the Whitted renderer's random sequence.
 */
#define PT_KEY       0x5054u /* per-primary-ray domain separator        */
#define PT_CH_BSDF_A 0xB501u /* BSDF uniform u1                         */
#define PT_CH_BSDF_B 0xB502u /* BSDF uniform u2                         */
#define PT_CH_LOBE   0x1001u /* diffuse/specular lobe coin              */
#define PT_CH_GLASS  0x1002u /* reflect/refract coin                    */
#define PT_CH_RR     0x5201u /* Russian-Roulette survival coin          */
#define PT_CH_NEE_SUN_A 0xA101u /* sun-NEE disk sample u1                 */
#define PT_CH_NEE_SUN_B 0xA102u /* sun-NEE disk sample u2                 */
#define PT_CH_NEE_EMIT_A 0xA201u /* emissive-sphere NEE cone sample u1    */
#define PT_CH_NEE_EMIT_B 0xA202u /* emissive-sphere NEE cone sample u2    */

/* ------------------------------------------------------------------ */
/* Scalar helpers                                                      */
/* ------------------------------------------------------------------ */

/* Clamp to [0, 1]; NaN maps to 0. */
static double pt_clamp01(double v)
{
    if (!(v > 0.0)) return 0.0; /* also catches NaN */
    if (v > 1.0) return 1.0;
    return v;
}

/* Clamp to [lo, hi]; NaN maps to lo. Requires lo <= hi. */
static double pt_clamp(double v, double lo, double hi)
{
    if (!(v > lo)) return lo; /* also catches NaN */
    if (v > hi) return hi;
    return v;
}

/* Largest component (NaN-safe: a non-finite input yields +inf). */
static double pt_max_component(Vec3 v)
{
    double m = v.x;
    if (v.y > m) m = v.y;
    if (v.z > m) m = v.z;
    if (!(m > 0.0)) m = 0.0; /* also catches NaN / negatives */
    return m;
}

/* Rec.709 luminance of a linear RGB triple (used for lobe selection). */
static double pt_luminance(Vec3 c)
{
    double y = 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
    return (y > 0.0) ? y : 0.0; /* also catches NaN */
}

/* True when every component is finite. */
static int pt_is_finite(Vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* ------------------------------------------------------------------ */
/* Cook-Torrance microfacets (mirror the semantics in src/material.c)   */
/* ------------------------------------------------------------------ */

/* GGX / Trowbridge-Reitz D(NdotH, alpha); denom >= alpha^2 > 0. */
static double pt_d_ggx(double ndh, double alpha)
{
    double a2 = alpha * alpha;
    double d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (SAMPLING_PI * d * d);
}

/* Smith-Schlick-GGX visibility using PERCEPTUAL roughness (k = (r+1)^2/8). */
static double pt_g_smith_schlick(double ndl, double ndv, double roughness)
{
    double k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    k = pt_clamp01(k);

    double gl = ndl / (ndl * (1.0 - k) + k);
    double gv = ndv / (ndv * (1.0 - k) + k);
    return gl * gv;
}

/* ------------------------------------------------------------------ */
/* Beer-Lambert medium attenuation                                     */
/* ------------------------------------------------------------------ */

/*
 * Fold the attenuation of one segment of length `dist` through a transmissive
 * medium into (beta, radiance).
 *
 * `water_attenuate` is affine in its inner colour:
 *
 *     attenuate(inner, d) = T(d) * inner + (1 - T(d)) * deep_color
 *
 * so the downstream radiance seen after the segment must be scaled by
 * beta * T, while the deep-colour term beta * (1 - T) * deep_color is an
 * ADDITIVE contribution. Both parts are recovered by calling the existing
 * `water_attenuate` helper twice (with a white and a black inner colour) and
 * subtracting, which reuses the module's exact absorption/deep-colour maths.
 */
static void pt_apply_medium(const Material *m, double dist, Vec3 *beta, Vec3 *radiance)
{
    if (m == NULL || beta == NULL || radiance == NULL) {
        return;
    }

    Vec3 white = water_attenuate(m, vec3(1.0, 1.0, 1.0), dist); /* T + (1-T)*deep */
    Vec3 deep = water_attenuate(m, vec3(0.0, 0.0, 0.0), dist);  /*     (1-T)*deep */

    Vec3 transmittance = vec3_sub(white, deep); /* = T, component-wise in [0,1] */

    *radiance = vec3_add(*radiance, vec3_mul(*beta, deep));
    *beta = vec3_mul(*beta, transmittance);
}

/* ------------------------------------------------------------------ */
/* BSDF sampling                                                       */
/* ------------------------------------------------------------------ */

/*
 * PBR (Cook-Torrance) lobe sampling. Stochastically selects the diffuse or
 * specular lobe with probability `p_spec`, importance-samples the chosen lobe,
 * and returns the unbiased throughput multiplier in *w_out.
 *
 *   specular : H ~ GGX(alpha); L = reflect(d, H);
 *              w = D*G*F*cos / (4*NdotV*pdf*p_spec) / p_spec
 *   diffuse  : L ~ cosine;  w = kD*albedo / (1 - p_spec)
 *
 * Returns 1 on success, 0 when the path should terminate (sample below the
 * surface / degenerate denominator -> zero true contribution).
 */
static int pt_sample_pbr(const Material *m, Vec3 N, Vec3 d, Vec3 V,
                         double u1, double u2, double u_lobe,
                         Vec3 *wi_out, Vec3 *w_out, int *out_specular)
{
    if (out_specular != NULL) {
        *out_specular = 0; /* diffuse unless a specular lobe is chosen below */
    }
    double metallic = pt_clamp01(m->metallic);
    double roughness = pt_clamp01(m->roughness);
    double alpha = material_roughness_to_alpha(roughness);

    /* F0 = mix(0.04, albedo, metallic); scalar form drives lobe selection. */
    Vec3 f0 = vec3_lerp(vec3(0.04, 0.04, 0.04), m->albedo, metallic);
    double f0_scalar = pt_luminance(f0);
    double p_spec = sampling_specular_probability(metallic, roughness, f0_scalar);

    double ndv = vec3_dot(N, V);
    if (!(ndv > 0.0)) {
        return 0;
    }

    if (u_lobe < p_spec) {
        /* ---- specular lobe ------------------------------------------ */
        Vec3 H = sampling_ggx_half_vector(N, alpha, u1, u2);
        Vec3 wi = vec3_reflect(d, H);

        double ndl = vec3_dot(N, wi);
        double ndh = vec3_dot(N, H);
        double vdh = vec3_dot(V, H);
        if (!(ndl > 0.0) || !(ndh > 0.0) || !(vdh > 0.0)) {
            return 0; /* sample leaves the +N hemisphere: zero contribution */
        }

        double D = pt_d_ggx(ndh, alpha);
        double G = pt_g_smith_schlick(ndl, ndv, roughness);
        Vec3 F = sampling_fresnel_schlick_rgb(vdh, f0);

        double pdf = sampling_ggx_pdf(ndh, vdh, alpha);
        if (!(pdf > 0.0) || !(p_spec > 0.0)) {
            return 0;
        }

        /* f_spec * NdotL / pdf / p_spec = D*G*F / (4*NdotV*pdf*p_spec). */
        double scale = D * G / (4.0 * ndv * pdf * p_spec);
        *wi_out = wi;
        *w_out = vec3_scale(F, scale);
        if (out_specular != NULL) {
            *out_specular = 1; /* specular lobe: NEE cannot sample it */
        }
        return 1;
    }

    /* ---- diffuse lobe ----------------------------------------------- */
    double denom = 1.0 - p_spec;
    if (!(denom > 0.0)) {
        return 0; /* p_spec == 1: the diffuse lobe carries no energy */
    }

    Vec3 wi = sampling_cosine_hemisphere(N, u1, u2);
    double ndl = vec3_dot(N, wi);
    if (!(ndl > 0.0)) {
        return 0;
    }

    /* kD = (1 - F(NdotV)) * (1 - metallic); f_diff * cos / pdf = kD*albedo. */
    Vec3 F = sampling_fresnel_schlick_rgb(ndv, f0);
    Vec3 kd = vec3_scale(vec3_sub(vec3(1.0, 1.0, 1.0), F), 1.0 - metallic);

    *wi_out = wi;
    *w_out = vec3_scale(vec3_mul(kd, m->albedo), 1.0 / denom);
    return 1;
}

/*
 * Legacy (non-PBR) lobe sampling: a stochastic mix of mirror, dielectric
 * (glass/water) and Lambertian diffuse lobes, selected with probabilities
 * derived from `reflectivity` / `transparency` and divided out again so the
 * single-path estimator stays unbiased.
 *
 *   p_mirror = reflectivity, p_diel = transparency, p_diff = 1 - p_mir - p_die
 *
 * (renormalised when p_mir + p_die > 1). Returns 1 on success, 0 to terminate.
 * `*out_refract` is set to 1 when the dielectric branch actually transmitted
 * (so the caller can track medium entry/exit), 0 otherwise.
 */
static int pt_sample_legacy(const Material *m, Vec3 N, Vec3 d, Vec3 V,
                            int front_face,
                            double u1, double u2, double u_lobe, double u_glass,
                            Vec3 *wi_out, Vec3 *w_out, int *out_refract,
                            int *out_specular)
{
    if (out_refract != NULL) {
        *out_refract = 0;
    }
    if (out_specular != NULL) {
        *out_specular = 0; /* diffuse unless a delta lobe is chosen below */
    }

    double r_mir = pt_clamp01(m->reflectivity);
    double r_die = pt_clamp01(m->transparency);
    double r_dif = 1.0 - r_mir - r_die;
    if (r_dif < 0.0) r_dif = 0.0;

    double p_mir = r_mir;
    double p_die = r_die;
    double p_dif = 1.0 - p_mir - p_die;
    double norm = 1.0; /* energy-conserving lobe-weight normalisation */
    if (p_dif < 0.0) {
        double s = p_mir + p_die;
        if (s > 0.0) {
            p_mir /= s;
            p_die /= s;
            /*
             * The mirror/dielectric lobe WEIGHTS below still carry the
             * un-normalised reflectivity/transparency, so r/p == s for BOTH
             * lobes, which would inject a spurious x s energy gain on every
             * bounce when reflectivity + transparency > 1 (e.g. water:
             * 1.0 + 0.85 = 1.85). Dividing both lobe weights by the same s
             * makes them sum to 1 and conserves energy. When s <= 1 nothing
             * is renormalised and norm stays exactly 1.0, so existing
             * (already energy-conserving) materials are bit-for-bit unchanged.
             */
            if (s > 1.0) norm = s;
        }
        p_dif = 0.0;
    }

    /* ---- mirror lobe ------------------------------------------------- */
    if (u_lobe < p_mir) {
        if (!(p_mir > 0.0)) return 0;
        double w = r_mir / (p_mir * norm);
        *wi_out = vec3_reflect(d, N);
        *w_out = vec3(w, w, w);
        if (out_specular != NULL) {
            *out_specular = 1; /* delta mirror: NEE cannot sample it */
        }
        return 1;
    }

    /* ---- dielectric (glass / water) lobe ----------------------------- */
    if (u_lobe < p_mir + p_die) {
        if (!(p_die > 0.0)) return 0;

        if (out_specular != NULL) {
            *out_specular = 1; /* delta dielectric: NEE cannot sample it */
        }
        double ior = (m->ior > 0.0) ? m->ior : 1.0;
        double eta = front_face ? (1.0 / ior) : ior; /* eta_i / eta_t */
        double cos_i = pt_clamp01(vec3_dot(N, V));
        double F = sampling_fresnel_dielectric(cos_i, eta);
        double lobe = r_die / (p_die * norm); /* lobe-selection normalisation */

        if (F >= 1.0) {
            /* Total internal reflection: 100% reflected, no refraction. */
            *wi_out = vec3_reflect(d, N);
            *w_out = vec3(lobe, lobe, lobe);
            return 1;
        }

        double p_r = pt_clamp(F, PT_GLASS_P_MIN, PT_GLASS_P_MAX);
        if (u_glass < p_r) {
            /* Reflect with weight F / p_r. */
            double w = lobe * (F / p_r);
            *wi_out = vec3_reflect(d, N);
            *w_out = vec3(w, w, w);
            return 1;
        }

        /* Refract with weight (1 - F) / (1 - p_r) * eta^2 (radiance form). */
        Vec3 wi;
        if (!sampling_refract(d, N, eta, &wi)) {
            /* Numerical TIR fallback: behave as the reflect branch. */
            double w = lobe * (F / p_r);
            *wi_out = vec3_reflect(d, N);
            *w_out = vec3(w, w, w);
            return 1;
        }
        double w = lobe * ((1.0 - F) / (1.0 - p_r)) * eta * eta;
        *wi_out = wi;
        *w_out = vec3(w, w, w);
        if (out_refract != NULL) {
            *out_refract = 1;
        }
        return 1;
    }

    /* ---- Lambertian diffuse lobe ------------------------------------- */
    if (!(p_dif > 0.0)) {
        return 0;
    }

    Vec3 wi = sampling_cosine_hemisphere(N, u1, u2);
    double ndl = vec3_dot(N, wi);
    if (!(ndl > 0.0)) {
        return 0;
    }

    /* f = albedo/PI, pdf = cos/PI  =>  f*cos/pdf = albedo; times r_dif/p_dif. */
    double lobe = r_dif / p_dif;
    *wi_out = wi;
    *w_out = vec3_scale(m->albedo, lobe);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Tone mapping                                                        */
/* ------------------------------------------------------------------ */

/*
 * Gamma-encode one linear channel and quantize to an 8-bit byte. Matches the
 * renderer's file-static `to_byte` (clamp to [0,1], pow(x, 1/2.2), round),
 * with an added NaN guard. Exposed for the path-trace render entry point.
 */
unsigned char pathtrace_to_byte(double linear)
{
    double c = linear;
    if (!(c >= 0.0)) {
        c = 0.0; /* also catches NaN and negatives */
    } else if (c > 1.0) {
        c = 1.0;
    }
    double v = pow(c, 1.0 / 2.2) * 255.0;
    int iv = (int)(v + 0.5);
    if (iv < 0) {
        iv = 0;
    } else if (iv > 255) {
        iv = 255;
    }
    return (unsigned char)iv;
}

/* ------------------------------------------------------------------ */
/* Sun Next-Event-Estimation (t-104b1)                                 */
/* ------------------------------------------------------------------ */

/*
 * Sun-only Next-Event-Estimation. Samples a direction on the sun disk, shadows
 * it, and returns its contribution to the path, already weighted by `throughput`
 * (which the caller must NOT modify). The estimator divides by the sampling
 * pdf, but the pdf of a uniform disk sample in solid angle is 1/Omega, so the
 * weight is simply the material response times the disk solid angle Omega.
 *
 * Determinism: the two disk samples are pure functions of
 * (seed_key, bounce, PT_CH_NEE_SUN_A/B), independent of the BSDF stream.
 *
 * NOTE (skip_env policy, see the miss path in pathtrace_radiance): `sky_sample`
 * carries only a smooth sun *glow* (pow(cos_sun, exponent) * strength) and no
 * discrete sun-disk term, and the glow fields are not part of the public
 * SkyParams surface relied on here. Subtracting "the sun disk" from a miss is
 * therefore not cleanly expressible, so NEE is applied on the FIRST bounce only
 * (b == 0) and the miss path is left untouched. This avoids double counting the
 * sun's direct contribution while keeping the gradient/cloud sky intact.
 */
static Vec3 pt_nee_sun(const Scene *scene, const Material *mm, Vec3 P, Vec3 N, Vec3 V,
                       Vec3 throughput, unsigned seed_key, int bounce)
{
    Vec3 sun = scene->sky.sun_dir;
    double r1 = pt_rand01(seed_key, (unsigned)bounce, PT_CH_NEE_SUN_A);
    double r2 = pt_rand01(seed_key, (unsigned)bounce, PT_CH_NEE_SUN_B);
    Vec3 L = sky_sun_disk_dir(sun, scene->sky.sun_radius, r1, r2);
    if (!pt_is_finite(L) || vec3_dot(N, L) <= 0.0) return vec3(0.0, 0.0, 0.0);
    Hit sh;
    Ray sr; sr.origin = vec3_add(P, vec3_scale(N, 1e-3)); sr.dir = L;
    if (scene_intersect(scene, sr, 1e-3, 1e30, &sh)) return vec3(0.0, 0.0, 0.0);
    double rad = scene->sky.sun_radius * 3.14159265358979323846 / 180.0;
    double Omega = (scene->sky.sun_radius > 0.0)
                       ? 2.0 * 3.14159265358979323846 * (1.0 - cos(rad))
                       : 1.0;
    Vec3 lit = mm->pbr ? material_shade_pbr(mm, N, L, V, scene->sky.sun_color)
                       : material_shade_local(mm, N, L, V, scene->sky.sun_color);
    return vec3_mul(throughput, vec3_scale(lit, Omega));   /* 1/pdf = Omega */
}

/* ------------------------------------------------------------------ */
/* Emissive-sphere Next-Event-Estimation (t-104b2)                     */
/* ------------------------------------------------------------------ */

/*
 * Emissive-sphere Next-Event-Estimation. For every emissive sphere in the
 * scene (excluding the one the shading point sits on), it importance-samples a
 * direction inside the sphere's visible cap, shadows the connection, and adds
 * the direct lighting term weighted by `throughput` (which the caller must NOT
 * modify). The estimator divides by the sampling pdf, but the pdf of a uniform
 * visible-cap sample in solid angle is 1/Omega, so the weight is simply the
 * material response times the cap solid angle Omega.
 *
 * Determinism: the two cone samples are pure functions of
 * (seed_key, bounce, PT_CH_NEE_EMIT_A/B, light index), independent of the BSDF
 * stream. `self_prim` suppresses self-illumination (a point on a lamp must not
 * "see" its own emitter, which is handled by the emitted-radiance term).
 */
static Vec3 pt_nee_emissive(const Scene *scene, const Material *mm, Vec3 P, Vec3 N, Vec3 V,
                            Vec3 throughput, int self_prim, unsigned seed_key, int bounce)
{
    Vec3 sum = vec3(0.0, 0.0, 0.0);
    for (int li = 0; li < scene->emissive_light_count; ++li) {
        const EmissiveLight *lt = &scene->emissive_lights[li];
        if (lt->prim_index == self_prim) continue;          /* no self-illumination */
        Vec3 to_c = vec3_sub(lt->center, P);
        double dc2 = vec3_length_sq(to_c);
        double dc = sqrt(dc2);
        if (dc <= lt->radius) continue;                     /* inside/on the lamp */
        double cos_mx = sqrt(1.0 - (lt->radius * lt->radius) / dc2);
        double Omega = 2.0 * 3.14159265358979323846 * (1.0 - cos_mx);
        if (!(Omega > 0.0)) continue;
        Vec3 w = vec3_scale(to_c, 1.0 / dc);
        double u1 = pt_rand01(seed_key ^ (unsigned)li, (unsigned)bounce, PT_CH_NEE_EMIT_A);
        double u2 = pt_rand01(seed_key ^ (unsigned)li, (unsigned)bounce, PT_CH_NEE_EMIT_B);
        Vec3 wi = light_sphere_sample_dir(w, cos_mx, u1, u2);
        if (!pt_is_finite(wi) || vec3_dot(N, wi) <= 0.0) continue;
        Hit sh; Ray sr; sr.origin = vec3_add(P, vec3_scale(N, 1e-3)); sr.dir = wi;
        if (scene_intersect(scene, sr, 1e-3, 1e30, &sh) && sh.prim_index != lt->prim_index) continue;
        Vec3 lit = mm->pbr ? material_shade_pbr(mm, N, wi, V, lt->emissive)
                           : material_shade_local(mm, N, wi, V, lt->emissive);
        sum = vec3_add(sum, vec3_mul(throughput, vec3_scale(lit, Omega)));  /* 1/pdf = Omega */
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Public kernel                                                       */
/* ------------------------------------------------------------------ */

Vec3 pathtrace_radiance(const Scene *scene, Ray primary, int max_depth,
                        unsigned seed_key)
{
    Vec3 radiance = vec3(0.0, 0.0, 0.0);
    if (scene == NULL) {
        return radiance;
    }
    if (max_depth < 0) {
        max_depth = 0;
    }

    Ray r;
    r.origin = primary.origin;
    r.dir = vec3_normalize(primary.dir);
    if (vec3_length_sq(r.dir) <= 0.0) {
        return radiance; /* degenerate primary ray */
    }

    Vec3 throughput = vec3(1.0, 1.0, 1.0);
    const Material *medium = NULL; /* material of the medium currently inside */

    /* De-duplication state (t-104b2): the camera ray (b == 0) is treated as
     * "specular" so emitted radiance is always added on the primary hit; it is
     * then set per bounce to whether the chosen BSDF lobe was a delta
     * (mirror/glass/specular) or a diffuse one. Only specular/camera hits add
     * mm->emissive; diffuse hits rely on NEE (see the emitted-radiance block). */
    int last_bounce_specular = 1;

    for (int b = 0; b <= max_depth; b++) {
        Hit h;
        if (!scene_intersect(scene, r, PT_RAY_EPS, PT_RAY_MAX, &h)) {
            if (medium != NULL) {
                pt_apply_medium(medium, PT_MEDIUM_FALLBACK_DEPTH, &throughput,
                                &radiance);
                medium = NULL;
            }
            /* Escaped to the environment: add the sky once, weighted by beta. */
            radiance = vec3_add(radiance,
                                vec3_mul(throughput, sky_sample(r.dir, &scene->sky)));
            break;
        }

        /* Attenuate the segment just travelled through a transmissive medium. */
        if (medium != NULL) {
            pt_apply_medium(medium, h.t, &throughput, &radiance);
        }

        const Material *m = scene_material(scene, h.material_index);
        if (m == NULL) {
            radiance = vec3_add(radiance,
                                vec3_mul(throughput, sky_sample(r.dir, &scene->sky)));
            break;
        }

        Vec3 P = h.point;
        Vec3 d = r.dir;
        Vec3 N = h.normal; /* already flipped to oppose the incoming ray */

        if (m->is_water) {
            N = water_normal(P.x, P.z, 0.0); /* wave-perturbed normal, t = 0 */
        }
        if (vec3_dot(N, d) > 0.0) {
            N = vec3_neg(N); /* safety re-flip */
        }
        N = vec3_normalize(N);
        if (vec3_length_sq(N) <= 0.0) {
            break; /* degenerate shading normal */
        }
        Vec3 V = vec3_neg(d); /* toward the eye */

        /* Texture-modulated albedo on a local copy (physical fields intact). */
        Material m_local = *m;
        m_local.albedo = texture_albedo(m, P);
        const Material *mm = &m_local;

        /* Emitted radiance of a directly-hit light (zero for ordinary mats).
         *
         * DE-DUPLICATION (t-104b2): direct light from emissive spheres is
         * handled by the Next-Event-Estimation pass below (pt_nee_emissive), so
         * emitted radiance is only added here when it cannot double-count that
         * term, i.e. when the hit was reached either
         *   (a) directly by the CAMERA ray (first bounce, b == 0), or
         *   (b) through a SPECULAR reflection/refraction, where NEE cannot
         *       sample the delta BSDF lobe.
         * Diffuse bounces are skipped: their direct lighting is fully accounted
         * for by NEE. `last_bounce_specular` is 1 for the camera ray and for
         * any mirror/glass/specular bounce, 0 after a diffuse bounce. */
        if (b == 0 || last_bounce_specular) {
            radiance = vec3_add(radiance, vec3_mul(throughput, mm->emissive));
        }

        /* Emissive-sphere Next-Event-Estimation (t-104b2): applied at EVERY
         * bounce. Adds the direct light term WITHOUT modifying `throughput`, so
         * the rest of the path stays unbiased. */
        radiance = vec3_add(radiance,
                            pt_nee_emissive(scene, mm, P, N, V, throughput,
                                            h.prim_index, seed_key, b));

        /* Sun Next-Event-Estimation (t-104b1): first bounce only (see the
         * skip_env policy note on pt_nee_sun). Adds the direct sun term WITHOUT
         * modifying `throughput`, so the rest of the path stays unbiased. */
        if (b == 0) {
            radiance = vec3_add(radiance,
                                pt_nee_sun(scene, mm, P, N, V, throughput,
                                           seed_key, b));
        }

        /* Per-bounce randomness: pure in (seed_key, b, channel). */
        unsigned kb = pt_hash3(seed_key ^ PT_KEY, (unsigned)b, 0u);
        double u1 = pt_rand01(kb, (unsigned)b, PT_CH_BSDF_A);
        double u2 = pt_rand01(kb, (unsigned)b, PT_CH_BSDF_B);
        double u_lobe = pt_rand01(kb, (unsigned)b, PT_CH_LOBE);
        double u_glass = pt_rand01(kb, (unsigned)b, PT_CH_GLASS);

        Vec3 wi, w;
        int ok;
        int refracted = 0;
        int sampled_specular = 0;
        if (mm->pbr) {
            ok = pt_sample_pbr(mm, N, d, V, u1, u2, u_lobe, &wi, &w,
                               &sampled_specular);
        } else {
            ok = pt_sample_legacy(mm, N, d, V, h.front_face, u1, u2, u_lobe,
                                  u_glass, &wi, &w, &refracted,
                                  &sampled_specular);
        }
        if (!ok) {
            break; /* zero-weight sample: terminate the path */
        }
        last_bounce_specular = sampled_specular;

        /* Update throughput; reject non-finite / non-positive results. */
        throughput = vec3_mul(throughput, w);
        if (!pt_is_finite(throughput) || !(pt_max_component(throughput) > 0.0)) {
            break;
        }

        /* Medium bookkeeping: track entry/exit across dielectric interfaces. */
        if (refracted && (m->is_water || m->beer_lambert)) {
            medium = h.front_face ? m : NULL;
        }

        /* ---- Russian Roulette (unbiased termination) ----------------- */
        if (b >= PT_RR_START_BOUNCE) {
            double q = pt_clamp01(pt_max_component(throughput));
            if (!(q > 0.0)) {
                break;
            }
            double u_rr = pt_rand01(kb, (unsigned)b, PT_CH_RR);
            if (u_rr >= q) {
                break;
            }
            throughput = vec3_scale(throughput, 1.0 / q);
        }

        /* ---- advance the ray ----------------------------------------- */
        Vec3 offset;
        if (refracted) {
            offset = vec3_scale(wi, PT_RAY_EPS); /* travel into the medium */
        } else {
            offset = vec3_scale(N, vec3_dot(wi, N) >= 0.0 ? PT_RAY_EPS
                                                          : -PT_RAY_EPS);
        }
        r.origin = vec3_add(P, offset);
        r.dir = wi;
    }

    if (!pt_is_finite(radiance)) {
        return vec3(0.0, 0.0, 0.0);
    }
    /* Clamp negatives (should not occur, but keeps the estimator well-formed). */
    if (radiance.x < 0.0) radiance.x = 0.0;
    if (radiance.y < 0.0) radiance.y = 0.0;
    if (radiance.z < 0.0) radiance.z = 0.0;
    return radiance;
}

/* ------------------------------------------------------------------ */
/* Per-pixel / per-region kernel (shared by both builds)               */
/* ------------------------------------------------------------------ */

/*
 * Path-trace ONE pixel: average `spp` jittered camera rays and quantize the
 * linear mean into the TOP-DOWN, row-major RGB buffer. Pure function of
 * (x, y, scene, cam, width, height, spp, max_depth): it reads no shared state
 * and writes ONLY its own three output bytes, so it is safe to call from any
 * thread on a disjoint pixel.
 *
 * Determinism: the sub-pixel jitter, the thin-lens DOF sample and the
 * per-primary-ray seed are all pure functions of (x, y, s) through the module
 * PRNG -- never of a thread or tile -- so the buffer is byte-reproducible for
 * ANY thread count. The per-pixel sample loop stays serial, keeping the
 * summation order fixed.
 */
static void pt_render_pixel(const Scene *scene, const Camera *cam,
                            int width, int height, int spp, int max_depth,
                            unsigned char *rgb_out, int x, int y)
{
    Vec3 acc = vec3(0.0, 0.0, 0.0);
    for (int s = 0; s < spp; ++s) {
        /*
         * Deterministic sub-pixel jitter and thin-lens DOF sample. The pixel
         * offset uses PRNG channels s*2+0 / s*2+1 and the lens sample uses
         * s*2+100 / s*2+101, so the two streams never collide; both are also
         * distinct from render.c's DOF channels (s*2+2 / s*2+3). Every draw is
         * a pure function of (x, y, s), never of a thread or tile, so the image
         * is reproducible.
         */
        double jx = pt_rand01((unsigned)x, (unsigned)y,
                              (unsigned)(s * 2 + 0));
        double jy = pt_rand01((unsigned)x, (unsigned)y,
                              (unsigned)(s * 2 + 1));
        double lr1 = pt_rand01((unsigned)x, (unsigned)y,
                               (unsigned)(s * 2 + 100));
        double lr2 = pt_rand01((unsigned)x, (unsigned)y,
                               (unsigned)(s * 2 + 101));

        double uu = ((double)x + jx) / (double)width;
        double vv = 1.0 - ((double)y + jy) / (double)height;
        Ray ray = camera_ray_dof(cam, uu, vv, lr1, lr2);

        /*
         * Per-primary-ray key: a pure function of (x, y, s) only, so the
         * path-trace result is independent of the schedule.
         */
        unsigned seed_key = pt_hash3((unsigned)x, (unsigned)y, (unsigned)s);
        acc = vec3_add(acc, pathtrace_radiance(scene, ray, max_depth, seed_key));
    }

    acc = vec3_scale(acc, 1.0 / (double)spp);

    unsigned char *p = &rgb_out[((size_t)y * (size_t)width + (size_t)x) * 3];
    p[0] = pathtrace_to_byte(acc.x);
    p[1] = pathtrace_to_byte(acc.y);
    p[2] = pathtrace_to_byte(acc.z);
}

/*
 * Path-trace the half-open pixel rectangle [x0, x1) x [y0, y1). Writes only
 * inside that rectangle, so disjoint regions never race. Used by the threaded
 * build to render one claimed tile; the serial build calls pt_render_pixel
 * directly from its row loop, so this helper is compiled only under
 * -DUSE_PTHREADS (avoiding an unused-function warning otherwise).
 */
#ifdef USE_PTHREADS

static void pt_render_region(const Scene *scene, const Camera *cam,
                             int width, int height, int spp, int max_depth,
                             unsigned char *rgb_out,
                             int x0, int x1, int y0, int y1)
{
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            pt_render_pixel(scene, cam, width, height, spp, max_depth,
                            rgb_out, x, y);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Threading (optional, t-104c2)                                       */
/* ------------------------------------------------------------------ */

/*
 * Mirrors render.c's tile-parallel pattern EXACTLY: a fixed-size tile grid over
 * the image, a shared atomic claim counter, and a pool of workers that each
 * render whatever tile they claim. Because every pixel is a pure function of
 * (x, y, s), the output is byte-identical to the serial path for any thread
 * count. Without -DUSE_PTHREADS this whole section compiles away and the render
 * is the plain single-threaded loop.
 */

/* Upper bound on the worker count (matches render.c's RENDER_MAX_THREADS). */
#define PT_MAX_THREADS 64

/* Edge length of a work tile, in pixels (matches render.c's RENDER_TILE_SIZE). */
#define PT_TILE_SIZE 16

/*
 * Shared, read-only render description plus the dynamic work counter. The
 * atomic tile counter is the single point of synchronisation: workers claim the
 * next tile with atomic_fetch_add and never touch each other's pixels. The
 * `tiles_done` counter feeds the progress meter (single-writer gate inside
 * render_progress_update keeps stderr from interleaving).
 */
typedef struct {
    const Scene   *scene;
    const Camera  *cam;
    int            width;
    int            height;
    int            spp;
    int            max_depth;
    unsigned char *rgb_out;
    int            tiles_x;
    int            tiles_y;
    int            progress;   /* non-zero: draw the stderr meter */
    atomic_int     next_tile;  /* index of the next tile to claim */
    atomic_int     tiles_done; /* tiles finished (thread-safe progress) */
} PtShared;

static void *pt_worker(void *arg)
{
    PtShared *sh = (PtShared *)arg;
    const int ntiles = sh->tiles_x * sh->tiles_y;

    for (;;) {
        int tile = atomic_fetch_add(&sh->next_tile, 1);
        if (tile >= ntiles) {
            break; /* all work claimed */
        }

        int ty = tile / sh->tiles_x;
        int tx = tile - ty * sh->tiles_x;

        int x0 = tx * PT_TILE_SIZE;
        int y0 = ty * PT_TILE_SIZE;
        int x1 = x0 + PT_TILE_SIZE;
        int y1 = y0 + PT_TILE_SIZE;
        if (x1 > sh->width) {
            x1 = sh->width; /* last column may be a partial tile */
        }
        if (y1 > sh->height) {
            y1 = sh->height; /* last row may be a partial tile */
        }

        pt_render_region(sh->scene, sh->cam, sh->width, sh->height, sh->spp,
                         sh->max_depth, sh->rgb_out, x0, x1, y0, y1);

        /*
         * Thread-safe progress: count the finished tile, then let
         * render_progress_update() decide (via its atomic single-writer gate
         * and ~120 ms throttle) whether THIS worker redraws the meter. At most
         * one thread ever writes to stderr, so the line never interleaves.
         */
        if (sh->progress) {
            int done = atomic_fetch_add(&sh->tiles_done, 1) + 1;
            render_progress_update(NULL, done);
        }
    }
    return NULL;
}

/*
 * Pick the worker count the SAME way render.c does: the online CPU count,
 * clamped to [1, PT_MAX_THREADS] and never more than the number of tiles
 * (spawning more threads than there is work for is pure overhead). A positive
 * RAYTRACER_THREADS environment variable overrides the CPU count, which is
 * handy for verifying byte-identity across different thread counts.
 */
static int pt_choose_thread_count(int tiles_x, int tiles_y)
{
    int nthreads;
    const char *env = getenv("RAYTRACER_THREADS");

    if (env != NULL && env[0] != '\0') {
        long v = strtol(env, NULL, 10);
        nthreads = (v >= 1 && v <= PT_MAX_THREADS) ? (int)v : 1;
    } else {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = (ncpu >= 1) ? (int)ncpu : 4;
        if (nthreads > PT_MAX_THREADS) {
            nthreads = PT_MAX_THREADS;
        }
    }

    int ntiles = tiles_x * tiles_y;
    if (nthreads > ntiles) {
        nthreads = ntiles; /* never spawn more threads than there is work */
    }
    if (nthreads < 1) {
        nthreads = 1;
    }
    return nthreads;
}

#endif /* USE_PTHREADS */

/* ------------------------------------------------------------------ */
/* Public render entry point                                           */
/* ------------------------------------------------------------------ */

/*
 * Path-trace render. Mirrors render_image()'s per-pixel loop (average `spp`
 * jittered camera rays, gamma-encode/quantize into the TOP-DOWN row-major RGB
 * buffer) and, under -DUSE_PTHREADS, its tile-parallel worker pool: the image
 * is split into fixed-size tiles that workers claim from a shared atomic
 * counter. The output is byte-identical to the serial path for any thread
 * count because every pixel is a pure function of (x, y, s) and tiles are
 * disjoint. Without -DUSE_PTHREADS the render is the plain single-threaded
 * loop and never references pthread.
 *
 * Determinism: the sub-pixel jitter, the thin-lens DOF sample and the
 * per-primary-ray seed are all pure functions of (x, y, s) through the module
 * PRNG -- never of a thread or tile -- so the buffer is byte-reproducible.
 */
int pathtrace_render(const Scene *scene, const Camera *cam, int width, int height,
                     int samples_per_pixel, int max_depth,
                     unsigned char *rgb_out)
{
    if (scene == NULL || cam == NULL || rgb_out == NULL) {
        return 1; /* NULL pointer */
    }
    if (width <= 0 || height <= 0) {
        return 2; /* non-positive dimensions */
    }
    if (samples_per_pixel < 1 || max_depth < 0) {
        return 3; /* bad sample / depth counts */
    }

    /* Progress is stderr-only and easily silenced via RAYTRACER_NO_PROGRESS. */
#ifdef USE_PTHREADS
    int progress = render_progress_enabled();
    /* Tile grid covering the image (tiles may be partial at the edges). */
    int tiles_x = (width + PT_TILE_SIZE - 1) / PT_TILE_SIZE;
    int tiles_y = (height + PT_TILE_SIZE - 1) / PT_TILE_SIZE;
    int nthreads = pt_choose_thread_count(tiles_x, tiles_y);

    PtShared shared;
    shared.scene = scene;
    shared.cam = cam;
    shared.width = width;
    shared.height = height;
    shared.spp = samples_per_pixel;
    shared.max_depth = max_depth;
    shared.rgb_out = rgb_out;
    shared.tiles_x = tiles_x;
    shared.tiles_y = tiles_y;
    shared.progress = progress;
    atomic_init(&shared.next_tile, 0);
    atomic_init(&shared.tiles_done, 0);

    /*
     * Legacy no-plumbing progress: a NULL state pointer selects the
     * process-wide meter, exactly as the renderer's own entry points use it, so
     * pathtrace_render needs no caller plumbing. Whole render == tiles_x *
     * tiles_y work units; the meter throttles internally and is single-writer.
     */
    RenderProgress pr;
    pr.total = 0;
    pr.total_expected = (long long)tiles_x * (long long)tiles_y;
    pr.pixels_total = (long long)width * (long long)height;
    render_progress_begin(&pr);

    if (nthreads <= 1) {
        /* Trivial case: no point paying for thread creation. */
        pt_worker(&shared);
    } else {
        pthread_t *tids =
            (pthread_t *)malloc((size_t)nthreads * sizeof(pthread_t));
        if (tids == NULL) {
            /* Allocation failure: render everything on the calling thread. */
            pt_worker(&shared);
        } else {
            int started = 0;
            for (int i = 0; i < nthreads; ++i) {
                if (pthread_create(&tids[i], NULL, pt_worker, &shared) == 0) {
                    started++;
                } else {
                    /* Creation failed: drain remaining tiles inline, then
                     * stop spawning more so no tile is left unrendered. */
                    pt_worker(&shared);
                    break;
                }
            }
            /* If every create failed, `started` is 0 and the loop above
             * already drained all tiles inline. */
            for (int i = 0; i < started; ++i) {
                pthread_join(tids[i], NULL);
            }
            free(tids);
        }
    }

    render_progress_finish(&pr);
    return 0;
#else
    /*
     * Legacy no-plumbing progress: one work unit per completed row (the meter
     * throttles internally), matching the pre-tiling behaviour exactly.
     */
    render_progress_begin(NULL);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pt_render_pixel(scene, cam, width, height, samples_per_pixel,
                            max_depth, rgb_out, x, y);
        }
        render_progress_update(NULL, (long long)(y + 1));
    }

    render_progress_finish(NULL);
    return 0;
#endif
}
