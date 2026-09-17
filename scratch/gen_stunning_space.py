#!/usr/bin/env python3
"""
Generate the definitive photorealistic space scene:
'Cosmic Odyssey: The Majesty of the Solar System'
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
    random.seed(1337)
    lines = []

    cam_eye = (-4.2, 3.2, -15.0)
    cam_target = (2.2, 0.8, 12.0)
    cam_up_approx = norm((0.05, 0.99, 0.02))

    cam_fwd = norm(sub(cam_target, cam_eye))
    cam_right = norm(cross(cam_fwd, cam_up_approx))
    cam_up = cross(cam_right, cam_fwd)

    lines.append(f"""camera {{
  eye    = {cam_eye[0]:.2f} {cam_eye[1]:.2f} {cam_eye[2]:.2f}
  target = {cam_target[0]:.2f} {cam_target[1]:.2f} {cam_target[2]:.2f}
  up     = {cam_up[0]:.4f} {cam_up[1]:.4f} {cam_up[2]:.4f}
  vfov   = 44.0
}}
""")

    # Place the Sun in upper-right corner with balanced margin
    u_sun = 0.80 * 0.718  # right offset
    v_sun = 0.75 * 0.404  # up offset
    sun_dir = norm(add(cam_fwd, add(scale(cam_right, u_sun), scale(cam_up, v_sun))))

    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.94 0.82
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 70.0
  sun_glow_strength  = 4.2
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Sun Core ---
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 90.0 76.0 42.0\n  roughness = 0.5"),

        # --- Earth Materials ---
        # Deep sapphire ocean with specular sheen
        ("mat_earth_ocean", "pbr = 1\n  albedo = 0.03 0.22 0.68\n  roughness = 0.12\n  metallic = 0.04"),
        # Polar Ice Caps: clean, brilliant white
        ("mat_earth_ice", "pbr = 1\n  albedo = 0.96 0.97 1.00\n  roughness = 0.30\n  metallic = 0.0"),
        # Continents: subtle ochre and lush green landmasses
        ("mat_earth_land", "pbr = 1\n  albedo = 0.20 0.38 0.18\n  roughness = 0.75\n  metallic = 0.0"),
        # Glowing city lights on dark side
        ("mat_city_lights", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 14.0 10.0 3.2\n  roughness = 0.5"),

        # --- Moon ---
        ("mat_moon", "pbr = 1\n  albedo = 0.54 0.54 0.56\n  texture = checker\n  texture_scale = 0.35\n  texture_color_a = 0.58 0.58 0.60\n  texture_color_b = 0.40 0.40 0.42\n  roughness = 0.94\n  metallic = 0.0"),
        ("mat_moon_mare", "pbr = 1\n  albedo = 0.22 0.22 0.24\n  roughness = 0.90\n  metallic = 0.0"),

        # --- Jupiter ---
        ("mat_jupiter", "pbr = 1\n  albedo = 0.88 0.78 0.62\n  texture = stripes\n  texture_scale = 1.35\n  texture_color_a = 0.96 0.88 0.74\n  texture_color_b = 0.68 0.42 0.24\n  roughness = 0.52\n  metallic = 0.0"),
        ("mat_jupiter_grs", "pbr = 1\n  albedo = 0.76 0.24 0.12\n  roughness = 0.55\n  metallic = 0.0"),
        ("mat_io", "pbr = 1\n  albedo = 0.86 0.76 0.24\n  roughness = 0.70\n  metallic = 0.0"),
        ("mat_europa", "pbr = 1\n  albedo = 0.88 0.90 0.94\n  roughness = 0.25\n  metallic = 0.0"),

        # --- Saturn ---
        ("mat_saturn", "pbr = 1\n  albedo = 0.92 0.84 0.64\n  texture = stripes\n  texture_scale = 0.80\n  texture_color_a = 0.95 0.88 0.70\n  texture_color_b = 0.80 0.70 0.50\n  roughness = 0.46\n  metallic = 0.0"),
        ("mat_saturn_ring_bright", "pbr = 1\n  albedo = 0.98 0.92 0.72\n  roughness = 0.52\n  metallic = 0.0"),
        ("mat_saturn_ring_a", "pbr = 1\n  albedo = 0.85 0.76 0.55\n  roughness = 0.58\n  metallic = 0.0"),
        ("mat_saturn_ring_b", "pbr = 1\n  albedo = 0.65 0.56 0.38\n  roughness = 0.62\n  metallic = 0.0"),

        # --- Mars ---
        ("mat_mars", "pbr = 1\n  albedo = 0.76 0.35 0.16\n  roughness = 0.82\n  metallic = 0.0"),
        ("mat_mars_ice", "pbr = 1\n  albedo = 0.95 0.96 0.99\n  roughness = 0.35\n  metallic = 0.0"),

        # --- Stars ---
        ("mat_star_white", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 16.0 16.0 16.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 8.0 14.0 26.0\n  roughness = 0.5"),
        ("mat_star_gold", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 20.0 16.0 7.0\n  roughness = 0.5"),
        ("mat_star_red", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 16.0 6.0 1.5\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # -------------------------------------------------------------
    # 0. The Sun (Dazzling Celestial Body)
    # -------------------------------------------------------------
    sun_pos = add(cam_eye, scale(sun_dir, 48.0))
    prims.append(f"""sphere {{
  center   = {sun_pos[0]:.2f} {sun_pos[1]:.2f} {sun_pos[2]:.2f}
  radius   = 2.2
  material = mat_sun_core
}}""")

    # -------------------------------------------------------------
    # 1. Earth: Blue Marble in the foreground
    # -------------------------------------------------------------
    earth_center = (3.2, -1.8, 1.0)
    earth_radius = 2.6

    # Ocean body
    prims.append(f"""sphere {{
  center   = {earth_center[0]:.3f} {earth_center[1]:.3f} {earth_center[2]:.3f}
  radius   = {earth_radius:.3f}
  material = mat_earth_ocean
}}""")

    # North Polar Ice Cap
    prims.append(f"""sphere {{
  center   = {earth_center[0]:.3f} {earth_center[1] + earth_radius * 0.90:.3f} {earth_center[2] - earth_radius * 0.18:.3f}
  radius   = 0.62
  material = mat_earth_ice
}}""")

    # South Polar Ice Cap
    prims.append(f"""sphere {{
  center   = {earth_center[0]:.3f} {earth_center[1] - earth_radius * 0.90:.3f} {earth_center[2] - earth_radius * 0.18:.3f}
  radius   = 0.52
  material = mat_earth_ice
}}""")

    # City lights on the dark night side:
    cam_to_earth = norm(sub(cam_eye, earth_center))
    for _ in range(85):
        theta = random.uniform(0, 2*math.pi)
        phi = random.uniform(-0.8, 0.8)
        px = math.cos(phi) * math.cos(theta)
        py = math.sin(phi)
        pz = math.cos(phi) * math.sin(theta)
        pdir = norm((px, py, pz))

        if dot(pdir, cam_to_earth) > 0.20 and dot(pdir, sun_dir) < -0.12:
            cpos = add(earth_center, scale(pdir, earth_radius + 0.015))
            cr = random.uniform(0.018, 0.036)
            prims.append(f"""sphere {{
  center   = {cpos[0]:.3f} {cpos[1]:.3f} {cpos[2]:.3f}
  radius   = {cr:.3f}
  material = mat_city_lights
}}""")

    # -------------------------------------------------------------
    # 2. The Moon (Luna)
    # -------------------------------------------------------------
    moon_center = (0.2, 1.4, 3.5)
    moon_radius = 0.75
    prims.append(f"""sphere {{
  center   = {moon_center[0]:.3f} {moon_center[1]:.3f} {moon_center[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # Basaltic maria
    prims.append(f"""sphere {{
  center   = {moon_center[0] - 0.18:.3f} {moon_center[1] + 0.14:.3f} {moon_center[2] - 0.68:.3f}
  radius   = 0.32
  material = mat_moon_mare
}}""")

    # -------------------------------------------------------------
    # 3. Saturn with Realistic Rings & Cassini Division
    # -------------------------------------------------------------
    saturn_center = (10.8, 4.2, 28.0)
    saturn_radius = 2.6
    prims.append(f"""sphere {{
  center   = {saturn_center[0]:.3f} {saturn_center[1]:.3f} {saturn_center[2]:.3f}
  radius   = {saturn_radius:.3f}
  material = mat_saturn
}}""")

    ring_normal = norm((0.25, 0.85, 0.46))
    u_axis = norm(cross(ring_normal, (0, 1, 0)))
    v_axis = cross(ring_normal, u_axis)

    # Realistic Saturn Rings with Cassini Division gap!
    # Ring B (inner bright): radii 3.4 to 4.4
    # Cassini Division: gap between 4.4 and 4.8!
    # Ring A (outer): radii 4.8 to 5.6
    ring_bands = [
        # B-Ring (Dense, brilliant golden core)
        (3.4, 260, "mat_saturn_ring_b"),
        (3.7, 280, "mat_saturn_ring_bright"),
        (4.0, 310, "mat_saturn_ring_bright"),
        (4.3, 330, "mat_saturn_ring_bright"),
        # --- Gap: 4.3 to 4.8 is the Cassini Division! ---
        # A-Ring (Outer ring system)
        (4.8, 370, "mat_saturn_ring_a"),
        (5.1, 390, "mat_saturn_ring_a"),
        (5.5, 420, "mat_saturn_ring_b"),
    ]

    for r_orb, count, mat in ring_bands:
        for i in range(count):
            angle = (2.0 * math.pi * i) / count
            pos = add(saturn_center,
                      add(scale(u_axis, r_orb * math.cos(angle)),
                          scale(v_axis, r_orb * math.sin(angle))))
            prims.append(f"""sphere {{
  center   = {pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f}
  radius   = 0.072
  material = {mat}
}}""")

    # -------------------------------------------------------------
    # 4. Jupiter & Galilean Moons
    # -------------------------------------------------------------
    jupiter_center = (-5.8, -0.6, 22.0)
    jupiter_radius = 3.5
    prims.append(f"""sphere {{
  center   = {jupiter_center[0]:.3f} {jupiter_center[1]:.3f} {jupiter_center[2]:.3f}
  radius   = {jupiter_radius:.3f}
  material = mat_jupiter
}}""")

    grs_pos = (jupiter_center[0] + 0.95, jupiter_center[1] - 0.78, jupiter_center[2] - 3.25)
    prims.append(f"""sphere {{
  center   = {grs_pos[0]:.3f} {grs_pos[1]:.3f} {grs_pos[2]:.3f}
  radius   = 0.46
  material = mat_jupiter_grs
}}""")

    prims.append(f"""sphere {{
  center   = {jupiter_center[0] - 4.5:.3f} {jupiter_center[1] + 0.5:.3f} {jupiter_center[2] - 2.6:.3f}
  radius   = 0.25
  material = mat_io
}}""")

    prims.append(f"""sphere {{
  center   = {jupiter_center[0] + 5.2:.3f} {jupiter_center[1] - 0.4:.3f} {jupiter_center[2] + 1.2:.3f}
  radius   = 0.20
  material = mat_europa
}}""")

    # -------------------------------------------------------------
    # 5. Mars (The Red Planet)
    # -------------------------------------------------------------
    mars_center = (2.2, 3.8, 24.0)
    mars_radius = 1.1
    prims.append(f"""sphere {{
  center   = {mars_center[0]:.3f} {mars_center[1]:.3f} {mars_center[2]:.3f}
  radius   = {mars_radius:.3f}
  material = mat_mars
}}""")

    prims.append(f"""sphere {{
  center   = {mars_center[0]:.3f} {mars_center[1] + 0.96:.3f} {mars_center[2] - 0.40:.3f}
  radius   = 0.25
  material = mat_mars_ice
}}""")

    # -------------------------------------------------------------
    # 6. The Milky Way: 600 Pinpoint Diamond Stars
    # -------------------------------------------------------------
    star_dist = 140.0
    for _ in range(600):
        t_arc = random.uniform(-1.0, 1.0)
        spread = random.gauss(0, 0.22)

        gx = -0.72 * t_arc + spread * 0.65 + random.gauss(0, 0.12)
        gy = 0.68 * t_arc + spread * 0.70 + random.gauss(0, 0.12)
        gz = 0.48 + random.uniform(0.1, 0.7)

        d = norm((gx, gy, gz))
        spos = scale(d, star_dist + random.uniform(-10.0, 20.0))

        roll = random.random()
        if roll < 0.42:
            mat = "mat_star_white"
            r = random.uniform(0.11, 0.18)
        elif roll < 0.72:
            mat = "mat_star_blue"
            r = random.uniform(0.13, 0.22)
        elif roll < 0.90:
            mat = "mat_star_gold"
            r = random.uniform(0.13, 0.24)
        else:
            mat = "mat_star_red"
            r = random.uniform(0.15, 0.26)

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
