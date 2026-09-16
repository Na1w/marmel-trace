/*
 * tests/test_integration_pbr.c - End-to-end integration test for the
 * physically-based (PBR) material layer:
 *
 *   - the opt-in `pbr` (0/1) gate on `Material` (default 0 = byte-identical
 *     legacy Blinn-Phong path),
 *   - the `metallic` / `roughness` / `emissive` scalar/vector fields,
 *   - `material_shade_pbr()` driving the direct-sun term whenever `pbr` is
 *     set (src/render.c swaps `material_shade_local` for
 *     `material_shade_pbr`),
 *   - the self-lit `emissive` term added once per shaded hit (guarded by
 *     `pbr` AND a non-zero emissive so the legacy return stays identical),
 *   - the named `type =` presets that imply `pbr = 1` (gold conductor,
 *     rough dielectric, emissive lamp).
 *
 * Pipeline under test (mirrors src/main.c):
 *   scene_desc_load_string -> scene_build_from_desc -> camera from the parsed
 *     `camera` block -> render_image -> ppm_write + bmp_write -> independent
 *     on-disk validation of BOTH files.
 *
 * SEMANTIC checks: `pbr = 1` vs `pbr = 0`, `emissive = 0` vs non-zero and
 * `roughness = 0.05` vs `0.8` on a metal must each change the render.
 * REGRESSION: a scene with NO PBR keys is byte-identical to the same scene
 * with `pbr = 0` spelled out explicitly (the legacy path is preserved).
 *
 * The Makefile links every C source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and
 * returns 0 on success / non-zero on any failure.
 *
 * C11, -Wall -Wextra clean. No rand(). All heap memory is freed and every
 * temporary file is removed.
 */

#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"
#include "bmp.h"
#include "vec3.h"
#include "material.h"
#include "texture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness (same convention as the other integration test) */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Temp files (fixed names, removed at start and end)                  */
/* ------------------------------------------------------------------ */

#define TMP_PPM "/tmp/rt_test_integration_pbr.ppm"
#define TMP_BMP "/tmp/rt_test_integration_pbr.bmp"

/* ------------------------------------------------------------------ */
/* The integration scene                                               */
/*                                                                     */
/* A polished `type = gold` conductor, a ROUGH coloured dielectric     */
/* (`pbr = 1`, `metallic = 0`, `roughness = 0.8`) and a `type =        */
/* emissive` lamp sit over a checker-textured floor, under a sun/sky.  */
/* ------------------------------------------------------------------ */

static const char *PBR_SCENE =
    "# PBR integration scene\n"
    "\n"
    "camera {\n"
    "    eye = 0 3 11\n"
    "    target = 0 2 -4\n"
    "    up = 0 1 0\n"
    "    vfov = 55\n"
    "    aperture = 0\n"
    "    focus_distance = 0\n"
    "}\n"
    "\n"
    "sky {\n"
    "    sun_dir = 0.40 0.72 -0.57\n"
    "    sun_color = 1 0.96 0.88\n"
    "    horizon_color = 0.78 0.86 1\n"
    "    zenith_color = 0.32 0.52 0.95\n"
    "    gradient_gamma = 0.65\n"
    "    sun_glow_exponent = 350\n"
    "    sun_glow_strength = 0.8\n"
    "    cloud_height = 120\n"
    "    cloud_scale = 0.0025\n"
    "    cloud_coverage = 0.5\n"
    "    cloud_softness = 0.12\n"
    "    cloud_sharpness = 1.5\n"
    "    cloud_octaves = 5\n"
    "    seed = 1337\n"
    "    sun_radius = 0\n"
    "}\n"
    "\n"
    "material floor {\n"
    "    albedo = 0.9 0.9 0.9\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "    texture = checker\n"
    "    texture_scale = 2\n"
    "    texture_color_a = 1 0.2 0.2\n"
    "    texture_color_b = 0.1 0.2 1\n"
    "}\n"
    "\n"
    "# polished conductor: the `type = gold` preset (pbr = 1, metallic = 1)\n"
    "material gold_mat {\n"
    "    type = gold\n"
    "}\n"
    "\n"
    "# rough coloured dielectric, explicitly opted into the PBR path\n"
    "material rough_dielectric {\n"
    "    pbr = 1\n"
    "    metallic = 0\n"
    "    roughness = 0.8\n"
    "    albedo = 0.15 0.45 0.25\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 32\n"
    "}\n"
    "\n"
    "# warm-white self-lit lamp\n"
    "material lamp {\n"
    "    type = emissive\n"
    "}\n"
    "\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = floor\n"
    "}\n"
    "\n"
    "sphere {\n"
    "    center = -2.2 1.5 0\n"
    "    radius = 1.5\n"
    "    material = gold_mat\n"
    "}\n"
    "sphere {\n"
    "    center = 0 1.5 0\n"
    "    radius = 1.5\n"
    "    material = rough_dielectric\n"
    "}\n"
    "sphere {\n"
    "    center = 2.4 1.3 0\n"
    "    radius = 1.3\n"
    "    material = lamp\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Legacy regression scene: NO PBR keys at all (implicit pbr = 0).     */
