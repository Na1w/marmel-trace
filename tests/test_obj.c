/*
 * test_obj.c - Unit and integration tests for Wavefront OBJ loader.
 */

#include "obj.h"
#include "scene_desc.h"
#include "scene.h"
#include "bvh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        g_pass++; \
    } else { \
        g_fail++; \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
    } \
} while (0)

static void test_obj_basic_triangle(void)
{
    const char *obj_text =
        "# Simple triangle\n"
        "v 0.0 0.0 0.0\n"
        "v 2.0 0.0 0.0\n"
        "v 0.0 2.0 0.0\n"
        "f 1 2 3\n";

    Geometry g;
    geometry_init(&g);

    int count = obj_load_mem(obj_text, strlen(obj_text), &g, 0, NULL);
    CHECK(count == 1, "loads 1 triangle from memory");
    CHECK(g.count == 1, "geometry has 1 primitive");
    if (g.count == 1) {
        CHECK(g.prims[0].kind == PRIM_TRIANGLE, "primitive kind is PRIM_TRIANGLE");
        CHECK(fabs(g.prims[0].a.x - 0.0) < 1e-6, "vertex A matches");
        CHECK(fabs(g.prims[0].b.x - 2.0) < 1e-6, "vertex B matches");
        CHECK(fabs(g.prims[0].c.y - 2.0) < 1e-6, "vertex C matches");
    }

    geometry_free(&g);
}

static void test_obj_quad_fan_triangulation(void)
{
    /* Quad face with texture and normals */
    const char *obj_text =
        "v -1.0 -1.0 0.0\n"
        "v  1.0 -1.0 0.0\n"
        "v  1.0  1.0 0.0\n"
        "v -1.0  1.0 0.0\n"
        "vn 0.0 0.0 1.0\n"
        "vt 0.0 0.0\n"
        "vt 1.0 0.0\n"
        "vt 1.0 1.0\n"
        "vt 0.0 1.0\n"
        "f 1/1/1 2/2/1 3/3/1 4/4/1\n";

    Geometry g;
    geometry_init(&g);

    int count = obj_load_mem(obj_text, strlen(obj_text), &g, 2, NULL);
    CHECK(count == 2, "quad triangulated into 2 triangles");
    CHECK(g.count == 2, "geometry has 2 primitives");
    if (g.count == 2) {
        CHECK(g.prims[0].material_index == 2, "material index passed through");
        CHECK(g.prims[0].radius > 0.5, "smooth normals enabled when vn present");
        /* Check interpolated normal */
        Ray ray = { vec3(0.0, 0.0, 5.0), vec3(0.0, 0.0, -1.0) };
        Hit hit;
        memset(&hit, 0, sizeof(hit));
        int hit_rc = primitive_intersect(&g.prims[0], ray, 1e-4, 100.0, &hit);
        if (!hit_rc) {
            hit_rc = primitive_intersect(&g.prims[1], ray, 1e-4, 100.0, &hit);
        }
        CHECK(hit_rc == 1, "ray hits triangulated quad");
        CHECK(hit.normal.z > 0.99, "normal points towards +Z");
    }

    geometry_free(&g);
}

static void test_obj_relative_indexing(void)
{
    /* Negative indices */
    const char *obj_text =
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f -3 -2 -1\n";

    Geometry g;
    geometry_init(&g);

    int count = obj_load_mem(obj_text, strlen(obj_text), &g, 0, NULL);
    CHECK(count == 1, "relative negative indices parsed correctly");

    geometry_free(&g);
}

