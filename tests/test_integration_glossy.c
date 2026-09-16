/*
 * tests/test_integration_glossy.c - End-to-end INTEGRATION test for
 *                                   ROUGHNESS-DRIVEN GLOSSY REFLECTIONS.
 *
 * Feature under test (src/material.{h,c} + src/render.c):
 *   The OPT-IN PBR path importance-samples a GGX lobe for the recursive
 *   mirror reflection, blurring it by `roughness`. The gate in
 *   `trace_hit()` (src/render.c) is taken iff
 *
 *       mm->pbr != 0 && m->reflectivity > 0.0 &&
 *       m->roughness > GLOSSY_ROUGHNESS_EPSILON          (1e-6)
 *
 *   otherwise the legacy SHARP Whitted reflection statements run verbatim.
 *   Key helpers: `material_roughness_to_alpha()` (the shared alpha =
 *   max(roughness^2, PBR_ALPHA_MIN) convention) and
 *   `material_sample_glossy_dir()` (deterministic GGX half-vector sampling).
 *
 * Unlike tests/test_glossy.c (which exercises the unit helpers plus a byte
 * identity gate), THIS test drives the WHOLE pipeline end-to-end exactly as
 * src/main.c wires it:
 *
 *   scene_desc_load_string -> scene_build_from_desc -> camera (parsed
 *     `camera` block) -> render_image / render_image_ex -> compare buffers.
 *
 * Controlled scene: a mirror-metal sphere (`reflectivity = 1.0`,
 * `metallic = 1.0`) sits over a high-contrast checkerboard floor. Because
 * `reflectivity = 1.0` the direct-lighting term is weighted out of the final
 * mix (`out = lerp(color, refl_col, 1.0) = refl_col`), so any difference
 * between a roughness > 0 and a roughness == 0 render is attributable to the
 * glossy REFLECTION blur alone (not to the direct-sun Cook-Torrance lobe).
 *
 * Assertions:
 *   (a) DIFFERS: `pbr = 1, roughness = 0.35, reflectivity = 1` differs from
 *       the otherwise-identical `roughness = 0.0` scene by a meaningful
 *       fraction of pixels (>= 10 %), a mean-absolute-error threshold
 *       (>= 2.0 grey levels) and a max-channel threshold (>= 20); the
 *       reflective region has strictly LOWER high-frequency energy (it really
 *       is a blur, not just a shift), and roughness 0.35 vs 0.6 differ.
 *   (b) BYTE-IDENTICAL: the default (no-flag) render is byte-identical to a
 *       second identical render; `render_image_ex(adaptive = 0)` is
 *       byte-identical to `render_image`; and non-PBR scenes are UNAFFECTED by
 *       roughness (`pbr = 0` roughness inert; `pbr = 1, roughness = 0` ==
 *       `pbr = 0` sharp reflection).
 *
 * Deterministic (no rand(), no clock) and fast (96x54, low spp).
 * The Makefile links every test source in tests/ against all project objects
 * EXCEPT src/main.o, so this file supplies its own `int main(void)` and returns
 * 0 on success / non-zero on any failure. C11, -Wall -Wextra clean.
 */

/* Feature-test macro so setenv()/unsetenv() are declared under -std=c11. */
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "scene.h"
#include "scene_desc.h"
#include "camera.h"
#include "render.h"
#include "vec3.h"
#include "material.h"

/* The compiled-in default scene (no `--scene` path), byte-identical to
 * scenes/default.scene. Header-only, so no extra translation unit. */
#include "default_scene_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Minimal test harness (same convention as the other integration tests) */
/* ------------------------------------------------------------------ */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Render configuration (small + fast, yet with a stable signal)        */
/* ------------------------------------------------------------------ */

#define W      96
#define H      54
#define SPP    4
#define DEPTH  4
#define NPIX   ((size_t)W * (size_t)H)
#define NBYTES (NPIX * 3u)

/*
 * Thresholds for the "differs" assertion. Empirically the blurred reflection
 * covers the whole metal sphere (~15-25 % of the frame), so a 10 % pixel
 * fraction is comfortably above noise yet far below the observed signal; the
 * mean-absolute-error and max-channel gates reject a change that is merely a
 * rounding wobble on a handful of pixels.
 */
#define GLOSSY_MIN_DIFF_PIXEL_FRAC 0.10   /* >= 10 % of pixels differ      */
#define GLOSSY_MIN_MEAN_ABS_DIFF   2.0    /* >= 2.0 grey levels per channel */
#define GLOSSY_MIN_MAX_CHANNEL_DIFF 20    /* at least one channel moves a lot */

