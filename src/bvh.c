/*
 * bvh.c - Binned-SAH bounding volume hierarchy over Geometry primitives.
 *
 * Design:
 *   - Nodes are stored in a flat, POD array (`BvhNode nodes[]`). A node is a
 *     leaf when `count > 0`; otherwise it is an interior node with child
 *     indices `left` and `right`. `first` is the offset into the primitive
 *     index array (`order`) for leaves and is unused (0) for interior nodes.
 *     This explicit leaf encoding keeps traversal branch-predictable and the
 *     struct small (two AABBs' worth of doubles + four ints).
 *   - The BVH references primitives by index into Geometry.prims; nothing is
 *     copied. A permutation array `order[]` reorders indices during build.
 *   - Build: binned SAH over 12 bins along the largest axis of the node's
 *     centroid bounds; if SAH does not beat a leaf, fall back to a median
 *     split (by the same axis) to guarantee termination. Leaves hold at most
 *     BVH_LEAF_SIZE primitives.
 *   - PRIM_PLANE primitives are NOT placed in the SAH tree: their bounds are
 *     a huge [-1e4,1e4]^3 box (see bounds_plane) that destroys split quality.
 *     Instead their indices are collected in `planes[]` and tested linearly
 *     during traversal, combined with the tree result under the exact same
 *     nearest-hit + lowest-prim_index tie-break as geometry_intersect().
 *   - Traversal: iterative with a fixed on-C-stack array of BVH_STACK_FIXED
 *     entries (zero heap traffic on the normal path), with a malloc'd fallback
 *     only if a pathological tree ever exceeds that capacity. Children are
 *     pushed FARTHER-first so the NEARER child is traversed first, enabling
 *     early tightening of the closest distance and culling of the far child.
 *     Ray-AABB slab test computes `1/dir` once per ray and relies on IEEE
 *     infinities (with a NaN-safe parallel-ray branch) so division by zero is
 *     safe.
 *
 * Depends only on geometry.h. C11, -Wall -Wextra clean, no I/O, no globals.
 */

#include "bvh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Maximum primitives in a leaf. */
#define BVH_LEAF_SIZE 4
/* Number of SAH bins along the chosen split axis. */
#define BVH_NBINS 12
/* Pad degenerate/zero-extent bounds so slab tests do not fail spuriously. */
#define BVH_BOUNDS_EPS 1e-7
/* Fixed traversal stack capacity. 64 levels is far more than any real scene
 * reaches; the malloc fallback below covers pathological cases. */
#define BVH_STACK_FIXED 64

typedef struct {
    Vec3 bounds_min;
    Vec3 bounds_max;
    int left;    /* interior: child index (>= 0); leaf: -1 */
    int right;   /* interior: child index (>= 0); leaf: -1 */
    int first;   /* leaf: offset into order[]; interior: 0 */
    int count;   /* leaf: number of primitives (> 0); interior: 0 */
} BvhNode;

/* Traversal stack entry: node index plus the ray entry distance of that
 * node's AABB (computed once at push time, reused for culling at pop time). */
typedef struct {
    int idx;
    double t;
} BvhStackEntry;

