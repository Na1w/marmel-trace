#ifndef TEXTURE_H
#define TEXTURE_H

/*
 * texture.h - Procedural, position-modulated textures for materials.
 *
 * Self-contained by design: depends ONLY on src/vec3.h and src/material.h.
 * It must NOT include geometry/scene/render headers (mirrors the dependency
 * rule stated in material.h), so the texture code can never see a Hit,
 * Primitive, Scene or Ray.
 *
 * There is no UV / primitive-local parameterisation in this raytracer, so the
 * textures are pure functions of the 3D WORLD-SPACE hit position. They are
 * fully deterministic: no globals, no I/O, no rand()/clock()/time().
 *
 * Colours returned here are LINEAR (no gamma encoding); tone mapping happens
 * downstream in the renderer, exactly like `Material.albedo`.
 */

#include "vec3.h"
#include "material.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 2D Image texture map                                                */
/* ------------------------------------------------------------------ */

typedef struct ImageTexture {
    int width;
    int height;
    unsigned char *rgb; /* row-major top-down RGB, 3 bytes per pixel */
} ImageTexture;

/* Allocate an image texture buffer (owned rgb). */
ImageTexture *texture_image_create(int width, int height);

/* Load a 24-bit or 32-bit BMP into an ImageTexture. Returns NULL on failure. */
ImageTexture *texture_image_load_bmp(const char *path);

/* Release an ImageTexture and its pixel data. Safe on NULL. */
void texture_image_free(ImageTexture *tex);

/* Sample an ImageTexture at UV coordinates (u, v) with bilinear filtering.
 * Wrap mode is repeat (tiling). Returned colour is LINEAR RGB. */
Vec3 texture_image_sample(const ImageTexture *tex, double u, double v);

/*
 * Modulated albedo of material `m` at world-space point `p` and UV `(u, v)`.
 *
 *   TEXTURE_NONE       -> returns m->albedo unchanged.
 *   TEXTURE_CHECKER    -> 3D checkerboard from floor(p * scale).
 *   TEXTURE_STRIPES    -> sinusoidal bands along world Y.
 *   TEXTURE_UV_CHECKER -> 2D checkerboard from floor(u * scale), floor(v * scale).
 *   TEXTURE_IMAGE      -> bilinear sample from m->texture_image at (u*scale, v*scale)
 *                         multiplied by m->albedo.
 */
Vec3 texture_albedo_uv(const Material *m, Vec3 p, double u, double v);

/*
 * Legacy wrapper: modulated albedo at world-space point `p` with u=0, v=0.
 */
Vec3 texture_albedo(const Material *m, Vec3 p);

/*
 * Modulated specular tint at world-space point `p`. Currently returns
 * `m->specular` unchanged (reserved extension point so the renderer can adopt
 * it without a signature change); `m == NULL` yields black.
 */
Vec3 texture_specular(const Material *m, Vec3 p);

#ifdef __cplusplus
}
#endif

#endif /* TEXTURE_H */
