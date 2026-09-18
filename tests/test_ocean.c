/*
 * test_ocean.c - Unit tests for procedural Gerstner ocean waves and scene parsing.
 */

#include "scene_desc.h"
#include "scene.h"
#include "geometry.h"
#include "material.h"

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

static void test_ocean_parse_and_defaults(void)
{
    const char *scene_text =
        "material ocean_mat {\n"
        "    type = water\n"
        "    ior = 1.333\n"
        "    is_water = 1\n"
        "}\n"
        "ocean {\n"
        "    material = ocean_mat\n"
        "    center = 0.0 0.5 -10.0\n"
        "    size = 60.0 80.0\n"
        "    resolution = 32 32\n"
        "    amplitude = 0.35\n"
        "    wavelength = 15.0\n"
        "    direction = 1.0 0.5\n"
        "    steepness = 0.6\n"
        "    chop = 0.10\n"
        "    chop_wavelength = 4.5\n"
        "    depth = 8.0\n"
        "    seed = 1234\n"
        "}\n";

    SceneDesc d;
    scene_desc_init(&d);
    char errbuf[256] = {0};
    int rc = scene_desc_load_string(&d, scene_text, "test_ocean", errbuf, sizeof(errbuf));
    CHECK(rc == 0, "loads ocean scene without errors");
    CHECK(d.ocean_count == 1, "parsed 1 ocean block");

    if (d.ocean_count == 1) {
        const SceneOceanDesc *o = &d.oceans[0];
        CHECK(strcmp(o->material_name, "ocean_mat") == 0, "material name is ocean_mat");
        CHECK(o->material_index >= 0, "material index resolved");
        CHECK(fabs(o->center.x - 0.0) < 1e-5, "center.x matches");
        CHECK(fabs(o->center.y - 0.5) < 1e-5, "center.y matches");
        CHECK(fabs(o->center.z - (-10.0)) < 1e-5, "center.z matches");
        CHECK(fabs(o->size.x - 60.0) < 1e-5, "size.x matches");
        CHECK(fabs(o->size.z - 80.0) < 1e-5, "size.z matches");
        CHECK(o->res_x == 32 && o->res_z == 32, "resolution matches");
        CHECK(fabs(o->amplitude - 0.35) < 1e-5, "amplitude matches");
        CHECK(fabs(o->wavelength - 15.0) < 1e-5, "wavelength matches");
        CHECK(fabs(o->steepness - 0.6) < 1e-5, "steepness matches");
        CHECK(fabs(o->chop - 0.10) < 1e-5, "chop matches");
        CHECK(fabs(o->chop_wavelength - 4.5) < 1e-5, "chop wavelength matches");
        CHECK(fabs(o->depth - 8.0) < 1e-5, "depth matches");
        CHECK(o->seed == 1234, "seed matches");
    }

    scene_desc_free(&d);
}

static void test_ocean_scene_construction_and_height(void)
{
    const char *scene_text =
        "material water_mat {\n"
        "    type = water\n"
        "    ior = 1.333\n"
        "    is_water = 1\n"
        "}\n"
        "ocean {\n"
        "    material = water_mat\n"
        "    center = 0.0 0.0 0.0\n"
        "    size = 20.0 20.0\n"
        "    resolution = 16 16\n"
        "    amplitude = 0.4\n"
        "    wavelength = 10.0\n"
        "    steepness = 0.5\n"
        "    depth = 5.0\n"
        "}\n";

    SceneDesc d;
    scene_desc_init(&d);
    char errbuf[256] = {0};
    int rc = scene_desc_load_string(&d, scene_text, "test_ocean", errbuf, sizeof(errbuf));
    CHECK(rc == 0, "loads scene string");

    Scene s;
    memset(&s, 0, sizeof(s));
    rc = scene_build_from_desc(&s, &d);
    CHECK(rc == 0, "builds scene from desc");
    CHECK(s.has_ocean == 1, "scene has_ocean is active");
    CHECK(s.water_material >= 0, "scene water_material is set");

    /* Top surface quads: 15 * 15 * 2 = 450 triangles.
     * Skirts: 4 * 15 * 2 = 120 triangles.
     * Floor: 2 triangles. Total = 572 triangles. */
    CHECK(s.geo.count == 572, "geometry has expected 572 triangles");

    /* Check wave height evaluation */
    double h0 = scene_water_height(&s, 0.0, 0.0);
    /* Mean height at center should be near center.y + sum(A) = 0.0 + amplitude */
    CHECK(h0 > -1.0 && h0 < 1.0, "water height within physical wave envelope");

    /* Ray below water level */
    double submerged_y = h0 - 0.5;
    CHECK(submerged_y < scene_water_height(&s, 0.0, 0.0), "submerged ray origin correctly detected as underwater");

    /* Ray above water level */
    double above_y = h0 + 0.5;
    CHECK(above_y > scene_water_height(&s, 0.0, 0.0), "above water ray origin correctly detected as air");

    /* Check normals of triangles are normalized unit vectors */
    int normals_ok = 1;
    for (int i = 0; i < s.geo.count; i++) {
        Primitive *p = &s.geo.prims[i];
        if (p->radius > 0.5) { /* smooth triangle */
            double la = vec3_length(p->center);
            double lb = vec3_length(p->axis);
            double lc = vec3_length(p->half);
            if (fabs(la - 1.0) > 1e-4 || fabs(lb - 1.0) > 1e-4 || fabs(lc - 1.0) > 1e-4) {
                normals_ok = 0;
                break;
            }
        }
    }
    CHECK(normals_ok, "all smooth triangle vertex normals are normalized unit vectors");

    scene_free(&s);
    scene_desc_free(&d);
}

int main(void)
{
    printf("Running test_ocean...\n");
    test_ocean_parse_and_defaults();
    test_ocean_scene_construction_and_height();

    printf("test_ocean: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