struct Bvh {
    BvhNode *nodes;
    int node_count;
    int node_capacity;
    int *order;      /* permutation of primitive indices (tree prims only) */
    int order_count;
    int depth;
    int *planes;     /* indices of PRIM_PLANE prims, excluded from the tree */
    int plane_count;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static double bvh_axis(const Vec3 *v, int axis)
{
    switch (axis) {
    case 0:  return v->x;
    case 1:  return v->y;
    default: return v->z;
    }
}

/* Surface area of an AABB; 0 for a fully degenerate box. */
static double bvh_surface_area(Vec3 mn, Vec3 mx)
{
    double dx = mx.x - mn.x;
    double dy = mx.y - mn.y;
    double dz = mx.z - mn.z;
    if (dx < 0.0) dx = 0.0;
    if (dy < 0.0) dy = 0.0;
    if (dz < 0.0) dz = 0.0;
    return 2.0 * (dx * dy + dy * dz + dz * dx);
}

static Vec3 bvh_bounds_extent(Vec3 mn, Vec3 mx)
{
    return vec3(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
}

/* ------------------------------------------------------------------ */
/* Node allocation                                                     */
/* ------------------------------------------------------------------ */

static int bvh_reserve_nodes(Bvh *b, int extra)
{
    if (b->node_count + extra <= b->node_capacity) return 1;

    int cap = b->node_capacity ? b->node_capacity : 64;
    while (cap < b->node_count + extra) cap *= 2;

    BvhNode *nn = (BvhNode *)realloc(b->nodes, (size_t)cap * sizeof(BvhNode));
    if (!nn) return 0;
    b->nodes = nn;
    b->node_capacity = cap;
    return 1;
}

/* Append a node, returning its index, or -1 on allocation failure. */
static int bvh_push_node(Bvh *b, BvhNode node)
{
    if (!bvh_reserve_nodes(b, 1)) return -1;
    int idx = b->node_count++;
    b->nodes[idx] = node;
    return idx;
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

/* Compute bounds + centroid bounds over a range of primitive indices. */
static void bvh_range_info(const Geometry *g, const int *order, int first, int count,
                           Vec3 *out_min, Vec3 *out_max,
                           Vec3 *cent_min, Vec3 *cent_max)
{
    Vec3 mn = vec3(1e300, 1e300, 1e300);
    Vec3 mx = vec3(-1e300, -1e300, -1e300);
    Vec3 cmn = mn;
    Vec3 cmx = mx;

    for (int i = 0; i < count; ++i) {
        Vec3 pmin, pmax;
        primitive_bounds(&g->prims[order[first + i]], &pmin, &pmax);
        mn = vec3_min(mn, pmin);
        mx = vec3_max(mx, pmax);
        Vec3 c = vec3_scale(vec3_add(pmin, pmax), 0.5);
        cmn = vec3_min(cmn, c);
        cmx = vec3_max(cmx, c);
    }

    *out_min = vec3(mn.x - BVH_BOUNDS_EPS, mn.y - BVH_BOUNDS_EPS, mn.z - BVH_BOUNDS_EPS);
    *out_max = vec3(mx.x + BVH_BOUNDS_EPS, mx.y + BVH_BOUNDS_EPS, mx.z + BVH_BOUNDS_EPS);
    *cent_min = cmn;
    *cent_max = cmx;
}

/* Make a leaf node spanning [first, first+count) of order[]. */
static int bvh_make_leaf(Bvh *b, Vec3 mn, Vec3 mx, int first, int count)
{
    BvhNode leaf;
    leaf.bounds_min = mn;
    leaf.bounds_max = mx;
    leaf.left = -1;
    leaf.right = -1;
    leaf.first = first;
    leaf.count = count;
    return bvh_push_node(b, leaf);
}

/* Recursively build the subtree over order[first .. first+count).
 * Returns the node index, or -1 on failure. `depth` tracks recursion depth. */
static int bvh_build_node(Bvh *b, const Geometry *g, int first, int count, int depth)
{
    if (depth > b->depth) b->depth = depth;

    Vec3 mn, mx, cmn, cmx;
    bvh_range_info(g, b->order, first, count, &mn, &mx, &cmn, &cmx);

    /* Leaf cases: small range, or no spatial spread to split on. */
    if (count <= BVH_LEAF_SIZE) {
        return bvh_make_leaf(b, mn, mx, first, count);
    }

    Vec3 extent = bvh_bounds_extent(cmn, cmx);
    double ex = extent.x, ey = extent.y, ez = extent.z;
    int axis = 0;
    double max_ext = ex;
    if (ey > max_ext) { max_ext = ey; axis = 1; }
    if (ez > max_ext) { max_ext = ez; axis = 2; }

    /* All centroids coincident -> cannot split by SAH; median split is also
     * meaningless (all equal), so emit a leaf and stop. */
    if (!(max_ext > 0.0)) {
        return bvh_make_leaf(b, mn, mx, first, count);
    }

    double cmin = bvh_axis(&cmn, axis);
    double cmax = bvh_axis(&cmx, axis);
    double scale = BVH_NBINS / max_ext;

    /* Bin primitives by centroid along `axis`. */
    Vec3 bmin[BVH_NBINS], bmax[BVH_NBINS];
    int bcount[BVH_NBINS];
    for (int i = 0; i < BVH_NBINS; ++i) {
        bmin[i] = vec3(1e300, 1e300, 1e300);
        bmax[i] = vec3(-1e300, -1e300, -1e300);
        bcount[i] = 0;
    }

    for (int i = 0; i < count; ++i) {
        int pi = b->order[first + i];
        Vec3 pmin, pmax;
        primitive_bounds(&g->prims[pi], &pmin, &pmax);
        Vec3 c = vec3_scale(vec3_add(pmin, pmax), 0.5);
        double cv = bvh_axis(&c, axis);
        int bin = (int)((cv - cmin) * scale);
        if (bin < 0) bin = 0;
        if (bin >= BVH_NBINS) bin = BVH_NBINS - 1;
        bcount[bin]++;
        bmin[bin] = vec3_min(bmin[bin], pmin);
        bmax[bin] = vec3_max(bmax[bin], pmax);
    }

    /* Sweep from the right: suffix bounds and counts. */
    Vec3 rmin[BVH_NBINS], rmax[BVH_NBINS];
    int rcount[BVH_NBINS];
    Vec3 acc_min = vec3(1e300, 1e300, 1e300);
    Vec3 acc_max = vec3(-1e300, -1e300, -1e300);
    int acc_n = 0;
    for (int i = BVH_NBINS - 1; i >= 0; --i) {
        if (bcount[i] > 0) {
            acc_min = vec3_min(acc_min, bmin[i]);
            acc_max = vec3_max(acc_max, bmax[i]);
            acc_n += bcount[i];
        }
        rmin[i] = acc_min;
        rmax[i] = acc_max;
        rcount[i] = acc_n;
    }

    /* Sweep from the left, evaluating SAH cost at each bin boundary. */
    Vec3 lacc_min = vec3(1e300, 1e300, 1e300);
    Vec3 lacc_max = vec3(-1e300, -1e300, -1e300);
    int lacc_n = 0;
    double best_cost = 1e300;
    int best_split = -1;
    for (int i = 0; i < BVH_NBINS - 1; ++i) {
        if (bcount[i] > 0) {
            lacc_min = vec3_min(lacc_min, bmin[i]);
            lacc_max = vec3_max(lacc_max, bmax[i]);
            lacc_n += bcount[i];
        }
        int rn = rcount[i + 1];
        if (lacc_n == 0 || rn == 0) continue;

        double la = bvh_surface_area(lacc_min, lacc_max);
        double ra = bvh_surface_area(rmin[i + 1], rmax[i + 1]);
        double cost = la * (double)lacc_n + ra * (double)rn;
        if (cost < best_cost) {
            best_cost = cost;
            best_split = i;
        }
    }

    /* Compare SAH split cost against the "make a leaf" cost (surface area *
     * count). If no split is cheaper, fall back to a median split. */
    double leaf_area = bvh_surface_area(mn, mx);
    double leaf_cost = leaf_area * (double)count;
    int use_split = -1;
    int median_mid = -1;

    if (best_split >= 0 && best_cost < leaf_cost) {
        use_split = best_split;
    }

    if (use_split < 0) {
        /* Median split by centroid: stable partition around the midpoint.
         * Guarantees progress because not all centroids are equal here. */
        double mid = 0.5 * (cmin + cmax);
        int i = first, j = first + count - 1;
        while (i <= j) {
            int pi = b->order[i];
            Vec3 pmin, pmax;
            primitive_bounds(&g->prims[pi], &pmin, &pmax);
            Vec3 c = vec3_scale(vec3_add(pmin, pmax), 0.5);
            double cv = bvh_axis(&c, axis);
            if (cv < mid) {
                ++i;
            } else {
                int tmp = b->order[i];
                b->order[i] = b->order[j];
                b->order[j] = tmp;
                --j;
            }
        }
        median_mid = i;
        if (median_mid <= first || median_mid >= first + count) {
            median_mid = first + count / 2;
        }
    }

    int mid_index;
    if (use_split >= 0) {
        /* Reorder order[] by the SAH partition: bins [0..best_split] left,
         * bins [best_split+1 .. NBINS-1] right. */
        int i = first, j = first + count - 1;
        while (i <= j) {
            int pi = b->order[i];
            Vec3 pmin, pmax;
            primitive_bounds(&g->prims[pi], &pmin, &pmax);
            Vec3 c = vec3_scale(vec3_add(pmin, pmax), 0.5);
            double cv = bvh_axis(&c, axis);
            int bin = (int)((cv - cmin) * scale);
            if (bin < 0) bin = 0;
            if (bin >= BVH_NBINS) bin = BVH_NBINS - 1;
            if (bin <= best_split) {
                ++i;
            } else {
                int tmp = b->order[i];
                b->order[i] = b->order[j];
                b->order[j] = tmp;
                --j;
            }
        }
        mid_index = i;
        if (mid_index == first || mid_index == first + count) {
            mid_index = first + count / 2;
        }
    } else {
        mid_index = median_mid;
        if (mid_index == first || mid_index == first + count) {
            mid_index = first + count / 2;
        }
    }

    int left_count = mid_index - first;
    int right_count = count - left_count;
    if (left_count <= 0 || right_count <= 0) {
        /* Absolute last-resort guard: should be unreachable. */
        return bvh_make_leaf(b, mn, mx, first, count);
    }

    int this_idx = bvh_push_node(b, (BvhNode){ mn, mx, -1, -1, 0, 0 });
    if (this_idx < 0) return -1;

    int left_idx = bvh_build_node(b, g, first, left_count, depth + 1);
    if (left_idx < 0) return -1;
    int right_idx = bvh_build_node(b, g, mid_index, right_count, depth + 1);
    if (right_idx < 0) return -1;

    b->nodes[this_idx].left = left_idx;
    b->nodes[this_idx].right = right_idx;
    return this_idx;
}

Bvh *bvh_build(const Geometry *g)
{
    if (!g || g->count <= 0 || !g->prims) return NULL;

    Bvh *b = (Bvh *)calloc(1, sizeof(Bvh));
    if (!b) return NULL;

    /* Partition primitive indices into tree prims (`order`) and planes
     * (`planes`). Planes are kept out of the SAH tree because their huge
     * AABB would dominate every split; they are tested linearly instead. */
    int tree_count = 0;
    int plane_count = 0;
    for (int i = 0; i < g->count; ++i) {
        if (g->prims[i].kind == PRIM_PLANE) ++plane_count;
        else ++tree_count;
    }

    if (tree_count > 0) {
        b->order = (int *)malloc((size_t)tree_count * sizeof(int));
        if (!b->order) {
            bvh_free(b);
            return NULL;
        }
    }
    if (plane_count > 0) {
        b->planes = (int *)malloc((size_t)plane_count * sizeof(int));
        if (!b->planes) {
            bvh_free(b);
            return NULL;
        }
    }

    int ti = 0, pi = 0;
    for (int i = 0; i < g->count; ++i) {
        if (g->prims[i].kind == PRIM_PLANE) b->planes[pi++] = i;
        else b->order[ti++] = i;
    }
    b->order_count = tree_count;
    b->plane_count = plane_count;

    /* All-plane (or otherwise plane-only) geometry has no tree to build.
     * This is valid: bvh_intersect handles node_count == 0 by testing the
     * plane list alone. */
    if (tree_count > 0) {
        int root = bvh_build_node(b, g, 0, tree_count, 1);
        if (root < 0) {
            bvh_free(b);
            return NULL;
        }
    }

    return b;
}

void bvh_free(Bvh *b)
{
    if (!b) return;
    free(b->nodes);
    free(b->order);
    free(b->planes);
    free(b);
}

/* ------------------------------------------------------------------ */
/* Traversal                                                           */
/* ------------------------------------------------------------------ */

/* Ray-AABB slab test. Computes the entry distance along the ray and reports
 * whether the box is hit within [tmin, tmax]. `inv` holds 1/dir per axis.
 * Returns 1 on hit and writes the entry distance to *t_enter. */
static int bvh_slab(Vec3 mn, Vec3 mx, Vec3 origin, Vec3 inv,
                    double tmin, double tmax, double *t_enter)
{
    /* fmin/fmax return the non-NaN operand, which makes the `0 * inf = NaN`
     * case (parallel ray exactly on a slab boundary) well behaved. */
    double t1x = (mn.x - origin.x) * inv.x;
    double t2x = (mx.x - origin.x) * inv.x;
    double txmin = fmin(t1x, t2x);
    double txmax = fmax(t1x, t2x);

    double t1y = (mn.y - origin.y) * inv.y;
    double t2y = (mx.y - origin.y) * inv.y;
    double tymin = fmin(t1y, t2y);
    double tymax = fmax(t1y, t2y);

    double t1z = (mn.z - origin.z) * inv.z;
    double t2z = (mx.z - origin.z) * inv.z;
    double tzmin = fmin(t1z, t2z);
    double tzmax = fmax(t1z, t2z);

    double enter = fmax(fmax(txmin, tymin), tzmin);
    double exit = fmin(fmin(txmax, tymax), tzmax);

    if (exit < tmin || enter > tmax) return 0;
    if (enter < tmin) enter = tmin;
    *t_enter = enter;
    return 1;
}

int bvh_intersect(const Bvh *b, const Geometry *g, Ray r, double tmin, double tmax, Hit *out)
{
    if (!b || !g || !out) return 0;

    /* Normalize the direction once so `t` is a distance (matches
     * primitive_intersect, which normalizes internally as well). */
    double len_sq = vec3_length_sq(r.dir);
    if (len_sq > 0.0 && fabs(len_sq - 1.0) > 1e-12) {
        r.dir = vec3_normalize(r.dir);
    }

    Vec3 inv = vec3(1.0 / r.dir.x, 1.0 / r.dir.y, 1.0 / r.dir.z);

    int found = 0;
    double closest = tmax;
    Hit best;

    /* Planes are not part of the tree (their huge AABB would ruin SAH), so
     * test them linearly first. This both lets a plane nearer than any tree
     * hit win AND tightens `closest` so the tree traversal below prunes with
     * the running best distance. The nearest-hit + lowest-prim_index
     * tie-break mirrors geometry_intersect() exactly. */
    for (int i = 0; i < b->plane_count; ++i) {
        int pi = b->planes[i];
        Hit h;
        h.prim_index = pi;
        if (primitive_intersect(&g->prims[pi], r, tmin, closest, &h)) {
            if (!found || h.t < best.t ||
                (h.t == best.t && h.prim_index < best.prim_index)) {
                best = h;
                closest = h.t;
                found = 1;
            }
        }
    }

    /* Tree traversal over the non-plane primitives. The stack lives on the C
     * stack (zero heap traffic on the normal path); a malloc'd buffer is only
     * allocated if a pathological tree ever exceeds BVH_STACK_FIXED levels,
     * so no node is ever silently dropped. */
    if (b->node_count > 0) {
        BvhStackEntry fixed[BVH_STACK_FIXED];
        BvhStackEntry *stack = fixed;
        int cap = BVH_STACK_FIXED;
        int sp = 0;

        double t_root;
        if (bvh_slab(b->nodes[0].bounds_min, b->nodes[0].bounds_max, r.origin,
                     inv, tmin, closest, &t_root)) {
            stack[sp].idx = 0;
            stack[sp].t = t_root;
            ++sp;
        }

        while (sp > 0) {
            BvhStackEntry e = stack[--sp];

            /* Cull using the running closest distance. The stored entry `t` is
             * a lower bound on any hit inside the node. Use a STRICT `>` so a
             * node whose entry distance equals `closest` is still visited: a
             * primitive exactly at `closest` with a LOWER prim_index must win
             * the tie-break, matching geometry_intersect(). */
            if (found && e.t > closest) continue;

            const BvhNode *node = &b->nodes[e.idx];

            if (node->count > 0) {
                for (int i = 0; i < node->count; ++i) {
                    int pi = b->order[node->first + i];
                    Hit h;
                    h.prim_index = pi;
                    if (primitive_intersect(&g->prims[pi], r, tmin, closest, &h)) {
                        /* Tie-break on equal t by LOWEST prim_index so that the
                         * result is identical to geometry_intersect(), which
                         * scans in index order with a strict `h.t < best.t`
                         * comparison. Traversal order is permuted by the
                         * build's `order[]` (and by the front-to-back child
                         * ordering below), so without this, coincident
                         * primitives split across leaves would resolve to
                         * whichever leaf is visited first. */
                        if (!found || h.t < best.t ||
                            (h.t == best.t && h.prim_index < best.prim_index)) {
                            best = h;
                            closest = h.t;
                            found = 1;
                        }
                    }
                }
            } else {
                /* Front-to-back ordering: compute the entry distance of both
                 * child AABBs, then push the FARTHER child first so the NEARER
                 * child is popped (and traversed) first. This tightens
                 * `closest` early and lets the far child be culled at pop. A
                 * child whose AABB is missed is simply not pushed. */
                double tl = 0.0, tr = 0.0;
                int hl = bvh_slab(b->nodes[node->left].bounds_min,
                                  b->nodes[node->left].bounds_max,
                                  r.origin, inv, tmin, closest, &tl);
                int hr = bvh_slab(b->nodes[node->right].bounds_min,
                                  b->nodes[node->right].bounds_max,
                                  r.origin, inv, tmin, closest, &tr);

                if (hl || hr) {
                    /* Ensure room for up to two pushes (never overflow the
                     * fixed array). Only reached in pathological trees. */
                    if (sp + 2 > cap) {
                        int ncap = cap * 2;
                        BvhStackEntry *ns;
                        if (stack == fixed) {
                            ns = (BvhStackEntry *)malloc(
                                (size_t)ncap * sizeof(BvhStackEntry));
                            if (ns)
                                memcpy(ns, fixed,
                                       (size_t)sp * sizeof(BvhStackEntry));
                        } else {
                            ns = (BvhStackEntry *)realloc(
                                stack, (size_t)ncap * sizeof(BvhStackEntry));
                        }
                        if (!ns) {
                            /* Allocation failure: stop traversal cleanly.
                             * `best` (planes + any leaves already tested) is
                             * still a valid nearest hit. */
                            break;
                        }
                        stack = ns;
                        cap = ncap;
                    }

                    if (hl && hr) {
                        if (tl <= tr) {
                            /* left is nearer -> push right first. */
                            stack[sp].idx = node->right; stack[sp].t = tr; ++sp;
                            stack[sp].idx = node->left;  stack[sp].t = tl; ++sp;
                        } else {
                            stack[sp].idx = node->left;  stack[sp].t = tl; ++sp;
                            stack[sp].idx = node->right; stack[sp].t = tr; ++sp;
                        }
                    } else if (hl) {
                        stack[sp].idx = node->left; stack[sp].t = tl; ++sp;
                    } else {
                        stack[sp].idx = node->right; stack[sp].t = tr; ++sp;
                    }
                }
            }
        }

        if (stack != fixed) free(stack);
    }

    if (!found) return 0;
    *out = best;
    return 1;
}

int bvh_node_count(const Bvh *b)
{
    return b ? b->node_count : 0;
}

int bvh_depth(const Bvh *b)
{
    return b ? b->depth : 0;
}
