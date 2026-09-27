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

    # Sun position in right quadrant
    sun_pos = (-6.5, 0.35, 5.8)
    sun_dir = norm(sub(sun_pos, cam_eye))

    # Balanced solar radiation
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.78 0.25
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 130.0
  sun_glow_strength  = 1.5
  cloud_coverage     = 2.0
}}
""")

    materials = [
        # Sun core
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.6 2.4 0.38\n  roughness = 0.5"),

        # Earth Layer 1: Deep ocean body with Fresnel water specular glint
        ("mat_earth_ocean", "pbr = 1\n  albedo = 0.08 0.18 0.38\n  roughness = 0.16\n  metallic = 0.03"),

        # Earth Layer 2: Soft cloud mantle
        ("mat_earth_clouds", "pbr = 0\n  albedo = 0.90 0.92 0.96\n  specular = 0.20 0.20 0.25\n  shininess = 20\n  transparency = 0.70"),

        # Earth Layer 3: Concentric Rayleigh scattering atmosphere shell (vibrant cyan-blue limb)
        ("mat_atmo_glass", "pbr = 0\n  albedo = 0.15 0.60 1.00\n  specular = 0.35 0.70 1.0\n  shininess = 96\n  transparency = 0.86\n  reflectivity = 0.14\n  ior = 1.04"),

        # Transiting Moon
        ("mat_moon", "pbr = 1\n  albedo = 0.26 0.25 0.24\n  roughness = 0.92\n  metallic = 0.0"),

        # Distant Gas Giant: Latitudinally banded atmosphere
        ("mat_gas_giant", "pbr = 1\n  albedo = 0.68 0.62 0.52\n  roughness = 0.50\n  metallic = 0.0\n  texture_scale = 0.70\n  texture_color_a = 0.84 0.78 0.66\n  texture_color_b = 0.50 0.42 0.32"),

        # Gas Giant Rings: Ethereal icy crystalline rings
        ("mat_rings_inner", "pbr = 0\n  albedo = 0.88 0.88 0.92\n  specular = 0.35 0.35 0.40\n  shininess = 28\n  transparency = 0.40\n  reflectivity = 0.08\n  ior = 1.08"),
        ("mat_rings_outer", "pbr = 0\n  albedo = 0.82 0.84 0.88\n  specular = 0.28 0.28 0.32\n  shininess = 24\n  transparency = 0.60\n  reflectivity = 0.05\n  ior = 1.05"),

        # Stars
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.95 0.95 1.05\n  roughness = 0.5"),
        ("mat_star_mid", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.6 1.6 1.8\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.1 1.6 2.8\n  roughness = 0.5"),
        ("mat_star_gold", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 2.4 1.8 0.8\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 2.6 2.6 2.8\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # 1. The Sun: Incandescent golden star
    sun_radius = 1.15
    prims.append(f"""sphere {{
  center   = {sun_pos[0]:.3f} {sun_pos[1]:.3f} {sun_pos[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_core
}}""")

    # 2. Transiting Moon: Celestial eclipse silhouette
    moon_pos = (-4.45, 0.22, 1.0)
    moon_radius = 0.38
    prims.append(f"""sphere {{
  center   = {moon_pos[0]:.3f} {moon_pos[1]:.3f} {moon_pos[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # 3. Distant Companion Gas Giant with Dual Icy Rings
    upper_pos = (-1.8, 2.6, 11.0)
    upper_radius = 0.78
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_gas_giant
}}""")

    ring_normal = norm((0.25, 1.0, 0.35))
    ring_thick = 0.005
    ring_base = sub(upper_pos, scale(ring_normal, ring_thick))
    ring_top  = add(upper_pos, scale(ring_normal, ring_thick))

    # Inner bright ring: radius 1.40
    prims.append(f"""cylinder {{
  base     = {ring_base[0]:.3f} {ring_base[1]:.3f} {ring_base[2]:.3f}
  top      = {ring_top[0]:.3f} {ring_top[1]:.3f} {ring_top[2]:.3f}
  r_bottom = 1.400
  r_top    = 1.400
  material = mat_rings_inner
}}""")

    # Outer delicate ring: radius 1.70
    prims.append(f"""cylinder {{
  base     = {ring_base[0]:.3f} {ring_base[1]:.3f} {ring_base[2]:.3f}
  top      = {ring_top[0]:.3f} {ring_top[1]:.3f} {ring_top[2]:.3f}
  r_bottom = 1.700
  r_top    = 1.700
  material = mat_rings_outer
}}""")

    # 4. Hero Planet (Earth): Multi-layered physical sphere
    hero_center = (3.5, -0.22, 0.0)
    hero_radius = 2.80

    # Layer 1: Ocean body
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_earth_ocean
}}""")

    # Layer 2: Suspended weather envelope
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius + 0.016:.3f}
  material = mat_earth_clouds
}}""")

    # Layer 3: Concentric Rayleigh atmosphere
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius + 0.036:.3f}
  material = mat_atmo_glass
}}""")

    # 5. Milky Way Galactic Dust Stream & Stellar Field (780 stars)
    star_dist = 175.0
    for _ in range(580):
        t_arc = random.uniform(-1.25, 1.25)
        spread = random.gauss(0, 0.18)

        sx = (-0.72 * t_arc + spread * 0.9) * 55.0 + random.gauss(0, 2.2)
        sy = (0.68 * t_arc + spread * 0.9) * 38.0 + random.gauss(0, 2.0)
        sz = 70.0 + random.uniform(5.0, 95.0)

        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-20.0, 35.0))

        roll = random.random()
        if roll < 0.65:
            mat = "mat_star_faint"
            sr = random.uniform(0.014, 0.024)
        elif roll < 0.85:
            mat = "mat_star_mid"
            sr = random.uniform(0.020, 0.032)
        elif roll < 0.94:
            mat = "mat_star_blue"
            sr = random.uniform(0.022, 0.035)
        else:
            mat = "mat_star_gold"
            sr = random.uniform(0.024, 0.038)

        prims.append(f"""sphere {{
  center   = {spos[0]:.2f} {spos[1]:.2f} {spos[2]:.2f}
  radius   = {sr:.3f}
  material = {mat}
}}""")

    for _ in range(200):
        t_u = random.uniform(-1.0, 1.0)
        t_v = random.uniform(-1.0, 1.0)
        sx = t_u * 65.0
        sy = t_v * 45.0
        sz = 75.0 + random.uniform(10.0, 110.0)

        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-10.0, 30.0))

        roll = random.random()
        if roll < 0.80:
            mat = "mat_star_faint"
            sr = random.uniform(0.014, 0.022)
        elif roll < 0.95:
            mat = "mat_star_mid"
            sr = random.uniform(0.020, 0.030)
        else:
            mat = "mat_star_bright"
            sr = random.uniform(0.028, 0.042)

        prims.append(f"""sphere {{
  center   = {spos[0]:.2f} {spos[1]:.2f} {spos[2]:.2f}
  radius   = {sr:.3f}
  material = {mat}
}}""")

    output = "\n".join(lines) + "\n" + "\n\n".join(prims) + "\n"
    with open("scenes/space_solarsystem.scene", "w") as f:
        f.write(output)
    print(f"Successfully generated scenes/space_solarsystem.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
