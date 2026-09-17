/*
 * tests/test_curve.c - Unit tests for 3D curves, tubes, bent beams, and fairytale trees.
 */

#include "curve.h"
#include "geometry.h"
#include "bvh.h"
#include "vec3.h"

#include <stdio.h>
#include <stdlib.h>
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

static void test_bezier_eval(void)
{
    Vec3 p0 = vec3(0.0, 0.0, 0.0);
    Vec3 p1 = vec3(0.0, 2.0, 0.0);
    Vec3 p2 = vec3(0.0, 4.0, 0.0);
    Vec3 p3 = vec3(0.0, 6.0, 0.0);

    Curve3D c = curve_bezier(p0, p1, p2, p3, 0.5, 0.2);

    /* Endpoints */
    Vec3 at0 = curve_eval(&c, 0.0);
    Vec3 at1 = curve_eval(&c, 1.0);
    Vec3 at_mid = curve_eval(&c, 0.5);

    CHECK_CLOSE(at0.x, 0.0, 1e-6, "eval(0).x == p0.x");
    CHECK_CLOSE(at0.y, 0.0, 1e-6, "eval(0).y == p0.y");
    CHECK_CLOSE(at0.z, 0.0, 1e-6, "eval(0).z == p0.z");

    CHECK_CLOSE(at1.x, 0.0, 1e-6, "eval(1).x == p3.x");
    CHECK_CLOSE(at1.y, 6.0, 1e-6, "eval(1).y == p3.y");
    CHECK_CLOSE(at1.z, 0.0, 1e-6, "eval(1).z == p3.z");

    CHECK_CLOSE(at_mid.y, 3.0, 1e-6, "eval(0.5).y == 3.0 for linear curve");

    /* Radius interpolation */
    CHECK_CLOSE(curve_radius(&c, 0.0), 0.5, 1e-6, "radius(0) == r_start");
    CHECK_CLOSE(curve_radius(&c, 1.0), 0.2, 1e-6, "radius(1) == r_end");
    CHECK_CLOSE(curve_radius(&c, 0.5), 0.35, 1e-6, "radius(0.5) == 0.35");
}

static void test_wobble_and_noise_pinning(void)
{
    Vec3 p0 = vec3(1.0, 2.0, 3.0);
    Vec3 p1 = vec3(1.0, 5.0, 3.0);
    Vec3 p2 = vec3(2.0, 8.0, 4.0);
    Vec3 p3 = vec3(3.0, 10.0, 5.0);

    Curve3D c = curve_bezier(p0, p1, p2, p3, 0.4, 0.1);
    curve_set_wobble(&c, 1.5, 3.0);
    curve_set_noise(&c, 0.8, 2.0, 42u);

    /* Wobble and noise must strictly preserve endpoints (envelope E(0)=E(1)=0) */
    Vec3 at0 = curve_eval(&c, 0.0);
    Vec3 at1 = curve_eval(&c, 1.0);

    CHECK_CLOSE(at0.x, p0.x, 1e-6, "wobble+noise preserves p0.x");
    CHECK_CLOSE(at0.y, p0.y, 1e-6, "wobble+noise preserves p0.y");
    CHECK_CLOSE(at0.z, p0.z, 1e-6, "wobble+noise preserves p0.z");

    CHECK_CLOSE(at1.x, p3.x, 1e-6, "wobble+noise preserves p3.x");
    CHECK_CLOSE(at1.y, p3.y, 1e-6, "wobble+noise preserves p3.y");
    CHECK_CLOSE(at1.z, p3.z, 1e-6, "wobble+noise preserves p3.z");

    /* Midpoint should be displaced by modulation */
    Curve3D c_plain = curve_bezier(p0, p1, p2, p3, 0.4, 0.1);
    Vec3 plain_mid = curve_eval(&c_plain, 0.5);
    Vec3 mod_mid   = curve_eval(&c, 0.5);
    double dist = vec3_length(vec3_sub(plain_mid, mod_mid));
    CHECK(dist > 0.01, "midpoint is modulated by wobble/noise");
}

static void test_tangent(void)
{
    Vec3 p0 = vec3(0.0, 0.0, 0.0);
    Vec3 p1 = vec3(0.0, 2.0, 0.0);
    Vec3 p2 = vec3(0.0, 4.0, 0.0);
    Vec3 p3 = vec3(0.0, 6.0, 0.0);
    Curve3D c = curve_bezier(p0, p1, p2, p3, 0.3, 0.1);

    Vec3 t0 = curve_tangent(&c, 0.0);
    Vec3 t_mid = curve_tangent(&c, 0.5);
    Vec3 t1 = curve_tangent(&c, 1.0);

    CHECK_CLOSE(t0.y, 1.0, 1e-4, "straight curve tangent y == 1.0 at t=0");
    CHECK_CLOSE(t_mid.y, 1.0, 1e-4, "straight curve tangent y == 1.0 at t=0.5");
    CHECK_CLOSE(t1.y, 1.0, 1e-4, "straight curve tangent y == 1.0 at t=1");
    CHECK_CLOSE(vec3_length(t_mid), 1.0, 1e-6, "tangent is unit length");
}

