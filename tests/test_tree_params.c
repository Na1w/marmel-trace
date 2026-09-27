/*
 * tests/test_tree_params.c - Unit tests for FILE-CONTROLLABLE tree/bush
 * generator parameters (docs/scene_format.md §4.9/§4.10).
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure.
 *
 * Coverage (all deterministic; no rand(), no clock, no rendering needed):
 *   1. The new keys parse into the ScenePlantDesc fields with the expected
 *      values, and a parse -> write -> parse round-trip is bit-for-bit stable.
 *   2. The canonical writer OMITS every generator key when it is absent (so
 *      the default scene stays byte-identical).
 *   3. Defaults reproduce the legacy behaviour: a plant with the keys OMITTED
 *      and an identical plant with every key explicitly set to the legacy
 *      SCENE_* default generate byte-identical geometry (same primitive count,
 *      same per-primitive fields, same bounds).
 *   4. Different parameter values actually change the generated geometry
 *      (primitive counts and/or bounds differ) for: max_depth, leaf_min,
 *      third_child_chance, spread_deg and taper.
 *   5. The built-in default scene still expands to exactly 1286 primitives
 *      (regression guard for the byte-identity invariant).
 *
 * C11, -Wall -Wextra clean. All heap memory is freed.
 */

#include "scene_desc.h"
#include "scene.h"
#include "geometry.h"
#include "vec3.h"

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

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf != NULL) {
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

/* Whole-geometry bounding box, accumulated from every primitive. */
static void geo_bounds(const Geometry *g, Vec3 *out_min, Vec3 *out_max)
{
    int i;

    out_min->x = out_min->y = out_min->z = 1e300;
    out_max->x = out_max->y = out_max->z = -1e300;
    for (i = 0; i < g->count; ++i) {
        Vec3 a, b;
        primitive_bounds(&g->prims[i], &a, &b);
        if (a.x < out_min->x) out_min->x = a.x;
        if (a.y < out_min->y) out_min->y = a.y;
        if (a.z < out_min->z) out_min->z = a.z;
        if (b.x > out_max->x) out_max->x = b.x;
        if (b.y > out_max->y) out_max->y = b.y;
        if (b.z > out_max->z) out_max->z = b.z;
    }
}

/* Two primitives are identical field-for-field (no padding compared). */
static int prim_eq(const Primitive *a, const Primitive *b)
{
    return a->kind == b->kind &&
           a->material_index == b->material_index &&
           a->center.x == b->center.x && a->center.y == b->center.y &&
           a->center.z == b->center.z &&
           a->axis.x == b->axis.x && a->axis.y == b->axis.y &&
           a->axis.z == b->axis.z &&
           a->half.x == b->half.x && a->half.y == b->half.y &&
           a->half.z == b->half.z &&
           a->a.x == b->a.x && a->a.y == b->a.y && a->a.z == b->a.z &&
           a->b.x == b->b.x && a->b.y == b->b.y && a->b.z == b->b.z &&
           a->c.x == b->c.x && a->c.y == b->c.y && a->c.z == b->c.z &&
           a->radius == b->radius && a->radius2 == b->radius2;
}

static int geo_eq(const Geometry *a, const Geometry *b)
{
    int i;

    if (a->count != b->count)
        return 0;
    for (i = 0; i < a->count; ++i) {
        if (!prim_eq(&a->prims[i], &b->prims[i]))
            return 0;
    }
    return 1;
}

/* Build a Scene from a scene-file text. Returns 0 on success. */
static int build_from_text(Scene *s, const char *text, char *errbuf, size_t n)
{
    SceneDesc d;

    scene_desc_init(&d);
    if (scene_desc_load_string(&d, text, "<test>", errbuf, n) != 0) {
        scene_desc_free(&d);
        return 1;
    }
    memset(s, 0, sizeof *s);
    if (scene_build_from_desc(s, &d) != 0) {
        scene_desc_free(&d);
        return 1;
    }
    scene_desc_free(&d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test scenes                                                         */
/* ------------------------------------------------------------------ */

/* A tree carrying every generator key, plus a bush carrying a few. */
static const char *PARAM_SCENE =
    "tree {\n"
    "    position = 1 0 2\n"
    "    height = 3.5\n"
    "    radius = 0.2\n"
    "    seed = 4242\n"
    "    max_depth = 3\n"
    "    min_branch_radius = 0.03\n"
    "    taper = 0.65\n"
    "    len_decay = 0.68\n"
    "    spread_deg = 41.5\n"
    "    perturb_deg = 9.5\n"
    "    up_bias = 0.2\n"
    "    third_child_chance = 0.4\n"
    "    leaf_min = 7\n"
    "    leaf_span = 3\n"
    "}\n"
    "bush {\n"
    "    position = -4 0 5\n"
    "    height = 0.8\n"
    "    radius = 0.05\n"
    "    seed = 99\n"
    "    max_depth = 2\n"
    "    leaf_min = 4\n"
    "}\n";

/* Same two plants, but with NO generator keys at all (legacy defaults). */
static const char *PLAIN_SCENE =
    "tree {\n"
    "    position = 1 0 2\n"
    "    height = 3.5\n"
    "    radius = 0.2\n"
    "    seed = 4242\n"
    "}\n"
    "bush {\n"
    "    position = -4 0 5\n"
    "    height = 0.8\n"
    "    radius = 0.05\n"
    "    seed = 99\n"
    "}\n";

/* The PLAIN_SCENE plants with every key explicitly set to the legacy default. */
static const char *DEFAULTS_SCENE =
    "tree {\n"
    "    position = 1 0 2\n"
    "    height = 3.5\n"
    "    radius = 0.2\n"
    "    seed = 4242\n"
    "    max_depth = 4\n"
    "    min_branch_radius = 0.02\n"
    "    taper = 0.7\n"
    "    len_decay = 0.72\n"
    "    spread_deg = 33\n"
    "    perturb_deg = 7\n"
    "    up_bias = 0.12\n"
    "    third_child_chance = 0.25\n"
    "    leaf_min = 15\n"
    "    leaf_span = 5\n"
    "}\n"
    "bush {\n"
    "    position = -4 0 5\n"
    "    height = 0.8\n"
    "    radius = 0.05\n"
    "    seed = 99\n"
    "    max_depth = 4\n"
    "    min_branch_radius = 0.02\n"
    "    taper = 0.7\n"
    "    len_decay = 0.72\n"
    "    spread_deg = 33\n"
    "    perturb_deg = 7\n"
    "    up_bias = 0.12\n"
    "    third_child_chance = 0.25\n"
    "    leaf_min = 15\n"
    "    leaf_span = 5\n"
    "}\n";

/* A single tree with a given max_depth / leaf_min, for the "differs" tests. */
static void make_single_tree(char *buf, size_t n,
                             const char *extra_key, const char *extra_val)
{
    snprintf(buf, n,
             "tree {\n"
             "    position = 0 0 0\n"
             "    height = 3.5\n"
             "    radius = 0.2\n"
             "    seed = 777\n"
             "%s%s%s"
             "}\n",
             (extra_key != NULL) ? "    " : "",
             (extra_key != NULL) ? extra_key : "",
             (extra_key != NULL) ? extra_val : "");
}

/* ------------------------------------------------------------------ */
/* Test 1: parse the new keys + round-trip                             */
/* ------------------------------------------------------------------ */

static void test_parse_and_roundtrip(void)
{
    const char *tmp_out = "/tmp/tree_params_out.scene";
    SceneDesc d1, d2;
    char errbuf[256];

    scene_desc_init(&d1);
    scene_desc_init(&d2);

    CHECK(scene_desc_load_string(&d1, PARAM_SCENE, "<test>",
                                 errbuf, sizeof errbuf) == 0,
          "parse scene with tree generator keys succeeds");
    CHECK(d1.plant_count == 2, "two plants parsed");

    if (d1.plant_count == 2) {
        const ScenePlantDesc *t = &d1.plants[0];
        const ScenePlantDesc *b = &d1.plants[1];

        CHECK(t->kind == SD_PLANT_TREE, "plant 0 is a tree");
        CHECK(t->has_max_depth && t->max_depth == 3, "tree max_depth = 3");
        CHECK(t->has_min_branch_radius && t->min_branch_radius == 0.03,
              "tree min_branch_radius = 0.03");
        CHECK(t->has_taper && t->taper == 0.65, "tree taper = 0.65");
        CHECK(t->has_len_decay && t->len_decay == 0.68, "tree len_decay = 0.68");
        CHECK(t->has_spread_deg && t->spread_deg == 41.5, "tree spread_deg = 41.5");
        CHECK(t->has_perturb_deg && t->perturb_deg == 9.5, "tree perturb_deg = 9.5");
        CHECK(t->has_up_bias && t->up_bias == 0.2, "tree up_bias = 0.2");
        CHECK(t->has_third_child_chance && t->third_child_chance == 0.4,
              "tree third_child_chance = 0.4");
        CHECK(t->has_leaf_min && t->leaf_min == 7, "tree leaf_min = 7");
        CHECK(t->has_leaf_span && t->leaf_span == 3, "tree leaf_span = 3");

        CHECK(b->kind == SD_PLANT_BUSH, "plant 1 is a bush");
        CHECK(b->has_max_depth && b->max_depth == 2, "bush max_depth = 2");
        CHECK(b->has_leaf_min && b->leaf_min == 4, "bush leaf_min = 4");
        CHECK(b->has_taper == 0, "bush taper flag absent");
    }

    CHECK(scene_desc_write(&d1, tmp_out, errbuf, sizeof errbuf) == 0,
          "write scene with generator keys succeeds");

    char *text = slurp(tmp_out);
    CHECK(text != NULL, "read back written scene");
    if (text != NULL) {
        CHECK(strstr(text, "max_depth = 3") != NULL,
              "writer emits max_depth");
        CHECK(strstr(text, "min_branch_radius = 0.03") != NULL,
              "writer emits min_branch_radius");
        CHECK(strstr(text, "taper = 0.65") != NULL, "writer emits taper");
        CHECK(strstr(text, "len_decay = 0.68") != NULL, "writer emits len_decay");
        CHECK(strstr(text, "spread_deg = 41.5") != NULL, "writer emits spread_deg");
        CHECK(strstr(text, "perturb_deg = 9.5") != NULL, "writer emits perturb_deg");
        CHECK(strstr(text, "up_bias = 0.2") != NULL, "writer emits up_bias");
        CHECK(strstr(text, "third_child_chance = 0.4") != NULL,
              "writer emits third_child_chance");
        CHECK(strstr(text, "leaf_min = 7") != NULL, "writer emits leaf_min");
        CHECK(strstr(text, "leaf_span = 3") != NULL, "writer emits leaf_span");
        free(text);
    }

    CHECK(scene_desc_load(&d2, tmp_out, errbuf, sizeof errbuf) == 0,
          "re-parse written scene succeeds");
    CHECK(d2.plant_count == 2, "round-trip keeps two plants");
    if (d2.plant_count == 2 && d1.plant_count == 2) {
        const ScenePlantDesc *a = &d1.plants[0];
        const ScenePlantDesc *c = &d2.plants[0];
        CHECK(c->has_max_depth == a->has_max_depth && c->max_depth == a->max_depth,
              "max_depth round-trips bit-for-bit");
        CHECK(c->has_min_branch_radius == a->has_min_branch_radius &&
              c->min_branch_radius == a->min_branch_radius,
              "min_branch_radius round-trips bit-for-bit");
        CHECK(c->has_taper == a->has_taper && c->taper == a->taper,
              "taper round-trips bit-for-bit");
        CHECK(c->has_len_decay == a->has_len_decay && c->len_decay == a->len_decay,
              "len_decay round-trips bit-for-bit");
        CHECK(c->has_spread_deg == a->has_spread_deg &&
              c->spread_deg == a->spread_deg,
              "spread_deg round-trips bit-for-bit");
        CHECK(c->has_perturb_deg == a->has_perturb_deg &&
              c->perturb_deg == a->perturb_deg,
              "perturb_deg round-trips bit-for-bit");
        CHECK(c->has_up_bias == a->has_up_bias && c->up_bias == a->up_bias,
              "up_bias round-trips bit-for-bit");
        CHECK(c->has_third_child_chance == a->has_third_child_chance &&
              c->third_child_chance == a->third_child_chance,
              "third_child_chance round-trips bit-for-bit");
        CHECK(c->has_leaf_min == a->has_leaf_min && c->leaf_min == a->leaf_min,
              "leaf_min round-trips bit-for-bit");
        CHECK(c->has_leaf_span == a->has_leaf_span && c->leaf_span == a->leaf_span,
              "leaf_span round-trips bit-for-bit");
    }

    scene_desc_free(&d1);
    scene_desc_free(&d2);
}

/* ------------------------------------------------------------------ */
/* Test 2: the writer omits the keys when absent                       */
/* ------------------------------------------------------------------ */

static void test_writer_omits_defaults(void)
{
    const char *tmp_out = "/tmp/tree_params_default_out.scene";
    SceneDesc d;
    char errbuf[256];

    scene_desc_init(&d);
    CHECK(scene_desc_load_string(&d, PLAIN_SCENE, "<test>",
                                 errbuf, sizeof errbuf) == 0,
          "parse scene without generator keys succeeds");
    CHECK(d.plant_count == 2, "two plants parsed (plain)");
    if (d.plant_count == 2) {
        CHECK(d.plants[0].has_max_depth == 0, "absent max_depth -> flag 0");
        CHECK(d.plants[0].has_taper == 0, "absent taper -> flag 0");
        CHECK(d.plants[0].has_leaf_min == 0, "absent leaf_min -> flag 0");
    }

    CHECK(scene_desc_write(&d, tmp_out, errbuf, sizeof errbuf) == 0,
          "write plain scene succeeds");

    char *text = slurp(tmp_out);
    CHECK(text != NULL, "read back written plain scene");
    if (text != NULL) {
        CHECK(strstr(text, "max_depth") == NULL,
              "writer omits max_depth when absent");
        CHECK(strstr(text, "min_branch_radius") == NULL,
              "writer omits min_branch_radius when absent");
        CHECK(strstr(text, "taper") == NULL, "writer omits taper when absent");
        CHECK(strstr(text, "len_decay") == NULL,
              "writer omits len_decay when absent");
        CHECK(strstr(text, "spread_deg") == NULL,
              "writer omits spread_deg when absent");
        CHECK(strstr(text, "perturb_deg") == NULL,
              "writer omits perturb_deg when absent");
        CHECK(strstr(text, "up_bias") == NULL, "writer omits up_bias when absent");
        CHECK(strstr(text, "third_child_chance") == NULL,
              "writer omits third_child_chance when absent");
        CHECK(strstr(text, "leaf_min") == NULL,
              "writer omits leaf_min when absent");
        CHECK(strstr(text, "leaf_span") == NULL,
              "writer omits leaf_span when absent");
        free(text);
    }

    scene_desc_free(&d);
}

/* ------------------------------------------------------------------ */
/* Test 3: defaults reproduce the legacy behaviour exactly             */
/* ------------------------------------------------------------------ */

static void test_defaults_match_legacy(void)
{
    Scene s_plain, s_defaults;
    char errbuf[256];

    CHECK(build_from_text(&s_plain, PLAIN_SCENE, errbuf, sizeof errbuf) == 0,
          "build plain scene succeeds");
    CHECK(build_from_text(&s_defaults, DEFAULTS_SCENE, errbuf, sizeof errbuf) == 0,
          "build explicit-defaults scene succeeds");

    CHECK(s_plain.geo.count > 0, "plain scene generated primitives");
    CHECK(s_plain.geo.count == s_defaults.geo.count,
          "omitted keys and explicit defaults give the same primitive count");
    CHECK(geo_eq(&s_plain.geo, &s_defaults.geo),
          "omitted keys and explicit defaults give byte-identical geometry");

    scene_free(&s_plain);
    scene_free(&s_defaults);
}

/* ------------------------------------------------------------------ */
/* Test 4: parameters actually change the generated geometry           */
/* ------------------------------------------------------------------ */

static int count_for(const char *text)
{
    Scene s;
    char errbuf[256];
    int n;

    if (build_from_text(&s, text, errbuf, sizeof errbuf) != 0)
        return -1;
    n = s.geo.count;
    scene_free(&s);
    return n;
}

static void test_params_change_geometry(void)
{
    char a[512], b[512];
    Scene sa, sb;
    Vec3 amin, amax, bmin, bmax;
    char errbuf[256];
    int na, nb;

    /* max_depth: a shallow tree has strictly fewer segments than a deep one. */
    make_single_tree(a, sizeof a, "    max_depth = 1\n", "");
    make_single_tree(b, sizeof b, "    max_depth = 4\n", "");
    na = count_for(a);
    nb = count_for(b);
    CHECK(na > 0 && nb > 0, "depth scenes built");
    CHECK(na != nb, "max_depth changes the primitive count");
    CHECK(na < nb, "shallower max_depth yields fewer primitives");

    /* leaf_min: more leaf spheres per tip -> strictly more primitives. */
    make_single_tree(a, sizeof a, "    leaf_min = 2\n", "");
    make_single_tree(b, sizeof b, "    leaf_min = 40\n", "");
    na = count_for(a);
    nb = count_for(b);
    CHECK(na > 0 && nb > 0, "leaf scenes built");
    CHECK(nb > na, "larger leaf_min yields more primitives");

    /* third_child_chance: forcing 3-way forks (1.0) differs from never (0.0). */
    make_single_tree(a, sizeof a, "    third_child_chance = 0\n", "");
    make_single_tree(b, sizeof b, "    third_child_chance = 1\n", "");
    na = count_for(a);
    nb = count_for(b);
    CHECK(na > 0 && nb > 0, "branch-count scenes built");
    CHECK(na != nb, "third_child_chance changes the primitive count");

    /* spread_deg / taper: change the crown shape -> different bounds. */
    make_single_tree(a, sizeof a, "    spread_deg = 5\n", "");
    make_single_tree(b, sizeof b, "    spread_deg = 70\n", "");
    CHECK(build_from_text(&sa, a, errbuf, sizeof errbuf) == 0, "build narrow tree");
    CHECK(build_from_text(&sb, b, errbuf, sizeof errbuf) == 0, "build wide tree");
    geo_bounds(&sa.geo, &amin, &amax);
    geo_bounds(&sb.geo, &bmin, &bmax);
    CHECK(fabs(amax.x - bmax.x) > 1e-6 || fabs(amax.z - bmax.z) > 1e-6,
          "spread_deg changes the geometry bounds");
    scene_free(&sa);
    scene_free(&sb);

    make_single_tree(a, sizeof a, "    taper = 0.4\n", "");
    make_single_tree(b, sizeof b, "    taper = 0.95\n", "");
    CHECK(build_from_text(&sa, a, errbuf, sizeof errbuf) == 0, "build low-taper tree");
    CHECK(build_from_text(&sb, b, errbuf, sizeof errbuf) == 0, "build high-taper tree");
    CHECK(sa.geo.count != sb.geo.count,
          "taper changes the primitive count (depth via tip radius)");
    scene_free(&sa);
    scene_free(&sb);

    /* len_decay: shorter children -> different overall height. */
    make_single_tree(a, sizeof a, "    len_decay = 0.3\n", "");
    make_single_tree(b, sizeof b, "    len_decay = 0.9\n", "");
    CHECK(build_from_text(&sa, a, errbuf, sizeof errbuf) == 0, "build short-decay tree");
    CHECK(build_from_text(&sb, b, errbuf, sizeof errbuf) == 0, "build long-decay tree");
    geo_bounds(&sa.geo, &amin, &amax);
    geo_bounds(&sb.geo, &bmin, &bmax);
    CHECK(amax.y != bmax.y, "len_decay changes the tree height");
    scene_free(&sa);
    scene_free(&sb);
}

/* ------------------------------------------------------------------ */
/* Test 5: the built-in default scene is unchanged (regression)        */
/* ------------------------------------------------------------------ */

static void test_default_scene_primitive_count(void)
{
    SceneDesc desc;
    Scene scene;

    scene_desc_init(&desc);
    scene_default_desc(&desc);
    memset(&scene, 0, sizeof scene);
    CHECK(scene_build_from_desc(&scene, &desc) == 0, "build default scene");
    CHECK(scene.geo.count == 1286,
          "default scene still expands to exactly 1286 primitives");
    scene_free(&scene);
    scene_desc_free(&desc);
}

/* ------------------------------------------------------------------ */
/* Test 6: conifer, spruce, and polygonal foliage parsing/building    */
/* ------------------------------------------------------------------ */

static void test_conifer_and_foliage(void)
{
    const char *text =
        "material ground {\n"
        "    albedo = 0.5 0.5 0.5\n"
        "}\n"
        "material bark {\n"
        "    albedo = 0.3 0.2 0.1\n"
        "}\n"
        "material leaf {\n"
        "    albedo = 0.1 0.6 0.1\n"
        "}\n"
        "tree {\n"
        "    position = 0.0 0.0 0.0\n"
        "    height = 3.0\n"
        "    radius = 0.1\n"
        "    foliage = leaves\n"
        "}\n"
        "spruce {\n"
        "    position = 10.0 0.0 0.0\n"
        "    height = 4.0\n"
        "    radius = 0.15\n"
        "}\n";

    char errbuf[256];
    SceneDesc desc;
    scene_desc_init(&desc);
    CHECK(scene_desc_load_string(&desc, text, "<test>", errbuf, sizeof errbuf) == 0, "parse tree with leaves & spruce");
    CHECK(desc.plant_count == 2, "parsed 2 plants");
    CHECK(desc.plants[0].has_foliage == 1, "plant 0 has foliage");
    CHECK(desc.plants[0].foliage == PLANT_FOLIAGE_LEAVES, "plant 0 foliage is leaves");
    CHECK(desc.plants[1].has_plant_type == 1, "plant 1 has plant_type");
    CHECK(desc.plants[1].plant_type == PLANT_TYPE_CONIFER, "plant 1 is conifer");
    CHECK(desc.plants[1].has_foliage == 1, "plant 1 has foliage");
    CHECK(desc.plants[1].foliage == PLANT_FOLIAGE_NEEDLES, "plant 1 foliage is needles");

    Scene scene;
    memset(&scene, 0, sizeof scene);
    CHECK(scene_build_from_desc(&scene, &desc) == 0, "build scene with conifer and leaves");
    CHECK(scene.geo.count > 100, "scene contains generated geometry primitives");

    scene_free(&scene);
    scene_desc_free(&desc);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_parse_and_roundtrip();
    test_writer_omits_defaults();
    test_defaults_match_legacy();
    test_params_change_geometry();
    test_default_scene_primitive_count();
    test_conifer_and_foliage();

    fprintf(stderr, "test_tree_params: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail != 0) {
        fprintf(stderr, "TESTS FAILED\n");
        return 1;
    }
    printf("test_tree_params: all %d checks passed\n", g_pass);
    return 0;
}
