/*
 * tests/test_texture.c - Unit tests for the procedural texture module
 *                        (src/texture.{h,c}) and its scene-format plumbing.
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure.
 *
 * Coverage:
 *   1. texture = none leaves the albedo unchanged (bit-for-bit).
 *   2. checker and stripes produce DISTINCT values at a point where they must
 *      differ, and each pattern uses its two colours.
 *   3. Determinism: repeated calls (and a fresh call after other calls) return
 *      identical values - no hidden state, no rand()/time().
 *   4. Checker parity / stripes band geometry, including scale handling and a
 *      non-positive scale fallback.
 *   5. NULL material is handled without crashing.
 *   6. Parser round-trip: texture keys survive load -> write -> load with the
 *      exact same values, and the writer omits default (untextured) keys.
 *
 * C11, -Wall -Wextra clean. No rand(). All heap memory is freed.
 */

#include "texture.h"
#include "material.h"
#include "vec3.h"
#include "scene_desc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness                                                */
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

static Material make_material(int kind, double scale, Vec3 a, Vec3 b)
{
    Material m;

    memset(&m, 0, sizeof m);
    m.albedo          = vec3(1.0, 1.0, 1.0);
    m.specular        = vec3(0.5, 0.5, 0.5);
    m.shininess       = 16.0;
    m.ior             = 1.0;
    m.texture_kind    = kind;
    m.texture_scale   = scale;
    m.texture_color_a = a;
    m.texture_color_b = b;
    return m;
}

/* ------------------------------------------------------------------ */
/* Test 1: texture = none is a no-op                                   */
/* ------------------------------------------------------------------ */

static void test_none_is_identity(void)
{
    Material m = make_material(TEXTURE_NONE, 1.0,
                               vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0));
    m.albedo = vec3(0.3, 0.42, 0.16);

    Vec3 p = vec3(1.5, -2.25, 7.75);

    CHECK(vec3_eq(texture_albedo(&m, p), m.albedo),
          "texture=none returns albedo unchanged");
    CHECK(vec3_eq(texture_albedo(&m, vec3(0, 0, 0)), m.albedo),
          "texture=none unchanged at origin");
    CHECK(vec3_eq(texture_specular(&m, p), m.specular),
          "texture_specular returns specular unchanged");
}

/* ------------------------------------------------------------------ */
/* Test 2: checker vs stripes are distinct + two colours each          */
/* ------------------------------------------------------------------ */

static void test_checker_vs_stripes_distinct(void)
{
    /* Colors chosen so every pattern result is unambiguous. */
    Vec3 ca = vec3(1.0, 0.0, 0.0);
    Vec3 cb = vec3(0.0, 0.0, 1.0);
    Material checker = make_material(TEXTURE_CHECKER, 1.0, ca, cb);
    Material stripes = make_material(TEXTURE_STRIPES, 1.0, ca, cb);

    /*
     * Point (0.5, 0.25, 0.5) with scale 1:
     *   checker cell = floor(0.5,0.25,0.5) = (0,0,0) -> parity 0 -> color_b.
     *   stripes: sin(2*pi*0.25) = 1 -> wave 1.0 >= 0.5 -> color_a.
     * They must differ, proving the two kinds are genuinely different.
     */
    Vec3 p = vec3(0.5, 0.25, 0.5);
    Vec3 cval = texture_albedo(&checker, p);
    Vec3 sval = texture_albedo(&stripes, p);

    CHECK(vec3_eq(cval, cb), "checker at (0.5,0.25,0.5) picks color_b");
    CHECK(vec3_eq(sval, ca), "stripes at (0.5,0.25,0.5) picks color_a");
    CHECK(!vec3_eq(cval, sval), "checker and stripes differ at the same point");

    /* Both colours must be reachable in each pattern. */
    CHECK(vec3_eq(texture_albedo(&checker, vec3(1.5, 0.5, 0.5)), ca),
          "checker parity 1 picks color_a");
    CHECK(vec3_eq(texture_albedo(&stripes, vec3(0.5, 0.75, 0.5)), cb),
          "stripes opposite band picks color_b");
}