/* ------------------------------------------------------------------ */

static const char *LEGACY_SCENE =
    "material floor {\n"
    "    albedo = 0.9 0.9 0.9\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "    texture = checker\n"
    "    texture_scale = 2\n"
    "    texture_color_a = 1 0.2 0.2\n"
    "    texture_color_b = 0.1 0.2 1\n"
    "}\n"
    "material red {\n"
    "    albedo = 0.8 0.15 0.12\n"
    "    specular = 0.2 0.2 0.2\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n"
    "material blue {\n"
    "    albedo = 0.15 0.25 0.85\n"
    "    specular = 0.2 0.2 0.2\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = floor\n"
    "}\n"
    "sphere {\n"
    "    center = -2 1.2 0\n"
    "    radius = 1.2\n"
    "    material = red\n"
    "}\n"
    "sphere {\n"
    "    center = 2 1.2 0\n"
    "    radius = 1.2\n"
    "    material = blue\n"
    "}\n";

/* Identical to LEGACY_SCENE but with `pbr = 0` spelled out explicitly. */
static const char *LEGACY_PBR0_SCENE =
    "material floor {\n"
    "    pbr = 0\n"
    "    albedo = 0.9 0.9 0.9\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 8\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "    texture = checker\n"
    "    texture_scale = 2\n"
    "    texture_color_a = 1 0.2 0.2\n"
    "    texture_color_b = 0.1 0.2 1\n"
    "}\n"
    "material red {\n"
    "    pbr = 0\n"
    "    albedo = 0.8 0.15 0.12\n"
    "    specular = 0.2 0.2 0.2\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n"
    "material blue {\n"
    "    pbr = 0\n"
    "    albedo = 0.15 0.25 0.85\n"
    "    specular = 0.2 0.2 0.2\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = floor\n"
    "}\n"
    "sphere {\n"
    "    center = -2 1.2 0\n"
    "    radius = 1.2\n"
    "    material = red\n"
    "}\n"
    "sphere {\n"
    "    center = 2 1.2 0\n"
    "    radius = 1.2\n"
    "    material = blue\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int vec3_eq(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static int vec3_nonzero(Vec3 a)
{
    return a.x != 0.0 || a.y != 0.0 || a.z != 0.0;
}

/* Index of the material with the given name, or -1. */
static int find_material(const SceneDesc *d, const char *name)
{
    int i;
    for (i = 0; i < d->material_count; ++i) {
        if (d->materials[i].name != NULL &&
            strcmp(d->materials[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Read an entire file into a heap buffer. Returns NULL on error. */
static unsigned char *read_whole_file(const char *path, long *out_size)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *buf;
    long sz;

    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)sz + 1u);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_size = sz;
    return buf;
}

/* Length of the canonical P6 header for the given dimensions. */
static size_t ppm_header_len(int width, int height)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "P6\n%d %d\n255\n", width, height);
    return (n > 0) ? (size_t)n : 0u;
}

/* Raw little-endian BMP field readers. */
static unsigned rd_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned rd_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int rd_i32(const unsigned char *p)
{
    return (int)rd_u32(p);
}

