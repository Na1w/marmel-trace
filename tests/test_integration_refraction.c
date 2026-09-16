/*
 * tests/test_integration_refraction.c - End-to-end integration test for the
 * newly implemented GENERAL REFRACTION / GLASS material support:
 *
 *   - the `type = glass` material preset (transparency 1.0, ior 1.5,
 *     reflectivity 0, is_water 0, beer_lambert 0, neutral absorption and
 *     deep_color) - a clear refractive medium that keeps the exact geometric
 *     normal (no water wave perturbation),
 *   - the per-material `beer_lambert` (0/1) flag that opts a transmissive
 *     NON-water material into Beer-Lambert depth absorption using its
 *     `absorption` + `deep_color` fields (src/render.c gates the
 *     `water_attenuate()` call on `m->is_water || m->beer_lambert`).
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

#define TMP_PPM "/tmp/rt_test_integration_refraction.ppm"
#define TMP_BMP "/tmp/rt_test_integration_refraction.bmp"

/* ------------------------------------------------------------------ */
/* The integration scene                                               */
/*                                                                     */
/* A clear `type = glass` lens and a TINTED glass sphere (beer_lambert */
/* = 1 + nonzero absorption/deep_color) sit in front of structured     */
/* background geometry: a checker-textured floor plus coloured opaque  */
/* spheres, under a sun/sky/ground.                                    */
/* ------------------------------------------------------------------ */

static const char *REFRACTION_SCENE =
    "# general refraction / glass integration scene\n"
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
    "# clear glass lens: the `type = glass` preset, no absorption\n"
    "material lens {\n"
    "    type = glass\n"
    "}\n"
    "\n"
    "# tinted glass: beer_lambert opts this transmissive material into\n"
    "# Beer-Lambert depth absorption via absorption + deep_color.\n"
    "material tinted {\n"
    "    type = glass\n"
    "    beer_lambert = 1\n"
    "    absorption = 2.0 0.8 0.4\n"
    "    deep_color = 0.05 0.25 0.45\n"
    "}\n"
    "\n"
    "material red {\n"
    "    albedo = 0.8 0.15 0.12\n"
    "    specular = 0.1 0.1 0.1\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n"
    "\n"
    "material green {\n"
    "    albedo = 0.15 0.7 0.2\n"
    "    specular = 0.1 0.1 0.1\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n"
    "\n"
    "material blue {\n"
    "    albedo = 0.15 0.25 0.85\n"
    "    specular = 0.1 0.1 0.1\n"
    "    shininess = 24\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    absorption = 0 0 0\n"
    "    deep_color = 0 0 0\n"
    "}\n"
    "\n"
    "# structured background: a checker floor\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = floor\n"
    "}\n"
    "\n"
    "# background geometry seen THROUGH the glass\n"
    "sphere {\n"
    "    center = 0 1.6 -7\n"
    "    radius = 1.8\n"
    "    material = blue\n"
    "}\n"
    "sphere {\n"
    "    center = -4 1.0 -4\n"
    "    radius = 1.2\n"
    "    material = red\n"
    "}\n"
    "sphere {\n"
    "    center = 4 1.0 -4\n"
    "    radius = 1.2\n"
    "    material = green\n"
    "}\n"
    "\n"
    "# the refractive foreground: clear lens + tinted (beer_lambert) glass\n"
    "sphere {\n"
    "    center = -2.2 2.2 0\n"
    "    radius = 2.0\n"
    "    material = lens\n"
    "}\n"
    "sphere {\n"
    "    center = 2.2 2.2 0\n"
    "    radius = 2.0\n"
    "    material = tinted\n"
    "}\n";