/* ------------------------------------------------------------------ */
/* Test 3: determinism across repeated and interleaved calls           */
/* ------------------------------------------------------------------ */

static void test_determinism(void)
{
    Material checker = make_material(TEXTURE_CHECKER, 0.5,
                                     vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 1.0));
    Material stripes = make_material(TEXTURE_STRIPES, 2.0,
                                     vec3(0.0, 0.0, 1.0), vec3(1.0, 0.5, 0.0));

    Vec3 p = vec3(3.125, -1.75, 0.5);
    Vec3 c1 = texture_albedo(&checker, p);
    Vec3 s1 = texture_albedo(&stripes, p);

    /* Interleave a bunch of unrelated calls, then re-sample. */
    {
        int i;
        for (i = 0; i < 100; ++i) {
            (void)texture_albedo(&checker, vec3((double)i, 0.5, 0.5));
            (void)texture_albedo(&stripes, vec3(0.0, (double)i, 0.0));
        }
    }

    CHECK(vec3_eq(texture_albedo(&checker, p), c1),
          "checker is deterministic across calls");
    CHECK(vec3_eq(texture_albedo(&stripes, p), s1),
          "stripes is deterministic across calls");

    /* A second, independently-built identical material agrees too. */
    {
        Material checker2 = make_material(TEXTURE_CHECKER, 0.5,
                                          vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 1.0));
        CHECK(vec3_eq(texture_albedo(&checker2, p), c1),
              "identical material yields identical sample");
    }
}

/* ------------------------------------------------------------------ */
/* Test 4: geometry - checker parity, stripes bands, scale fallback    */
/* ------------------------------------------------------------------ */

static void test_geometry_and_scale(void)
{
    Vec3 ca = vec3(1.0, 1.0, 1.0);
    Vec3 cb = vec3(0.0, 0.0, 0.0);
    Material checker = make_material(TEXTURE_CHECKER, 1.0, ca, cb);

    /* Adjacent cells along each axis flip parity. */
    CHECK(vec3_eq(texture_albedo(&checker, vec3(0.5, 0.5, 0.5)), cb),
          "checker cell (0,0,0) -> b");
    CHECK(vec3_eq(texture_albedo(&checker, vec3(1.5, 0.5, 0.5)), ca),
          "checker cell (1,0,0) -> a");
    CHECK(vec3_eq(texture_albedo(&checker, vec3(0.5, 1.5, 0.5)), ca),
          "checker cell (0,1,0) -> a");
    CHECK(vec3_eq(texture_albedo(&checker, vec3(0.5, 0.5, 1.5)), ca),
          "checker cell (0,0,1) -> a");
    CHECK(vec3_eq(texture_albedo(&checker, vec3(1.5, 1.5, 1.5)), ca),
          "checker cell (1,1,1) -> a (three odd => odd)");
    CHECK(vec3_eq(texture_albedo(&checker, vec3(1.5, 1.5, 0.5)), cb),
          "checker cell (1,1,0) -> b (two odd => even)");

    /* Negative coordinates: floor() rounds toward -inf. */
    CHECK(vec3_eq(texture_albedo(&checker, vec3(-0.5, 0.5, 0.5)), ca),
          "checker cell (-1,0,0) -> a");

    /* scale = 0.5 means a 0.5-unit cell: (0.5,...) is in cell 1. */
    {
        Material c2 = make_material(TEXTURE_CHECKER, 0.5, ca, cb);
        CHECK(vec3_eq(texture_albedo(&c2, vec3(0.5, 0.5, 0.5)), ca),
              "checker scale=0.5 doubles cell frequency");
    }

    /* Non-positive / zero scale falls back to 1.0 (no division by zero). */
    {
        Material c0 = make_material(TEXTURE_CHECKER, 0.0, ca, cb);
        CHECK(vec3_eq(texture_albedo(&c0, vec3(0.5, 0.5, 0.5)), cb),
              "checker scale=0 falls back to default 1.0");
        CHECK(vec3_eq(texture_albedo(&c0, vec3(1.5, 0.5, 0.5)), ca),
              "checker scale=0 fallback parity still flips");
    }

    /* Stripes: y = 0.25 -> sin = +1 -> color_a; y = 0.75 -> sin = -1 -> b. */
    {
        Material st = make_material(TEXTURE_STRIPES, 1.0, ca, cb);
        CHECK(vec3_eq(texture_albedo(&st, vec3(0.0, 0.25, 0.0)), ca),
              "stripes crest -> color_a");
        CHECK(vec3_eq(texture_albedo(&st, vec3(0.0, 0.75, 0.0)), cb),
              "stripes trough -> color_b");
        /* Stripes must not depend on x or z. */
        CHECK(vec3_eq(texture_albedo(&st, vec3(9.0, 0.25, -4.0)),
                      texture_albedo(&st, vec3(0.0, 0.25, 0.0))),
              "stripes independent of x and z");
    }

    /* Base albedo tints the pattern (multiplied in). */
    {
        Material tint = make_material(TEXTURE_CHECKER, 1.0,
                                      vec3(0.5, 0.5, 0.5), vec3(1.0, 1.0, 1.0));
        tint.albedo = vec3(0.5, 1.0, 0.25);
        /* cell (1,0,0) -> color_a = 0.5 grey * albedo. */
        CHECK(vec3_eq(texture_albedo(&tint, vec3(1.5, 0.5, 0.5)),
                      vec3(0.25, 0.5, 0.125)),
              "texture colour is multiplied by the base albedo");
    }
}

