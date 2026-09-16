/*
 * material.c - Materials, local shading, procedural sky and water helpers.
 *
 * Depends only on src/vec3.h and src/noise.h. Pure functions, no globals,
 * no I/O, no dynamic allocation. Colors are linear (gamma handled downstream).
 */

#include "material.h"

#include "noise.h"

#include <math.h>
#include <stddef.h>

/* Local PI so we do not depend on M_PI (POSIX-only). */
#define MATERIAL_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* Small scalar helpers                                                */
/* ------------------------------------------------------------------ */

static double clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

/* Hermite smoothstep between edges e0 and e1 (e0 may be > e1 or == e1). */
static double smoothstep(double e0, double e1, double x)
{
    double denom = e1 - e0;
    if (denom == 0.0) {
        return x < e0 ? 0.0 : 1.0;
    }
    double t = clamp01((x - e0) / denom);
    return t * t * (3.0 - 2.0 * t);
}

/* Guarded pow for a non-negative base (avoids NaN for tiny negatives). */
static double pow_nonneg(double base, double exponent)
{
    if (base <= 0.0) return 0.0;
    return pow(base, exponent);
}

/* Forward declaration: the orthonormal tangent-frame helper is defined further
 * down (with the sun-disk code) and is reused by the glossy reflection
 * sampler below, so the whole project shares ONE basis routine. */
static void sky_basis(Vec3 n, Vec3 *t, Vec3 *b);

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */

void sky_default_params(SkyParams *p)
{
    if (p == NULL) {
        return;
    }

    /* Sun up and slightly to the side; roughly 40 degrees elevation. */
    p->sun_dir = vec3_normalize(vec3(0.45, 0.75, -0.50));

    p->sun_color         = vec3(1.00, 0.95, 0.85);
    p->horizon_color     = vec3(0.75, 0.85, 1.00);
    p->zenith_color      = vec3(0.35, 0.55, 0.95);
    p->gradient_gamma    = 0.6;
    p->sun_glow_exponent = 350.0;
    p->sun_glow_strength = 0.8;

    p->cloud_height   = 120.0;
    p->cloud_scale    = 0.0025;
    p->cloud_coverage = 0.5;
    p->cloud_softness = 0.12;
    p->cloud_sharpness = 1.5;
    p->cloud_octaves  = 5;
    p->seed           = 1337u;

    /* Point-like sun: hard shadows (byte-identical to the pre-soft-shadow
     * renderer). Non-zero values enable sun-disk area-light sampling. */
    p->sun_radius     = SKY_DEFAULT_SUN_RADIUS;
}

/* ------------------------------------------------------------------ */
/* Procedural texture defaults                                         */
/* ------------------------------------------------------------------ */

void material_texture_defaults(Material *m)
{
    if (m == NULL) {
        return;
    }
    m->texture_kind    = TEXTURE_DEFAULT_KIND;
    m->texture_scale   = TEXTURE_DEFAULT_SCALE;
    m->texture_color_a = TEXTURE_DEFAULT_COLOR_A;
    m->texture_color_b = TEXTURE_DEFAULT_COLOR_B;
}

/* ------------------------------------------------------------------ */
/* Fresnel                                                             */
/* ------------------------------------------------------------------ */

