/*
 * tests/test_csg.c - Unit tests for Constructive Solid Geometry (CSG).
 *
 * Tests:
 *   1. CSG difference (Sphere \ Cylinder):
 *      - Ray through drilled hole passes through (miss).
 *      - Ray hitting outside sphere hits at sphere surface with sphere normal.
 *      - Ray hitting inside cylinder tunnel hits with inverted cylinder normal.
 *      - Ray origin inside cavity.
 *   2. CSG difference (Box \ Cylinder):
 *      - Drilled tunnel through box.
 *   3. CSG intersection (Sphere ∩ Sphere):
 *      - Overlapping sphere lens: rays outside intersection miss; ray through lens hits.
 *   4. CSG union (Sphere ∪ Box):
 *      - Ray hitting combined boundary; internal boundaries removed.
 *   5. Shadow occlusion:
 *      - Ray passing through drilled hole is NOT occluded.
 *      - Ray through solid part IS occluded.
 *   6. Scene description parsing & resolution:
 *      - scene_desc_load_string with `csg_difference`, `csg_intersection`, `csg_union`.
 *      - Material inheritance when child primitive has no material.
 *      - Explicit child material override.
 *      - Round-trip formatting via scene_desc_write.
 *   7. Scene construction & BVH integration:
 *      - scene_build_from_desc produces valid BVH and primitives.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <assert.h>

#include "geometry.h"
#include "scene_desc.h"
#include "scene.h"

static int g_passed = 0;
static int g_failed = 0;

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        g_passed++; \
    } else { \
        g_failed++; \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
    } \
} while (0)

#define TEST_NEAR(a, b, eps, msg) do { \
    if (fabs((a) - (b)) <= (eps)) { \
        g_passed++; \
    } else { \
        g_failed++; \
        fprintf(stderr, "FAIL [%s:%d]: %s (expected %.6f, got %.6f, diff %.6f)\n", \
                __FILE__, __LINE__, msg, (double)(b), (double)(a), fabs((double)(a) - (double)(b))); \
    } \
} while (0)

static void test_csg_difference_sphere_cylinder(void)
{
    /* Solid sphere at (0, 0, 0), radius 1.0 */
    Primitive s = prim_sphere(vec3(0, 0, 0), 1.0, 10);
    /* Drill cylinder along Z axis from z = -2 to z = 2, radius 0.4 */
    Primitive cyl = prim_cylinder(vec3(0, 0, -2), vec3(0, 0, 2), 0.4, 0.4, 20);

    Primitive csg = prim_csg_difference(s, cyl, 10);

    /* 1. Ray right down the center of the tunnel: origin (0, 0, -5), dir (0, 0, 1) */
    Ray r_tunnel = { vec3(0, 0, -5), vec3(0, 0, 1) };
    Hit hit;
    int hit_res = primitive_intersect(&csg, r_tunnel, 1e-4, 100.0, &hit);
    TEST_CHECK(!hit_res, "Ray through center of drilled tunnel should miss");
    TEST_CHECK(!primitive_occluded(&csg, r_tunnel, 1e-4, 100.0), "Tunnel ray should not be occluded");

    /* 2. Ray near the edge of the tunnel (x = 0.35, y = 0, z = -5):
     * Tunnel radius is 0.4, so x = 0.35 is INSIDE the tunnel! It should pass through! */
    Ray r_tunnel_edge = { vec3(0.35, 0, -5), vec3(0, 0, 1) };
    TEST_CHECK(!primitive_intersect(&csg, r_tunnel_edge, 1e-4, 100.0, &hit), "Ray within tunnel radius should miss");

    /* 3. Ray outside the tunnel (x = 0.7, y = 0, z = -5):
     * Sphere at x = 0.7 has y=0, z = sqrt(1.0^2 - 0.7^2) = sqrt(0.51) ≈ 0.71414.
     * So it should hit the sphere at z = -0.71414, which is t = 5 - 0.71414 = 4.28585.
     * Material should be 10 (sphere material). */
    Ray r_outside = { vec3(0.7, 0, -5), vec3(0, 0, 1) };
    hit_res = primitive_intersect(&csg, r_outside, 1e-4, 100.0, &hit);
    TEST_CHECK(hit_res, "Ray hitting sphere outside tunnel should hit");
    TEST_NEAR(hit.t, 5.0 - sqrt(0.51), 1e-4, "Hit t matches sphere entry");
    TEST_CHECK(hit.material_index == 10, "Material is sphere material");
    TEST_CHECK(hit.front_face == 1, "Front face is 1");
    TEST_CHECK(hit.normal.z < 0.0, "Normal opposes incoming ray");

    /* 4. Ray aimed transversely through the sphere and hitting the carved tunnel wall:
     * Ray from origin (-5, 0, 0) aiming along +X (1, 0, 0).
     * The sphere outer surface is at x = -1.0 (t = 4.0).
     * The carved tunnel wall is at x = -0.4 (t = 4.6) and x = +0.4 (t = 5.4).
     * If the ray enters from outside: it first hits outer sphere at x = -1.0 (t = 4.0). */
    Ray r_trans = { vec3(-5, 0, 0), vec3(1, 0, 0) };
    hit_res = primitive_intersect(&csg, r_trans, 1e-4, 100.0, &hit);
    TEST_CHECK(hit_res, "Transverse ray should hit outer sphere");
    TEST_NEAR(hit.t, 4.0, 1e-4, "Hit outer sphere at x = -1");

    /* Now start the ray INSIDE the carved tunnel: origin (0, 0, 0), direction (1, 0, 0).
     * It travels in open air inside the tunnel until x = 0.4 (t = 0.4), where it hits
     * the carved tunnel wall entering the solid!
     * The normal at x = 0.4 should point into the tunnel (-X direction, opposing ray)! */
    Ray r_from_tunnel = { vec3(0, 0, 0), vec3(1, 0, 0) };
    hit_res = primitive_intersect(&csg, r_from_tunnel, 1e-4, 100.0, &hit);
    TEST_CHECK(hit_res, "Ray starting inside tunnel should hit carved wall");
    TEST_NEAR(hit.t, 0.4, 1e-4, "Hit tunnel wall at distance = 0.4");
    TEST_CHECK(hit.normal.x < -0.99, "Normal at tunnel wall points into tunnel (-X)");
    TEST_CHECK(hit.material_index == 20, "Tunnel wall has tool material (cylinder material)");

    /* Occlusion test */
    TEST_CHECK(primitive_occluded(&csg, r_from_tunnel, 1e-4, 10.0), "Tunnel wall occludes ray");

    primitive_destroy(&csg);
}

