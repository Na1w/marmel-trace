/*
 * tests/test_math.c - Unit tests for the raytracer math / geometry modules.
 *
 * Covers:
 *   vec3.h     - construction, arithmetic, products, norms, normalize, lerp,
 *                min/max, reflect, refract, vec3_at.
 *   camera.h   - basis construction and primary-ray generation.
 *   geometry.h - sphere / plane / box / triangle / tapered cylinder hits,
 *                tmin/tmax clipping, normal orientation, primitive_bounds,
 *                geometry_intersect nearest-hit selection.
 *   bvh.h      - equivalence with geometry_intersect over a large random
 *                scene, plus hit invariants.
 *
 * The file defines its own main(), uses only a deterministic xorshift PRNG
 * (never rand()), frees every allocation, and returns 0 on success.
 */

#include "vec3.h"
#include "camera.h"
#include "geometry.h"
#include "bvh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test harness                                                        */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } } while (0)

#define CHECK_NEAR(a, b, eps, msg) CHECK(fabs((a) - (b)) <= (eps), msg)

/* Self-intersection epsilon used by the renderer. */
#define TMIN 1e-4
#define TMAX 1e9

/* ------------------------------------------------------------------ */
/* Deterministic PRNG (xorshift64) - never rand()                      */
/* ------------------------------------------------------------------ */

static unsigned long long g_rng_state = 0x9E3779B97F4A7C15ULL;

static unsigned long long xorshift64(void)
{
    unsigned long long x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng_state = x;
    return x;
}

/* Uniform double in [0,1) with 53 bits of entropy. */
static double rnd01(void)
{
    return (double)(xorshift64() >> 11) * (1.0 / 9007199254740992.0);
}