static void test_obj_transformations(void)
{
    const char *obj_text =
        "v -1 0 0\n"
        "v  1 0 0\n"
        "v  0 2 0\n"
        "f 1 2 3\n";

    Geometry g;
    geometry_init(&g);

    ObjTransform xf = obj_transform_default();
    xf.position = vec3(10.0, 5.0, -2.0);
    xf.scale = vec3(2.0, 2.0, 2.0);

    int count = obj_load_mem(obj_text, strlen(obj_text), &g, 1, &xf);
    CHECK(count == 1, "1 triangle transformed");
    if (g.count == 1) {
        /* Vertex 0 originally (-1, 0, 0) -> scaled (-2, 0, 0) -> translated (8, 5, -2) */
        CHECK(fabs(g.prims[0].a.x - 8.0) < 1e-5, "transformed vertex A.x matches");
        CHECK(fabs(g.prims[0].a.y - 5.0) < 1e-5, "transformed vertex A.y matches");
        CHECK(fabs(g.prims[0].a.z - (-2.0)) < 1e-5, "transformed vertex A.z matches");
        /* Vertex 2 originally (0, 2, 0) -> scaled (0, 4, 0) -> translated (10, 9, -2) */
        CHECK(fabs(g.prims[0].c.x - 10.0) < 1e-5, "transformed vertex C.x matches");
        CHECK(fabs(g.prims[0].c.y - 9.0) < 1e-5, "transformed vertex C.y matches");
    }

    geometry_free(&g);
}

static void test_obj_scene_desc_integration(void)
{
    /* Create a temporary .obj file */
    const char *tmp_obj_path = "tests/temp_test_cube.obj";
    FILE *f = fopen(tmp_obj_path, "w");
    if (f) {
        fprintf(f,
            "# Cube\n"
            "v -0.5 -0.5  0.5\n"
            "v  0.5 -0.5  0.5\n"
            "v -0.5  0.5  0.5\n"
            "v  0.5  0.5  0.5\n"
            "v -0.5  0.5 -0.5\n"
            "v  0.5  0.5 -0.5\n"
            "v -0.5 -0.5 -0.5\n"
            "v  0.5 -0.5 -0.5\n"
            "f 1 2 4 3\n"
            "f 3 4 6 5\n"
            "f 5 6 8 7\n"
            "f 7 8 2 1\n"
            "f 2 8 6 4\n"
            "f 7 1 3 5\n"
        );
        fclose(f);
    }

    const char *scene_txt =
        "material bronze {\n"
        "    type = copper\n"
        "}\n"
        "\n"
        "mesh {\n"
        "    file = \"tests/temp_test_cube.obj\"\n"
        "    material = bronze\n"
        "    center = 0 2 0\n"
        "    scale = 1.5\n"
        "}\n";

    SceneDesc desc;
    scene_desc_init(&desc);
    char errbuf[256] = {0};
    int rc = scene_desc_load_string(&desc, scene_txt, "<test_mesh>", errbuf, sizeof(errbuf));
    if (rc != 0) {
        fprintf(stderr, "Scene load error: %s\n", errbuf);
    }
    CHECK(rc == 0, "scene_desc_load_string parses mesh block");
    CHECK(desc.mesh_count == 1, "desc.mesh_count is 1");
    if (desc.mesh_count == 1) {
        CHECK(strcmp(desc.meshes[0].file, "tests/temp_test_cube.obj") == 0, "mesh file path matches");
        CHECK(desc.meshes[0].material_index == 0, "mesh material resolved to bronze");
        CHECK(fabs(desc.meshes[0].scale.x - 1.5) < 1e-5, "mesh scale is 1.5");
    }

    /* Build Scene and BVH */
    Scene scene;
    int build_rc = scene_build_from_desc(&scene, &desc);
    CHECK(build_rc == 0, "scene_build_from_desc succeeds with mesh");
    CHECK(scene.geo.count == 12, "cube loaded exactly 12 triangles (6 quads * 2)");

    /* Fire ray at the cube */
    Ray ray = { vec3(0.0, 2.0, 5.0), vec3(0.0, 0.0, -1.0) };
    Hit hit;
    memset(&hit, 0, sizeof(hit));
    int hit_rc = bvh_intersect(scene.bvh, &scene.geo, ray, 1e-4, 100.0, &hit);
    CHECK(hit_rc == 1, "ray hits mesh cube via BVH");
    CHECK(hit.material_index == 0, "hit material matches bronze");

    scene_free(&scene);
    scene_desc_free(&desc);

    remove(tmp_obj_path);
}

int main(void)
{
    printf("Running test_obj...\n");

    test_obj_basic_triangle();
    test_obj_quad_fan_triangulation();
    test_obj_relative_indexing();
    test_obj_transformations();
    test_obj_scene_desc_integration();

    printf("test_obj: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
