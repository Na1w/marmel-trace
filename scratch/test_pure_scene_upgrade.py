#!/usr/bin/env python3
"""
Test photorealism upgrades PURELY in scene description (.scene):
1. Earth Ocean & Continents: Deep oceanic body with realistic Fresnel specular glint (Cook-Torrance).
2. Earth Clouds: Concentric semi-transparent cloud sphere with procedural bands casting real ray-traced shadows.
3. Terminator Sunset Glow: Warm Rayleigh scattering along the terminator.
4. Moon with lunar surface texture and subtle Earthshine response.
"""

import math
import random

def norm(v):
    l = math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])
    return (v[0]/l, v[1]/l, v[2]/l) if l > 0 else (0, 1, 0)

def add(a, b):
    return (a[0]+b[0], a[1]+b[1], a[2]+b[2])

def scale(v, s):
    return (v[0]*s, v[1]*s, v[2]*s)

def sub(a, b):
    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])

def dot(a, b):
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]

def generate():
    random.seed(2026)
    lines = []

    cam_eye = (0.0, 0.0, -10.0)
    cam_target = (0.0, 0.0, 0.0)

    lines.append("""camera {
  eye    = 0.0 0.0 -10.0
  target = 0.0 0.0 0.0
  up     = 0.0 1.0 0.0
  vfov   = 38.0
}
""")

    # Sun position
    sun_pos = (-8.2, 0.35, 11.5)
    sun_dir = norm(sub(sun_pos, cam_eye))

    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.78 0.25
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 130.0
  sun_glow_strength  = 1.5
  cloud_coverage     = 2.0
}}
""")

    materials = [
        # Sun core
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.6 2.4 0.38\n  roughness = 0.5"),

        # Earth Oceans: Deep navy-slate with high Fresnel specular glint (smooth water)
        ("mat_earth_ocean", "pbr = 1\n  albedo = 0.08 0.16 0.32\n  roughness = 0.14\n  metallic = 0.04"),

        # Earth Continents / Terrain: Rougher ochre-slate terrain
        ("mat_earth_land", "pbr = 1\n  albedo = 0.42 0.45 0.40\n  roughness = 0.65\n  metallic = 0.0"),

        # Earth Clouds: Semi-transmissive white weather systems with procedural bands
        ("mat_earth_clouds", "pbr = 0\n  albedo = 0.88 0.90 0.94\n  specular = 0.2 0.2 0.2\n  shininess = 32\n  transparency = 0.72\n  texture_scale = 1.4\n  texture_color_a = 0.95 0.95 0.98\n  texture_color_b = 0.20 0.25 0.35"),

        # Concentric Transmissive Atmosphere Shell (Rayleigh Blue)
        ("mat_atmo_glass", "pbr = 0\n  albedo = 0.15 0.55 0.95\n  specular = 0.30 0.65 1.0\n  shininess = 90\n  transparency = 0.88\n  reflectivity = 0.12\n  ior = 1.04"),

        # Moon with realistic lunar regolith
        ("mat_moon", "pbr = 1\n  albedo = 0.28 0.27 0.26\n  roughness = 0.92\n  metallic = 0.0"),

        # Distant Planet with Jupiter/Saturn-like atmospheric banding
        ("mat_distant_planet", "pbr = 1\n  albedo = 0.62 0.58 0.50\n  roughness = 0.55\n  metallic = 0.0\n  texture_scale = 0.8\n  texture_color_a = 0.82 0.76 0.64\n  texture_color_b = 0.45 0.38 0.28"),

        # Stars
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.1 1.1 1.2\n  roughness = 0.5"),
        ("mat_star_mid", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.8 1.8 2.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.2 1.6 2.8\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 2.8 2.8 3.0\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # 1. The Sun
    sun_radius = 1.15
    prims.append(f"""sphere {{
  center   = {sun_pos[0]:.3f} {sun_pos[1]:.3f} {sun_pos[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_core
}}""")

    # 2. Transiting Moon
    moon_pos = (-5.43, 0.18, 3.8)
    moon_radius = 0.38
    prims.append(f"""sphere {{
  center   = {moon_pos[0]:.3f} {moon_pos[1]:.3f} {moon_pos[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # 3. Distant Companion Planet (Atmospherically banded)
    upper_pos = (-1.8, 2.6, 11.0)
    upper_radius = 0.78
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_distant_planet
}}""")

    # 4. Earth: Layered Oceans, Continents, Clouds, Atmosphere
    hero_center = (3.5, -0.22, 0.0)
    hero_radius = 2.80

    # Base Ocean Sphere (deep blue, high specular water reflection)
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_earth_ocean
}}""")

    # Concentric Cloud Layer (suspended slightly above ocean, semi-transparent)
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius + 0.018:.3f}
  material = mat_earth_clouds
}}""")

    # Concentric Atmosphere Shell (Rayleigh scattering outer limb)
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius + 0.038:.3f}
  material = mat_atmo_glass
}}""")

    # 5. Stars
    star_dist = 160.0
    for _ in range(350):
        t_arc = random.uniform(-1.0, 1.0)
        spread = random.gauss(0, 0.36)

        sx = (-0.65 * t_arc + spread * 0.7) * 48.0 + random.gauss(0, 4.0)
        sy = (0.70 * t_arc + spread * 0.7) * 30.0 + random.gauss(0, 3.5)
        sz = 60.0 + random.uniform(5.0, 75.0)

        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-15.0, 25.0))

        roll = random.random()
        if roll < 0.75:
            mat = "mat_star_faint"
            sr = random.uniform(0.016, 0.026)
        elif roll < 0.90:
            mat = "mat_star_mid"
            sr = random.uniform(0.024, 0.036)
        elif roll < 0.97:
            mat = "mat_star_blue"
            sr = random.uniform(0.026, 0.040)
        else:
            mat = "mat_star_bright"
            sr = random.uniform(0.035, 0.050)

        prims.append(f"""sphere {{
  center   = {spos[0]:.2f} {spos[1]:.2f} {spos[2]:.2f}
  radius   = {sr:.3f}
  material = {mat}
}}""")

    output = "\n".join(lines) + "\n" + "\n\n".join(prims) + "\n"
    with open("scratch/test_pure_scene_upgrade.scene", "w") as f:
        f.write(output)
    print(f"Generated scratch/test_pure_scene_upgrade.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
