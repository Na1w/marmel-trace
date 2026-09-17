#!/usr/bin/env python3
"""
Refined Masterpiece Space Scene matching user feedback:
1. Atmosphere completely encloses Earth in 360 degrees via perspective-corrected concentric shells.
2. The Sun has immense, blazing, incandescent golden-orange coronal bloom/glow (no flat yellow circle!).
3. Stars are realistic, faint, microscopic pinpoints with natural clustering and deep cosmic blackness.
4. Clean, smooth planets with zero artificial artifacts.
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
    random.seed(1337)
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
    sun_center = (-4.1, -0.40, 0.0)
    sun_dir = norm(sub(sun_center, cam_eye))

    # Sky: immense, blazing golden-orange coronal glow (natural bloom fading into deep black)
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.68 0.12
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 18.0
  sun_glow_strength  = 5.2
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Sun Materials (Seamless Blazing Incandescent Furnace) ---
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 6.6 4.8 0.72\n  roughness = 0.5"),
        ("mat_sun_flare", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 5.0 2.8 0.06\n  roughness = 0.5"),

        # --- Hero Planet Materials (Smooth Slate/Blue-Grey Body) ---
        ("mat_hero_body", "pbr = 1\n  albedo = 0.60 0.64 0.72\n  roughness = 0.42\n  metallic = 0.03"),

        # --- Concentric Atmosphere Shells (Enclosing Earth 360 degrees) ---
        ("mat_atmo_inner", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.55 2.60 6.20\n  roughness = 0.2"),
        ("mat_atmo_outer", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.25 1.40 4.80\n  roughness = 0.2"),

        # --- Transiting Moon (In front of Sun) ---
        ("mat_transiting_moon", "pbr = 1\n  albedo = 0.38 0.36 0.34\n  roughness = 0.85\n  metallic = 0.02"),

        # --- Upper Crescent Planet ---
        ("mat_upper_planet", "pbr = 1\n  albedo = 0.52 0.54 0.58\n  roughness = 0.75\n  metallic = 0.0"),

        # --- Authentic Microscopic Diamond Stars ---
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 4.5 4.5 5.0\n  roughness = 0.5"),
        ("mat_star_mid", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 7.5 7.5 8.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.5 6.0 14.0\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 11.0 11.0 11.0\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # =============================================================
    # 1. The Sun (Right Side: x = -4.1)
    # =============================================================
    # The Sun (Hero Solar Body on the Right: x = -4.1)
    sun_radius = 2.60
    prims.append(f"""sphere {{
  center   = {sun_center[0]:.3f} {sun_center[1]:.3f} {sun_center[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_core
}}""")

    # =============================================================
    # 2. Transiting Moon (Straddling Left Limb of the Sun)
    # =============================================================
    transit_pos = (-1.50, -0.15, -3.2)
    transit_radius = 0.54
    prims.append(f"""sphere {{
  center   = {transit_pos[0]:.3f} {transit_pos[1]:.3f} {transit_pos[2]:.3f}
  radius   = {transit_radius:.3f}
  material = mat_transiting_moon
}}""")

    # =============================================================
    # 3. Upper Crescent Planet (Above the Sun)
    # =============================================================
    upper_pos = (-2.1, 2.85, 5.2)
    upper_radius = 0.95
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_upper_planet
}}""")

    # =============================================================
    # 4. Hero Planet & 360-Degree Concentric Atmosphere
    # =============================================================
    hero_center = (3.6, -0.15, 0.0)
    hero_radius = 2.85

    # Main planet body (Clean, smooth slate blue-grey!)
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_hero_body
}}""")

    # 360-Degree Concentric Atmosphere Shells:
    # Using perspective correction: x' = x * (cam_dist + z_offset) / cam_dist
    # cam_dist = 10.0
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

    # =============================================================
    # 5. Authentic Astronomical Pinpoint Stars
    # =============================================================
    # Natural clustering: faint stars, microscopic radii, deep cosmic voids
    star_dist = 145.0
    for _ in range(220):
        # Cosmic cluster arc
        t_arc = random.uniform(-1.0, 1.0)
        spread = random.gauss(0, 0.28)

        sx = (-0.65 * t_arc + spread * 0.7) * 40.0 + random.gauss(0, 3.0)
        sy = (0.75 * t_arc + spread * 0.7) * 25.0 + random.gauss(0, 2.5)
        sz = 70.0 + random.uniform(5.0, 50.0)

        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-10.0, 20.0))

        roll = random.random()
        if roll < 0.72:
            mat = "mat_star_faint"
            sr = random.uniform(0.022, 0.038)
        elif roll < 0.88:
            mat = "mat_star_mid"
            sr = random.uniform(0.035, 0.052)
        elif roll < 0.96:
            mat = "mat_star_blue"
            sr = random.uniform(0.038, 0.058)
        else:
            mat = "mat_star_bright"
            sr = random.uniform(0.050, 0.075)

        prims.append(f"""sphere {{
  center   = {spos[0]:.2f} {spos[1]:.2f} {spos[2]:.2f}
  radius   = {sr:.3f}
  material = {mat}
}}""")

    output = "\n".join(lines) + "\n" + "\n\n".join(prims) + "\n"
    with open("scenes/space_solarsystem.scene", "w") as f:
        f.write(output)
    print(f"Generated scenes/space_solarsystem.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