/* ------------------------------------------------------------------ */
/* Scene construction: camera + sky + checker floor + mirror metal      */
/* ------------------------------------------------------------------ */

/*
 * Everything up to (and including) the shared `material metal {` header.
 * Only the metal material's PBR keys differ between variants, so every byte
 * difference is attributable to the roughness-driven glossy reflection.
 */
static const char *SCENE_PREFIX =
    "water_enabled = 0\n"
    "water_material = none\n"
    "camera {\n"
    "    eye = 0 4.2 4.6\n"
    "    target = 0 0.9 0\n"
    "    up = 0 1 0\n"
    "    vfov = 50\n"
    "    aperture = 0\n"
    "    focus_distance = 0\n"
    "}\n"
    "sky {\n"
    "    sun_dir = 0.35 0.72 -0.55\n"
    "    sun_color = 1 0.96 0.88\n"
    "    horizon_color = 0.8 0.88 1\n"
    "    zenith_color = 0.3 0.5 0.95\n"
    "    gradient_gamma = 0.65\n"
    "    sun_glow_exponent = 350\n"
    "    sun_glow_strength = 0.8\n"
    "    cloud_height = 120\n"
    "    cloud_scale = 0.0025\n"
    "    cloud_coverage = 0.5\n"
    "    cloud_softness = 0.12\n"
    "    cloud_sharpness = 1.5\n"
    "    cloud_octaves = 5\n"
    "    seed = 4242\n"
    "}\n"
    "material floor {\n"
    "    albedo = 0.9 0.9 0.9\n"
    "    specular = 0.05 0.05 0.05\n"
    "    shininess = 16\n"
    "    reflectivity = 0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    texture = checker\n"
    "    texture_scale = 1.5\n"
    "    texture_color_a = 0.95 0.95 0.95\n"
    "    texture_color_b = 0.06 0.08 0.12\n"
    "}\n"
    "material metal {\n"
    "    albedo = 0.95 0.77 0.34\n"
    "    specular = 0.5 0.5 0.5\n"
    "    shininess = 128\n"
    "    reflectivity = 1.0\n"
    "    transparency = 0\n"
    "    ior = 1\n"
    "    is_water = 0\n"
    "    metallic = 1.0\n";

/* Closes `material metal` and adds the geometry. */
static const char *SCENE_SUFFIX =
    "}\n"
    "plane {\n"
    "    point = 0 0 0\n"
    "    normal = 0 1 0\n"
    "    material = floor\n"
    "}\n"
    "sphere {\n"
    "    center = 0 1.5 0\n"
    "    radius = 1.5\n"
    "    material = metal\n"
    "}\n";

/* Build a scene string: SCENE_PREFIX + `metal_keys` + SCENE_SUFFIX. */
static char *make_scene(const char *metal_keys)
{
    size_t n = strlen(SCENE_PREFIX) + strlen(metal_keys) +
               strlen(SCENE_SUFFIX) + 1u;
    char *s = (char *)malloc(n);
    if (s == NULL) {
        return NULL;
    }
    s[0] = '\0';
    strcat(s, SCENE_PREFIX);
    strcat(s, metal_keys);
    strcat(s, SCENE_SUFFIX);
    return s;
}

/* ------------------------------------------------------------------ */
/* Render helpers (mirror src/main.c's wiring)                          */
/* ------------------------------------------------------------------ */

/* Parse `text`, build the scene, render it into `out`. Returns 0 on success. */
static int render_text(const char *text, const char *name,
                       unsigned char *out, size_t out_bytes)
{
    SceneDesc desc;
    Scene scene;
    Camera cam;
    char errbuf[256];
    int rc;

    errbuf[0] = '\0';
    scene_desc_init(&desc);
    rc = scene_desc_load_string(&desc, text, name, errbuf, sizeof errbuf);
    if (rc != 0) {
        fprintf(stderr, "  parse error (%s): %s\n", name, errbuf);
        scene_desc_free(&desc);
        return -1;
    }

    memset(&scene, 0, sizeof scene);
    rc = scene_build_from_desc(&scene, &desc);
    if (rc != 0) {
        fprintf(stderr, "  scene_build_from_desc(%s) failed\n", name);
        scene_desc_free(&desc);
        return -2;
    }

    /* Build the camera from the PARSED `camera` block, exactly as src/main.c
     * wires it (mirrors test_integration_pbr.c's camera_from_desc). */
    if (desc.camera.present) {
        cam = camera_create(desc.camera.eye, desc.camera.target,
                            desc.camera.up, desc.camera.vfov_deg,
                            (double)W / (double)H);
        cam.aperture = desc.camera.aperture;
        if (desc.camera.focus_distance > CAMERA_FOCUS_DISTANCE_DERIVED) {
            cam.focus_distance = desc.camera.focus_distance;
        }
    } else {
        Vec3 eye, target, up;
        double vfov = 0.0;
        scene_default_view(&eye, &target, &up, &vfov);
        cam = camera_create(eye, target, up, vfov, (double)W / (double)H);
    }
    scene_desc_free(&desc);

    memset(out, 0xAB, out_bytes);
    rc = render_image(&scene, &cam, W, H, SPP, DEPTH, out);
    scene_free(&scene);
    return rc;
}

