#!/usr/bin/env python3
"""
Test composition: Deep Cosmic Void, Brilliant Sun, and Illuminated Planets.
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

def cross(a, b):
    return (a[1]*b[2] - a[2]*b[1],
            a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0])

def generate():
    random.seed(42)
    lines = []

    # Camera: High perspective looking down the plane of the solar system
    lines.append("""camera {
  eye    = -5.0 4.5 -16.0
  target = 2.0 0.5 10.0
  up     = 0.05 0.98 0.05
  vfov   = 45.0
}
""")

    # Sun direction in the sky: Upper-left
    # Pure pitch black horizon and zenith!
    sun_dir = norm((-0.58, 0.40, 0.71))

    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.94 0.82
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 50.0
  sun_glow_strength  = 4.2
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Sun Core ---
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 55.0 48.0 28.0\n  roughness = 0.5"),

        # --- Earth Materials ---
        # Ocean: deep glossy royal blue
        ("mat_earth_ocean", "pbr = 1\n  albedo = 0.03 0.18 0.55\n  roughness = 0.15\n  metallic = 0.02"),
        # Polar Ice Caps: brilliant white
        ("mat_earth_ice", "pbr = 1\n  albedo = 0.95 0.96 0.99\n  roughness = 0.35\n  metallic = 0.0"),
        # Atmosphere Halo: glowing cyan limb
        ("mat_earth_atmo", "pbr = 1\n  albedo = 0.20 0.55 0.98\n  roughness = 0.20\n  metallic = 0.0"),

        # --- Moon ---
        ("mat_moon", "pbr = 1\n  albedo = 0.48 0.48 0.50\n  roughness = 0.92\n  metallic = 0.0"),

        # --- Mars ---
        ("mat_mars", "pbr = 1\n  albedo = 0.76 0.34 0.16\n  roughness = 0.82\n  metallic = 0.0"),
        ("mat_mars_ice", "pbr = 1\n  albedo = 0.92 0.94 0.98\n  roughness = 0.35\n  metallic = 0.0"),

        # --- Jupiter ---
        # Fine Jovian cloud bands (high frequency scale = 1.8)
        ("mat_jupiter", "pbr = 1\n  albedo = 0.88 0.78 0.64\n  texture = stripes\n  texture_scale = 1.8\n  texture_color_a = 0.96 0.88 0.74\n  texture_color_b = 0.68 0.42 0.25\n  roughness = 0.50\n  metallic = 0.0"),
        ("mat_jupiter_grs", "pbr = 1\n  albedo = 0.75 0.22 0.12\n  roughness = 0.55\n  metallic = 0.0"),
        ("mat_io", "pbr = 1\n  albedo = 0.85 0.75 0.22\n  roughness = 0.70\n  metallic = 0.0"),
        ("mat_europa", "pbr = 1\n  albedo = 0.88 0.90 0.94\n  roughness = 0.25\n  metallic = 0.0"),

        # --- Saturn ---
        # Delicate golden bands
        ("mat_saturn", "pbr = 1\n  albedo = 0.92 0.84 0.65\n  texture = stripes\n  texture_scale = 1.2\n  texture_color_a = 0.95 0.88 0.70\n  texture_color_b = 0.82 0.72 0.52\n  roughness = 0.45\n  metallic = 0.0"),
        ("mat_saturn_ring_bright", "pbr = 1\n  albedo = 0.98 0.92 0.72\n  roughness = 0.50\n  metallic = 0.0"),
        ("mat_saturn_ring_a", "pbr = 1\n  albedo = 0.84 0.75 0.54\n  roughness = 0.55\n  metallic = 0.0"),
        ("mat_saturn_ring_b", "pbr = 1\n  albedo = 0.65 0.55 0.36\n  roughness = 0.60\n  metallic = 0.0"),

        # --- Stars ---
        ("mat_star_white", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 15.0 15.0 15.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 8.0 12.0 22.0\n  roughness = 0.5"),
        ("mat_star_gold", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 18.0 15.0 7.0\n  roughness = 0.5"),
        ("mat_star_red", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 15.0 5.0 1.5\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # -------------------------------------------------------------
    # 0. The Sun Core Sphere (Area light & visual star)
    # -------------------------------------------------------------
    sun_pos = scale(sun_dir, 38.0)
    prims.append(f"""sphere {{
  center   = {sun_pos[0]:.2f} {sun_pos[1]:.2f} {sun_pos[2]:.2f}
  radius   = 1.8
  material = mat_sun_core
}}""")

    # -------------------------------------------------------------
    # 1. Earth & Moon in the foreground / midground
    # -------------------------------------------------------------
    # Earth at (1.5, -1.2, 0.0), radius 2.4
    earth_center = (1.5, -1.2, 0.0)
    earth_radius = 2.4

    # Ocean body
    prims.append(f"""sphere {{
  center   = {earth_center[0]:.3f} {earth_center[1]:.3f} {earth_center[2]:.3f}
  radius   = {earth_radius:.3f}
  material = mat_earth_ocean
}}""")

    # North Polar Ice Cap
    prims.append(f"""sphere {{
  center   = {earth_center[0]:.3f} {earth_center[1] + earth_radius * 0.88:.3f} {earth_center[2] - earth_radius * 0.25:.3f}
  radius   = 0.65
  material = mat_earth_ice
}}""")

    # South Polar Ice Cap
    prims.append(f"""sphere {{
  center   = {earth_center[0]:.3f} {earth_center[1] - earth_radius * 0.88:.3f} {earth_center[2] - earth_radius * 0.25:.3f}
  radius   = 0.55
  material = mat_earth_ice
}}""")

    # Moon
    moon_center = (-1.2, 1.8, 2.5)
    moon_radius = 0.68
    prims.append(f"""sphere {{
  center   = {moon_center[0]:.3f} {moon_center[1]:.3f} {moon_center[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # -------------------------------------------------------------
    # 2. Jupiter & Moons
    # -------------------------------------------------------------
    # Jupiter at (-5.5, -1.0, 20.0), radius 3.5
    jupiter_center = (-5.5, -1.0, 20.0)
    jupiter_radius = 3.5
    prims.append(f"""sphere {{
  center   = {jupiter_center[0]:.3f} {jupiter_center[1]:.3f} {jupiter_center[2]:.3f}
  radius   = {jupiter_radius:.3f}
  material = mat_jupiter
}}""")

    # Great Red Spot
    grs_pos = (jupiter_center[0] + 0.95, jupiter_center[1] - 0.85, jupiter_center[2] - 3.25)
    prims.append(f"""sphere {{
  center   = {grs_pos[0]:.3f} {grs_pos[1]:.3f} {grs_pos[2]:.3f}
  radius   = 0.45
  material = mat_jupiter_grs
}}""")

    # Moons
    prims.append(f"""sphere {{
  center   = {jupiter_center[0] - 4.5:.3f} {jupiter_center[1] + 0.4:.3f} {jupiter_center[2] - 2.5:.3f}
  radius   = 0.24
  material = mat_io
}}""")

    prims.append(f"""sphere {{
  center   = {jupiter_center[0] + 5.2:.3f} {jupiter_center[1] - 0.3:.3f} {jupiter_center[2] + 1.2:.3f}
  radius   = 0.20
  material = mat_europa
}}""")

    # -------------------------------------------------------------
    # 3. Saturn with Dense, Brilliant Golden Rings
    # -------------------------------------------------------------
    # Saturn at (9.5, 3.8, 28.0), radius 2.6
    saturn_center = (9.5, 3.8, 28.0)
    saturn_radius = 2.6
    prims.append(f"""sphere {{
  center   = {saturn_center[0]:.3f} {saturn_center[1]:.3f} {saturn_center[2]:.3f}
  radius   = {saturn_radius:.3f}
  material = mat_saturn
}}""")

    # Rings
    ring_normal = norm((0.26, 0.85, 0.45))
    u_axis = norm(cross(ring_normal, (0, 1, 0)))
    v_axis = cross(ring_normal, u_axis)

    ring_bands = [
        (3.4, 0.055, 110, "mat_saturn_ring_b"),
        (3.8, 0.060, 125, "mat_saturn_ring_bright"),
        (4.2, 0.065, 140, "mat_saturn_ring_bright"),
        (4.6, 0.060, 150, "mat_saturn_ring_a"),
        (5.0, 0.055, 160, "mat_saturn_ring_a"),
        (5.4, 0.050, 170, "mat_saturn_ring_b"),
    ]

    for r_orb, r_sph, count, mat in ring_bands:
        for i in range(count):
            angle = (2.0 * math.pi * i) / count
            pos = add(saturn_center,
                      add(scale(u_axis, r_orb * math.cos(angle)),
                          scale(v_axis, r_orb * math.sin(angle))))
            prims.append(f"""sphere {{
  center   = {pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f}
  radius   = {r_sph:.3f}
  material = {mat}
}}""")

    # -------------------------------------------------------------
    # 4. Mars (The Red Planet)
    # -------------------------------------------------------------
    mars_center = (1.8, 3.8, 22.0)
    mars_radius = 1.1
    prims.append(f"""sphere {{
  center   = {mars_center[0]:.3f} {mars_center[1]:.3f} {mars_center[2]:.3f}
  radius   = {mars_radius:.3f}
  material = mat_mars
}}""")

    # Mars North Polar Cap
    prims.append(f"""sphere {{
  center   = {mars_center[0]:.3f} {mars_center[1] + 0.98:.3f} {mars_center[2] - 0.40:.3f}
  radius   = 0.25
  material = mat_mars_ice
}}""")

    # -------------------------------------------------------------
    # 5. The Milky Way: 500 Pinpoint Diamond Stars
    # -------------------------------------------------------------
    star_dist = 120.0
    for _ in range(500):
        t_arc = random.uniform(-1.0, 1.0)
        spread = random.gauss(0, 0.24)

        gx = -0.72 * t_arc + spread * 0.65 + random.gauss(0, 0.12)
        gy = 0.68 * t_arc + spread * 0.70 + random.gauss(0, 0.12)
        gz = 0.50 + random.uniform(0.1, 0.7)

        d = norm((gx, gy, gz))
        spos = scale(d, star_dist + random.uniform(-10.0, 20.0))

        roll = random.random()
        if roll < 0.42:
            mat = "mat_star_white"
            r = random.uniform(0.10, 0.16)
        elif roll < 0.72:
            mat = "mat_star_blue"
            r = random.uniform(0.12, 0.20)
        elif roll < 0.90:
            mat = "mat_star_gold"
            r = random.uniform(0.12, 0.22)
        else:
            mat = "mat_star_red"
            r = random.uniform(0.14, 0.24)

        prims.append(f"""sphere {{
  center   = {spos[0]:.2f} {spos[1]:.2f} {spos[2]:.2f}
  radius   = {r:.2f}
  material = {mat}
}}""")

    output = "\n".join(lines) + "\n" + "\n\n".join(prims) + "\n"
    with open("scenes/space_solarsystem.scene", "w") as f:
        f.write(output)
    print(f"Generated scenes/space_solarsystem.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
