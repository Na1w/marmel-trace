/*
 * scene_desc.c - Scene-description data model: lifecycle, ownership,
 *                dynamic-array construction helpers, and the line-oriented
 *                lexer/parser for the frozen format in docs/scene_format.md.
 *
 * Scope of THIS revision (t-116 + t-119 + t-109):
 *   - whole-file reader with a dynamically grown buffer (no fixed-size
 *     overflow, LF/CRLF, missing trailing newline, optional UTF-8 BOM),
 *   - a small line-oriented lexer (identifiers, numbers, quoted strings,
 *     '{', '}', '=', '#' comments) with 1-based line tracking,
 *   - a one-statement-per-line dispatcher for the top-level globals,
 *     the `camera { }` block and the `sky { }` block,
 *   - t-119: full `material <name> { }` semantics (all Material fields plus
 *     the `type = water|opaque|glass` preset shorthand), the five explicit
 *     primitive blocks (sphere/plane/box/triangle/cylinder) referencing a
 *     material BY NAME, file-order-preserving primitive accumulation, and a
 *     second resolution pass that maps names to table indices (forward
 *     references allowed, duplicate/unknown names are hard errors),
 *   - t-109: full `tree` / `bush` plant semantics (position with y forced to
 *     0, height, radius, optional seed and leaf_variant presence flags, and
 *     optional `material_bark` / `material_leaf` names resolved like every
 *     other material reference),
 *   - exact `FILE:LINE: error: MSG (near 'TOKEN')` diagnostics.
 *
 * Not in scope here: exhaustive per-key range validation and the built-in
 * default scene fallback (the CLI supplies the embedded default instead).
 *
 * No recursion (one nesting level), no globals, C11 standard library only,
 * warning-free under -Wall -Wextra. The writer lives in scene_desc_write.c.
 */

#include "scene_desc.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SD_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Lexical limits (docs/scene_format.md §2). */
#define SD_MAX_LINE   4096  /* physical line incl. terminator; longer = error */
#define SD_MAX_TOKENS 64    /* tokens per physical line                      */
#define SD_TOK_TEXT   128   /* stored text of one token                      */

/* Spec default for the water surface height (SCENE_WATER_LEVEL in scene.c). */
#define SD_DEFAULT_WATER_LEVEL 0.02

/* ------------------------------------------------------------------ */
/* Small internal helpers                                              */
/* ------------------------------------------------------------------ */

/*
 * Duplicate `s` (strdup is POSIX, not C11, so provide a local copy).
 * Returns NULL for NULL input or on allocation failure.
 */
static char *sd_strdup(const char *s)
{
    size_t n;
    char  *copy;

    if (s == NULL)
        return NULL;
    n = strlen(s) + 1;
    copy = (char *)malloc(n);
    if (copy != NULL)
        memcpy(copy, s, n);
    return copy;
}

/*
 * Format a diagnostic into a caller-supplied buffer, always NUL-terminated
 * and truncated to `errlen`. `errbuf` may be NULL (then this is a no-op).
 */
static void sd_set_err(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf == NULL || errlen == 0)
        return;
    snprintf(errbuf, errlen, "%s", msg);
}

/*
 * Grow a count+capacity array in place. `elem` is the element size. On
 * success the pointer/capacity are updated and 0 is returned; on failure the
 * array is untouched and -1 is returned.
 */
static int sd_grow(void **arr, int *capacity, int count, size_t elem)
{
    int    newcap;
    void  *grown;

    if (count < *capacity)
        return 0;

    newcap = (*capacity > 0) ? (*capacity * 2) : 8;
    grown = realloc(*arr, (size_t)newcap * elem);
    if (grown == NULL)
        return -1;

    *arr = grown;
    *capacity = newcap;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    TOK_IDENT,   /* bare [A-Za-z_][A-Za-z0-9_]*                  */
    TOK_STRING,  /* double-quoted string, escapes already applied */
    TOK_NUMBER,  /* number literal; `num` holds the value         */
    TOK_LBRACE,  /* {                                             */
    TOK_RBRACE,  /* }                                             */
    TOK_EQUAL    /* =                                             */
} TokKind;

typedef struct {
    TokKind kind;
    double  num;
    char    text[SD_TOK_TEXT];
} Token;

/*
 * Parser state threaded through every helper so diagnostics can be emitted
 * from anywhere. `failed` latches on the first hard error.
 */
typedef struct {
    const char *path;   /* file name used in diagnostics        */
    char       *errbuf; /* may be NULL                          */
    size_t      errlen;
    int         line;   /* 1-based current physical line        */
    int         failed;
    /* t-119: state for the material / primitive blocks currently open. */
    ScenePrimDesc prim;    /* primitive under construction (BLK_PRIM) */
    char          prim_mat_name[SD_TOK_TEXT]; /* referenced material name */
    int           prim_mat_line;  /* line of the primitive's `material` key  */
    Material      mat;     /* material under construction (BLK_MATERIAL) */
    char          mat_name[SD_TOK_TEXT]; /* name of the open material block */
    int           mat_type;/* material `type` preset selector (SD_MAT_*):
                            * 0 = none, else a named preset swapped in as the
                            * base at block close (see SD_MAT_TYPES)         */
    int          *ref_lines;     /* per-prim line of its material reference  */
    int           ref_count;     /* number of recorded reference lines       */
    int           ref_cap;       /* capacity of `ref_lines`                  */
    int           water_ref;     /* 0 = unset, 1 = `none`, 2 = a name        */
    char          water_name[SD_TOK_TEXT]; /* deferred `water_material` name */
    int           water_line;       /* line of the `water_material` global     */
    int           water_enabled_set;/* 1 if `water_enabled` was explicit       */
    /* t-109: state for the plant (tree/bush) block currently open. */
    ScenePlantDesc plant;               /* plant under construction (BLK_PLANT) */
    char          plant_bark_name[SD_TOK_TEXT]; /* `material_bark` name        */
    char          plant_leaf_name[SD_TOK_TEXT]; /* `material_leaf` name        */
    int           plant_bark_line;      /* line of the `material_bark` key     */
    int           plant_leaf_line;      /* line of the `material_leaf` key     */
    int          *plant_lines;          /* 2 lines (bark, leaf) per plant      */
    int           plant_line_count;     /* number of recorded ints             */
    int           plant_line_cap;       /* capacity of `plant_lines`           */
    /* state for boulder / rock block currently open */
    SceneBoulderDesc boulder;
    char          boulder_mat_name[SD_TOK_TEXT];
    int           boulder_mat_line;
    int          *boulder_lines;
    int           boulder_line_count;
    int           boulder_line_cap;
    /* state for light / point_light block currently open */
    SceneLightDesc light;
} Parser;

/* FILE:LINE: error: MSG (near 'TOKEN')  — the one true error form (§6). */
static void sd_err_at(Parser *p, int line, const char *msg, const char *tok)
{
    if (p->failed)
        return;
    p->failed = 1;
    if (p->errbuf != NULL && p->errlen > 0) {
        snprintf(p->errbuf, p->errlen, "%s:%d: error: %s (near '%s')",
                 p->path, line, msg, (tok != NULL) ? tok : "<eof>");
    }
}

static void sd_err(Parser *p, const char *msg, const char *tok)
{
    sd_err_at(p, p->line, msg, tok);
}

/* Error for a single offending byte (kept short for the message). */
static void sd_err_char(Parser *p, char c)
{
    char buf[2];

    buf[0] = c;
    buf[1] = '\0';
    sd_err(p, "unexpected character", buf);
}

/* Error whose token is the whitespace-delimited slice starting at `s`. */
static void sd_err_slice(Parser *p, const char *s, const char *msg)
{
    char   buf[SD_TOK_TEXT];
    size_t n = 0;

    while (s[n] != '\0' && n < sizeof(buf) - 1 &&
           s[n] != ' ' && s[n] != '\t' && s[n] != '#')
        n++;
    memcpy(buf, s, n);
    buf[n] = '\0';
    sd_err(p, msg, buf);
}

static void sd_tok_text(Token *t, const char *s, size_t len)
{
    if (len >= SD_TOK_TEXT)
        len = SD_TOK_TEXT - 1;
    memcpy(t->text, s, len);
    t->text[len] = '\0';
}

static int sd_is_ident_start(int c)
{
    return isalpha(c) || c == '_';
}

static int sd_is_ident_char(int c)
{
    return isalnum(c) || c == '_';
}

/* Lex one `"..."` token; `*pos` points at the opening quote. */
static int sd_lex_string(Parser *p, const char *line, size_t *pos, Token *t)
{
    size_t i = *pos + 1; /* skip the opening quote */
    size_t out = 0;

    while (line[i] != '"') {
        char ch = line[i];

        if (ch == '\0') {
            sd_err(p, "unterminated string", "\"");
            return -1;
        }
        if (ch == '\\') {
            char esc = line[i + 1];
            if (esc == '"' || esc == '\\') {
                ch = esc;
            } else if (esc == 'n') {
                ch = '\n';
            } else if (esc == 't') {
                ch = '\t';
            } else {
                sd_err_slice(p, line + i, "bad escape in string");
                return -1;
            }
            i += 2;
        } else {
            i++;
        }
        if (out + 1 < SD_TOK_TEXT)
            t->text[out++] = ch;
    }
    t->text[out] = '\0';
    t->kind = TOK_STRING;
    *pos = i + 1;
    return 0;
}

/* Lex one number token; rejects hex, inf/nan and trailing junk (§2). */
static int sd_lex_number(Parser *p, const char *line, size_t *pos, Token *t)
{
    const char *start = line + *pos;
    const char *q;
    char       *end = NULL;
    double      v;

    v = strtod(start, &end);
    if (end == start) {
        sd_err_slice(p, start, "malformed number");
        return -1;
    }
    for (q = start; q < end; q++) {
        unsigned char c = (unsigned char)*q;
        if (!(isdigit(c) || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E')) {
            sd_err_slice(p, start, "malformed number"); /* 0x10, inf, nan, ... */
            return -1;
        }
    }
    if (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\r' && *end != '#') {
        sd_err_slice(p, start, "malformed number"); /* 1.2.3, 4x, ... */
        return -1;
    }
    if (!isfinite(v)) {
        sd_err_slice(p, start, "malformed number"); /* 1e999 */
        return -1;
    }

    t->kind = TOK_NUMBER;
    t->num = v;
    sd_tok_text(t, start, (size_t)(end - start));
    *pos += (size_t)(end - start);
    return 0;
}

static int sd_lex_ident(Parser *p, const char *line, size_t *pos, Token *t)
{
    size_t i = *pos;

    while (sd_is_ident_char((unsigned char)line[i]))
        i++;
    if (i - *pos >= SD_TOK_TEXT) {
        sd_err(p, "identifier too long", line + *pos);
        return -1;
    }
    t->kind = TOK_IDENT;
    sd_tok_text(t, line + *pos, i - *pos);
    *pos = i;
    return 0;
}

/*
 * Tokenize one physical line (newline already stripped). Returns the token
 * count (>= 0) or -1 after reporting a lexical error.
 */
static int sd_lex_line(Parser *p, const char *line, Token *toks, int max)
{
    size_t pos = 0;
    int    nt = 0;

    while (line[pos] != '\0') {
        unsigned char c = (unsigned char)line[pos];

        if (c == ' ' || c == '\t' || c == '\r') {
            pos++;
            continue;
        }
        if (c == '#') /* trailing or full-line comment */
            break;

        if (nt >= max) {
            sd_err(p, "too many tokens on line", line);
            return -1;
        }
        switch (c) {
        case '{': toks[nt].kind = TOK_LBRACE; sd_tok_text(&toks[nt], "{", 1); nt++; pos++; continue;
        case '}': toks[nt].kind = TOK_RBRACE; sd_tok_text(&toks[nt], "}", 1); nt++; pos++; continue;
        case '=': toks[nt].kind = TOK_EQUAL;  sd_tok_text(&toks[nt], "=", 1); nt++; pos++; continue;
        case '"':
            if (sd_lex_string(p, line, &pos, &toks[nt]) != 0)
                return -1;
            nt++;
            continue;
        case ',':
        case ';':
            sd_err_char(p, (char)c);
            return -1;
        default:
            break;
        }
        if (c >= 0x80) {
            sd_err(p, "non-ASCII byte outside a quoted string", "<byte>");
            return -1;
        }
        if (sd_is_ident_start(c)) {
            if (sd_lex_ident(p, line, &pos, &toks[nt]) != 0)
                return -1;
            nt++;
            continue;
        }
        if (isdigit(c) || c == '+' || c == '-' || c == '.') {
            if (sd_lex_number(p, line, &pos, &toks[nt]) != 0)
                return -1;
            nt++;
            continue;
        }
        sd_err_char(p, (char)c);
        return -1;
    }
    return nt;
}

/* ------------------------------------------------------------------ */
/* Parser: block/key bookkeeping                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    BLK_NONE = 0,
    BLK_CAMERA,
    BLK_SKY,
    BLK_FOG,
    BLK_MATERIAL,
    BLK_PRIM,
    BLK_PLANT,
    BLK_BOULDER,
    BLK_LIGHT,
    BLK_CSG
} BlockKind;

typedef struct {
    BlockKind kind;
    int       line;      /* line of the opening header (for diagnostics) */
    unsigned  seen;      /* bitmask of keys already seen in this block   */
    char      kw[32];    /* opening keyword                              */
    CsgOp     csg_op;
    char      csg_mat_name[64];
    int       csg_mat_line;
    ScenePrimDesc csg_children[2];
    int       csg_child_count;
} Block;

