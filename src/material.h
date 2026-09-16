#ifndef MATERIAL_H
#define MATERIAL_H

/*
 * material.h - Surface materials, local shading helpers, procedural sky,
 *              and water-surface normal / depth-tint utilities.
 *
 * This module is intentionally self-contained: it depends ONLY on
 * src/vec3.h (vector math) and src/noise.h (procedural noise). It must NOT
 * include geometry/scene/render headers.
 *
 * All functions are pure: no globals, no dynamic allocation, no I/O.
 * Colors returned here are LINEAR (no gamma encoding); tone mapping happens
 * downstream in the renderer.
 */

#include "vec3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Procedural texture kinds                                            */
/* ------------------------------------------------------------------ */

/*
 * Selector for the per-material procedural texture applied by src/texture.{h,c}.
 * Kept as a plain `int`-valued enum so a `Material` stays a trivially copyable
 * POD and zero-initialises to TEXTURE_NONE (existing scenes stay untextured).
 * `Material.texture_kind` stores one of these values.
 */
typedef enum {
    TEXTURE_NONE    = 0,  /* no texture: albedo is used verbatim          */
    TEXTURE_CHECKER = 1,  /* 3D checkerboard on world position            */
    TEXTURE_STRIPES = 2   /* sinusoidal/triangular bands along world Y    */
} TextureKind;

/* Defaults for the texture fields, shared by the parser and the writer so the
 * "untextured" state has one definition everywhere. `texture=none` with these
 * values is the no-op default and is therefore NEVER emitted by the canonical
 * writer (preserving the byte-identity of scenes/default.scene). */
#define TEXTURE_DEFAULT_KIND      TEXTURE_NONE
#define TEXTURE_DEFAULT_SCALE     1.0
#define TEXTURE_DEFAULT_COLOR_A   ((Vec3){ 1.0, 1.0, 1.0 })
#define TEXTURE_DEFAULT_COLOR_B   ((Vec3){ 0.0, 0.0, 0.0 })

/* ------------------------------------------------------------------ */
/* Surface material                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    Vec3   albedo;        /* base diffuse color                          */
    Vec3   specular;      /* specular tint                               */
    double shininess;     /* Blinn-Phong exponent, e.g. 16..256          */
    double reflectivity;  /* 0..1 mirror mix                             */
    double transparency;  /* 0..1 (0 = opaque)                           */
    double ior;           /* index of refraction, e.g. 1.33 for water    */
    int    is_water;      /* 1 => wave-normal perturbation + depth tint  */
    Vec3   absorption;    /* per-channel Beer-Lambert coefficient        */
    Vec3   deep_color;    /* color of deep transmissive medium           */

    /*
     * Beer-Lambert attenuation flag for GENERAL transmissive materials.
     * 1 => apply per-channel `absorption` / `deep_color` attenuation along the
     *      transmitted ray (reusing water_attenuate) WITHOUT the water wave
     *      normal perturbation that `is_water` also enables.
     * 0 => no medium attenuation (the raw transmitted colour is used), which
     *      is the historical behaviour for every non-water material.
     * Kept separate from `is_water` so glass/gems attenuate while keeping the
     * exact geometric normal. Zero-initialises to 0, so every existing
     * material is unaffected and the default scene stays byte-identical.
     */
    int    beer_lambert;  /* 1 => attenuate along transmission path      */

    /* --- physically-based (PBR) material layer (opt-in) -------------- */
    /*
     * Metallic/roughness material parameters consumed by the opt-in
     * Cook-Torrance microfacet path (see docs/research_pbr_shading.md). Every
     * field below zero-initialises to its legacy-neutral default, so any
     * existing material — and the whole default scene — is unaffected.
     */
    double metallic;      /* 0..1; 1 = conductor, 0 = dielectric (default 0)  */
    double roughness;     /* 0..1 perceptual roughness; 0 = mirror (default 0) */
    Vec3   emissive;      /* self-emission (linear RGB, can exceed 1)          */
    int    pbr;           /* opt-in gate: 0 = legacy Blinn-Phong (default),
                           * 1 = microfacet PBR. 0 keeps behaviour — and the
                           * rendered output — byte-identical.                 */

    /* --- procedural texture (position-modulated albedo) -------------- */
    int    texture_kind;  /* TextureKind: 0 none, 1 checker, 2 stripes   */
    double texture_scale; /* world units per cell; 0 or <=0 => default 1  */
    Vec3   texture_color_a; /* first cell / band colour                  */
    Vec3   texture_color_b; /* second cell / band colour                 */
} Material;