/* Allocate + render a scene string. Caller frees. NULL on failure. */
static unsigned char *render_alloc(const char *text, const char *name)
{
    unsigned char *buf = (unsigned char *)malloc(NBYTES);
    if (buf == NULL) {
        return NULL;
    }
    if (render_text(text, name, buf, NBYTES) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* Image comparison metrics                                            */
/* ------------------------------------------------------------------ */

/* Number of pixels where ANY of the three channels differs. */
static size_t diff_pixel_count(const unsigned char *a, const unsigned char *b)
{
    size_t i, n = 0;
    for (i = 0; i < NPIX; ++i) {
        if (a[i * 3u + 0u] != b[i * 3u + 0u] ||
            a[i * 3u + 1u] != b[i * 3u + 1u] ||
            a[i * 3u + 2u] != b[i * 3u + 2u]) {
            ++n;
        }
    }
    return n;
}

/* Mean absolute per-channel difference, in grey levels (0..255). */
static double mean_abs_diff(const unsigned char *a, const unsigned char *b)
{
    size_t i;
    double sum = 0.0;
    for (i = 0; i < NBYTES; ++i) {
        sum += fabs((double)a[i] - (double)b[i]);
    }
    return sum / (double)NBYTES;
}

/* Largest per-channel absolute difference. */
static int max_abs_diff(const unsigned char *a, const unsigned char *b)
{
    size_t i;
    int m = 0;
    for (i = 0; i < NBYTES; ++i) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}

/*
 * High-frequency energy: mean absolute luminance gradient between
 * 4-neighbour pixels (total-variation style). A blurred reflection of the
 * checkerboard floor has strictly FEWER sharp edges, hence lower energy than
 * the mirror-sharp reflection.
 *
 * When `mask` is non-NULL only pixels (and neighbours) whose mask entry is
 * non-zero are counted, so the metric can be restricted to the reflective
 * region and is not swamped by the static, unaffected floor.
 */
static double high_freq_energy(const unsigned char *rgb, const unsigned char *mask)
{
    size_t x, y;
    double sum = 0.0;
    size_t count = 0;

    for (y = 0; y < H; ++y) {
        for (x = 0; x < W; ++x) {
            size_t idx = y * (size_t)W + x;
            size_t p = idx * 3u;
            double lum;
            if (mask != NULL && mask[idx] == 0) {
                continue;
            }
            lum = 0.2126 * (double)rgb[p + 0u] +
                  0.7152 * (double)rgb[p + 1u] +
                  0.0722 * (double)rgb[p + 2u];
            if (x + 1u < W) {
                size_t q = p + 3u;
                double lr = 0.2126 * (double)rgb[q + 0u] +
                            0.7152 * (double)rgb[q + 1u] +
                            0.0722 * (double)rgb[q + 2u];
                sum += fabs(lum - lr);
                ++count;
            }
            if (y + 1u < H) {
                size_t q = p + (size_t)W * 3u;
                double ld = 0.2126 * (double)rgb[q + 0u] +
                            0.7152 * (double)rgb[q + 1u] +
                            0.0722 * (double)rgb[q + 2u];
                sum += fabs(lum - ld);
                ++count;
            }
        }
    }
    return (count > 0u) ? (sum / (double)count) : 0.0;
}

/* Mark every pixel (1 byte per pixel) where ANY channel of a differs from b. */
static void diff_pixel_mask(const unsigned char *a, const unsigned char *b,
                            unsigned char *mask)
{
    size_t i;
    for (i = 0; i < NPIX; ++i) {
        mask[i] = (a[i * 3u + 0u] != b[i * 3u + 0u] ||
                   a[i * 3u + 1u] != b[i * 3u + 1u] ||
                   a[i * 3u + 2u] != b[i * 3u + 2u]) ? 1u : 0u;
    }
}

/* ------------------------------------------------------------------ */
/* (a) Glossy blur is observable end-to-end                            */
/* ------------------------------------------------------------------ */

static void test_glossy_differs_from_sharp(void)
{
    /* Only the metal material's PBR keys differ between the two variants.
     * reflectivity = 1.0 => the final mix is `refl_col` alone, so the
     * difference is the glossy REFLECTION blur, not the direct-sun lobe. */
    char *sharp_keys = (char *)"    pbr = 1\n    roughness = 0.0\n";
    char *rough_keys = (char *)"    pbr = 1\n    roughness = 0.35\n";
    char *rougher_keys = (char *)"    pbr = 1\n    roughness = 0.6\n";

    char *s_sharp = make_scene(sharp_keys);
    char *s_rough = make_scene(rough_keys);
    char *s_rougher = make_scene(rougher_keys);
    CHECK(s_sharp && s_rough && s_rougher, "glossy scene strings allocate");

    if (!s_sharp || !s_rough || !s_rougher) {
        free(s_sharp); free(s_rough); free(s_rougher);
        return;
    }

    unsigned char *b_sharp = render_alloc(s_sharp, "<glossy-sharp>");
    unsigned char *b_rough = render_alloc(s_rough, "<glossy-rough>");
    unsigned char *b_rougher = render_alloc(s_rougher, "<glossy-rougher>");
    CHECK(b_sharp && b_rough && b_rougher,
          "roughness=0, 0.35 and 0.6 PBR metal scenes all render");

    if (b_sharp && b_rough && b_rougher) {
        size_t dpix = diff_pixel_count(b_sharp, b_rough);
        double frac = (double)dpix / (double)NPIX;
        double mad = mean_abs_diff(b_sharp, b_rough);
        int mx = max_abs_diff(b_sharp, b_rough);

        printf("test_integration_glossy: (a) rough=0.35 vs sharp=0 : "
               "%zu/%zu px differ (%.1f%%), MAD=%.3f, max=%d\n",
               dpix, NPIX, frac * 100.0, mad, mx);

        /* The glossy render must DIFFER from the sharp mirror render. */
        CHECK(dpix > 0,
              "roughness>0 PBR metal render differs from roughness==0");
        CHECK(frac >= GLOSSY_MIN_DIFF_PIXEL_FRAC,
              "a meaningful fraction of pixels differ (>= 10%)");
        CHECK(mad >= GLOSSY_MIN_MEAN_ABS_DIFF,
              "mean absolute difference exceeds threshold (>= 2.0)");
        CHECK(mx >= GLOSSY_MIN_MAX_CHANNEL_DIFF,
              "at least one channel differs strongly (>= 20)");

        /* It is a genuine BLUR: within the region affected by the glossy lobe
         * (the reflective sphere), the sharp checker reflection carries far
         * more high-frequency energy than either rough render. Restricting to
         * the changed region keeps the metric from being swamped by the
         * static, unaffected floor. */
        unsigned char *mask = (unsigned char *)malloc(NPIX);
        CHECK(mask != NULL, "diff mask allocates");
        if (mask != NULL) {
            double e_sharp, e_rough, e_rougher;
            diff_pixel_mask(b_sharp, b_rough, mask);
            e_sharp = high_freq_energy(b_sharp, mask);
            e_rough = high_freq_energy(b_rough, mask);
            e_rougher = high_freq_energy(b_rougher, mask);
            printf("test_integration_glossy: (a) reflective-region high-freq "
                   "energy sharp=%.4f rough0.35=%.4f rough0.6=%.4f\n",
                   e_sharp, e_rough, e_rougher);
            CHECK(e_rough < e_sharp,
                  "glossy reflection has less high-frequency energy than sharp");
            CHECK(e_rougher < e_sharp,
                  "roughness=0.6 reflection is also smoother than sharp");

            /* The lobe is roughness-driven: 0.35 and 0.6 must themselves
             * produce different renders (wider lobe => different blur). */
            CHECK(diff_pixel_count(b_rough, b_rougher) > 0,
                  "roughness 0.35 vs 0.6 produce different glossy renders");
            free(mask);
        }

        /* Determinism: the rough scene renders byte-identically twice. */
        unsigned char *b_rough2 = render_alloc(s_rough, "<glossy-rough2>");
        CHECK(b_rough2 && diff_pixel_count(b_rough, b_rough2) == 0,
              "roughness>0 PBR render is byte-identical across repeated runs");
        free(b_rough2);
    }

    free(b_sharp); free(b_rough); free(b_rougher);
    free(s_sharp); free(s_rough); free(s_rougher);
}

/* ------------------------------------------------------------------ */
/* (b) Default / non-PBR renders are byte-identical (feature inactive)  */
/* ------------------------------------------------------------------ */

static void test_default_render_byte_identical(void)
{
    /* The built-in default scene carries NO `pbr` keys (feature inactive):
     * two identical renders must be BYTE-IDENTICAL. */
    unsigned char *d1 = render_alloc(DEFAULT_SCENE_TEXT, "<default-1>");
    unsigned char *d2 = render_alloc(DEFAULT_SCENE_TEXT, "<default-2>");
    CHECK(d1 && d2, "default scene renders twice");
    if (d1 && d2) {
        CHECK(memcmp(d1, d2, NBYTES) == 0,
              "default (no-flag) render is byte-identical to a second render");
        CHECK(diff_pixel_count(d1, d2) == 0,
              "default render: zero differing pixels");
    }
    free(d1); free(d2);

    /* render_image_ex(adaptive = 0) must be byte-identical to render_image,
     * i.e. the opt-in entry point leaves the fixed path untouched. */
    SceneDesc desc;
    Scene scene;
    Camera cam;
    char errbuf[256];
    RenderParams rp;

    errbuf[0] = '\0';
    scene_desc_init(&desc);
    CHECK(scene_desc_load_string(&desc, DEFAULT_SCENE_TEXT, "<default-ex>",
                                 errbuf, sizeof errbuf) == 0,
          "default scene parses for render_image_ex");
    memset(&scene, 0, sizeof scene);
    CHECK(scene_build_from_desc(&scene, &desc) == 0,
          "default scene builds for render_image_ex");
    scene_desc_free(&desc);

    cam = scene_default_camera(&scene);
    rp.samples_per_pixel = SPP;
    rp.max_depth = DEPTH;
    rp.adaptive = 0;
    rp.adaptive_max_spp = 0;
    rp.adaptive_tau = 0.0;

    unsigned char *ex = (unsigned char *)malloc(NBYTES);
    unsigned char *fixed = (unsigned char *)malloc(NBYTES);
    CHECK(ex && fixed, "render buffers allocate for render_image_ex");
    if (ex && fixed) {
        memset(ex, 0xCD, NBYTES);
        memset(fixed, 0xCD, NBYTES);
        CHECK(render_image_ex(&scene, &cam, W, H, &rp, ex) == 0,
              "render_image_ex(default) returns 0");
        CHECK(render_image(&scene, &cam, W, H, SPP, DEPTH, fixed) == 0,
              "render_image(default) returns 0");
        CHECK(memcmp(ex, fixed, NBYTES) == 0,
              "render_image_ex(adaptive=0) is byte-identical to render_image");
    }
    free(ex); free(fixed);
    scene_free(&scene);
}

static void test_nonpbr_scenes_unaffected(void)
{
    /* pbr = 0: `roughness` is inert, so a rough and a smooth material render
     * BYTE-IDENTICALLY (the legacy sharp reflection runs verbatim). */
    char *nonpbr_r0 = make_scene("    pbr = 0\n    roughness = 0.0\n");
    char *nonpbr_r8 = make_scene("    pbr = 0\n    roughness = 0.8\n");
    /* pbr = 1 with roughness = 0 must equal the pbr = 0 sharp reflection
     * (the mirror fallback short-circuit). */
    char *pbr_sharp = make_scene("    pbr = 1\n    roughness = 0.0\n");

    CHECK(nonpbr_r0 && nonpbr_r8 && pbr_sharp, "non-PBR scene strings allocate");
    if (!nonpbr_r0 || !nonpbr_r8 || !pbr_sharp) {
        free(nonpbr_r0); free(nonpbr_r8); free(pbr_sharp);
        return;
    }

    unsigned char *b_r0 = render_alloc(nonpbr_r0, "<nonpbr-r0>");
    unsigned char *b_r8 = render_alloc(nonpbr_r8, "<nonpbr-r8>");
    unsigned char *b_ps = render_alloc(pbr_sharp, "<pbr-sharp>");
    CHECK(b_r0 && b_r8 && b_ps, "non-PBR / sharp variants render");

    if (b_r0 && b_r8 && b_ps) {
        CHECK(memcmp(b_r0, b_r8, NBYTES) == 0,
              "non-PBR scene is byte-identical for roughness 0 vs 0.8");
        CHECK(memcmp(b_r0, b_ps, NBYTES) == 0,
              "pbr=1,roughness=0 is byte-identical to the pbr=0 sharp reflection");
    }
    free(b_r0); free(b_r8); free(b_ps);
    free(nonpbr_r0); free(nonpbr_r8); free(pbr_sharp);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    (void)setenv("RAYTRACER_NO_PROGRESS", "1", 1);

    test_glossy_differs_from_sharp();
    test_default_render_byte_identical();
    test_nonpbr_scenes_unaffected();

    printf("tests/test_integration_glossy: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