enum {
    CAM_EYE    = 1u << 0,
    CAM_TARGET = 1u << 1,
    CAM_UP     = 1u << 2,
    CAM_VFOV   = 1u << 3,
    /* Depth-of-field keys (§4.1). */
    CAM_APERTURE       = 1u << 4,
    CAM_FOCUS_DISTANCE = 1u << 5
};

enum {
    SKY_SUN_DIR     = 1u << 0,
    SKY_SUN_COLOR   = 1u << 1,
    SKY_HORIZON     = 1u << 2,
    SKY_ZENITH      = 1u << 3,
    SKY_GAMMA       = 1u << 4,
    SKY_GLOW_EXP    = 1u << 5,
    SKY_GLOW_STR    = 1u << 6,
    SKY_CLOUD_H     = 1u << 7,
    SKY_CLOUD_SCALE = 1u << 8,
    SKY_CLOUD_COV   = 1u << 9,
    SKY_CLOUD_SOFT  = 1u << 10,
    SKY_CLOUD_SHARP = 1u << 11,
    SKY_CLOUD_OCT   = 1u << 12,
    SKY_SEED           = 1u << 13,
    SKY_SUN_RADIUS     = 1u << 14,
    SKY_STAR_INTENSITY   = 1u << 15,
    SKY_STAR_DENSITY     = 1u << 16,
    SKY_NEBULA_INTENSITY = 1u << 17,
    SKY_GALAXY_INTENSITY = 1u << 18,
    SKY_GALAXY_DIR       = 1u << 19,
    SKY_NEBULA_DIR       = 1u << 20,
    SKY_GALAXY_TILT      = 1u << 21,
    SKY_GALAXY_ROLL      = 1u << 22
};

/* Value kinds a key can take (docs/scene_format.md §3.4). */
typedef enum {
    KT_DOUBLE,
    KT_VEC3,
    KT_INT,
    KT_UINT,
    KT_BOOL
} KeyType;

/*
 * Declarative description of one `key = value` line, so camera and sky share
 * a single generic applier instead of two near-identical chains of strcmps.
 */
typedef struct {
    const char *key;
    KeyType     type;
    unsigned    bit;
    size_t      offset; /* byte offset of the target field in the struct */
} KeySpec;

static const KeySpec CAM_KEYS[] = {
    { "eye",    KT_VEC3,   CAM_EYE,    offsetof(CameraDesc, eye) },
    { "target", KT_VEC3,   CAM_TARGET, offsetof(CameraDesc, target) },
    { "up",     KT_VEC3,   CAM_UP,     offsetof(CameraDesc, up) },
    { "vfov",   KT_DOUBLE, CAM_VFOV,   offsetof(CameraDesc, vfov_deg) },
    { "aperture",       KT_DOUBLE, CAM_APERTURE,
      offsetof(CameraDesc, aperture) },
    { "focus_distance", KT_DOUBLE, CAM_FOCUS_DISTANCE,
      offsetof(CameraDesc, focus_distance) }
};

static const KeySpec SKY_KEYS[] = {
    { "sun_dir",           KT_VEC3,   SKY_SUN_DIR,     offsetof(SkyParams, sun_dir) },
    { "sun_color",         KT_VEC3,   SKY_SUN_COLOR,   offsetof(SkyParams, sun_color) },
    { "horizon_color",     KT_VEC3,   SKY_HORIZON,     offsetof(SkyParams, horizon_color) },
    { "zenith_color",      KT_VEC3,   SKY_ZENITH,      offsetof(SkyParams, zenith_color) },
    { "gradient_gamma",    KT_DOUBLE, SKY_GAMMA,       offsetof(SkyParams, gradient_gamma) },
    { "sun_glow_exponent", KT_DOUBLE, SKY_GLOW_EXP,    offsetof(SkyParams, sun_glow_exponent) },
    { "sun_glow_strength", KT_DOUBLE, SKY_GLOW_STR,    offsetof(SkyParams, sun_glow_strength) },
    { "cloud_height",      KT_DOUBLE, SKY_CLOUD_H,     offsetof(SkyParams, cloud_height) },
    { "cloud_scale",       KT_DOUBLE, SKY_CLOUD_SCALE, offsetof(SkyParams, cloud_scale) },
    { "cloud_coverage",    KT_DOUBLE, SKY_CLOUD_COV,   offsetof(SkyParams, cloud_coverage) },
    { "cloud_softness",    KT_DOUBLE, SKY_CLOUD_SOFT,  offsetof(SkyParams, cloud_softness) },
    { "cloud_sharpness",   KT_DOUBLE, SKY_CLOUD_SHARP, offsetof(SkyParams, cloud_sharpness) },
    { "cloud_octaves",     KT_INT,    SKY_CLOUD_OCT,   offsetof(SkyParams, cloud_octaves) },
    { "seed",              KT_UINT,   SKY_SEED,           offsetof(SkyParams, seed) },
    { "sun_radius",        KT_DOUBLE, SKY_SUN_RADIUS,     offsetof(SkyParams, sun_radius) },
    { "star_intensity",    KT_DOUBLE, SKY_STAR_INTENSITY,   offsetof(SkyParams, star_intensity) },
    { "star_density",      KT_DOUBLE, SKY_STAR_DENSITY,     offsetof(SkyParams, star_density) },
    { "nebula_intensity",  KT_DOUBLE, SKY_NEBULA_INTENSITY, offsetof(SkyParams, nebula_intensity) },
    { "galaxy_intensity",  KT_DOUBLE, SKY_GALAXY_INTENSITY, offsetof(SkyParams, galaxy_intensity) },
    { "galaxy_dir",        KT_VEC3,   SKY_GALAXY_DIR,       offsetof(SkyParams, galaxy_dir) },
    { "nebula_dir",        KT_VEC3,   SKY_NEBULA_DIR,       offsetof(SkyParams, nebula_dir) },
    { "galaxy_tilt",       KT_DOUBLE, SKY_GALAXY_TILT,      offsetof(SkyParams, galaxy_tilt) },
    { "galaxy_roll",       KT_DOUBLE, SKY_GALAXY_ROLL,      offsetof(SkyParams, galaxy_roll) }
};

enum {
    FOG_DENSITY        = 1u << 0,
    FOG_COLOR          = 1u << 1,
    FOG_HEIGHT         = 1u << 2,
    FOG_HEIGHT_FALLOFF = 1u << 3,
    FOG_INSCATTER_STR  = 1u << 4,
    FOG_SUN_ANISOTROPY = 1u << 5,
    FOG_NOISE_SCALE    = 1u << 6,
    FOG_NOISE_AMOUNT   = 1u << 7
};

static const KeySpec FOG_KEYS[] = {
    { "density",            KT_DOUBLE, FOG_DENSITY,        offsetof(FogParams, density) },
    { "color",              KT_VEC3,   FOG_COLOR,          offsetof(FogParams, color) },
    { "height",             KT_DOUBLE, FOG_HEIGHT,         offsetof(FogParams, height) },
    { "height_falloff",     KT_DOUBLE, FOG_HEIGHT_FALLOFF, offsetof(FogParams, height_falloff) },
    { "inscatter_strength", KT_DOUBLE, FOG_INSCATTER_STR,  offsetof(FogParams, inscatter_strength) },
    { "sun_anisotropy",     KT_DOUBLE, FOG_SUN_ANISOTROPY, offsetof(FogParams, sun_anisotropy) },
    { "noise_scale",        KT_DOUBLE, FOG_NOISE_SCALE,    offsetof(FogParams, noise_scale) },
    { "noise_amount",       KT_DOUBLE, FOG_NOISE_AMOUNT,   offsetof(FogParams, noise_amount) }
};

/* ------------------------------------------------------------------ */
/* Parser: material block (docs/scene_format.md §4.3)                  */
/* ------------------------------------------------------------------ */

enum {
    MAT_ALBEDO       = 1u << 0,
    MAT_SPECULAR     = 1u << 1,
    MAT_SHININESS    = 1u << 2,
    MAT_REFLECTIVITY = 1u << 3,
    MAT_TRANSPARENCY = 1u << 4,
    MAT_IOR          = 1u << 5,
    MAT_IS_WATER     = 1u << 6,
    MAT_ABSORPTION   = 1u << 7,
    MAT_DEEP_COLOR   = 1u << 8,
    MAT_TYPE         = 1u << 9,
    /* Procedural texture keys (§4.3). `texture` is enum-ish and handled by the
     * `sd_apply_material_key` special case below; the other three are generic. */
    MAT_TEXTURE_KIND = 1u << 10,
    MAT_TEX_SCALE    = 1u << 11,
    MAT_TEX_COLOR_A  = 1u << 12,
    MAT_TEX_COLOR_B  = 1u << 13,
    /* General transmissive Beer-Lambert attenuation flag (§4.3). */
    MAT_BEER_LAMBERT = 1u << 14,
    /* Opt-in physically-based material keys (§4.3, docs/research_pbr_shading.md).
     * The next free bit above MAT_BEER_LAMBERT (1u<<14) is 1u<<15. */
    MAT_METALLIC        = 1u << 15,
    MAT_ROUGHNESS       = 1u << 16,
    MAT_EMISSIVE        = 1u << 17,
    MAT_PBR             = 1u << 18,
    MAT_BUMP_STRENGTH   = 1u << 19,
    MAT_BUMP_SCALE      = 1u << 20,
    MAT_ATMOSPHERE_GLOW = 1u << 21
};

static const KeySpec MAT_KEYS[] = {
    { "albedo",       KT_VEC3,   MAT_ALBEDO,       offsetof(Material, albedo) },
    { "specular",     KT_VEC3,   MAT_SPECULAR,     offsetof(Material, specular) },
    { "shininess",    KT_DOUBLE, MAT_SHININESS,    offsetof(Material, shininess) },
    { "reflectivity", KT_DOUBLE, MAT_REFLECTIVITY, offsetof(Material, reflectivity) },
    { "transparency", KT_DOUBLE, MAT_TRANSPARENCY, offsetof(Material, transparency) },
    { "ior",          KT_DOUBLE, MAT_IOR,          offsetof(Material, ior) },
    { "is_water",     KT_BOOL,   MAT_IS_WATER,     offsetof(Material, is_water) },
    { "beer_lambert", KT_BOOL,   MAT_BEER_LAMBERT, offsetof(Material, beer_lambert) },
    { "absorption",   KT_VEC3,   MAT_ABSORPTION,   offsetof(Material, absorption) },
    { "deep_color",   KT_VEC3,   MAT_DEEP_COLOR,   offsetof(Material, deep_color) },
    /* Opt-in PBR layer (§4.3). Defaults zero => no legacy behaviour change. */
    { "metallic",     KT_DOUBLE, MAT_METALLIC,     offsetof(Material, metallic) },
    { "roughness",    KT_DOUBLE, MAT_ROUGHNESS,    offsetof(Material, roughness) },
    { "emissive",     KT_VEC3,   MAT_EMISSIVE,     offsetof(Material, emissive) },
    { "pbr",          KT_BOOL,   MAT_PBR,          offsetof(Material, pbr) },
    { "texture_scale",   KT_DOUBLE, MAT_TEX_SCALE,   offsetof(Material, texture_scale) },
    { "texture_color_a", KT_VEC3,   MAT_TEX_COLOR_A, offsetof(Material, texture_color_a) },
    { "texture_color_b", KT_VEC3,   MAT_TEX_COLOR_B, offsetof(Material, texture_color_b) },
    { "bump_strength",   KT_DOUBLE, MAT_BUMP_STRENGTH,   offsetof(Material, bump_strength) },
    { "bump_scale",      KT_DOUBLE, MAT_BUMP_SCALE,      offsetof(Material, bump_scale) },
    { "atmosphere_glow", KT_VEC3,   MAT_ATMOSPHERE_GLOW, offsetof(Material, atmosphere_glow) }
};

/*
 * `type = water` preset: exactly the current MAT_WATER values (§4.3,
 * src/scene.c, cross-checked against scene_desc_write.c's detection).
 */
static Material sd_water_preset(void)
{
    Material m;

    memset(&m, 0, sizeof m);
    m.albedo = vec3(0.05, 0.15, 0.20);
    m.specular = vec3(0.90, 0.90, 0.90);
    m.shininess = 256.0;
    m.reflectivity = 1.0;
    m.transparency = 0.85;
    m.ior = 1.33;
    m.is_water = 1;
    m.absorption = vec3(0.45, 0.12, 0.06);
    m.deep_color = vec3(0.02, 0.10, 0.16);
    material_texture_defaults(&m);
    return m;
}

/*
 * `type = glass` preset: a general clear refractive material (§4.3).
 *
 * `is_water = 0` keeps the exact geometric normal (no wave perturbation) while
 * `beer_lambert = 0` leaves attenuation opt-in: a clear glass with zero
 * `absorption` is a strict no-op relative to the historical `refr_col = inner`
 * path. `deep_color` is the neutral medium colour reached as depth -> inf.
 * Cross-checked against scene_desc_write.c's mat_is_glass_preset().
 */
static Material sd_glass_preset(void)
{
    Material m;

    memset(&m, 0, sizeof m);
    m.albedo = vec3(0.02, 0.02, 0.02);
    m.specular = vec3(1.0, 1.0, 1.0);
    m.shininess = 256.0;
    m.reflectivity = 0.0;
    m.transparency = 1.0;
    m.ior = 1.5;
    m.is_water = 0;
    m.beer_lambert = 0;
    m.absorption = vec3(0.0, 0.0, 0.0);
    m.deep_color = vec3(0.5, 0.5, 0.5);
    material_texture_defaults(&m);
    return m;
}