static double rnd_range(double lo, double hi)
{
    return lo + (hi - lo) * rnd01();
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int finite_vec(Vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static int vec_near(Vec3 a, Vec3 b, double eps)
{
    return fabs(a.x - b.x) <= eps &&
           fabs(a.y - b.y) <= eps &&
           fabs(a.z - b.z) <= eps;
}

static int is_unit(Vec3 v, double eps)
{
    return fabs(vec3_length(v) - 1.0) <= eps;
}

static int sign_of(double x)
{
    if (x > 0.0) return 1;
    if (x < 0.0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 1. vec3 basics                                                      */
/* ------------------------------------------------------------------ */

static void test_vec3_basics(void)
{
    Vec3 v = vec3(1.0, 2.0, 3.0);
    Vec3 w = vec3(4.0, 5.0, 6.0);

    /* construct */
    CHECK(v.x == 1.0 && v.y == 2.0 && v.z == 3.0, "vec3 constructs components");

    /* add / sub / neg */
    CHECK(vec_near(vec3_add(v, w), vec3(5.0, 7.0, 9.0), 0.0), "vec3_add");
    CHECK(vec_near(vec3_sub(v, w), vec3(-3.0, -3.0, -3.0), 0.0), "vec3_sub");
    CHECK(vec_near(vec3_neg(v), vec3(-1.0, -2.0, -3.0), 0.0), "vec3_neg");

    /* scale / div */
    CHECK(vec_near(vec3_scale(v, 2.0), vec3(2.0, 4.0, 6.0), 0.0), "vec3_scale");
    CHECK(vec_near(vec3_div(v, 2.0), vec3(0.5, 1.0, 1.5), 0.0), "vec3_div");

    /* div by zero -> zero vector, never NaN (documented behaviour) */
    Vec3 dz = vec3_div(v, 0.0);
    CHECK(dz.x == 0.0 && dz.y == 0.0 && dz.z == 0.0, "vec3_div by zero is zero vector");
    CHECK(!isnan(dz.x) && !isnan(dz.y) && !isnan(dz.z), "vec3_div by zero is not NaN");

    /* hadamard mul */
    CHECK(vec_near(vec3_mul(v, w), vec3(4.0, 10.0, 18.0), 0.0), "vec3_mul (hadamard)");

    /* dot */
    CHECK_NEAR(vec3_dot(v, w), 32.0, 0.0, "vec3_dot");

    /* cross: known value + anti-commutativity */
    Vec3 c1 = vec3_cross(v, w);
    Vec3 c2 = vec3_cross(w, v);
    CHECK(vec_near(c1, vec3(-3.0, 6.0, -3.0), 0.0), "vec3_cross known value");
    CHECK(vec_near(c2, vec3_neg(c1), 0.0), "vec3_cross anti-commutative");

    /* unit axes: cross(x, y) == z */
    Vec3 ex = vec3(1.0, 0.0, 0.0);
    Vec3 ey = vec3(0.0, 1.0, 0.0);
    Vec3 ez = vec3(0.0, 0.0, 1.0);
    CHECK(vec_near(vec3_cross(ex, ey), ez, 0.0), "cross(x,y) == z");
    CHECK(vec_near(vec3_cross(ey, ez), ex, 0.0), "cross(y,z) == x");
    CHECK(vec_near(vec3_cross(ez, ex), ey, 0.0), "cross(z,x) == y");

    /* length / length_sq */
    Vec3 p345 = vec3(3.0, 4.0, 0.0);
    CHECK_NEAR(vec3_length(p345), 5.0, 0.0, "vec3_length (3,4,0) == 5");
    CHECK_NEAR(vec3_length_sq(p345), 25.0, 0.0, "vec3_length_sq (3,4,0) == 25");

    /* normalize */
    CHECK(vec_near(vec3_normalize(vec3(0.0, 3.0, 0.0)), vec3(0.0, 1.0, 0.0), 1e-15),
          "vec3_normalize (0,3,0) -> (0,1,0)");
    CHECK(is_unit(vec3_normalize(vec3(1.0, 1.0, 1.0)), 1e-15), "normalize yields unit length");
    Vec3 nz = vec3_normalize(vec3(0.0, 0.0, 0.0));
    CHECK(nz.x == 0.0 && nz.y == 0.0 && nz.z == 0.0, "normalize of zero vector is zero");
    CHECK(!isnan(nz.x), "normalize of zero vector is not NaN");

    /* lerp */
    CHECK(vec_near(vec3_lerp(v, w, 0.0), v, 0.0), "lerp t=0 returns a");
    CHECK(vec_near(vec3_lerp(v, w, 1.0), w, 0.0), "lerp t=1 returns b");
    CHECK(vec_near(vec3_lerp(v, w, 0.5), vec3(2.5, 3.5, 4.5), 0.0), "lerp t=0.5 midpoint");

    /* min / max component-wise */
    CHECK(vec_near(vec3_min(vec3(1.0, 5.0, 3.0), vec3(4.0, 2.0, 6.0)),
                   vec3(1.0, 2.0, 3.0), 0.0), "vec3_min component-wise");
    CHECK(vec_near(vec3_max(vec3(1.0, 5.0, 3.0), vec3(4.0, 2.0, 6.0)),
                   vec3(4.0, 5.0, 6.0), 0.0), "vec3_max component-wise");

    /* vec3_at on a ray */
    Ray r;
    r.origin = vec3(1.0, 2.0, 3.0);
    r.dir = vec3(0.0, 0.0, 1.0);
    CHECK(vec_near(vec3_at(r, 5.0), vec3(1.0, 2.0, 8.0), 0.0), "vec3_at ray parameter");
    CHECK(vec_near(vec3_at(r, 0.0), r.origin, 0.0), "vec3_at t=0 is origin");
}

/* ------------------------------------------------------------------ */
/* 2. reflect / refract                                                */
/* ------------------------------------------------------------------ */

static void test_reflect_refract(void)
{
    Vec3 n = vec3(0.0, 1.0, 0.0);
    Vec3 i = vec3(0.6, -0.8, 0.0); /* unit */

    /* known reflection off the y-axis */
    Vec3 rr = vec3_reflect(i, n);
    CHECK(vec_near(rr, vec3(0.6, 0.8, 0.0), 1e-15), "reflect known value");
    CHECK(is_unit(rr, 1e-15), "reflect result is unit length");

    /* involution for a unit normal */
    Vec3 rr2 = vec3_reflect(rr, n);
    CHECK(vec_near(rr2, i, 1e-15), "reflect is an involution");

    /* eta == 1.0 must return the incoming direction (no bending) */
    Vec3 t1 = vec3_refract(i, n, 1.0);
    CHECK(vec_near(t1, i, 1e-15), "refract eta=1 returns incoming direction");

    /* air -> glass, 30 degrees incidence. eta = 1/1.5.
     * sin(theta_t) = 1/3, cos(theta_t) = 2*sqrt(2)/3, so the refracted
     * direction is (1/3, -2*sqrt(2)/3, 0). */
    Vec3 in_glass = vec3(0.5, -0.8660254037844386, 0.0); /* (sin30, -cos30, 0) */
    Vec3 tg = vec3_refract(in_glass, n, 1.0 / 1.5);
    double sqrt8_3 = 2.0 * sqrt(2.0) / 3.0;
    CHECK(vec_near(tg, vec3(1.0 / 3.0, -sqrt8_3, 0.0), 1e-12),
          "refract air->glass Snell direction");
    CHECK(is_unit(tg, 1e-12), "refracted direction is unit length");

    /* total internal reflection: glass -> air, 60 degrees (> critical 41.8). */
    Vec3 i_dense = vec3(0.8660254037844386, -0.5, 0.0);
    Vec3 tir = vec3_refract(i_dense, n, 1.5);
    CHECK(tir.x == 0.0 && tir.y == 0.0 && tir.z == 0.0,
          "refract TIR returns zero vector");
}

/* ------------------------------------------------------------------ */
/* 3. camera                                                           */
/* ------------------------------------------------------------------ */

static void test_camera(void)
{
    Camera cam = camera_create(vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0),
                               vec3(0.0, 1.0, 0.0), 90.0, 1.0);

    /* centre ray points straight down -z */
    Ray c = camera_ray(&cam, 0.5, 0.5);
    CHECK(vec_near(c.dir, vec3(0.0, 0.0, -1.0), 1e-9), "camera centre ray is (0,0,-1)");
    CHECK(vec_near(c.origin, vec3(0.0, 0.0, 0.0), 0.0), "camera ray origin is eye");

    /* top-centre ray (u=0.5, v=1) points up, not left */
    Ray top = camera_ray(&cam, 0.5, 1.0);
    CHECK(top.dir.y > 0.0, "camera ray (0.5,1.0) points up");
    CHECK(top.dir.z < 0.0, "camera ray (0.5,1.0) points forward");
    CHECK(top.dir.x > -1e-12, "camera ray (0.5,1.0) is not to the left");

    /* top-right corner points up and to the right */
    Ray ur = camera_ray(&cam, 1.0, 1.0);
    CHECK(ur.dir.x > 0.0 && ur.dir.y > 0.0, "camera ray (1,1) points up-right");

    /* bottom-left corner points down-left */
    Ray bl = camera_ray(&cam, 0.0, 0.0);
    CHECK(bl.dir.x < 0.0 && bl.dir.y < 0.0, "camera ray (0,0) points down-left");

    /* all generated rays are unit length */
    int all_unit = 1;
    for (int iy = 0; iy <= 4; ++iy) {
        for (int ix = 0; ix <= 4; ++ix) {
            Ray r = camera_ray(&cam, (double)ix / 4.0, (double)iy / 4.0);
            if (!is_unit(r.dir, 1e-12)) all_unit = 0;
        }
    }
    CHECK(all_unit, "all camera rays are unit length");

    /* degenerate: up parallel to the view direction must not produce NaN */
    Camera deg = camera_create(vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0),
                               vec3(0.0, 0.0, -1.0), 60.0, 1.333);
    int deg_ok = 1;
    for (int iy = 0; iy <= 2; ++iy) {
        for (int ix = 0; ix <= 2; ++ix) {
            Ray r = camera_ray(&deg, (double)ix / 2.0, (double)iy / 2.0);
            if (!finite_vec(r.dir) || !is_unit(r.dir, 1e-9)) deg_ok = 0;
        }
    }
    CHECK(deg_ok, "degenerate up (parallel to forward) yields finite unit rays");

    /* degenerate: from == at must not produce NaN */
    Camera same = camera_create(vec3(1.0, 1.0, 1.0), vec3(1.0, 1.0, 1.0),
                                vec3(0.0, 1.0, 0.0), 45.0, 1.0);
    Ray sr = camera_ray(&same, 0.5, 0.5);
    CHECK(finite_vec(sr.dir) && is_unit(sr.dir, 1e-9), "from==at yields finite unit ray");
}

/* ------------------------------------------------------------------ */
/* 4. sphere                                                           */
/* ------------------------------------------------------------------ */

static void test_sphere(void)
{
    Primitive s = prim_sphere(vec3(0.0, 0.0, -5.0), 1.0, 7);
    Hit h;

    /* hit from outside */
    Ray r;
    r.origin = vec3(0.0, 0.0, 0.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    int ok = primitive_intersect(&s, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "sphere: outside ray hits");
    CHECK_NEAR(h.t, 4.0, 1e-12, "sphere: expected t == 4");
    CHECK(vec_near(h.point, vec3(0.0, 0.0, -4.0), 1e-12), "sphere: hit point");
    CHECK(vec_near(h.normal, vec3(0.0, 0.0, 1.0), 1e-12), "sphere: outward normal");
    CHECK(h.front_face == 1, "sphere: front_face set");
    CHECK(h.material_index == 7, "sphere: material_index propagated");

    /* miss */
    r.dir = vec3(0.0, 1.0, 0.0);
    CHECK(primitive_intersect(&s, r, TMIN, TMAX, &h) == 0, "sphere: miss");

    /* inside origin -> exit intersection, normal opposes ray */
    r.origin = vec3(0.0, 0.0, -5.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    ok = primitive_intersect(&s, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "sphere: inside-origin hits exit");
    CHECK_NEAR(h.t, 1.0, 1e-12, "sphere: inside-origin t == 1");
    CHECK(vec_near(h.point, vec3(0.0, 0.0, -6.0), 1e-12), "sphere: exit point");
    CHECK(vec3_dot(h.normal, r.dir) < 0.0, "sphere: inside normal opposes ray");
    CHECK(h.front_face == 0, "sphere: inside sets back face");

    /* tangent / edge case */
    r.origin = vec3(1.0, 0.0, 0.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    ok = primitive_intersect(&s, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "sphere: tangent ray hits");
    CHECK_NEAR(h.t, 5.0, 1e-9, "sphere: tangent t == 5");
    CHECK_NEAR(fabs(h.point.x), 1.0, 1e-9, "sphere: tangent point on surface");

    /* tmax clipping */
    r.origin = vec3(0.0, 0.0, 0.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    /* roots are t=4 (near) and t=6 (far). A window containing neither misses. */
    CHECK(primitive_intersect(&s, r, TMIN, 3.0, &h) == 0, "sphere: tmax clips near hit");
    CHECK(primitive_intersect(&s, r, 4.5, 5.5, &h) == 0, "sphere: tmin/tmax clips both roots");
    CHECK(primitive_intersect(&s, r, TMIN, 4.5, &h) == 1, "sphere: t inside range hits");
    /* tmin past the near root must yield the FAR (exit) root at t=6. */
    ok = primitive_intersect(&s, r, 5.0, TMAX, &h);
    CHECK(ok == 1, "sphere: tmin past near root still hits far root");
    CHECK_NEAR(h.t, 6.0, 1e-12, "sphere: tmin past near root selects far root t == 6");

    /* hit behind the origin is rejected */
    Primitive back = prim_sphere(vec3(0.0, 0.0, 5.0), 1.0, 1);
    CHECK(primitive_intersect(&back, r, TMIN, TMAX, &h) == 0, "sphere: behind-origin rejected");
}

/* ------------------------------------------------------------------ */
/* 5. plane                                                            */
/* ------------------------------------------------------------------ */

static void test_plane(void)
{
    Primitive p = prim_plane(vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0), 3);
    Hit h;
    Ray r;

    /* known hit from above */
    r.origin = vec3(0.0, 5.0, 0.0);
    r.dir = vec3(0.0, -1.0, 0.0);
    int ok = primitive_intersect(&p, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "plane: hit from above");
    CHECK_NEAR(h.t, 4.0, 1e-12, "plane: expected t == 4");
    CHECK(vec_near(h.point, vec3(0.0, 1.0, 0.0), 1e-12), "plane: hit point");
    CHECK(vec_near(h.normal, vec3(0.0, 1.0, 0.0), 1e-12), "plane: normal faces ray");
    CHECK(h.material_index == 3, "plane: material_index propagated");

    /* parallel ray rejected */
    r.dir = vec3(1.0, 0.0, 0.0);
    CHECK(primitive_intersect(&p, r, TMIN, TMAX, &h) == 0, "plane: parallel rejected");

    /* hit behind the origin rejected */
    r.dir = vec3(0.0, 1.0, 0.0);
    CHECK(primitive_intersect(&p, r, TMIN, TMAX, &h) == 0, "plane: behind-origin rejected");

    /* normal always opposes the ray (hit from below) */
    r.origin = vec3(0.0, -5.0, 0.0);
    r.dir = vec3(0.0, 1.0, 0.0);
    ok = primitive_intersect(&p, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "plane: hit from below");
    CHECK(vec3_dot(h.normal, r.dir) < 0.0, "plane: normal opposes ray from below");
    CHECK(vec_near(h.normal, vec3(0.0, -1.0, 0.0), 1e-12), "plane: flipped normal from below");

    /* tmin/tmax clipping */
    r.origin = vec3(0.0, 5.0, 0.0);
    r.dir = vec3(0.0, -1.0, 0.0);
    CHECK(primitive_intersect(&p, r, TMIN, 3.0, &h) == 0, "plane: tmax clips hit");
}

/* ------------------------------------------------------------------ */
/* 6. box                                                              */
/* ------------------------------------------------------------------ */

static void test_box(void)
{
    Primitive b = prim_box(vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0), 5);
    Hit h;
    Ray r;

    /* from -x */
    r.origin = vec3(-5.0, 0.0, 0.0);
    r.dir = vec3(1.0, 0.0, 0.0);
    int ok = primitive_intersect(&b, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "box: hit from -x");
    CHECK_NEAR(h.t, 4.0, 1e-12, "box: -x t == 4");
    CHECK(vec_near(h.point, vec3(-1.0, 0.0, 0.0), 1e-12), "box: -x hit point");
    CHECK(vec_near(h.normal, vec3(-1.0, 0.0, 0.0), 1e-12), "box: -x normal");

    /* from +y */
    r.origin = vec3(0.0, 5.0, 0.0);
    r.dir = vec3(0.0, -1.0, 0.0);
    ok = primitive_intersect(&b, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "box: hit from +y");
    CHECK_NEAR(h.t, 4.0, 1e-12, "box: +y t == 4");
    CHECK(vec_near(h.point, vec3(0.0, 1.0, 0.0), 1e-12), "box: +y hit point");
    CHECK(vec_near(h.normal, vec3(0.0, 1.0, 0.0), 1e-12), "box: +y normal");

    /* from +z */
    r.origin = vec3(0.0, 0.0, 5.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    ok = primitive_intersect(&b, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "box: hit from +z");
    CHECK_NEAR(h.t, 4.0, 1e-12, "box: +z t == 4");
    CHECK(vec_near(h.point, vec3(0.0, 0.0, 1.0), 1e-12), "box: +z hit point");
    CHECK(vec_near(h.normal, vec3(0.0, 0.0, 1.0), 1e-12), "box: +z normal");

    /* miss */
    r.origin = vec3(-5.0, 2.0, 0.0);
    r.dir = vec3(1.0, 0.0, 0.0);
    CHECK(primitive_intersect(&b, r, TMIN, TMAX, &h) == 0, "box: miss outside y slab");

    /* inside origin -> exit face */
    r.origin = vec3(0.0, 0.0, 0.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    ok = primitive_intersect(&b, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "box: inside origin hits exit face");
    CHECK_NEAR(h.t, 1.0, 1e-12, "box: inside t == 1");
    CHECK_NEAR(h.point.z, -1.0, 1e-12, "box: inside returns exit face at z=-1");
    CHECK(vec3_dot(h.normal, r.dir) < 0.0, "box: inside normal opposes ray");
    CHECK(h.front_face == 0, "box: inside sets back face");
}

/* ------------------------------------------------------------------ */
/* 7. triangle                                                         */
/* ------------------------------------------------------------------ */

static void test_triangle(void)
{
    Primitive t = prim_triangle(vec3(0.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0),
                                vec3(0.0, 1.0, 0.0), 9);
    Hit h;
    Ray r;

    /* front-face hit (ray travels -z, triangle normal +z) */
    r.origin = vec3(0.25, 0.25, 1.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    int ok = primitive_intersect(&t, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "triangle: front-face hit");
    CHECK_NEAR(h.t, 1.0, 1e-12, "triangle: front t == 1");
    CHECK(vec_near(h.point, vec3(0.25, 0.25, 0.0), 1e-12), "triangle: front hit point");
    CHECK(vec_near(h.normal, vec3(0.0, 0.0, 1.0), 1e-12), "triangle: front normal");
    CHECK(h.front_face == 1, "triangle: front face flag");
    CHECK(h.material_index == 9, "triangle: material_index propagated");

    /* back-face hit registers (two-sided) */
    r.origin = vec3(0.25, 0.25, -1.0);
    r.dir = vec3(0.0, 0.0, 1.0);
    ok = primitive_intersect(&t, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "triangle: back-face hit registers");
    CHECK_NEAR(h.t, 1.0, 1e-12, "triangle: back t == 1");
    CHECK(vec3_dot(h.normal, r.dir) < 0.0, "triangle: back normal opposes ray");
    CHECK(h.front_face == 0, "triangle: back face flag");

    /* miss (outside the triangle) */
    r.origin = vec3(0.6, 0.6, 1.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    CHECK(primitive_intersect(&t, r, TMIN, TMAX, &h) == 0, "triangle: miss outside");

    /* ray parallel to the triangle plane */
    r.origin = vec3(0.25, 0.25, 1.0);
    r.dir = vec3(1.0, 0.0, 0.0);
    CHECK(primitive_intersect(&t, r, TMIN, TMAX, &h) == 0, "triangle: parallel rejected");

    /* hit just outside an edge */
    r.origin = vec3(1.0 + 1e-6, 0.0, 1.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    CHECK(primitive_intersect(&t, r, TMIN, TMAX, &h) == 0, "triangle: just outside edge rejected");

    /* hit just inside an edge */
    r.origin = vec3(0.999999, 0.0, 1.0);
    r.dir = vec3(0.0, 0.0, -1.0);
    CHECK(primitive_intersect(&t, r, TMIN, TMAX, &h) == 1, "triangle: just inside edge hits");
}

/* ------------------------------------------------------------------ */
/* 8. tapered cylinder                                                 */
/* ------------------------------------------------------------------ */

static void test_cylinder(void)
{
    /* Frustum: base (0,0,0) r=1, top (0,2,0) r=2. */
    Primitive cyl = prim_cylinder(vec3(0.0, 0.0, 0.0), vec3(0.0, 2.0, 0.0),
                                  1.0, 2.0, 11);
    Hit h;
    Ray r;

    /* lateral hit at y = 1, where the interpolated radius is 1.5 */
    r.origin = vec3(5.0, 1.0, 0.0);
    r.dir = vec3(-1.0, 0.0, 0.0);
    int ok = primitive_intersect(&cyl, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "cylinder: lateral frustum hit");
    CHECK_NEAR(h.t, 3.5, 1e-9, "cylinder: lateral t == 3.5");
    CHECK_NEAR(h.point.x, 1.5, 1e-9, "cylinder: hit point at interpolated radius");
    CHECK_NEAR(h.point.y, 1.0, 1e-9, "cylinder: hit point at y=1");
    /* radial distance from the axis point must equal the interpolated radius */
    double s = 0.5;
    double r_expect = 1.0 + s * (2.0 - 1.0);
    Vec3 axis_pt = vec3(0.0, 1.0, 0.0);
    Vec3 radial = vec3_sub(h.point, axis_pt);
    CHECK_NEAR(vec3_length(radial), r_expect, 1e-9,
               "cylinder: radial distance equals interpolated radius");
    CHECK(is_unit(h.normal, 1e-12), "cylinder: lateral normal is unit");
    CHECK(vec3_dot(h.normal, r.dir) < 0.0, "cylinder: lateral normal opposes ray");

    /* top cap hit */
    r.origin = vec3(0.0, 5.0, 0.0);
    r.dir = vec3(0.0, -1.0, 0.0);
    ok = primitive_intersect(&cyl, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "cylinder: top cap hit");
    CHECK_NEAR(h.t, 3.0, 1e-9, "cylinder: top cap t == 3");
    CHECK_NEAR(h.point.y, 2.0, 1e-9, "cylinder: top cap at y=2");
    CHECK(vec_near(h.normal, vec3(0.0, 1.0, 0.0), 1e-9), "cylinder: top cap normal +axis");

    /* bottom cap hit */
    r.origin = vec3(0.0, -5.0, 0.0);
    r.dir = vec3(0.0, 1.0, 0.0);
    ok = primitive_intersect(&cyl, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "cylinder: bottom cap hit");
    CHECK_NEAR(h.t, 5.0, 1e-9, "cylinder: bottom cap t == 5");
    CHECK_NEAR(h.point.y, 0.0, 1e-9, "cylinder: bottom cap at y=0");
    CHECK(vec_near(h.normal, vec3(0.0, -1.0, 0.0), 1e-9), "cylinder: bottom cap normal -axis");

    /* miss that passes outside the cap radius */
    r.origin = vec3(3.0, 5.0, 0.0);
    r.dir = vec3(0.0, -1.0, 0.0);
    CHECK(primitive_intersect(&cyl, r, TMIN, TMAX, &h) == 0,
          "cylinder: outside cap radius misses");

    /* pure cylinder (r_bottom == r_top) */
    Primitive pc = prim_cylinder(vec3(0.0, 0.0, 0.0), vec3(0.0, 3.0, 0.0),
                                 1.0, 1.0, 12);
    r.origin = vec3(5.0, 1.5, 0.0);
    r.dir = vec3(-1.0, 0.0, 0.0);
    ok = primitive_intersect(&pc, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "cylinder: pure cylinder lateral hit");
    CHECK_NEAR(h.t, 4.0, 1e-9, "cylinder: pure lateral t == 4");
    CHECK(vec_near(h.normal, vec3(1.0, 0.0, 0.0), 1e-9), "cylinder: pure lateral normal");
}

/* ------------------------------------------------------------------ */
/* 9. primitive_bounds                                                 */
/* ------------------------------------------------------------------ */

static int in_box(Vec3 p, Vec3 mn, Vec3 mx, double eps)
{
    return p.x >= mn.x - eps && p.x <= mx.x + eps &&
           p.y >= mn.y - eps && p.y <= mx.y + eps &&
           p.z >= mn.z - eps && p.z <= mx.z + eps;
}

static void test_bounds(void)
{
    Vec3 mn, mx;

    /* --- sphere: centred, half extent == radius --- */
    Primitive s = prim_sphere(vec3(1.0, 2.0, 3.0), 2.5, 0);
    primitive_bounds(&s, &mn, &mx);
    CHECK_NEAR(mn.x, -1.5, 1e-12, "sphere bounds min.x");
    CHECK_NEAR(mn.y, -0.5, 1e-12, "sphere bounds min.y");
    CHECK_NEAR(mn.z, 0.5, 1e-12, "sphere bounds min.z");
    CHECK_NEAR(mx.x, 3.5, 1e-12, "sphere bounds max.x");
    CHECK_NEAR(mx.y, 4.5, 1e-12, "sphere bounds max.y");
    CHECK_NEAR(mx.z, 5.5, 1e-12, "sphere bounds max.z");
    {
        Vec3 centre = vec3_scale(vec3_add(mn, mx), 0.5);
        CHECK(vec_near(centre, s.center, 1e-12), "sphere bounds centred on centre");
        CHECK_NEAR(mx.x - mn.x, 2.0 * s.radius, 1e-12, "sphere bounds half extent == radius");
    }
    {
        /* sample surface points and confirm containment */
        Vec3 dirs[6] = { vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0),
                         vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1) };
        int all_in = 1;
        for (int i = 0; i < 6; ++i) {
            Vec3 p = vec3_add(s.center, vec3_scale(dirs[i], s.radius));
            if (!in_box(p, mn, mx, 1e-9)) all_in = 0;
        }
        CHECK(all_in, "sphere bounds contain sampled surface");
        CHECK(!in_box(vec3(1.0 + 2.5 + 0.01, 2.0, 3.0), mn, mx, 1e-9),
              "sphere bounds are tight (outside point excluded)");
    }

    /* --- plane: large finite box --- */
    Primitive pl = prim_plane(vec3(0.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), 0);
    primitive_bounds(&pl, &mn, &mx);
    CHECK(mn.x == -1e4 && mn.y == -1e4 && mn.z == -1e4, "plane bounds min is -1e4");
    CHECK(mx.x == 1e4 && mx.y == 1e4 && mx.z == 1e4, "plane bounds max is +1e4");
    CHECK(in_box(vec3(0.0, 0.0, 0.0), mn, mx, 0.0), "plane bounds contain a plane point");

    /* --- box: exactly centre +/- half --- */
    Primitive b = prim_box(vec3(1.0, 2.0, 3.0), vec3(2.0, 1.0, 0.5), 0);
    primitive_bounds(&b, &mn, &mx);
    CHECK_NEAR(mn.x, -1.0, 1e-12, "box bounds min.x");
    CHECK_NEAR(mn.y, 1.0, 1e-12, "box bounds min.y");
    CHECK_NEAR(mn.z, 2.5, 1e-12, "box bounds min.z");
    CHECK_NEAR(mx.x, 3.0, 1e-12, "box bounds max.x");
    CHECK_NEAR(mx.y, 3.0, 1e-12, "box bounds max.y");
    CHECK_NEAR(mx.z, 3.5, 1e-12, "box bounds max.z");
    {
        Vec3 corners[8];
        int k = 0;
        for (int sx = 0; sx < 2; ++sx)
            for (int sy = 0; sy < 2; ++sy)
                for (int sz = 0; sz < 2; ++sz)
                    corners[k++] = vec3(sx ? mx.x : mn.x, sy ? mx.y : mn.y,
                                        sz ? mx.z : mn.z);
        int all_in = 1;
        for (int i = 0; i < 8; ++i)
            if (!in_box(corners[i], mn, mx, 1e-12)) all_in = 0;
        CHECK(all_in, "box bounds contain all corners");
    }

    /* --- triangle: contains vertices, edges and centroid --- */
    Primitive t = prim_triangle(vec3(1.0, 1.0, 1.0), vec3(3.0, 2.0, 4.0),
                                vec3(2.0, 5.0, 0.0), 0);
    primitive_bounds(&t, &mn, &mx);
    CHECK_NEAR(mn.x, 1.0, 1e-12, "triangle bounds min.x");
    CHECK_NEAR(mn.y, 1.0, 1e-12, "triangle bounds min.y");
    CHECK_NEAR(mn.z, 0.0, 1e-12, "triangle bounds min.z");
    CHECK_NEAR(mx.x, 3.0, 1e-12, "triangle bounds max.x");
    CHECK_NEAR(mx.y, 5.0, 1e-12, "triangle bounds max.y");
    CHECK_NEAR(mx.z, 4.0, 1e-12, "triangle bounds max.z");
    {
        Vec3 samples[7];
        samples[0] = t.a;
        samples[1] = t.b;
        samples[2] = t.c;
        samples[3] = vec3_scale(vec3_add(t.a, t.b), 0.5);
        samples[4] = vec3_scale(vec3_add(t.b, t.c), 0.5);
        samples[5] = vec3_scale(vec3_add(t.c, t.a), 0.5);
        samples[6] = vec3_scale(vec3_add(vec3_add(t.a, t.b), t.c), 1.0 / 3.0);
        int all_in = 1;
        for (int i = 0; i < 7; ++i)
            if (!in_box(samples[i], mn, mx, 1e-9)) all_in = 0;
        CHECK(all_in, "triangle bounds contain vertices/edges/centroid");
    }

    /* --- tapered cylinder: base + top + radius perpendicular --- */
    Primitive c = prim_cylinder(vec3(0.0, 0.0, 0.0), vec3(0.0, 3.0, 0.0),
                                1.0, 2.0, 0);
    primitive_bounds(&c, &mn, &mx);
    CHECK_NEAR(mn.x, -2.0, 1e-12, "cylinder bounds min.x");
    CHECK_NEAR(mn.y, -2.0, 1e-12, "cylinder bounds min.y");
    CHECK_NEAR(mn.z, -2.0, 1e-12, "cylinder bounds min.z");
    CHECK_NEAR(mx.x, 2.0, 1e-12, "cylinder bounds max.x");
    CHECK_NEAR(mx.y, 5.0, 1e-12, "cylinder bounds max.y");
    CHECK_NEAR(mx.z, 2.0, 1e-12, "cylinder bounds max.z");
    CHECK(in_box(vec3(0.0, 0.0, 0.0), mn, mx, 1e-12), "cylinder bounds contain base centre");
    CHECK(in_box(vec3(0.0, 3.0, 0.0), mn, mx, 1e-12), "cylinder bounds contain top centre");
    {
        /* sample the lateral surface: r(s) = r0 + s*(r1-r0) */
        int all_in = 1;
        for (int i = 0; i <= 4; ++i) {
            double s = (double)i / 4.0;
            double rr = 1.0 + s * (2.0 - 1.0);
            Vec3 axis_pt = vec3(0.0, s * 3.0, 0.0);
            if (!in_box(vec3_add(axis_pt, vec3(rr, 0.0, 0.0)), mn, mx, 1e-9)) all_in = 0;
            if (!in_box(vec3_add(axis_pt, vec3(0.0, 0.0, rr)), mn, mx, 1e-9)) all_in = 0;
        }
        CHECK(all_in, "cylinder bounds contain sampled lateral surface");
        CHECK(!in_box(vec3(2.5, 0.0, 0.0), mn, mx, 1e-9),
              "cylinder bounds exclude point beyond max radius");
    }
}

/* ------------------------------------------------------------------ */
/* 10. geometry_intersect nearest hit                                  */
/* ------------------------------------------------------------------ */

static void test_geometry_intersect(void)
{
    Geometry g;
    geometry_init(&g);
    CHECK(g.count == 0 && g.prims == NULL, "geometry_init empties container");

    int i0 = geometry_add(&g, prim_sphere(vec3(0.0, 0.0, -10.0), 1.0, 10));
    int i1 = geometry_add(&g, prim_sphere(vec3(0.0, 0.0, -5.0), 1.0, 11));
    int i2 = geometry_add(&g, prim_sphere(vec3(0.0, 0.0, -2.0), 1.0, 12));
    int i3 = geometry_add(&g, prim_plane(vec3(0.0, 0.0, -20.0), vec3(0.0, 0.0, 1.0), 13));

    CHECK(i0 == 0 && i1 == 1 && i2 == 2 && i3 == 3, "geometry_add returns sequential indices");
    CHECK(g.count == 4, "geometry_add increments count");

    Ray r;
    r.origin = vec3(0.0, 0.0, 0.0);
    r.dir = vec3(0.0, 0.0, -1.0);

    Hit h;
    int ok = geometry_intersect(&g, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "geometry_intersect hits");
    CHECK_NEAR(h.t, 1.0, 1e-12, "geometry_intersect returns nearest t");
    CHECK(h.prim_index == 2, "geometry_intersect returns nearest prim_index");
    CHECK(h.material_index == 12, "geometry_intersect returns nearest material_index");

    /* tmax short of the nearest hit -> no hit */
    CHECK(geometry_intersect(&g, r, TMIN, 0.5, &h) == 0,
          "geometry_intersect respects tmax");
    /* ray pointing away -> no hit */
    r.dir = vec3(0.0, 0.0, 1.0);
    CHECK(geometry_intersect(&g, r, TMIN, TMAX, &h) == 0,
          "geometry_intersect miss when pointing away");

    geometry_free(&g);
    CHECK(g.count == 0 && g.prims == NULL, "geometry_free resets container");
}

/* ------------------------------------------------------------------ */
/* 11 + 12. BVH equivalence and hit invariants                         */
/* ------------------------------------------------------------------ */

#define SCENE_PRIMS 600
#define SCENE_RAYS  6000

static Geometry g_big;
static Bvh *g_big_bvh = NULL;

static void build_random_scene(Geometry *g, int n)
{
    for (int i = 0; i < n; ++i) {
        int kind = (int)(xorshift64() % 4u);
        int mat = i % 8;
        double cx = rnd_range(-6.0, 6.0);
        double cy = rnd_range(-6.0, 6.0);
        double cz = rnd_range(-6.0, 6.0);

        if (kind == 0) {
            double rad = rnd_range(0.2, 1.5);
            geometry_add(g, prim_sphere(vec3(cx, cy, cz), rad, mat));
        } else if (kind == 1) {
            Vec3 half = vec3(rnd_range(0.1, 1.2), rnd_range(0.1, 1.2),
                             rnd_range(0.1, 1.2));
            geometry_add(g, prim_box(vec3(cx, cy, cz), half, mat));
        } else if (kind == 2) {
            Vec3 a = vec3(cx, cy, cz);
            Vec3 b = vec3(cx + rnd_range(-2.0, 2.0), cy + rnd_range(-2.0, 2.0),
                          cz + rnd_range(-2.0, 2.0));
            Vec3 c = vec3(cx + rnd_range(-2.0, 2.0), cy + rnd_range(-2.0, 2.0),
                          cz + rnd_range(-2.0, 2.0));
            geometry_add(g, prim_triangle(a, b, c, mat));
        } else {
            Vec3 base = vec3(cx, cy, cz);
            Vec3 top = vec3(cx + rnd_range(-1.0, 1.0), cy + rnd_range(-1.0, 1.0),
                            cz + rnd_range(-1.0, 1.0));
            double r0 = rnd_range(0.1, 1.2);
            double r1 = rnd_range(0.1, 1.2);
            geometry_add(g, prim_cylinder(base, top, r0, r1, mat));
        }
    }
}

static void big_scene_setup(void)
{
    geometry_init(&g_big);
    build_random_scene(&g_big, SCENE_PRIMS);
    g_big_bvh = bvh_build(&g_big);
}

static void big_scene_teardown(void)
{
    bvh_free(g_big_bvh);
    g_big_bvh = NULL;
    geometry_free(&g_big);
}

static void test_bvh_equivalence(void)
{
    CHECK(g_big.count >= 500, "random scene has at least 500 primitives");
    CHECK(g_big_bvh != NULL, "bvh_build succeeds on random scene");
    if (!g_big_bvh) return;

    CHECK(bvh_node_count(g_big_bvh) > 0, "bvh_node_count > 0");
    CHECK(bvh_depth(g_big_bvh) > 0, "bvh_depth > 0");

    int total = 0;
    int both_hit = 0, both_miss = 0;
    int m_hitmiss = 0, m_prim = 0, m_mat = 0, m_t = 0, m_norm = 0;

    for (int i = 0; i < SCENE_RAYS; ++i) {
        Ray r;
        r.origin = vec3(rnd_range(-10.0, 10.0), rnd_range(-10.0, 10.0),
                        rnd_range(-10.0, 10.0));
        Vec3 d = vec3(rnd_range(-1.0, 1.0), rnd_range(-1.0, 1.0),
                      rnd_range(-1.0, 1.0));
        if (vec3_length_sq(d) < 1e-9) continue;
        r.dir = vec3_normalize(d);
        ++total;

        Hit gh, bh;
        int gok = geometry_intersect(&g_big, r, TMIN, TMAX, &gh);
        int bok = bvh_intersect(g_big_bvh, &g_big, r, TMIN, TMAX, &bh);

        if (gok != bok) {
            if (m_hitmiss < 5)
                fprintf(stderr, "  bvh mismatch (hit/miss) at ray %d: g=%d b=%d\n",
                        i, gok, bok);
            m_hitmiss++;
            continue;
        }
        if (!gok) { both_miss++; continue; }

        both_hit++;
        if (gh.prim_index != bh.prim_index) {
            if (m_prim < 5)
                fprintf(stderr, "  bvh mismatch prim %d: g=%d b=%d\n",
                        i, gh.prim_index, bh.prim_index);
            m_prim++;
        }
        if (gh.material_index != bh.material_index) m_mat++;
        if (!(fabs(gh.t - bh.t) <= 1e-9 * fmax(1.0, fabs(gh.t)))) {
            if (m_t < 5)
                fprintf(stderr, "  bvh mismatch t %d: g=%.17g b=%.17g\n", i, gh.t, bh.t);
            m_t++;
        }
        if (sign_of(vec3_dot(gh.normal, r.dir)) != sign_of(vec3_dot(bh.normal, r.dir)))
            m_norm++;
    }

    CHECK(m_hitmiss == 0, "bvh/geometry agree on hit vs miss for every ray");
    CHECK(m_prim == 0, "bvh/geometry agree on prim_index for every ray");
    CHECK(m_mat == 0, "bvh/geometry agree on material_index for every ray");
    CHECK(m_t == 0, "bvh/geometry agree on t for every ray");
    CHECK(m_norm == 0, "bvh/geometry agree on normal/ray dot sign for every ray");

    printf("tests/test_math: BVH equivalence over %d rays (%d hit-agreements, "
           "%d miss-agreements, %d mismatches)\n",
           total, both_hit, both_miss, m_hitmiss + m_prim + m_mat + m_t + m_norm);
}

static void test_hit_invariants(void)
{
    if (!g_big_bvh) return;

    int hits = 0;
    int bad_t = 0, bad_point = 0, bad_normal_finite = 0, bad_normal_unit = 0;
    int bad_oppose = 0;

    for (int i = 0; i < SCENE_RAYS; ++i) {
        Ray r;
        r.origin = vec3(rnd_range(-10.0, 10.0), rnd_range(-10.0, 10.0),
                        rnd_range(-10.0, 10.0));
        Vec3 d = vec3(rnd_range(-1.0, 1.0), rnd_range(-1.0, 1.0),
                      rnd_range(-1.0, 1.0));
        if (vec3_length_sq(d) < 1e-9) continue;
        r.dir = vec3_normalize(d);

        Hit h;
        if (!geometry_intersect(&g_big, r, TMIN, TMAX, &h)) continue;
        ++hits;

        if (!isfinite(h.t) || h.t <= 0.0) bad_t++;
        if (!finite_vec(h.point)) bad_point++;
        if (!finite_vec(h.normal)) bad_normal_finite++;
        if (!is_unit(h.normal, 1e-9)) bad_normal_unit++;
        if (vec3_dot(h.normal, r.dir) > 1e-9) bad_oppose++;
    }

    CHECK(hits > 0, "invariants: sampled at least one hit");
    CHECK(bad_t == 0, "invariants: t finite and > 0 on every hit");
    CHECK(bad_point == 0, "invariants: hit point finite (no NaN/inf) on every hit");
    CHECK(bad_normal_finite == 0, "invariants: normal finite (no NaN/inf) on every hit");
    CHECK(bad_normal_unit == 0, "invariants: normal unit length within 1e-9 on every hit");
    CHECK(bad_oppose == 0, "invariants: normal opposes ray direction on every hit");

    printf("tests/test_math: invariants checked on %d hits\n", hits);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_vec3_basics();
    test_reflect_refract();
    test_camera();
    test_sphere();
    test_plane();
    test_box();
    test_triangle();
    test_cylinder();
    test_bounds();
    test_geometry_intersect();

    big_scene_setup();
    test_bvh_equivalence();
    test_hit_invariants();
    big_scene_teardown();

    printf("tests/test_math: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
