/*
 * tests/test_bvh_planes.c - Focused regression tests for BVH PLANE handling.
 *
 * Background: src/bvh.c keeps PRIM_PLANE primitives OUT of the SAH tree
 * (their huge [-1e4,1e4]^3 AABB ruins split quality) and instead stores their
 * indices in a linear side-list that is tested before the tree traversal.
 * bvh_intersect() must remain semantically identical to geometry_intersect()
 * for these cases, including the tie-break rule: on an equal `t`, the hit
 * with the LOWEST prim_index wins.
 *
 * This file deliberately does NOT duplicate the broad random BVH-vs-linear
 * equivalence test in tests/test_math.c; it adds the plane-specific coverage:
 *   1. plane nearer than any tree hit must win,
 *   2. all-plane geometry (node_count == 0) still returns correct hits,
 *   3. no-plane geometry (plane side-list empty) stays equivalent,
 *   4. plane + tree tie-break at exactly equal `t` (both orders),
 *   5. a single plane,
 *   6. randomized mixed-scene equivalence including planes, with exact
 *      prim_index / material_index / front_face agreement.
 *
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own main() and returns non-zero
 * on any failure. C11, -Wall -Wextra clean. No rand() (deterministic xorshift).
 * All heap memory is freed.
 */

#include "geometry.h"
#include "bvh.h"
#include "vec3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Test harness (same style as the existing tests)                     */
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

static unsigned long long g_rng_state = 0x243F6A8885A308D3ULL;

static unsigned long long xorshift64(void)
{
    unsigned long long x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng_state = x;
    return x;
}

static double rnd01(void)
{
    return (double)(xorshift64() >> 11) * (1.0 / 9007199254740992.0);
}

