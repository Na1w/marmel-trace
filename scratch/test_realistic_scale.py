#!/usr/bin/env python3
"""
Realistic astronomical scale & balanced solar corona:
1. The Sun at a realistic, elegant astronomical distance/scale (not a giant screen-filling ball).
2. Tightly controlled, refined golden-amber corona (sun_glow_exponent = 55.0, strength = 3.2).
3. Deep velvet black space visible between the celestial bodies.
4. Earth in the foreground with its complete 360-degree electric blue atmosphere.
5. Realistic faint diamond pinpoint stars.
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

    # Sun position in world space:
    # Placed further in the distance (z = 12.0) on screen-right (x = -5.2, y = 0.8)
    sun_pos = (-5.2, 0.8, 12.0)
    sun_dir = norm(sub(sun_pos, cam_eye))

    # Sky: refined, elegant corona (no more giant blown-out yellow wash)
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.82 0.32
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 55.0
  sun_glow_strength  = 3.4
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Sun (Incandescent Star in the Distance) ---
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 8.5 6.2 1.8\n  roughness = 0.5"),

        # --- Hero Planet (Earth / Exoplanet) ---
        ("mat_hero_body", "pbr = 1\n  albedo = 0.62 0.66 0.74\n  roughness = 0.40\n  metallic = 0.03"),

        # --- Concentric Atmosphere (360-degree enclosing halo) ---
        ("mat_atmo_inner", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.50 2.40 5.80\n  roughness = 0.2"),
        ("mat_atmo_outer", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.22 1.30 4.60\n  roughness = 0.2"),

        # --- Moon / Secondary Bodies ---
        ("mat_moon", "pbr = 1\n  albedo = 0.44 0.42 0.40\n  roughness = 0.85\n  metallic = 0.02"),
        ("mat_distant_planet", "pbr = 1\n  albedo = 0.52 0.54 0.58\n  roughness = 0.75\n  metallic = 0.0"),

        # --- Diamond Stars ---
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 4.5 4.5 5.0\n  roughness = 0.5"),
        ("mat_star_mid", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 7.5 7.5 8.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.5 6.0 14.0\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 11.0 11.0 11.0\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # 1. The Sun (Distant, radiant celestial star)
    # Radius 1.35 at distance 22 units: subtends a realistic ~3.5 degree angular size!
    sun_radius = 1.35
    prims.append(f"""sphere {{
  center   = {sun_pos[0]:.3f} {sun_pos[1]:.3f} {sun_pos[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_core
}}""")

    # 2. Transiting / Companion Moon near the Sun line-of-sight
    moon_pos = (-2.8, 0.4, 2.0)
    moon_radius = 0.62
    prims.append(f"""sphere {{
  center   = {moon_pos[0]:.3f} {moon_pos[1]:.3f} {moon_pos[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # 3. Distant Crescent Planet (upper background)
    upper_pos = (-1.6, 2.6, 14.0)
    upper_radius = 0.95
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_distant_planet
}}""")

    # 4. Hero Planet & 360-Degree Concentric Atmosphere
    hero_center = (3.4, -0.25, 0.0)
    hero_radius = 2.75

    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_hero_body
}}""")

    # Atmosphere perspective correction
    z_off1 = 0.12
    scale1 = (10.0 + z_off1) / 10.0
    prims.append(f"""sphere {{
  center   = {hero_center[0] * scale1:.3f} {hero_center[1] * scale1:.3f} {hero_center[2] + z_off1:.3f}
  radius   = {hero_radius + 0.038:.3f}
  material = mat_atmo_inner
}}""")

    z_off2 = 0.18
    scale2 = (10.0 + z_off2) / 10.0
    prims.append(f"""sphere {{
  center   = {hero_center[0] * scale2:.3f} {hero_center[1] * scale2:.3f} {hero_center[2] + z_off2:.3f}
  radius   = {hero_radius + 0.065:.3f}
  material = mat_atmo_outer
}}""")

    # 5. Authentic Astronomical Pinpoint Stars (Sub-pixel at distance 150.0)
    star_dist = 150.0
    for _ in range(280):
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
    with open("scratch/test_realistic_scale.scene", "w") as f:
        f.write(output)
    print(f"Generated scratch/test_realistic_scale.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