/*
 * Defaults for the opt-in PBR fields. These are the exact zero-initialised
 * values every `Material` gets from `memset`/`calloc`, so the canonical scene
 * writer can treat them as "not present" and emit nothing (keeping
 * scenes/default.scene byte-identical) and the parser can leave the legacy
 * Blinn-Phong path untouched.
 */
#define MATERIAL_DEFAULT_METALLIC   0.0
#define MATERIAL_DEFAULT_ROUGHNESS  0.0
#define MATERIAL_DEFAULT_EMISSIVE   ((Vec3){ 0.0, 0.0, 0.0 })
#define MATERIAL_DEFAULT_PBR        0

/* ------------------------------------------------------------------ */
/* Sky / atmosphere parameters                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    Vec3   sun_dir;           /* unit vector pointing TOWARD the sun     */
    Vec3   sun_color;         /* sun light color (linear, can exceed 1)  */
    Vec3   horizon_color;
    Vec3   zenith_color;
    double gradient_gamma;    /* e.g. 0.5 .. 1.0                         */
    double sun_glow_exponent; /* e.g. 350                                */
    double sun_glow_strength; /* e.g. 0.8                                */
    double cloud_height;      /* world-space height of the cloud layer   */
    double cloud_scale;       /* fBm frequency, e.g. 0.001..0.005        */
    double cloud_coverage;    /* 0.4 .. 0.7                              */
    double cloud_softness;    /* 0.05 .. 0.25                            */
    double cloud_sharpness;   /* extra pow() on density, e.g. 1.0 .. 3.0 */
    int    cloud_octaves;     /* 4 .. 6                                  */
    unsigned seed;

    /*
     * Angular radius of the sun disk, in DEGREES. It drives soft (penumbral)
     * shadows: the renderer samples directions uniformly over a cone of this
     * half-angle around `sun_dir` instead of casting a single hard ray.
     * 0.0 (the default) means a point-like sun -> a hard shadow, exactly
     * preserving the pre-soft-shadow behaviour and output.
     */
    double sun_radius;
} SkyParams;

/* Default angular radius of the sun disk, in degrees (0 = hard shadow). The
 * canonical scene writer emits `sun_radius` ONLY when it differs from this
 * value, so the default scene stays byte-identical. */
#define SKY_DEFAULT_SUN_RADIUS 0.0

/* Fill `p` with a pleasant outdoor-day default setup. */
void sky_default_params(SkyParams *p);

/* Set the procedural-texture fields of `m` to their no-op defaults
 * (TEXTURE_NONE, scale 1, colors A=white / B=black). Leaves every other field
 * untouched. Used by the parser's material-block defaults and mirrored by the
 * canonical writer's default check. Safe on NULL. */
void material_texture_defaults(Material *m);

/* ------------------------------------------------------------------ */
/* Fresnel                                                             */
/* ------------------------------------------------------------------ */

/* Schlick's approximation: F0 + (1 - F0) * (1 - cos_theta)^5.
 * `cos_theta` is clamped to [0, 1] for safety. */
double fresnel_schlick(double cos_theta, double f0);

/* Per-channel Schlick Fresnel (useful for colored conductors). */
Vec3 fresnel_schlick_rgb(double cos_theta, Vec3 f0);

/* ------------------------------------------------------------------ */
/* Local shading                                                       */
/* ------------------------------------------------------------------ */

/* Blinn-Phong diffuse + specular contribution for one light.
 * N, L, V are unit vectors; `light_color` is the already-attenuated
 * incoming light color. Returns outgoing radiance (linear, may exceed 1). */
Vec3 material_shade_local(const Material *m, Vec3 N, Vec3 L, Vec3 V,
                          Vec3 light_color);

