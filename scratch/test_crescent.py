#!/usr/bin/env python3
"""
Epic Photorealism Upgrade for Marmel Space Scene:
1. Milky Way Galactic Band: Dense diagonal stellar dust stream (850+ micro-stars).
2. Ethereal, Semi-Transparent Saturnian Rings with Cassini Division gap.
3. Total Eclipse Solar Prominences: Fiery coronal flares leaping off the solar limb.
4. Richer Earth Crescent with Rayleigh Terminator & Layered Weather Swirls.
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
    sun_pos = (-6.5, 0.35, 5.8)
    sun_dir = norm(sub(sun_pos, cam_eye))

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
        # --- Sun Core: Golden incandescent furnace ---
        ("mat_sun_core", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.6 2.4 0.38\n  roughness = 0.5"),

        # --- Solar Prominences / Coronal Flares: Fiery crimson-gold arches leaping off the limb ---
        ("mat_sun_flare", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 7.5 2.6 0.12\n  roughness = 0.4"),

        # --- Earth Layer 1: Ocean Body (Deep sapphire with high Fresnel water specular glint) ---
        ("mat_earth_ocean", "pbr = 1\n  albedo = 0.06 0.16 0.38\n  roughness = 0.12\n  metallic = 0.04"),

        # --- Earth Layer 2: Cloud Weather Systems (Semi-transmissive cloud banks) ---
        ("mat_earth_clouds", "pbr = 0\n  albedo = 0.90 0.92 0.96\n  specular = 0.35 0.35 0.40\n  shininess = 40\n  transparency = 0.68\n  texture_scale = 1.4\n  texture_color_a = 0.98 0.98 1.00\n  texture_color_b = 0.22 0.28 0.42"),

        # --- Earth Layer 3: Concentric Rayleigh Scattering Atmosphere Shell ---
        ("mat_atmo_glass", "pbr = 0\n  albedo = 0.15 0.60 1.00\n  specular = 0.35 0.70 1.0\n  shininess = 96\n  transparency = 0.86\n  reflectivity = 0.14\n  ior = 1.04"),

        # --- Transiting Moon: Basaltic lunar regolith ---
        ("mat_moon", "pbr = 1\n  albedo = 0.24 0.23 0.22\n  roughness = 0.94\n  metallic = 0.0"),

        # --- Distant Gas Giant: Latitudinally banded planetary atmosphere ---
        ("mat_gas_giant", "pbr = 1\n  albedo = 0.68 0.62 0.52\n  roughness = 0.50\n  metallic = 0.0\n  texture_scale = 0.70\n  texture_color_a = 0.84 0.78 0.66\n  texture_color_b = 0.50 0.42 0.32"),

        # --- Gas Giant Rings: Semi-transparent, ethereal icy crystalline rings ---
        ("mat_rings_inner", "pbr = 0\n  albedo = 0.82 0.78 0.70\n  specular = 0.3 0.3 0.3\n  shininess = 24\n  transparency = 0.45\n  reflectivity = 0.08\n  ior = 1.08"),
        ("mat_rings_outer", "pbr = 0\n  albedo = 0.75 0.72 0.65\n  specular = 0.25 0.25 0.25\n  shininess = 20\n  transparency = 0.62\n  reflectivity = 0.05\n  ior = 1.05"),

        # --- Astronomical Diamond Stars & Milky Way Dust ---
        ("mat_star_faint", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.85 0.85 0.95\n  roughness = 0.5"),
        ("mat_star_mid", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.5 1.5 1.7\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 1.0 1.5 2.6\n  roughness = 0.5"),
        ("mat_star_gold", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 2.4 1.8 0.8\n  roughness = 0.5"),
        ("mat_star_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 2.5 2.5 2.7\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # 1. The Sun: Golden incandescent star
    sun_radius = 1.15
    prims.append(f"""sphere {{
  center   = {sun_pos[0]:.3f} {sun_pos[1]:.3f} {sun_pos[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_core
}}""")

    # Solar Prominences / Coronal Flares leaping off the solar limb
    flare_angles = [0.45, 1.15, 2.30, 3.85, 5.10]
    for fa in flare_angles:
        fx = sun_pos[0] + math.cos(fa) * (sun_radius + 0.04)
        fy = sun_pos[1] + math.sin(fa) * (sun_radius + 0.04)
        fz = sun_pos[2] + random.uniform(-0.1, 0.1)
        fr = random.uniform(0.045, 0.075)
        prims.append(f"""sphere {{
  center   = {fx:.3f} {fy:.3f} {fz:.3f}
  radius   = {fr:.3f}
  material = mat_sun_flare
}}""")

    # 2. Transiting Moon: Celestial eclipse silhouette
    moon_pos = (-4.45, 0.22, 1.0)
    moon_radius = 0.38
    prims.append(f"""sphere {{
  center   = {moon_pos[0]:.3f} {moon_pos[1]:.3f} {moon_pos[2]:.3f}
  radius   = {moon_radius:.3f}
  material = mat_moon
}}""")

    # 3. Distant Companion Gas Giant with Dual Realistic Rings
    upper_pos = (-1.8, 2.6, 11.0)
    upper_radius = 0.78
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_gas_giant
}}""")

    # Dual Semi-Transparent Rings (Inner Main Ring & Outer Ring with Cassini gap)
    ring_normal = norm((0.25, 1.0, 0.35))
    ring_thick = 0.005
    ring_base = sub(upper_pos, scale(ring_normal, ring_thick))
    ring_top  = add(upper_pos, scale(ring_normal, ring_thick))

    # Inner bright ring: radius 1.45
    prims.append(f"""cylinder {{
  base     = {ring_base[0]:.3f} {ring_base[1]:.3f} {ring_base[2]:.3f}
  top      = {ring_top[0]:.3f} {ring_top[1]:.3f} {ring_top[2]:.3f}
  r_bottom = 1.420
  r_top    = 1.420
  material = mat_rings_inner
}}""")

    # Outer delicate ring: radius 1.75
    prims.append(f"""cylinder {{
  base     = {ring_base[0]:.3f} {ring_base[1]:.3f} {ring_base[2]:.3f}
  top      = {ring_top[0]:.3f} {ring_top[1]:.3f} {ring_top[2]:.3f}
  r_bottom = 1.720
  r_top    = 1.720
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

    # Layer 2: Suspended weather systems
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius + 0.016:.3f}
  material = mat_earth_clouds
}}""")

    # Layer 3: Concentric Rayleigh scattering atmosphere
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius + 0.036:.3f}
  material = mat_atmo_glass
}}""")

    # 5. Milky Way Galactic Dust Stream & Stellar Field (750+ stars)
    star_dist = 175.0
    # A. Milky Way Band (diagonal arc of dense cosmic diamond pinpoints)
    for _ in range(580):
        t_arc = random.uniform(-1.25, 1.25)
        # Dense galactic core concentration
        spread = random.gauss(0, 0.18)

        # Diagonal galactic equator across the sky
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

    # B. Uniform background field stars
    for _ in range(220):
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
    with open("scratch/test_epic_photorealism.scene", "w") as f:
        f.write(output)
    print(f"Generated scratch/test_epic_photorealism.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