static double rnd_range(double lo, double hi)
{
    return lo + (hi - lo) * rnd01();
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Compare a BVH hit against the linear-scan reference for one ray.
 * Returns the number of mismatches observed (0 == perfect agreement). */
static int compare_ray(const Geometry *g, const Bvh *b, Ray r, int *checked_hit)
{
    Hit gh, bh;
    int gok = geometry_intersect(g, r, TMIN, TMAX, &gh);
    int bok = bvh_intersect(b, g, r, TMIN, TMAX, &bh);

    if (gok != bok) return 1;
    if (!gok) return 0;

    if (checked_hit) (*checked_hit)++;

    int bad = 0;
    if (gh.prim_index != bh.prim_index) bad = 1;
    if (gh.material_index != bh.material_index) bad = 1;
    if (gh.front_face != bh.front_face) bad = 1;
    /* t within ~1e-9 (relative to magnitude). */
    if (!(fabs(gh.t - bh.t) <= 1e-9 * fmax(1.0, fabs(gh.t)))) bad = 1;
    return bad;
}

/* ------------------------------------------------------------------ */
/* 1. Plane nearer than any tree hit must win                          */
/* ------------------------------------------------------------------ */

static void test_plane_nearer_than_tree(void)
{
    Geometry g;
    geometry_init(&g);

    /* A tree primitive (sphere) far away, and a plane much nearer. */
    int sphere_idx = geometry_add(&g, prim_sphere(vec3(0.0, -5.0, 0.0), 2.0, 21));
    int plane_idx  = geometry_add(&g, prim_plane(vec3(0.0, 1.0, 0.0),
                                                 vec3(0.0, 1.0, 0.0), 22));
    CHECK(sphere_idx == 0 && plane_idx == 1, "nearer-than-tree: indices assigned");

    Bvh *b = bvh_build(&g);
    CHECK(b != NULL, "nearer-than-tree: bvh_build succeeds");
    CHECK(bvh_node_count(b) > 0, "nearer-than-tree: tree exists (sphere)");
    if (!b) { geometry_free(&g); return; }

    /* Straight down from above: plane at y=1 -> t=4; sphere top at y=-3 -> t=8. */
    Ray r;
    r.origin = vec3(0.0, 5.0, 0.0);
    r.dir = vec3(0.0, -1.0, 0.0);

    Hit h;
    int ok = bvh_intersect(b, &g, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "nearer-than-tree: bvh hit found");
    CHECK(h.prim_index == plane_idx, "nearer-than-tree: plane (nearer) wins");
    CHECK(h.material_index == 22, "nearer-than-tree: plane material reported");
    CHECK_NEAR(h.t, 4.0, 1e-12, "nearer-than-tree: t == 4 (plane)");

    /* And the reference agrees. */
    Hit gh;
    CHECK(geometry_intersect(&g, r, TMIN, TMAX, &gh) == 1,
          "nearer-than-tree: linear scan hits");
    CHECK(gh.prim_index == h.prim_index && gh.material_index == h.material_index,
          "nearer-than-tree: bvh == linear");

    bvh_free(b);
    geometry_free(&g);
}

/* ------------------------------------------------------------------ */
/* 2. All-plane geometry (no tree at all)                              */
/* ------------------------------------------------------------------ */

static void test_all_plane_geometry(void)
{
    Geometry g;
    geometry_init(&g);

    /* Three axis-aligned planes forming a corner around the origin. */
    geometry_add(&g, prim_plane(vec3(0.0, 0.0, 6.0), vec3(0.0, 0.0, 1.0), 31));
    geometry_add(&g, prim_plane(vec3(0.0, -3.0, 0.0), vec3(0.0, 1.0, 0.0), 32));
    geometry_add(&g, prim_plane(vec3(4.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), 33));

    Bvh *b = bvh_build(&g);
    CHECK(b != NULL, "all-plane: bvh_build succeeds");
    CHECK(bvh_node_count(b) == 0, "all-plane: node_count == 0 (no SAH tree)");
    if (!b) { geometry_free(&g); return; }

    /* A known hit: shoot +z from the origin -> the z=6 plane at t=6. */
    Ray r;
    r.origin = vec3(0.0, 0.0, 0.0);
    r.dir = vec3(0.0, 0.0, 1.0);
    Hit h;
    int ok = bvh_intersect(b, &g, r, TMIN, TMAX, &h);
    CHECK(ok == 1, "all-plane: known ray hits");
    CHECK(h.prim_index == 0, "all-plane: known ray hits prim 0");
    CHECK_NEAR(h.t, 6.0, 1e-12, "all-plane: known ray t == 6");

    /* Randomized equivalence against the linear scan (plane-only scene). */
    int total = 0, mism = 0, hits = 0;
    for (int i = 0; i < 2000; ++i) {
        Ray rr;
        rr.origin = vec3(rnd_range(-10.0, 10.0), rnd_range(-10.0, 10.0),
                         rnd_range(-10.0, 10.0));
        Vec3 d = vec3(rnd_range(-1.0, 1.0), rnd_range(-1.0, 1.0),
                      rnd_range(-1.0, 1.0));
        if (vec3_length_sq(d) < 1e-9) continue;
        rr.dir = vec3_normalize(d);
        ++total;
        mism += compare_ray(&g, b, rr, &hits);
    }
    CHECK(mism == 0, "all-plane: bvh == linear for every random ray");
    CHECK(hits > 0, "all-plane: random rays produced at least one hit");

    printf("tests/test_bvh_planes: all-plane over %d rays (%d hits, %d mismatches)\n",
           total, hits, mism);

    bvh_free(b);
    geometry_free(&g);
}

/* ------------------------------------------------------------------ */
/* 3. No-plane geometry (empty plane side-list)                        */
/* ------------------------------------------------------------------ */

static void test_no_plane_geometry(void)
{
    Geometry g;
    geometry_init(&g);
    geometry_add(&g, prim_sphere(vec3(0.0, 0.0, -3.0), 1.0, 41));
    geometry_add(&g, prim_box(vec3(2.0, 0.0, -4.0), vec3(1.0, 1.0, 1.0), 42));
    geometry_add(&g, prim_cylinder(vec3(-2.0, -1.0, -3.0), vec3(-2.0, 1.0, -3.0),
                                   0.8, 0.5, 43));

    Bvh *b = bvh_build(&g);
    CHECK(b != NULL, "no-plane: bvh_build succeeds");
    CHECK(bvh_node_count(b) > 0, "no-plane: SAH tree exists");
    if (!b) { geometry_free(&g); return; }

    int mism = 0, hits = 0;
    for (int i = 0; i < 2000; ++i) {
        Ray rr;
        rr.origin = vec3(rnd_range(-8.0, 8.0), rnd_range(-8.0, 8.0),
                         rnd_range(-8.0, 8.0));
        /* Aim roughly at the origin (with jitter) so most rays hit the
         * clustered primitives, making the comparison meaningful. */
        Vec3 target = vec3(rnd_range(-2.0, 2.0), rnd_range(-2.0, 2.0),
                           rnd_range(-2.0, 2.0));
        Vec3 d = vec3_sub(target, rr.origin);
        if (vec3_length_sq(d) < 1e-9) continue;
        rr.dir = vec3_normalize(d);
        mism += compare_ray(&g, b, rr, &hits);
    }
    CHECK(mism == 0, "no-plane: bvh == linear for every random ray");
    CHECK(hits > 0, "no-plane: random rays produced at least one hit");

    printf("tests/test_bvh_planes: no-plane scene over 2000 rays (%d hits, "
           "%d mismatches)\n", hits, mism);

    bvh_free(b);
    geometry_free(&g);
}

/* ------------------------------------------------------------------ */
/* 4. Plane + tree tie-break at exactly equal t                        */
/* ------------------------------------------------------------------ */

/*
 * Build a scene where a plane and a sphere both intersect the ray at
 * t == 4.0 exactly (all inputs are exactly representable, so the equality is
 * bit-exact). Depending on the insertion order, either the plane or the
 * sphere has the LOWER prim_index; bvh_intersect must return the lower index,
 * exactly like geometry_intersect.
 */
static void tie_break_case(int plane_first, const char *label)
{
    Geometry g;
    geometry_init(&g);

    int plane_idx, sphere_idx;
    /* Plane y = 1, ray from (0,5,0) down -> t = (1-5)/(-1) = 4 (exact).
     * Sphere centre (0,-3,0) r=4, ray down -> nearest root t = 8 - 4 = 4. */
    Primitive pl = prim_plane(vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0), 51);
    Primitive sp = prim_sphere(vec3(0.0, -3.0, 0.0), 4.0, 52);

    if (plane_first) {
        plane_idx  = geometry_add(&g, pl);
        sphere_idx = geometry_add(&g, sp);
    } else {
        sphere_idx = geometry_add(&g, sp);
        plane_idx  = geometry_add(&g, pl);
    }
    int expected = plane_idx < sphere_idx ? plane_idx : sphere_idx;

    Bvh *b = bvh_build(&g);
    CHECK(b != NULL, label);
    if (!b) { geometry_free(&g); return; }

    Ray r;
    r.origin = vec3(0.0, 5.0, 0.0);
    r.dir = vec3(0.0, -1.0, 0.0);

    Hit gh, bh;
    int gok = geometry_intersect(&g, r, TMIN, TMAX, &gh);
    int bok = bvh_intersect(b, &g, r, TMIN, TMAX, &bh);
    CHECK(gok == 1 && bok == 1, label);
    CHECK_NEAR(gh.t, 4.0, 0.0, "tie-break: reference t == 4 exactly");
    CHECK_NEAR(bh.t, 4.0, 0.0, "tie-break: bvh t == 4 exactly");
    CHECK(gh.prim_index == expected, "tie-break: linear picks lowest prim_index");
    CHECK(bh.prim_index == expected, "tie-break: bvh picks lowest prim_index");
    CHECK(bh.prim_index == gh.prim_index, "tie-break: bvh == linear prim_index");

    bvh_free(b);
    geometry_free(&g);
}

