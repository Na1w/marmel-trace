/*
 * obj.c - Wavefront .obj 3D mesh loader implementation.
 */

#include "obj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ObjTransform obj_transform_default(void)
{
    ObjTransform xf;
    xf.position = vec3(0.0, 0.0, 0.0);
    xf.scale = vec3(1.0, 1.0, 1.0);
    xf.rotation_deg = vec3(0.0, 0.0, 0.0);
    xf.smooth_normals = 1;
    xf.auto_center = 0;
    xf.auto_scale = 0.0;
    return xf;
}

static int grow_vec3_array(Vec3 **arr, int *cap, int count)
{
    if (count < *cap) return 0;
    int new_cap = (*cap > 0) ? (*cap * 2) : 256;
    Vec3 *grown = (Vec3 *)realloc(*arr, (size_t)new_cap * sizeof(Vec3));
    if (!grown) return -1;
    *arr = grown;
    *cap = new_cap;
    return 0;
}

static Vec3 rotate_euler(Vec3 v, Vec3 rot_deg)
{
    if (fabs(rot_deg.x) < 1e-9 && fabs(rot_deg.y) < 1e-9 && fabs(rot_deg.z) < 1e-9) {
        return v;
    }
    double rx = rot_deg.x * (M_PI / 180.0);
    double ry = rot_deg.y * (M_PI / 180.0);
    double rz = rot_deg.z * (M_PI / 180.0);

    /* Rotate X */
    double cx = cos(rx), sx = sin(rx);
    Vec3 v1 = vec3(v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx);

    /* Rotate Y */
    double cy = cos(ry), sy = sin(ry);
    Vec3 v2 = vec3(v1.x * cy + v1.z * sy, v1.y, -v1.x * sy + v1.z * cy);

    /* Rotate Z */
    double cz = cos(rz), sz = sin(rz);
    return vec3(v2.x * cz - v2.y * sz, v2.x * sz + v2.y * cz, v2.z);
}

static Vec3 transform_normal(Vec3 n, Vec3 scale, Vec3 rot_deg)
{
    Vec3 sn = vec3(
        fabs(scale.x) > 1e-12 ? n.x / scale.x : n.x,
        fabs(scale.y) > 1e-12 ? n.y / scale.y : n.y,
        fabs(scale.z) > 1e-12 ? n.z / scale.z : n.z
    );
    Vec3 rn = rotate_euler(sn, rot_deg);
    double len = vec3_length(rn);
    if (len > 1e-12) {
        return vec3_scale(rn, 1.0 / len);
    }
    return rn;
}

static Vec3 transform_position(Vec3 v, Vec3 center_offset, double auto_scale_factor,
                               Vec3 scale, Vec3 rot_deg, Vec3 pos)
{
    Vec3 p = vec3_sub(v, center_offset);
    p = vec3_scale(p, auto_scale_factor);
    p = vec3(p.x * scale.x, p.y * scale.y, p.z * scale.z);
    p = rotate_euler(p, rot_deg);
    p = vec3_add(p, pos);
    return p;
}

static int resolve_index(int idx, int count)
{
    if (idx > 0) return idx - 1;
    if (idx < 0) return count + idx;
    return -1;
}

typedef struct {
    int vi;   /* vertex index (0-based) */
    int vti;  /* texcoord index (0-based) */
    int vni;  /* normal index (0-based) */
} FaceVertex;

static int parse_face_vertex(const char *tok, int *v_out, int *vt_out, int *vn_out)
{
    *v_out = 0;
    *vt_out = 0;
    *vn_out = 0;
    char *endptr = NULL;
    long v = strtol(tok, &endptr, 10);
    *v_out = (int)v;
    if (endptr && *endptr == '/') {
        endptr++;
        if (*endptr != '/') {
            long vt = strtol(endptr, &endptr, 10);
            *vt_out = (int)vt;
        }
        if (endptr && *endptr == '/') {
            endptr++;
            long vn = strtol(endptr, &endptr, 10);
            *vn_out = (int)vn;
        }
    }
    return (*v_out != 0);
}