/* `type = opaque` preset: the all-zero opaque default, ior 1, not water. */
static Material sd_opaque_preset(void)
{
    Material m;

    memset(&m, 0, sizeof m);
    m.ior = 1.0;
    material_texture_defaults(&m);
    return m;
}

/* ------------------------------------------------------------------ */
/* Named real-world material presets (docs/scene_format.md §4.3)        */
/*                                                                      */
/* Values are taken verbatim from docs/research_material_reference.md   */
/* (t-027). Every preset is a pure nullary function returning a         */
/* `Material` by value, mirroring sd_water_preset()/sd_glass_preset().  */
/* ------------------------------------------------------------------ */

/*
 * Selector stored in Parser.mat_type: 0 = no preset, every other value names
 * the preset swapped in as the base at material-block close. The order here is
 * the documented §4.3 order and is independent of the numeric value.
 */
enum {
    SD_MAT_NONE = 0,
    SD_MAT_WATER,
    SD_MAT_OPAQUE,
    SD_MAT_GLASS,
    SD_MAT_GOLD,
    SD_MAT_COPPER,
    SD_MAT_SILVER,
    SD_MAT_ALUMINUM,
    SD_MAT_IRON,
    SD_MAT_CHROME,
    SD_MAT_BRASS,
    SD_MAT_PLASTIC,
    SD_MAT_RUBBER,
    SD_MAT_CERAMIC,
    SD_MAT_DIAMOND,
    SD_MAT_EMISSIVE
};

/*
 * Keyword spelling -> SD_MAT_* selector, in the §4.3 documented order. The
 * parser walks this table (case-sensitive, lower-case ASCII keywords) so the
 * `type` special case stays a small data-driven loop and the error message can
 * enumerate the valid names.
 */
static const struct { const char *kw; int type; } SD_MAT_TYPE_NAMES[] = {
    { "water",    SD_MAT_WATER    },
    { "opaque",   SD_MAT_OPAQUE   },
    { "glass",    SD_MAT_GLASS    },
    { "gold",     SD_MAT_GOLD     },
    { "copper",   SD_MAT_COPPER   },
    { "silver",   SD_MAT_SILVER   },
    { "aluminum", SD_MAT_ALUMINUM },
    { "iron",     SD_MAT_IRON     },
    { "chrome",   SD_MAT_CHROME   },
    { "brass",    SD_MAT_BRASS    },
    { "plastic",  SD_MAT_PLASTIC  },
    { "rubber",   SD_MAT_RUBBER   },
    { "ceramic",  SD_MAT_CERAMIC  },
    { "diamond",  SD_MAT_DIAMOND  },
    { "emissive", SD_MAT_EMISSIVE }
};

/*
 * Shared base for the polished-conductor presets (§2). A metal has no diffuse
 * term: `albedo` IS its F0, so `F0 = mix(vec3(0.04), albedo, metallic)` reduces
 * to `albedo` exactly. `specular` mirrors `albedo` so the legacy Blinn-Phong
 * path (used when `pbr = 0`) still tints the highlight with the metal colour.
 * `reflectivity = 0` avoids double-counting the mirror mix (the PBR specular
 * term already provides the reflection) and `ior = 1` is the neutral default.
 */
static Material sd_conductor_preset(Vec3 f0, double roughness)
{
    Material m;

    memset(&m, 0, sizeof m);
    m.albedo = f0;
    m.specular = f0;
    m.shininess = 256.0; /* mirror-like Blinn-Phong fallback */
    m.reflectivity = 0.0;
    m.transparency = 0.0;
    m.ior = 1.0;
    m.is_water = 0;
    m.beer_lambert = 0;
    m.absorption = vec3(0.0, 0.0, 0.0);
    m.deep_color = vec3(0.0, 0.0, 0.0);
    m.metallic = 1.0;
    m.roughness = roughness;
    m.emissive = vec3(0.0, 0.0, 0.0);
    m.pbr = 1;
    material_texture_defaults(&m);
    return m;
}

/*
 * Shared base for the opaque dielectric presets (§3): `metallic = 0` so the
 * PBR path's flat 4 % dielectric F0 (`mix(vec3(0.04), albedo, 0)`) applies and
 * colour is driven through `albedo`. Chromatic diffuse, achromatic specular.
 */
static Material sd_dielectric_preset(Vec3 albedo, Vec3 specular, double shininess,
                                     double roughness, double ior)
{
    Material m;

    memset(&m, 0, sizeof m);
    m.albedo = albedo;
    m.specular = specular;
    m.shininess = shininess;
    m.reflectivity = 0.0;
    m.transparency = 0.0;
    m.ior = ior;
    m.is_water = 0;
    m.beer_lambert = 0;
    m.absorption = vec3(0.0, 0.0, 0.0);
    m.deep_color = vec3(0.0, 0.0, 0.0);
    m.metallic = 0.0;
    m.roughness = roughness;
    m.emissive = vec3(0.0, 0.0, 0.0);
    m.pbr = 1;
    material_texture_defaults(&m);
    return m;
}

/* Conductors: F0 linear RGB [Unity] with the recommended polished roughness. */
static Material sd_gold_preset(void)
{
    return sd_conductor_preset(vec3(1.000, 0.766, 0.336), 0.05);
}
static Material sd_copper_preset(void)
{
    return sd_conductor_preset(vec3(0.955, 0.637, 0.538), 0.05);
}
static Material sd_silver_preset(void)
{
    return sd_conductor_preset(vec3(0.972, 0.960, 0.915), 0.03);
}
static Material sd_aluminum_preset(void)
{
    return sd_conductor_preset(vec3(0.913, 0.921, 0.925), 0.05);
}
static Material sd_iron_preset(void)
{
    return sd_conductor_preset(vec3(0.560, 0.570, 0.580), 0.10);
}
static Material sd_chrome_preset(void)
{
    return sd_conductor_preset(vec3(0.550, 0.556, 0.554), 0.03);
}
static Material sd_brass_preset(void)
{
    return sd_conductor_preset(vec3(0.910, 0.778, 0.423), 0.08);
}

/* Dielectrics: dye/base colour in `albedo`, ~4 % specular tint, per-material
 * IOR and roughness (§3.1-§3.3). */
static Material sd_plastic_preset(void)
{
    return sd_dielectric_preset(vec3(0.30, 0.05, 0.06), vec3(0.05, 0.05, 0.05),
                                64.0, 0.10, 1.46);
}
static Material sd_rubber_preset(void)
{
    return sd_dielectric_preset(vec3(0.05, 0.05, 0.05), vec3(0.04, 0.04, 0.04),
                                8.0, 0.90, 1.50);
}
static Material sd_ceramic_preset(void)
{
    return sd_dielectric_preset(vec3(0.85, 0.85, 0.82), vec3(0.05, 0.05, 0.05),
                                128.0, 0.20, 1.60);
}

/*
 * `type = diamond` (§3.4): a transparent dielectric that reuses the glass /
 * `beer_lambert` conventions from sd_glass_preset() with the measured diamond
 * IOR (2.417 at 589.29 nm). `is_water = 0` keeps the exact geometric normal
 * (no wave perturbation) and `beer_lambert = 0` leaves attenuation opt-in.
 */
static Material sd_diamond_preset(void)
{
    Material m;

    memset(&m, 0, sizeof m);
    m.albedo = vec3(0.02, 0.02, 0.02);
    m.specular = vec3(1.0, 1.0, 1.0);
    m.shininess = 256.0;
    m.reflectivity = 0.0;
    m.transparency = 1.0;
    m.ior = 2.417;
    m.is_water = 0;
    m.beer_lambert = 0;
    m.absorption = vec3(0.0, 0.0, 0.0);
    m.deep_color = vec3(0.5, 0.5, 0.5);
    m.metallic = 0.0;
    m.roughness = 0.0;
    m.emissive = vec3(0.0, 0.0, 0.0);
    m.pbr = 1;
    material_texture_defaults(&m);
    return m;
}

/*
 * `type = emissive` (§4): a warm-white lamp emitter. A pure emitter has no
 * diffuse or specular response (`albedo`/`specular` black, `shininess` 0), and
 * adds its self-lit `emissive` colour once per shaded hit. Kept in the PBR path
 * (`pbr = 1`) with a neutral roughness.
 */
static Material sd_emissive_preset(void)
{
    Material m;

    memset(&m, 0, sizeof m);
    m.albedo = vec3(0.0, 0.0, 0.0);
    m.specular = vec3(0.0, 0.0, 0.0);
    m.shininess = 0.0;
    m.reflectivity = 0.0;
    m.transparency = 0.0;
    m.ior = 1.0;
    m.is_water = 0;
    m.beer_lambert = 0;
    m.absorption = vec3(0.0, 0.0, 0.0);
    m.deep_color = vec3(0.0, 0.0, 0.0);
    m.metallic = 0.0;
    m.roughness = 0.5;
    m.emissive = vec3(1.0, 0.85, 0.65);
    m.pbr = 1;
    material_texture_defaults(&m);
    return m;
}

/* Preset for an SD_MAT_* selector; SD_MAT_NONE maps to the opaque default. */
static Material sd_preset_for_type(int type)
{
    switch (type) {
    case SD_MAT_WATER:    return sd_water_preset();
    case SD_MAT_GLASS:    return sd_glass_preset();
    case SD_MAT_GOLD:     return sd_gold_preset();
    case SD_MAT_COPPER:   return sd_copper_preset();
    case SD_MAT_SILVER:   return sd_silver_preset();
    case SD_MAT_ALUMINUM: return sd_aluminum_preset();
    case SD_MAT_IRON:     return sd_iron_preset();
    case SD_MAT_CHROME:   return sd_chrome_preset();
    case SD_MAT_BRASS:    return sd_brass_preset();
    case SD_MAT_PLASTIC:  return sd_plastic_preset();
    case SD_MAT_RUBBER:   return sd_rubber_preset();
    case SD_MAT_CERAMIC:  return sd_ceramic_preset();
    case SD_MAT_DIAMOND:  return sd_diamond_preset();
    case SD_MAT_EMISSIVE: return sd_emissive_preset();
    case SD_MAT_OPAQUE:
    default:              return sd_opaque_preset();
    }
}

/* SD_MAT_* selector for a `type = <name>` keyword, or -1 when unknown. */
static int sd_mat_type_from_name(const char *name)
{
    size_t i;

    for (i = 0; i < SD_ARRAY_LEN(SD_MAT_TYPE_NAMES); i++) {
        if (strcmp(name, SD_MAT_TYPE_NAMES[i].kw) == 0)
            return SD_MAT_TYPE_NAMES[i].type;
    }
    return -1;
}

/*
 * Reset the per-block material state when a `material <name> {` header opens.
 * §4.3 defaults are the opaque preset (all-zero, ior 1); the `type` key then
 * swaps in a preset, and explicitly-set keys override it at close time.
 */
static void sd_mat_begin(Parser *p, const char *name)
{
    p->mat = sd_opaque_preset();
    p->mat_type = 0;
    snprintf(p->mat_name, sizeof p->mat_name, "%s", name);
}

/*
 * Reset the per-block primitive state when a primitive header opens. The
 * referenced material name/line are filled in by the `material = <name>` key.
 */
static void sd_prim_begin(Parser *p)
{
    memset(&p->prim, 0, sizeof p->prim);
    p->prim_mat_name[0] = '\0';
    p->prim_mat_line = p->line;
}

/* ------------------------------------------------------------------ */
/* Parser: primitive blocks (docs/scene_format.md §4.4 - §4.8)         */
/* ------------------------------------------------------------------ */

/*
 * Per-kind geometry key tables. The bit values are local to each table: a
 * primitive block only ever sees its own kind's keys, so `seen` reuse is
 * safe. Offsets point straight at the matching ScenePrimDesc fields, which
 * mirror the grammar (and the flat Primitive layout) exactly.
 */
static const KeySpec PRIM_SPHERE_KEYS[] = {
    { "center", KT_VEC3,   1u << 0, offsetof(ScenePrimDesc, center) },
    { "radius", KT_DOUBLE, 1u << 1, offsetof(ScenePrimDesc, radius) }
};
static const KeySpec PRIM_PLANE_KEYS[] = {
    { "point",  KT_VEC3, 1u << 0, offsetof(ScenePrimDesc, point) },
    { "normal", KT_VEC3, 1u << 1, offsetof(ScenePrimDesc, normal) }
};
static const KeySpec PRIM_BOX_KEYS[] = {
    { "center", KT_VEC3, 1u << 0, offsetof(ScenePrimDesc, center) },
    { "half",   KT_VEC3, 1u << 1, offsetof(ScenePrimDesc, half) }
};
static const KeySpec PRIM_TRIANGLE_KEYS[] = {
    { "a", KT_VEC3, 1u << 0, offsetof(ScenePrimDesc, a) },
    { "b", KT_VEC3, 1u << 1, offsetof(ScenePrimDesc, b) },
    { "c", KT_VEC3, 1u << 2, offsetof(ScenePrimDesc, c) }
};
static const KeySpec PRIM_CYLINDER_KEYS[] = {
    { "base",     KT_VEC3,   1u << 0, offsetof(ScenePrimDesc, base) },
    { "top",      KT_VEC3,   1u << 1, offsetof(ScenePrimDesc, top) },
    { "r_bottom", KT_DOUBLE, 1u << 2, offsetof(ScenePrimDesc, radius) },
    { "r_top",    KT_DOUBLE, 1u << 3, offsetof(ScenePrimDesc, radius2) }
};

typedef struct {
    const char    *kw;
    PrimKind       kind;
    const KeySpec *keys;
    size_t         nkeys;
    unsigned       required; /* OR of the geometry keys that must be present */
} PrimSpec;

