#!/usr/bin/env python3
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
    sun_pos = (-7.0, 0.35, 11.0)
    sun_dir = norm(sub(sun_pos, cam_eye))

    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.80 0.30
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 200.0
  sun_glow_strength  = 1.10
  cloud_coverage     = 2.0
}}
""")

    materials = [
        # Sun core
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.5 2.4 0.35\n  roughness = 0.5"),

        # Earth: Completely diffuse planetary body - ZERO specular shine, NO billiard ball effect
        ("mat_earth_body", "pbr = 1\n  albedo = 0.22 0.48 0.78\n  roughness = 0.90\n  metallic = 0.0"),

        # Transiting Moon
        ("mat_moon", "pbr = 1\n  albedo = 0.42 0.40 0.38\n  roughness = 0.88\n  metallic = 0.02"),

        # Distant Companion Planet
        ("mat_distant_planet", "pbr = 1\n  albedo = 0.52 0.54 0.58\n  roughness = 0.78\n  metallic = 0.0"),

        # Stars
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.0 1.0 1.1\n  roughness = 0.5"),
        ("mat_star_mid", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.7 1.7 1.9\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.2 1.6 2.6\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 2.6 2.6 2.8\n  roughness = 0.5"),
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
    moon_pos = (-4.35, 0.12, 3.8)
    moon_radius = 0.40
    prims.append(f"""sphere {{
  center   = {moon_pos[0]:.3f} {moon_pos[1]:.3f} {moon_pos[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # 3. Distant Companion Planet
    upper_pos = (-1.8, 2.6, 11.0)
    upper_radius = 0.78
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_distant_planet
}}""")

    # 4. Hero Planet (Earth) - Clean, diffuse planetary sphere (NO shiny glass shell)
    hero_center = (3.5, -0.22, 0.0)
    hero_radius = 2.80

    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_earth_body
}}""")

    # 5. Stars
    star_dist = 160.0
    for _ in range(400):
        t_arc = random.uniform(-1.1, 1.1)
        spread = random.gauss(0, 0.32)

        sx = (-0.68 * t_arc + spread * 0.8) * 50.0 + random.gauss(0, 3.5)
        sy = (0.70 * t_arc + spread * 0.8) * 32.0 + random.gauss(0, 3.0)
        sz = 60.0 + random.uniform(5.0, 85.0)

        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-15.0, 25.0))

        roll = random.random()
        if roll < 0.75:
            mat = "mat_star_faint"
            sr = random.uniform(0.016, 0.026)
        elif roll < 0.90:
            mat = "mat_star_mid"
            sr = random.uniform(0.024, 0.034)
        elif roll < 0.97:
            mat = "mat_star_blue"
            sr = random.uniform(0.026, 0.038)
        else:
            mat = "mat_star_bright"
            sr = random.uniform(0.032, 0.046)

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
