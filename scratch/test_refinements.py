#!/usr/bin/env python3
"""
Test refinements:
1. Concentric atmosphere enclosing Earth.
2. Intense radiant solar glow/bloom.
3. Realistic faint pinpoint stars (no chunky dots).
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

def generate():
    random.seed(42)
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

    sun_center = (-4.1, -0.40, 0.0)
    sun_dir = norm(sub(sun_center, cam_eye))

    # Sky: intense radiant golden-orange corona glow
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.74 0.16
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 16.0
  sun_glow_strength  = 5.2
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Sun Materials ---
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 6.2 4.2 0.35\n  roughness = 0.5"),
        ("mat_sun_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 7.5 5.8 0.65\n  roughness = 0.5"),
        ("mat_sun_gold", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 5.4 3.4 0.12\n  roughness = 0.5"),
        ("mat_sun_orange", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 4.2 1.8 0.02\n  roughness = 0.5"),

        # --- Hero Planet Materials ---
        ("mat_hero_body", "pbr = 1\n  albedo = 0.64 0.68 0.76\n  roughness = 0.45\n  metallic = 0.02"),
        # Concentric Atmosphere Shell
        ("mat_earth_atmosphere", "type = glass\n  ior = 1.03\n  beer_lambert = 1\n  absorption = 0.6 0.15 0.0\n  deep_color = 0.15 0.65 1.00"),

        # --- Transiting Moon ---
        ("mat_transiting_moon", "pbr = 1\n  albedo = 0.42 0.40 0.38\n  roughness = 0.85\n  metallic = 0.02"),

        # --- Upper Crescent Planet ---
        ("mat_upper_planet", "pbr = 1\n  albedo = 0.52 0.54 0.58\n  roughness = 0.75\n  metallic = 0.0"),

        # --- Realistic Faint Microscopic Stars ---
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 5.0 5.0 5.5\n  roughness = 0.5"),
        ("mat_star_medium", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 8.0 8.0 9.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 4.0 6.5 14.0\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 12.0 12.0 12.0\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # 1. The Sun
    sun_radius = 2.45  # Slightly smaller core so corona halo envelops it
    prims.append(f"""sphere {{
  center   = {sun_center[0]:.3f} {sun_center[1]:.3f} {sun_center[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_core
}}""")

    for _ in range(75):
        theta = random.uniform(0, 2*math.pi)
        phi = random.uniform(0.05, math.pi * 0.42)
        px = math.sin(phi) * math.cos(theta)
        py = math.sin(phi) * math.sin(theta)
        pz = -math.cos(phi)
        pdir = norm((px, py, pz))

        r_patch = random.uniform(0.35, 0.75)
        pos = add(sun_center, scale(pdir, sun_radius - r_patch + random.uniform(0.002, 0.005)))
        mat = "mat_sun_bright" if random.random() < 0.3 else "mat_sun_gold"

        prims.append(f"""sphere {{
  center   = {pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f}
  radius   = {r_patch:.3f}
  material = {mat}
}}""")

    # 2. Transiting Moon
    transit_pos = (-1.50, -0.15, -3.2)
    transit_radius = 0.54
    prims.append(f"""sphere {{
  center   = {transit_pos[0]:.3f} {transit_pos[1]:.3f} {transit_pos[2]:.3f}
  radius   = {transit_radius:.3f}
  material = mat_transiting_moon
}}""")

    # 3. Upper Crescent Planet
    upper_pos = (-2.1, 2.85, 5.2)
    upper_radius = 0.95
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_upper_planet
}}""")

    # 4. Hero Planet & CONCENTRIC Atmosphere
    hero_center = (3.6, -0.15, 0.0)
    hero_radius = 2.85

    # Core planet
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_hero_body
}}""")

    # Atmosphere: concentric shell completely enclosing Earth!
    atmo_radius = hero_radius + 0.055
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {atmo_radius:.3f}
  material = mat_earth_atmosphere
}}""")

    # 5. Realistic Microscopic Pinpoint Stars
    star_dist = 110.0
    for _ in range(260):
        sx = random.uniform(-45.0, 45.0)
        sy = random.uniform(-25.0, 25.0)
        sz = random.uniform(35.0, 90.0)
        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-10.0, 20.0))

        roll = random.random()
        if roll < 0.70:
            mat = "mat_star_faint"
            sr = random.uniform(0.025, 0.045)
        elif roll < 0.88:
            mat = "mat_star_medium"
            sr = random.uniform(0.040, 0.065)
        elif roll < 0.96:
            mat = "mat_star_blue"
            sr = random.uniform(0.045, 0.075)
        else:
            mat = "mat_star_bright"
            sr = random.uniform(0.060, 0.090)

        prims.append(f"""sphere {{
  center   = {spos[0]:.2f} {spos[1]:.2f} {spos[2]:.2f}
  radius   = {sr:.3f}
  material = {mat}
}}""")

    output = "\n".join(lines) + "\n" + "\n\n".join(prims) + "\n"
    with open("scratch/test_refinements.scene", "w") as f:
        f.write(output)
    print(f"Generated scratch/test_refinements.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