static void test_tie_break(void)
{
    tie_break_case(1, "tie-break (plane inserted first): build + hit");
    tie_break_case(0, "tie-break (sphere inserted first): build + hit");
}

/* ------------------------------------------------------------------ */
/* 5. A single plane                                                   */
/* ------------------------------------------------------------------ */

static void test_single_plane(void)
{
    Geometry g;
    geometry_init(&g);
    geometry_add(&g, prim_plane(vec3(0.0, 2.0, 0.0), vec3(0.0, 1.0, 0.0), 61));

    Bvh *b = bvh_build(&g);
    CHECK(b != NULL, "single-plane: bvh_build succeeds");
    CHECK(bvh_node_count(b) == 0, "single-plane: node_count == 0");
    if (!b) { geometry_free(&g); return; }

    Ray r;
    r.origin = vec3(1.0, 7.0, -3.0);
    r.dir = vec3(0.0, -1.0, 0.0);
    Hit h;
    CHECK(bvh_intersect(b, &g, r, TMIN, TMAX, &h) == 1, "single-plane: hit found");
    CHECK(h.prim_index == 0, "single-plane: prim_index == 0");
    CHECK(h.material_index == 61, "single-plane: material reported");
    CHECK_NEAR(h.t, 5.0, 1e-12, "single-plane: t == 5");

    /* Parallel ray must miss. */
    Ray rp = r;
    rp.dir = vec3(1.0, 0.0, 0.0);
    CHECK(bvh_intersect(b, &g, rp, TMIN, TMAX, &h) == 0,
          "single-plane: parallel ray misses");

    bvh_free(b);
    geometry_free(&g);
}