int obj_load_mem(const char *data, size_t len, Geometry *g, int material_index, const ObjTransform *xform)
{
    if (!data || len == 0 || !g) return -1;

    ObjTransform xf = xform ? *xform : obj_transform_default();

    Vec3 *positions = NULL;
    int   pos_count = 0, pos_cap = 0;

    Vec3 *normals = NULL;
    int   norm_count = 0, norm_cap = 0;

    Vec3 bmin = vec3(1e30, 1e30, 1e30);
    Vec3 bmax = vec3(-1e30, -1e30, -1e30);

    /* --- PASS 1: Parse vertices (v) and normals (vn) --- */
    const char *p = data;
    const char *end = data + len;

    while (p < end) {
        /* Find end of line */
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = eol ? (size_t)(eol - p) : (size_t)(end - p);

        /* Skip leading whitespace */
        const char *line = p;
        while (line < p + line_len && isspace((unsigned char)*line)) {
            line++;
        }

        if (line < p + line_len && *line != '#') {
            if (line[0] == 'v' && isspace((unsigned char)line[1])) {
                double x, y, z;
                if (sscanf(line + 2, "%lf %lf %lf", &x, &y, &z) == 3) {
                    if (grow_vec3_array(&positions, &pos_cap, pos_count) == 0) {
                        Vec3 v = vec3(x, y, z);
                        positions[pos_count++] = v;
                        bmin.x = fmin(bmin.x, x);
                        bmin.y = fmin(bmin.y, y);
                        bmin.z = fmin(bmin.z, z);
                        bmax.x = fmax(bmax.x, x);
                        bmax.y = fmax(bmax.y, y);
                        bmax.z = fmax(bmax.z, z);
                    }
                }
            } else if (line[0] == 'v' && line[1] == 'n' && isspace((unsigned char)line[2])) {
                double nx, ny, nz;
                if (sscanf(line + 3, "%lf %lf %lf", &nx, &ny, &nz) == 3) {
                    if (grow_vec3_array(&normals, &norm_cap, norm_count) == 0) {
                        normals[norm_count++] = vec3(nx, ny, nz);
                    }
                }
            }
        }

        p += line_len + (eol ? 1 : 0);
    }

    if (pos_count == 0) {
        free(positions);
        free(normals);
        return 0;
    }

    /* Compute auto-center offset and auto-scale factor */
    Vec3 center_offset = vec3(0.0, 0.0, 0.0);
    if (xf.auto_center) {
        center_offset = vec3_scale(vec3_add(bmin, bmax), 0.5);
    }

    double auto_scale_factor = 1.0;
    if (xf.auto_scale > 0.0) {
        double dx = bmax.x - bmin.x;
        double dy = bmax.y - bmin.y;
        double dz = bmax.z - bmin.z;
        double max_dim = fmax(dx, fmax(dy, dz));
        if (max_dim > 1e-9) {
            auto_scale_factor = xf.auto_scale / max_dim;
        }
    }

    /* --- PASS 2: Parse faces (f) and emit triangles --- */
    int triangles_loaded = 0;
    p = data;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = eol ? (size_t)(eol - p) : (size_t)(end - p);

        const char *line = p;
        while (line < p + line_len && isspace((unsigned char)*line)) {
            line++;
        }

        if (line < p + line_len && line[0] == 'f' && isspace((unsigned char)line[1])) {
            char line_buf[1024];
            size_t copy_len = (line_len < sizeof(line_buf) - 1) ? line_len : sizeof(line_buf) - 1;
            memcpy(line_buf, p, copy_len);
            line_buf[copy_len] = '\0';

            FaceVertex fverts[64];
            int fcount = 0;

            char *tok = strtok(line_buf + 2, " \t\r\n");
            while (tok && fcount < 64) {
                int vi = 0, vti = 0, vni = 0;
                if (parse_face_vertex(tok, &vi, &vti, &vni)) {
                    fverts[fcount].vi = resolve_index(vi, pos_count);
                    fverts[fcount].vti = resolve_index(vti, 0);
                    fverts[fcount].vni = resolve_index(vni, norm_count);
                    if (fverts[fcount].vi >= 0 && fverts[fcount].vi < pos_count) {
                        fcount++;
                    }
                }
                tok = strtok(NULL, " \t\r\n");
            }

            /* Fan triangulation for polygons with >= 3 vertices */
            for (int i = 1; i < fcount - 1; i++) {
                int ia = fverts[0].vi;
                int ib = fverts[i].vi;
                int ic = fverts[i + 1].vi;

                Vec3 va = transform_position(positions[ia], center_offset, auto_scale_factor,
                                             xf.scale, xf.rotation_deg, xf.position);
                Vec3 vb = transform_position(positions[ib], center_offset, auto_scale_factor,
                                             xf.scale, xf.rotation_deg, xf.position);
                Vec3 vc = transform_position(positions[ic], center_offset, auto_scale_factor,
                                             xf.scale, xf.rotation_deg, xf.position);

                int na_idx = fverts[0].vni;
                int nb_idx = fverts[i].vni;
                int nc_idx = fverts[i + 1].vni;

                if (xf.smooth_normals && na_idx >= 0 && na_idx < norm_count &&
                    nb_idx >= 0 && nb_idx < norm_count &&
                    nc_idx >= 0 && nc_idx < norm_count) {
                    Vec3 na = transform_normal(normals[na_idx], xf.scale, xf.rotation_deg);
                    Vec3 nb = transform_normal(normals[nb_idx], xf.scale, xf.rotation_deg);
                    Vec3 nc = transform_normal(normals[nc_idx], xf.scale, xf.rotation_deg);
                    if (geometry_add(g, prim_triangle_smooth(va, vb, vc, na, nb, nc, material_index)) >= 0) {
                        triangles_loaded++;
                    }
                } else {
                    if (geometry_add(g, prim_triangle(va, vb, vc, material_index)) >= 0) {
                        triangles_loaded++;
                    }
                }
            }
        }

        p += line_len + (eol ? 1 : 0);
    }

    free(positions);
    free(normals);
    return triangles_loaded;
}

int obj_load_file(const char *path, Geometry *g, int material_index, const ObjTransform *xform)
{
    if (!path || !g) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -2;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -2;
    }
    rewind(f);

    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(f);
        return -2;
    }

    size_t read_bytes = fread(buffer, 1, (size_t)size, f);
    fclose(f);
    buffer[read_bytes] = '\0';

    int rc = obj_load_mem(buffer, read_bytes, g, material_index, xform);
    free(buffer);
    return rc;
}