/*
 * Physically-based Cook-Torrance microfacet contribution for one light
 * (opt-in via `m->pbr`; see docs/research_pbr_shading.md).
 *
 * Mirrors `material_shade_local`'s signature/contract exactly: N, L, V are
 * unit vectors, `light_color` is the already-attenuated incoming light colour,
 * and the return value is outgoing LINEAR radiance (may exceed 1). It is a pure
 * function (no globals, no I/O) so it preserves the renderer's thread/tile
 * determinism.
 *
 * Model:
 *   F0    = mix(vec3(0.04), m->albedo, clamp01(m->metallic))
 *   F     = fresnel_schlick_rgb(NdotV, F0)
 *   D     = GGX/Trowbridge-Reitz with alpha = max(roughness^2, 1e-4)
 *   G     = Smith-Schlick-GGX, k = (roughness+1)^2 / 8
 *   f_spec= D * G * F / (4 * NdotL * NdotV)
 *   f_diff= kD * albedo / PI,  kD = (1 - F) * (1 - metallic)
 *   L_o   = light_color * NdotL * (f_diff + f_spec)
 *
 * Energy conservation: the diffuse lobe carries the (1 - F) compensation
 * (kS = F is reflected specularly), so for `metallic` in [0,1] and `F0` in
 * [0,1] the combined BRDF integrated over the hemisphere never exceeds 1,
 * i.e. the reflected radiance cannot exceed the incident `light_color` scaled
 * by NdotL. The (1 - metallic) weight removes the diffuse lobe for a pure
 * conductor, and the 1/PI factor keeps the Lambert lobe's integral at
 * (1 - metallic) * mean(1 - F) * albedo <= (1 - metallic) * albedo.
 *
 * Returns black when `m == NULL` or NdotL <= 0 (light behind the surface),
 * matching `material_shade_local`. `m->emissive` is NOT included here: the
 * caller adds it ONCE per shaded hit (see render.c), never per light.
 */
Vec3 material_shade_pbr(const Material *m, Vec3 N, Vec3 L, Vec3 V,
                        Vec3 light_color);

/* ------------------------------------------------------------------ */
/* Glossy (roughness-blurred) reflection sampling                      */
/* ------------------------------------------------------------------ */

/*
 * Roughness values at or below this are treated as a perfect mirror. The
 * recursive Whitted reflection then uses the EXACT sharp direction with a
 * single ray, preserving the legacy output bit-for-bit. Shared by the renderer
 * gate and the sampler short-circuit so the two can never disagree.
 */
#define GLOSSY_ROUGHNESS_EPSILON 1e-6

/*
 * The single GGX alpha convention shared by the PBR shading and the glossy
 * reflection sampler: alpha = max(clamp01(roughness)^2, PBR_ALPHA_MIN).
 * Keeping one definition guarantees the blurred reflection lobe has the same
 * width as the direct-sun Cook-Torrance highlight. Pure function.
 */
double material_roughness_to_alpha(double roughness);

/*
 * Importance-sample a GGX/Trowbridge-Reitz half-vector for the material's
 * perceptual `roughness` (via `material_roughness_to_alpha`), reflect the
 * incident unit vector `incident` (pointing away from the eye, into the
 * surface) about it, and return a UNIT reflection direction. The tangent frame
 * is the existing `sky_basis` frame around the unit surface normal `N`.
 *
 * `r1`, `r2` are deterministic pseudo-random values in [0, 1) supplied by the
 * caller's PRNG (e.g. `render_rand01`); the function is PURE: identical inputs
 * always produce bit-identical output (no hidden state, no rand()/clock), so
 * the renderer stays byte-identical for any thread count.
 *
 *   - `m == NULL`, or `roughness <= GLOSSY_ROUGHNESS_EPSILON` -> the EXACT
 *     mirror direction `vec3_reflect(incident, N)` (single-ray legacy path).
 *   - if the sampled `R` would fall below the surface (`dot(R, N) <= 0`, only
 *     possible for very rough near-grazing hits) -> the exact mirror direction,
 *     so the traced ray always stays on the correct side of the surface.
 */
Vec3 material_sample_glossy_dir(const Material *m, Vec3 N, Vec3 incident,
                                double r1, double r2);

/* Hemisphere-approximated ambient term derived from the sky parameters. */
Vec3 material_ambient(const Material *m, Vec3 N, const SkyParams *sky);

/* ------------------------------------------------------------------ */
/* Procedural sky                                                      */
/* ------------------------------------------------------------------ */

/* Full procedural sky: horizon gradient + sun glow + fBm clouds.
 * `dir` must be a unit vector. Pure function of direction and params. */
Vec3 sky_sample(Vec3 dir, const SkyParams *sky);

/* ------------------------------------------------------------------ */
/* Sun disk (soft shadows)                                             */
/* ------------------------------------------------------------------ */

