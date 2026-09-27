/*
 * tests/test_anamorphic.c - Unit and integration tests for anamorphic lens features:
 *                           anamorphic oval bokeh squeeze, scene format keys, and
 *                           anamorphic horizontal streak lens flare filter.
 */

#include "camera.h"
#include "flare.h"
#include "scene_desc.h"
#include "vec3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); } \
} while (0)

static void test_camera_anamorphic_defaults(void)
{
    Camera cam = camera_create(vec3(0, 0, 5), vec3(0, 0, 0), vec3(0, 1, 0), 45.0, 16.0 / 9.0);
    CHECK(cam.anamorphic_squeeze == 1.0, "default camera squeeze is 1.0");
    CHECK(cam.flare.enabled == 0, "default camera flare is disabled");
    CHECK(cam.flare.intensity == FLARE_DEFAULT_INTENSITY, "default flare intensity matches constant");
}

static void test_anamorphic_oval_bokeh(void)
{
    /*
     * Build two cameras: spherical (squeeze = 1.0) and anamorphic (squeeze = 2.0)
     */
    Camera cam_sph = camera_create(vec3(0, 0, 5), vec3(0, 0, 0), vec3(0, 1, 0), 45.0, 1.0);
    cam_sph.aperture = 0.5;
    cam_sph.focus_distance = 5.0;
    cam_sph.anamorphic_squeeze = 1.0;

    Camera cam_ana = camera_create(vec3(0, 0, 5), vec3(0, 0, 0), vec3(0, 1, 0), 45.0, 1.0);
    cam_ana.aperture = 0.5;
    cam_ana.focus_distance = 5.0;
    cam_ana.anamorphic_squeeze = 2.0;

    double max_x_sph = 0.0, max_y_sph = 0.0;
    double max_x_ana = 0.0, max_y_ana = 0.0;

    /* Sample 1000 lens points */
    for (int i = 0; i < 1000; ++i) {
        double r1 = (double)(i % 100) / 100.0 + 0.005;
        double r2 = (double)(i / 100) / 10.0 + 0.05;
        if (r1 > 1.0) r1 = 0.99;
        if (r2 > 1.0) r2 = 0.99;

        Ray rs = camera_ray_dof(&cam_sph, 0.5, 0.5, r1, r2);
        Ray ra = camera_ray_dof(&cam_ana, 0.5, 0.5, r1, r2);

        Vec3 off_s = vec3_sub(rs.origin, cam_sph.position);
        Vec3 off_a = vec3_sub(ra.origin, cam_ana.position);

        if (fabs(off_s.x) > max_x_sph) max_x_sph = fabs(off_s.x);
        if (fabs(off_s.y) > max_y_sph) max_y_sph = fabs(off_s.y);

        if (fabs(off_a.x) > max_x_ana) max_x_ana = fabs(off_a.x);
        if (fabs(off_a.y) > max_y_ana) max_y_ana = fabs(off_a.y);

        /* Vertical extent should match spherical lens */
        CHECK(fabs(off_a.y - off_s.y) < 1e-12, "vertical lens offset is unchanged");
        /* Horizontal extent should be compressed exactly by 2x */
        CHECK(fabs(off_a.x * 2.0 - off_s.x) < 1e-12, "horizontal lens offset is compressed 2x");
    }

    CHECK(max_x_sph > 0.45 && max_x_sph <= 0.5001, "spherical max_x reaches aperture radius");
    CHECK(max_y_sph > 0.45 && max_y_sph <= 0.5001, "spherical max_y reaches aperture radius");

    CHECK(max_x_ana > 0.22 && max_x_ana <= 0.2501, "anamorphic max_x is compressed by 2x");
    CHECK(max_y_ana > 0.45 && max_y_ana <= 0.5001, "anamorphic max_y remains uncompressed");
}