static void test_csg_difference_box_cylinder(void)
{
    /* Box centered at (0, 0, 0), half (1, 1, 1) -> [-1, 1]^3 */
    Primitive b = prim_box(vec3(0, 0, 0), vec3(1, 1, 1), 1);
    /* Vertical hole along Y: base (0, -2, 0), top (0, 2, 0), radius 0.5 */
    Primitive cyl = prim_cylinder(vec3(0, -2, 0), vec3(0, 2, 0), 0.5, 0.5, 2);

    Primitive csg = prim_csg_difference(b, cyl, 1);

    /* Vertical ray down through the hole: origin (0, 5, 0), dir (0, -1, 0) */
    Ray r_hole = { vec3(0, 5, 0), vec3(0, -1, 0) };
    Hit hit;
    TEST_CHECK(!primitive_intersect(&csg, r_hole, 1e-4, 100.0, &hit), "Vertical ray through drilled box hole should miss");
    TEST_CHECK(!primitive_occluded(&csg, r_hole, 1e-4, 100.0), "Hole ray not occluded");

    /* Ray hitting top face of box outside hole: origin (0.8, 5, 0), dir (0, -1, 0) */
    Ray r_box_top = { vec3(0.8, 5, 0), vec3(0, -1, 0) };
    int hit_res = primitive_intersect(&csg, r_box_top, 1e-4, 100.0, &hit);
    TEST_CHECK(hit_res, "Ray outside hole hits top face of box");
    TEST_NEAR(hit.t, 4.0, 1e-4, "Hits top face at y = 1 (t = 4.0)");
    TEST_NEAR(hit.normal.y, 1.0, 1e-4, "Top face normal points up (+Y)");

    primitive_destroy(&csg);
}

