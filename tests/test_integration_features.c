/*
 * tests/test_integration_features.c - End-to-end integration test for the
 * renderer features layered on top of the base pipeline:
 *
 *   - procedural per-material textures  (`texture = checker`,
 *     `texture_scale`, `texture_color_a`, `texture_color_b`)
 *   - soft shadows                      (`sky.sun_radius` > 0)
 *   - depth of field                    (`camera.aperture`, `focus_distance`)
 *   - file-controllable tree/bush generator parameters (docs/scene_format.md
 *     §4.9/§4.10)
 *   - PPM (P6) and BMP output writers
 *
 * Pipeline under test (mirrors src/main.c):
 *   scene_desc_load_string -> scene_build_from_desc -> camera from the parsed
 *     `camera` block -> render_image -> ppm_write + bmp_write -> independent
 *     on-disk validation of BOTH files.
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
/* Minimal test harness                                                */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Temp files (fixed names, removed at start and end)                  */
/* ------------------------------------------------------------------ */

#define TMP_PPM "/tmp/rt_test_integration_features.ppm"
#define TMP_BMP "/tmp/rt_test_integration_features.bmp"

/* ------------------------------------------------------------------ */
/* The integration scene: exercises EVERY new feature at once          */
/* ------------------------------------------------------------------ */

static const char *INTEGRATION_SCENE =
    "# integration scene for the new renderer features\n"
    "\n"
    "camera {\n"
    "    eye = -18 6 22\n"
    "    target = 0 5 -20\n"
    "    up = 0 1 0\n"
    "    vfov = 40\n"
    "    aperture = 0.25\n"
    "    focus_distance = 30\n"
    "}\n"
    "\n"
    "sky {\n"
    "    sun_dir = 0.45 0.75 -0.50\n"
    "    sun_color = 1 0.95 0.85\n"
    "    horizon_color = 0.75 0.85 1\n"
    "    zenith_color = 0.35 0.55 0.95\n"
    "    gradient_gamma = 0.6\n"
    "    sun_glow_exponent = 350\n"
    "    sun_glow_strength = 0.8\n"
    "    cloud_height = 120\n"
    "    cloud_scale = 0.0025\n"
    "    cloud_coverage = 0.5\n"
    "    cloud_softness = 0.12\n"
    "    cloud_sharpness = 1.5\n"
    "    cloud_octaves = 5\n"
    "    seed = 1337\n"
    "    sun_radius = 2.5\n"
    "}\n"
    "\n"
    "material checker {\n"
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
    "material bark {\n"
    "    albedo = 0.26 0.18 0.11\n"
    "    specular = 0.04 0.04 0.04\n"
    "    shininess = 6\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n"
    "\n"
    "material leaf {\n"
    "    albedo = 0.22 0.45 0.15\n"
    "    specular = 0.03 0.04 0.03\n"
    "    shininess = 10\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n"
    "\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = checker\n"
    "}\n"
    "\n"
    "tree {\n"
    "    position = 6 0 -14\n"
    "    height = 3.4\n"
    "    radius = 0.2\n"
    "    seed = 4242\n"
    "    material_bark = bark\n"
    "    material_leaf = leaf\n"
    "    leaf_variant = 1\n"
    "    max_depth = 3\n"
    "    min_branch_radius = 0.03\n"
    "    taper = 0.65\n"
    "    len_decay = 0.68\n"
    "    spread_deg = 41.5\n"
    "    perturb_deg = 9.5\n"
    "    up_bias = 0.2\n"
    "    third_child_chance = 0.4\n"
    "    leaf_min = 6\n"
    "    leaf_span = 3\n"
    "}\n"
    "\n"
    "bush {\n"
    "    position = -8 0 -18\n"
    "    height = 0.8\n"
    "    radius = 0.05\n"
    "    seed = 99\n"
    "    material_bark = bark\n"
    "    material_leaf = leaf\n"
    "    leaf_variant = 2\n"
    "    max_depth = 2\n"
    "    leaf_min = 4\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int vec3_eq(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
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