static void test_anamorphic_scene_desc(void)
{
    const char *scene_txt =
        "camera {\n"
        "    eye = 0 2 10\n"
        "    target = 0 0 0\n"
        "    up = 0 1 0\n"
        "    vfov = 35\n"
        "    aperture = 0.2\n"
        "    anamorphic_squeeze = 2.0\n"
        "    lens_flare = 1\n"
        "    flare_intensity = 0.75\n"
        "    flare_threshold = 0.9\n"
        "    flare_streak_length = 0.3\n"
        "    flare_tint = 0.2 0.5 1.0\n"
        "}\n";

    SceneDesc desc;
    scene_desc_init(&desc);
    char errbuf[256];
    int rc = scene_desc_load_string(&desc, scene_txt, "<test_anamorphic>", errbuf, sizeof(errbuf));
    CHECK(rc == 0, "scene_desc_load_string parses anamorphic camera keys");
    CHECK(desc.camera.present == 1, "camera block is present");
    CHECK(desc.camera.anamorphic_squeeze == 2.0, "anamorphic_squeeze is 2.0");
    CHECK(desc.camera.flare.enabled == 1, "flare.enabled is 1");
    CHECK(fabs(desc.camera.flare.intensity - 0.75) < 1e-6, "flare.intensity is 0.75");
    CHECK(fabs(desc.camera.flare.threshold - 0.9) < 1e-6, "flare.threshold is 0.9");
    CHECK(fabs(desc.camera.flare.streak_length - 0.3) < 1e-6, "flare.streak_length is 0.3");
    CHECK(fabs(desc.camera.flare.tint.x - 0.2) < 1e-6, "flare.tint.x is 0.2");
    CHECK(fabs(desc.camera.flare.tint.y - 0.5) < 1e-6, "flare.tint.y is 0.5");
    CHECK(fabs(desc.camera.flare.tint.z - 1.0) < 1e-6, "flare.tint.z is 1.0");

    /* Test writer roundtrip */
    const char *tmp_path = "/tmp/test_anamorphic_out.scene";
    rc = scene_desc_write(&desc, tmp_path, errbuf, sizeof(errbuf));
    CHECK(rc == 0, "scene_desc_write succeeds");

    SceneDesc d2;
    scene_desc_init(&d2);
    rc = scene_desc_load(&d2, tmp_path, errbuf, sizeof(errbuf));
    CHECK(rc == 0, "read back written anamorphic scene");
    CHECK(d2.camera.anamorphic_squeeze == 2.0, "roundtrip anamorphic_squeeze matches");
    CHECK(d2.camera.flare.enabled == 1, "roundtrip flare.enabled matches");
    CHECK(fabs(d2.camera.flare.intensity - 0.75) < 1e-6, "roundtrip flare.intensity matches");

    scene_desc_free(&desc);
    scene_desc_free(&d2);
}

static void test_flare_filter(void)
{
    /* Test 1: invalid arguments */
    FlareParams params;
    flare_default_params(&params);
    params.enabled = 1;
    CHECK(flare_apply(NULL, 100, 100, &params) != 0, "null rgb fails cleanly");
    unsigned char dummy[3];
    CHECK(flare_apply(dummy, 0, 100, &params) != 0, "0 width fails cleanly");
    CHECK(flare_apply(dummy, 100, -5, &params) != 0, "negative height fails cleanly");
    CHECK(flare_apply(dummy, 100, 100, NULL) != 0, "null params fails cleanly");

    /* Test 2: below threshold -> image is completely unchanged */
    int w = 64, h = 16;
    unsigned char *img = (unsigned char *)malloc((size_t)w * (size_t)h * 3);
    memset(img, 100, (size_t)w * (size_t)h * 3); /* gray pixels, lum ~ 100/255 = 0.39 < 0.85 */
    params.threshold = 0.85;
    CHECK(flare_apply(img, w, h, &params) == 0, "flare_apply succeeds");
    int all_100 = 1;
    for (size_t i = 0; i < (size_t)w * (size_t)h * 3; ++i) {
        if (img[i] != 100) { all_100 = 0; break; }
    }
    CHECK(all_100, "pixels below threshold produce zero flare");

    /* Test 3: one bright light point at center of row 8 */
    img[((size_t)8 * (size_t)w + (size_t)32) * 3 + 0] = 255;
    img[((size_t)8 * (size_t)w + (size_t)32) * 3 + 1] = 255;
    img[((size_t)8 * (size_t)w + (size_t)32) * 3 + 2] = 255;

    CHECK(flare_apply(img, w, h, &params) == 0, "flare_apply on bright emitter succeeds");

    /* Row 8 should have a horizontal streak spreading left and right symmetrically */
    int left_px  = img[((size_t)8 * (size_t)w + (size_t)25) * 3 + 2]; /* Blue channel */
    int right_px = img[((size_t)8 * (size_t)w + (size_t)39) * 3 + 2]; /* Blue channel */
    CHECK(left_px > 100, "flare streak illuminates left of light source");
    CHECK(right_px > 100, "flare streak illuminates right of light source");
    CHECK(abs(left_px - right_px) <= 1, "horizontal streak is symmetric around the emitter");

    /* Adjacent rows (row 0) should remain untouched because the flare is strictly horizontal */
    CHECK(img[((size_t)0 * (size_t)w + (size_t)32) * 3 + 0] == 100, "unrelated rows remain untouched");

    free(img);
}

int main(void)
{
    printf("== test_anamorphic ==\n");
    test_camera_anamorphic_defaults();
    test_anamorphic_oval_bokeh();
    test_anamorphic_scene_desc();
    test_flare_filter();

    printf("test_anamorphic: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
