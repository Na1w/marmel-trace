import math
import random

def create_earthrise_scene():
    # Apollo 8 Earthrise: Dec 24, 1968
    random.seed(19681224)
    
    # Physical Units: 1 unit = 100 km
    R_M = 17.374      # Moon radius = 1737.4 km
    h = 1.12          # 112 km Apollo 8 orbit altitude
    R_E = 63.71       # Earth radius = 6371 km
    D_EM = 3844.0     # Earth-Moon distance = 384,400 km

    # Moon center is at (0, -R_M, 0)
    moon_center = (0.0, -R_M, 0.0)
    eye = (0.0, h, 0.0)
    
    # Horizon dip angle from local horizontal:
    # cos(pitch) = R_M / (R_M + h)
    cos_pitch = R_M / (R_M + h)
    pitch_rad = math.acos(cos_pitch)
    pitch_deg = math.degrees(pitch_rad)
    
    # Earth elevation above lunar horizon: ~5.8 degrees
    earth_elev_deg = -pitch_deg + 6.4
    earth_elev_rad = math.radians(earth_elev_deg)
    
    # Earth azimuth: slightly right of center
    earth_az_deg = 1.2
    earth_az_rad = math.radians(earth_az_deg)
    
    earth_z = D_EM * math.cos(earth_elev_rad) * math.cos(earth_az_rad)
    earth_x = D_EM * math.cos(earth_elev_rad) * math.sin(earth_az_rad)
    earth_y = eye[1] + D_EM * math.sin(earth_elev_rad)
    
    earth_center = (earth_x, earth_y, earth_z)
    
    # Camera aiming (telephoto ~250mm Hasselblad)
    vfov = 14.5
    cam_pitch_deg = -pitch_deg + 3.6
    cam_pitch_rad = math.radians(cam_pitch_deg)
    cam_dist = 100.0
    target_x = cam_dist * math.sin(earth_az_rad * 0.5)
    target_y = eye[1] + cam_dist * math.sin(cam_pitch_rad)
    target_z = cam_dist * math.cos(cam_pitch_rad) * math.cos(earth_az_rad * 0.5)
    
    # Sun direction: raking low-angle sunlight across lunar terrain
    # Direction vector points TOWARDS the sun:
    sun_dir = (-0.72, 0.38, 0.58)
    sun_len = math.sqrt(sun_dir[0]**2 + sun_dir[1]**2 + sun_dir[2]**2)
    sun_dir = (sun_dir[0]/sun_len, sun_dir[1]/sun_len, sun_dir[2]/sun_len)
    
    scene = []
    scene.append("# 100% Physical Apollo 8 Earthrise Simulation")
    scene.append("# Scale: 1 unit = 100 km (Earth R=6371km, Moon R=1737.4km, Distance=384,400km)")
    scene.append("")
    scene.append("camera {")
    scene.append(f"  eye = {eye[0]:.4f} {eye[1]:.4f} {eye[2]:.4f}")
    scene.append(f"  target = {target_x:.4f} {target_y:.4f} {target_z:.4f}")
    scene.append("  up = 0 1 0")
    scene.append(f"  vfov = {vfov:.2f}")
    scene.append("  aperture = 0.0")
    scene.append("  focus_distance = 100.0")
    scene.append("}")
    scene.append("")
    scene.append("sky {")
    scene.append(f"  sun_dir = {sun_dir[0]:.4f} {sun_dir[1]:.4f} {sun_dir[2]:.4f}")
    scene.append("  sun_color = 1.38 1.35 1.32")
    scene.append("  horizon_color = 0.0 0.0 0.0")
    scene.append("  zenith_color = 0.0 0.0 0.0")
    scene.append("  sun_radius = 0.267")
    scene.append("  sun_glow_exponent = 200.0")
    scene.append("  sun_glow_strength = 0.0")
    scene.append("}")
    scene.append("")
    
    # Materials
    # Moon: True physical albedo 0.11-0.12 (asphalt dark), high roughness (dust regolith)
    scene.append("material moon_regolith {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.115 0.115 0.110")
    scene.append("  roughness = 0.95")
    scene.append("  metallic = 0.0")
    scene.append("}")
    scene.append("")
    scene.append("material moon_highlands {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.155 0.150 0.142")
    scene.append("  roughness = 0.92")
    scene.append("  metallic = 0.0")
    scene.append("}")
    scene.append("")
    scene.append("material moon_crater_basalt {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.085 0.085 0.082")
    scene.append("  roughness = 0.97")
    scene.append("  metallic = 0.0")
    scene.append("}")
    scene.append("")
    
    # Earth Materials (True Physical Geometric Albedos):
    # Ocean: albedo ~0.05
    scene.append("material earth_ocean {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.04 0.12 0.32")
    scene.append("  roughness = 0.28")
    scene.append("  metallic = 0.0")
    scene.append("}")
    scene.append("")
    # Continents (vegetation & savannah): albedo ~0.18
    scene.append("material earth_land {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.22 0.27 0.16")
    scene.append("  roughness = 0.85")
    scene.append("  metallic = 0.0")
    scene.append("}")
    scene.append("")
    # Deserts / Arid terrain (Sahara / Middle East): albedo ~0.35
    scene.append("material earth_desert {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.38 0.32 0.20")
    scene.append("  roughness = 0.88")
    scene.append("  metallic = 0.0")
    scene.append("}")
    scene.append("")
    # Polar Ice / Glaciers: albedo ~0.80
    scene.append("material earth_ice {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.78 0.82 0.86")
    scene.append("  roughness = 0.82")
    scene.append("  metallic = 0.0")
    scene.append("}")
    scene.append("")
    # Clouds (water droplets / ice crystals): albedo ~0.72 - 0.76
    scene.append("material earth_clouds {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.75 0.77 0.80")
    scene.append("  roughness = 0.90")
    scene.append("  metallic = 0.0")
    scene.append("}")
    scene.append("")
    
    # Distant Stars
    scene.append("material star_dim {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.1 0.1 0.1")
    scene.append("  roughness = 0.5")
    scene.append("  emissive = 1.4 1.4 1.5")
    scene.append("}")
    scene.append("")
    scene.append("material star_bright {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.1 0.1 0.1")
    scene.append("  roughness = 0.5")
    scene.append("  emissive = 3.5 3.5 3.8")
    scene.append("}")
    scene.append("")
    
    # 1. Main Lunar Sphere
    scene.append("sphere {")
    scene.append(f"  center = {moon_center[0]:.4f} {moon_center[1]:.4f} {moon_center[2]:.4f}")
    scene.append(f"  radius = {R_M:.4f}")
    scene.append("  material = moon_regolith")
    scene.append("}")
    scene.append("")
    
    # 2. Lunar Horizon Crater Topography:
    d_h = math.sqrt(2 * R_M * h + h * h)
    for i in range(50):
        az = math.radians(-11.5 + i * (23.0 / 49.0) + random.uniform(-0.25, 0.25))
        dist_factor = random.uniform(0.68, 0.995)
        dist_surf = d_h * dist_factor
        
        gamma = dist_surf / R_M
        px = R_M * math.sin(gamma) * math.sin(az)
        pz = R_M * math.sin(gamma) * math.cos(az)
        py = moon_center[1] + R_M * math.cos(gamma)
        
        crater_r = random.uniform(0.05, 0.28)
        rim_h = random.uniform(0.005, 0.026)
        
        nx = px - moon_center[0]
        ny = py - moon_center[1]
        nz = pz - moon_center[2]
        n_len = math.sqrt(nx*nx + ny*ny + nz*nz)
        nx /= n_len; ny /= n_len; nz /= n_len
        
        rx = px + nx * rim_h
        ry = py + ny * rim_h
        rz = pz + nz * rim_h
        
        pick = random.random()
        if pick < 0.45:
            mat = "moon_highlands"
        elif pick < 0.80:
            mat = "moon_regolith"
        else:
            mat = "moon_crater_basalt"
            
        scene.append("sphere {")
        scene.append(f"  center = {rx:.5f} {ry:.5f} {rz:.5f}")
        scene.append(f"  radius = {crater_r:.4f}")
        scene.append(f"  material = {mat}")
        scene.append("}")
        scene.append("")

    # 3. Earth:
    # Base Ocean Sphere
    scene.append("sphere {")
    scene.append(f"  center = {earth_center[0]:.4f} {earth_center[1]:.4f} {earth_center[2]:.4f}")
    scene.append(f"  radius = {R_E:.4f}")
    scene.append("  material = earth_ocean")
    scene.append("}")
    scene.append("")
    
    # Vector from Earth to camera:
    v_to_cam = (eye[0] - earth_center[0], eye[1] - earth_center[1], eye[2] - earth_center[2])
    v_cam_len = math.sqrt(v_to_cam[0]**2 + v_to_cam[1]**2 + v_to_cam[2]**2)
    ecx = v_to_cam[0] / v_cam_len
    ecy = v_to_cam[1] / v_cam_len
    ecz = v_to_cam[2] / v_cam_len
    
    # Polar Ice Cap (Antarctica on the sunlit limb)
    ice_pos_x = earth_center[0] + (ecx - 0.02) * (R_E - 1.5)
    ice_pos_y = earth_center[1] + (ecy - 0.72) * (R_E - 1.5)
    ice_pos_z = earth_center[2] + (ecz + 0.18) * (R_E - 1.5)
    scene.append("sphere {")
    scene.append(f"  center = {ice_pos_x:.4f} {ice_pos_y:.4f} {ice_pos_z:.4f}")
    scene.append("  radius = 22.0")
    scene.append("  material = earth_ice")
    scene.append("}")
    scene.append("")
    
    # Continents
    landmasses = [
        # (offset_x, offset_y, offset_z, radius, mat)
        (0.12, 0.22, -0.15, 25.0, "earth_land"),
        (0.25, 0.35, -0.18, 18.0, "earth_desert"), # arid northern Africa / Arabia
        (-0.22, 0.08, -0.10, 26.0, "earth_land"),
        (0.32, -0.18, -0.05, 21.0, "earth_land"),
        (-0.08, -0.28, 0.06, 23.0, "earth_land"),
        (0.04, 0.38, -0.22, 19.0, "earth_desert"),
    ]
    for lx, ly, lz, lr, lmat in landmasses:
        pos_x = earth_center[0] + (ecx + lx) * (R_E - lr * 0.1)
        pos_y = earth_center[1] + (ecy + ly) * (R_E - lr * 0.1)
        pos_z = earth_center[2] + (ecz + lz) * (R_E - lr * 0.1)
        scene.append("sphere {")
        scene.append(f"  center = {pos_x:.4f} {pos_y:.4f} {pos_z:.4f}")
        scene.append(f"  radius = {lr:.4f}")
        scene.append(f"  material = {lmat}")
        scene.append("}")
        scene.append("")
        
    # Swirling Cloud Deck
    cloud_systems = [
        # Cyclone in northern hemisphere
        (0.08, 0.52, -0.22, 15.0),
        (0.20, 0.44, -0.16, 17.0),
        (-0.12, 0.38, -0.08, 18.0),
        # Equatorial convergence band (ITCZ)
        (0.28, 0.08, -0.08, 21.0),
        (0.05, 0.02, -0.04, 23.0),
        (-0.22, -0.08, 0.06, 20.0),
        # Southern ocean storm spiral
        (0.12, -0.42, 0.14, 22.0),
        (-0.08, -0.55, 0.18, 18.0),
        (0.32, -0.32, 0.08, 16.0),
        (-0.30, 0.12, 0.02, 17.0),
        (0.18, -0.22, 0.04, 15.0),
        (-0.16, -0.38, 0.12, 17.0),
    ]
    for cx, cy, cz, cr in cloud_systems:
        pos_x = earth_center[0] + (ecx + cx) * (R_E + 0.35 - cr * 0.08)
        pos_y = earth_center[1] + (ecy + cy) * (R_E + 0.35 - cr * 0.08)
        pos_z = earth_center[2] + (ecz + cz) * (R_E + 0.35 - cr * 0.08)
        scene.append("sphere {")
        scene.append(f"  center = {pos_x:.4f} {pos_y:.4f} {pos_z:.4f}")
        scene.append(f"  radius = {cr:.4f}")
        scene.append("  material = earth_clouds")
        scene.append("}")
        scene.append("")

    # 4. Background Starfield
    for _ in range(300):
        star_az = earth_az_rad + math.radians(random.uniform(-14.0, 14.0))
        star_el = cam_pitch_rad + math.radians(random.uniform(1.0, 14.0))
        star_d = 5000.0
        sx = eye[0] + star_d * math.cos(star_el) * math.sin(star_az)
        sy = eye[1] + star_d * math.sin(star_el)
        sz = eye[2] + star_d * math.cos(star_el) * math.cos(star_az)
        star_r = random.uniform(0.5, 1.6)
        star_mat = "star_bright" if random.random() < 0.15 else "star_dim"
        scene.append("sphere {")
        scene.append(f"  center = {sx:.2f} {sy:.2f} {sz:.2f}")
        scene.append(f"  radius = {star_r:.2f}")
        scene.append(f"  material = {star_mat}")
        scene.append("}")
        scene.append("")
        
    scene_text = "\n".join(scene) + "\n"
    with open("scenes/space_solarsystem.scene", "w") as f:
        f.write(scene_text)
    print("Wrote scenes/space_solarsystem.scene successfully.")

if __name__ == "__main__":
    create_earthrise_scene()
