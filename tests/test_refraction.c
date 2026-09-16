/*
 * tests/test_refraction.c - Unit tests for the general refraction / glass
 *                           material support:
 *
 *   - `Material.beer_lambert`  (src/material.h)         - attenuation flag
 *   - `type = glass` preset + `beer_lambert` key        (src/scene_desc.c)
 *   - `mat_is_glass_preset()` + canonical emission      (src/scene_desc_write.c)
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure. C11, -Wall -Wextra clean, no rand().
 *
 * Expected `type = glass` preset (verified against sd_glass_preset()):
 *   albedo       vec3(0.02, 0.02, 0.02)
 *   specular     vec3(1.0, 1.0, 1.0)
 *   shininess    256.0
 *   reflectivity 0.0
 *   transparency 1.0
 *   ior          1.5
 *   is_water     0
 *   beer_lambert 0
 *   absorption   vec3(0, 0, 0)
 *   deep_color   vec3(0.5, 0.5, 0.5)
 * plus the default (untextured) texture fields.
 *
 * Coverage:
 *   1. `type = glass` yields the exact preset field values.
 *   2. An explicit `ior = 1.7` overrides only ior, keeping the rest.
 *   3. `beer_lambert = 1` sets the flag; `= 0` and default leave it 0.
 *   4. Regression: `type = water` still has is_water == 1, beer_lambert == 0.
 *   5. Writer: glass-preset -> exactly `type = glass` (no beer_lambert key);
 *      beer_lambert == 1 -> emitted; changed ior -> explicit keys, not glass.
 *   6. Round-trip: parse(write(m)) == m for all glass cases.
 */

#include "material.h"
#include "vec3.h"
#include "scene_desc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test harness (same style as the existing tests)                     */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int vec3_eq(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/*
 * Structural equality of every field the refraction feature touches, including
 * the texture fields the preset installs (untextured defaults).
 */
static int material_eq(const Material *a, const Material *b)
{
    return vec3_eq(a->albedo, b->albedo) &&
           vec3_eq(a->specular, b->specular) &&
           a->shininess    == b->shininess &&
           a->reflectivity == b->reflectivity &&
           a->transparency == b->transparency &&
           a->ior          == b->ior &&
           a->is_water     == b->is_water &&
           a->beer_lambert == b->beer_lambert &&
           vec3_eq(a->absorption, b->absorption) &&
           vec3_eq(a->deep_color, b->deep_color) &&
           a->texture_kind == b->texture_kind &&
           a->texture_scale == b->texture_scale &&
           vec3_eq(a->texture_color_a, b->texture_color_a) &&
           vec3_eq(a->texture_color_b, b->texture_color_b);
}

/* Read a whole file into a malloc'd, NUL-terminated buffer (or NULL). */
static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long  sz;
    char *buf;

    if (fp == NULL)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    buf = (char *)malloc((size_t)sz + 1u);
    if (buf == NULL) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    buf[sz] = '\0';
    return buf;
}

/* Pointer to the start of the body of the first `material <name>` block. */
static const char *material_body(const char *text, const char *name)
{
    char needle[128];
    const char *at;

    snprintf(needle, sizeof needle, "material %s {", name);
    at = strstr(text, needle);
    return (at == NULL) ? NULL : strchr(at, '\n');
}

/* 1 if `key` occurs in the block body before its closing brace. */
static int body_has_key(const char *body, const char *key)
{
    const char *close;

    if (body == NULL)
        return 0;
    close = strstr(body, "}\n");
    return strstr(body, key) != NULL &&
           (close == NULL || strstr(body, key) < close);
}

/*
 * Parse a scene declaring exactly one material and copy it into *out.
 * Returns 0 on success, non-zero on parse error / unexpected material count.
 */
