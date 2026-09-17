#!/usr/bin/env python3
"""
Final refined production script for the Space Solar System scene:
- Scaled, realistic Sun placed at an authentic astronomical distance with balanced, non-blinding golden emission.
- Elegant, tight solar corona (exp=400.0, strength=0.35) preserving deep velvet-black cosmic space.
- Photorealistic transiting moon creating a dramatic celestial transit across the golden solar disk.
- Earth enveloped by a concentric physical transmissive atmosphere shell (Fresnel refraction & scattering)
  that naturally illuminates a blue limb on the daylight side while keeping the night side authentically dark.
- Delicate, warm continental city light networks on Earth's night side.
- Distant crescent companion planet for immense planetary scale and depth.
- Diamond pinpoint stars with varied stellar magnitudes and spectral classes.
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

    # Sun position in world space: placed at realistic astronomical distance
    sun_pos = (-4.8, 0.35, 11.5)
    sun_dir = norm(sub(sun_pos, cam_eye))

    # Toned down, elegant corona: tight exponent (400.0), restrained strength (0.35), warm gold
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.82 0.35
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 400.0
  sun_glow_strength  = 0.35
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Sun Materials: Rich golden incandescent star, well balanced and non-blinding ---
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 2.8 1.9 0.25\n  roughness = 0.5"),

        # --- Earth Materials: Smooth Oceanic Slate Body, Clean & Majestic ---
        ("mat_earth_body", "pbr = 1\n  albedo = 0.62 0.66 0.74\n  roughness = 0.36\n  metallic = 0.03"),

        # --- Concentric Physical Atmosphere: Transmissive Dielectric Shell ---
        # Wraps around Earth seamlessly; catches glancing sunlight with realistic cyan-blue Fresnel scattering
        ("mat_atmo_glass", "pbr = 0\n  albedo = 0.15 0.55 0.95\n  specular = 0.30 0.65 1.0\n  shininess = 90\n  transparency = 0.88\n  reflectivity = 0.12\n  ior = 1.04"),

        # --- Delicate Continental Night City Lights ---
        ("mat_city_lights", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.6 1.1 0.4\n  roughness = 0.6"),

        # --- Celestial Moons & Planets ---
        ("mat_moon", "pbr = 1\n  albedo = 0.46 0.44 0.42\n  roughness = 0.85\n  metallic = 0.02"),
        ("mat_distant_planet", "pbr = 1\n  albedo = 0.54 0.56 0.62\n  roughness = 0.75\n  metallic = 0.0"),

        # --- Authentic Astronomical Diamond Stars ---
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 4.5 4.5 5.0\n  roughness = 0.5"),
        ("mat_star_mid", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 7.5 7.5 8.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.5 6.0 14.0\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 11.0 11.0 11.0\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # 1. The Sun: Balanced astronomical scale
    sun_radius = 1.15
    prims.append(f"""sphere {{
  center   = {sun_pos[0]:.3f} {sun_pos[1]:.3f} {sun_pos[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_core
}}""")

    # 2. Transiting Moon: Dramatic celestial transit across the Sun
    moon_pos = (-3.25, 0.18, 3.8)
    moon_radius = 0.38
    prims.append(f"""sphere {{
  center   = {moon_pos[0]:.3f} {moon_pos[1]:.3f} {moon_pos[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # 3. Distant Companion Planet (upper background crescent)
    upper_pos = (-1.8, 2.6, 11.0)
    upper_radius = 0.78
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_distant_planet
}}""")

    # 4. Hero Planet (Earth) & Concentric Physical Atmosphere Shell
    hero_center = (3.5, -0.22, 0.0)
    hero_radius = 2.80

    # Base planet sphere
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_earth_body
}}""")

    # Truly concentric atmosphere shell enclosing Earth (same center)
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius + 0.035:.3f}
  material = mat_atmo_glass
}}""")

    # Continental night city light networks
    cam_to_earth = norm(sub(cam_eye, hero_center))
    continent_centers = [
        (-0.55, 0.25, 0.78),
        (-0.35, -0.15, 0.92),
        (-0.65, -0.40, 0.64),
        (-0.20, 0.45, 0.86),
    ]
    for c_dir in continent_centers:
        c_dir_norm = norm(c_dir)
        for _ in range(35):
            offset = (random.gauss(0, 0.12), random.gauss(0, 0.12), random.gauss(0, 0.08))
            pdir = norm(add(c_dir_norm, offset))
            if dot(pdir, cam_to_earth) > 0.15 and dot(pdir, sun_dir) < -0.10:
                cpos = add(hero_center, scale(pdir, hero_radius + 0.008))
                cr = random.uniform(0.012, 0.024)
                prims.append(f"""sphere {{
  center   = {cpos[0]:.3f} {cpos[1]:.3f} {cpos[2]:.3f}
  radius   = {cr:.3f}
  material = mat_city_lights
}}""")

    # 5. Astronomical Pinpoint Stars (Sub-pixel at distance 150.0)
    star_dist = 150.0
    for _ in range(320):
        t_arc = random.uniform(-1.0, 1.0)
        spread = random.gauss(0, 0.32)

        sx = (-0.65 * t_arc + spread * 0.7) * 45.0 + random.gauss(0, 3.5)
        sy = (0.70 * t_arc + spread * 0.7) * 28.0 + random.gauss(0, 3.0)
        sz = 60.0 + random.uniform(5.0, 65.0)

        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-10.0, 20.0))

        roll = random.random()
        if roll < 0.72:
            mat = "mat_star_faint"
            sr = random.uniform(0.022, 0.036)
        elif roll < 0.88:
            mat = "mat_star_mid"
            sr = random.uniform(0.035, 0.050)
        elif roll < 0.96:
            mat = "mat_star_blue"
            sr = random.uniform(0.038, 0.056)
        else:
            mat = "mat_star_bright"
            sr = random.uniform(0.048, 0.070)

        prims.append(f"""sphere {{
  center   = {spos[0]:.2f} {spos[1]:.2f} {spos[2]:.2f}
  radius   = {sr:.3f}
  material = {mat}
}}""")

    output = "\n".join(lines) + "\n" + "\n\n".join(prims) + "\n"
    with open("scenes/space_solarsystem.scene", "w") as f:
        f.write(output)
    print(f"Successfully generated scenes/space_solarsystem.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