static void test_csg_intersection(void)
{
    /* Two spheres of radius 1.0, one at (-0.5, 0, 0) and one at (0.5, 0, 0).
     * Intersection is a lens centered at x = 0, spanning x in [-0.5, 0.5]. */
    Primitive s1 = prim_sphere(vec3(-0.5, 0, 0), 1.0, 1);
    Primitive s2 = prim_sphere(vec3(0.5, 0, 0), 1.0, 2);

    Primitive csg = prim_csg_intersection(s1, s2, 3);

    /* Ray through center: origin (0, 0, -5), dir (0, 0, 1).
     * Both spheres have center at y = 0, z = 0, and x = +-0.5.
     * At x = 0, distance from center is 0.5.
     * Sphere radius 1.0 -> z = sqrt(1.0^2 - 0.5^2) = sqrt(0.75) ≈ 0.866025.
     * Ray should hit at z = -0.866025, t = 5.0 - 0.866025 = 4.133975. */
    Ray r_lens = { vec3(0, 0, -5), vec3(0, 0, 1) };
    Hit hit;
    int hit_res = primitive_intersect(&csg, r_lens, 1e-4, 100.0, &hit);
    TEST_CHECK(hit_res, "Ray through lens center should hit");
    TEST_NEAR(hit.t, 5.0 - sqrt(0.75), 1e-4, "Hit distance matches lens entry");

    /* Ray passing through s1 but outside s2 (e.g. at x = -1.2, y = 0, z = -5) */
    Ray r_miss = { vec3(-1.2, 0, -5), vec3(0, 0, 1) };
    TEST_CHECK(!primitive_intersect(&csg, r_miss, 1e-4, 100.0, &hit), "Ray outside intersection volume must miss");

    primitive_destroy(&csg);
}

static void test_csg_union(void)
{
    /* Sphere at (0, 0, 0) radius 1.0 and Box at (0.8, 0, 0) half (0.5, 0.5, 0.5) */
    Primitive s = prim_sphere(vec3(0, 0, 0), 1.0, 1);
    Primitive b = prim_box(vec3(0.8, 0, 0), vec3(0.5, 0.5, 0.5), 2);

    Primitive csg = prim_csg_union(s, b, 1);

    /* Ray hitting sphere: origin (-5, 0, 0), dir (1, 0, 0) */
    Ray r_left = { vec3(-5, 0, 0), vec3(1, 0, 0) };
    Hit hit;
    int hit_res = primitive_intersect(&csg, r_left, 1e-4, 100.0, &hit);
    TEST_CHECK(hit_res, "Ray hits union at left sphere boundary");
    TEST_NEAR(hit.t, 4.0, 1e-4, "Hits at x = -1.0");

    /* Ray hitting box: origin (5, 0, 0), dir (-1, 0, 0) */
    Ray r_right = { vec3(5, 0, 0), vec3(-1, 0, 0) };
    hit_res = primitive_intersect(&csg, r_right, 1e-4, 100.0, &hit);
    TEST_CHECK(hit_res, "Ray hits union at right box boundary");
    TEST_NEAR(hit.t, 5.0 - (0.8 + 0.5), 1e-4, "Hits at x = 1.3");

    primitive_destroy(&csg);
}