/* ------------------------------------------------------------------ */
/* Test 5: NULL material safety                                        */
/* ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    Vec3 z = vec3(0.0, 0.0, 0.0);
    CHECK(vec3_eq(texture_albedo(NULL, vec3(1, 2, 3)), z),
          "texture_albedo(NULL) -> black");
    CHECK(vec3_eq(texture_specular(NULL, vec3(1, 2, 3)), z),
          "texture_specular(NULL) -> black");

    /* material_texture_defaults(NULL) must not crash. */
    material_texture_defaults(NULL);
    CHECK(1, "material_texture_defaults(NULL) is safe");
}

/* ------------------------------------------------------------------ */
/* Test 6: parser / writer round-trip of the texture keys              */
/* ------------------------------------------------------------------ */

static const char *TEX_SCENE =
    "# texture round-trip\n"
    "water_level = 0.02\n"
    "water_material = none\n"
    "water_enabled = 0\n"
    "\n"
    "material checkered {\n"
    "    albedo = 0.8 0.8 0.8\n"
    "    specular = 0.1 0.1 0.1\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "    texture = checker\n"
    "    texture_scale = 2.5\n"
    "    texture_color_a = 0.9 0.1 0.1\n"
    "    texture_color_b = 0.1 0.1 0.9\n"
    "}\n"
    "\n"
    "material plain {\n"
    "    type = opaque\n"
    "}\n"
    "\n"
    "sphere {\n"
    "    center = 0 1 0\n"
    "    radius = 1\n"
    "    material = checkered\n"
    "}\n";

static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long sz;
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