double fresnel_schlick(double cos_theta, double f0)
{
    double c = clamp01(cos_theta);
    double m = 1.0 - c;
    double m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

Vec3 fresnel_schlick_rgb(double cos_theta, Vec3 f0)
{
    double c = clamp01(cos_theta);
    double m = 1.0 - c;
    double m2 = m * m;
    double k = m2 * m2 * m; /* (1 - cos)^5 */
    return vec3(f0.x + (1.0 - f0.x) * k,
                f0.y + (1.0 - f0.y) * k,
                f0.z + (1.0 - f0.z) * k);
}

/* ------------------------------------------------------------------ */
/* Local shading                                                       */
/* ------------------------------------------------------------------ */

Vec3 material_shade_local(const Material *m, Vec3 N, Vec3 L, Vec3 V,
                          Vec3 light_color)
{
    if (m == NULL) {
        return vec3(0.0, 0.0, 0.0);
    }

    double ndl = vec3_dot(N, L);
    if (ndl <= 0.0) {
        /* Light is behind the surface: nothing is lit. */
        return vec3(0.0, 0.0, 0.0);
    }

    /* Diffuse (Lambert). */
    Vec3 diffuse = vec3_scale(vec3_mul(m->albedo, light_color), ndl);

    /* Specular (Blinn-Phong half-vector). */
    Vec3 h = vec3_add(L, V);
    Vec3 spec = vec3(0.0, 0.0, 0.0);

    /* Handle the degenerate L == -V case (H -> 0). */
    if (vec3_length_sq(h) > 1e-12) {
        Vec3 H = vec3_normalize(h);
        double ndh = vec3_dot(N, H);
        if (ndh > 0.0) {
            double spec_term = pow_nonneg(ndh, m->shininess);
            spec = vec3_scale(vec3_mul(m->specular, light_color), spec_term);
        }
    }

    return vec3_add(diffuse, spec);
}

/* ------------------------------------------------------------------ */
/* Physically-based Cook-Torrance microfacet shading (opt-in)          */
/* ------------------------------------------------------------------ */

/*
 * Smallest alpha squared-roughness maps to. roughness = 0 would give a = 0 and
 * a 0/0 in D at NdotH = 1; the clamp keeps D finite and makes it peak at
 * a^2 / (PI * a^2) = 1/PI at the mirror limit (see research doc sec. 4.4).
 */
#define PBR_ALPHA_MIN 1e-4

/* GGX / Trowbridge-Reitz normal distribution D(NdotH, a), a = alpha > 0.
 * denom = NdotH^2 * (a^2 - 1) + 1 is >= a^2 > 0, so no divide-by-zero. */
static double d_ggx(double ndh, double a)
{
    double a2 = a * a;
    double d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (MATERIAL_PI * d * d);
}

/* Smith-Schlick-GGX visibility G for direct lighting, using PERCEPTUAL
 * roughness (not alpha): k = (roughness + 1)^2 / 8, clamped to [0, 1].
 * Both NdotL and NdotV must be >= 0. */
static double g_smith_schlick(double ndl, double ndv, double roughness)
{
    double k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    if (k < 0.0) k = 0.0;
    else if (k > 1.0) k = 1.0;

    double gl = ndl / (ndl * (1.0 - k) + k);
    double gv = ndv / (ndv * (1.0 - k) + k);
    return gl * gv;
}

/* Disney/GGX perceptual-roughness remap: alpha = max(roughness^2, A_MIN).
 * Public so the renderer's glossy reflection sampler shares the EXACT same
 * alpha convention as the direct-sun Cook-Torrance lobe (see material.h). */
double material_roughness_to_alpha(double roughness)
{
    double r = clamp01(roughness);
    double a = r * r;
    return a < PBR_ALPHA_MIN ? PBR_ALPHA_MIN : a;
}

Vec3 material_shade_pbr(const Material *m, Vec3 N, Vec3 L, Vec3 V,
                        Vec3 light_color)
{
    if (m == NULL) {
        return vec3(0.0, 0.0, 0.0);
    }

    double ndl = vec3_dot(N, L);
    if (ndl <= 0.0) {
        /* Light is behind the surface: nothing is lit (same early-out as
         * material_shade_local). */
        return vec3(0.0, 0.0, 0.0);
    }

    double ndv = vec3_dot(N, V);
    if (ndv <= 0.0) {
        /* Viewer below the horizon of the surface: no specular response. */
        return vec3(0.0, 0.0, 0.0);
    }

    double metallic = clamp01(m->metallic);
    double alpha = material_roughness_to_alpha(m->roughness);

    /* Half-vector (guard the degenerate L == -V case, H -> 0). */
    Vec3 h = vec3_add(L, V);
    double ndh = 0.0;
    if (vec3_length_sq(h) > 1e-12) {
        ndh = vec3_dot(N, vec3_normalize(h));
        if (ndh < 0.0) ndh = 0.0;
    }

    /* Fresnel: F0 = mix(0.04, albedo, metallic), evaluated at NdotV. */
    Vec3 f0 = vec3_lerp(vec3(0.04, 0.04, 0.04), m->albedo, metallic);
    Vec3 F = fresnel_schlick_rgb(ndv, f0);

    /* Cook-Torrance specular: f = D*G*F / (4*NdotL*NdotV). The denominator is
     * strictly positive because we already guarded NdotL > 0 and NdotV > 0. */
    double D = d_ggx(ndh, alpha);
    double G = g_smith_schlick(ndl, ndv, clamp01(m->roughness));
    double denom = 4.0 * ndl * ndv;
    double spec_scale = (denom > 0.0) ? (D * G / denom) : 0.0;
    Vec3 spec = vec3_scale(F, spec_scale);

    /* Diffuse (pi-consistent Lambert) weighted by the standard kD term
     * kD = (1 - F) * (1 - metallic), so the energy that the specular lobe
     * reflects (kS = F) is NOT also re-emitted by the diffuse lobe. This is
     * what makes the combined BRDF directional-hemispherical energy
     * conserving: rho_diff = (1 - metallic) * integral (1 - F) * albedo/PI,
     * which is strictly below the previous (1 - metallic) * albedo and leaves
     * headroom for rho_spec <= 1. */
    Vec3 kd = vec3_scale(vec3_sub(vec3(1.0, 1.0, 1.0), F), (1.0 - metallic));
    Vec3 f_diff = vec3_scale(vec3_mul(kd, m->albedo), 1.0 / MATERIAL_PI);
    Vec3 f = vec3_add(f_diff, spec);

    /* L_o = light_color * NdotL * (f_diff + f_spec). */
    return vec3_scale(vec3_mul(light_color, f), ndl);
}

/* ------------------------------------------------------------------ */
/* Glossy (roughness-blurred) reflection sampling                      */
/* ------------------------------------------------------------------ */

/*
 * Largest representable double strictly below 1.0, used to clamp the
 * uniform samples into [0, 1) so `1 - r1` and the GGX denominator stay
 * strictly positive.
 */
#define MATERIAL_RAND01_MAX 0.9999999999999999

Vec3 material_sample_glossy_dir(const Material *m, Vec3 N, Vec3 incident,
                                double r1, double r2)
{
    Vec3 mirror = vec3_reflect(incident, N);

    /* NULL material or a mirror-smooth surface: the EXACT legacy direction. */
    if (m == NULL || m->roughness <= GLOSSY_ROUGHNESS_EPSILON) {
        return mirror;
    }

    double alpha = material_roughness_to_alpha(m->roughness);
    double a2 = alpha * alpha;

    /* Clamp the uniforms defensively: 1 - r1 > 0 and the denominator
     * 1 + (a2 - 1) * r1 >= a2 > 0, so there is never a divide-by-zero and
     * cosTheta lands in (0, 1], i.e. H always lies in the +N hemisphere. */
    if (!(r1 > 0.0)) r1 = 0.0;          /* also catches NaN */
    else if (r1 > MATERIAL_RAND01_MAX) r1 = MATERIAL_RAND01_MAX;
    if (!(r2 > 0.0)) r2 = 0.0;          /* also catches NaN */
    else if (r2 > MATERIAL_RAND01_MAX) r2 = MATERIAL_RAND01_MAX;

    /* GGX / Trowbridge-Reitz NDF importance sampling of the half-vector
     * (Karis/UE4): cosTheta = sqrt((1 - u1) / (1 + (a2 - 1) u1)). */
    double cos_theta = sqrt((1.0 - r1) / (1.0 + (a2 - 1.0) * r1));
    double sin_theta = sqrt(1.0 - cos_theta * cos_theta);
    double phi = 2.0 * MATERIAL_PI * r2;

    /* World-space half-vector from the shared `sky_basis` tangent frame. */
    Vec3 t, b;
    sky_basis(N, &t, &b);
    Vec3 H = vec3_add(
        vec3_add(vec3_scale(t, sin_theta * cos(phi)),
                 vec3_scale(b, sin_theta * sin(phi))),
        vec3_scale(N, cos_theta));
    H = vec3_normalize(H);

    /* Reflect the incident direction about H. */
    Vec3 R = vec3_reflect(incident, H);

    /* Very rough near-grazing hits can push R below the surface; fall back to
     * the exact mirror direction so the traced ray stays on the correct side
     * (never a clamped/zero vector). */
    if (!(vec3_dot(R, N) > 0.0)) {      /* also catches NaN */
        return mirror;
    }
    return vec3_normalize(R);
}

Vec3 material_ambient(const Material *m, Vec3 N, const SkyParams *sky)
{
    if (m == NULL) {
        return vec3(0.0, 0.0, 0.0);
    }

    /* Groundish color: a muted fraction of the horizon. Skyish: zenith. */
    Vec3 groundish = vec3_scale(vec3(0.35, 0.34, 0.30), 1.0);
    Vec3 skyish = vec3(0.35, 0.55, 0.95);

    if (sky != NULL) {
        /* Tie ambient to the actual sky so scenes stay consistent. */
        groundish = vec3_scale(sky->horizon_color, 0.45);
        skyish = sky->zenith_color;
    }

    double w = clamp01(0.5 + 0.5 * N.y);
    Vec3 hemi = vec3_lerp(groundish, skyish, w);

    /* Small ambient factor so unlit areas stay dark but readable. */
    const double ambient_factor = 0.15;

    return vec3_scale(vec3_mul(m->albedo, hemi), ambient_factor);
}

/* ------------------------------------------------------------------ */
/* Procedural sky                                                      */
/* ------------------------------------------------------------------ */

Vec3 sky_sample(Vec3 dir, const SkyParams *sky)
{
    if (sky == NULL) {
        return vec3(0.0, 0.0, 0.0);
    }

    /* --- Base gradient: horizon -> zenith ----------------------------- */
    double h = clamp01(dir.y);
    double t = pow_nonneg(h, sky->gradient_gamma);
    Vec3 base = vec3_lerp(sky->horizon_color, sky->zenith_color, t);

    /* --- Sun glow (tight halo around the sun direction) --------------- */
    double cos_sun = vec3_dot(dir, sky->sun_dir);
    if (cos_sun > 0.0) {
        double glow = pow_nonneg(cos_sun, sky->sun_glow_exponent)
                      * sky->sun_glow_strength;
        base = vec3_add(base, vec3_scale(sky->sun_color, glow));
    }

    /* --- Clouds on a horizontal layer at cloud_height ----------------- */
    const double cloud_eps = 0.03;
    if (dir.y > cloud_eps) {
        /* Project the ray onto the cloud plane (camera at origin). */
        double tt = sky->cloud_height / dir.y;
        double px = dir.x * tt;
        double pz = dir.z * tt;

        int octaves = sky->cloud_octaves;
        if (octaves < 1) {
            octaves = 1;
        }

        /* fBm in ~[-1, 1] remapped to [0, 1]. */
        double raw = noise_fbm2(px * sky->cloud_scale,
                                pz * sky->cloud_scale,
                                octaves, 2.0, 0.5, sky->seed);
        double density = clamp01(0.5 * (raw + 1.0));

        double a = sky->cloud_coverage - sky->cloud_softness;
        double b = sky->cloud_coverage + sky->cloud_softness;
        double alpha = smoothstep(a, b, density);
        alpha = pow_nonneg(alpha, sky->cloud_sharpness);

        /* Fade clouds out as the ray approaches the horizon. */
        alpha *= smoothstep(0.0, 0.15, dir.y);

        if (alpha > 0.0) {
            /* Fake self-shadow: thicker cloud interior darkens. */
            double shadow = 0.6 + 0.4 * alpha;
            Vec3 cloud_color = vec3_scale(vec3(0.95, 0.95, 0.98), shadow);

            /* Sun lighting on the cloud top. */
            double lit = cos_sun > 0.0 ? cos_sun : 0.0;
            Vec3 lit_cloud = vec3_add(cloud_color,
                                      vec3_scale(vec3_mul(cloud_color, sky->sun_color),
                                                 0.35 * lit));

            base = vec3_lerp(base, lit_cloud, alpha);
        }
    }

    /* Final safety: keep components finite and non-negative. */
    if (!(base.x >= 0.0)) base.x = 0.0;
    if (!(base.y >= 0.0)) base.y = 0.0;
    if (!(base.z >= 0.0)) base.z = 0.0;

    return base;
}

/* ------------------------------------------------------------------ */
/* Sun disk (soft shadows)                                             */
/* ------------------------------------------------------------------ */

/*
 * Orthonormal basis (t, b) spanning the plane perpendicular to the unit
 * vector `n`. Gram-Schmidt against the world axis least parallel to `n`,
 * exactly as camera_create does for its look-at basis.
 */
static void sky_basis(Vec3 n, Vec3 *t, Vec3 *b)
{
    /* Pick the world axis least aligned with n to avoid a degenerate cross. */
    Vec3 axis;
    if (fabs(n.x) <= fabs(n.y) && fabs(n.x) <= fabs(n.z)) {
        axis = vec3(1.0, 0.0, 0.0);
    } else if (fabs(n.y) <= fabs(n.z)) {
        axis = vec3(0.0, 1.0, 0.0);
    } else {
        axis = vec3(0.0, 0.0, 1.0);
    }
    *t = vec3_normalize(vec3_cross(axis, n));
    *b = vec3_cross(n, *t); /* already unit: n and t are orthonormal */
}

Vec3 sky_sun_disk_dir(Vec3 sun_dir, double radius_deg, double r1, double r2)
{
    /* Degenerate/point-like sun: the hard-shadow direction, unchanged. */
    if (radius_deg <= 0.0) {
        return sun_dir;
    }

    /* Defensive normalisation so the result is always unit length. */
    Vec3 n = vec3_normalize(sun_dir);
    if (vec3_length_sq(n) == 0.0) {
        return vec3(0.0, 1.0, 0.0); /* arbitrary but finite unit vector */
    }

    /* Clamp the two uniform samples to [0, 1] for safety. */
    if (r1 < 0.0) r1 = 0.0; else if (r1 > 1.0) r1 = 1.0;
    if (r2 < 0.0) r2 = 0.0; else if (r2 > 1.0) r2 = 1.0;

    /* Uniform disk sampling: r = sqrt(u) removes the centre bias. */
    double radius_rad = radius_deg * (MATERIAL_PI / 180.0);
    double offset = sqrt(r1) * radius_rad;
    double theta  = 2.0 * MATERIAL_PI * r2;

    Vec3 t, b;
    sky_basis(n, &t, &b);

    /* Small-angle cone: perturb by tan(offset) along (cos t + sin b). */
    Vec3 radial = vec3_add(vec3_scale(t, cos(theta)),
                           vec3_scale(b, sin(theta)));
    Vec3 dir = vec3_add(n, vec3_scale(radial, tan(offset)));
    return vec3_normalize(dir);
}

/* ------------------------------------------------------------------ */
/* Emissive sphere area light (solid-angle cone sampling)              */
/* ------------------------------------------------------------------ */

Vec3 light_sphere_sample_dir(Vec3 w, double cos_alpha_max, double u1,
                             double u2)
{
    Vec3 t, b, dir;

    /* Defensive normalisation so the frame is always well-defined. A zero `w`
     * returns a fixed finite unit vector rather than a NaN. */
    w = vec3_normalize(w);
    if (vec3_length_sq(w) == 0.0) {
        return vec3(0.0, 1.0, 0.0);
    }

    /* Clamp the cone extent and the sample to their valid ranges (also
     * catches NaN via the negated comparisons). */
    if (!(cos_alpha_max >= 0.0)) cos_alpha_max = 0.0; /* also catches NaN */
    else if (cos_alpha_max > 1.0) cos_alpha_max = 1.0;
    if (!(u1 > 0.0)) u1 = 0.0;               /* also catches NaN */
    else if (u1 > MATERIAL_RAND01_MAX) u1 = MATERIAL_RAND01_MAX;
    if (!(u2 > 0.0)) u2 = 0.0;               /* also catches NaN */
    else if (u2 > MATERIAL_RAND01_MAX) u2 = MATERIAL_RAND01_MAX;

    /* Uniform in SOLID ANGLE across the cap: cos_alpha spans
     * [cos_alpha_max, 1] linearly. */
    double cos_alpha = 1.0 - u1 * (1.0 - cos_alpha_max);
    double sin_alpha = sqrt(1.0 - cos_alpha * cos_alpha);
    double phi = 2.0 * MATERIAL_PI * u2;

    sky_basis(w, &t, &b);
    dir = vec3_add(
        vec3_add(vec3_scale(t, sin_alpha * cos(phi)),
                 vec3_scale(b, sin_alpha * sin(phi))),
        vec3_scale(w, cos_alpha));
    return vec3_normalize(dir);
}

/* ------------------------------------------------------------------ */
/* Water                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    double ax, az; /* unit horizontal direction of the wave */
    double w;      /* angular wavenumber 2*PI/lambda        */
    double amp;    /* amplitude                             */
    double speed;  /* temporal angular speed                */
    double phase;  /* phase offset                          */
} Wave;

