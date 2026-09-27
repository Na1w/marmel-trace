#!/usr/bin/env python3
"""
Brighten the overall scene naturally:
1. Boost direct sunlight illumination: sky.sun_color raised from (1.0, 0.8, 0.3) to (1.8, 1.5, 0.9).
   This makes the sunlit crescents of Earth and the companion moon significantly brighter and more luminous.
2. Faint cosmic starlight ambient fill in sky: subtle deep-space indigo starlight (0.018 0.022 0.038)
   so the dark silhouettes of the planets are softly readable against the cosmos rather than disappearing.
3. Earth albedo boosted to vibrant, clean oceanic-atmospheric tones (0.35 0.60 0.88).
4. Stars slightly more luminous and sparkling across deep space.
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

    # Sun position in right quadrant
    sun_pos = (-7.0, 0.35, 11.0)
    sun_dir = norm(sub(sun_pos, cam_eye))

    # Sky with boosted solar illumination and subtle cosmic starlight ambiance
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.75 1.45 0.85
  sun_radius         = 0.0
  horizon_color      = 0.018 0.022 0.038
  zenith_color       = 0.012 0.016 0.028
  gradient_gamma     = 1.0
  sun_glow_exponent  = 180.0
  sun_glow_strength  = 1.35
  cloud_coverage     = 2.0
}}
""")

    materials = [
        # Sun core: warm, glowing incandescent star
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 4.2 2.8 0.45\n  roughness = 0.5"),

        # Earth: Luminous, vibrant oceanic-atmospheric planet, high roughness (diffuse, NO billiard ball reflection)
        ("mat_earth_body", "pbr = 1\n  albedo = 0.36 0.62 0.88\n  roughness = 0.88\n  metallic = 0.0"),

        # Transiting Moon: Realistic dark lunar regolith
        ("mat_moon", "pbr = 1\n  albedo = 0.38 0.36 0.34\n  roughness = 0.88\n  metallic = 0.02"),

        # Distant Companion Moon: Softly illuminated lunar sphere
        ("mat_distant_planet", "pbr = 1\n  albedo = 0.65 0.64 0.62\n  roughness = 0.75\n  metallic = 0.0"),

        # Sparkling diamond stars
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.4 1.4 1.6\n  roughness = 0.5"),
        ("mat_star_mid", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 2.2 2.2 2.5\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.6 2.2 3.6\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.5 3.5 3.8\n  roughness = 0.5"),
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

    # 3. Distant Companion Moon
    upper_pos = (-1.8, 2.6, 11.0)
    upper_radius = 0.78
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_distant_planet
}}""")

    # 4. Hero Planet (Earth)
    hero_center = (3.5, -0.22, 0.0)
    hero_radius = 2.80

    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_earth_body
}}""")

    # 5. Stars (420 stars)
    star_dist = 160.0
    for _ in range(420):
        t_arc = random.uniform(-1.1, 1.1)
        spread = random.gauss(0, 0.32)

        sx = (-0.68 * t_arc + spread * 0.8) * 50.0 + random.gauss(0, 3.5)
        sy = (0.70 * t_arc + spread * 0.8) * 32.0 + random.gauss(0, 3.0)
        sz = 60.0 + random.uniform(5.0, 85.0)

        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-15.0, 25.0))

        roll = random.random()
        if roll < 0.72:
            mat = "mat_star_faint"
            sr = random.uniform(0.016, 0.026)
        elif roll < 0.88:
            mat = "mat_star_mid"
            sr = random.uniform(0.024, 0.035)
        elif roll < 0.96:
            mat = "mat_star_blue"
            sr = random.uniform(0.026, 0.038)
        else:
            mat = "mat_star_bright"
            sr = random.uniform(0.034, 0.048)

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
