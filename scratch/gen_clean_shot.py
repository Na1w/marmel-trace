#!/usr/bin/env python3
"""
Recreate the exact composition and visual aesthetic of 'space_ref.png':
1. Blazing golden-yellow Sun on the right with textured solar granulation & warm corona.
2. Transiting planet/moon silhouetted right in front of the Sun.
3. Upper crescent planet above the Sun.
4. Hero planet on the left:
   - Brilliant white/silver solar rim on the side facing the Sun.
   - Detailed grey/slate surface terrain.
   - Razor-thin electric cyan-blue glowing atmospheric rim along the outer limb.
5. Inky black cosmic background with crisp diamond starfield.
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
    random.seed(1337)
    lines = []

    # Camera looking down the Z axis
    cam_eye = (0.0, 0.0, -10.0)
    cam_target = (0.0, 0.0, 0.0)

    lines.append("""camera {
  eye    = 0.0 0.0 -10.0
  target = 0.0 0.0 0.0
  up     = 0.0 1.0 0.0
  vfov   = 40.0
}
""")

    # Sun position in world space:
    sun_center = (3.8, -0.4, 0.0)
    sun_dir = norm(sub(sun_center, cam_eye))

    # Sky parameters: pure black space with warm golden-amber corona around the Sun
    lines.append(f"""sky {{
  sun_dir            = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}
  sun_color          = 1.00 0.65 0.06
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 28.0
  sun_glow_strength  = 3.8
  cloud_coverage     = 2.0
}}
""")

    # Materials
    materials = [
        # --- Sun Materials (Rich Golden/Orange Plasma, NO washed-out blue!) ---
        ("mat_sun_base", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 5.0 3.8 0.25\n  roughness = 0.5"),
        ("mat_sun_bright", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 6.2 5.0 0.45\n  roughness = 0.5"),
        ("mat_sun_gold", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 4.5 2.8 0.10\n  roughness = 0.5"),
        ("mat_sun_orange", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 3.6 1.6 0.02\n  roughness = 0.5"),
        ("mat_sun_flare", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 5.8 4.2 0.30\n  roughness = 0.5"),

        # --- Hero Planet Materials ---
        # Planet body: detailed slate/grey/blue-grey terrain
        ("mat_hero_body", "pbr = 1\n  albedo = 0.50 0.54 0.62\n  texture = checker\n  texture_scale = 0.18\n  texture_color_a = 0.55 0.58 0.65\n  texture_color_b = 0.38 0.42 0.48\n  roughness = 0.72\n  metallic = 0.0"),
        # Planet crater terrain
        ("mat_hero_crater", "pbr = 1\n  albedo = 0.32 0.34 0.38\n  roughness = 0.88\n  metallic = 0.0"),
        # Electric blue atmospheric rim (glowing cyan limb arc)
        ("mat_atmo_rim_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.35 1.60 4.20\n  roughness = 0.3"),
        ("mat_atmo_rim_cyan", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 0.60 2.20 4.80\n  roughness = 0.3"),

        # --- Transiting Moon (In front of Sun) ---
        ("mat_transiting_moon", "pbr = 1\n  albedo = 0.38 0.36 0.34\n  roughness = 0.85\n  metallic = 0.0"),

        # --- Upper Crescent Planet ---
        ("mat_upper_planet", "pbr = 1\n  albedo = 0.42 0.44 0.48\n  roughness = 0.80\n  metallic = 0.0"),

        # --- Milky Way Stars ---
        ("mat_star_white", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 14.0 14.0 14.0\n  roughness = 0.5"),
        ("mat_star_blue", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 6.0 10.0 22.0\n  roughness = 0.5"),
        ("mat_star_gold", "pbr = 1\n  albedo = 0.1 0.1 0.1\n  emissive = 16.0 13.0 4.0\n  roughness = 0.5"),
    ]

    for name, content in materials:
        lines.append(f"material {name} {{\n  {content}\n}}\n")

    prims = []

    # =============================================================
    # 1. The Sun (Hero Solar Body on the Right)
    # =============================================================
    sun_radius = 2.45

    # Base Sun Sphere
    prims.append(f"""sphere {{
  center   = {sun_center[0]:.3f} {sun_center[1]:.3f} {sun_center[2]:.3f}
  radius   = {sun_radius:.3f}
  material = mat_sun_base
}}""")

    # Solar Granulation & Plasma Cells (Clusters across the visible face)
    # Visible hemisphere faces camera (towards -Z):
    for _ in range(95):
        # Generate random directions on visible hemisphere (z < 0.2)
        theta = random.uniform(0, 2*math.pi)
        phi = random.uniform(0.1, math.pi * 0.48)
        px = math.sin(phi) * math.cos(theta)
        py = math.sin(phi) * math.sin(theta)
        pz = -math.cos(phi)
        pdir = (px, py, pz)

        # Offset slightly from base surface to create granulated organic texture
        r_node = random.uniform(0.25, 0.55)
        pos = add(sun_center, scale(pdir, sun_radius - 0.02 + random.uniform(0.0, 0.06)))

        roll = random.random()
        if roll < 0.35:
            mat = "mat_sun_bright"
        elif roll < 0.65:
            mat = "mat_sun_gold"
        elif roll < 0.85:
            mat = "mat_sun_orange"
        else:
            mat = "mat_sun_flare"

        prims.append(f"""sphere {{
  center   = {pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f}
  radius   = {r_node:.3f}
  material = {mat}
}}""")

    # =============================================================
    # 2. Transiting Moon (Silhouetted in front of the Sun)
    # =============================================================
    # In space_ref.png, it sits right on the left rim of the Sun
    transit_pos = (1.75, -0.45, -3.2)
    transit_radius = 0.58
    prims.append(f"""sphere {{
  center   = {transit_pos[0]:.3f} {transit_pos[1]:.3f} {transit_pos[2]:.3f}
  radius   = {transit_radius:.3f}
  material = mat_transiting_moon
}}""")

    # =============================================================
    # 3. Upper Crescent Planet (Above the Sun)
    # =============================================================
    upper_pos = (2.2, 2.1, 4.5)
    upper_radius = 0.82
    prims.append(f"""sphere {{
  center   = {upper_pos[0]:.3f} {upper_pos[1]:.3f} {upper_pos[2]:.3f}
  radius   = {upper_radius:.3f}
  material = mat_upper_planet
}}""")

    # =============================================================
    # 4. Hero Planet (Majestic World on the Left)
    # =============================================================
    hero_center = (-3.6, -0.2, 0.0)
    hero_radius = 2.75

    # Base planet body
    prims.append(f"""sphere {{
  center   = {hero_center[0]:.3f} {hero_center[1]:.3f} {hero_center[2]:.3f}
  radius   = {hero_radius:.3f}
  material = mat_hero_body
}}""")

    # Detailed crater patches across the terrain facing camera
    craters = [
        (-0.25, 0.45, -0.85, 0.65),
        (0.15, 0.30, -0.94, 0.55),
        (-0.35, -0.15, -0.92, 0.70),
        (0.10, -0.40, -0.90, 0.60),
        (-0.10, 0.10, -0.98, 0.50),
        (0.35, 0.15, -0.92, 0.45),
        (-0.45, 0.20, -0.87, 0.55),
    ]
    for cx, cy, cz, cr in craters:
        cdir = norm((cx, cy, cz))
        cp = add(hero_center, scale(cdir, hero_radius - 0.02))
        prims.append(f"""sphere {{
  center   = {cp[0]:.3f} {cp[1]:.3f} {cp[2]:.3f}
  radius   = {cr:.3f}
  material = mat_hero_crater
}}""")

    # Electric Blue Atmospheric Rim along the outer (left) limb:
    # Modeled as a thin shell slightly offset to the left and rear
    # Center slightly offset by (-0.035, 0.0, 0.05), radius slightly larger (hero_radius + 0.015)
    # This creates the exact razor-thin neon cyan-blue crescent along the outer limb!
    atmo_center = (hero_center[0] - 0.038, hero_center[1], hero_center[2] + 0.06)
    atmo_radius = hero_radius + 0.025
    prims.append(f"""sphere {{
  center   = {atmo_center[0]:.3f} {atmo_center[1]:.3f} {atmo_center[2]:.3f}
  radius   = {atmo_radius:.3f}
  material = mat_atmo_rim_cyan
}}""")

    atmo_center2 = (hero_center[0] - 0.055, hero_center[1], hero_center[2] + 0.10)
    atmo_radius2 = hero_radius + 0.040
    prims.append(f"""sphere {{
  center   = {atmo_center2[0]:.3f} {atmo_center2[1]:.3f} {atmo_center2[2]:.3f}
  radius   = {atmo_radius2:.3f}
  material = mat_atmo_rim_blue
}}""")

    # =============================================================
    # 5. Cosmic Starfield (Sharp Pinpoint Diamonds in Void)
    # =============================================================
    star_dist = 60.0
    for _ in range(450):
        # Sample across background (z > 5)
        sx = random.uniform(-25.0, 25.0)
        sy = random.uniform(-16.0, 16.0)
        sz = random.uniform(20.0, 55.0)
        sdir = norm((sx, sy, sz))
        spos = scale(sdir, star_dist + random.uniform(-5.0, 15.0))

        roll = random.random()
        if roll < 0.60:
            mat = "mat_star_white"
            sr = random.uniform(0.06, 0.12)
        elif roll < 0.85:
            mat = "mat_star_blue"
            sr = random.uniform(0.08, 0.14)
        else:
            mat = "mat_star_gold"
            sr = random.uniform(0.08, 0.15)

        prims.append(f"""sphere {{
  center   = {spos[0]:.2f} {spos[1]:.2f} {spos[2]:.2f}
  radius   = {sr:.2f}
  material = {mat}
}}""")

    output = "\n".join(lines) + "\n" + "\n\n".join(prims) + "\n"
    with open("scenes/space_solarsystem.scene", "w") as f:
        f.write(output)
    print(f"Generated scenes/space_solarsystem.scene with {len(prims)} primitives.")

if __name__ == "__main__":
    generate()
