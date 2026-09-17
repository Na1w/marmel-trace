/*
 * scene_desc_write.c - Canonical scene writer.
 *
 * Emits the canonical text form of a SceneDesc exactly as frozen in
 * docs/scene_format.md §9 (canonical formatting) and exercised by §7 (worked
 * example). The output is comment-free apart from the single canonical header
 * line, uses LF endings, 4-space body indentation, shortest round-tripping numbers, and a
 * trailing newline, so that parse(write(S)) reproduces S and write(parse(...))
 * is a fixed point.
 *
 * Structure: one short static helper per block type plus a handful of tiny
 * emit primitives. No third-party dependencies, C11 standard library only.
 * No global mutable state; all diagnostics flow through a small Writer context.
 */

#include "scene_desc.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Single canonical header line (§9.1); pure ASCII, no timestamp (§9.10). */
static const char SD_HEADER_COMMENT[] = "# scene description (canonical)";

/* ------------------------------------------------------------------ */
/* Writer context                                                      */
/* ------------------------------------------------------------------ */

/*
 * Carries the output stream plus the diagnostic buffer so every helper can
 * report a failure without threading six parameters through the call chain.
 * `failed` latches on the first error; all emit helpers become no-ops after.
 */
typedef struct {
    FILE  *fp;
    const char *path;
    char  *errbuf;
    size_t errlen;
    int    failed;
    const char *indent; /* line prefix: "" at top level, "    " in a block */
} Writer;

static void w_fail(Writer *w, const char *msg)
{
    if (w->failed)
        return;
    w->failed = 1;
    if (w->errbuf != NULL && w->errlen > 0)
        snprintf(w->errbuf, w->errlen, "%s: error: %s", w->path, msg);
}

/* ------------------------------------------------------------------ */
/* Low-level emit primitives                                           */
/* ------------------------------------------------------------------ */

static void w_raw(Writer *w, const char *s)
{
    if (w->failed)
        return;
    if (fputs(s, w->fp) < 0)
        w_fail(w, "write error");
}

/* Emit `\n` (used as the blank separator between top-level blocks, §9.8). */
static void w_blank(Writer *w)
{
    w_raw(w, "\n");
}

/* Emit `<indent>key = ` (the shared key/value line prefix). The indent is
 * empty for top-level globals and four spaces inside a block (§9.3). */
static void w_key_prefix(Writer *w, const char *key)
{
    char buf[128];

    if (w->failed)
        return;
    snprintf(buf, sizeof buf, "%s%s = ", w->indent, key);
    w_raw(w, buf);
}

static void w_eol(Writer *w)
{
    w_raw(w, "\n");
}

/*
 * Format one double canonically. The primary form is the frozen `%.6g` (§9.4);
 * when that would not round-trip back to the identical `double`, the shortest
 * `"%.*g"` (up to 17 significant digits) that does is emitted instead. This
 * keeps the canonical output short and readable for typical values while
 * guaranteeing the round-trip is *bit-exact* on every `double` (§9), which the
 * `%.6g`-only rule could not do for values such as a normalised `sun_dir`.
 * Negative zero is normalised to `0`. Returns 0 on success, -1 if `v` is not
 * finite (which could never be re-loaded, so it is a hard writer error).
 */