/* Count distinct 24-bit colours using a 2^24-bit set (2 MB). */
static int count_distinct_colors(const unsigned char *rgb, size_t npix,
                                 unsigned char *set)
{
    size_t i;
    int distinct = 0;

    for (i = 0; i < npix; ++i) {
        unsigned color = ((unsigned)rgb[i * 3u + 0u] << 16) |
                         ((unsigned)rgb[i * 3u + 1u] << 8)  |
                         ((unsigned)rgb[i * 3u + 2u]);
        size_t byte = (size_t)color >> 3;
        unsigned char mask = (unsigned char)(1u << (color & 7u));

        if ((set[byte] & mask) == 0) {
            set[byte] |= mask;
            distinct++;
        }
    }
    return distinct;
}

static int buffer_is_all(const unsigned char *rgb, size_t n, unsigned char v)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (rgb[i] != v) {
            return 0;
        }
    }
    return 1;
}

/* Mean relative luminance (Rec. 709) of a top-down RGB buffer, 0..255. */
static double buffer_mean_luma(const unsigned char *rgb, size_t npix)
{
    size_t i;
    double sum = 0.0;
    for (i = 0; i < npix; ++i) {
        sum += 0.2126 * (double)rgb[i * 3u + 0u] +
               0.7152 * (double)rgb[i * 3u + 1u] +
               0.0722 * (double)rgb[i * 3u + 2u];
    }
    return (npix > 0u) ? (sum / (double)npix) : 0.0;
}