/* Slopes (amp * w) sum well below 1; see docs/research_water.md sec. 5.1. */
static const Wave WATER_WAVES[] = {
    /* ax     az      w = 2*PI/lambda    amp    speed  phase */
    { 0.9584, 0.2855, 2.0 * MATERIAL_PI / 24.0, 0.1800, 1.0, 0.0 },
    { 0.7071, -0.7071, 2.0 * MATERIAL_PI / 13.0, 0.1000, 1.4, 1.7 },
    { -0.4061, 0.9138, 2.0 * MATERIAL_PI / 7.0, 0.0500, 2.0, 3.1 },
    { 0.2005, 0.9797, 2.0 * MATERIAL_PI / 4.0, 0.0250, 2.6, 0.6 },
};

#define WATER_NWAVES ((int)(sizeof(WATER_WAVES) / sizeof(WATER_WAVES[0])))

Vec3 water_normal(double x, double z, double time)
{
    double dhdx = 0.0;
    double dhdz = 0.0;

    /* --- Directional sine swell with exact analytic derivatives ------- */
    for (int i = 0; i < WATER_NWAVES; ++i) {
        const Wave *wv = &WATER_WAVES[i];
        double u = (wv->ax * x + wv->az * z) * wv->w
                   + wv->phase + time * wv->speed * wv->w;
        double c = cos(u) * wv->amp * wv->w;
        dhdx += c * wv->ax;
        dhdz += c * wv->az;
    }

    /* --- Small high-frequency fBm ripple via central differences ------ */
    const double freq = 0.35;
    const double namp = 0.06;
    /* Central-difference step in WORLD metres. Must be small enough to
     * resolve micro-ripples: the previous 0.5 / freq (~1.43 m, i.e. a
     * ~2.86 m span) low-passed away every high-frequency detail. A fixed
     * ~2 cm step captures the fine structure while staying well above the
     * noise's numerical resolution. */
    const double e = 0.02;
    const unsigned seed = 1337u;

    double hL = noise_fbm2((x - e) * freq, z * freq, 4, 2.0, 0.5, seed);
    double hR = noise_fbm2((x + e) * freq, z * freq, 4, 2.0, 0.5, seed);
    double hD = noise_fbm2(x * freq, (z - e) * freq, 4, 2.0, 0.5, seed);
    double hU = noise_fbm2(x * freq, (z + e) * freq, 4, 2.0, 0.5, seed);

    dhdx += namp * (hR - hL) / (2.0 * e);
    dhdz += namp * (hU - hD) / (2.0 * e);

    /* --- Height-field normal: normalize(-dh/dx, 1, -dh/dz) ------------ */
    Vec3 n = vec3_normalize(vec3(-dhdx, 1.0, -dhdz));

    /* vec3_normalize returns zero only for a degenerate input; the y = 1
     * component guarantees non-degeneracy, so this is a safety net. */
    if (vec3_length_sq(n) < 1e-12) {
        return vec3(0.0, 1.0, 0.0);
    }
    if (n.y < 0.0) {
        n = vec3_neg(n);
    }
    return n;
}