static int parse_single_material(const char *text, Material *out,
                                 char *errbuf, size_t errlen)
{
    SceneDesc d;
    int ok = 1;

    scene_desc_init(&d);
    if (scene_desc_load_string(&d, text, "<test>", errbuf, errlen) != 0) {
        ok = 0;
    } else if (d.material_count != 1) {
        ok = 0;
    } else {
        *out = d.materials[0].mat;
    }
    scene_desc_free(&d);
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* Scene fixtures                                                      */
/* ------------------------------------------------------------------ */

static const char *GLASS_SCENE =
    "material glassy {\n"
    "    type = glass\n"
    "}\n";

static const char *GLASS_IOR_SCENE =
    "material glassy {\n"
    "    type = glass\n"
    "    ior = 1.7\n"
    "}\n";

static const char *GLASS_BL_SCENE =
    "material glassy {\n"
    "    type = glass\n"
    "    beer_lambert = 1\n"
    "    absorption = 0.3 0.15 0.05\n"
    "    deep_color = 0.1 0.2 0.3\n"
    "}\n";

static const char *GLASS_BL0_SCENE =
    "material glassy {\n"
    "    type = glass\n"
    "    beer_lambert = 0\n"
    "}\n";

static const char *PLAIN_BL_SCENE =
    "material glassy {\n"
    "    albedo = 0.2 0.3 0.4\n"
    "    beer_lambert = 1\n"
    "}\n";

static const char *WATER_SCENE =
    "material watery {\n"
    "    type = water\n"
    "}\n";

/* Build the exact glass preset in memory (mirrors sd_glass_preset()). */
static Material make_glass_preset(void)
{
    Material m;

    memset(&m, 0, sizeof m);
    m.albedo       = vec3(0.02, 0.02, 0.02);
    m.specular     = vec3(1.0, 1.0, 1.0);
    m.shininess    = 256.0;
    m.reflectivity = 0.0;
    m.transparency = 1.0;
    m.ior          = 1.5;
    m.is_water     = 0;
    m.beer_lambert = 0;
    m.absorption   = vec3(0.0, 0.0, 0.0);
    m.deep_color   = vec3(0.5, 0.5, 0.5);
    material_texture_defaults(&m);
    return m;
}

/* ------------------------------------------------------------------ */
/* Test 1: `type = glass` preset field values                          */
/* ------------------------------------------------------------------ */

static void test_glass_preset_values(void)
{
    Material g;
    Material ref;
    char errbuf[256];

    CHECK(parse_single_material(GLASS_SCENE, &g, errbuf, sizeof errbuf) == 0,
          "parse `type = glass` succeeds");

    CHECK(vec3_eq(g.albedo, vec3(0.02, 0.02, 0.02)), "glass albedo 0.02");
    CHECK(vec3_eq(g.specular, vec3(1.0, 1.0, 1.0)), "glass specular 1 1 1");
    CHECK(g.shininess == 256.0, "glass shininess 256");
    CHECK(g.reflectivity == 0.0, "glass reflectivity 0");
    CHECK(g.transparency == 1.0, "glass transparency 1");
    CHECK(g.ior == 1.5, "glass ior 1.5");
    CHECK(g.is_water == 0, "glass is_water 0");
    CHECK(g.beer_lambert == 0, "glass beer_lambert 0");
    CHECK(vec3_eq(g.absorption, vec3(0.0, 0.0, 0.0)), "glass absorption 0");
    CHECK(vec3_eq(g.deep_color, vec3(0.5, 0.5, 0.5)), "glass deep_color 0.5");
    /* The preset also installs the default (untextured) texture fields. */
    CHECK(g.texture_kind == TEXTURE_DEFAULT_KIND &&
          g.texture_scale == TEXTURE_DEFAULT_SCALE,
          "glass preset installs the default texture fields");

    /* And it equals the independently-built reference preset bit-for-bit. */
    ref = make_glass_preset();
    CHECK(material_eq(&g, &ref), "parsed glass equals the reference preset");
}

/* ------------------------------------------------------------------ */
/* Test 2: explicit `ior = 1.7` override keeps the other glass fields  */
/* ------------------------------------------------------------------ */

static void test_glass_ior_override(void)
{
    Material g, ref;
    char errbuf[256];

    CHECK(parse_single_material(GLASS_IOR_SCENE, &g, errbuf, sizeof errbuf) == 0,
          "parse `type = glass` + `ior = 1.7` succeeds");

    CHECK(g.ior == 1.7, "explicit ior overrides the preset (1.7)");
    CHECK(vec3_eq(g.albedo, vec3(0.02, 0.02, 0.02)), "ior override keeps albedo");
    CHECK(vec3_eq(g.specular, vec3(1.0, 1.0, 1.0)), "ior override keeps specular");
    CHECK(g.is_water == 0, "ior override keeps is_water 0");
    CHECK(g.beer_lambert == 0, "ior override keeps beer_lambert 0");

    /* Only the ior differs from the untouched preset. */
    ref = make_glass_preset();
    ref.ior = 1.7;
    CHECK(material_eq(&g, &ref), "overridden glass equals preset with ior 1.7");
}

/* ------------------------------------------------------------------ */
/* Test 3: `beer_lambert` flag parsing                                 */
/* ------------------------------------------------------------------ */

static void test_beer_lambert_flag(void)
{
    Material g;
    char errbuf[256];

    /* Explicit 1 sets the flag; nothing else about the glass changes. */
    CHECK(parse_single_material(GLASS_BL_SCENE, &g, errbuf, sizeof errbuf) == 0,
          "parse `type = glass` + `beer_lambert = 1` succeeds");
    CHECK(g.beer_lambert == 1, "beer_lambert = 1 sets the flag");
    CHECK(g.is_water == 0, "beer_lambert = 1 keeps is_water 0");

    /* Explicit 0 leaves it clear (same as the default). */
    CHECK(parse_single_material(GLASS_BL0_SCENE, &g, errbuf, sizeof errbuf) == 0,
          "parse `type = glass` + `beer_lambert = 0` succeeds");
    CHECK(g.beer_lambert == 0, "beer_lambert = 0 leaves the flag clear");

    /* Default (key absent) also leaves it clear. */
    CHECK(parse_single_material(GLASS_SCENE, &g, errbuf, sizeof errbuf) == 0,
          "parse `type = glass` without beer_lambert succeeds");
    CHECK(g.beer_lambert == 0, "absent beer_lambert defaults to 0");

    /* A non-preset material can opt into attenuation too. */
    CHECK(parse_single_material(PLAIN_BL_SCENE, &g, errbuf, sizeof errbuf) == 0,
          "parse non-preset material with beer_lambert = 1 succeeds");
    CHECK(g.beer_lambert == 1, "beer_lambert works without a `type` preset");
    CHECK(vec3_eq(g.albedo, vec3(0.2, 0.3, 0.4)),
          "non-preset beer_lambert keeps explicit albedo");
}

/* ------------------------------------------------------------------ */
/* Test 4: regression - `type = water` unchanged                       */
/* ------------------------------------------------------------------ */

static void test_water_regression(void)
{
    Material w;
    char errbuf[256];

    CHECK(parse_single_material(WATER_SCENE, &w, errbuf, sizeof errbuf) == 0,
          "parse `type = water` succeeds");
    CHECK(w.is_water == 1, "water preset keeps is_water 1");
    CHECK(w.beer_lambert == 0, "water preset keeps beer_lambert 0 (regression)");
    CHECK(w.ior == 1.33, "water preset keeps ior 1.33");
    CHECK(vec3_eq(w.albedo, vec3(0.05, 0.15, 0.20)), "water preset keeps albedo");
    CHECK(vec3_eq(w.absorption, vec3(0.45, 0.12, 0.06)),
          "water preset keeps absorption");
}

/* ------------------------------------------------------------------ */
/* Test 5: writer emission of glass materials                          */
/* ------------------------------------------------------------------ */

static void test_writer_glass_preset(void)
{
    const char *tmp_out = "/tmp/rt_test_refraction_preset.scene";
    SceneDesc d;
    char errbuf[256];
    char *text;

    scene_desc_init(&d);
    CHECK(scene_desc_load_string(&d, GLASS_SCENE, "<test>",
                                 errbuf, sizeof errbuf) == 0,
          "parse glass-preset scene succeeds");
    CHECK(scene_desc_write(&d, tmp_out, errbuf, sizeof errbuf) == 0,
          "write glass-preset scene succeeds");

    text = slurp(tmp_out);
    CHECK(text != NULL, "read back written glass-preset scene");
    if (text != NULL) {
        const char *body = material_body(text, "glassy");

        CHECK(strstr(text, "type = glass") != NULL,
              "glass preset emitted as `type = glass`");
        /* The block body must carry NO explicit keys and, crucially, no
         * `beer_lambert` key. */
        CHECK(body_has_key(body, "beer_lambert") == 0,
              "glass preset emits NO beer_lambert key");
        CHECK(body_has_key(body, "albedo") == 0 &&
              body_has_key(body, "ior") == 0,
              "glass preset emits no explicit albedo/ior keys");
        free(text);
    }

    scene_desc_free(&d);
    remove(tmp_out);
}

static void test_writer_beer_lambert(void)
{
    const char *tmp_out = "/tmp/rt_test_refraction_bl.scene";
    SceneDesc d;
    char errbuf[256];
    char *text;

    scene_desc_init(&d);
    CHECK(scene_desc_load_string(&d, GLASS_BL_SCENE, "<test>",
                                 errbuf, sizeof errbuf) == 0,
          "parse glass + beer_lambert scene succeeds");
    CHECK(scene_desc_write(&d, tmp_out, errbuf, sizeof errbuf) == 0,
          "write glass + beer_lambert scene succeeds");

    text = slurp(tmp_out);
    CHECK(text != NULL, "read back written beer_lambert scene");
    if (text != NULL) {
        /* A non-zero flag is NOT the glass preset, so explicit keys are used. */
        CHECK(strstr(text, "type = glass") == NULL,
              "flagged glass is not emitted as `type = glass`");
        CHECK(strstr(text, "beer_lambert = 1") != NULL,
              "writer emits `beer_lambert = 1` when set");
        CHECK(strstr(text, "ior = 1.5") != NULL,
              "flagged glass emits explicit ior");
        free(text);
    }

    scene_desc_free(&d);
    remove(tmp_out);
}

static void test_writer_ior_change_is_explicit(void)
{
    const char *tmp_out = "/tmp/rt_test_refraction_ior.scene";
    SceneDesc d;
    char errbuf[256];
    char *text;

    scene_desc_init(&d);
    CHECK(scene_desc_load_string(&d, GLASS_IOR_SCENE, "<test>",
                                 errbuf, sizeof errbuf) == 0,
          "parse glass + changed ior scene succeeds");
    CHECK(scene_desc_write(&d, tmp_out, errbuf, sizeof errbuf) == 0,
          "write glass + changed ior scene succeeds");

    text = slurp(tmp_out);
    CHECK(text != NULL, "read back written changed-ior scene");
    if (text != NULL) {
        CHECK(strstr(text, "type = glass") == NULL,
              "changed-ior glass is NOT emitted as `type = glass`");
        CHECK(strstr(text, "ior = 1.7") != NULL,
              "writer emits the changed ior explicitly");
        /* beer_lambert stays 0, so it must remain omitted. */
        CHECK(strstr(text, "beer_lambert") == NULL,
              "writer omits beer_lambert while it stays 0");
        free(text);
    }

    scene_desc_free(&d);
    remove(tmp_out);
}

/* ------------------------------------------------------------------ */
/* Test 6: round-trip stability  parse(write(m)) == m                  */
/* ------------------------------------------------------------------ */

/*
 * Write `scene` to `path`, re-parse it, and assert the single material is
 * structurally preserved (including beer_lambert and the texture fields).
 */
static void roundtrip_material(const char *scene, const char *path,
                               const char *label, char *errbuf, size_t errlen)
{
    SceneDesc d1, d2;

    scene_desc_init(&d1);
    scene_desc_init(&d2);

    CHECK(scene_desc_load_string(&d1, scene, "<test>", errbuf, errlen) == 0,
          label);
    CHECK(scene_desc_write(&d1, path, errbuf, errlen) == 0,
          "write for round-trip succeeds");
    CHECK(scene_desc_load(&d2, path, errbuf, errlen) == 0 &&
          d2.material_count == d1.material_count,
          "re-parse written scene succeeds with the same material count");
    if (d1.material_count == 1 && d2.material_count == 1) {
        CHECK(material_eq(&d1.materials[0].mat, &d2.materials[0].mat),
              "material (incl. beer_lambert) round-trips bit-for-bit");
    }

    scene_desc_free(&d1);
    scene_desc_free(&d2);
    remove(path);
}

static void test_roundtrip(void)
{
    char errbuf[256];

    roundtrip_material(GLASS_SCENE, "/tmp/rt_test_refraction_rt1.scene",
                       "round-trip: parse glass preset", errbuf, sizeof errbuf);
    roundtrip_material(GLASS_IOR_SCENE, "/tmp/rt_test_refraction_rt2.scene",
                       "round-trip: parse glass with ior 1.7",
                       errbuf, sizeof errbuf);
    roundtrip_material(GLASS_BL_SCENE, "/tmp/rt_test_refraction_rt3.scene",
                       "round-trip: parse glass with beer_lambert 1",
                       errbuf, sizeof errbuf);
    roundtrip_material(WATER_SCENE, "/tmp/rt_test_refraction_rt4.scene",
                       "round-trip: parse water preset", errbuf, sizeof errbuf);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_glass_preset_values();
    test_glass_ior_override();
    test_beer_lambert_flag();
    test_water_regression();
    test_writer_glass_preset();
    test_writer_beer_lambert();
    test_writer_ior_change_is_explicit();
    test_roundtrip();

    fprintf(stderr, "test_refraction: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
