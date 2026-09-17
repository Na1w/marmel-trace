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

    p->sun_radius        = SKY_DEFAULT_SUN_RADIUS;
    p->star_intensity    = SKY_DEFAULT_STAR_INTENSITY;
    p->star_density      = SKY_DEFAULT_STAR_DENSITY;
    p->nebula_intensity  = SKY_DEFAULT_NEBULA_INTENSITY;
    p->galaxy_intensity  = SKY_DEFAULT_GALAXY_INTENSITY;
    p->galaxy_dir        = SKY_DEFAULT_GALAXY_DIR;
    p->nebula_dir        = SKY_DEFAULT_NEBULA_DIR;
    p->galaxy_tilt       = SKY_DEFAULT_GALAXY_TILT;
    p->galaxy_roll       = SKY_DEFAULT_GALAXY_ROLL;
}

void fog_default_params(FogParams *p)
{
    if (p == NULL) {
        return;
    }
    p->density            = FOG_DEFAULT_DENSITY;
    p->color              = FOG_DEFAULT_COLOR;
    p->height             = FOG_DEFAULT_HEIGHT;
    p->height_falloff     = FOG_DEFAULT_HEIGHT_FALLOFF;
    p->inscatter_strength = FOG_DEFAULT_INSCATTER_STRENGTH;
    p->sun_anisotropy     = FOG_DEFAULT_SUN_ANISOTROPY;
    p->noise_scale        = FOG_DEFAULT_NOISE_SCALE;
    p->noise_amount       = FOG_DEFAULT_NOISE_AMOUNT;
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

    /* --- Procedural stars (opt-in) ----------------------------------- */
    if (sky->star_intensity > 0.0) {
        double freq = sky->star_density > 0.0 ? sky->star_density : 250.0;
        Vec3 g = vec3_scale(dir, freq);
        int ix = (int)floor(g.x);
        int iy = (int)floor(g.y);
        int iz = (int)floor(g.z);

        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int cx = ix + dx;
                    int cy = iy + dy;
                    int cz = iz + dz;
                    /* Wang-hash mix */
                    unsigned h = sky->seed ^ 0x57A85u;
                    h += (unsigned)cx * 0x85ebca6bU;
                    h += (unsigned)cy * 0xc2b2ae35U;
                    h += (unsigned)cz * 0x27d4eb2fU;
                    h ^= h >> 16; h *= 0x7feb352dU;
                    h ^= h >> 15; h *= 0x846ca68bU;
                    h ^= h >> 16;

                    if ((h & 0xFF) < 14) { /* ~5.5% probability */
                        double jx = (double)cx + 0.1 + 0.8 * ((double)((h >> 8) & 0xFF) / 255.0);
                        double jy = (double)cy + 0.1 + 0.8 * ((double)((h >> 16) & 0xFF) / 255.0);
                        double jz = (double)cz + 0.1 + 0.8 * ((double)((h >> 24) & 0xFF) / 255.0);
                        double d2 = (g.x - jx)*(g.x - jx) + (g.y - jy)*(g.y - jy) + (g.z - jz)*(g.z - jz);
                        const double r_star = 0.085; /* sub-pixel pinprick (~0.8 px) */
                        if (d2 < r_star * r_star) {
                            double d = sqrt(d2);
                            double s = 1.0 - d / r_star;
                            s = s * s;
                            unsigned mag_val = h % 1000;
                            double brightness = (mag_val > 985) ? 6.0 : ((mag_val > 920) ? 2.8 : ((mag_val > 700) ? 1.2 : 0.5));
                            Vec3 star_col;
                            if (h % 9 == 0) star_col = vec3(0.75, 0.88, 1.30);      /* Hot O/B star */
                            else if (h % 13 == 0) star_col = vec3(1.30, 0.85, 0.50); /* K/M giant */
                            else if (h % 7 == 0) star_col = vec3(1.15, 1.10, 0.85);  /* F/G star */
                            else star_col = vec3(1.0, 1.0, 1.0);                     /* Pure white */
                            base = vec3_add(base, vec3_scale(star_col, s * brightness * sky->star_intensity));
                        }
                    }
                }
            }
        }
    }

    /* --- Procedural 3D Volumetric Interstellar Emission Complex ------ */
    /* Modeled directly after actual narrowband astrophotography (nebula.tiff, nebula2.tiff):
     * - Vast interstellar molecular complex (North America & Pelican complex NGC 7000 / IC 5070)
     * - Deep hydrogen-alpha (656.3 nm) crimson gas clouds permeating space
     * - Energetic ionization shock fronts / bright walls (the Cygnus Wall)
     * - Cold molecular dust lanes (LDN 935) creating deep dark absorption silhouettes
     * - True 3D raymarching with domain-warped fBm turbulence */
    if (sky->nebula_intensity > 0.0) {
        Vec3 ndir = vec3_normalize(sky->nebula_dir);
        if (vec3_length_sq(ndir) > 1e-6) {
            Vec3 nup = (fabs(ndir.y) < 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            Vec3 nt1 = vec3_normalize(vec3_cross(ndir, nup));
            Vec3 nt2 = vec3_cross(ndir, nt1);

            double u = vec3_dot(dir, nt1);
            double v = vec3_dot(dir, nt2);
            double w_dir = vec3_dot(dir, ndir);

            /* Authentic telescope angular scale for emission complex */
            const double R_field = 0.078;
            double r_ang = sqrt(u * u + v * v);

            if (w_dir > 0.985 && r_ang < R_field) {
                /* Depth interval for 3D cosmic slab */
                double t_enter = 0.88;
                double t_exit  = 1.12;
                const int num_steps = 26;
                double dt = (t_exit - t_enter) / (double)num_steps;

                /* Guaranteed smooth edge feathering to prevent any circular knife-edge */
                double edge_fade = 1.0 - smoothstep(0.35, 0.95, r_ang / R_field);
                edge_fade = edge_fade * edge_fade;

                Vec3 accum_nebula = vec3(0.0, 0.0, 0.0);
                double transmittance = 1.0;

                for (int s = 0; s < num_steps; ++s) {
                    double t_cur = t_enter + (s + 0.5) * dt;
                    Vec3 p_world = vec3_scale(dir, t_cur);
                    Vec3 p_rel = vec3_sub(p_world, ndir);

                    double nx = vec3_dot(p_rel, nt1) / R_field;
                    double ny = vec3_dot(p_rel, nt2) / R_field;
                    double nz = vec3_dot(p_rel, ndir) / 0.10;

                    /* Rotate coordinates by 35 degrees to align Cygnus Wall shock front */
                    double rx =  0.82 * nx + 0.57 * ny;
                    double ry = -0.57 * nx + 0.82 * ny;

                    /* Soft z-axis containment */
                    double z_decay = exp(-nz * nz * 4.0);

                    /* Higher spatial frequency for intricate multi-scale fractal wisps */
                    double fx = rx * 6.5;
                    double fy = ry * 6.5;
                    double fz = nz * 6.5;

                    /* Domain warping creates swirling, turbulent cosmic fluid motion */
                    double qx = noise_fbm3(fx + 1.4, fy + 0.8, fz, 3, 2.0, 0.5, 0x57415250u);
                    double qy = noise_fbm3(fx + 4.2, fy + 2.6, fz, 3, 2.0, 0.5, 0x45444459u);
                    double qz = noise_fbm3(fx - 2.8, fy + 5.1, fz, 3, 2.0, 0.5, 0x5A574152u);

                    double wx = fx + 0.70 * qx;
                    double wy = fy + 0.70 * qy;
                    double wz = fz + 0.70 * qz;

                    /* 5-octave turbulent density field with domain warping */
                    double fbm1 = noise_fbm3(wx, wy, wz, 5, 2.15, 0.50, 0x4E454255u);
                    double fbm2 = noise_fbm3(wx * 2.2 + 2.4, wy * 2.2 - 1.8, wz * 2.2, 4, 2.1, 0.5, 0x53484545u);

                    /* Folded interstellar sheets / ionization curtains */
                    double sheet1 = exp(-fbm1 * fbm1 * 18.0);
                    double sheet2 = exp(-fbm2 * fbm2 * 22.0);
                    double curtains = sheet1 * 0.75 + sheet2 * 0.45;

                    /* Cygnus Wall: prominent glowing ionization front in NGC 7000 */
                    double wall_coord = ry + 0.28 * rx * rx - 0.04;
                    double wall_front = exp(-wall_coord * wall_coord * 36.0) * sheet1;

                    /* Large-scale cloud clumping with high threshold:
                     * Carves deep dark voids, bays (Gulf of Mexico), and distinct cloud masses! */
                    double clump = noise_fbm3(rx * 2.2 + 0.7, ry * 2.2 + 2.9, nz * 2.2, 3, 2.0, 0.5, 0x434C554Du);
                    double clump_mask = smoothstep(0.46, 0.78, clump * 0.5 + 0.5);

                    /* Total glowing gas density */
                    double gas = (curtains * 0.70 + wall_front * 0.90) * clump_mask * edge_fade * z_decay;
                    if (gas < 0.005) continue;

                    /* 3D Cold Molecular Dust Veins (LDN 935 Gulf of Mexico dark rift) */
                    double dust_fbm = noise_fbm3(fx * 1.4 - 2.1 + 0.35 * qx, fy * 1.4 + 1.8 + 0.35 * qy, fz * 1.4, 4, 2.0, 0.5, 0x44555354u);
                    double dust_rift = smoothstep(0.38, 0.72, dust_fbm * 0.5 + 0.5);
                    double dust_channel = exp(-(rx * rx + (ry - 0.08) * (ry - 0.08)) * 4.0);
                    double dust_density = dust_rift * dust_channel * edge_fade * z_decay;

                    /* Astrophotographic Narrowband Colors:
                     * Hydrogen-alpha (656.3 nm) monochromatic ruby crimson:
                     * - Deep H-alpha crimson base: (0.85, 0.08, 0.16)
                     * - Bright ionization shock ridge: (1.25, 0.18, 0.26)
                     * - Cygnus Wall intense excitation: (1.45, 0.25, 0.32)
                     * - [O III] cyan ionization veil: (0.10, 0.60, 0.75) */
                    Vec3 ha_base = vec3(0.85, 0.08, 0.16);
                    Vec3 ha_bright = vec3(1.25, 0.18, 0.26);
                    Vec3 ha_wall = vec3(1.45, 0.25, 0.32);
                    Vec3 o3_tint = vec3(0.10, 0.60, 0.75);

                    double wall_blend = clamp01(wall_front * 2.0);
                    Vec3 gas_color = vec3_lerp(ha_base, ha_bright, curtains);
                    if (wall_blend > 0.2) {
                        gas_color = vec3_lerp(gas_color, ha_wall, wall_blend * 0.65);
                    }
                    /* Delicate [O III] cyan ionization in dense shock pockets */
                    double o3_mix = smoothstep(0.45, 0.85, gas) * (1.0 - wall_blend * 0.5) * 0.24;
                    gas_color = vec3_lerp(gas_color, o3_tint, o3_mix);

                    Vec3 emission = vec3_scale(gas_color, gas * 0.60);

                    /* Volumetric extinction (Beer-Lambert):
                     * Foreground dust absorbs background emission, giving real 3D depth */
                    double sigma_a = (gas * 0.25 + dust_density * 28.0);
                    double step_tau = sigma_a * dt;
                    double step_trans = exp(-step_tau);

                    double integ_factor = (sigma_a > 1e-6) ? ((1.0 - step_trans) / sigma_a) : dt;
                    accum_nebula = vec3_add(accum_nebula, vec3_scale(emission, transmittance * integ_factor * 11.0));
                    transmittance *= step_trans;

                    if (transmittance < 0.01) break;
                }

                base = vec3_add(base, vec3_scale(accum_nebula, sky->nebula_intensity));
            }
        }
    }

    /* --- Procedural 3D Volumetric Distant Spiral Galaxy ---------------- */
    /* Modeled directly after genuine telescope deep-sky exposure (real_space.tiff):
     * - True 3D volumetric raymarching through thick galactic disk & spheroidal bulge
     * - 3D multi-octave fBm turbulence along logarithmic spiral arms
     * - 3D spheroidal nucleus & bulge (Population II golden starlight)
     * - 3D Beer-Lambert dust extinction casting realistic near-side silhouettes
     * - Seamless feathering into deep space with zero edge artifact */
    if (sky->galaxy_intensity > 0.0) {
        Vec3 gdir = vec3_normalize(sky->galaxy_dir);
        if (vec3_length_sq(gdir) > 1e-6) {
            Vec3 gup = (fabs(gdir.y) < 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            Vec3 gt1 = vec3_normalize(vec3_cross(gdir, gup));
            Vec3 gt2 = vec3_cross(gdir, gt1);

            /* Orientation roll and inclination angle */
            double roll = sky->galaxy_roll * (M_PI / 180.0);
            double cos_r = cos(roll);
            double sin_r = sin(roll);
            Vec3 disk_u = vec3_add(vec3_scale(gt1, cos_r), vec3_scale(gt2, -sin_r));
            Vec3 disk_v = vec3_add(vec3_scale(gt1, sin_r), vec3_scale(gt2, cos_r));

            double u = vec3_dot(dir, disk_u);
            double v = vec3_dot(dir, disk_v);
            double w_dir = vec3_dot(dir, gdir);

            const double R_disk = 0.016;  /* Authentic telescope angular scale from real_space.tiff */
            double inc_rad = sky->galaxy_tilt * (M_PI / 180.0);
            double sin_inc = sin(inc_rad);
            double cos_inc = cos(inc_rad);
            double ellip_r = sqrt(u * u + (v / cos_inc) * (v / cos_inc));

            if (w_dir > 0.985 && ellip_r < R_disk * 1.45) {
                /* Depth interval for 3D galactic slab */
                double denom = w_dir * cos_inc - v * sin_inc;
                double t_mid = (fabs(denom) > 1e-4) ? (cos_inc / denom) : 1.0;

                const double t_half = 0.0075;
                double t_enter = t_mid - t_half;
                double t_exit  = t_mid + t_half;
                const int num_gal_steps = 32;
                double dt = (t_exit - t_enter) / (double)num_gal_steps;

                Vec3 accum_gal = vec3(0.0, 0.0, 0.0);
                double gal_transmittance = 1.0;

                for (int s = 0; s < num_gal_steps; ++s) {
                    double t_cur = t_enter + (s + 0.5) * dt;
                    Vec3 p_world = vec3_scale(dir, t_cur);
                    Vec3 p_rel = vec3_sub(p_world, gdir);

                    double gu = vec3_dot(p_rel, disk_u);
                    double gv = vec3_dot(p_rel, disk_v);
                    double gw = vec3_dot(p_rel, gdir);

                    /* Galaxy intrinsic 3D coordinates */
                    double x_gal = gu;
                    double y_gal = gv * cos_inc + gw * sin_inc;
                    double z_gal = -gv * sin_inc + gw * cos_inc;

                    double r_cyl = sqrt(x_gal * x_gal + y_gal * y_gal);
                    double gn = r_cyl / R_disk;
                    if (gn > 1.45) continue;

                    double phi = atan2(y_gal, x_gal);

                    /* 1. True 3D Spheroidal Bulge (thick in z, round in 3D) */
                    const double q_bulge = 0.55;
                    double r_bulge = sqrt((x_gal / R_disk) * (x_gal / R_disk) + 
                                          (y_gal / R_disk) * (y_gal / R_disk) + 
                                          (z_gal / (R_disk * q_bulge)) * (z_gal / (R_disk * q_bulge)));
                    
                    /* Sharp stellar nucleus + diffuse spheroidal bulge */
                    double nuc_core  = exp(-r_bulge * 45.0) * 16.0;
                    double nuc_bulge = exp(-r_bulge * 9.0) * 3.6 + exp(-r_bulge * 3.8) * 1.1;

                    /* 2. 3D Stellar Disk Profile: sech^2(z / z0) * exp(-r / Rd) */
                    const double z0_star = 0.0013;
                    double sech_star = 1.0 / cosh(z_gal / z0_star);
                    double sech2_star = sech_star * sech_star;
                    double disk_base = exp(-gn * 2.4) * sech2_star;

                    /* 3. Multi-arm Logarithmic Spiral Density Waves with 3D Domain Warping */
                    double pitch = 2.4;
                    double log_r = log(fmax(0.015, gn + 0.03));
                    double spiral_phase = phi - pitch * log_r;

                    /* 3D domain warping creates natural swirling filaments */
                    double warp = noise_fbm3(x_gal * 450.0, y_gal * 450.0, z_gal * 900.0, 3, 2.0, 0.5, 0x50495241u);
                    double spiral_warped = spiral_phase + 0.35 * warp;

                    /* Primary 2-arm mode + secondary branching spurs */
                    double arm_wave1 = pow_nonneg(clamp01(0.5 * (1.0 + cos(2.0 * spiral_warped))), 1.5);
                    double arm_wave2 = pow_nonneg(clamp01(0.5 * (1.0 + cos(4.0 * spiral_warped - 0.7))), 2.0);
                    
                    /* Inner spiral ring / pseudobar at gn ~ 0.22 - 0.38 */
                    double ring_dist = fabs(gn - 0.28) / 0.12;
                    double inner_ring = exp(-ring_dist * ring_dist * 3.0) * 0.60;

                    double arm_density = (arm_wave1 * 0.75 + arm_wave2 * 0.35 + inner_ring);

                    /* 3D clumping: OB associations & young star-forming clusters along arms */
                    double fbm_clump = noise_fbm3(x_gal * 800.0 + 2.1, y_gal * 800.0 - 1.8, z_gal * 1600.0, 4, 2.15, 0.5, 0x4B4E4F54u);
                    double knot_val = smoothstep(0.38, 0.78, fbm_clump * 0.5 + 0.5);
                    double ob_knots = knot_val * knot_val * arm_density * smoothstep(0.10, 0.30, gn);

                    /* Spiral arms modulate the continuous stellar disk (density waves) */
                    double arm_mod = 0.18 + 0.82 * arm_density + 2.4 * ob_knots;
                    double stars = disk_base * arm_mod;

                    /* 4. Dramatic 3D Volumetric Dust Lanes (Beer-Lambert extinction)
                     * Dust is concentrated on inner trailing edge of spiral arms in a thin layer */
                    const double z0_dust = 0.00035;
                    double sech_dust = 1.0 / cosh(z_gal / z0_dust);
                    double sech2_dust = sech_dust * sech_dust;

                    double dust_phase = spiral_phase - 0.35 + 0.20 * warp;
                    double dust_wave = pow_nonneg(clamp01(0.5 * (1.0 + cos(2.0 * dust_phase))), 2.2);
                    double dust_turb = noise_fbm3(x_gal * 950.0 - 4.5, y_gal * 950.0 + 3.2, z_gal * 1900.0, 4, 2.0, 0.5, 0x44555354u);
                    double dust_lane = dust_wave * smoothstep(0.20, 0.70, dust_turb * 0.5 + 0.5);
                    double dust = dust_lane * exp(-gn * 1.6) * smoothstep(0.06, 0.20, gn) * sech2_dust;

                    /* Authentic Astrophotographic Colors:
                     * Nucleus: warm golden-yellow starlight (Population II)
                     * Disk/Arms: soft bluish-white starlight (Population I)
                     * OB associations: bright sparkling starlight clusters */
                    Vec3 c_nuc  = vec3(1.45, 1.25, 0.90);
                    Vec3 c_disk = vec3(0.78, 0.90, 1.25);
                    Vec3 c_knot = vec3(1.10, 1.35, 1.95);

                    Vec3 emis = vec3_add(
                        vec3_scale(c_nuc, (nuc_core + nuc_bulge)),
                        vec3_add(vec3_scale(c_disk, stars * 2.8),
                                 vec3_scale(c_knot, ob_knots * disk_base * 6.5))
                    );

                    /* Physical Beer-Lambert extinction:
                     * Powerful dust extinction silhouettes foreground arms across the bulge */
                    double sigma_a = stars * 1.5 + dust * 2200.0;
                    double step_tau = sigma_a * dt;
                    double step_trans = exp(-step_tau);

                    double integ = (sigma_a > 1e-5) ? ((1.0 - step_trans) / sigma_a) : dt;
                    accum_gal = vec3_add(accum_gal, vec3_scale(emis, gal_transmittance * integ * 22.0));
                    gal_transmittance *= step_trans;

                    if (gal_transmittance < 0.01) break;
                }

                /* Smooth edge feathering: seamless fade into space background */
                double edge_fade = 1.0 - smoothstep(1.0, 1.45, ellip_r / R_disk);
                base = vec3_add(base, vec3_scale(accum_gal, edge_fade * edge_fade * sky->galaxy_intensity));
            }
        }
    }

    /* Final safety: keep components finite and non-negative. */
    if (!(base.x >= 0.0)) base.x = 0.0;
    if (!(base.y >= 0.0)) base.y = 0.0;
    if (!(base.z >= 0.0)) base.z = 0.0;

    return base;
}

/* ------------------------------------------------------------------ */
/* Atmospheric Fog & Smoke                                             */
/* ------------------------------------------------------------------ */

void fog_segment(const FogParams *fog, const SkyParams *sky, Ray ray,
                 double dist, double *transmittance, Vec3 *inscatter)
{
    if (transmittance) *transmittance = 1.0;
    if (inscatter) *inscatter = vec3(0.0, 0.0, 0.0);

    if (fog == NULL || fog->density <= 0.0 || dist <= 1e-7) {
        return;
    }

    double rho0 = fog->density;
    double lambda = (fog->height_falloff > 0.0) ? fog->height_falloff : 0.0;
    double y0 = ray.origin.y - fog->height;
    double dy = ray.dir.y;
    double tau = 0.0;
    int is_sky = (dist >= 1e20);

    if (lambda <= 1e-6) {
        /* Uniform distance fog (constant density rho0) */
        tau = is_sky ? 1e6 : (rho0 * dist);
    } else {
        /* Exponential height fog: rho(y) = rho0 * exp(-lambda * (y - height)) */
        double base_density = rho0 * exp(-lambda * y0);

        if (is_sky) {
            if (dy > 1e-5) {
                tau = base_density / (lambda * dy);
            } else {
                tau = 1e6;
            }
        } else {
            if (fabs(dy) < 1e-5) {
                tau = base_density * dist;
            } else {
                double term = (1.0 - exp(-lambda * dy * dist)) / (lambda * dy);
                tau = base_density * term;
            }
        }
    }

    if (tau < 0.0) tau = 0.0;

    /* Optional 3D procedural noise turbulence */
    if (fog->noise_amount > 1e-4 && fog->noise_scale > 1e-4) {
        double eval_dist = is_sky ? 20.0 : (dist > 50.0 ? 50.0 : dist * 0.5);
        Vec3 p = vec3_add(ray.origin, vec3_scale(ray.dir, eval_dist));
        double n = noise_fbm3(p.x * fog->noise_scale,
                              p.y * fog->noise_scale,
                              p.z * fog->noise_scale,
                              3, 2.0, 0.5, 1337u);
        double turb = 0.5 * (n + 1.0);
        if (turb < 0.0) turb = 0.0;
        if (turb > 1.0) turb = 1.0;
        double mod = (1.0 - fog->noise_amount) + fog->noise_amount * turb * 2.0;
        tau *= mod;
    }

    double T = exp(-tau);
    if (T < 0.0) T = 0.0;
    if (T > 1.0) T = 1.0;

    Vec3 inscatter_col = fog->color;
    if (sky != NULL && fog->inscatter_strength > 0.0) {
        double g = fog->sun_anisotropy;
        if (g < -0.95) g = -0.95;
        if (g >  0.95) g =  0.95;
        double cos_theta = vec3_dot(ray.dir, sky->sun_dir);
        double denom = 1.0 + g * g - 2.0 * g * cos_theta;
        if (denom < 1e-4) denom = 1e-4;
        double phase = (1.0 - g * g) / (denom * sqrt(denom));
        Vec3 sun_glow = vec3_scale(sky->sun_color, phase * fog->inscatter_strength);
        inscatter_col = vec3_add(inscatter_col, sun_glow);
    }

    if (transmittance) *transmittance = T;
    if (inscatter) *inscatter = vec3_scale(inscatter_col, 1.0 - T);
}

Vec3 fog_apply(const FogParams *fog, const SkyParams *sky, Ray ray,
               double dist, Vec3 surface_color)
{
    if (fog == NULL || fog->density <= 0.0) {
        return surface_color;
    }
    double T = 1.0;
    Vec3 inscatter = vec3(0.0, 0.0, 0.0);
    fog_segment(fog, sky, ray, dist, &T, &inscatter);
    return vec3_add(vec3_scale(surface_color, T), inscatter);
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