/* ------------------------------------------------------------------ */
/* 6. Randomized mixed scene including planes                          */
/* ------------------------------------------------------------------ */

#define MIX_PRIMS 200
#define MIX_RAYS  4000

static void test_mixed_random_scene(void)
{
    Geometry g;
    geometry_init(&g);

    for (int i = 0; i < MIX_PRIMS; ++i) {
        int kind = (int)(xorshift64() % 5u);
        int mat = i % 8;
        double cx = rnd_range(-6.0, 6.0);
        double cy = rnd_range(-6.0, 6.0);
        double cz = rnd_range(-6.0, 6.0);

        if (kind == 0) {
            geometry_add(&g, prim_sphere(vec3(cx, cy, cz), rnd_range(0.2, 1.5), mat));
        } else if (kind == 1) {
            Vec3 half = vec3(rnd_range(0.1, 1.2), rnd_range(0.1, 1.2),
                             rnd_range(0.1, 1.2));
            geometry_add(&g, prim_box(vec3(cx, cy, cz), half, mat));
        } else if (kind == 2) {
            Vec3 a = vec3(cx, cy, cz);
            Vec3 b = vec3(cx + rnd_range(-2.0, 2.0), cy + rnd_range(-2.0, 2.0),
                          cz + rnd_range(-2.0, 2.0));
            Vec3 c = vec3(cx + rnd_range(-2.0, 2.0), cy + rnd_range(-2.0, 2.0),
                          cz + rnd_range(-2.0, 2.0));
            geometry_add(&g, prim_triangle(a, b, c, mat));
        } else if (kind == 3) {
            Vec3 base = vec3(cx, cy, cz);
            Vec3 top = vec3(cx + rnd_range(-1.0, 1.0), cy + rnd_range(-1.0, 1.0),
                            cz + rnd_range(-1.0, 1.0));
            geometry_add(&g, prim_cylinder(base, top, rnd_range(0.1, 1.2),
                                           rnd_range(0.1, 1.2), mat));
        } else {
            Vec3 nrm = vec3(rnd_range(-1.0, 1.0), rnd_range(-1.0, 1.0),
                            rnd_range(-1.0, 1.0));
            if (vec3_length_sq(nrm) < 1e-6) nrm = vec3(0.0, 1.0, 0.0);
            geometry_add(&g, prim_plane(vec3(cx, cy, cz), nrm, mat));
        }
    }

    /* Count planes actually inserted so the test is meaningful. */
    int planes = 0;
    for (int i = 0; i < g.count; ++i)
        if (g.prims[i].kind == PRIM_PLANE) ++planes;
    CHECK(planes > 0, "mixed: scene contains planes");
    CHECK(planes < g.count, "mixed: scene contains non-plane primitives");

    Bvh *b = bvh_build(&g);
    CHECK(b != NULL, "mixed: bvh_build succeeds");
    if (!b) { geometry_free(&g); return; }

    int total = 0, mism = 0, hits = 0;
    for (int i = 0; i < MIX_RAYS; ++i) {
        Ray rr;
        rr.origin = vec3(rnd_range(-10.0, 10.0), rnd_range(-10.0, 10.0),
                         rnd_range(-10.0, 10.0));
        Vec3 d = vec3(rnd_range(-1.0, 1.0), rnd_range(-1.0, 1.0),
                      rnd_range(-1.0, 1.0));
        if (vec3_length_sq(d) < 1e-9) continue;
        rr.dir = vec3_normalize(d);
        ++total;
        mism += compare_ray(&g, b, rr, &hits);
    }
    CHECK(mism == 0, "mixed: bvh == linear (prim/material/front_face/t) for every ray");
    CHECK(hits > 0, "mixed: random rays produced at least one hit");

    printf("tests/test_bvh_planes: mixed scene (%d prims, %d planes) over %d rays "
           "(%d hits, %d mismatches)\n", g.count, planes, total, hits, mism);

    bvh_free(b);
    geometry_free(&g);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_plane_nearer_than_tree();
    test_all_plane_geometry();
    test_no_plane_geometry();
    test_tie_break();
    test_single_plane();
    test_mixed_random_scene();

    printf("tests/test_bvh_planes: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