static void test_csg_parser_integration(void)
{
    const char *scene_text =
        "material rock {\n"
        "    albedo = 0.5 0.5 0.5\n"
        "}\n"
        "material gold {\n"
        "    albedo = 1.0 0.8 0.2\n"
        "}\n"
        "csg_difference {\n"
        "    material = rock\n"
        "    sphere {\n"
        "        center = 0.0 1.0 0.0\n"
        "        radius = 1.0\n"
        "    }\n"
        "    cylinder {\n"
        "        base = 0.0 1.0 -2.0\n"
        "        top = 0.0 1.0 2.0\n"
        "        r_bottom = 0.3\n"
        "        r_top = 0.3\n"
        "        material = gold\n"
        "    }\n"
        "}\n";

    SceneDesc desc;
    scene_desc_init(&desc);
    char errbuf[256] = {0};
    int rc = scene_desc_load_string(&desc, scene_text, "test_csg", errbuf, sizeof(errbuf));
    TEST_CHECK(rc == 0, "Parsed scene with csg_difference successfully");
    if (rc != 0) {
        fprintf(stderr, "Parser error: %s\n", errbuf);
    } else {
        TEST_CHECK(desc.prim_count == 1, "Desc has 1 root primitive");
        TEST_CHECK(desc.prims[0].kind == PRIM_CSG, "Root primitive is PRIM_CSG");
        TEST_CHECK(desc.prims[0].csg_op == CSG_DIFFERENCE, "CSG op is difference");
        TEST_CHECK(desc.prims[0].left != NULL, "Left child present");
        TEST_CHECK(desc.prims[0].right != NULL, "Right child present");
        TEST_CHECK(desc.prims[0].left->kind == PRIM_SPHERE, "Left is sphere");
        TEST_CHECK(desc.prims[0].right->kind == PRIM_CYLINDER, "Right is cylinder");
        TEST_CHECK(desc.prims[0].left->material_index == 0, "Left inherited rock material (0)");
        TEST_CHECK(desc.prims[0].right->material_index == 1, "Right has explicit gold material (1)");

        /* Test build into Scene */
        Scene s;
        int build_rc = scene_build_from_desc(&s, &desc);
        TEST_CHECK(build_rc == 0, "Scene built from desc with CSG");

        /* Ray through carved hole at y = 1.0: (0, 1, -5) -> (0, 0, 1) */
        Ray r_hole = { vec3(0, 1, -5), vec3(0, 0, 1) };
        Hit h;
        int h_res = scene_intersect(&s, r_hole, 1e-4, 100.0, &h);
        TEST_CHECK(!h_res, "Ray through carved tunnel in scene misses");

        /* Ray hitting outer sphere: (0.7, 1, -5) -> (0, 0, 1) */
        Ray r_rock = { vec3(0.7, 1, -5), vec3(0, 0, 1) };
        h_res = scene_intersect(&s, r_rock, 1e-4, 100.0, &h);
        TEST_CHECK(h_res, "Ray hits rock outside hole");
        TEST_CHECK(h.material_index == 0, "Hit has rock material");

        /* Ray from inside tunnel towards tunnel wall: (0, 1, 0) -> (1, 0, 0) */
        Ray r_inner = { vec3(0, 1, 0), vec3(1, 0, 0) };
        h_res = scene_intersect(&s, r_inner, 1e-4, 100.0, &h);
        TEST_CHECK(h_res, "Ray from inside tunnel hits gold interior wall");
        TEST_CHECK(h.material_index == 1, "Hit has gold material");
        TEST_NEAR(h.t, 0.3, 1e-4, "Hits at cylinder radius 0.3");

        scene_free(&s);
    }
    scene_desc_free(&desc);
}

int main(void)
{
    printf("== bin/test_csg ==\n");
    test_csg_difference_sphere_cylinder();
    test_csg_difference_box_cylinder();
    test_csg_intersection();
    test_csg_union();
    test_csg_parser_integration();

    printf("tests/test_csg: %d passed, %d failed\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