/* Index of the (first) material carrying a procedural checker texture. */
static int find_checker_material(const Scene *s)
{
    int i;
    for (i = 0; i < s->material_count; ++i) {
        const Material *m = scene_material(s, i);
        if (m != NULL && m->texture_kind == TEXTURE_CHECKER) {
            return i;
        }
    }
    return -1;
}

/*
 * Clear every optional generator-parameter presence flag on a plant, so the
 * builder falls back to the legacy SCENE_* constants. Used to prove that the
 * file-controllable parameters actually change the generated geometry.
 */
static void clear_plant_flags(ScenePlantDesc *p)
{
    p->has_max_depth = 0;
    p->has_min_branch_radius = 0;
    p->has_taper = 0;
    p->has_len_decay = 0;
    p->has_spread_deg = 0;
    p->has_perturb_deg = 0;
    p->has_up_bias = 0;
    p->has_third_child_chance = 0;
    p->has_leaf_min = 0;
    p->has_leaf_span = 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    enum { W = 64, H = 48, SPP = 2, DEPTH = 2 };
    SceneDesc desc, desc_plain;
    Scene scene_a, scene_b, scene_c;
    Camera cam;
    Vec3 eye, target, up;
    double vfov = 0.0;
    unsigned char *rgb_a = NULL, *rgb_a2 = NULL, *rgb_b = NULL, *color_set = NULL;
    size_t nbytes = (size_t)W * (size_t)H * 3u;
    char errbuf[256];
    int rc;
    int checker_idx = -1;
    int i;

    /* Clean any leftovers from a previous aborted run. */
    remove(TMP_PPM);
    remove(TMP_BMP);

    scene_desc_init(&desc);
    scene_desc_init(&desc_plain);
    memset(&scene_a, 0, sizeof scene_a);
    memset(&scene_b, 0, sizeof scene_b);
    memset(&scene_c, 0, sizeof scene_c);

    /* --- 1. parse the all-features scene ------------------------------- */
    errbuf[0] = '\0';
    CHECK(scene_desc_load_string(&desc, INTEGRATION_SCENE, "<integration>",
                                 errbuf, sizeof errbuf) == 0,
          "scene_desc_load_string parses the all-features scene");

    /* --- 2. the parsed description carries every new feature ----------- */
    CHECK(desc.camera.present == 1, "camera block present");
    CHECK(desc.camera.aperture == 0.25, "camera aperture parsed as 0.25");
    CHECK(desc.camera.focus_distance == 30.0,
          "camera focus_distance parsed as 30");
    CHECK(desc.has_sky == 1, "sky block present");
    CHECK(desc.sky.sun_radius == 2.5, "sky sun_radius parsed as 2.5");
    CHECK(desc.plant_count == 2, "two plant directives parsed");

    if (desc.plant_count == 2) {
        const ScenePlantDesc *t = &desc.plants[0];
        const ScenePlantDesc *b = &desc.plants[1];
        CHECK(t->kind == SD_PLANT_TREE && b->kind == SD_PLANT_BUSH,
              "plant 0 is a tree, plant 1 is a bush");
        CHECK(t->has_max_depth && t->max_depth == 3, "tree max_depth = 3");
        CHECK(t->has_leaf_min && t->leaf_min == 6, "tree leaf_min = 6");
        CHECK(t->has_taper && t->taper == 0.65, "tree taper = 0.65");
        CHECK(b->has_max_depth && b->max_depth == 2, "bush max_depth = 2");
        CHECK(b->has_leaf_min && b->leaf_min == 4, "bush leaf_min = 4");
    }

    /* --- 3. camera, exactly as src/main.c wires it --------------------- */
    if (desc.camera.present) {
        eye = desc.camera.eye;
        target = desc.camera.target;
        up = desc.camera.up;
        vfov = desc.camera.vfov_deg;
    } else {
        scene_default_view(&eye, &target, &up, &vfov);
    }
    cam = camera_create(eye, target, up, vfov, (double)W / (double)H);
    cam.aperture = desc.camera.aperture;
    if (desc.camera.focus_distance > CAMERA_FOCUS_DISTANCE_DERIVED)
        cam.focus_distance = desc.camera.focus_distance;
    CHECK(cam.aperture == 0.25, "camera carries aperture 0.25");
    CHECK(cam.focus_distance == 30.0, "camera carries focus_distance 30");

    /* --- 4. build the full-feature scene ------------------------------- */
    rc = scene_build_from_desc(&scene_a, &desc);
    CHECK(rc == 0, "scene_build_from_desc(full features) returns 0");
    if (rc != 0)
        goto cleanup;

    CHECK(scene_a.geo.count > 0, "built scene has geometry");
    CHECK(scene_a.material_count == 3, "built scene has three materials");
    CHECK(scene_a.sky.sun_radius == 2.5, "built scene keeps sun_radius = 2.5");

    checker_idx = find_checker_material(&scene_a);
    CHECK(checker_idx >= 0, "built scene has a checker-textured material");

    if (checker_idx >= 0) {
        const Material *m = scene_material(&scene_a, checker_idx);
        Vec3 p_even = vec3(0.5, 0.0, 0.5);   /* cell (0,0,0) -> colour B */
        Vec3 p_odd  = vec3(2.5, 0.0, 0.5);   /* cell (1,0,0) -> colour A */
        Vec3 c_even = texture_albedo(m, p_even);
        Vec3 c_odd  = texture_albedo(m, p_odd);

        CHECK(m->texture_kind == TEXTURE_CHECKER,
              "checker material texture_kind == TEXTURE_CHECKER");
        CHECK(m->texture_scale == 2.0, "checker material texture_scale == 2");
        CHECK(vec3_eq(m->texture_color_a, vec3(1.0, 0.2, 0.2)),
              "checker texture_color_a parsed exactly");
        CHECK(vec3_eq(m->texture_color_b, vec3(0.1, 0.2, 1.0)),
              "checker texture_color_b parsed exactly");

        /* The texture must genuinely VARY the colour across the surface. */
        CHECK(!vec3_eq(c_even, c_odd),
              "checker texture varies colour across the surface");
        CHECK(c_even.z > c_even.x,
              "even cell picks the blue-dominant texture_color_b");
        CHECK(c_odd.x > c_odd.z,
              "odd cell picks the red-dominant texture_color_a");
    }

    /* --- 5. custom generator params actually change the geometry ------- */
    errbuf[0] = '\0';
    CHECK(scene_desc_load_string(&desc_plain, INTEGRATION_SCENE, "<integration>",
                                 errbuf, sizeof errbuf) == 0,
          "re-parse the scene for the default-generator comparison");
    for (i = 0; i < desc_plain.plant_count; ++i)
        clear_plant_flags(&desc_plain.plants[i]);

    rc = scene_build_from_desc(&scene_c, &desc_plain);
    CHECK(rc == 0, "scene_build_from_desc(default generator params) returns 0");
    scene_desc_free(&desc_plain);
    if (rc == 0) {
        printf("geometry: custom-params prims=%d  default-params prims=%d\n",
               scene_a.geo.count, scene_c.geo.count);
        CHECK(scene_a.geo.count != scene_c.geo.count,
              "file generator params change the generated primitive count");
    }
    scene_free(&scene_c);

    /* --- 6. a plain-feature variant: same geometry, features disabled --- */
    if (checker_idx >= 0)
        desc.materials[checker_idx].mat.texture_kind = TEXTURE_NONE;
    desc.sky.sun_radius = 0.0;
    desc.camera.aperture = 0.0;
    desc.camera.focus_distance = 0.0;

    rc = scene_build_from_desc(&scene_b, &desc);
    CHECK(rc == 0, "scene_build_from_desc(plain features) returns 0");
    scene_desc_free(&desc);
    if (rc != 0)
        goto cleanup;
    CHECK(scene_b.geo.count == scene_a.geo.count,
          "plain-feature variant keeps identical geometry (only looks differ)");

    /* --- 7. allocate render buffers ------------------------------------ */
    rgb_a = (unsigned char *)malloc(nbytes);
    rgb_a2 = (unsigned char *)malloc(nbytes);
    rgb_b = (unsigned char *)malloc(nbytes);
    color_set = (unsigned char *)calloc((size_t)1 << 21, 1u); /* 2^24 bits */
    CHECK(rgb_a != NULL && rgb_a2 != NULL && rgb_b != NULL && color_set != NULL,
          "all working buffers allocated");
    if (rgb_a == NULL || rgb_a2 == NULL || rgb_b == NULL || color_set == NULL)
        goto cleanup;

    /* --- 8. render the full-feature scene (soft shadows + DOF + tex) --- */
    rc = render_image(&scene_a, &cam, W, H, SPP, DEPTH, rgb_a);
    CHECK(rc == 0, "render_image(full features) returns 0");
    if (rc != 0)
        goto cleanup;

    /* --- 9. non-degenerate pixel content ------------------------------- */
    {
        int distinct = count_distinct_colors(rgb_a, (size_t)W * (size_t)H,
                                             color_set);
        int all_same = 1;
        size_t k;
        for (k = 1; k < nbytes; ++k) {
            if (rgb_a[k] != rgb_a[0]) { all_same = 0; break; }
        }
        printf("rendered image: %d distinct colours\n", distinct);
        CHECK(distinct > 1, "rendered image has more than one distinct colour");
        CHECK(distinct > 4, "rendered image is clearly non-degenerate");
        CHECK(!all_same, "render buffer was actually written");
    }

    /* --- 10. determinism: soft shadows + DOF are reproducible ---------- */
    rc = render_image(&scene_a, &cam, W, H, SPP, DEPTH, rgb_a2);
    CHECK(rc == 0, "second full-feature render returns 0");
    if (rc == 0)
        CHECK(memcmp(rgb_a, rgb_a2, nbytes) == 0,
              "soft-shadow/DOF render is byte-identical across repeats");

    /* --- 11. the new features actually change the output --------------- */
    rc = render_image(&scene_b, &cam, W, H, SPP, DEPTH, rgb_b);
    CHECK(rc == 0, "render_image(plain features) returns 0");
    if (rc == 0) {
        int differ = (memcmp(rgb_a, rgb_b, nbytes) != 0);
        CHECK(differ,
              "texture/soft-shadow/DOF change the rendered image vs plain");
    }

    /* --- 12. write BOTH PPM and BMP ------------------------------------ */
    remove(TMP_PPM);
    remove(TMP_BMP);
    rc = ppm_write(TMP_PPM, rgb_a, W, H);
    CHECK(rc == 0, "ppm_write returns 0");
    rc = bmp_write(TMP_BMP, rgb_a, W, H);
    CHECK(rc == 0, "bmp_write returns 0");

    /* --- 13. independent on-disk PPM validation ------------------------ */
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
            CHECK(memcmp(buf + hlen, rgb_a, nbytes) == 0,
                  "PPM payload is the verbatim top-down RGB buffer");
            free(buf);
        }
    }

    /* --- 14. independent on-disk BMP validation ------------------------ */
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
                        if (src[x * 3 + 0] != rgb_a[si + 2] ||
                            src[x * 3 + 1] != rgb_a[si + 1] ||
                            src[x * 3 + 2] != rgb_a[si + 0]) {
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

    /* --- 15. cleanup --------------------------------------------------- */
cleanup:
    free(rgb_a);
    free(rgb_a2);
    free(rgb_b);
    free(color_set);
    scene_free(&scene_a);
    scene_free(&scene_b);
    scene_free(&scene_c);
    remove(TMP_PPM);
    remove(TMP_BMP);

    printf("\ntest_integration_features: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}