static const PrimSpec PRIM_SPECS[] = {
    { "sphere",   PRIM_SPHERE,   PRIM_SPHERE_KEYS,   SD_ARRAY_LEN(PRIM_SPHERE_KEYS),   (1u << 0) | (1u << 1) },
    { "plane",    PRIM_PLANE,    PRIM_PLANE_KEYS,    SD_ARRAY_LEN(PRIM_PLANE_KEYS),    (1u << 0) | (1u << 1) },
    { "box",      PRIM_BOX,      PRIM_BOX_KEYS,      SD_ARRAY_LEN(PRIM_BOX_KEYS),      (1u << 0) | (1u << 1) },
    { "triangle", PRIM_TRIANGLE, PRIM_TRIANGLE_KEYS, SD_ARRAY_LEN(PRIM_TRIANGLE_KEYS), (1u << 0) | (1u << 1) | (1u << 2) },
    { "cylinder", PRIM_CYLINDER, PRIM_CYLINDER_KEYS, SD_ARRAY_LEN(PRIM_CYLINDER_KEYS), (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) }
};

static const PrimSpec *sd_prim_spec(const char *kw)
{
    size_t i;

    for (i = 0; i < SD_ARRAY_LEN(PRIM_SPECS); i++) {
        if (strcmp(kw, PRIM_SPECS[i].kw) == 0)
            return &PRIM_SPECS[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Parser: plant blocks (docs/scene_format.md §4.9 - §4.10)            */
/* ------------------------------------------------------------------ */

enum {
    PLANT_POSITION          = 1u << 0,
    PLANT_HEIGHT            = 1u << 1,
    PLANT_RADIUS            = 1u << 2,
    PLANT_SEED              = 1u << 3,
    PLANT_LEAF_VARIANT      = 1u << 4,
    PLANT_MAX_DEPTH         = 1u << 5,
    PLANT_MIN_BRANCH_RADIUS = 1u << 6,
    PLANT_TAPER             = 1u << 7,
    PLANT_LEN_DECAY         = 1u << 8,
    PLANT_SPREAD_DEG        = 1u << 9,
    PLANT_PERTURB_DEG       = 1u << 10,
    PLANT_UP_BIAS           = 1u << 11,
    PLANT_THIRD_CHILD_CHANCE= 1u << 12,
    PLANT_LEAF_MIN          = 1u << 13,
    PLANT_LEAF_SPAN         = 1u << 14,
    PLANT_FOLIAGE           = 1u << 15,
    PLANT_TYPE              = 1u << 16
};

/*
 * `material_bark` / `material_leaf` carry NAMES, not a scalar field, so they
 * use bits far above the geometry bits and are stored into the parser's name
 * buffers (the referenced material table is only complete after the whole
 * file has been read, so resolution is deferred to the second pass, exactly
 * as for primitive material references).
 */
#define SD_PLANT_BARK_BIT (1u << 20)
#define SD_PLANT_LEAF_BIT (1u << 21)

static const KeySpec PLANT_KEYS[] = {
    { "position",          KT_VEC3,   PLANT_POSITION,          offsetof(ScenePlantDesc, position) },
    { "height",            KT_DOUBLE, PLANT_HEIGHT,            offsetof(ScenePlantDesc, height) },
    { "radius",            KT_DOUBLE, PLANT_RADIUS,            offsetof(ScenePlantDesc, radius) },
    { "seed",              KT_UINT,   PLANT_SEED,              offsetof(ScenePlantDesc, seed) },
    { "leaf_variant",      KT_INT,    PLANT_LEAF_VARIANT,      offsetof(ScenePlantDesc, leaf_variant) },
    { "max_depth",         KT_INT,    PLANT_MAX_DEPTH,         offsetof(ScenePlantDesc, max_depth) },
    { "min_branch_radius", KT_DOUBLE, PLANT_MIN_BRANCH_RADIUS, offsetof(ScenePlantDesc, min_branch_radius) },
    { "taper",             KT_DOUBLE, PLANT_TAPER,             offsetof(ScenePlantDesc, taper) },
    { "len_decay",         KT_DOUBLE, PLANT_LEN_DECAY,         offsetof(ScenePlantDesc, len_decay) },
    { "spread_deg",        KT_DOUBLE, PLANT_SPREAD_DEG,        offsetof(ScenePlantDesc, spread_deg) },
    { "perturb_deg",       KT_DOUBLE, PLANT_PERTURB_DEG,       offsetof(ScenePlantDesc, perturb_deg) },
    { "up_bias",           KT_DOUBLE, PLANT_UP_BIAS,           offsetof(ScenePlantDesc, up_bias) },
    { "third_child_chance",KT_DOUBLE, PLANT_THIRD_CHILD_CHANCE,offsetof(ScenePlantDesc, third_child_chance) },
    { "leaf_min",          KT_INT,    PLANT_LEAF_MIN,          offsetof(ScenePlantDesc, leaf_min) },
    { "leaf_span",         KT_INT,    PLANT_LEAF_SPAN,         offsetof(ScenePlantDesc, leaf_span) }
};

/* Reset the per-block plant state when a `tree {` / `bush {` header opens. */
static void sd_plant_begin(Parser *p)
{
    memset(&p->plant, 0, sizeof p->plant);
    p->plant_bark_name[0] = '\0';
    p->plant_leaf_name[0] = '\0';
    p->plant_bark_line = p->line;
    p->plant_leaf_line = p->line;
}

/* ------------------------------------------------------------------ */
/* Parser: value readers                                               */
/* ------------------------------------------------------------------ */

static int sd_val_double(Parser *p, const Token *t, int n, int i, double *out)
{
    if (i >= n) {
        sd_err(p, "missing value", t[0].text);
        return -1;
    }
    if (t[i].kind != TOK_NUMBER) {
        sd_err(p, "expected a number", t[i].text);
        return -1;
    }
    *out = t[i].num;
    return i + 1;
}

static int sd_val_vec3(Parser *p, const Token *t, int n, int i, Vec3 *out)
{
    double x, y, z;
    int    next;

    if (i >= n) {
        sd_err(p, "missing value", t[0].text);
        return -1;
    }
    if (n - i != 3) {
        sd_err(p, "expected exactly three numbers", t[i].text);
        return -1;
    }
    next = sd_val_double(p, t, n, i, &x);
    if (next < 0)
        return -1;
    next = sd_val_double(p, t, n, next, &y);
    if (next < 0)
        return -1;
    next = sd_val_double(p, t, n, next, &z);
    if (next < 0)
        return -1;

    out->x = x;
    out->y = y;
    out->z = z;
    return next;
}

static int sd_val_int(Parser *p, const Token *t, int n, int i, int *out)
{
    double v;
    int    next = sd_val_double(p, t, n, i, &v);

    if (next < 0)
        return -1;
    if (v != floor(v)) {
        sd_err(p, "integer key requires a whole number", t[i].text);
        return -1;
    }
    if (v < -2147483648.0 || v > 2147483647.0) {
        sd_err(p, "integer out of range", t[i].text);
        return -1;
    }
    *out = (int)v;
    return next;
}

static int sd_val_uint(Parser *p, const Token *t, int n, int i, unsigned *out)
{
    double v;
    int    next = sd_val_double(p, t, n, i, &v);

    if (next < 0)
        return -1;
    if (v != floor(v) || v < 0.0 || v > 4294967295.0) {
        sd_err(p, "expected an unsigned integer", t[i].text);
        return -1;
    }
    *out = (unsigned)v;
    return next;
}

/* Boolean-ish flag: exactly the integers 0 or 1 (§3.4). */
static int sd_val_bool(Parser *p, const Token *t, int n, int i, int *out)
{
    double v;
    int    next = sd_val_double(p, t, n, i, &v);

    if (next < 0)
        return -1;
    if (v != 0.0 && v != 1.0) {
        sd_err(p, "expected 0 or 1", t[i].text);
        return -1;
    }
    *out = (v != 0.0);
    return next;
}

static int sd_val_name(Parser *p, const Token *t, int n, int i, const char **out)
{
    if (i >= n) {
        sd_err(p, "missing value", t[0].text);
        return -1;
    }
    if (t[i].kind != TOK_IDENT && t[i].kind != TOK_STRING) {
        sd_err(p, "expected a name", t[i].text);
        return -1;
    }
    *out = t[i].text;
    return i + 1;
}

/* ------------------------------------------------------------------ */
/* Parser: key application                                             */
/* ------------------------------------------------------------------ */

static void sd_warn_unknown_key(Parser *p, const char *key)
{
    fprintf(stderr, "%s:%d: warning: unknown key, using default (near '%s')\n",
            p->path, p->line, key);
}

static int sd_apply_value(Parser *p, void *base, const KeySpec *spec,
                          const Token *t, int n, int i)
{
    void *dst = (char *)base + spec->offset;
    int   next;

    if (i >= n) {
        sd_err(p, "missing value", t[0].text);
        return -1;
    }
    switch (spec->type) {
    case KT_DOUBLE: next = sd_val_double(p, t, n, i, (double *)dst); break;
    case KT_VEC3:   next = sd_val_vec3(p, t, n, i, (Vec3 *)dst);     break;
    case KT_INT:    next = sd_val_int(p, t, n, i, (int *)dst);       break;
    case KT_UINT:   next = sd_val_uint(p, t, n, i, (unsigned *)dst); break;
    case KT_BOOL:   next = sd_val_bool(p, t, n, i, (int *)dst);      break;
    default:
        sd_err(p, "internal error: bad key spec", spec->key);
        return -1;
    }
    if (next < 0)
        return -1;
    if (next != n) {
        sd_err(p, "unexpected token after value", t[next].text);
        return -1;
    }
    return 0;
}

/*
 * Apply `key = value...` from `t[0]` against a key table. Unknown keys are a
 * warning (forward compatibility, §6); a duplicate key in one block is a hard
 * error (§3.3).
 */
static void sd_apply_key(Parser *p, void *base, const KeySpec *specs,
                         size_t nspec, Block *b, const Token *t, int n)
{
    const char *key = t[0].text;
    size_t      s;

    for (s = 0; s < nspec; s++) {
        if (strcmp(key, specs[s].key) != 0)
            continue;
        if (b->seen & specs[s].bit) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= specs[s].bit;
        (void)sd_apply_value(p, base, &specs[s], t, n, 2);
        return;
    }
    sd_warn_unknown_key(p, key);
}

/* ------------------------------------------------------------------ */
/* Parser: material block application (§4.3, §5.1)                     */
/* ------------------------------------------------------------------ */

/* Linear, case-sensitive name lookup in the material table (§5.1.2). */
static int sd_find_material(const SceneDesc *d, const char *name)
{
    int i;

    if (name == NULL)
        return -1;
    for (i = 0; i < d->material_count; i++) {
        if (d->materials[i].name != NULL &&
            strcmp(d->materials[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Index of the first water material (is_water != 0), or -1. */
static int sd_find_water_material(const SceneDesc *d)
{
    int i;

    for (i = 0; i < d->material_count; i++) {
        if (d->materials[i].mat.is_water != 0)
            return i;
    }
    return -1;
}

/* Copy only the explicitly-set fields of `src` over `dst` (preset overlay). */
static void sd_overlay_explicit(Material *dst, const Material *src, unsigned seen)
{
    if (seen & MAT_ALBEDO)       dst->albedo = src->albedo;
    if (seen & MAT_SPECULAR)     dst->specular = src->specular;
    if (seen & MAT_SHININESS)    dst->shininess = src->shininess;
    if (seen & MAT_REFLECTIVITY) dst->reflectivity = src->reflectivity;
    if (seen & MAT_TRANSPARENCY) dst->transparency = src->transparency;
    if (seen & MAT_IOR)          dst->ior = src->ior;
    if (seen & MAT_IS_WATER)     dst->is_water = src->is_water;
    if (seen & MAT_BEER_LAMBERT) dst->beer_lambert = src->beer_lambert;
    if (seen & MAT_ABSORPTION)   dst->absorption = src->absorption;
    if (seen & MAT_DEEP_COLOR)   dst->deep_color = src->deep_color;
    if (seen & MAT_METALLIC)     dst->metallic = src->metallic;
    if (seen & MAT_ROUGHNESS)    dst->roughness = src->roughness;
    if (seen & MAT_EMISSIVE)     dst->emissive = src->emissive;
    if (seen & MAT_PBR)          dst->pbr = src->pbr;
    if (seen & MAT_TEXTURE_KIND) dst->texture_kind = src->texture_kind;
    if (seen & MAT_TEX_SCALE)    dst->texture_scale = src->texture_scale;
    if (seen & MAT_TEX_COLOR_A)  dst->texture_color_a = src->texture_color_a;
    if (seen & MAT_TEX_COLOR_B)  dst->texture_color_b = src->texture_color_b;
}

/* Apply one `key = value` line inside a `material` block. */
static void sd_apply_material_key(Parser *p, Block *b, const Token *t, int n)
{
    const char *key = t[0].text;

    if (strcmp(key, "type") == 0) {
        const char *name = NULL;
        int         mt;

        if (b->seen & MAT_TYPE) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= MAT_TYPE;
        if (sd_val_name(p, t, n, 2, &name) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        mt = sd_mat_type_from_name(name);
        if (mt < 0) {
            sd_err(p, "unknown material type, expected one of 'water', "
                      "'opaque', 'glass', 'gold', 'copper', 'silver', "
                      "'aluminum', 'iron', 'chrome', 'brass', 'plastic', "
                      "'rubber', 'ceramic', 'diamond', 'emissive'", name);
            return;
        }
        p->mat_type = mt;
        return;
    }

    /*
     * `texture = none|checker|stripes` is enum-ish (like `type`), so it is not
     * a generic KeySpec row. Store the decoded kind directly into the material
     * under construction and mark the bit so presets/duplicates behave.
     */
    if (strcmp(key, "texture") == 0) {
        const char *name = NULL;
        int kind;

        if (b->seen & MAT_TEXTURE_KIND) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= MAT_TEXTURE_KIND;
        if (sd_val_name(p, t, n, 2, &name) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        if (strcmp(name, "none") == 0)
            kind = TEXTURE_NONE;
        else if (strcmp(name, "checker") == 0)
            kind = TEXTURE_CHECKER;
        else if (strcmp(name, "stripes") == 0)
            kind = TEXTURE_STRIPES;
        else if (strcmp(name, "earth") == 0)
            kind = TEXTURE_PLANET_EARTH;
        else if (strcmp(name, "moon") == 0)
            kind = TEXTURE_PLANET_MOON;
        else if (strcmp(name, "noise") == 0)
            kind = TEXTURE_NOISE;
        else {
            sd_err(p, "unknown texture kind, expected 'none', 'checker', 'stripes', 'earth', 'moon' or 'noise'", name);
            return;
        }
        p->mat.texture_kind = kind;
        return;
    }

    sd_apply_key(p, &p->mat, MAT_KEYS, SD_ARRAY_LEN(MAT_KEYS), b, t, n);
}

/* Close a `material` block: overlay the preset, reject duplicates, append. */
static void sd_close_material(Parser *p, SceneDesc *d, const Block *b)
{
    Material final;

    if (sd_find_material(d, p->mat_name) >= 0) {
        sd_err_at(p, b->line, "duplicate material name", p->mat_name);
        return;
    }
    /* §4.3: the `type` preset is the base; explicitly-set keys override it.
     * With no `type`, p->mat already holds the defaults + explicit keys. */
    if (p->mat_type == SD_MAT_NONE) {
        final = p->mat;
    } else {
        final = sd_preset_for_type(p->mat_type);
        sd_overlay_explicit(&final, &p->mat, b->seen);
    }

    if (scene_desc_add_material(d, p->mat_name, &final) < 0)
        sd_err(p, "out of memory", p->mat_name);
}

/* ------------------------------------------------------------------ */
/* Parser: primitive block application (§4.4 - §4.8)                   */
/* ------------------------------------------------------------------ */

/* `material` reference key uses a bit far above the geometry key bits. */
#define SD_MAT_REF_BIT (1u << 30)

/* First required geometry key not seen, or NULL when all are present. */
static const char *sd_first_missing(const PrimSpec *sp, unsigned seen)
{
    size_t i;

    for (i = 0; i < sp->nkeys; i++) {
        if ((sp->required & sp->keys[i].bit) && !(seen & sp->keys[i].bit))
            return sp->keys[i].key;
    }
    return NULL;
}

/* Apply one `key = value` line inside a primitive block. */
static void sd_apply_prim_key(Parser *p, const PrimSpec *sp, Block *b,
                              const Token *t, int n)
{
    const char *key = t[0].text;
    size_t      s;

    if (strcmp(key, "material") == 0) {
        const char *name = NULL;
        if (b->seen & SD_MAT_REF_BIT) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= SD_MAT_REF_BIT;
        if (sd_val_name(p, t, n, 2, &name) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        snprintf(p->prim_mat_name, sizeof p->prim_mat_name, "%s", name);
        p->prim_mat_line = p->line;
        return;
    }
    for (s = 0; s < sp->nkeys; s++) {
        if (strcmp(key, sp->keys[s].key) != 0)
            continue;
        if (b->seen & sp->keys[s].bit) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= sp->keys[s].bit;
        (void)sd_apply_value(p, &p->prim, &sp->keys[s], t, n, 2);
        return;
    }
    sd_warn_unknown_key(p, key);
}

/* Record the reference line of the primitive that is about to be appended. */
static int sd_push_ref_line(Parser *p, int line)
{
    int *grown;
    int  ncap;

    if (p->ref_count >= p->ref_cap) {
        ncap = (p->ref_cap > 0) ? p->ref_cap * 2 : 16;
        grown = (int *)realloc(p->ref_lines, (size_t)ncap * sizeof(int));
        if (grown == NULL)
            return -1;
        p->ref_lines = grown;
        p->ref_cap = ncap;
    }
    p->ref_lines[p->ref_count++] = line;
    return 0;
}

static void scene_prim_desc_destroy(ScenePrimDesc *p)
{
    if (p == NULL)
        return;
    free(p->material_name);
    p->material_name = NULL;
    if (p->left != NULL) {
        scene_prim_desc_destroy(p->left);
        free(p->left);
        p->left = NULL;
    }
    if (p->right != NULL) {
        scene_prim_desc_destroy(p->right);
        free(p->right);
        p->right = NULL;
    }
}

static ScenePrimDesc *scene_prim_desc_clone(const ScenePrimDesc *p)
{
    ScenePrimDesc *c;
    if (p == NULL)
        return NULL;
    c = (ScenePrimDesc *)malloc(sizeof(ScenePrimDesc));
    if (c == NULL)
        return NULL;
    *c = *p;
    c->material_name = sd_strdup(p->material_name);
    c->left = scene_prim_desc_clone(p->left);
    c->right = scene_prim_desc_clone(p->right);
    return c;
}

static void sd_close_prim_csg_child(Parser *p, const Block *b, Block *parent)
{
    const PrimSpec *sp = sd_prim_spec(b->kw);
    const char     *missing;

    if (sp == NULL) {
        sd_err(p, "internal error: unknown primitive keyword", b->kw);
        return;
    }
    missing = sd_first_missing(sp, b->seen);
    if (missing != NULL) {
        sd_err_at(p, b->line, "missing required key", missing);
        return;
    }
    if (!(b->seen & SD_MAT_REF_BIT) && parent->csg_mat_name[0] == '\0') {
        sd_err_at(p, b->line, "missing required key", "material");
        return;
    }
    if (parent->csg_child_count >= 2) {
        sd_err_at(p, b->line, "CSG operation accepts exactly 2 children", b->kw);
        return;
    }

    ScenePrimDesc *slot = &parent->csg_children[parent->csg_child_count++];
    memset(slot, 0, sizeof(*slot));
    *slot = p->prim;
    slot->kind = sp->kind;
    if (b->seen & SD_MAT_REF_BIT) {
        slot->material_name = sd_strdup(p->prim_mat_name);
    } else if (parent->csg_mat_name[0] != '\0') {
        slot->material_name = sd_strdup(parent->csg_mat_name);
    }
    slot->material_index = SCENE_DESC_NO_MATERIAL;
    slot->left = NULL;
    slot->right = NULL;
}

static void sd_close_csg(Parser *p, SceneDesc *d, Block *b, Block *parent)
{
    if (b->csg_child_count != 2) {
        sd_err_at(p, b->line, "CSG operation requires exactly 2 children", b->kw);
        return;
    }
    ScenePrimDesc prim;
    memset(&prim, 0, sizeof(prim));
    prim.kind = PRIM_CSG;
    prim.csg_op = b->csg_op;
    prim.material_name = (b->csg_mat_name[0] != '\0') ? sd_strdup(b->csg_mat_name) : NULL;
    prim.material_index = SCENE_DESC_NO_MATERIAL;
    prim.left = scene_prim_desc_clone(&b->csg_children[0]);
    prim.right = scene_prim_desc_clone(&b->csg_children[1]);

    scene_prim_desc_destroy(&b->csg_children[0]);
    scene_prim_desc_destroy(&b->csg_children[1]);
    b->csg_child_count = 0;

    if (parent != NULL && parent->kind == BLK_CSG) {
        if (parent->csg_child_count < 2) {
            parent->csg_children[parent->csg_child_count++] = prim;
        } else {
            sd_err_at(p, b->line, "CSG operation accepts exactly 2 children", b->kw);
            scene_prim_desc_destroy(&prim);
        }
    } else {
        int idx = scene_desc_add_prim(d, &prim);
        if (idx < 0) {
            sd_err(p, "out of memory", b->kw);
            scene_prim_desc_destroy(&prim);
            return;
        }
        if (sd_push_ref_line(p, b->csg_mat_line > 0 ? b->csg_mat_line : b->line) != 0)
            sd_err(p, "out of memory", b->kw);
        scene_prim_desc_destroy(&prim);
    }
}

static void sd_apply_csg_key(Parser *p, Block *b, const Token *t, int n)
{
    const char *key = t[0].text;
    if (strcmp(key, "material") == 0) {
        const char *name = NULL;
        if (sd_val_name(p, t, n, 2, &name) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        snprintf(b->csg_mat_name, sizeof b->csg_mat_name, "%s", name);
        b->csg_mat_line = p->line;
        b->seen |= SD_MAT_REF_BIT;
        return;
    }
    sd_err(p, "unknown CSG block key", key);
}

/* Close a primitive block: validate required keys, then append in file order. */
static void sd_close_prim(Parser *p, SceneDesc *d, const Block *b)
{
    const PrimSpec *sp = sd_prim_spec(b->kw);
    const char     *missing;
    int             idx;

    if (sp == NULL) {
        sd_err(p, "internal error: unknown primitive keyword", b->kw);
        return;
    }
    missing = sd_first_missing(sp, b->seen);
    if (missing != NULL) {
        sd_err_at(p, b->line, "missing required key", missing);
        return;
    }
    if (!(b->seen & SD_MAT_REF_BIT)) {
        sd_err_at(p, b->line, "missing required key", "material");
        return;
    }
    p->prim.kind = sp->kind;
    p->prim.material_name = p->prim_mat_name; /* add_prim copies the name */
    idx = scene_desc_add_prim(d, &p->prim);
    if (idx < 0) {
        sd_err(p, "out of memory", b->kw);
        return;
    }
    if (sd_push_ref_line(p, p->prim_mat_line) != 0)
        sd_err(p, "out of memory", b->kw);
}

/* Record two diagnostics lines (bark, leaf) for the plant being appended. */
static int sd_push_plant_lines(Parser *p, int bark_line, int leaf_line)
{
    int *grown;
    int  ncap;

    if (p->plant_line_count + 2 > p->plant_line_cap) {
        ncap = (p->plant_line_cap > 0) ? p->plant_line_cap * 2 : 16;
        while (ncap < p->plant_line_count + 2)
            ncap *= 2;
        grown = (int *)realloc(p->plant_lines, (size_t)ncap * sizeof(int));
        if (grown == NULL)
            return -1;
        p->plant_lines = grown;
        p->plant_line_cap = ncap;
    }
    p->plant_lines[p->plant_line_count++] = bark_line;
    p->plant_lines[p->plant_line_count++] = leaf_line;
    return 0;
}

/* Apply one `key = value` line inside a `tree` / `bush` block (§4.9, §4.10). */
static void sd_apply_plant_key(Parser *p, Block *b, const Token *t, int n)
{
    const char *key = t[0].text;
    size_t      s;

    if (strcmp(key, "material_bark") == 0) {
        const char *name = NULL;
        if (b->seen & SD_PLANT_BARK_BIT) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= SD_PLANT_BARK_BIT;
        if (sd_val_name(p, t, n, 2, &name) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        snprintf(p->plant_bark_name, sizeof p->plant_bark_name, "%s", name);
        p->plant_bark_line = p->line;
        return;
    }
    if (strcmp(key, "material_leaf") == 0) {
        const char *name = NULL;
        if (b->seen & SD_PLANT_LEAF_BIT) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= SD_PLANT_LEAF_BIT;
        if (sd_val_name(p, t, n, 2, &name) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        snprintf(p->plant_leaf_name, sizeof p->plant_leaf_name, "%s", name);
        p->plant_leaf_line = p->line;
        return;
    }
    if (strcmp(key, "type") == 0) {
        const char *name = NULL;
        if (b->seen & PLANT_TYPE) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= PLANT_TYPE;
        if (sd_val_name(p, t, n, 2, &name) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        p->plant.has_plant_type = 1;
        if (strcmp(name, "conifer") == 0 || strcmp(name, "spruce") == 0 || strcmp(name, "pine") == 0) {
            p->plant.plant_type = PLANT_TYPE_CONIFER;
            if (!(b->seen & PLANT_FOLIAGE)) {
                p->plant.has_foliage = 1;
                p->plant.foliage = PLANT_FOLIAGE_NEEDLES;
            }
        } else if (strcmp(name, "deciduous") == 0) {
            p->plant.plant_type = PLANT_TYPE_DECIDUOUS;
        } else if (strcmp(name, "bush") == 0) {
            p->plant.plant_type = PLANT_TYPE_BUSH;
        } else {
            sd_err(p, "unknown plant type", name);
        }
        return;
    }
    if (strcmp(key, "foliage") == 0) {
        const char *name = NULL;
        if (b->seen & PLANT_FOLIAGE) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= PLANT_FOLIAGE;
        if (sd_val_name(p, t, n, 2, &name) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        p->plant.has_foliage = 1;
        if (strcmp(name, "spheres") == 0) {
            p->plant.foliage = PLANT_FOLIAGE_SPHERES;
        } else if (strcmp(name, "leaves") == 0 || strcmp(name, "leaf") == 0 || strcmp(name, "polygons") == 0) {
            p->plant.foliage = PLANT_FOLIAGE_LEAVES;
        } else if (strcmp(name, "needles") == 0 || strcmp(name, "needle") == 0 || strcmp(name, "barr") == 0) {
            p->plant.foliage = PLANT_FOLIAGE_NEEDLES;
        } else {
            sd_err(p, "unknown foliage type", name);
        }
        return;
    }
    for (s = 0; s < SD_ARRAY_LEN(PLANT_KEYS); s++) {
        if (strcmp(key, PLANT_KEYS[s].key) != 0)
            continue;
        if (b->seen & PLANT_KEYS[s].bit) {
            sd_err(p, "duplicate key", key);
            return;
        }
        b->seen |= PLANT_KEYS[s].bit;
        (void)sd_apply_value(p, &p->plant, &PLANT_KEYS[s], t, n, 2);
        return;
    }
    sd_warn_unknown_key(p, key);
}

/*
 * Close a `tree` / `bush` block: validate the required keys (position and
 * height, §4.9/§4.10), record presence flags, then append in file order. The
 * referenced material names are stored verbatim; resolution to table indices
 * happens after the whole file has been read, so forward references work.
 */
static void sd_close_plant(Parser *p, SceneDesc *d, const Block *b)
{
    if (!(b->seen & PLANT_POSITION)) {
        sd_err_at(p, b->line, "missing required key", "position");
        return;
    }
    if (!(b->seen & PLANT_HEIGHT)) {
        sd_err_at(p, b->line, "missing required key", "height");
        return;
    }
    p->plant.kind = (strcmp(b->kw, "bush") == 0) ? SD_PLANT_BUSH
                                                 : SD_PLANT_TREE;
    if (strcmp(b->kw, "conifer") == 0 || strcmp(b->kw, "spruce") == 0 || strcmp(b->kw, "pine") == 0) {
        p->plant.has_plant_type = 1;
        p->plant.plant_type = PLANT_TYPE_CONIFER;
        if (!(b->seen & PLANT_FOLIAGE)) {
            p->plant.has_foliage = 1;
            p->plant.foliage = PLANT_FOLIAGE_NEEDLES;
        }
    }
    /* §4.9/§4.10: `position.y` is ignored and the trunk starts at ground. */
    p->plant.position.y = 0.0;
    p->plant.has_seed = (b->seen & PLANT_SEED) ? 1 : 0;
    p->plant.has_leaf_variant = (b->seen & PLANT_LEAF_VARIANT) ? 1 : 0;
    /* Record presence of each optional generator parameter (§4.9/§4.10). The
     * builder substitutes the legacy SCENE_* constant whenever a flag is 0. */
    p->plant.has_max_depth          = (b->seen & PLANT_MAX_DEPTH)          ? 1 : 0;
    p->plant.has_min_branch_radius  = (b->seen & PLANT_MIN_BRANCH_RADIUS)  ? 1 : 0;
    p->plant.has_taper              = (b->seen & PLANT_TAPER)              ? 1 : 0;
    p->plant.has_len_decay          = (b->seen & PLANT_LEN_DECAY)          ? 1 : 0;
    p->plant.has_spread_deg         = (b->seen & PLANT_SPREAD_DEG)         ? 1 : 0;
    p->plant.has_perturb_deg        = (b->seen & PLANT_PERTURB_DEG)        ? 1 : 0;
    p->plant.has_up_bias            = (b->seen & PLANT_UP_BIAS)            ? 1 : 0;
    p->plant.has_third_child_chance = (b->seen & PLANT_THIRD_CHILD_CHANCE) ? 1 : 0;
    p->plant.has_leaf_min           = (b->seen & PLANT_LEAF_MIN)           ? 1 : 0;
    p->plant.has_leaf_span          = (b->seen & PLANT_LEAF_SPAN)          ? 1 : 0;
    /* A `radius` default of 0.20 (tree) / 0.05 (bush) is applied by the
     * builder when the key is absent; 0.0 is the documented "unset" marker. */
    p->plant.material_bark = (b->seen & SD_PLANT_BARK_BIT)
                                 ? p->plant_bark_name : NULL;
    p->plant.material_leaf = (b->seen & SD_PLANT_LEAF_BIT)
                                 ? p->plant_leaf_name : NULL;
    p->plant.material_bark_index = SCENE_DESC_NO_MATERIAL;
    p->plant.material_leaf_index = SCENE_DESC_NO_MATERIAL;

    if (scene_desc_add_plant(d, &p->plant) < 0) {
        sd_err(p, "out of memory", b->kw);
        return;
    }
    if (sd_push_plant_lines(p, p->plant_bark_line, p->plant_leaf_line) != 0)
        sd_err(p, "out of memory", b->kw);
}

/* ------------------------------------------------------------------ */
/* Parser: boulder and light blocks                                   */
/* ------------------------------------------------------------------ */

enum {
    BOULDER_POSITION  = 1u << 0,
    BOULDER_RADIUS    = 1u << 1,
    BOULDER_ROUGHNESS = 1u << 2,
    BOULDER_FLATNESS  = 1u << 3,
    BOULDER_SEED      = 1u << 4,
    BOULDER_MATERIAL  = 1u << 5
};

enum {
    LIGHT_POSITION  = 1u << 0,
    LIGHT_COLOR     = 1u << 1,
    LIGHT_INTENSITY = 1u << 2,
    LIGHT_RADIUS    = 1u << 3
};

static void sd_boulder_begin(Parser *p)
{
    memset(&p->boulder, 0, sizeof p->boulder);
    p->boulder.radius = 1.0;
    p->boulder.roughness = 0.35;
    p->boulder.flatness = 0.75;
    p->boulder_mat_name[0] = '\0';
    p->boulder_mat_line = p->line;
}

static int sd_push_boulder_line(Parser *p, int line)
{
    int *grown;
    int  ncap;

    if (p->boulder_line_count >= p->boulder_line_cap) {
        ncap = (p->boulder_line_cap > 0) ? p->boulder_line_cap * 2 : 16;
        grown = (int *)realloc(p->boulder_lines, (size_t)ncap * sizeof(int));
        if (grown == NULL)
            return -1;
        p->boulder_lines = grown;
        p->boulder_line_cap = ncap;
    }
    p->boulder_lines[p->boulder_line_count++] = line;
    return 0;
}

static void sd_apply_boulder_key(Parser *p, Block *b, const Token *t, int n)
{
    const char *key = t[0].text;
    if (strcmp(key, "position") == 0 || strcmp(key, "center") == 0) {
        if (b->seen & BOULDER_POSITION) { sd_err(p, "duplicate key", key); return; }
        b->seen |= BOULDER_POSITION;
        (void)sd_val_vec3(p, t, n, 2, &p->boulder.position);
        return;
    }
    if (strcmp(key, "radius") == 0) {
        if (b->seen & BOULDER_RADIUS) { sd_err(p, "duplicate key", key); return; }
        b->seen |= BOULDER_RADIUS;
        (void)sd_val_double(p, t, n, 2, &p->boulder.radius);
        return;
    }
    if (strcmp(key, "roughness") == 0) {
        if (b->seen & BOULDER_ROUGHNESS) { sd_err(p, "duplicate key", key); return; }
        b->seen |= BOULDER_ROUGHNESS;
        (void)sd_val_double(p, t, n, 2, &p->boulder.roughness);
        return;
    }
    if (strcmp(key, "flatness") == 0) {
        if (b->seen & BOULDER_FLATNESS) { sd_err(p, "duplicate key", key); return; }
        b->seen |= BOULDER_FLATNESS;
        (void)sd_val_double(p, t, n, 2, &p->boulder.flatness);
        return;
    }
    if (strcmp(key, "seed") == 0) {
        if (b->seen & BOULDER_SEED) { sd_err(p, "duplicate key", key); return; }
        b->seen |= BOULDER_SEED;
        (void)sd_val_uint(p, t, n, 2, &p->boulder.seed);
        return;
    }
    if (strcmp(key, "material") == 0) {
        const char *name = NULL;
        if (b->seen & BOULDER_MATERIAL) { sd_err(p, "duplicate key", key); return; }
        b->seen |= BOULDER_MATERIAL;
        if (sd_val_name(p, t, n, 2, &name) < 0) return;
        if (n != 3) { sd_err(p, "unexpected token after value", t[3].text); return; }
        snprintf(p->boulder_mat_name, sizeof p->boulder_mat_name, "%s", name);
        p->boulder_mat_line = p->line;
        return;
    }
    sd_warn_unknown_key(p, key);
}

static void sd_close_boulder(Parser *p, SceneDesc *d, const Block *b)
{
    if (!(b->seen & BOULDER_POSITION)) {
        sd_err_at(p, b->line, "missing required key", "position");
        return;
    }
    p->boulder.material_name = (b->seen & BOULDER_MATERIAL) ? p->boulder_mat_name : NULL;
    if (scene_desc_add_boulder(d, &p->boulder) < 0) {
        sd_err(p, "out of memory", b->kw);
        return;
    }
    if (sd_push_boulder_line(p, p->boulder_mat_line) != 0) {
        sd_err(p, "out of memory", b->kw);
        return;
    }
}

static void sd_light_begin(Parser *p)
{
    memset(&p->light, 0, sizeof p->light);
    p->light.color = vec3(1.0, 1.0, 1.0);
    p->light.intensity = 20.0;
    p->light.radius = 0.10;
}

static void sd_apply_light_key(Parser *p, Block *b, const Token *t, int n)
{
    const char *key = t[0].text;
    if (strcmp(key, "position") == 0 || strcmp(key, "center") == 0) {
        if (b->seen & LIGHT_POSITION) { sd_err(p, "duplicate key", key); return; }
        b->seen |= LIGHT_POSITION;
        (void)sd_val_vec3(p, t, n, 2, &p->light.position);
        return;
    }
    if (strcmp(key, "color") == 0 || strcmp(key, "radiance") == 0 || strcmp(key, "emissive") == 0) {
        if (b->seen & LIGHT_COLOR) { sd_err(p, "duplicate key", key); return; }
        b->seen |= LIGHT_COLOR;
        (void)sd_val_vec3(p, t, n, 2, &p->light.color);
        return;
    }
    if (strcmp(key, "intensity") == 0) {
        if (b->seen & LIGHT_INTENSITY) { sd_err(p, "duplicate key", key); return; }
        b->seen |= LIGHT_INTENSITY;
        (void)sd_val_double(p, t, n, 2, &p->light.intensity);
        return;
    }
    if (strcmp(key, "radius") == 0) {
        if (b->seen & LIGHT_RADIUS) { sd_err(p, "duplicate key", key); return; }
        b->seen |= LIGHT_RADIUS;
        (void)sd_val_double(p, t, n, 2, &p->light.radius);
        return;
    }
    sd_warn_unknown_key(p, key);
}

static void sd_close_light(Parser *p, SceneDesc *d, const Block *b)
{
    if (!(b->seen & LIGHT_POSITION)) {
        sd_err_at(p, b->line, "missing required key", "position");
        return;
    }
    if (scene_desc_add_light(d, &p->light) < 0) {
        sd_err(p, "out of memory", b->kw);
        return;
    }
}

/* Dispatch the close of one block to its type-specific finisher. */
static void sd_close_block(Parser *p, SceneDesc *d, const Block *b)
{
    switch (b->kind) {
    case BLK_MATERIAL: sd_close_material(p, d, b); break;
    case BLK_PRIM:     sd_close_prim(p, d, b);     break;
    case BLK_PLANT:    sd_close_plant(p, d, b);    break;
    case BLK_BOULDER:  sd_close_boulder(p, d, b);  break;
    case BLK_LIGHT:    sd_close_light(p, d, b);    break;
    default:           break; /* camera / sky need no close action */
    }
}

/* ------------------------------------------------------------------ */
/* Parser: statements                                                  */
/* ------------------------------------------------------------------ */

static void sd_begin_block(Block *b, BlockKind kind, int line, const char *kw)
{
    b->kind = kind;
    b->line = line;
    b->seen = 0;
    snprintf(b->kw, sizeof b->kw, "%s", kw);
}

static int sd_is_global_key(const char *k)
{
    return strcmp(k, "water_level") == 0 ||
           strcmp(k, "water_material") == 0 ||
           strcmp(k, "water_enabled") == 0;
}

static int sd_is_prim_keyword(const char *k)
{
    return strcmp(k, "sphere") == 0 || strcmp(k, "plane") == 0 ||
           strcmp(k, "box") == 0 || strcmp(k, "triangle") == 0 ||
           strcmp(k, "cylinder") == 0;
}

static int sd_is_csg_keyword(const char *k, CsgOp *out_op)
{
    if (strcmp(k, "csg_union") == 0) {
        if (out_op) *out_op = CSG_UNION;
        return 1;
    }
    if (strcmp(k, "csg_intersection") == 0) {
        if (out_op) *out_op = CSG_INTERSECTION;
        return 1;
    }
    if (strcmp(k, "csg_difference") == 0) {
        if (out_op) *out_op = CSG_DIFFERENCE;
        return 1;
    }
    return 0;
}

/* Top-level `water_* = value` statement (§4.11). */
static void sd_handle_global(Parser *p, SceneDesc *d, const Token *t, int n)
{
    const char *key = t[0].text;

    if (n < 2 || t[1].kind != TOK_EQUAL) {
        sd_err(p, "missing '='", key);
        return;
    }
    if (strcmp(key, "water_level") == 0) {
        if (sd_val_double(p, t, n, 2, &d->water_level) < 0)
            return;
        if (n != 3)
            sd_err(p, "unexpected token after value", t[3].text);
    } else if (strcmp(key, "water_material") == 0) {
        const char *name = NULL;
        if (sd_val_name(p, t, n, 2, &name) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        /* Deferred: the material table may not be complete yet, so only
         * record the name (or the explicit `none`) here; the resolution pass
         * maps it to a table index once every block has been read (§4.11). */
        if (strcmp(name, "none") == 0) {
            p->water_ref = 1;
        } else {
            p->water_ref = 2;
            snprintf(p->water_name, sizeof p->water_name, "%s", name);
        }
        p->water_line = p->line;
    } else { /* water_enabled */
        int flag = 0;
        if (sd_val_int(p, t, n, 2, &flag) < 0)
            return;
        if (n != 3) {
            sd_err(p, "unexpected token after value", t[3].text);
            return;
        }
        if (flag != 0 && flag != 1) {
            sd_err(p, "expected 0 or 1", t[2].text);
            return;
        }
        d->water_enabled = flag;
        p->water_enabled_set = 1;
    }
}

/* `camera {` / `sky {` / `material <name> {` / primitive / plant headers. */
static void sd_open_block(Parser *p, SceneDesc *d, const Token *t, int n, Block *b)
{
    const char *kw = t[0].text;

    if (strcmp(kw, "material") == 0) {
        if (n < 3 || (t[1].kind != TOK_IDENT && t[1].kind != TOK_STRING) ||
            t[2].kind != TOK_LBRACE) {
            sd_err(p, "malformed block header, expected `material <name> {`", kw);
            return;
        }
        if (n != 3) {
            sd_err(p, "unexpected token after '{'", t[3].text);
            return;
        }
        sd_mat_begin(p, t[1].text);
        sd_begin_block(b, BLK_MATERIAL, p->line, kw);
        return;
    }
    if (strcmp(kw, "tree") == 0 || strcmp(kw, "bush") == 0 ||
        strcmp(kw, "conifer") == 0 || strcmp(kw, "spruce") == 0 || strcmp(kw, "pine") == 0) {
        if (n != 2 || t[1].kind != TOK_LBRACE) {
            sd_err(p, "malformed block header, expected `keyword {`", kw);
            return;
        }
        sd_plant_begin(p);
        sd_begin_block(b, BLK_PLANT, p->line, kw);
        return;
    }
    if (strcmp(kw, "camera") == 0 || strcmp(kw, "sky") == 0 ||
        strcmp(kw, "fog") == 0 || sd_is_prim_keyword(kw)) {
        if (n != 2 || t[1].kind != TOK_LBRACE) {
            sd_err(p, "malformed block header, expected `keyword {`", kw);
            return;
        }
        if (strcmp(kw, "camera") == 0) {
            d->camera.present = 1;
            sd_begin_block(b, BLK_CAMERA, p->line, kw);
        } else if (strcmp(kw, "sky") == 0) {
            d->has_sky = 1;
            sd_begin_block(b, BLK_SKY, p->line, kw);
        } else if (strcmp(kw, "fog") == 0) {
            d->has_fog = 1;
            sd_begin_block(b, BLK_FOG, p->line, kw);
        } else {
            sd_prim_begin(p);
            sd_begin_block(b, BLK_PRIM, p->line, kw);
        }
        return;
    }
    if (strcmp(kw, "boulder") == 0 || strcmp(kw, "rock") == 0) {
        if (n != 2 || t[1].kind != TOK_LBRACE) {
            sd_err(p, "malformed block header, expected `keyword {`", kw);
            return;
        }
        sd_boulder_begin(p);
        sd_begin_block(b, BLK_BOULDER, p->line, kw);
        return;
    }
    if (strcmp(kw, "light") == 0 || strcmp(kw, "point_light") == 0) {
        if (n != 2 || t[1].kind != TOK_LBRACE) {
            sd_err(p, "malformed block header, expected `keyword {`", kw);
            return;
        }
        sd_light_begin(p);
        sd_begin_block(b, BLK_LIGHT, p->line, kw);
        return;
    }
    CsgOp csg_op;
    if (sd_is_csg_keyword(kw, &csg_op)) {
        if (n != 2 || t[1].kind != TOK_LBRACE) {
            sd_err(p, "malformed block header, expected `keyword {`", kw);
            return;
        }
        sd_begin_block(b, BLK_CSG, p->line, kw);
        b->csg_op = csg_op;
        b->csg_mat_name[0] = '\0';
        b->csg_mat_line = 0;
        b->csg_child_count = 0;
        return;
    }
    sd_err(p, "unknown block keyword", kw);
}

static void sd_handle_top(Parser *p, SceneDesc *d, const Token *t, int n, Block *b)
{
    if (t[0].kind != TOK_IDENT) {
        sd_err(p, "unexpected token", t[0].text);
        return;
    }
    if (sd_is_global_key(t[0].text)) {
        sd_handle_global(p, d, t, n);
        return;
    }
    if (n >= 2 && t[1].kind == TOK_EQUAL) {
        sd_err(p, "key outside a block", t[0].text);
        return;
    }
    sd_open_block(p, d, t, n, b);
}

static void sd_handle_body(Parser *p, SceneDesc *d, const Token *t, int n, Block *b)
{
    if (t[0].kind == TOK_LBRACE) {
        sd_err(p, "nested blocks are not allowed", "{");
        return;
    }
    if (t[0].kind != TOK_IDENT) {
        sd_err(p, "expected a key", t[0].text);
        return;
    }
    if (n < 2 || t[1].kind != TOK_EQUAL) {
        sd_err(p, "missing '='", t[0].text);
        return;
    }
    switch (b->kind) {
    case BLK_CAMERA:
        sd_apply_key(p, &d->camera, CAM_KEYS, SD_ARRAY_LEN(CAM_KEYS), b, t, n);
        break;
    case BLK_SKY:
        sd_apply_key(p, &d->sky, SKY_KEYS, SD_ARRAY_LEN(SKY_KEYS), b, t, n);
        break;
    case BLK_FOG:
        sd_apply_key(p, &d->fog, FOG_KEYS, SD_ARRAY_LEN(FOG_KEYS), b, t, n);
        break;
    case BLK_MATERIAL:
        sd_apply_material_key(p, b, t, n);
        break;
    case BLK_PRIM:
        {
            const PrimSpec *sp = sd_prim_spec(b->kw);
            if (sp == NULL)
                sd_err(p, "internal error: unknown primitive keyword", b->kw);
            else
                sd_apply_prim_key(p, sp, b, t, n);
        }
        break;
    case BLK_PLANT:
        sd_apply_plant_key(p, b, t, n);
        break;
    case BLK_BOULDER:
        sd_apply_boulder_key(p, b, t, n);
        break;
    case BLK_LIGHT:
        sd_apply_light_key(p, b, t, n);
        break;
    case BLK_CSG:
        sd_apply_csg_key(p, b, t, n);
        break;
    default:
        sd_err(p, "key outside a block", t[0].text);
        break;
    }
}

#define SD_MAX_BLOCK_DEPTH 16

static void sd_open_csg_child(Parser *p, const Token *t, int n, Block *stack, int *depth)
{
    const char *kw = t[0].text;
    CsgOp child_op;

    if (n != 2 || t[1].kind != TOK_LBRACE) {
        sd_err(p, "malformed block header, expected `keyword {`", kw);
        return;
    }

    if (sd_is_csg_keyword(kw, &child_op)) {
        Block *child_b = &stack[*depth];
        sd_begin_block(child_b, BLK_CSG, p->line, kw);
        child_b->csg_op = child_op;
        child_b->csg_mat_name[0] = '\0';
        child_b->csg_mat_line = 0;
        child_b->csg_child_count = 0;
        (*depth)++;
    } else if (sd_is_prim_keyword(kw)) {
        sd_prim_begin(p);
        Block *child_b = &stack[*depth];
        sd_begin_block(child_b, BLK_PRIM, p->line, kw);
        (*depth)++;
    } else {
        sd_err(p, "expected primitive or CSG block inside CSG", kw);
    }
}

static void sd_handle_top_stack(Parser *p, SceneDesc *d, const Token *t, int n,
                                Block *stack, int *depth)
{
    if (t[0].kind != TOK_IDENT) {
        sd_err(p, "unexpected token", t[0].text);
        return;
    }
    if (sd_is_global_key(t[0].text)) {
        sd_handle_global(p, d, t, n);
        return;
    }
    if (n >= 2 && t[1].kind == TOK_EQUAL) {
        sd_err(p, "key outside a block", t[0].text);
        return;
    }
    Block *b = &stack[*depth];
    sd_open_block(p, d, t, n, b);
    if (b->kind != BLK_NONE) {
        (*depth)++;
    }
}

static void sd_handle_line_stack(Parser *p, SceneDesc *d, const Token *t, int n,
                                 Block *stack, int *depth)
{
    if (t[0].kind == TOK_RBRACE) {
        if (n > 1) {
            sd_err(p, "unexpected token after '}'", t[1].text);
            return;
        }
        if (*depth <= 0) {
            sd_err(p, "unexpected '}'", "}");
            return;
        }
        Block *top = &stack[*depth - 1];
        Block *parent = (*depth > 1) ? &stack[*depth - 2] : NULL;

        if (top->kind == BLK_CSG) {
            sd_close_csg(p, d, top, parent);
        } else if (top->kind == BLK_PRIM && parent != NULL && parent->kind == BLK_CSG) {
            sd_close_prim_csg_child(p, top, parent);
        } else {
            sd_close_block(p, d, top);
        }
        top->kind = BLK_NONE;
        (*depth)--;
        return;
    }

    if (*depth == 0) {
        sd_handle_top_stack(p, d, t, n, stack, depth);
    } else {
        Block *curr = &stack[*depth - 1];
        if (curr->kind == BLK_CSG) {
            CsgOp child_op;
            if (sd_is_prim_keyword(t[0].text) || sd_is_csg_keyword(t[0].text, &child_op)) {
                if (*depth >= SD_MAX_BLOCK_DEPTH) {
                    sd_err(p, "block nesting too deep", t[0].text);
                    return;
                }
                sd_open_csg_child(p, t, n, stack, depth);
                return;
            }
        }
        sd_handle_body(p, d, t, n, curr);
    }
}

/* ------------------------------------------------------------------ */
/* Parser: second (resolution) pass (§5.1 - §5.3, §4.11)               */
/* ------------------------------------------------------------------ */

/*
 * Resolve every primitive's `material_name` against the now-complete material
 * table. Runs after the whole file has been read, so forward references (a
 * primitive naming a material declared further down the file) work. An
 * unknown name is a hard error reported at the line of the reference (§5.3).
 */
static void sd_resolve_one_prim(Parser *p, SceneDesc *d, ScenePrimDesc *prim, int ref_line)
{
    if (prim->material_name != NULL) {
        int idx = sd_find_material(d, prim->material_name);
        if (idx < 0) {
            sd_err_at(p, ref_line,
                      "unknown material reference", prim->material_name);
            return;
        }
        prim->material_index = idx;
    }
    if (prim->left != NULL)
        sd_resolve_one_prim(p, d, prim->left, ref_line);
    if (prim->right != NULL)
        sd_resolve_one_prim(p, d, prim->right, ref_line);
}

static void sd_resolve_prims(Parser *p, SceneDesc *d)
{
    int i;

    for (i = 0; i < d->prim_count && !p->failed; i++) {
        sd_resolve_one_prim(p, d, &d->prims[i], p->ref_lines[i]);
    }
}

/*
 * Resolve the plant directives' optional material names to table indices
 * (§5.1, §5.3). A name that was never declared is a hard error reported at
 * the line of the reference. Plants without a `material_*` key keep the
 * NO_MATERIAL sentinel so the builder applies its built-in default.
 */
static void sd_resolve_plants(Parser *p, SceneDesc *d)
{
    int i;

    for (i = 0; i < d->plant_count && !p->failed; i++) {
        ScenePlantDesc *pl = &d->plants[i];
        int bark_line = p->plant_lines[2 * i];
        int leaf_line = p->plant_lines[2 * i + 1];

        if (pl->material_bark != NULL) {
            int idx = sd_find_material(d, pl->material_bark);
            if (idx < 0) {
                sd_err_at(p, bark_line, "unknown material reference",
                          pl->material_bark);
                return;
            }
            pl->material_bark_index = idx;
        }
        if (pl->material_leaf != NULL) {
            int idx = sd_find_material(d, pl->material_leaf);
            if (idx < 0) {
                sd_err_at(p, leaf_line, "unknown material reference",
                          pl->material_leaf);
                return;
            }
            pl->material_leaf_index = idx;
        }
    }
}

static void sd_resolve_boulders(Parser *p, SceneDesc *d)
{
    int i;

    for (i = 0; i < d->boulder_count && !p->failed; i++) {
        SceneBoulderDesc *bd = &d->boulders[i];
        if (bd->material_name != NULL) {
            int idx = sd_find_material(d, bd->material_name);
            if (idx < 0) {
                sd_err_at(p, p->boulder_lines[i], "unknown material reference",
                          bd->material_name);
                return;
            }
            bd->material_index = idx;
        } else {
            bd->material_index = SCENE_DESC_NO_MATERIAL;
        }
    }
}

/*
 * Resolve the `water_material` global to a table index and settle the
 * `water_enabled` flag (§4.11):
 *   - an explicit `water_material = <name>` must resolve (or be `none`);
 *   - otherwise the first material with is_water != 0 is used, else -1;
 *   - `water_enabled` defaults to "1 iff a water material exists", unless the
 *     file set it explicitly.
 */
static void sd_resolve_water(Parser *p, SceneDesc *d)
{
    int index = SCENE_DESC_NO_MATERIAL;

    if (p->water_ref == 2) {
        index = sd_find_material(d, p->water_name);
        if (index < 0) {
            sd_err_at(p, p->water_line, "unknown material reference",
                      p->water_name);
            return;
        }
    } else if (p->water_ref == 0) {
        index = sd_find_water_material(d);
    } /* water_ref == 1 (`none`) keeps the NO_MATERIAL sentinel */

    d->water_material = index;
    if (!p->water_enabled_set)
        d->water_enabled = (index >= 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Parser: file-level driver                                           */
/* ------------------------------------------------------------------ */

/*
 * Apply the spec defaults for every scalar a host may read before parsing,
 * so an omitted key or an absent block still yields a usable description
 * (§4.1, §4.2, §4.11). Arrays are untouched.
 */
static void sd_apply_defaults(SceneDesc *d)
{
    d->camera.present = 0;
    d->camera.eye = vec3(-18.0, 6.0, 22.0);
    d->camera.target = vec3(0.0, 5.0, -20.0);
    d->camera.up = vec3(0.0, 1.0, 0.0);
    d->camera.vfov_deg = 40.0;
    d->camera.aspect = 0.0; /* never in the file: host supplies width/height */
    d->camera.aperture = CAMERA_DEFAULT_APERTURE;
    d->camera.focus_distance = CAMERA_FOCUS_DISTANCE_DERIVED;

    sky_default_params(&d->sky);
    d->has_sky = 0;

    fog_default_params(&d->fog);
    d->has_fog = 0;

    d->water_level = SD_DEFAULT_WATER_LEVEL;
    d->water_material = SCENE_DESC_NO_MATERIAL;
    /* Provisional: sd_resolve_water() re-derives this from the material table
     * ("1 iff a water material exists") unless the file set it explicitly. */
    d->water_enabled = 0;
}

/* Walk the NUL-terminated buffer one physical line at a time. */
static void sd_parse_lines(Parser *p, SceneDesc *d, char *buf)
{
    Block stack[SD_MAX_BLOCK_DEPTH];
    int   depth = 0;
    Token toks[SD_MAX_TOKENS];
    char *line = buf;

    for (int i = 0; i < SD_MAX_BLOCK_DEPTH; ++i) {
        stack[i].kind = BLK_NONE;
        stack[i].line = 0;
        stack[i].seen = 0;
        stack[i].kw[0] = '\0';
        stack[i].csg_child_count = 0;
    }

    while (line != NULL && !p->failed) {
        char  *nl = strchr(line, '\n');
        char  *next = (nl != NULL) ? nl + 1 : NULL;
        size_t len;
        int    nt;

        if (nl != NULL)
            *nl = '\0';

        len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') { /* CRLF */
            line[len - 1] = '\0';
            len--;
        }

        p->line++;
        if (len >= SD_MAX_LINE) {
            sd_err(p, "line too long", "<line>");
            return;
        }

        nt = sd_lex_line(p, line, toks, SD_MAX_TOKENS);
        if (nt < 0)
            return;
        if (nt > 0)
            sd_handle_line_stack(p, d, toks, nt, stack, &depth);

        line = next;
    }

    if (!p->failed && depth > 0)
        sd_err_at(p, stack[depth - 1].line, "unterminated block", stack[depth - 1].kw);
}

/* Read the whole file into a dynamically grown, NUL-terminated buffer. */
static int sd_read_file(Parser *p, const char *path, char **out, size_t *outlen)
{
    FILE  *fp;
    char  *buf = NULL;
    size_t cap = 0;
    size_t len = 0;

    /* Reject a directory up front: fopen() on a directory can succeed on
     * some platforms and only fail later at fread() time. */
    {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            sd_err_at(p, 1, "cannot open: is a directory", NULL);
            return -1;
        }
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        char msg[256];
        snprintf(msg, sizeof msg, "cannot open: %s", strerror(errno));
        sd_err_at(p, 1, msg, NULL);
        return -1;
    }

    for (;;) {
        size_t want;
        size_t got;

        if (cap - len < 2) {
            size_t ncap = (cap == 0) ? 8192 : cap * 2;
            char  *nb = (char *)realloc(buf, ncap);
            if (nb == NULL) {
                fclose(fp);
                free(buf);
                sd_err_at(p, 1, "out of memory while reading file", NULL);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        want = cap - len - 1;
        got = fread(buf + len, 1, want, fp);
        len += got;
        if (got < want)
            break; /* EOF or error */
    }

    if (ferror(fp)) {
        char msg[256];
        snprintf(msg, sizeof msg, "read error: %s", strerror(errno));
        fclose(fp);
        free(buf);
        sd_err_at(p, 1, msg, NULL);
        return -1;
    }
    fclose(fp);

    if (buf == NULL) { /* empty file: still return a valid buffer */
        buf = (char *)malloc(1);
        if (buf == NULL) {
            sd_err_at(p, 1, "out of memory while reading file", NULL);
            return -1;
        }
        cap = 1;
    }
    buf[len] = '\0';
    *out = buf;
    *outlen = len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void scene_desc_init(SceneDesc *d)
{
    if (d == NULL)
        return;
    memset(d, 0, sizeof(*d));
    fog_default_params(&d->fog);
    d->water_material = SCENE_DESC_NO_MATERIAL;
}

void scene_desc_free(SceneDesc *d)
{
    int i;

    if (d == NULL)
        return;

    if (d->materials != NULL) {
        for (i = 0; i < d->material_count; i++)
            free(d->materials[i].name);
        free(d->materials);
    }

    if (d->prims != NULL) {
        for (i = 0; i < d->prim_count; i++)
            scene_prim_desc_destroy(&d->prims[i]);
        free(d->prims);
    }

    if (d->plants != NULL) {
        for (i = 0; i < d->plant_count; i++) {
            free(d->plants[i].material_bark);
            free(d->plants[i].material_leaf);
        }
        free(d->plants);
    }

    if (d->boulders != NULL) {
        for (i = 0; i < d->boulder_count; i++)
            free(d->boulders[i].material_name);
        free(d->boulders);
    }

    if (d->lights != NULL) {
        free(d->lights);
    }

    scene_desc_init(d);
}

/*
 * Shared parse pipeline: apply spec defaults, run the line parser over a
 * mutable NUL-terminated buffer, then resolve material names. `buf` is
 * borrowed (the caller frees it) and may be modified in place. `name` is used
 * verbatim in diagnostics. Returns 0 on success, non-zero on error (with a
 * message in `errbuf`); on error `*d` is left safe and freeable.
 */
static int sd_run_parser(SceneDesc *d, char *buf, size_t len,
                         const char *name, char *errbuf, size_t errlen)
{
    Parser p;

    sd_apply_defaults(d);

    p.path = name;
    p.errbuf = errbuf;
    p.errlen = errlen;
    p.line = 0;
    p.failed = 0;
    p.ref_lines = NULL;
    p.ref_count = 0;
    p.ref_cap = 0;
    p.water_ref = 0;
    p.water_name[0] = '\0';
    p.water_line = 0;
    p.water_enabled_set = 0;
    p.mat_type = 0;
    p.mat_name[0] = '\0';
    p.plant_lines = NULL;
    p.plant_line_count = 0;
    p.plant_line_cap = 0;
    p.boulder_lines = NULL;
    p.boulder_line_count = 0;
    p.boulder_line_cap = 0;
    sd_prim_begin(&p);
    sd_plant_begin(&p);
    sd_boulder_begin(&p);
    sd_light_begin(&p);

    if (len > 0) {
        char *start = buf;
        /* Skip a UTF-8 BOM silently (§2). */
        if (len >= 3 && (unsigned char)start[0] == 0xEF &&
            (unsigned char)start[1] == 0xBB && (unsigned char)start[2] == 0xBF)
            start += 3;
        sd_parse_lines(&p, d, start);
    }

    /* Second pass: the material table is complete, so names can be resolved. */
    if (!p.failed)
        sd_resolve_prims(&p, d);
    if (!p.failed)
        sd_resolve_plants(&p, d);
    if (!p.failed)
        sd_resolve_boulders(&p, d);
    if (!p.failed)
        sd_resolve_water(&p, d);

    free(p.ref_lines);
    free(p.plant_lines);
    free(p.boulder_lines);

    if (p.failed) {
        scene_desc_free(d); /* *d stays safe (and idempotently freeable) */
        return 1;
    }
    return 0;
}

int scene_desc_load(SceneDesc *d, const char *path, char *errbuf, size_t errlen)
{
    Parser p;
    char  *buf = NULL;
    size_t len = 0;
    int    rc;

    if (errbuf != NULL && errlen > 0)
        errbuf[0] = '\0';

    if (d == NULL || path == NULL) {
        sd_set_err(errbuf, errlen,
                   "<null>:0: error: null scene or path (near '<eof>')");
        return 1;
    }

    /* A throwaway Parser only supplies the diagnostic context for the reader. */
    memset(&p, 0, sizeof p);
    p.path = path;
    p.errbuf = errbuf;
    p.errlen = errlen;
    p.line = 0;

    if (sd_read_file(&p, path, &buf, &len) != 0) {
        scene_desc_free(d);
        return 1;
    }

    rc = sd_run_parser(d, buf, len, path, errbuf, errlen);
    free(buf);
    return rc;
}

/*
 * Parse a scene description held in memory (`text`, NUL-terminated) instead
 * of on disk. `name` is used verbatim in diagnostics (e.g. "<embedded>").
 * The buffer is copied, so `text` is never modified. Same return contract as
 * scene_desc_load().
 *
 * Live caller: src/main.c, on the no-`--scene` path, parses the compiled-in
 * DEFAULT_SCENE_TEXT (src/default_scene_text.h) with name "<embedded>". This
 * makes the built-in default use the identical parser path as a scene file,
 * so the program works standalone without a scenes/ directory.
 */
int scene_desc_load_string(SceneDesc *d, const char *text, const char *name,
                           char *errbuf, size_t errlen)
{
    char  *buf;
    size_t len;
    int    rc;

    if (errbuf != NULL && errlen > 0)
        errbuf[0] = '\0';

    if (d == NULL || text == NULL) {
        sd_set_err(errbuf, errlen,
                   "<null>:0: error: null scene or text (near '<eof>')");
        return 1;
    }

    len = strlen(text);
    buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        sd_set_err(errbuf, errlen,
                   "<embedded>:0: error: out of memory (near '<eof>')");
        return 1;
    }
    memcpy(buf, text, len + 1);

    rc = sd_run_parser(d, buf, len, (name != NULL) ? name : "<memory>",
                       errbuf, errlen);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Construction helpers                                                */
/* ------------------------------------------------------------------ */

int scene_desc_add_material(SceneDesc *d, const char *name, const Material *mat)
{
    MaterialDesc *slot;

    if (d == NULL)
        return -1;
    if (sd_grow((void **)&d->materials, &d->material_capacity,
                d->material_count, sizeof(*d->materials)) != 0)
        return -1;

    slot = &d->materials[d->material_count];
    memset(slot, 0, sizeof(*slot));
    slot->name = sd_strdup(name);
    if (name != NULL && slot->name == NULL)
        return -1;                       /* keep count unchanged on failure */
    if (mat != NULL)
        slot->mat = *mat;

    return d->material_count++;
}

int scene_desc_add_prim(SceneDesc *d, const ScenePrimDesc *prim)
{
    ScenePrimDesc *slot;
    char          *name;

    if (d == NULL || prim == NULL)
        return -1;
    if (sd_grow((void **)&d->prims, &d->prim_capacity,
                d->prim_count, sizeof(*d->prims)) != 0)
        return -1;

    name = sd_strdup(prim->material_name);
    if (prim->material_name != NULL && name == NULL)
        return -1;

    slot = &d->prims[d->prim_count];
    *slot = *prim;
    slot->material_name = name;
    slot->material_index = SCENE_DESC_NO_MATERIAL;
    slot->left = scene_prim_desc_clone(prim->left);
    slot->right = scene_prim_desc_clone(prim->right);

    return d->prim_count++;
}

int scene_desc_add_plant(SceneDesc *d, const ScenePlantDesc *plant)
{
    ScenePlantDesc *slot;
    char           *bark;
    char           *leaf;

    if (d == NULL || plant == NULL)
        return -1;
    if (sd_grow((void **)&d->plants, &d->plant_capacity,
                d->plant_count, sizeof(*d->plants)) != 0)
        return -1;

    bark = sd_strdup(plant->material_bark);
    leaf = sd_strdup(plant->material_leaf);
    if ((plant->material_bark != NULL && bark == NULL) ||
        (plant->material_leaf != NULL && leaf == NULL)) {
        free(bark);
        free(leaf);
        return -1;
    }

    slot = &d->plants[d->plant_count];
    *slot = *plant;
    slot->material_bark = bark;
    slot->material_leaf = leaf;
    slot->material_bark_index = SCENE_DESC_NO_MATERIAL;
    slot->material_leaf_index = SCENE_DESC_NO_MATERIAL;

    return d->plant_count++;
}

int scene_desc_add_boulder(SceneDesc *d, const SceneBoulderDesc *boulder)
{
    SceneBoulderDesc *slot;
    char             *mat;

    if (d == NULL || boulder == NULL)
        return -1;
    if (sd_grow((void **)&d->boulders, &d->boulder_capacity,
                d->boulder_count, sizeof(*d->boulders)) != 0)
        return -1;

    mat = sd_strdup(boulder->material_name);
    if (boulder->material_name != NULL && mat == NULL)
        return -1;

    slot = &d->boulders[d->boulder_count];
    *slot = *boulder;
    slot->material_name = mat;
    slot->material_index = SCENE_DESC_NO_MATERIAL;

    return d->boulder_count++;
}

int scene_desc_add_light(SceneDesc *d, const SceneLightDesc *light)
{
    SceneLightDesc *slot;

    if (d == NULL || light == NULL)
        return -1;
    if (sd_grow((void **)&d->lights, &d->light_capacity,
                d->light_count, sizeof(*d->lights)) != 0)
        return -1;

    slot = &d->lights[d->light_count];
    *slot = *light;

    return d->light_count++;
}