/*
 * Build a unit direction toward a uniformly-sampled point on the sun disk:
 * a cone of half-angle `radius_deg` DEGREES around the unit vector `sun_dir`.
 *
 * `r1` and `r2` are deterministic pseudo-random values in [0, 1) supplied by
 * the caller's PRNG (e.g. `render_rand01`), so this function is pure: the same
 * inputs always yield bit-identical output, with no hidden state and no
 * rand()/clock. A uniform disk is produced with `r = sqrt(r1)` and
 * `theta = 2*pi*r2`; the small-angle approximation is used for the cone offset.
 *
 * Edge cases:
 *   - `radius_deg <= 0` returns `sun_dir` unchanged (hard shadow path).
 *   - `sun_dir` is normalised defensively; a zero `sun_dir` returns a fixed
 *     unit vector so the result is always finite and unit-length.
 *   - `r1`/`r2` are clamped to [0, 1] for safety.
 *
 * The returned vector is always normalised (unit length).
 */
Vec3 sky_sun_disk_dir(Vec3 sun_dir, double radius_deg, double r1, double r2);

/* ------------------------------------------------------------------ */
/* Emissive sphere area light (solid-angle cone sampling)              */
/* ------------------------------------------------------------------ */

/*
 * Sample a UNIT direction from a shading point toward a uniformly distributed
 * direction on the VISIBLE spherical cap of an emissive sphere light.
 *
 * `w` is the unit direction from the shading point to the sphere CENTRE and
 * `cos_alpha_max` is cos of the cone half-angle subtended by the sphere:
 * cos(alpha_max) = sqrt(1 - (R/dc)^2). The visible cap has solid angle
 * Omega = 2*pi*(1 - cos_alpha_max), so uniform-in-solid-angle sampling gives
 * pdf(w_i) = 1/Omega and the caller's estimator multiplies by Omega.
 *
 * `u1`, `u2` are deterministic pseudo-random values in [0, 1) supplied by the
 * caller's PRNG (e.g. render_rand01), so this function is PURE: identical
 * inputs always produce bit-identical output (no hidden state, no rand()). The
 * tangent frame is the shared `sky_basis` frame around `w`.
 *
 *   cos_alpha = 1 - u1 * (1 - cos_alpha_max)   (uniform in solid angle)
 *   sin_alpha = sqrt(max(0, 1 - cos_alpha^2))
 *   phi       = 2*pi*u2
 *   w_i       = normalize(cos_alpha * w + sin_alpha * (cos(phi)*t + sin(phi)*b))
 *
 * Every returned direction therefore lies within the cone of half-angle
 * alpha_max, i.e. the ray from the shading point along w_i is guaranteed to
 * hit the emitter's sphere (the visibility test is then a pure occlusion
 * query). The result is always normalised (unit length). Degenerate inputs
 * (`u1 <= 0`, `cos_alpha_max` outside [0,1], zero `w`) are handled safely
 * without NaN.
 */
Vec3 light_sphere_sample_dir(Vec3 w, double cos_alpha_max, double u1,
                             double u2);

/* ------------------------------------------------------------------ */
/* Water                                                               */
/* ------------------------------------------------------------------ */

/* Perturbed unit normal of a water surface at world position (x, z) at
 * time `time`. Sum of directional sine waves (analytic derivatives) plus a
 * small fBm ripple. Returns a unit vector pointing generally +Y. */
Vec3 water_normal(double x, double z, double time);

/* Physically-correct depth attenuation (Beer-Lambert) applied to the
 * transmitted `inner` colour as it travels `depth` metres through the
 * medium, per channel:
 *
 *     T_c       = exp(-absorption_c * depth)      (T_c = 1 if absorption_c == 0)
 *     result_c  = inner_c * T_c + deep_color_c * (1 - T_c)
 *
 * This converges to `deep_color` as depth -> infinity and to `inner` as
 * depth -> 0, i.e. it is a proper transmittance-weighted blend rather than
 * a multiplicative tint. `depth` is clamped at 0 and `T_c` is clamped to
 * [0, 1] so the result is always finite (no NaN/Inf). Pure function. */
Vec3 water_attenuate(const Material *m, Vec3 inner, double depth);

/* DEPRECATED: legacy multiplicative depth tint, kept only so existing
 * callers keep compiling. New code should use `water_attenuate`, which
 * correctly converges to `deep_color` instead of to black. */
Vec3 water_depth_tint(const Material *m, double depth);

#ifdef __cplusplus
}
#endif

#endif /* MATERIAL_H */
