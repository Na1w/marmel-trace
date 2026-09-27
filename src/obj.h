#ifndef RAYTRACER_OBJ_H
#define RAYTRACER_OBJ_H

/*
 * obj.h - Wavefront .obj 3D mesh loader.
 *
 * Supports:
 *   - Vertex coordinates: v x y z [w]
 *   - Vertex normals: vn x y z
 *   - Texture coordinates: vt u [v [w]]
 *   - Polygonal faces with arbitrary vertex counts:
 *       f v1 v2 v3 ...
 *       f v1/vt1 v2/vt2 ...
 *       f v1//vn1 v2//vn2 ...
 *       f v1/vt1/vn1 v2/vt2/vn2 ...
 *     (triangulated via fan triangulation)
 *   - Negative relative indexing (e.g. -1 means most recent vertex)
 *   - Linear transforms: scaling, Euler rotation (in degrees), translation
 *   - Auto-centering and auto-scaling to target size
 *   - Smooth vertex normal interpolation (Phong shading)
 *   - Integration into Scene / BVH via prim_triangle / prim_triangle_smooth
 */

#include "vec3.h"
#include "geometry.h"
#include <stddef.h>

typedef struct {
    Vec3   position;      /* translation / placement center */
    Vec3   scale;         /* scale factors along X, Y, Z (default: 1, 1, 1) */
    Vec3   rotation_deg;  /* Euler rotation angles in degrees around X, Y, Z */
    int    smooth_normals;/* 1: interpolate vn normals; 0: force flat face normals */
    int    auto_center;   /* 1: center model's bounding box at (0,0,0) before position */
    double auto_scale;    /* if > 0.0, scale model bounding box max dimension to this size */
} ObjTransform;

/* Returns default transform: position=0, scale=1, rotation=0, smooth=1, auto_center=0, auto_scale=0 */
ObjTransform obj_transform_default(void);

/*
 * Load an OBJ model from a file path and append its triangles to Geometry.
 *
 * Parameters:
 *   path           - path to the .obj file
 *   g              - target Geometry array to append triangles to
 *   material_index - material assigned to the loaded mesh
 *   xform          - optional transformation (NULL for default)
 *
 * Returns:
 *   >= 0 : number of triangles successfully appended
 *   < 0  : error code (-1: cannot open file, -2: parse/memory error)
 */
int obj_load_file(const char *path, Geometry *g, int material_index, const ObjTransform *xform);

/*
 * Load an OBJ model from a memory buffer.
 */
int obj_load_mem(const char *data, size_t len, Geometry *g, int material_index, const ObjTransform *xform);

#endif /* RAYTRACER_OBJ_H */