/* A minimal `type = water` scene for the is_water regression sanity check. */
static const char *WATER_SCENE =
    "material pond {\n"
    "    type = water\n"
    "}\n"
    "sphere {\n"
    "    center = 0 1 0\n"
    "    radius = 1\n"
    "    material = pond\n"
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
    SceneDesc desc_main, desc_off, desc_zero, desc_water;
    Scene scene_on, scene_off, scene_zero;
    Camera cam;
    unsigned char *rgb_on = NULL, *rgb_on2 = NULL, *rgb_off = NULL,
                  *rgb_zero = NULL, *color_set = NULL;
    size_t nbytes = (size_t)W * (size_t)H * 3u;
    char errbuf[256];
    int rc;
    int lens_idx = -1, tinted_idx = -1, floor_idx = -1;
    int water_idx = -1;

    /* Clean any leftovers from a previous aborted run. */
    remove(TMP_PPM);
    remove(TMP_BMP);

    scene_desc_init(&desc_main);
    scene_desc_init(&desc_off);
    scene_desc_init(&desc_zero);
    scene_desc_init(&desc_water);
    memset(&scene_on, 0, sizeof scene_on);
    memset(&scene_off, 0, sizeof scene_off);
    memset(&scene_zero, 0, sizeof scene_zero);

    /* --- 1. parse the refraction scene --------------------------------- */
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_main, REFRACTION_SCENE, "<refraction>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "scene_desc_load_string parses the refraction scene");
    if (rc != 0) {
        fprintf(stderr, "parse error: %s\n", errbuf);
        goto cleanup;
    }

    /* --- 2. the description carries the glass + background materials --- */
    CHECK(desc_main.material_count == 6,
          "parsed scene has six materials (floor/lens/tinted/red/green/blue)");

    lens_idx   = find_material(&desc_main, "lens");
    tinted_idx = find_material(&desc_main, "tinted");
    floor_idx  = find_material(&desc_main, "floor");
    CHECK(lens_idx >= 0, "`lens` material found");
    CHECK(tinted_idx >= 0, "`tinted` material found");
    CHECK(floor_idx >= 0, "`floor` material found");

    /* --- 3. `type = glass` preset semantics ---------------------------- */
    if (lens_idx >= 0) {
        const Material *m = &desc_main.materials[lens_idx].mat;
        CHECK(m->transparency == 1.0, "glass preset transparency == 1.0");
        CHECK(m->ior == 1.5, "glass preset ior == 1.5");
        CHECK(m->reflectivity == 0.0, "glass preset reflectivity == 0");
        CHECK(m->is_water == 0, "glass preset is_water == 0");
        CHECK(m->beer_lambert == 0, "glass preset beer_lambert == 0");
        CHECK(vec3_eq(m->absorption, vec3(0.0, 0.0, 0.0)),
              "glass preset absorption is neutral (0,0,0)");
        CHECK(vec3_eq(m->deep_color, vec3(0.5, 0.5, 0.5)),
              "glass preset deep_color is neutral (0.5,0.5,0.5)");
    }

    /* --- 4. the tinted glass opted into Beer-Lambert ------------------- */
    if (tinted_idx >= 0) {
        const Material *m = &desc_main.materials[tinted_idx].mat;
        CHECK(m->transparency == 1.0, "tinted glass keeps preset transparency 1.0");
        CHECK(m->ior == 1.5, "tinted glass keeps preset ior 1.5");
        CHECK(m->is_water == 0, "tinted glass is not water");
        CHECK(m->beer_lambert == 1, "`beer_lambert = 1` parsed on tinted glass");
        CHECK(vec3_nonzero(m->absorption),
              "tinted glass has a nonzero absorption");
        CHECK(vec3_nonzero(m->deep_color),
              "tinted glass has a nonzero deep_color");
    }

    /* --- 5. the checker floor really carries the procedural texture ---- */
    if (floor_idx >= 0) {
        const Material *m = &desc_main.materials[floor_idx].mat;
        CHECK(m->texture_kind == TEXTURE_CHECKER,
              "floor material carries the checker texture");
    }

    /* --- 6. camera block parsed ---------------------------------------- */
    CHECK(desc_main.camera.present == 1, "camera block present");
    cam = camera_from_desc(&desc_main.camera, W, H);

    /* --- 7. build the refraction scene --------------------------------- */
    rc = scene_build_from_desc(&scene_on, &desc_main);
    CHECK(rc == 0, "scene_build_from_desc(refraction) returns 0");
    if (rc != 0)
        goto cleanup;
    CHECK(scene_on.geo.count > 0, "built refraction scene has geometry");

    /* --- 8. variant scenes: identical except the tinted material ------- */
    /* desc_off: beer_lambert 0, absorption still nonzero. */
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_off, REFRACTION_SCENE, "<refraction>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "re-parse the scene for the beer_lambert=0 variant");
    /* desc_zero: beer_lambert 0 AND absorption zeroed. */
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_zero, REFRACTION_SCENE, "<refraction>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "re-parse the scene for the zero-absorption variant");
    if (rc != 0)
        goto cleanup;

    {
        int off_idx = find_material(&desc_off, "tinted");
        int zero_idx = find_material(&desc_zero, "tinted");
        CHECK(off_idx >= 0 && zero_idx >= 0,
              "both variant descriptions carry the tinted material");
        if (off_idx >= 0 && zero_idx >= 0) {
            desc_off.materials[off_idx].mat.beer_lambert = 0;
            desc_zero.materials[zero_idx].mat.beer_lambert = 0;
            desc_zero.materials[zero_idx].mat.absorption = vec3(0.0, 0.0, 0.0);
            CHECK(desc_off.materials[off_idx].mat.beer_lambert == 0 &&
                  vec3_nonzero(desc_off.materials[off_idx].mat.absorption),
                  "beer_lambert=0 variant keeps nonzero absorption");
            CHECK(vec3_eq(desc_zero.materials[zero_idx].mat.absorption,
                          vec3(0.0, 0.0, 0.0)),
                  "zero-absorption variant has absorption (0,0,0)");
        }
    }

    rc = scene_build_from_desc(&scene_off, &desc_off);
    CHECK(rc == 0, "scene_build_from_desc(beer_lambert=0) returns 0");
    rc = scene_build_from_desc(&scene_zero, &desc_zero);
    CHECK(rc == 0, "scene_build_from_desc(zero absorption) returns 0");
    if (rc != 0)
        goto cleanup;
    CHECK(scene_off.geo.count == scene_on.geo.count,
          "variant scene keeps identical geometry (only material flag differs)");
    CHECK(scene_zero.geo.count == scene_on.geo.count,
          "zero-absorption scene keeps identical geometry");

    /* --- 9. allocate render buffers ------------------------------------ */
    rgb_on   = (unsigned char *)malloc(nbytes);
    rgb_on2  = (unsigned char *)malloc(nbytes);
    rgb_off  = (unsigned char *)malloc(nbytes);
    rgb_zero = (unsigned char *)malloc(nbytes);
    color_set = (unsigned char *)calloc((size_t)1 << 21, 1u); /* 2^24 bits */
    CHECK(rgb_on != NULL && rgb_on2 != NULL && rgb_off != NULL &&
          rgb_zero != NULL && color_set != NULL,
          "all working buffers allocated");
    if (rgb_on == NULL || rgb_on2 == NULL || rgb_off == NULL ||
        rgb_zero == NULL || color_set == NULL)
        goto cleanup;

    /* --- 10. render the refraction scene ------------------------------- */
    rc = render_image(&scene_on, &cam, W, H, SPP, DEPTH, rgb_on);
    CHECK(rc == 0, "render_image(refraction) returns 0");
    if (rc != 0)
        goto cleanup;

    /* --- 11. non-degenerate pixel content ------------------------------ */
    {
        int distinct = count_distinct_colors(rgb_on, (size_t)W * (size_t)H,
                                             color_set);
        int all_same = 1;
        size_t k;
        for (k = 1; k < nbytes; ++k) {
            if (rgb_on[k] != rgb_on[0]) { all_same = 0; break; }
        }
        printf("refraction render: %d distinct colours\n", distinct);
        CHECK(distinct > 1, "rendered image has more than one distinct colour");
        CHECK(distinct > 4, "rendered image is clearly non-degenerate");
        CHECK(!all_same, "render buffer was actually written (not all identical)");
        CHECK(!buffer_is_all(rgb_on, nbytes, 0u),
              "render is not an all-black image");
    }

    /* --- 12. determinism: refraction render is reproducible ------------ */
    rc = render_image(&scene_on, &cam, W, H, SPP, DEPTH, rgb_on2);
    CHECK(rc == 0, "second refraction render returns 0");
    if (rc == 0)
        CHECK(memcmp(rgb_on, rgb_on2, nbytes) == 0,
              "refraction render is byte-identical across repeats (deterministic)");

    /* --- 13. SEMANTIC CHECK: beer_lambert actually changes the image --- */
    rc = render_image(&scene_off, &cam, W, H, SPP, DEPTH, rgb_off);
    CHECK(rc == 0, "render_image(beer_lambert=0) returns 0");
    if (rc == 0) {
        int differ = (memcmp(rgb_on, rgb_off, nbytes) != 0);
        printf("beer_lambert 1 vs 0: %s\n", differ ? "DIFFER" : "identical");
        CHECK(differ,
              "beer_lambert=1 with nonzero absorption changes the render");
    }

    /* --- 14. SEMANTIC CHECK: absorption ignored when beer_lambert == 0 - */
    rc = render_image(&scene_zero, &cam, W, H, SPP, DEPTH, rgb_zero);
    CHECK(rc == 0, "render_image(beer_lambert=0, zero absorption) returns 0");
    if (rc == 0) {
        int same = (memcmp(rgb_off, rgb_zero, nbytes) == 0);
        printf("beer_lambert=0 absorption A vs 0: %s\n",
               same ? "IDENTICAL" : "differ");
        CHECK(same,
              "with beer_lambert=0 nonzero absorption is ignored (== zero absorption)");
        /* And, transitively, the attenuation flag is what makes the
         * difference: the beer_lambert=1 render differs from BOTH. */
        CHECK(memcmp(rgb_on, rgb_zero, nbytes) != 0,
              "beer_lambert=1 render differs from the zero-absorption render");
    }

    /* --- 15. write BOTH PPM and BMP ------------------------------------ */
    remove(TMP_PPM);
    remove(TMP_BMP);
    rc = ppm_write(TMP_PPM, rgb_on, W, H);
    CHECK(rc == 0, "ppm_write returns 0");
    rc = bmp_write(TMP_BMP, rgb_on, W, H);
    CHECK(rc == 0, "bmp_write returns 0");

    /* --- 16. independent on-disk PPM validation ------------------------ */
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
            CHECK(memcmp(buf + hlen, rgb_on, nbytes) == 0,
                  "PPM payload is the verbatim top-down RGB buffer");
            free(buf);
        }
    }

    /* --- 17. independent on-disk BMP validation ------------------------ */
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
                        if (src[x * 3 + 0] != rgb_on[si + 2] ||
                            src[x * 3 + 1] != rgb_on[si + 1] ||
                            src[x * 3 + 2] != rgb_on[si + 0]) {
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

    /* --- 18. regression sanity: `type = water` still sets is_water ---- */
    errbuf[0] = '\0';
    rc = scene_desc_load_string(&desc_water, WATER_SCENE, "<water>",
                                errbuf, sizeof errbuf);
    CHECK(rc == 0, "scene_desc_load_string parses the `type = water` scene");
    if (rc == 0) {
        water_idx = find_material(&desc_water, "pond");
        CHECK(water_idx >= 0, "`pond` water material found");
        if (water_idx >= 0) {
            const Material *m = &desc_water.materials[water_idx].mat;
            CHECK(m->is_water == 1, "`type = water` still parses with is_water == 1");
            CHECK(m->ior == 1.33, "`type = water` preset keeps ior 1.33");
        }
        CHECK(desc_water.water_material == water_idx,
              "the resolved water_material points at the water material");
    }

    /* --- 19. cleanup --------------------------------------------------- */
cleanup:
    free(rgb_on);
    free(rgb_on2);
    free(rgb_off);
    free(rgb_zero);
    free(color_set);
    scene_free(&scene_on);
    scene_free(&scene_off);
    scene_free(&scene_zero);
    scene_desc_free(&desc_main);
    scene_desc_free(&desc_off);
    scene_desc_free(&desc_zero);
    scene_desc_free(&desc_water);
    remove(TMP_PPM);
    remove(TMP_BMP);

    printf("\ntest_integration_refraction: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail ? 1 : 0;
}
