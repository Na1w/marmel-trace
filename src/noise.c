/* noise.c — Deterministic procedural noise (value, Perlin gradient, fBm, turbulence).
 *
 * See noise.h for the contract. Everything here is a pure function of its arguments:
 * the same inputs always produce bit-identical outputs, on any platform with IEEE-754
 * doubles, and there is no global mutable state.
 *
 * Implementation notes
 * --------------------
 *  - `noise_hash()` mixes an explicit `seed` with integer lattice coordinates using a
 *    Wang/PCG-style 32-bit avalanche mix. `noise_rand01()` maps the result to [0,1] and
 *    `noise_rand_signed()` to [-1,1].
 *  - Value noise interpolates lattice values with the quintic fade curve 6t^5-15t^4+10t^3.
 *  - Perlin noise uses pseudo-random unit gradients per corner, dot-products them with the
 *    offset from the corner, interpolates with the same fade curve and applies the classical
 *    normalisation factor (sqrt(2) for 2D, 2/sqrt(3) for 3D) before clamping to [-1,1].
 */

#include "noise.h"

#include <math.h>
#include <stdint.h>

/* Local PI so we do not depend on the POSIX-only M_PI. */
#define NOISE_PI 3.14159265358979323846

/* ------------------------------------------------------------------ */
/* Hashing                                                             */
/* ------------------------------------------------------------------ */

