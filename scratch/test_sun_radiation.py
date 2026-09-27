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

    sun_pos = (-4.8, 0.35, 11.5)
    sun_dir = norm(sub(sun_pos, cam_eye))

    # Slightly increased solar radiation: corona strength raised from 0.35 to 0.70, exp 280.0
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.82 0.35
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 280.0
  sun_glow_strength  = 0.70
  cloud_coverage     = 2.0
}}
""")

    materials = [
        # Sun core with slightly boosted incandescent radiance
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.2 2.2 0.35\n  roughness = 0.5"),

        # Earth clean body
        ("mat_earth_body", "pbr = 1\n  albedo = 0.62 0.66 0.74\n  roughness = 0.36\n  metallic = 0.03"),

        # Concentric Transmissive Atmosphere Shell
        ("mat_atmo_glass", "pbr = 0\n  albedo = 0.15 0.55 0.95\n  specular = 0.30 0.65 1.0\n  shininess = 90\n  transparency = 0.88\n  reflectivity = 0.12\n  ior = 1.04"),

        # Moon & Companion Planet
        ("mat_moon", "pbr = 1\n  albedo = 0.46 0.44 0.42\n  roughness = 0.85\n  metallic = 0.02"),
        ("mat_distant_planet", "pbr = 1\n  albedo = 0.54 0.56 0.62\n  roughness = 0.75\n  metallic = 0.0"),

        # Subdued stars
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
    moon_pos = (-3.25, 0.18, 3.8)
    moon_radius = 0.38
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

    # 4. Earth - Clean, pure planetary sphere
    hero_center = (3.5, -0.22, 0.0)
    hero_radius = 2.80

    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_earth_body
}}""")

    # Concentric physical atmosphere shell enclosing Earth
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius + 0.035:.3f}
  material = mat_atmo_glass
}}""")

    # 5. Realistic Subdued Astronomical Stars
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
    with open("scenes/space_solarsystem.scene", "w") as f:
        f.write(output)
    print(f"Generated scenes/space_solarsystem.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
