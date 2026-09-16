/* noise.h — Deterministic procedural noise (value, Perlin gradient, fBm, turbulence).
 *
 * Design contract
 * ---------------
 *  - Self-contained: depends only on the C standard library. No project headers.
 *  - Deterministic: the output is a pure function of (coordinates, parameters, seed).
 *    There is NO global mutable state; the seed is always an explicit argument.
 *  - No dynamic allocation, no rand(), no time(), no static mutable data.
 *
 * Output ranges
 * -------------
 *  - noise_value1/2/3    -> [-1, 1]
 *  - noise_perlin2/3     -> [-1, 1] (scaled by the classical normalisation factors and
 *                                    finally clamped to guarantee the bound)
 *  - noise_fbm2/3        -> ~[-1, 1] (normalised by the sum of octave amplitudes)
 *  - noise_turbulence2   -> [0, 1]   (normalised by the sum of octave amplitudes)
 *
 * fBm / turbulence use Perlin gradient noise as their base octave, matching the classical
 * definitions of fractal Brownian motion and Perlin turbulence.
 *
 * All angles use a locally defined PI so the module does not rely on M_PI (POSIX-only).
 */

#ifndef NOISE_H
#define NOISE_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- Value noise: returns a value in [-1, 1] --- */
double noise_value1(double x, unsigned seed);
double noise_value2(double x, double y, unsigned seed);
double noise_value3(double x, double y, double z, unsigned seed);

/* --- Perlin-style gradient noise: returns a value in [-1, 1] --- */
double noise_perlin2(double x, double y, unsigned seed);
double noise_perlin3(double x, double y, double z, unsigned seed);

/* --- Fractal Brownian motion (normalized so result is ~[-1, 1]) ---
 * octaves >= 1, lacunarity typically 2.0, gain typically 0.5.
 * Safe if octaves <= 0 (treated as 1). Non-positive lacunarity/gain are guarded
 * so the sum of amplitudes never degenerates. */
double noise_fbm2(double x, double y, int octaves, double lacunarity, double gain,
                  unsigned seed);
double noise_fbm3(double x, double y, double z, int octaves, double lacunarity, double gain,
                  unsigned seed);

/* --- Turbulence: sum of |noise| per octave, normalized to [0, 1] --- */
double noise_turbulence2(double x, double y, int octaves, double lacunarity, double gain,
                         unsigned seed);

#ifdef __cplusplus
}
#endif

#endif /* NOISE_H */