/* 32-bit integer avalanche mix (Wang hash / PCG-style finaliser). */
static unsigned noise_mix(unsigned x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Deterministically combine a seed and three integer lattice coordinates. */
static unsigned noise_hash(int x, int y, int z, unsigned seed)
{
    unsigned h = seed + 0x9e3779b9U;
    h ^= (unsigned)x * 0x85ebca6bU;
    h ^= (unsigned)y * 0xc2b2ae35U;
    h ^= (unsigned)z * 0x27d4eb2fU;
    return noise_mix(h);
}

/* Pseudo-random double in [0, 1]. */
static double noise_rand01(int x, int y, int z, unsigned seed)
{
    return (double)noise_hash(x, y, z, seed) / (double)UINT32_MAX;
}

/* Pseudo-random double in [-1, 1]. */
static double noise_rand_signed(int x, int y, int z, unsigned seed)
{
    return noise_rand01(x, y, z, seed) * 2.0 - 1.0;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* Quintic fade curve: 6t^5 - 15t^4 + 10t^3 (C2 continuous). */
static double noise_fade(double t)
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

static double noise_lerp(double a, double b, double t)
{
    return a + t * (b - a);
}

static double noise_clamp(double v, double lo, double hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Value noise                                                         */
/* ------------------------------------------------------------------ */

double noise_value1(double x, unsigned seed)
{
    double fx = floor(x);
    int ix = (int)fx;
    double u = noise_fade(x - fx);

    double a = noise_rand_signed(ix, 0, 0, seed);
    double b = noise_rand_signed(ix + 1, 0, 0, seed);
    return noise_clamp(noise_lerp(a, b, u), -1.0, 1.0);
}

double noise_value2(double x, double y, unsigned seed)
{
    double fx = floor(x);
    double fy = floor(y);
    int ix = (int)fx;
    int iy = (int)fy;
    double u = noise_fade(x - fx);
    double v = noise_fade(y - fy);

    double c00 = noise_rand_signed(ix,     iy,     0, seed);
    double c10 = noise_rand_signed(ix + 1, iy,     0, seed);
    double c01 = noise_rand_signed(ix,     iy + 1, 0, seed);
    double c11 = noise_rand_signed(ix + 1, iy + 1, 0, seed);

    double x0 = noise_lerp(c00, c10, u);
    double x1 = noise_lerp(c01, c11, u);
    return noise_clamp(noise_lerp(x0, x1, v), -1.0, 1.0);
}

double noise_value3(double x, double y, double z, unsigned seed)
{
    double fx = floor(x);
    double fy = floor(y);
    double fz = floor(z);
    int ix = (int)fx;
    int iy = (int)fy;
    int iz = (int)fz;
    double u = noise_fade(x - fx);
    double v = noise_fade(y - fy);
    double w = noise_fade(z - fz);

    double c000 = noise_rand_signed(ix,     iy,     iz,     seed);
    double c100 = noise_rand_signed(ix + 1, iy,     iz,     seed);
    double c010 = noise_rand_signed(ix,     iy + 1, iz,     seed);
    double c110 = noise_rand_signed(ix + 1, iy + 1, iz,     seed);
    double c001 = noise_rand_signed(ix,     iy,     iz + 1, seed);
    double c101 = noise_rand_signed(ix + 1, iy,     iz + 1, seed);
    double c011 = noise_rand_signed(ix,     iy + 1, iz + 1, seed);
    double c111 = noise_rand_signed(ix + 1, iy + 1, iz + 1, seed);

    double x00 = noise_lerp(c000, c100, u);
    double x10 = noise_lerp(c010, c110, u);
    double x01 = noise_lerp(c001, c101, u);
    double x11 = noise_lerp(c011, c111, u);

    double y0 = noise_lerp(x00, x10, v);
    double y1 = noise_lerp(x01, x11, v);
    return noise_clamp(noise_lerp(y0, y1, w), -1.0, 1.0);
}

/* ------------------------------------------------------------------ */
/* Perlin gradient noise                                               */
/* ------------------------------------------------------------------ */

double noise_perlin2(double x, double y, unsigned seed)
{
    double fx = floor(x);
    double fy = floor(y);
    int ix = (int)fx;
    int iy = (int)fy;

    double rx = x - fx; /* offset from the lower-left corner */
    double ry = y - fy;
    double u = noise_fade(rx);
    double v = noise_fade(ry);

    /* Pseudo-random unit gradients at the four corners. */
    double a00 = noise_rand01(ix,     iy,     0, seed) * 2.0 * NOISE_PI;
    double a10 = noise_rand01(ix + 1, iy,     0, seed) * 2.0 * NOISE_PI;
    double a01 = noise_rand01(ix,     iy + 1, 0, seed) * 2.0 * NOISE_PI;
    double a11 = noise_rand01(ix + 1, iy + 1, 0, seed) * 2.0 * NOISE_PI;

    double d00 = cos(a00) * rx       + sin(a00) * ry;
    double d10 = cos(a10) * (rx - 1) + sin(a10) * ry;
    double d01 = cos(a01) * rx       + sin(a01) * (ry - 1);
    double d11 = cos(a11) * (rx - 1) + sin(a11) * (ry - 1);

    double x0 = noise_lerp(d00, d10, u);
    double x1 = noise_lerp(d01, d11, u);
    double val = noise_lerp(x0, x1, v);

    /* Classical 2D Perlin range is ~[-1/sqrt(2), 1/sqrt(2)]; scale then clamp. */
    return noise_clamp(val * 1.4142135623730951, -1.0, 1.0);
}

double noise_perlin3(double x, double y, double z, unsigned seed)
{
    double fx = floor(x);
    double fy = floor(y);
    double fz = floor(z);
    int ix = (int)fx;
    int iy = (int)fy;
    int iz = (int)fz;

    double rx = x - fx;
    double ry = y - fy;
    double rz = z - fz;
    double u = noise_fade(rx);
    double v = noise_fade(ry);
    double w = noise_fade(rz);

    /* Gradients for the eight corners. */
    double g[8][3];
    for (int i = 0; i < 8; ++i) {
        int cx = ix + (i & 1);
        int cy = iy + ((i >> 1) & 1);
        int cz = iz + ((i >> 2) & 1);
        double gx = noise_rand_signed(cx, cy, cz, seed);
        double gy = noise_rand_signed(cx, cy, cz, seed ^ 0x68bc21ebU);
        double gz = noise_rand_signed(cx, cy, cz, seed ^ 0x02e5be93U);
        double len = sqrt(gx * gx + gy * gy + gz * gz);
        if (len < 1e-12) {
            /* Degenerate direction: fall back to a fixed unit axis. Deterministic. */
            gx = 1.0;
            gy = 0.0;
            gz = 0.0;
        } else {
            gx /= len;
            gy /= len;
            gz /= len;
        }
        g[i][0] = gx;
        g[i][1] = gy;
        g[i][2] = gz;
    }

    /* Dot products with the offset from each corner. */
    double dot[8];
    for (int i = 0; i < 8; ++i) {
        double dx = rx - (double)(i & 1);
        double dy = ry - (double)((i >> 1) & 1);
        double dz = rz - (double)((i >> 2) & 1);
        dot[i] = g[i][0] * dx + g[i][1] * dy + g[i][2] * dz;
    }

    double x00 = noise_lerp(dot[0], dot[1], u); /* z = 0 face */
    double x10 = noise_lerp(dot[2], dot[3], u);
    double x01 = noise_lerp(dot[4], dot[5], u); /* z = 1 face */
    double x11 = noise_lerp(dot[6], dot[7], u);

    double y0 = noise_lerp(x00, x10, v);
    double y1 = noise_lerp(x01, x11, v);
    double val = noise_lerp(y0, y1, w);

    /* Classical 3D Perlin range is ~[-sqrt(3)/2, sqrt(3)/2]; scale then clamp. */
    return noise_clamp(val * 1.1547005383792515, -1.0, 1.0);
}

/* ------------------------------------------------------------------ */
/* fBm & turbulence                                                    */
/* ------------------------------------------------------------------ */

/* Resolve the effective octave count and parameters so degenerate inputs are safe. */
static int noise_octaves(int octaves)
{
    return (octaves <= 0) ? 1 : octaves;
}

static double noise_lacunarity(double lacunarity)
{
    return (lacunarity > 0.0) ? lacunarity : 2.0;
}

static double noise_gain(double gain)
{
    return (gain > 0.0) ? gain : 0.5;
}

double noise_fbm2(double x, double y, int octaves, double lacunarity, double gain,
                  unsigned seed)
{
    int n = noise_octaves(octaves);
    double lac = noise_lacunarity(lacunarity);
    double g = noise_gain(gain);

    double amp = 1.0;
    double freq = 1.0;
    double sum = 0.0;
    double norm = 0.0;

    for (int i = 0; i < n; ++i) {
        sum += amp * noise_perlin2(x * freq, y * freq, seed);
        norm += amp;
        amp *= g;
        freq *= lac;
    }
    if (norm <= 0.0) {
        return 0.0;
    }
    return noise_clamp(sum / norm, -1.0, 1.0);
}

double noise_fbm3(double x, double y, double z, int octaves, double lacunarity, double gain,
                  unsigned seed)
{
    int n = noise_octaves(octaves);
    double lac = noise_lacunarity(lacunarity);
    double g = noise_gain(gain);

    double amp = 1.0;
    double freq = 1.0;
    double sum = 0.0;
    double norm = 0.0;

    for (int i = 0; i < n; ++i) {
        sum += amp * noise_perlin3(x * freq, y * freq, z * freq, seed);
        norm += amp;
        amp *= g;
        freq *= lac;
    }
    if (norm <= 0.0) {
        return 0.0;
    }
    return noise_clamp(sum / norm, -1.0, 1.0);
}

double noise_turbulence2(double x, double y, int octaves, double lacunarity, double gain,
                         unsigned seed)
{
    int n = noise_octaves(octaves);
    double lac = noise_lacunarity(lacunarity);
    double g = noise_gain(gain);

    double amp = 1.0;
    double freq = 1.0;
    double sum = 0.0;
    double norm = 0.0;

    for (int i = 0; i < n; ++i) {
        sum += amp * fabs(noise_perlin2(x * freq, y * freq, seed));
        norm += amp;
        amp *= g;
        freq *= lac;
    }
    if (norm <= 0.0) {
        return 0.0;
    }
    return noise_clamp(sum / norm, 0.0, 1.0);
}