/* Build a camera exactly as src/main.c wires it from a parsed description. */
static Camera camera_from_desc(const CameraDesc *cd, int w, int h)
{
    Camera cam;
    if (cd->present) {
        cam = camera_create(cd->eye, cd->target, cd->up, cd->vfov_deg,
                            (double)w / (double)h);
        cam.aperture = cd->aperture;
        if (cd->focus_distance > CAMERA_FOCUS_DISTANCE_DERIVED) {
            cam.focus_distance = cd->focus_distance;
        }
    } else {
        Vec3 eye, target, up;
        double vfov = 0.0;
        scene_default_view(&eye, &target, &up, &vfov);
        cam = camera_create(eye, target, up, vfov, (double)w / (double)h);
    }
    return cam;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    enum { W = 64, H = 48, SPP = 2, DEPTH = 4 };
    SceneDesc desc_main, desc_pbr0, desc_emis0, desc_rlo, desc_rhi;
    SceneDesc desc_legacy, desc_legacy0;
    Scene scene_main, scene_pbr0, scene_emis0, scene_rlo, scene_rhi;
    Scene scene_legacy, scene_legacy0;
    Camera cam;
    unsigned char *rgb_main = NULL, *rgb_main2 = NULL, *rgb_pbr0 = NULL,
                  *rgb_emis0 = NULL, *rgb_rlo = NULL, *rgb_rhi = NULL,
                  *rgb_legacy = NULL, *rgb_legacy0 = NULL, *color_set = NULL;
    size_t nbytes = (size_t)W * (size_t)H * 3u;
    char errbuf[256];
    int rc;
    int gold_idx = -1, rough_idx = -1, lamp_idx = -1, floor_idx = -1;

    /* Clean any leftovers from a previous aborted run. */
    remove(TMP_PPM);
    remove(TMP_BMP);

    scene_desc_init(&desc_main);
    scene_desc_init(&desc_pbr0);
    scene_desc_init(&desc_emis0);
    scene_desc_init(&desc_rlo);
    scene_desc_init(&desc_rhi);
    scene_desc_init(&desc_legacy);
    scene_desc_init(&desc_legacy0);
    memset(&scene_main, 0, sizeof scene_main);
    memset(&scene_pbr0, 0, sizeof scene_pbr0);
    memset(&scene_emis0, 0, sizeof scene_emis0);
    memset(&scene_rlo, 0, sizeof scene_rlo);
    memset(&scene_rhi, 0, sizeof scene_rhi);
    memset(&scene_legacy, 0, sizeof scene_legacy);
    memset(&scene_legacy0, 0, sizeof scene_legacy0);

    /* --- 1. parse the PBR scene ---------------------------------------- */
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_main, PBR_SCENE, "<pbr>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "scene_desc_load_string parses the PBR scene");
    if (rc != 0) {
        fprintf(stderr, "parse error: %s\n", errbuf);
        goto cleanup;
    }

    /* --- 2. the description carries all four materials ----------------- */
    CHECK(desc_main.material_count == 4,
          "parsed scene has four materials (floor/gold/rough/lamp)");

    gold_idx  = find_material(&desc_main, "gold_mat");
    rough_idx = find_material(&desc_main, "rough_dielectric");
    lamp_idx  = find_material(&desc_main, "lamp");
    floor_idx = find_material(&desc_main, "floor");
    CHECK(gold_idx >= 0, "`gold_mat` material found");
    CHECK(rough_idx >= 0, "`rough_dielectric` material found");
    CHECK(lamp_idx >= 0, "`lamp` material found");
    CHECK(floor_idx >= 0, "`floor` material found");

    /* --- 3. `type = gold` preset semantics ----------------------------- */
    if (gold_idx >= 0) {
        const Material *m = &desc_main.materials[gold_idx].mat;
        CHECK(m->pbr == 1, "gold preset opts into PBR (pbr == 1)");
        CHECK(m->metallic == 1.0, "gold preset is a conductor (metallic == 1)");
        CHECK(m->roughness == 0.05, "gold preset roughness == 0.05");
        CHECK(vec3_eq(m->albedo, vec3(1.000, 0.766, 0.336)),
              "gold preset albedo is the linear gold F0");
        CHECK(vec3_eq(m->emissive, vec3(0.0, 0.0, 0.0)),
              "gold preset is not emissive");
    }

    /* --- 4. rough dielectric explicit keys ----------------------------- */
    if (rough_idx >= 0) {
        const Material *m = &desc_main.materials[rough_idx].mat;
        CHECK(m->pbr == 1, "rough dielectric sets pbr == 1");
        CHECK(m->metallic == 0.0, "rough dielectric is not metallic");
        CHECK(m->roughness == 0.8, "rough dielectric roughness == 0.8");
        CHECK(vec3_eq(m->albedo, vec3(0.15, 0.45, 0.25)),
              "rough dielectric keeps its coloured albedo");
        CHECK(vec3_eq(m->emissive, vec3(0.0, 0.0, 0.0)),
              "rough dielectric is not emissive");
    }

    /* --- 5. `type = emissive` preset semantics ------------------------- */
    if (lamp_idx >= 0) {
        const Material *m = &desc_main.materials[lamp_idx].mat;
        CHECK(m->pbr == 1, "emissive preset opts into PBR (pbr == 1)");
        CHECK(vec3_nonzero(m->emissive), "emissive preset has a non-zero emissive");
        CHECK(m->emissive.x == 1.0 && m->emissive.y == 0.85 &&
              m->emissive.z == 0.65,
              "emissive preset is the warm-white (1.0,0.85,0.65) lamp");
        CHECK(m->metallic == 0.0, "emissive preset is non-metallic");
    }

    /* --- 6. legacy floor material stays out of the PBR path ------------ */
    if (floor_idx >= 0) {
        const Material *m = &desc_main.materials[floor_idx].mat;
        CHECK(m->pbr == 0, "legacy floor material keeps pbr == 0 (default)");
        CHECK(m->texture_kind == TEXTURE_CHECKER,
              "floor material carries the checker texture");
    }

    /* --- 7. camera block parsed + scene built -------------------------- */
    CHECK(desc_main.camera.present == 1, "camera block present");
    cam = camera_from_desc(&desc_main.camera, W, H);

    rc = scene_build_from_desc(&scene_main, &desc_main);
    CHECK(rc == 0, "scene_build_from_desc(PBR) returns 0");
    if (rc != 0)
        goto cleanup;
    CHECK(scene_main.geo.count > 0, "built PBR scene has geometry");

    /* --- 8. variant scenes (same text, mutated fields) ----------------- */
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_pbr0, PBR_SCENE, "<pbr>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "re-parse the scene for the pbr=0 variant");
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_emis0, PBR_SCENE, "<pbr>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "re-parse the scene for the emissive=0 variant");
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_rlo, PBR_SCENE, "<pbr>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "re-parse the scene for the roughness=0.05 variant");
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_rhi, PBR_SCENE, "<pbr>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "re-parse the scene for the roughness=0.8 variant");
    if (rc != 0)
        goto cleanup;

    {
        int i_pbr0 = find_material(&desc_pbr0, "gold_mat");
        int i_emis0 = find_material(&desc_emis0, "lamp");
        int i_rlo = find_material(&desc_rlo, "gold_mat");
        int i_rhi = find_material(&desc_rhi, "gold_mat");

        CHECK(i_pbr0 >= 0 && i_emis0 >= 0 && i_rlo >= 0 && i_rhi >= 0,
              "all variant descriptions carry the target materials");

        if (i_pbr0 >= 0) {
            desc_pbr0.materials[i_pbr0].mat.pbr = 0;   /* legacy path */
            CHECK(desc_pbr0.materials[i_pbr0].mat.pbr == 0,
                  "pbr=0 variant has pbr == 0");
        }
        if (i_emis0 >= 0) {
            desc_emis0.materials[i_emis0].mat.emissive = vec3(0.0, 0.0, 0.0);
            CHECK(vec3_eq(desc_emis0.materials[i_emis0].mat.emissive,
                          vec3(0.0, 0.0, 0.0)),
                  "emissive=0 variant has a zero emissive");
            CHECK(desc_emis0.materials[i_emis0].mat.pbr == 1,
                  "emissive=0 variant stays on the PBR path");
        }
        if (i_rlo >= 0) {
            desc_rlo.materials[i_rlo].mat.roughness = 0.05;
            CHECK(desc_rlo.materials[i_rlo].mat.roughness == 0.05,
                  "roughness=0.05 variant applied");
        }
        if (i_rhi >= 0) {
            desc_rhi.materials[i_rhi].mat.roughness = 0.8;
            CHECK(desc_rhi.materials[i_rhi].mat.roughness == 0.8,
                  "roughness=0.8 variant applied");
        }
    }

    rc = scene_build_from_desc(&scene_pbr0, &desc_pbr0);
    CHECK(rc == 0, "scene_build_from_desc(pbr=0) returns 0");
    rc = scene_build_from_desc(&scene_emis0, &desc_emis0);
    CHECK(rc == 0, "scene_build_from_desc(emissive=0) returns 0");
    rc = scene_build_from_desc(&scene_rlo, &desc_rlo);
    CHECK(rc == 0, "scene_build_from_desc(roughness=0.05) returns 0");
    rc = scene_build_from_desc(&scene_rhi, &desc_rhi);
    CHECK(rc == 0, "scene_build_from_desc(roughness=0.8) returns 0");
    if (rc != 0)
        goto cleanup;

    CHECK(scene_pbr0.geo.count == scene_main.geo.count &&
          scene_emis0.geo.count == scene_main.geo.count &&
          scene_rlo.geo.count == scene_main.geo.count &&
          scene_rhi.geo.count == scene_main.geo.count,
          "variant scenes keep identical geometry (only material fields differ)");

    /* --- 9. allocate render buffers ------------------------------------ */
    rgb_main   = (unsigned char *)malloc(nbytes);
    rgb_main2  = (unsigned char *)malloc(nbytes);
    rgb_pbr0   = (unsigned char *)malloc(nbytes);
    rgb_emis0  = (unsigned char *)malloc(nbytes);
    rgb_rlo    = (unsigned char *)malloc(nbytes);
    rgb_rhi    = (unsigned char *)malloc(nbytes);
    rgb_legacy = (unsigned char *)malloc(nbytes);
    rgb_legacy0 = (unsigned char *)malloc(nbytes);
    color_set  = (unsigned char *)calloc((size_t)1 << 21, 1u); /* 2^24 bits */
    CHECK(rgb_main != NULL && rgb_main2 != NULL && rgb_pbr0 != NULL &&
          rgb_emis0 != NULL && rgb_rlo != NULL && rgb_rhi != NULL &&
          rgb_legacy != NULL && rgb_legacy0 != NULL && color_set != NULL,
          "all working buffers allocated");
    if (rgb_main == NULL || rgb_main2 == NULL || rgb_pbr0 == NULL ||
        rgb_emis0 == NULL || rgb_rlo == NULL || rgb_rhi == NULL ||
        rgb_legacy == NULL || rgb_legacy0 == NULL || color_set == NULL)
        goto cleanup;

    /* --- 10. render the PBR scene -------------------------------------- */
    rc = render_image(&scene_main, &cam, W, H, SPP, DEPTH, rgb_main);
    CHECK(rc == 0, "render_image(PBR) returns 0");
    if (rc != 0)
        goto cleanup;

    /* --- 11. non-degenerate pixel content ------------------------------ */
    {
        int distinct = count_distinct_colors(rgb_main, (size_t)W * (size_t)H,
                                             color_set);
        int all_same = 1;
        size_t k;
        for (k = 1; k < nbytes; ++k) {
            if (rgb_main[k] != rgb_main[0]) { all_same = 0; break; }
        }
        printf("pbr render: %d distinct colours\n", distinct);
        CHECK(distinct > 1, "rendered image has more than one distinct colour");
        CHECK(distinct > 4, "rendered image is clearly non-degenerate");
        CHECK(!all_same, "render buffer was actually written (not all identical)");
        CHECK(!buffer_is_all(rgb_main, nbytes, 0u),
              "render is not an all-black image");
    }

    /* --- 12. determinism: the PBR render is reproducible --------------- */
    rc = render_image(&scene_main, &cam, W, H, SPP, DEPTH, rgb_main2);
    CHECK(rc == 0, "second PBR render returns 0");
    if (rc == 0)
        CHECK(memcmp(rgb_main, rgb_main2, nbytes) == 0,
              "PBR render is byte-identical across repeats (deterministic)");

    /* --- 13. SEMANTIC CHECK: pbr = 1 vs pbr = 0 ------------------------ */
    rc = render_image(&scene_pbr0, &cam, W, H, SPP, DEPTH, rgb_pbr0);
    CHECK(rc == 0, "render_image(pbr=0) returns 0");
    if (rc == 0) {
        int differ = (memcmp(rgb_main, rgb_pbr0, nbytes) != 0);
        printf("pbr 1 vs 0: %s\n", differ ? "DIFFER" : "identical");
        CHECK(differ,
              "turning the gold conductor off the PBR path changes the render");
    }

    /* --- 14. SEMANTIC CHECK: emissive 0 vs non-zero -------------------- */
    rc = render_image(&scene_emis0, &cam, W, H, SPP, DEPTH, rgb_emis0);
    CHECK(rc == 0, "render_image(emissive=0) returns 0");
    if (rc == 0) {
        int differ = (memcmp(rgb_main, rgb_emis0, nbytes) != 0);
        double luma_on = buffer_mean_luma(rgb_main, (size_t)W * (size_t)H);
        double luma_off = buffer_mean_luma(rgb_emis0, (size_t)W * (size_t)H);
        printf("emissive on vs off: %s (mean luma %.3f vs %.3f)\n",
               differ ? "DIFFER" : "identical", luma_on, luma_off);
        CHECK(differ, "removing the emissive term changes the render");
        CHECK(luma_on > luma_off,
              "the emissive lamp brightens the image (higher mean luminance)");
    }

    /* --- 15. SEMANTIC CHECK: roughness 0.05 vs 0.8 (metal) ------------- */
    rc = render_image(&scene_rlo, &cam, W, H, SPP, DEPTH, rgb_rlo);
    CHECK(rc == 0, "render_image(gold roughness=0.05) returns 0");
    rc = render_image(&scene_rhi, &cam, W, H, SPP, DEPTH, rgb_rhi);
    CHECK(rc == 0, "render_image(gold roughness=0.8) returns 0");
    if (rc == 0) {
        int differ = (memcmp(rgb_rlo, rgb_rhi, nbytes) != 0);
        printf("gold roughness 0.05 vs 0.8: %s\n",
               differ ? "DIFFER" : "identical");
        CHECK(differ, "metal roughness changes the rendered highlights");
        /* A rough metal must also differ from the polished preset render. */
        CHECK(memcmp(rgb_rhi, rgb_main, nbytes) != 0,
              "rough gold differs from the polished gold preset render");
    }

    /* --- 16. write BOTH PPM and BMP ------------------------------------ */
    remove(TMP_PPM);
    remove(TMP_BMP);
    rc = ppm_write(TMP_PPM, rgb_main, W, H);
    CHECK(rc == 0, "ppm_write returns 0");
    rc = bmp_write(TMP_BMP, rgb_main, W, H);
    CHECK(rc == 0, "bmp_write returns 0");

    /* --- 17. independent on-disk PPM validation ------------------------ */
    {
        unsigned char *buf;
        long size = 0;
        char expected[64];
        size_t hlen = ppm_header_len(W, H);

        buf = read_whole_file(TMP_PPM, &size);
        CHECK(buf != NULL, "read back the PPM file");
        if (buf != NULL) {
            (void)snprintf(expected, sizeof expected, "P6\n%d %d\n255\n", W, H);
            printf("ppm: filesize=%ld expected=%ld\n", size,
                   (long)(hlen + nbytes));
            CHECK(buf[0] == 'P' && buf[1] == '6', "PPM magic is 'P','6'");
            CHECK(memcmp(buf, expected, hlen) == 0,
                  "PPM header is exactly 'P6\\n<w> <h>\\n255\\n'");
            CHECK(size == (long)(hlen + nbytes),
                  "PPM size == header + width*height*3 (no padding)");
            CHECK(memcmp(buf + hlen, rgb_main, nbytes) == 0,
                  "PPM payload is the verbatim top-down RGB buffer");
            free(buf);
        }
    }

    /* --- 18. independent on-disk BMP validation ------------------------ */
    {
        unsigned char *buf;
        long size = 0;

        buf = read_whole_file(TMP_BMP, &size);
        CHECK(buf != NULL, "read back the BMP file");
        if (buf != NULL) {
            size_t row_size = (((size_t)W * 3u + 3u) / 4u) * 4u;
            size_t expected = 54u + row_size * (size_t)H;
            unsigned bfSize = rd_u32(buf + 2);
            unsigned bfOffBits = rd_u32(buf + 10);
            int biWidth = rd_i32(buf + 18);
            int biHeight = rd_i32(buf + 22);
            unsigned biBitCount = rd_u16(buf + 28);
            unsigned biCompression = rd_u32(buf + 30);

            printf("bmp: filesize=%ld bfSize=%u offBits=%u w=%d h=%d bits=%u comp=%u\n",
                   size, bfSize, bfOffBits, biWidth, biHeight,
                   biBitCount, biCompression);

            CHECK(buf[0] == 'B' && buf[1] == 'M', "BMP magic bytes are 'BM'");
            CHECK((size_t)bfSize == (size_t)size,
                  "BMP bfSize equals the real on-disk size");
            CHECK(bfOffBits == 54u, "BMP bfOffBits == 54");
            CHECK(biWidth == W, "BMP biWidth matches the render width");
            CHECK(biHeight == H, "BMP biHeight matches the render height");
            CHECK(biBitCount == 24u, "BMP biBitCount == 24");
            CHECK(biCompression == 0u, "BMP biCompression == BI_RGB (0)");
            CHECK((size_t)size == expected,
                  "BMP size == 54 + padded_row_size*height");

            /* Cross-format consistency: decode the bottom-up BGR BMP and
             * compare it pixel-for-pixel with the top-down RGB buffer that
             * also produced the PPM. */
            if ((size_t)size == expected) {
                long mismatches = 0;
                int x, y;
                for (y = 0; y < H; ++y) {
                    int file_row = H - 1 - y; /* BMP rows are bottom-up */
                    const unsigned char *src =
                        buf + 54u + (size_t)file_row * row_size;
                    for (x = 0; x < W; ++x) {
                        size_t si = ((size_t)y * (size_t)W + (size_t)x) * 3u;
                        if (src[x * 3 + 0] != rgb_main[si + 2] ||
                            src[x * 3 + 1] != rgb_main[si + 1] ||
                            src[x * 3 + 2] != rgb_main[si + 0]) {
                            mismatches++;
                        }
                    }
                }
                CHECK(mismatches == 0,
                      "BMP pixels match the rendered RGB (BGR + row flip)");
            }
            free(buf);
        }
    }

    /* --- 19. REGRESSION: no-PBR scene == explicit pbr = 0 -------------- */
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_legacy, LEGACY_SCENE, "<legacy>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "scene_desc_load_string parses the legacy (no-PBR) scene");
    if (rc != 0) {
        fprintf(stderr, "parse error: %s\n", errbuf);
        goto cleanup;
    }
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_legacy0, LEGACY_PBR0_SCENE, "<legacy0>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "scene_desc_load_string parses the explicit pbr=0 scene");
    if (rc != 0) {
        fprintf(stderr, "parse error: %s\n", errbuf);
        goto cleanup;
    }

    {
        int lg_floor = find_material(&desc_legacy, "floor");
        int lg_red = find_material(&desc_legacy, "red");
        int lg_blue = find_material(&desc_legacy, "blue");
        CHECK(lg_floor >= 0 && lg_red >= 0 && lg_blue >= 0,
              "legacy scene materials found");
        if (lg_floor >= 0 && lg_red >= 0 && lg_blue >= 0) {
            CHECK(desc_legacy.materials[lg_floor].mat.pbr == 0 &&
                  desc_legacy.materials[lg_red].mat.pbr == 0 &&
                  desc_legacy.materials[lg_blue].mat.pbr == 0,
                  "omitting `pbr` leaves every legacy material at pbr == 0");
            CHECK(desc_legacy.materials[lg_red].mat.metallic == 0.0 &&
                  desc_legacy.materials[lg_red].mat.roughness == 0.0,
                  "legacy material keeps metallic/roughness defaults (0)");
            CHECK(vec3_eq(desc_legacy.materials[lg_red].mat.emissive,
                          vec3(0.0, 0.0, 0.0)),
                  "legacy material keeps the zero emissive default");
        }
    }

    rc = scene_build_from_desc(&scene_legacy, &desc_legacy);
    CHECK(rc == 0, "scene_build_from_desc(legacy) returns 0");
    rc = scene_build_from_desc(&scene_legacy0, &desc_legacy0);
    CHECK(rc == 0, "scene_build_from_desc(legacy, explicit pbr=0) returns 0");
    if (rc != 0)
        goto cleanup;

    rc = render_image(&scene_legacy, &cam, W, H, SPP, DEPTH, rgb_legacy);
    CHECK(rc == 0, "render_image(legacy) returns 0");
    rc = render_image(&scene_legacy0, &cam, W, H, SPP, DEPTH, rgb_legacy0);
    CHECK(rc == 0, "render_image(legacy, explicit pbr=0) returns 0");
    if (rc != 0)
        goto cleanup;

    {
        int same = (memcmp(rgb_legacy, rgb_legacy0, nbytes) == 0);
        printf("legacy (no key) vs explicit pbr=0: %s\n",
               same ? "IDENTICAL" : "differ");
        CHECK(same,
              "spelling `pbr = 0` explicitly changes nothing (legacy path preserved)");
        /* And the legacy result must NOT accidentally equal the PBR result. */
        CHECK(memcmp(rgb_legacy, rgb_main, nbytes) != 0,
              "legacy (Blinn-Phong) render differs from the PBR render");
    }

    /* --- 20. cleanup --------------------------------------------------- */
cleanup:
    free(rgb_main);
    free(rgb_main2);
    free(rgb_pbr0);
    free(rgb_emis0);
    free(rgb_rlo);
    free(rgb_rhi);
    free(rgb_legacy);
    free(rgb_legacy0);
    free(color_set);
    scene_free(&scene_main);
    scene_free(&scene_pbr0);
    scene_free(&scene_emis0);
    scene_free(&scene_rlo);
    scene_free(&scene_rhi);
    scene_free(&scene_legacy);
    scene_free(&scene_legacy0);
    scene_desc_free(&desc_main);
    scene_desc_free(&desc_pbr0);
    scene_desc_free(&desc_emis0);
    scene_desc_free(&desc_rlo);
    scene_desc_free(&desc_rhi);
    scene_desc_free(&desc_legacy);
    scene_desc_free(&desc_legacy0);
    remove(TMP_PPM);
    remove(TMP_BMP);

    printf("\ntest_integration_pbr: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}
