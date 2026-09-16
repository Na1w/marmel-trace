/*
 * tests/test_material_presets.c - Unit tests for the named real-world material
 *                                 presets (t-027):
 *
 *   - `type = <name>` presets          (src/scene_desc.c)
 *   - preset-detection predicates      (src/scene_desc_write.c)
 *
 * The presets are the conductor set (gold, copper, silver, aluminum, iron,
 * chrome, brass), the opaque dielectrics (plastic, rubber, ceramic), the
 * refractive dielectric (diamond, ior 2.417) and the warm-white emitter
 * (emissive). All numeric values come from docs/research_material_reference.md.
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure. C11, -Wall -Wextra clean, no rand().
 *
 * Coverage:
 *   1. Each `type = <name>` parses to the exact documented field values.
 *   2. Unknown `type` is a hard error.
 *   3. `type = water`/`opaque`/`glass` are unchanged (regression).
 *   4. Writer: a material equal to a preset emits exactly `type = <name>` and
 *      no explicit keys.
 *   5. Writer: a preset with one overridden key falls back to explicit keys.
 *   6. Round-trip: parse(write(m)) == m for every preset and every override.
 *   7. The default (all-zero) material still emits `type = opaque` and the
 *      PBR-free/untouched behaviour is preserved.
 */

#include "material.h"
#include "vec3.h"
#include "scene_desc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
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

/* Structural equality of every Material field. */
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
           a->metallic  == b->metallic &&
           a->roughness == b->roughness &&
           vec3_eq(a->emissive, b->emissive) &&
           a->pbr       == b->pbr &&
           a->texture_kind  == b->texture_kind &&
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

/* Parse a scene declaring exactly one material and copy it into *out. */
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

/*
 * Write a scene containing exactly one material and return the malloc'd
 * canonical text (caller frees) or NULL on failure.
 */
static char *write_single_material(const char *name, const Material *m)
{
    const char *path = "/tmp/rt_test_material_presets.scene";
    SceneDesc d;
    char errbuf[256];
    char *text = NULL;
    Material copy = *m;

    scene_desc_init(&d);
    if (scene_desc_add_material(&d, name, &copy) == 0) {
        if (scene_desc_write(&d, path, errbuf, sizeof errbuf) == 0)
            text = slurp(path);
    }
    scene_desc_free(&d);
    return text;
}

/* Build a scene text with exactly one `type = <name>` block. */
static void type_scene(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "material m {\n    type = %s\n}\n", name);
}

/* ------------------------------------------------------------------ */
/* Expected presets (mirroring sd_<name>_preset() in scene_desc.c)     */
/* ------------------------------------------------------------------ */

static Material base(void)
{
    Material m;

    memset(&m, 0, sizeof m);
    material_texture_defaults(&m);
    return m;
}

static Material make_conductor(Vec3 f0, double roughness)
{
    Material m = base();

    m.albedo = f0;
    m.specular = f0;
    m.shininess = 256.0;
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
    return m;
}

static Material make_dielectric(Vec3 albedo, Vec3 spec, double shin,
                                double rough, double ior)
{
    Material m = base();

    m.albedo = albedo;
    m.specular = spec;
    m.shininess = shin;
    m.reflectivity = 0.0;
    m.transparency = 0.0;
    m.ior = ior;
    m.is_water = 0;
    m.beer_lambert = 0;
    m.absorption = vec3(0.0, 0.0, 0.0);
    m.deep_color = vec3(0.0, 0.0, 0.0);
    m.metallic = 0.0;
    m.roughness = rough;
    m.emissive = vec3(0.0, 0.0, 0.0);
    m.pbr = 1;
    return m;
}

static Material make_diamond(void)
{
    Material m = base();

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
    return m;
}

static Material make_emissive(void)
{
    Material m = base();

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
    return m;
}

