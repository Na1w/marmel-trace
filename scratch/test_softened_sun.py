#!/usr/bin/env python3
"""
Refine the scene independently of the reference:
1. Soften the Sun: reduce intensity/glare, tighter and more elegant corona (strength = 2.0, exp = 65.0),
   warm golden-amber tones instead of blinding white blowout.
2. Elevate Earth: rich vibrant ocean, realistic cloud systems, continent tones, and glowing night-side city lights.
3. Complete 360-degree atmospheric glow enclosing Earth.
4. Detailed companion moons and deep velvet cosmic void with sparkling stars.
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

def cross(a, b):
    return (a[1]*b[2] - a[2]*b[1],
            a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0])

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

    # Sun position in world space:
    sun_pos = (-4.8, 0.3, 8.5)
    sun_dir = norm(sub(sun_pos, cam_eye))

    # Softened, elegant corona: tighter halo (exp 65.0), restrained strength (2.2), warm gold
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.82 0.38
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 65.0
  sun_glow_strength  = 2.2
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Sun Materials (Softened, Warm, Non-Blinding Incandescent Body) ---
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 4.2 3.0 0.85\n  roughness = 0.5"),

        # --- Earth Materials ---
        # Ocean: deep royal sapphire blue with glossy water sheen
        ("mat_earth_ocean", "pbr = 1\n  albedo = 0.04 0.24 0.68\n  roughness = 0.16\n  metallic = 0.03"),
        # Swirling white clouds
        ("mat_earth_clouds", "pbr = 1\n  albedo = 0.94 0.96 1.00\n  texture = stripes\n  texture_scale = 0.85\n  texture_color_a = 0.98 0.98 1.00\n  texture_color_b = 0.12 0.28 0.60\n  roughness = 0.38\n  metallic = 0.0"),
        # Glowing city lights on the dark night side
        ("mat_city_lights", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 12.0 9.0 3.0\n  roughness = 0.5"),

        # --- Concentric Atmosphere (360-degree enclosing halo) ---
        ("mat_atmo_inner", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.45 2.20 5.60\n  roughness = 0.2"),
        ("mat_atmo_outer", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.20 1.20 4.20\n  roughness = 0.2"),

        # --- Moon Materials ---
        ("mat_moon", "pbr = 1\n  albedo = 0.48 0.48 0.50\n  texture = checker\n  texture_scale = 0.35\n  texture_color_a = 0.52 0.52 0.54\n  texture_color_b = 0.36 0.36 0.38\n  roughness = 0.92\n  metallic = 0.0"),
        ("mat_distant_planet", "pbr = 1\n  albedo = 0.56 0.58 0.64\n  roughness = 0.75\n  metallic = 0.0"),

        # --- Authentic Diamond Stars ---
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 4.5 4.5 5.0\n  roughness = 0.5"),
        ("mat_star_mid", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 7.5 7.5 8.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.5 6.0 14.0\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 11.0 11.0 11.0\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # 1. The Sun (Softened, well-defined radiant star)
    sun_radius = 1.60
    prims.append(f"""sphere {{
  center   = {sun_pos[0]:.3f} {sun_pos[1]:.3f} {sun_pos[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_core
}}""")

    # 2. Transiting Moon
    moon_pos = (-2.80, 0.10, 2.2)
    moon_radius = 0.60
    prims.append(f"""sphere {{
  center   = {moon_pos[0]:.3f} {moon_pos[1]:.3f} {moon_pos[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # 3. Distant Crescent Planet (upper background)
    upper_pos = (-1.8, 2.6, 11.0)
    upper_radius = 0.90
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_distant_planet
}}""")

    # 4. Hero Planet (Earth) & 360-Degree Concentric Atmosphere
    hero_center = (3.5, -0.22, 0.0)
    hero_radius = 2.80

    # Base planet with clouds and oceans
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_earth_clouds
}}""")

    # City lights on the night side:
    cam_to_earth = norm(sub(cam_eye, hero_center))
    for _ in range(90):
        theta = random.uniform(0, 2*math.pi)
        phi = random.uniform(-0.8, 0.8)
        px = math.cos(phi) * math.cos(theta)
        py = math.sin(phi)
        pz = math.cos(phi) * math.sin(theta)
        pdir = norm((px, py, pz))

        # Night side check: visible to camera AND facing away from Sun
        if dot(pdir, cam_to_earth) > 0.20 and dot(pdir, sun_dir) < -0.15:
            cpos = add(hero_center, scale(pdir, hero_radius + 0.012))
            cr = random.uniform(0.016, 0.034)
            prims.append(f"""sphere {{
  center   = {cpos[0]:.3f} {cpos[1]:.3f} {cpos[2]:.3f}
  radius   = {cr:.3f}
  material = mat_city_lights
}}""")

    # Atmosphere perspective-corrected concentric shells (enclosing 360 degrees)
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
    with open("scratch/test_softened_sun.scene", "w") as f:
        f.write(output)
    print(f"Generated scratch/test_softened_sun.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
