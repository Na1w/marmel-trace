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

/*
 * Modulated albedo of material `m` at world-space point `p`.
 *
 *   TEXTURE_NONE    -> returns m->albedo unchanged (bit-for-bit).
 *   TEXTURE_CHECKER -> 3D checkerboard from the parity of floor(p * scale):
 *                      cells alternate between texture_color_a and
 *                      texture_color_b; the result is then multiplied by the
 *                      base albedo so the base colour still tints the pattern.
 *   TEXTURE_STRIPES -> sinusoidal bands along world Y, thresholded at the
 *                      zero crossing (a triangular/sine stripe); band A is
 *                      texture_color_a, band B is texture_color_b, again
 *                      multiplied by the base albedo.
 *
 * `m == NULL` yields black. A non-positive `texture_scale` is treated as the
 * default 1.0 (see TEXTURE_DEFAULT_SCALE), so a zero-initialised material is
 * always well-defined.
 */
Vec3 texture_albedo(const Material *m, Vec3 p);

/*
 * Modulated specular tint at world-space point `p`. Currently returns
 * `m->specular` unchanged (reserved extension point so the renderer can adopt
 * it without a signature change); `m == NULL` yields black.
 */
Vec3 texture_specular(const Material *m, Vec3 p);

/*
 * Perturbed shading normal for material `m` at world-space point `p`,
 * given geometric surface normal `n`.
 *
 * If `m->bump_strength <= 0`, returns `n` unchanged (bit-for-bit).
 * If `m->bump_strength > 0`, evaluates procedural displacement gradient and
 * perturbs `n` in the tangent plane.
 */
Vec3 texture_normal(const Material *m, Vec3 p, Vec3 n);

#ifdef __cplusplus
}
#endif

#endif /* TEXTURE_H */
