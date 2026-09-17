#!/usr/bin/env python3
"""
Masterpiece Space Scene:
'Cosmic Majesty: The Solar System and the Milky Way'
"""

import math
import random

def norm(v):
    l = math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])
    return (v[0]/l, v[1]/l, v[2]/l) if l > 0 else (0, 1, 0)

def add(a, b):
    return (a[0]+b[0], a[1]+b[1], a[2]+b[2])

def sub(a, b):
    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])

def scale(v, s):
    return (v[0]*s, v[1]*s, v[2]*s)

def dot(a, b):
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]

def cross(a, b):
    return (a[1]*b[2] - a[2]*b[1],
            a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0])

def generate():
    random.seed(2026)
    lines = []

    # Camera setup:
    # Placed to frame the Sun in the upper-left, Earth and Moon in foreground,
    # Saturn and Jupiter in midground, all catching brilliant sunlight!
    lines.append("""camera {
  eye    = -3.5 2.2 -14.0
  target = 1.0 0.2 12.0
  up     = 0.05 0.99 0.02
  vfov   = 46.0
}
""")

    # Sun direction in the sky:
    # Upper left in view: dir roughly (-0.52, 0.42, 0.74)
    sun_dir = norm((-0.52, 0.42, 0.74))

    # Subtle starlight fill (0.008) so shadows show spherical form rather than dead black clipping
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.95 0.85
  sun_radius         = 0.0
  horizon_color      = 0.006 0.008 0.016
  zenith_color       = 0.010 0.012 0.024
  gradient_gamma     = 1.0
  sun_glow_exponent  = 55.0
  sun_glow_strength  = 3.8
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Earth Materials ---
        # Ocean base: vibrant deep ocean blue
        ("mat_earth_ocean", "pbr = 1\n  albedo = 0.02 0.14 0.44\n  roughness = 0.15\n  metallic = 0.04"),
        # Continents: rich green and ochre landmasses
        ("mat_earth_land", "pbr = 1\n  albedo = 0.18 0.28 0.14\n  texture = checker\n  texture_scale = 0.6\n  texture_color_a = 0.14 0.32 0.12\n  texture_color_b = 0.38 0.30 0.16\n  roughness = 0.82\n  metallic = 0.0"),
        # Cloud layer: brilliant white swirl belts
        ("mat_earth_clouds", "pbr = 1\n  albedo = 0.95 0.96 1.00\n  texture = stripes\n  texture_scale = 0.32\n  texture_color_a = 0.98 0.98 1.00\n  texture_color_b = 0.12 0.25 0.52\n  roughness = 0.35\n  metallic = 0.0"),
        # Glowing city lights on night side
        ("mat_city_lights", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 8.0 5.5 2.0\n  roughness = 0.5"),
        # Atmospheric blue limb
        ("mat_earth_atmosphere", "pbr = 1\n  albedo = 0.18 0.48 0.92\n  roughness = 0.12\n  metallic = 0.0"),

        # --- Moon ---
        ("mat_moon", "pbr = 1\n  albedo = 0.52 0.52 0.54\n  texture = checker\n  texture_scale = 0.3\n  texture_color_a = 0.54 0.54 0.56\n  texture_color_b = 0.35 0.35 0.37\n  roughness = 0.92\n  metallic = 0.0"),
        ("mat_moon_mare", "pbr = 1\n  albedo = 0.22 0.22 0.23\n  roughness = 0.88\n  metallic = 0.0"),

        # --- Jupiter ---
        # Rich, majestic ammonia cream and red-brown bands
        ("mat_jupiter", "pbr = 1\n  albedo = 0.90 0.80 0.65\n  texture = stripes\n  texture_scale = 0.85\n  texture_color_a = 0.98 0.88 0.72\n  texture_color_b = 0.62 0.35 0.20\n  roughness = 0.50\n  metallic = 0.0"),
        ("mat_jupiter_grs", "pbr = 1\n  albedo = 0.75 0.22 0.12\n  roughness = 0.55\n  metallic = 0.0"),
        ("mat_io", "pbr = 1\n  albedo = 0.84 0.74 0.24\n  roughness = 0.72\n  metallic = 0.0"),
        ("mat_europa", "pbr = 1\n  albedo = 0.88 0.90 0.94\n  roughness = 0.25\n  metallic = 0.0"),

        # --- Saturn ---
        # Pale golden honey gas giant
        ("mat_saturn", "pbr = 1\n  albedo = 0.92 0.84 0.64\n  texture = stripes\n  texture_scale = 0.38\n  texture_color_a = 0.95 0.88 0.70\n  texture_color_b = 0.78 0.68 0.48\n  roughness = 0.45\n  metallic = 0.0"),
        ("mat_saturn_ring_bright", "pbr = 1\n  albedo = 0.98 0.90 0.70\n  roughness = 0.55\n  metallic = 0.0"),
        ("mat_saturn_ring_a", "pbr = 1\n  albedo = 0.82 0.72 0.52\n  roughness = 0.60\n  metallic = 0.0"),
        ("mat_saturn_ring_b", "pbr = 1\n  albedo = 0.64 0.54 0.36\n  roughness = 0.65\n  metallic = 0.0"),

        # --- Mars ---
        ("mat_mars", "pbr = 1\n  albedo = 0.72 0.34 0.16\n  roughness = 0.84\n  metallic = 0.0"),
        ("mat_mars_polar", "pbr = 1\n  albedo = 0.95 0.96 0.99\n  roughness = 0.35\n  metallic = 0.0"),

        # --- Milky Way Stars ---
        ("mat_star_white", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 14.0 14.0 14.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 6.0 9.0 18.0\n  roughness = 0.5"),
        ("mat_star_gold", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 16.0 13.0 6.0\n  roughness = 0.5"),
        ("mat_star_red", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 14.0 5.0 1.5\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # -------------------------------------------------------------
    # 1. Earth: Blue Marble in the foreground
    # -------------------------------------------------------------
    # Placed at (2.2, -1.8, 1.5), radius 3.2
    # The sun hits it from upper-left, showing a gorgeous, wide illuminated crescent/hemisphere!
    earth_center = (2.4, -2.0, 1.8)
    earth_radius = 3.2

    prims.append(f"""sphere {{
  center   = {earth_center[0]:.3f} {earth_center[1]:.3f} {earth_center[2]:.3f}
  radius   = {earth_radius:.3f}
  material = mat_earth_clouds
}}""")

    # City lights on the dark night side:
    # Camera eye: (-3.5, 2.2, -14.0)
    # Earth to camera vector: sub(eye, earth_center)
    cam_to_earth = norm(sub((-3.5, 2.2, -14.0), earth_center))
    for _ in range(80):
        # Sample points on the hemisphere visible to camera
        theta = random.uniform(0, 2*math.pi)
        phi = random.uniform(-0.8, 0.8)
        px = math.cos(phi) * math.cos(theta)
        py = math.sin(phi)
        pz = math.cos(phi) * math.sin(theta)
        pdir = norm((px, py, pz))

        # Check: visible to camera AND in shadow from Sun
        if dot(pdir, cam_to_earth) > 0.25 and dot(pdir, sun_dir) < -0.15:
            cpos = add(earth_center, scale(pdir, earth_radius + 0.012))
            cr = random.uniform(0.016, 0.035)
            prims.append(f"""sphere {{
  center   = {cpos[0]:.3f} {cpos[1]:.3f} {cpos[2]:.3f}
  radius   = {cr:.3f}
  material = mat_city_lights
}}""")

    # -------------------------------------------------------------
    # 2. The Moon (Luna)
    # -------------------------------------------------------------
    # Orbiting to the left/above Earth
    moon_center = (-0.8, 1.2, 3.5)
    moon_radius = 0.88
    prims.append(f"""sphere {{
  center   = {moon_center[0]:.3f} {moon_center[1]:.3f} {moon_center[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # Dark basaltic mare
    mare_pos = (moon_center[0] - 0.22, moon_center[1] + 0.15, moon_center[2] - 0.80)
    prims.append(f"""sphere {{
  center   = {mare_pos[0]:.3f} {mare_pos[1]:.3f} {mare_pos[2]:.3f}
  radius   = 0.32
  material = mat_moon_mare
}}""")

    # -------------------------------------------------------------
    # 3. Saturn with Dense, Brilliant Golden Rings
    # -------------------------------------------------------------
    # In midground right: (11.5, 4.5, 34.0), radius 2.6
    saturn_center = (10.5, 4.2, 32.0)
    saturn_radius = 2.5
    prims.append(f"""sphere {{
  center   = {saturn_center[0]:.3f} {saturn_center[1]:.3f} {saturn_center[2]:.3f}
  radius   = {saturn_radius:.3f}
  material = mat_saturn
}}""")

    # Rings: tilted gracefully so the sunlit top face is angled right into the camera!
    # Ring normal tilted by ~25 degrees
    ring_normal = norm((0.28, 0.84, 0.46))
    u_axis = norm(cross(ring_normal, (0, 1, 0)))
    v_axis = cross(ring_normal, u_axis)

    # 6 dense concentric bands
    ring_bands = [
        (3.3, 0.055, 100, "mat_saturn_ring_b"),
        (3.7, 0.060, 115, "mat_saturn_ring_bright"),
        (4.1, 0.065, 130, "mat_saturn_ring_bright"),
        (4.5, 0.060, 140, "mat_saturn_ring_a"),
        (4.9, 0.055, 150, "mat_saturn_ring_a"),
        (5.3, 0.050, 160, "mat_saturn_ring_b"),
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
    # 4. Jupiter & Galilean Moons
    # -------------------------------------------------------------
    # In midground center-left: (-6.5, -0.8, 26.0), radius 3.5
    jupiter_center = (-6.5, -0.8, 25.0)
    jupiter_radius = 3.4
    prims.append(f"""sphere {{
  center   = {jupiter_center[0]:.3f} {jupiter_center[1]:.3f} {jupiter_center[2]:.3f}
  radius   = {jupiter_radius:.3f}
  material = mat_jupiter
}}""")

    # Great Red Spot in Southern Tropical Zone
    grs_pos = (jupiter_center[0] + 0.9, jupiter_center[1] - 0.75, jupiter_center[2] - 3.2)
    prims.append(f"""sphere {{
  center   = {grs_pos[0]:.3f} {grs_pos[1]:.3f} {grs_pos[2]:.3f}
  radius   = 0.45
  material = mat_jupiter_grs
}}""")

    # Moons: Io (sulfur yellow) & Europa (ice white)
    prims.append(f"""sphere {{
  center   = {jupiter_center[0] - 4.2:.3f} {jupiter_center[1] + 0.5:.3f} {jupiter_center[2] - 2.8:.3f}
  radius   = 0.25
  material = mat_io
}}""")

    prims.append(f"""sphere {{
  center   = {jupiter_center[0] + 5.0:.3f} {jupiter_center[1] - 0.4:.3f} {jupiter_center[2] + 1.5:.3f}
  radius   = 0.20
  material = mat_europa
}}""")

    # -------------------------------------------------------------
    # 5. Mars (The Red Planet)
    # -------------------------------------------------------------
    mars_center = (1.5, 4.2, 28.0)
    mars_radius = 1.1
    prims.append(f"""sphere {{
  center   = {mars_center[0]:.3f} {mars_center[1]:.3f} {mars_center[2]:.3f}
  radius   = {mars_radius:.3f}
  material = mat_mars
}}""")

    # Polar ice cap
    prims.append(f"""sphere {{
  center   = {mars_center[0]:.3f} {mars_center[1] + 0.96:.3f} {mars_center[2] - 0.42:.3f}
  radius   = 0.24
  material = mat_mars_polar
}}""")

    # -------------------------------------------------------------
    # 6. The Milky Way: 500 Pinpoint Diamond Stars
    # -------------------------------------------------------------
    star_dist = 115.0
    for _ in range(500):
        t_arc = random.uniform(-1.0, 1.0)
        spread = random.gauss(0, 0.26)

        # Diagonal galactic river
        gx = -0.70 * t_arc + spread * 0.65 + random.gauss(0, 0.12)
        gy = 0.70 * t_arc + spread * 0.68 + random.gauss(0, 0.12)
        gz = 0.48 + random.uniform(0.1, 0.7)

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
