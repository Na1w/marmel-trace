#!/usr/bin/env python3
"""
Generate a high-fidelity recreation of 'space_ref.png':
- Sun on the RIGHT (blazing golden/yellow, warm corona).
- Transiting moon silhouetted against the lower-left of the Sun.
- Upper crescent planet above the Sun.
- Hero Planet on the LEFT:
  - Brilliant white/silver daylight rim facing the Sun.
  - Razor-thin electric-blue glowing atmospheric limb on the outer left edge.
  - Smooth, detailed slate/blue-grey surface.
- Inky black cosmic void with pinpoint stars.
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

    # Camera: look down Z axis.
    # Note: right vector is -X, so:
    # Negative X is SCREEN-RIGHT (Sun side)
    # Positive X is SCREEN-LEFT (Hero Planet side)
    cam_eye = (0.0, 0.0, -10.0)
    cam_target = (0.0, 0.0, 0.0)

    lines.append("""camera {
  eye    = 0.0 0.0 -10.0
  target = 0.0 0.0 0.0
  up     = 0.0 1.0 0.0
  vfov   = 40.0
}
""")

    # Sun position in world space:
    # On screen-right: x = -3.8, y = -0.4, z = 0.0
    sun_center = (-3.8, -0.4, 0.0)
    sun_dir = norm(sub(sun_center, cam_eye))

    # Sky: pure black void with warm golden-amber corona around the Sun
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.65 0.06
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 26.0
  sun_glow_strength  = 3.8
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Sun Materials ---
        # Base incandescent solar core: rich sunny golden-yellow (low blue = warm golden)
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 5.6 4.4 0.32\n  roughness = 0.5"),

        # --- Hero Planet Materials (Left Side) ---
        # Deep slate/blue-grey rocky body with high roughness and subtle texture
        ("mat_hero_body", "pbr = 1\n  albedo = 0.52 0.56 0.64\n  texture = checker\n  texture_scale = 0.22\n  texture_color_a = 0.58 0.62 0.70\n  texture_color_b = 0.44 0.48 0.55\n  roughness = 0.78\n  metallic = 0.0"),
        # Electric Blue Atmospheric Rim (outer left limb)
        ("mat_atmo_cyan", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.45 2.20 5.20\n  roughness = 0.2"),
        ("mat_atmo_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.25 1.40 4.50\n  roughness = 0.2"),

        # --- Transiting Moon (In front of Sun) ---
        ("mat_transiting_moon", "pbr = 1\n  albedo = 0.40 0.38 0.36\n  roughness = 0.85\n  metallic = 0.0"),

        # --- Upper Crescent Planet ---
        ("mat_upper_planet", "pbr = 1\n  albedo = 0.45 0.46 0.50\n  roughness = 0.82\n  metallic = 0.0"),

        # --- Pinpoint Stars ---
        ("mat_star_white", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 14.0 14.0 14.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 6.0 10.0 22.0\n  roughness = 0.5"),
        ("mat_star_gold", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 16.0 13.0 4.0\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # =============================================================
    # 1. The Sun (Right Side of Screen: x = -3.8)
    # =============================================================
    # Primitive #0: Area light source for the path tracer!
    sun_radius = 2.45
    prims.append(f"""sphere {{
  center   = {sun_center[0]:.3f} {sun_center[1]:.3f} {sun_center[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_core
}}""")

    # =============================================================
    # 2. Transiting Moon (In front of lower-left of the Sun)
    # =============================================================
    # Sits between camera and the Sun: x = -1.75, y = -0.45, z = -3.2
    transit_pos = (-1.75, -0.45, -3.2)
    transit_radius = 0.58
    prims.append(f"""sphere {{
  center   = {transit_pos[0]:.3f} {transit_pos[1]:.3f} {transit_pos[2]:.3f}
  radius   = {transit_radius:.3f}
  material = mat_transiting_moon
}}""")

    # =============================================================
    # 3. Upper Crescent Planet (Above the Sun)
    # =============================================================
    upper_pos = (-2.2, 2.1, 4.5)
    upper_radius = 0.82
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_upper_planet
}}""")

    # =============================================================
    # 4. Hero Planet (Left Side of Screen: x = +3.6)
    # =============================================================
    hero_center = (3.6, -0.2, 0.0)
    hero_radius = 2.75

    # Main planet body
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_hero_body
}}""")

    # Electric Blue Atmospheric Rim along outer limb (screen-left = +X):
    # Offset center slightly towards +X and +Z (behind), radius slightly larger:
    atmo_center = (hero_center[0] + 0.042, hero_center[1], hero_center[2] + 0.07)
    atmo_radius = hero_radius + 0.025
    prims.append(f"""sphere {{
  center   = {atmo_center[0]:.3f} {atmo_center[1]:.3f} {atmo_center[2]:.3f}
  radius   = {atmo_radius:.3f}
  material = mat_atmo_cyan
}}""")

    atmo_center2 = (hero_center[0] + 0.065, hero_center[1], hero_center[2] + 0.12)
    atmo_radius2 = hero_radius + 0.045
    prims.append(f"""sphere {{
  center   = {atmo_center2[0]:.3f} {atmo_center2[1]:.3f} {atmo_center2[2]:.3f}
  radius   = {atmo_radius2:.3f}
  material = mat_atmo_blue
}}""")

    # =============================================================
    # 5. Cosmic Starfield (Diamond Pinpoints in Void)
    # =============================================================
    star_dist = 65.0
    for _ in range(400):
        sx = random.uniform(-28.0, 28.0)
        sy = random.uniform(-18.0, 18.0)
        sz = random.uniform(25.0, 60.0)
        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-5.0, 15.0))

        roll = random.random()
        if roll < 0.60:
            mat = "mat_star_white"
            sr = random.uniform(0.06, 0.12)
        elif roll < 0.85:
            mat = "mat_star_blue"
            sr = random.uniform(0.08, 0.14)
        else:
            mat = "mat_star_gold"
            sr = random.uniform(0.08, 0.15)

        prims.append(f"""sphere {{
  center   = {spos[0]:.2f} {spos[1]:.2f} {spos[2]:.2f}
  radius   = {sr:.2f}
  material = {mat}
}}""")

    output = "\n".join(lines) + "\n" + "\n\n".join(prims) + "\n"
    with open("scenes/space_solarsystem.scene", "w") as f:
        f.write(output)
    print(f"Generated scenes/space_solarsystem.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
