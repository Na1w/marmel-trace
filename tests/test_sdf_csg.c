/*
 * tests/test_sdf_csg.c - Unit tests for SDF primitives, domain displacement, and CSG.
 */

#include "sdf.h"
#include "geometry.h"
#include "scene_desc.h"
#include "scene.h"
#include "bvh.h"
#include "vec3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

#define CHECK_CLOSE(a, b, eps, msg) do { \
    double diff = fabs((a) - (b)); \
    if (diff <= (eps)) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (got %g, expected %g, diff %g > %g at %s:%d)\n", \
                (msg), (double)(a), (double)(b), diff, (double)(eps), __FILE__, __LINE__); } \
} while (0)

static void test_sdf_analytical(void)
{
    SdfData box;
    memset(&box, 0, sizeof(box));
    box.shape_a = SDF_SHAPE_BOX;
    box.center_a = vec3(0.0, 0.0, 0.0);
    box.param1_a = vec3(1.0, 2.0, 1.0);

    /* Center is inside box: dist should be negative */
    CHECK(sdf_eval(&box, vec3(0.0, 0.0, 0.0)) < 0.0, "box center is negative distance");
    /* Exactly on surface */
    CHECK_CLOSE(sdf_eval(&box, vec3(1.0, 0.0, 0.0)), 0.0, 1e-6, "box x surface dist == 0");
    CHECK_CLOSE(sdf_eval(&box, vec3(0.0, 2.0, 0.0)), 0.0, 1e-6, "box y surface dist == 0");
    /* 1 unit outside in X */
    CHECK_CLOSE(sdf_eval(&box, vec3(2.0, 0.0, 0.0)), 1.0, 1e-6, "box outside dist == 1");

    /* Sphere */
    SdfData sph;
    memset(&sph, 0, sizeof(sph));
    sph.shape_a = SDF_SHAPE_SPHERE;
    sph.center_a = vec3(0.0, 1.0, 0.0);
    sph.param2_a = 2.0;

    CHECK_CLOSE(sdf_eval(&sph, vec3(0.0, 1.0, 0.0)), -2.0, 1e-6, "sphere center dist == -R");
    CHECK_CLOSE(sdf_eval(&sph, vec3(0.0, 3.0, 0.0)), 0.0, 1e-6, "sphere surface dist == 0");
    CHECK_CLOSE(sdf_eval(&sph, vec3(0.0, 4.0, 0.0)), 1.0, 1e-6, "sphere outside dist == 1");
}

static void test_sdf_csg_difference(void)
{
    /* Box [-1, 1]^3 minus Cylinder along Y radius 0.5 */
    SdfData csg;
    memset(&csg, 0, sizeof(csg));
    csg.op = SDF_OP_DIFFERENCE;
    csg.shape_a = SDF_SHAPE_BOX;
    csg.center_a = vec3(0.0, 0.0, 0.0);
    csg.param1_a = vec3(1.0, 1.0, 1.0);

    csg.shape_b = SDF_SHAPE_CYLINDER;
    csg.center_b = vec3(0.0, 0.0, 0.0);
    csg.param1_b = vec3(0.0, 2.0, 0.0); /* tall cylinder */
    csg.param2_b = 0.5;                /* radius 0.5 */

    /* Center of box (0,0,0) was carved out by cylinder, so distance should be POSITIVE outside solid */
    CHECK(sdf_eval(&csg, vec3(0.0, 0.0, 0.0)) > 0.0, "carved tunnel center is outside solid");
    /* Point at x=0.5 (inner wall of drilled tunnel) should be on the surface */
    CHECK_CLOSE(sdf_eval(&csg, vec3(0.5, 0.0, 0.0)), 0.0, 1e-5, "inner carved tunnel wall dist == 0");
    /* Point inside remaining box meat (e.g. x=0.8, y=0, z=0.8) should be inside (negative) */
    CHECK(sdf_eval(&csg, vec3(0.8, 0.0, 0.8)) < 0.0, "box corner meat is inside solid");
}

static void test_ray_intersection_and_bvh(void)
{
    Geometry g;
    geometry_init(&g);

    SdfData csg;
    memset(&csg, 0, sizeof(csg));
    csg.op = SDF_OP_DIFFERENCE;
    csg.shape_a = SDF_SHAPE_BOX;
    csg.center_a = vec3(0.0, 2.0, 0.0);
    csg.param1_a = vec3(1.0, 1.0, 1.0);

    csg.shape_b = SDF_SHAPE_CYLINDER;
    csg.center_b = vec3(0.0, 2.0, 0.0);
    csg.param1_b = vec3(0.0, 2.0, 0.0);
    csg.param2_b = 0.5;

    /* Add sine wobble displacement */
    csg.displace.kind = DISPLACE_SINE;
    csg.displace.direction = vec3(1.0, 0.0, 0.0);
    csg.displace.amplitude = 0.2;
    csg.displace.frequency = 1.0;

    int idx = geometry_add(&g, prim_sdf(csg, 3));
    CHECK(idx == 0, "prim_sdf added to geometry");

    Bvh *bvh = bvh_build(&g);
    CHECK(bvh != NULL, "BVH built on SDF shape");

    /* Fire ray at the outer wall of the carved box */
    Ray ray = { vec3(-5.0, 2.0, 0.8), vec3(1.0, 0.0, 0.0) };
    Hit hit;
    memset(&hit, 0, sizeof(hit));
    int hit_rc = bvh_intersect(bvh, &g, ray, 1e-4, 100.0, &hit);
    CHECK(hit_rc == 1, "ray hits warped CSG outer wall");
    CHECK(hit.material_index == 3, "hit material matches");
    CHECK(vec3_length(hit.normal) > 0.99 && vec3_length(hit.normal) < 1.01, "normal is unit length");
    CHECK(hit.front_face == 1, "front face hit");

    bvh_free(bvh);
    geometry_free(&g);
}