static void test_tube_builder(void)
{
    Geometry g;
    geometry_init(&g);

    Vec3 p0 = vec3(0.0, 0.0, 0.0);
    Vec3 p1 = vec3(1.0, 2.0, 0.0);
    Vec3 p2 = vec3(-1.0, 4.0, 0.0);
    Vec3 p3 = vec3(0.0, 6.0, 0.0);
    Curve3D c = curve_bezier(p0, p1, p2, p3, 0.4, 0.1);

    int rc = curve_build_tube(&g, &c, 8, 1);
    CHECK(rc == 0, "curve_build_tube returns 0");
    CHECK(g.count == 8, "tube with 8 segments adds exactly 8 primitives");

    for (int i = 0; i < g.count; ++i) {
        CHECK(g.prims[i].kind == PRIM_CYLINDER, "primitive is PRIM_CYLINDER");
        CHECK(g.prims[i].material_index == 1, "primitive has correct material_index");
    }

    geometry_free(&g);
}

static void test_beam_builder(void)
{
    Geometry g;
    geometry_init(&g);

    Vec3 p0 = vec3(0.0, 0.0, 0.0);
    Vec3 p1 = vec3(0.0, 2.0, 0.0);
    Vec3 p2 = vec3(0.0, 4.0, 0.0);
    Vec3 p3 = vec3(0.0, 6.0, 0.0);
    Curve3D c = curve_bezier(p0, p1, p2, p3, 0.5, 0.5);

    /* 10 segments => 10 * 8 sides + 4 caps = 84 triangles */
    int rc = curve_build_beam(&g, &c, 0.5, 0.5, 10, 2);
    CHECK(rc == 0, "curve_build_beam returns 0");
    CHECK(g.count == 84, "curve_build_beam produces exactly 84 triangles for 10 segments");

    /* Ray intersection test: ray aimed right at the center of the beam */
    Ray ray = { vec3(0.0, 3.0, 5.0), vec3(0.0, 0.0, -1.0) };
    Hit hit;
    int hit_rc = geometry_intersect(&g, ray, 1e-4, 100.0, &hit);
    CHECK(hit_rc == 1, "ray hits curved beam");
    CHECK_CLOSE(hit.point.z, 0.5, 1e-3, "hit point z is on front face at z=0.5");
    CHECK(hit.material_index == 2, "hit material_index matches beam");

    geometry_free(&g);
}

static void test_fairytale_tree(void)
{
    Geometry g;
    geometry_init(&g);

    Vec3 p0 = vec3(0.0, 0.0, 0.0);
    Vec3 p1 = vec3(0.5, 3.0, 0.2);
    Vec3 p2 = vec3(-0.8, 6.0, -0.3);
    Vec3 p3 = vec3(0.2, 8.0, 0.1);

    Curve3D trunk = curve_bezier(p0, p1, p2, p3, 0.6, 0.15);
    curve_set_wobble(&trunk, 0.3, 1.5);
    curve_set_noise(&trunk, 0.15, 1.2, 777u);

    FairytaleTreeParams params;
    fairytale_tree_params_default(&params);
    params.bough_count = 4;
    params.branch_max_depth = 3;

    int rc = curve_grow_fairytale_tree(&g, &trunk, 3, 4, 12345u, &params);
    CHECK(rc == 0, "curve_grow_fairytale_tree returns 0");
    CHECK(g.count > 100, "fairytale tree generates a rich set of primitives (>100)");

    /* Verify BVH builds cleanly on tree geometry */
    Bvh *bvh = bvh_build(&g);
    CHECK(bvh != NULL, "BVH builds successfully on fairytale tree geometry");

    /* Test ray cast against the tree BVH */
    Ray trunk_ray = { vec3(5.0, 1.0, 0.0), vec3(-1.0, 0.0, 0.0) };
    Hit hit;
    int bvh_hit = bvh_intersect(bvh, &g, trunk_ray, 1e-4, 100.0, &hit);
    CHECK(bvh_hit == 1, "ray hits fairytale tree trunk");
    CHECK(hit.material_index == 3, "trunk hit is bark material");

    bvh_free(bvh);
    geometry_free(&g);
}

int main(void)
{
    printf("Running test_curve...\n");

    test_bezier_eval();
    test_wobble_and_noise_pinning();
    test_tangent();
    test_tube_builder();
    test_beam_builder();
    test_fairytale_tree();

    printf("test_curve: %d passed, %d failed\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