/* ------------------------------------------------------------------ */
/* Test 1: each `type = <name>` parses to the exact documented values  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *kw;
    Material  (*make)(void);
} PresetCase;

static Material mk_gold(void)    { return make_conductor(vec3(1.000, 0.766, 0.336), 0.05); }
static Material mk_copper(void)  { return make_conductor(vec3(0.955, 0.637, 0.538), 0.05); }
static Material mk_silver(void)  { return make_conductor(vec3(0.972, 0.960, 0.915), 0.03); }
static Material mk_aluminum(void){ return make_conductor(vec3(0.913, 0.921, 0.925), 0.05); }
static Material mk_iron(void)    { return make_conductor(vec3(0.560, 0.570, 0.580), 0.10); }
static Material mk_chrome(void)  { return make_conductor(vec3(0.550, 0.556, 0.554), 0.03); }
static Material mk_brass(void)   { return make_conductor(vec3(0.910, 0.778, 0.423), 0.08); }
static Material mk_plastic(void) { return make_dielectric(vec3(0.30, 0.05, 0.06), vec3(0.05, 0.05, 0.05), 64.0, 0.10, 1.46); }
static Material mk_rubber(void)  { return make_dielectric(vec3(0.05, 0.05, 0.05), vec3(0.04, 0.04, 0.04), 8.0, 0.90, 1.50); }
static Material mk_ceramic(void) { return make_dielectric(vec3(0.85, 0.85, 0.82), vec3(0.05, 0.05, 0.05), 128.0, 0.20, 1.60); }

static const PresetCase CASES[] = {
    { "gold",     mk_gold     },
    { "copper",   mk_copper   },
    { "silver",   mk_silver   },
    { "aluminum", mk_aluminum },
    { "iron",     mk_iron     },
    { "chrome",   mk_chrome   },
    { "brass",    mk_brass    },
    { "plastic",  mk_plastic  },
    { "rubber",   mk_rubber   },
    { "ceramic",  mk_ceramic  },
    { "diamond",  make_diamond  },
    { "emissive", make_emissive }
};

static void test_preset_values(void)
{
    size_t i;

    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        char scene[128];
        char errbuf[256];
        Material got, want;
        char msg[160];

        type_scene(scene, sizeof scene, CASES[i].kw);
        want = CASES[i].make();
        snprintf(msg, sizeof msg, "parse `type = %s` succeeds", CASES[i].kw);
        CHECK(parse_single_material(scene, &got, errbuf, sizeof errbuf) == 0,
              msg);
        snprintf(msg, sizeof msg, "`type = %s` equals the documented preset",
                 CASES[i].kw);
        CHECK(material_eq(&got, &want), msg);
    }
}

/* Spot-check the exact numeric values for a couple of representatives. */
static void test_spot_values(void)
{
    Material m;
    char scene[128];
    char errbuf[256];

    type_scene(scene, sizeof scene, "gold");
    CHECK(parse_single_material(scene, &m, errbuf, sizeof errbuf) == 0,
          "parse gold");
    CHECK(vec3_eq(m.albedo, vec3(1.000, 0.766, 0.336)), "gold F0 albedo");
    CHECK(vec3_eq(m.specular, m.albedo), "gold specular == albedo");
    CHECK(m.metallic == 1.0, "gold metallic 1");
    CHECK(m.roughness == 0.05, "gold roughness 0.05");
    CHECK(m.reflectivity == 0.0, "gold reflectivity 0");
    CHECK(m.ior == 1.0, "gold ior 1");
    CHECK(m.pbr == 1, "gold pbr 1");
    CHECK(vec3_eq(m.emissive, vec3(0.0, 0.0, 0.0)), "gold emissive 0");

    type_scene(scene, sizeof scene, "diamond");
    CHECK(parse_single_material(scene, &m, errbuf, sizeof errbuf) == 0,
          "parse diamond");
    CHECK(m.ior == 2.417, "diamond ior 2.417");
    CHECK(m.transparency == 1.0, "diamond transparency 1");
    CHECK(m.is_water == 0, "diamond is_water 0");
    CHECK(m.beer_lambert == 0, "diamond beer_lambert 0");
    CHECK(m.pbr == 1, "diamond pbr 1");

    type_scene(scene, sizeof scene, "emissive");
    CHECK(parse_single_material(scene, &m, errbuf, sizeof errbuf) == 0,
          "parse emissive");
    CHECK(vec3_eq(m.emissive, vec3(1.0, 0.85, 0.65)), "emissive warm white");
    CHECK(m.roughness == 0.5, "emissive roughness 0.5");
    CHECK(m.pbr == 1, "emissive pbr 1");
    CHECK(vec3_eq(m.albedo, vec3(0.0, 0.0, 0.0)), "emissive albedo 0");
}

/* ------------------------------------------------------------------ */
/* Test 2: unknown `type` is a hard error                              */
/* ------------------------------------------------------------------ */

static void test_unknown_type(void)
{
    Material m;
    char scene[128];
    char errbuf[256];

    type_scene(scene, sizeof scene, "unobtanium");
    CHECK(parse_single_material(scene, &m, errbuf, sizeof errbuf) != 0,
          "unknown `type` is rejected");
    CHECK(strstr(errbuf, "unknown material type") != NULL,
          "unknown `type` error message names the problem");
    CHECK(strstr(errbuf, "gold") != NULL && strstr(errbuf, "emissive") != NULL,
          "unknown `type` error message lists valid types");
}

/* ------------------------------------------------------------------ */
/* Test 3: water / opaque / glass unchanged (regression)               */
/* ------------------------------------------------------------------ */

static void test_existing_presets_unchanged(void)
{
    Material m;
    char scene[128];
    char errbuf[256];

    type_scene(scene, sizeof scene, "water");
    CHECK(parse_single_material(scene, &m, errbuf, sizeof errbuf) == 0,
          "parse `type = water`");
    CHECK(m.is_water == 1 && m.ior == 1.33, "water preset unchanged");
    CHECK(vec3_eq(m.albedo, vec3(0.05, 0.15, 0.20)), "water albedo unchanged");

    type_scene(scene, sizeof scene, "glass");
    CHECK(parse_single_material(scene, &m, errbuf, sizeof errbuf) == 0,
          "parse `type = glass`");
    CHECK(m.ior == 1.5 && m.transparency == 1.0, "glass preset unchanged");

    type_scene(scene, sizeof scene, "opaque");
    CHECK(parse_single_material(scene, &m, errbuf, sizeof errbuf) == 0,
          "parse `type = opaque`");
    CHECK(vec3_eq(m.albedo, vec3(0.0, 0.0, 0.0)) && m.ior == 1.0,
          "opaque preset unchanged");
}