static void test_parser_roundtrip(void)
{
    const char *tmp_in  = "/tmp/rt_test_texture_in.scene";
    const char *tmp_out = "/tmp/rt_test_texture_out.scene";
    SceneDesc d1, d2;
    char errbuf[256];
    char *text;

    scene_desc_init(&d1);
    scene_desc_init(&d2);

    /* --- parse --- */
    CHECK(scene_desc_load_string(&d1, TEX_SCENE, "<test>", errbuf, sizeof errbuf) == 0,
          "parse texture scene succeeds");
    CHECK(d1.material_count == 2, "two materials parsed");

    if (d1.material_count == 2) {
        const Material *ck = &d1.materials[0].mat;
        const Material *pl = &d1.materials[1].mat;

        CHECK(d1.materials[0].name != NULL &&
              strcmp(d1.materials[0].name, "checkered") == 0,
              "first material name is 'checkered'");
        CHECK(ck->texture_kind == TEXTURE_CHECKER, "texture kind = checker");
        CHECK(ck->texture_scale == 2.5, "texture scale = 2.5");
        CHECK(vec3_eq(ck->texture_color_a, vec3(0.9, 0.1, 0.1)),
              "texture_color_a parsed");
        CHECK(vec3_eq(ck->texture_color_b, vec3(0.1, 0.1, 0.9)),
              "texture_color_b parsed");

        /* `type = opaque` preset must keep the default (none) texture. */
        CHECK(pl->texture_kind == TEXTURE_NONE, "preset material texture=none");
        CHECK(pl->texture_scale == 1.0, "preset material texture_scale=1");
        CHECK(vec3_eq(pl->texture_color_a, vec3(1.0, 1.0, 1.0)),
              "preset material default color_a = white");
        CHECK(vec3_eq(pl->texture_color_b, vec3(0.0, 0.0, 0.0)),
              "preset material default color_b = black");
    }

    /* --- write (canonical) --- */
    CHECK(scene_desc_write(&d1, tmp_out, errbuf, sizeof errbuf) == 0,
          "write texture scene succeeds");

    text = slurp(tmp_out);
    CHECK(text != NULL, "read back written scene");
    if (text != NULL) {
        CHECK(strstr(text, "texture = checker") != NULL,
              "writer emits texture = checker");
        CHECK(strstr(text, "texture_scale = 2.5") != NULL,
              "writer emits texture_scale = 2.5");
        CHECK(strstr(text, "texture_color_a = 0.9 0.1 0.1") != NULL,
              "writer emits texture_color_a");
        CHECK(strstr(text, "texture_color_b = 0.1 0.1 0.9") != NULL,
              "writer emits texture_color_b");
        /*
         * The `plain` material is `type = opaque` with default texture, so it
         * must be emitted as the preset with NO texture keys at all.
         */
        {
            const char *plain = strstr(text, "material plain");
            CHECK(plain != NULL, "writer emits material plain");
            if (plain != NULL) {
                const char *next = strstr(plain, "}\n");
                /* The block body must contain only `type = opaque`. */
                CHECK(next != NULL, "plain block terminated");
                if (next != NULL) {
                    const char *tkey = strstr(plain, "texture");
                    CHECK(tkey == NULL || tkey > next,
                          "untextured preset emits no texture keys");
                }
            }
        }
        free(text);
    }

    /* --- re-parse the written form: values must be preserved --- */
    CHECK(scene_desc_load(&d2, tmp_out, errbuf, sizeof errbuf) == 0,
          "re-parse written scene succeeds");
    CHECK(d2.material_count == d1.material_count, "material count preserved");
    if (d2.material_count == d1.material_count && d2.material_count >= 1) {
        const Material *a = &d1.materials[0].mat;
        const Material *b = &d2.materials[0].mat;
        CHECK(a->texture_kind == b->texture_kind, "round-trip texture kind");
        CHECK(a->texture_scale == b->texture_scale, "round-trip texture scale");
        CHECK(vec3_eq(a->texture_color_a, b->texture_color_a),
              "round-trip texture_color_a");
        CHECK(vec3_eq(a->texture_color_b, b->texture_color_b),
              "round-trip texture_color_b");
        CHECK(vec3_eq(a->albedo, b->albedo), "round-trip albedo");
    }

    scene_desc_free(&d1);
    scene_desc_free(&d2);
    remove(tmp_out);
    (void)tmp_in;
}

/* ------------------------------------------------------------------ */
/* Test 7: unknown texture kind is a hard error                        */
/* ------------------------------------------------------------------ */

static void test_bad_texture_kind(void)
{
    SceneDesc d;
    char errbuf[256];
    const char *bad =
        "material m {\n"
        "    type = opaque\n"
        "    texture = spiral\n"
        "}\n";

    scene_desc_init(&d);
    CHECK(scene_desc_load_string(&d, bad, "<bad>", errbuf, sizeof errbuf) != 0,
          "unknown texture kind is rejected");
    CHECK(strstr(errbuf, "unknown texture kind") != NULL,
          "error message names the texture kind");
    scene_desc_free(&d);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_none_is_identity();
    test_checker_vs_stripes_distinct();
    test_determinism();
    test_geometry_and_scale();
    test_null_safety();
    test_parser_roundtrip();
    test_bad_texture_kind();

    printf("test_texture: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