static int w_fmt_double(Writer *w, char *buf, size_t buflen, double v)
{
    int prec;

    if (!isfinite(v)) {
        w_fail(w, "non-finite floating-point value cannot be serialised");
        return -1;
    }
    if (v == 0.0)
        v = 0.0; /* fold -0 to +0 */
    snprintf(buf, buflen, "%.6g", v);
    if (strtod(buf, NULL) != v) {
        for (prec = 7; prec <= 17; prec++) {
            snprintf(buf, buflen, "%.*g", prec, v);
            if (strtod(buf, NULL) == v)
                break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Name / string emission (§9.6)                                       */
/* ------------------------------------------------------------------ */

/* 1 if `s` is a bare identifier `[A-Za-z_][A-Za-z0-9_]*`, else 0. */
static int name_is_bare(const char *s)
{
    const char *p;

    if (s == NULL || s[0] == '\0')
        return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return 0;
    for (p = s + 1; *p != '\0'; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_'))
            return 0;
    }
    return 1;
}

/* Emit a name: bare when it matches the identifier rule, else quoted. */
static void w_name_raw(Writer *w, const char *s)
{
    if (w->failed)
        return;
    if (s == NULL)
        s = "";
    if (name_is_bare(s)) {
        w_raw(w, s);
        return;
    }
    w_raw(w, "\"");
    for (; *s != '\0'; ++s) {
        char tmp[2];
        if (*s == '"' || *s == '\\') {
            tmp[0] = '\\';
            tmp[1] = *s;
            if (fwrite(tmp, 1, 2, w->fp) != 2)
                w_fail(w, "write error");
        } else {
            if (fputc((unsigned char)*s, w->fp) == EOF)
                w_fail(w, "write error");
        }
        if (w->failed)
            return;
    }
    w_raw(w, "\"");
}

/* ------------------------------------------------------------------ */
/* Key/value line emitters                                             */
/* ------------------------------------------------------------------ */

static void w_key_double(Writer *w, const char *key, double v)
{
    char buf[64];

    if (w->failed)
        return;
    if (w_fmt_double(w, buf, sizeof buf, v) != 0)
        return;
    w_key_prefix(w, key);
    w_raw(w, buf);
    w_eol(w);
}

static void w_key_int(Writer *w, const char *key, int v)
{
    char buf[32];

    if (w->failed)
        return;
    snprintf(buf, sizeof buf, "%d", v);
    w_key_prefix(w, key);
    w_raw(w, buf);
    w_eol(w);
}

static void w_key_uint(Writer *w, const char *key, unsigned v)
{
    char buf[32];

    if (w->failed)
        return;
    snprintf(buf, sizeof buf, "%u", v);
    w_key_prefix(w, key);
    w_raw(w, buf);
    w_eol(w);
}

static void w_key_vec3(Writer *w, const char *key, Vec3 v)
{
    char bx[64], by[64], bz[64];

    if (w->failed)
        return;
    if (w_fmt_double(w, bx, sizeof bx, v.x) != 0 ||
        w_fmt_double(w, by, sizeof by, v.y) != 0 ||
        w_fmt_double(w, bz, sizeof bz, v.z) != 0)
        return;
    w_key_prefix(w, key);
    w_raw(w, bx);
    w_raw(w, " ");
    w_raw(w, by);
    w_raw(w, " ");
    w_raw(w, bz);
    w_eol(w);
}

static void w_key_name(Writer *w, const char *key, const char *name)
{
    if (w->failed)
        return;
    w_key_prefix(w, key);
    w_name_raw(w, name);
    w_eol(w);
}

/* Block header: `kw {` (unnamed) or `kw <name> {` (named). Sets the body
 * indentation for the enclosed key/value lines (§9.3). */
static void w_open(Writer *w, const char *kw, const char *name)
{
    if (w->failed)
        return;
    w_raw(w, kw);
    if (name != NULL) {
        w_raw(w, " ");
        w_name_raw(w, name);
    }
    w_raw(w, " {\n");
    w->indent = "    ";
}

static void w_close(Writer *w)
{
    w->indent = ""; /* back to top level */
    w_raw(w, "}\n");
}

/* ------------------------------------------------------------------ */
/* Material preset detection (§9.2)                                    */
/* ------------------------------------------------------------------ */

static int vec3_is(Vec3 v, double x, double y, double z)
{
    return v.x == x && v.y == y && v.z == z;
}

static int vec3_is_zero(Vec3 v)
{
    return vec3_is(v, 0.0, 0.0, 0.0);
}

/*
 * 1 if every procedural-texture field of `m` equals its default, i.e. the
 * material is untextured and the `texture*` keys MUST NOT be emitted. This is
 * what keeps scenes/default.scene byte-identical: the built-in materials carry
 * the defaults, so no new line is produced for them.
 */
static int mat_texture_is_default(const Material *m)
{
    return m->texture_kind == TEXTURE_DEFAULT_KIND &&
           m->texture_scale == TEXTURE_DEFAULT_SCALE &&
           vec3_is(m->texture_color_a, TEXTURE_DEFAULT_COLOR_A.x,
                   TEXTURE_DEFAULT_COLOR_A.y, TEXTURE_DEFAULT_COLOR_A.z) &&
           vec3_is(m->texture_color_b, TEXTURE_DEFAULT_COLOR_B.x,
                   TEXTURE_DEFAULT_COLOR_B.y, TEXTURE_DEFAULT_COLOR_B.z);
}

/* 1 when every opt-in PBR field equals its zero default (nothing to emit). */
static int mat_pbr_is_default(const Material *m)
{
    return m->metallic == MATERIAL_DEFAULT_METALLIC &&
           m->roughness == MATERIAL_DEFAULT_ROUGHNESS &&
           vec3_is(m->emissive, MATERIAL_DEFAULT_EMISSIVE.x,
                   MATERIAL_DEFAULT_EMISSIVE.y,
                   MATERIAL_DEFAULT_EMISSIVE.z) &&
           m->pbr == MATERIAL_DEFAULT_PBR;
}

/* `type = opaque` preset: all-zero fields, ior 1, not water, untextured (§4.3). */
static int mat_is_opaque_preset(const Material *m)
{
    return vec3_is_zero(m->albedo) && vec3_is_zero(m->specular) &&
           m->shininess == 0.0 && m->reflectivity == 0.0 &&
           m->transparency == 0.0 && m->ior == 1.0 && m->is_water == 0 &&
           m->beer_lambert == 0 &&
           vec3_is_zero(m->absorption) && vec3_is_zero(m->deep_color) &&
           mat_pbr_is_default(m) &&
           mat_texture_is_default(m);
}

/* `type = water` preset: the exact MAT_WATER values (§4.3, src/scene.c). */
static int mat_is_water_preset(const Material *m)
{
    return vec3_is(m->albedo, 0.05, 0.15, 0.20) &&
           vec3_is(m->specular, 0.90, 0.90, 0.90) &&
           m->shininess == 256.0 && m->reflectivity == 1.0 &&
           m->transparency == 0.85 && m->ior == 1.33 && m->is_water == 1 &&
           m->beer_lambert == 0 &&
           vec3_is(m->absorption, 0.45, 0.12, 0.06) &&
           vec3_is(m->deep_color, 0.02, 0.10, 0.16) &&
           mat_pbr_is_default(m) &&
           mat_texture_is_default(m);
}

/* `type = glass` preset: the exact sd_glass_preset() values (§4.3). */
static int mat_is_glass_preset(const Material *m)
{
    return vec3_is(m->albedo, 0.02, 0.02, 0.02) &&
           vec3_is(m->specular, 1.0, 1.0, 1.0) &&
           m->shininess == 256.0 && m->reflectivity == 0.0 &&
           m->transparency == 1.0 && m->ior == 1.5 && m->is_water == 0 &&
           m->beer_lambert == 0 &&
           vec3_is_zero(m->absorption) &&
           vec3_is(m->deep_color, 0.5, 0.5, 0.5) &&
           mat_pbr_is_default(m) &&
           mat_texture_is_default(m);
}

/*
 * Named real-world material presets (docs/research_material_reference.md,
 * t-027). Each predicate mirrors the exact `sd_<name>_preset()` values in
 * src/scene_desc.c so a material equal to a preset round-trips as
 * `type = <name>` and a material that differs falls back to explicit keys.
 */

/*
 * Shared conductor shape (§2): albedo == specular == F0, shininess 256,
 * reflectivity/transparency 0, ior 1, no water/beer-attenuation, and the PBR
 * layer set to metallic 1 / emissive 0 / pbr 1 with the given roughness.
 * `mat_pbr_is_default` is deliberately NOT required here (the PBR layer is
 * non-default by construction); the individual PBR fields are checked exactly.
 */
static int mat_is_conductor(const Material *m, Vec3 f0, double roughness)
{
    return vec3_is(m->albedo, f0.x, f0.y, f0.z) &&
           vec3_is(m->specular, f0.x, f0.y, f0.z) &&
           m->shininess == 256.0 && m->reflectivity == 0.0 &&
           m->transparency == 0.0 && m->ior == 1.0 && m->is_water == 0 &&
           m->beer_lambert == 0 &&
           vec3_is_zero(m->absorption) && vec3_is_zero(m->deep_color) &&
           m->metallic == 1.0 && m->roughness == roughness &&
           vec3_is_zero(m->emissive) && m->pbr == 1 &&
           mat_texture_is_default(m);
}

static int mat_is_gold_preset(const Material *m)
{
    return mat_is_conductor(m, vec3(1.000, 0.766, 0.336), 0.05);
}
static int mat_is_copper_preset(const Material *m)
{
    return mat_is_conductor(m, vec3(0.955, 0.637, 0.538), 0.05);
}
static int mat_is_silver_preset(const Material *m)
{
    return mat_is_conductor(m, vec3(0.972, 0.960, 0.915), 0.03);
}
static int mat_is_aluminum_preset(const Material *m)
{
    return mat_is_conductor(m, vec3(0.913, 0.921, 0.925), 0.05);
}
static int mat_is_iron_preset(const Material *m)
{
    return mat_is_conductor(m, vec3(0.560, 0.570, 0.580), 0.10);
}
static int mat_is_chrome_preset(const Material *m)
{
    return mat_is_conductor(m, vec3(0.550, 0.556, 0.554), 0.03);
}
static int mat_is_brass_preset(const Material *m)
{
    return mat_is_conductor(m, vec3(0.910, 0.778, 0.423), 0.08);
}

/* Shared opaque-dielectric shape (§3): metallic 0 / emissive 0 / pbr 1. */
static int mat_is_dielectric(const Material *m, Vec3 albedo, Vec3 specular,
                             double shininess, double roughness, double ior)
{
    return vec3_is(m->albedo, albedo.x, albedo.y, albedo.z) &&
           vec3_is(m->specular, specular.x, specular.y, specular.z) &&
           m->shininess == shininess && m->reflectivity == 0.0 &&
           m->transparency == 0.0 && m->ior == ior && m->is_water == 0 &&
           m->beer_lambert == 0 &&
           vec3_is_zero(m->absorption) && vec3_is_zero(m->deep_color) &&
           m->metallic == 0.0 && m->roughness == roughness &&
           vec3_is_zero(m->emissive) && m->pbr == 1 &&
           mat_texture_is_default(m);
}

static int mat_is_plastic_preset(const Material *m)
{
    return mat_is_dielectric(m, vec3(0.30, 0.05, 0.06), vec3(0.05, 0.05, 0.05),
                             64.0, 0.10, 1.46);
}
static int mat_is_rubber_preset(const Material *m)
{
    return mat_is_dielectric(m, vec3(0.05, 0.05, 0.05), vec3(0.04, 0.04, 0.04),
                             8.0, 0.90, 1.50);
}
static int mat_is_ceramic_preset(const Material *m)
{
    return mat_is_dielectric(m, vec3(0.85, 0.85, 0.82), vec3(0.05, 0.05, 0.05),
                             128.0, 0.20, 1.60);
}

/* `type = diamond`: transparent dielectric, glass-like with ior 2.417 (§3.4). */
static int mat_is_diamond_preset(const Material *m)
{
    return vec3_is(m->albedo, 0.02, 0.02, 0.02) &&
           vec3_is(m->specular, 1.0, 1.0, 1.0) &&
           m->shininess == 256.0 && m->reflectivity == 0.0 &&
           m->transparency == 1.0 && m->ior == 2.417 && m->is_water == 0 &&
           m->beer_lambert == 0 &&
           vec3_is_zero(m->absorption) &&
           vec3_is(m->deep_color, 0.5, 0.5, 0.5) &&
           m->metallic == 0.0 && m->roughness == 0.0 &&
           vec3_is_zero(m->emissive) && m->pbr == 1 &&
           mat_texture_is_default(m);
}

/* `type = emissive`: warm-white lamp emitter (§4). */
static int mat_is_emissive_preset(const Material *m)
{
    return vec3_is_zero(m->albedo) && vec3_is_zero(m->specular) &&
           m->shininess == 0.0 && m->reflectivity == 0.0 &&
           m->transparency == 0.0 && m->ior == 1.0 && m->is_water == 0 &&
           m->beer_lambert == 0 &&
           vec3_is_zero(m->absorption) && vec3_is_zero(m->deep_color) &&
           m->metallic == 0.0 && m->roughness == 0.5 &&
           vec3_is(m->emissive, 1.0, 0.85, 0.65) && m->pbr == 1 &&
           mat_texture_is_default(m);
}

/* ------------------------------------------------------------------ */
/* Block emitters (one per block type)                                 */
/* ------------------------------------------------------------------ */

static void emit_globals(Writer *w, const SceneDesc *d)
{
    const char *water_name = "none";

    if (d->water_material >= 0 && d->water_material < d->material_count &&
        d->materials[d->water_material].name != NULL)
        water_name = d->materials[d->water_material].name;

    w_key_double(w, "water_level", d->water_level);
    w_key_name(w, "water_material", water_name);
    w_key_int(w, "water_enabled", d->water_enabled ? 1 : 0);
}

static void emit_camera(Writer *w, const SceneDesc *d)
{
    if (!d->camera.present)
        return;
    w_blank(w);
    w_open(w, "camera", NULL);
    w_key_vec3(w, "eye", d->camera.eye);
    w_key_vec3(w, "target", d->camera.target);
    w_key_vec3(w, "up", d->camera.up);
    w_key_double(w, "vfov", d->camera.vfov_deg);
    /*
     * Depth-of-field keys are emitted ONLY when they differ from their
     * defaults (aperture 0 = pinhole, focus_distance 0 = derive from
     * |target - eye|). The built-in scene carries the defaults, so neither
     * line is produced for it -> scenes/default.scene stays byte-identical.
     */
    if (d->camera.aperture != CAMERA_DEFAULT_APERTURE)
        w_key_double(w, "aperture", d->camera.aperture);
    if (d->camera.focus_distance != CAMERA_FOCUS_DISTANCE_DERIVED)
        w_key_double(w, "focus_distance", d->camera.focus_distance);
    w_close(w);
}

static void emit_sky(Writer *w, const SceneDesc *d)
{
    const SkyParams *s = &d->sky;

    if (!d->has_sky)
        return;
    w_blank(w);
    w_open(w, "sky", NULL);
    w_key_vec3(w, "sun_dir", s->sun_dir);
    w_key_vec3(w, "sun_color", s->sun_color);
    w_key_vec3(w, "horizon_color", s->horizon_color);
    w_key_vec3(w, "zenith_color", s->zenith_color);
    w_key_double(w, "gradient_gamma", s->gradient_gamma);
    w_key_double(w, "sun_glow_exponent", s->sun_glow_exponent);
    w_key_double(w, "sun_glow_strength", s->sun_glow_strength);
    w_key_double(w, "cloud_height", s->cloud_height);
    w_key_double(w, "cloud_scale", s->cloud_scale);
    w_key_double(w, "cloud_coverage", s->cloud_coverage);
    w_key_double(w, "cloud_softness", s->cloud_softness);
    w_key_double(w, "cloud_sharpness", s->cloud_sharpness);
    w_key_int(w, "cloud_octaves", s->cloud_octaves);
    w_key_uint(w, "seed", s->seed);
    /*
     * `sun_radius` is emitted ONLY when it differs from its default (0). The
     * built-in scene uses the point-like sun, so this key is never written for
     * it -> scenes/default.scene stays byte-identical.
     */
    if (s->sun_radius != SKY_DEFAULT_SUN_RADIUS)
        w_key_double(w, "sun_radius", s->sun_radius);
    if (s->star_intensity != SKY_DEFAULT_STAR_INTENSITY)
        w_key_double(w, "star_intensity", s->star_intensity);
    if (s->star_density != SKY_DEFAULT_STAR_DENSITY)
        w_key_double(w, "star_density", s->star_density);
    if (s->nebula_intensity != SKY_DEFAULT_NEBULA_INTENSITY)
        w_key_double(w, "nebula_intensity", s->nebula_intensity);
    if (s->galaxy_intensity != SKY_DEFAULT_GALAXY_INTENSITY)
        w_key_double(w, "galaxy_intensity", s->galaxy_intensity);
    if (!vec3_is(s->galaxy_dir, SKY_DEFAULT_GALAXY_DIR.x,
                 SKY_DEFAULT_GALAXY_DIR.y, SKY_DEFAULT_GALAXY_DIR.z))
        w_key_vec3(w, "galaxy_dir", s->galaxy_dir);
    if (!vec3_is(s->nebula_dir, SKY_DEFAULT_NEBULA_DIR.x,
                 SKY_DEFAULT_NEBULA_DIR.y, SKY_DEFAULT_NEBULA_DIR.z))
        w_key_vec3(w, "nebula_dir", s->nebula_dir);
    if (s->galaxy_tilt != SKY_DEFAULT_GALAXY_TILT)
        w_key_double(w, "galaxy_tilt", s->galaxy_tilt);
    if (s->galaxy_roll != SKY_DEFAULT_GALAXY_ROLL)
        w_key_double(w, "galaxy_roll", s->galaxy_roll);
    w_close(w);
}

static void emit_fog(Writer *w, const SceneDesc *d)
{
    const FogParams *f = &d->fog;
    if (!d->has_fog && f->density <= 0.0)
        return;

    w_open(w, "fog", NULL);
    w_key_double(w, "density", f->density);
    if (!vec3_is(f->color, FOG_DEFAULT_COLOR.x, FOG_DEFAULT_COLOR.y, FOG_DEFAULT_COLOR.z))
        w_key_vec3(w, "color", f->color);
    if (f->height != FOG_DEFAULT_HEIGHT)
        w_key_double(w, "height", f->height);
    if (f->height_falloff != FOG_DEFAULT_HEIGHT_FALLOFF)
        w_key_double(w, "height_falloff", f->height_falloff);
    if (f->inscatter_strength != FOG_DEFAULT_INSCATTER_STRENGTH)
        w_key_double(w, "inscatter_strength", f->inscatter_strength);
    if (f->sun_anisotropy != FOG_DEFAULT_SUN_ANISOTROPY)
        w_key_double(w, "sun_anisotropy", f->sun_anisotropy);
    if (f->noise_scale != FOG_DEFAULT_NOISE_SCALE)
        w_key_double(w, "noise_scale", f->noise_scale);
    if (f->noise_amount != FOG_DEFAULT_NOISE_AMOUNT)
        w_key_double(w, "noise_amount", f->noise_amount);
    w_close(w);
}

/*
 * Emit the procedural-texture keys, but ONLY when they differ from their
 * defaults (texture=none, scale 1, A=white, B=black). This conditional is the
 * critical invariant that keeps scenes/default.scene byte-identical: the
 * built-in materials are untextured, so nothing is emitted for them.
 *
 * `texture` is emitted first (it selects the pattern); the scale/colours follow
 * only when they are non-default, so a bare `texture = checker` is the tersest
 * canonical form of the common case.
 */
static void emit_material_texture(Writer *w, const Material *m)
{
    int default_kind  = (m->texture_kind == TEXTURE_DEFAULT_KIND);
    int default_scale = (m->texture_scale == TEXTURE_DEFAULT_SCALE);
    int default_a     = vec3_is(m->texture_color_a, TEXTURE_DEFAULT_COLOR_A.x,
                                TEXTURE_DEFAULT_COLOR_A.y,
                                TEXTURE_DEFAULT_COLOR_A.z);
    int default_b     = vec3_is(m->texture_color_b, TEXTURE_DEFAULT_COLOR_B.x,
                                TEXTURE_DEFAULT_COLOR_B.y,
                                TEXTURE_DEFAULT_COLOR_B.z);

    if (default_kind && default_scale && default_a && default_b)
        return; /* untextured: emit nothing (byte-identity preserved) */

    switch (m->texture_kind) {
    case TEXTURE_CHECKER:      w_key_name(w, "texture", "checker"); break;
    case TEXTURE_STRIPES:      w_key_name(w, "texture", "stripes"); break;
    case TEXTURE_PLANET_EARTH: w_key_name(w, "texture", "earth");   break;
    case TEXTURE_PLANET_MOON:  w_key_name(w, "texture", "moon");    break;
    case TEXTURE_NOISE:        w_key_name(w, "texture", "noise");   break;
    case TEXTURE_NONE:
    default:                   w_key_name(w, "texture", "none");    break;
    }

    if (!default_scale)
        w_key_double(w, "texture_scale", m->texture_scale);
    if (!default_a)
        w_key_vec3(w, "texture_color_a", m->texture_color_a);
    if (!default_b)
        w_key_vec3(w, "texture_color_b", m->texture_color_b);
}

/*
 * Emit the opt-in PBR keys (`metallic`, `roughness`, `emissive`, `pbr`) ONLY
 * when they differ from their zero defaults, mirroring how `beer_lambert` is
 * emitted only when non-zero. A material that is entirely default therefore
 * emits no new line at all, keeping scenes/default.scene byte-identical.
 */
static void emit_material_pbr(Writer *w, const Material *m)
{
    if (mat_pbr_is_default(m))
        return; /* no PBR layer: emit nothing (byte-identity preserved) */

    if (m->metallic != MATERIAL_DEFAULT_METALLIC)
        w_key_double(w, "metallic", m->metallic);
    if (m->roughness != MATERIAL_DEFAULT_ROUGHNESS)
        w_key_double(w, "roughness", m->roughness);
    if (!vec3_is(m->emissive, MATERIAL_DEFAULT_EMISSIVE.x,
                 MATERIAL_DEFAULT_EMISSIVE.y,
                 MATERIAL_DEFAULT_EMISSIVE.z))
        w_key_vec3(w, "emissive", m->emissive);
    if (m->pbr != MATERIAL_DEFAULT_PBR)
        w_key_int(w, "pbr", m->pbr ? 1 : 0);
}

static void emit_material(Writer *w, const SceneDesc *d, int i)
{
    const MaterialDesc *md = &d->materials[i];
    const Material     *m  = &md->mat;

    w_blank(w);
    w_open(w, "material", (md->name != NULL) ? md->name : "");

    if (mat_is_water_preset(m)) {
        w_key_name(w, "type", "water");
    } else if (mat_is_opaque_preset(m)) {
        w_key_name(w, "type", "opaque");
    } else if (mat_is_glass_preset(m)) {
        w_key_name(w, "type", "glass");
    } else if (mat_is_gold_preset(m)) {
        w_key_name(w, "type", "gold");
    } else if (mat_is_copper_preset(m)) {
        w_key_name(w, "type", "copper");
    } else if (mat_is_silver_preset(m)) {
        w_key_name(w, "type", "silver");
    } else if (mat_is_aluminum_preset(m)) {
        w_key_name(w, "type", "aluminum");
    } else if (mat_is_iron_preset(m)) {
        w_key_name(w, "type", "iron");
    } else if (mat_is_chrome_preset(m)) {
        w_key_name(w, "type", "chrome");
    } else if (mat_is_brass_preset(m)) {
        w_key_name(w, "type", "brass");
    } else if (mat_is_plastic_preset(m)) {
        w_key_name(w, "type", "plastic");
    } else if (mat_is_rubber_preset(m)) {
        w_key_name(w, "type", "rubber");
    } else if (mat_is_ceramic_preset(m)) {
        w_key_name(w, "type", "ceramic");
    } else if (mat_is_diamond_preset(m)) {
        w_key_name(w, "type", "diamond");
    } else if (mat_is_emissive_preset(m)) {
        w_key_name(w, "type", "emissive");
    } else {
        w_key_vec3(w, "albedo", m->albedo);
        w_key_vec3(w, "specular", m->specular);
        w_key_double(w, "shininess", m->shininess);
        w_key_double(w, "reflectivity", m->reflectivity);
        w_key_double(w, "transparency", m->transparency);
        w_key_double(w, "ior", m->ior);
        w_key_int(w, "is_water", m->is_water ? 1 : 0);
        /* `beer_lambert` is emitted ONLY when non-default (non-zero), so the
         * built-in materials (flag 0) emit no line and scenes/default.scene
         * stays byte-identical. */
        if (m->beer_lambert != 0)
            w_key_int(w, "beer_lambert", 1);
        w_key_vec3(w, "absorption", m->absorption);
        w_key_vec3(w, "deep_color", m->deep_color);
        emit_material_pbr(w, m);
        emit_material_texture(w, m);
        if (m->bump_strength != MATERIAL_DEFAULT_BUMP_STRENGTH)
            w_key_double(w, "bump_strength", m->bump_strength);
        if (m->bump_scale != MATERIAL_DEFAULT_BUMP_SCALE)
            w_key_double(w, "bump_scale", m->bump_scale);
        if (!vec3_is(m->atmosphere_glow, MATERIAL_DEFAULT_ATMOSPHERE_GLOW.x,
                     MATERIAL_DEFAULT_ATMOSPHERE_GLOW.y,
                     MATERIAL_DEFAULT_ATMOSPHERE_GLOW.z))
            w_key_vec3(w, "atmosphere_glow", m->atmosphere_glow);
    }
    w_close(w);
}

static void emit_prim_desc(Writer *w, const ScenePrimDesc *p)
{
    switch (p->kind) {
    case PRIM_SPHERE:
        w_open(w, "sphere", NULL);
        w_key_vec3(w, "center", p->center);
        w_key_double(w, "radius", p->radius);
        break;
    case PRIM_PLANE:
        w_open(w, "plane", NULL);
        w_key_vec3(w, "point", p->point);
        w_key_vec3(w, "normal", p->normal);
        break;
    case PRIM_BOX:
        w_open(w, "box", NULL);
        w_key_vec3(w, "center", p->center);
        w_key_vec3(w, "half", p->half);
        break;
    case PRIM_TRIANGLE:
        w_open(w, "triangle", NULL);
        w_key_vec3(w, "a", p->a);
        w_key_vec3(w, "b", p->b);
        w_key_vec3(w, "c", p->c);
        break;
    case PRIM_CYLINDER:
        w_open(w, "cylinder", NULL);
        w_key_vec3(w, "base", p->base);
        w_key_vec3(w, "top", p->top);
        w_key_double(w, "r_bottom", p->radius);
        w_key_double(w, "r_top", p->radius2);
        break;
    case PRIM_CSG: {
        const char *kw = "csg_union";
        if (p->csg_op == CSG_INTERSECTION) kw = "csg_intersection";
        else if (p->csg_op == CSG_DIFFERENCE) kw = "csg_difference";
        w_open(w, kw, NULL);
        if (p->material_name != NULL)
            w_key_name(w, "material", p->material_name);
        if (p->left) emit_prim_desc(w, p->left);
        if (p->right) emit_prim_desc(w, p->right);
        w_close(w);
        return;
    }
    default:
        w_fail(w, "unknown primitive kind cannot be serialised");
        return;
    }
    w_key_name(w, "material", p->material_name);
    w_close(w);
}

static void emit_prim(Writer *w, const SceneDesc *d, int i)
{
    w_blank(w);
    emit_prim_desc(w, &d->prims[i]);
}

static void emit_plant(Writer *w, const SceneDesc *d, int i)
{
    const ScenePlantDesc *p = &d->plants[i];

    w_blank(w);
    w_open(w, (p->kind == SD_PLANT_BUSH) ? "bush" : "tree", NULL);
    w_key_vec3(w, "position", p->position);
    w_key_double(w, "height", p->height);
    w_key_double(w, "radius", p->radius);
    if (p->has_seed)
        w_key_uint(w, "seed", p->seed);
    if (p->material_bark != NULL)
        w_key_name(w, "material_bark", p->material_bark);
    if (p->material_leaf != NULL)
        w_key_name(w, "material_leaf", p->material_leaf);
    if (p->has_leaf_variant)
        w_key_int(w, "leaf_variant", p->leaf_variant);

    /* Optional generator parameters (§4.9/§4.10). Each is emitted only when
     * the file carried the key, so the default scene round-trips byte-for-byte
     * (and the writer's output is a canonical fixed point). */
    if (p->has_max_depth)
        w_key_int(w, "max_depth", p->max_depth);
    if (p->has_min_branch_radius)
        w_key_double(w, "min_branch_radius", p->min_branch_radius);
    if (p->has_taper)
        w_key_double(w, "taper", p->taper);
    if (p->has_len_decay)
        w_key_double(w, "len_decay", p->len_decay);
    if (p->has_spread_deg)
        w_key_double(w, "spread_deg", p->spread_deg);
    if (p->has_perturb_deg)
        w_key_double(w, "perturb_deg", p->perturb_deg);
    if (p->has_up_bias)
        w_key_double(w, "up_bias", p->up_bias);
    if (p->has_third_child_chance)
        w_key_double(w, "third_child_chance", p->third_child_chance);
    if (p->has_leaf_min)
        w_key_int(w, "leaf_min", p->leaf_min);
    if (p->has_leaf_span)
        w_key_int(w, "leaf_span", p->leaf_span);
    if (p->has_plant_type) {
        const char *tname = "deciduous";
        if (p->plant_type == PLANT_TYPE_CONIFER) tname = "conifer";
        else if (p->plant_type == PLANT_TYPE_BUSH) tname = "bush";
        w_key_name(w, "type", tname);
    }
    if (p->has_foliage) {
        const char *fname = "spheres";
        if (p->foliage == PLANT_FOLIAGE_LEAVES) fname = "leaves";
        else if (p->foliage == PLANT_FOLIAGE_NEEDLES) fname = "needles";
        w_key_name(w, "foliage", fname);
    }
    w_close(w);
}

static void emit_boulder(Writer *w, const SceneDesc *d, int index)
{
    const SceneBoulderDesc *b = &d->boulders[index];
    w_open(w, "boulder", NULL);
    w_key_vec3(w, "position", b->position);
    w_key_double(w, "radius", b->radius);
    w_key_double(w, "roughness", b->roughness);
    w_key_double(w, "flatness", b->flatness);
    if (b->seed != 0)
        w_key_uint(w, "seed", b->seed);
    if (b->material_name != NULL)
        w_key_name(w, "material", b->material_name);
    else if (b->material_index >= 0 && b->material_index < d->material_count &&
             d->materials[b->material_index].name != NULL)
        w_key_name(w, "material", d->materials[b->material_index].name);
    w_close(w);
}

static void emit_light(Writer *w, const SceneDesc *d, int index)
{
    const SceneLightDesc *l = &d->lights[index];
    w_open(w, "light", NULL);
    w_key_vec3(w, "position", l->position);
    w_key_vec3(w, "color", l->color);
    w_key_double(w, "intensity", l->intensity);
    w_key_double(w, "radius", l->radius);
    w_close(w);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int scene_desc_write(const SceneDesc *d, const char *path,
                     char *errbuf, size_t errlen)
{
    Writer w;
    FILE  *fp;
    int    i;

    if (errbuf != NULL && errlen > 0)
        errbuf[0] = '\0';

    if (d == NULL || path == NULL) {
        if (errbuf != NULL && errlen > 0)
            snprintf(errbuf, errlen, "%s: error: null scene or path",
                     (path != NULL) ? path : "<null>");
        return 1;
    }

    fp = fopen(path, "wb"); /* binary: guarantees LF endings (§9.9) */
    if (fp == NULL) {
        if (errbuf != NULL && errlen > 0)
            snprintf(errbuf, errlen, "%s: error: cannot open: %s",
                     path, strerror(errno));
        return 1;
    }

    w.fp = fp;
    w.path = path;
    w.errbuf = errbuf;
    w.errlen = errlen;
    w.failed = 0;
    w.indent = ""; /* top level: globals are unindented (§7, §4.11) */

    /* §9.1 statement order. */
    w_raw(&w, SD_HEADER_COMMENT);
    w_eol(&w);
    emit_globals(&w, d);
    emit_camera(&w, d);
    emit_sky(&w, d);
    emit_fog(&w, d);
    for (i = 0; i < d->material_count; ++i)
        emit_material(&w, d, i);
    for (i = 0; i < d->prim_count; ++i)
        emit_prim(&w, d, i);
    for (i = 0; i < d->plant_count; ++i)
        emit_plant(&w, d, i);
    for (i = 0; i < d->boulder_count; ++i)
        emit_boulder(&w, d, i);
    for (i = 0; i < d->light_count; ++i)
        emit_light(&w, d, i);

    if (fflush(fp) != 0)
        w_fail(&w, "flush error");
    if (fclose(fp) != 0 && !w.failed)
        w_fail(&w, "close error");

    return w.failed ? 1 : 0;
}