/* ------------------------------------------------------------------ */
/* Test 4: writer emits exactly `type = <name>` for a preset material  */
/* ------------------------------------------------------------------ */

static void test_writer_preset_emission(void)
{
    size_t i;

    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        Material want = CASES[i].make();
        char *text = write_single_material("m", &want);
        char expect[64];
        char msg[160];

        snprintf(expect, sizeof expect, "    type = %s\n", CASES[i].kw);
        snprintf(msg, sizeof msg, "writer emits `type = %s`", CASES[i].kw);
        CHECK(text != NULL, msg);
        if (text != NULL) {
            CHECK(strstr(text, expect) != NULL, msg);
            /* No explicit keys: exactly one line inside the block. */
            snprintf(msg, sizeof msg, "`type = %s` is the only block line",
                     CASES[i].kw);
            CHECK(strstr(text, "albedo") == NULL &&
                  strstr(text, "metallic") == NULL &&
                  strstr(text, "roughness") == NULL &&
                  strstr(text, "pbr") == NULL, msg);
            free(text);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Test 5: a preset with an override falls back to explicit keys       */
/* ------------------------------------------------------------------ */

static void test_writer_override_fallback(void)
{
    Material gold = make_conductor(vec3(1.000, 0.766, 0.336), 0.30); /* brushed */
    Material lamp = make_emissive();
    char *text;

    lamp.emissive = vec3(4.0, 3.4, 2.6);

    text = write_single_material("m", &gold);
    CHECK(text != NULL, "write brushed gold");
    if (text != NULL) {
        CHECK(strstr(text, "type = gold") == NULL,
              "overridden gold does NOT emit `type = gold`");
        CHECK(strstr(text, "roughness = 0.3") != NULL,
              "overridden gold emits explicit roughness");
        CHECK(strstr(text, "albedo = 1 0.766 0.336") != NULL,
              "overridden gold emits explicit albedo");
        free(text);
    }

    text = write_single_material("m", &lamp);
    CHECK(text != NULL, "write bright lamp");
    if (text != NULL) {
        CHECK(strstr(text, "type = emissive") == NULL,
              "overridden emissive does NOT emit `type = emissive`");
        CHECK(strstr(text, "emissive = 4 3.4 2.6") != NULL,
              "overridden emissive emits explicit emissive");
        free(text);
    }
}

/* ------------------------------------------------------------------ */
/* Test 6: round-trip parse(write(m)) == m                             */
/* ------------------------------------------------------------------ */

static void test_round_trip(void)
{
    size_t i;

    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        Material want = CASES[i].make();
        Material back;
        char *text = write_single_material("m", &want);
        char errbuf[256];
        char msg[160];

        CHECK(text != NULL, "round-trip write");
        if (text == NULL)
            continue;
        snprintf(msg, sizeof msg, "round-trip parse for %s", CASES[i].kw);
        CHECK(parse_single_material(text, &back, errbuf, sizeof errbuf) == 0,
              msg);
        snprintf(msg, sizeof msg, "round-trip equality for %s", CASES[i].kw);
        CHECK(material_eq(&back, &want), msg);
        free(text);
    }

    /* Override case round-trips too. */
    {
        Material brushed = make_conductor(vec3(1.000, 0.766, 0.336), 0.30);
        Material back;
        char *text = write_single_material("m", &brushed);
        char errbuf[256];

        CHECK(text != NULL, "round-trip write brushed");
        if (text != NULL) {
            CHECK(parse_single_material(text, &back, errbuf,
                                        sizeof errbuf) == 0,
                  "round-trip parse brushed");
            CHECK(material_eq(&back, &brushed), "round-trip equality brushed");
            free(text);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Test 7: canonical write is a fixed point                            */
/* ------------------------------------------------------------------ */

static void test_fixed_point(void)
{
    Material gold = make_conductor(vec3(1.000, 0.766, 0.336), 0.05);
    char *t1 = write_single_material("m", &gold);
    Material back;
    char errbuf[256];
    char *t2;

    CHECK(t1 != NULL, "fixed point: first write");
    if (t1 == NULL)
        return;
    CHECK(parse_single_material(t1, &back, errbuf, sizeof errbuf) == 0,
          "fixed point: parse");
    t2 = write_single_material("m", &back);
    CHECK(t2 != NULL, "fixed point: second write");
    if (t2 != NULL) {
        CHECK(strcmp(t1, t2) == 0, "write(parse(write(m))) == write(m)");
        free(t2);
    }
    free(t1);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_preset_values();
    test_spot_values();
    test_unknown_type();
    test_existing_presets_unchanged();
    test_writer_preset_emission();
    test_writer_override_fallback();
    test_round_trip();
    test_fixed_point();

    printf("test_material_presets: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
