import math

def create_photoreal_scene():
    # Scale: 1 unit = 100 km
    R_M = 17.374      # Moon radius = 1737.4 km
    h = 1.12          # 112 km Apollo 8 orbit altitude
    R_E = 63.71       # Earth radius = 6371 km
    D_EM = 3844.0     # Earth-Moon distance = 384,400 km

    moon_center = (0.0, -R_M, 0.0)
    eye = (0.0, h, 0.0)
    
    cos_pitch = R_M / (R_M + h)
    pitch_rad = math.acos(cos_pitch)
    pitch_deg = math.degrees(pitch_rad)
    
    earth_elev_deg = -pitch_deg + 6.0
    earth_elev_rad = math.radians(earth_elev_deg)
    
    earth_az_deg = 1.0
    earth_az_rad = math.radians(earth_az_deg)
    
    earth_z = D_EM * math.cos(earth_elev_rad) * math.cos(earth_az_rad)
    earth_x = D_EM * math.cos(earth_elev_rad) * math.sin(earth_az_rad)
    earth_y = eye[1] + D_EM * math.sin(earth_elev_rad)
    
    earth_center = (earth_x, earth_y, earth_z)
    
    vfov = 14.0 # Telephoto framing
    cam_pitch_deg = -pitch_deg + 3.2
    cam_pitch_rad = math.radians(cam_pitch_deg)
    cam_dist = 100.0
    target_x = cam_dist * math.sin(earth_az_rad * 0.5)
    target_y = eye[1] + cam_dist * math.sin(cam_pitch_rad)
    target_z = cam_dist * math.cos(cam_pitch_rad) * math.cos(earth_az_rad * 0.5)
    
    sun_dir = (-0.72, 0.38, 0.58)
    sun_len = math.sqrt(sun_dir[0]**2 + sun_dir[1]**2 + sun_dir[2]**2)
    sun_dir = (sun_dir[0]/sun_len, sun_dir[1]/sun_len, sun_dir[2]/sun_len)
    
    # Spiral Galaxy direction in upper-left sky (targeted at px 280, 130)
    galaxy_dir = (0.1303, -0.2124, 0.9684)
    # Interstellar emission nebula direction in upper-right sky (targeted at px 980, 110)
    nebula_dir = (-0.1059, -0.2060, 0.9728)

    scene = []
    scene.append("# Photorealistic Apollo 8 Earthrise with Galaxy & Nebula")
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
    scene.append("  star_intensity = 0.85")
    scene.append("  star_density = 550.0")
    scene.append("  nebula_intensity = 0.85")
    scene.append(f"  nebula_dir = {nebula_dir[0]:.4f} {nebula_dir[1]:.4f} {nebula_dir[2]:.4f}")
    scene.append("  galaxy_intensity = 0.90")
    scene.append(f"  galaxy_dir = {galaxy_dir[0]:.4f} {galaxy_dir[1]:.4f} {galaxy_dir[2]:.4f}")
    scene.append("}")
    scene.append("")
    
    # 1. Moon Material
    scene.append("material mat_moon {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 1.0 1.0 1.0")
    scene.append("  roughness = 0.94")
    scene.append("  metallic = 0.0")
    scene.append("  texture = moon")
    scene.append(f"  texture_scale = {R_M:.3f}")
    scene.append("  bump_strength = 0.45")
    scene.append("  bump_scale = 1.2")
    scene.append("}")
    scene.append("")
    
    # 2. Earth Material
    scene.append("material mat_earth {")
    scene.append("  pbr = 1")
    scene.append("  albedo = 1.0 1.0 1.0")
    scene.append("  roughness = 0.65")
    scene.append("  metallic = 0.0")
    scene.append("  texture = earth")
    scene.append(f"  texture_scale = {R_E:.3f}")
    scene.append("  bump_strength = 0.20")
    scene.append("  bump_scale = 0.6")
    scene.append("  atmosphere_glow = 0.28 0.58 0.95")
    scene.append("}")
    scene.append("")
    
    # Clean Moon Sphere
    scene.append("sphere {")
    scene.append(f"  center = {moon_center[0]:.4f} {moon_center[1]:.4f} {moon_center[2]:.4f}")
    scene.append(f"  radius = {R_M:.4f}")
    scene.append("  material = mat_moon")
    scene.append("}")
    scene.append("")

    # Clean Earth Sphere
    scene.append("sphere {")
    scene.append(f"  center = {earth_center[0]:.4f} {earth_center[1]:.4f} {earth_center[2]:.4f}")
    scene.append(f"  radius = {R_E:.4f}")
    scene.append("  material = mat_earth")
    scene.append("}")
    scene.append("")
    
    scene_text = "\n".join(scene) + "\n"
    with open("scenes/space_solarsystem.scene", "w") as f:
        f.write(scene_text)
    print("Wrote scenes/space_solarsystem.scene with galaxy and nebula.")

if __name__ == "__main__":
    create_photoreal_scene()