static void test_scene_parser_integration(void)
{
    const char *scene_txt =
        "material gold {\n"
        "    type = gold\n"
        "}\n"
        "material copper {\n"
        "    type = copper\n"
        "}\n"
        "\n"
        "displace my_wobble {\n"
        "    type = sine\n"
        "    direction = 1 0 0\n"
        "    amplitude = 0.35\n"
        "    frequency = 1.5\n"
        "}\n"
        "\n"
        "box {\n"
        "    center = -2 2 0\n"
        "    half = 0.8 1.5 0.8\n"
        "    material = gold\n"
        "    displace = my_wobble\n"
        "}\n"
        "\n"
        "csg {\n"
        "    operation = difference\n"
        "    material = copper\n"
        "    displace = my_wobble\n"
        "    shape_a = box\n"
        "    center_a = 2 2 0\n"
        "    half_a = 0.9 1.5 0.9\n"
        "    shape_b = cylinder\n"
        "    center_b = 2 2 0\n"
        "    radius_b = 0.5\n"
        "    height_b = 4.0\n"
        "}\n";

    SceneDesc desc;
    scene_desc_init(&desc);
    char errbuf[256] = {0};
    int rc = scene_desc_load_string(&desc, scene_txt, "<test>", errbuf, sizeof(errbuf));
    if (rc != 0) {
        fprintf(stderr, "Parser error: %s\n", errbuf);
    }
    CHECK(rc == 0, "scene_desc_load_string succeeds on displace & csg blocks");
    CHECK(desc.displace_count == 1, "displace block parsed");
    CHECK(desc.prim_count == 2, "box and csg primitives parsed");
    if (desc.prim_count >= 2) {
        CHECK(desc.prims[0].displace_index == 0, "box displace resolved to my_wobble");
        CHECK(desc.prims[1].displace_index == 0, "csg displace resolved to my_wobble");
    }

    /* Build full Scene from description */
    Scene scene;
    int build_rc = scene_build_from_desc(&scene, &desc);
    CHECK(build_rc == 0, "scene_build_from_desc succeeds with displace and csg");
    CHECK(scene.geo.count == 2, "scene geometry contains exactly 2 primitives");
    CHECK(scene.geo.prims[0].kind == PRIM_SDF_SHAPE, "box with displace converted to PRIM_SDF_SHAPE");
    CHECK(scene.geo.prims[1].kind == PRIM_SDF_SHAPE, "csg converted to PRIM_SDF_SHAPE");

    scene_free(&scene);
    scene_desc_free(&desc);
}

static void test_chained_displacements(void)
{
    const char *scene_txt =
        "material gold {\n"
        "    type = gold\n"
        "}\n"
        "displace twist_mod {\n"
        "    type = twist\n"
        "    strength = 0.5\n"
        "}\n"
        "displace wobble_mod {\n"
        "    type = sine\n"
        "    amplitude = 0.2\n"
        "    frequency = 1.0\n"
        "}\n"
        "box {\n"
        "    center = 0 2 0\n"
        "    half = 1 1 1\n"
        "    material = gold\n"
        "    displace = twist_mod wobble_mod\n"
        "}\n";

    SceneDesc desc;
    scene_desc_init(&desc);
    char errbuf[256] = {0};
    int rc = scene_desc_load_string(&desc, scene_txt, "<test_chain>", errbuf, sizeof(errbuf));
    CHECK(rc == 0, "scene_desc_load_string succeeds on chained displace modifiers");
    CHECK(desc.displace_count == 2, "2 displace blocks parsed");
    CHECK(desc.prim_count == 1, "1 box parsed");
    if (desc.prim_count >= 1) {
        CHECK(desc.prims[0].displace_count == 2, "box has displace_count == 2");
        CHECK(desc.prims[0].displace_indices[0] == 0, "first displace is twist_mod (index 0)");
        CHECK(desc.prims[0].displace_indices[1] == 1, "second displace is wobble_mod (index 1)");
    }

    Scene scene;
    int build_rc = scene_build_from_desc(&scene, &desc);
    CHECK(build_rc == 0, "scene_build_from_desc succeeds with chained displaces");
    if (scene.geo.count >= 1) {
        CHECK(scene.geo.prims[0].kind == PRIM_SDF_SHAPE, "primitive is PRIM_SDF_SHAPE");
        CHECK(scene.geo.prims[0].sdf.displace_count == 2, "SdfData displace_count is 2");
    }

    scene_free(&scene);
    scene_desc_free(&desc);
}

int main(void)
{
    printf("Running test_sdf_csg...\n");

    test_sdf_analytical();
    test_sdf_csg_difference();
    test_ray_intersection_and_bvh();
    test_scene_parser_integration();
    test_chained_displacements();

    printf("test_sdf_csg: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