/* Per-channel Beer-Lambert transmittance, clamped to [0, 1].
 * A zero absorption coefficient means the channel is unattenuated (T = 1).
 * Any non-positive / NaN transmittance is coerced to 0 so the caller never
 * propagates NaN or Inf into the image. */
static double water_transmittance(double absorption, double depth)
{
    if (absorption == 0.0) {
        return 1.0;
    }
    double t = exp(-absorption * depth);
    if (!(t > 0.0)) {
        return 0.0; /* catches NaN as well as genuine underflow */
    }
    if (t > 1.0) {
        return 1.0;
    }
    return t;
}

Vec3 water_attenuate(const Material *m, Vec3 inner, double depth)
{
    if (m == NULL) {
        return vec3(0.0, 0.0, 0.0);
    }

    /* Clamp depth at 0 (also coerces NaN depth to 0). */
    double d = depth > 0.0 ? depth : 0.0;

    /* Beer-Lambert transmittance per channel: exp(-absorption_c * depth). */
    Vec3 tr = vec3(water_transmittance(m->absorption.x, d),
                   water_transmittance(m->absorption.y, d),
                   water_transmittance(m->absorption.z, d));

    /* Transmittance-weighted blend of the inner colour toward deep_color:
     *   out_c = inner_c * T_c + deep_color_c * (1 - T_c)
     * depth -> 0   gives out = inner
     * depth -> inf gives out = deep_color
     */
    Vec3 out = vec3(inner.x * tr.x + m->deep_color.x * (1.0 - tr.x),
                    inner.y * tr.y + m->deep_color.y * (1.0 - tr.y),
                    inner.z * tr.z + m->deep_color.z * (1.0 - tr.z));

    /* Final safety: keep every channel finite and non-negative. */
    if (!(out.x >= 0.0)) out.x = 0.0;
    if (!(out.y >= 0.0)) out.y = 0.0;
    if (!(out.z >= 0.0)) out.z = 0.0;

    return out;
}

Vec3 water_depth_tint(const Material *m, double depth)
{
    /* Deprecated multiplicative tint. Preserved as a thin wrapper so existing
     * callers keep compiling; it approximates the new blend with a white
     * inner colour. Migrate to `water_attenuate` for correct behaviour. */
    return water_attenuate(m, vec3(1.0, 1.0, 1.0), depth);
}
