import math
import random

def create_clean_earthrise():
    random.seed(19681224)
    
    # 1 unit = 100 km
    R_M = 17.374      # Moon radius = 1737.4 km
    h = 1.12          # 112 km Apollo 8 orbit altitude
    R_E = 63.71       # Earth radius = 6371 km
    D_EM = 3844.0     # Earth-Moon distance = 384,400 km

    # Moon center is at (0, -R_M, 0)
    moon_center = (0.0, -R_M, 0.0)
    eye = (0.0, h, 0.0)
    
    # Horizon dip angle from local horizontal:
    cos_pitch = R_M / (R_M + h)
    pitch_rad = math.acos(cos_pitch)
    pitch_deg = math.degrees(pitch_rad)
    
    # Earth elevation above lunar horizon: ~5.8 degrees
    earth_elev_deg = -pitch_deg + 6.2
    earth_elev_rad = math.radians(earth_elev_deg)
    
    # Earth azimuth: slightly right of center
    earth_az_deg = 1.0
    earth_az_rad = math.radians(earth_az_deg)
    
    earth_z = D_EM * math.cos(earth_elev_rad) * math.cos(earth_az_rad)
    earth_x = D_EM * math.cos(earth_elev_rad) * math.sin(earth_az_rad)
    earth_y = eye[1] + D_EM * math.sin(earth_elev_rad)
    
    earth_center = (earth_x, earth_y, earth_z)
    
    # Camera aiming (telephoto ~250mm Hasselblad)
    vfov = 15.0
    cam_pitch_deg = -pitch_deg + 3.6
    cam_pitch_rad = math.radians(cam_pitch_deg)
    cam_dist = 100.0
    target_x = cam_dist * math.sin(earth_az_rad * 0.5)
    target_y = eye[1] + cam_dist * math.sin(cam_pitch_rad)
    target_z = cam_dist * math.cos(cam_pitch_rad) * math.cos(earth_az_rad * 0.5)
    
    # Sun direction: raking low-angle sunlight across lunar terrain
    sun_dir = (-0.72, 0.38, 0.58)
    sun_len = math.sqrt(sun_dir[0]**2 + sun_dir[1]**2 + sun_dir[2]**2)
    sun_dir = (sun_dir[0]/sun_len, sun_dir[1]/sun_len, sun_dir[2]/sun_len)
    
    scene = []
    scene.append("# 100% Physical Apollo 8 Earthrise Simulation")
    scene.append("# Scale: 1 unit = 100 km")
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
    scene.append("  sun_color = 1.35 1.32 1.28")
    scene.append("  horizon_color = 0.0 0.0 0.0")
    scene.append("  zenith_color = 0.0 0.0 0.0")
    scene.append("  sun_radius = 0.267")
    scene.append("  sun_glow_exponent = 200.0")
    scene.append("  sun_glow_strength = 0.0")
    scene.append("}")
    scene.append("")
    
    # Materials
    # Moon: Single clean physical regolith material
    scene.append("material moon_regolith {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.12 0.12 0.11")
    scene.append("  roughness = 0.95")
    scene.append("  metallic = 0.0")
    scene.append("}")
    scene.append("")
    
    # Earth: Clean physical integrated planetary albedo
    # (Oceans + 60% cloud cover + continents = ~0.36 albedo, diffuse blue-white)
    scene.append("material earth_clean {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 0.24 0.38 0.62")
    scene.append("  roughness = 0.85")
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
    
    # 1. Main Lunar Sphere (clean, single sphere)
    scene.append("sphere {")
    scene.append(f"  center = {moon_center[0]:.4f} {moon_center[1]:.4f} {moon_center[2]:.4f}")
    scene.append(f"  radius = {R_M:.4f}")
    scene.append("  material = moon_regolith")
    scene.append("}")
    scene.append("")

    # 2. Earth (clean, single sphere)
    scene.append("sphere {")
    scene.append(f"  center = {earth_center[0]:.4f} {earth_center[1]:.4f} {earth_center[2]:.4f}")
    scene.append(f"  radius = {R_E:.4f}")
    scene.append("  material = earth_clean")
    scene.append("}")
    scene.append("")

    # 3. Background Starfield
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
    print("Wrote clean scenes/space_solarsystem.scene successfully.")

if __name__ == "__main__":
    create_clean_earthrise()
