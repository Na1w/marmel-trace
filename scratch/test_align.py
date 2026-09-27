import math
import subprocess

def test():
    # Scale: 1 unit = 100 km
    R_M = 17.374      # Moon radius = 1737.4 km
    h = 1.20          # 120 km altitude
    R_E = 63.71       # Earth radius = 6371 km
    D_EM = 3844.0     # Earth-Moon distance = 384,400 km

    # Moon center is at (0, -R_M, 0)
    # Eye is at (0, h, 0)
    eye = (0.0, h, 0.0)
    
    # Horizon angle from horizontal (looking in +Z direction):
    # Vector to horizon has pitch angle:
    # cos(pitch) = R_M / (R_M + h)
    cos_pitch = R_M / (R_M + h)
    pitch_rad = math.acos(cos_pitch)
    pitch_deg = math.degrees(pitch_rad)
    print(f"Horizon depression angle: {pitch_deg:.2f} degrees below horizontal")
    
    # We want Earth to sit nicely above the horizon, say 6 degrees above the horizon:
    earth_elev_deg = -pitch_deg + 7.5
    earth_elev_rad = math.radians(earth_elev_deg)
    
    # Earth position:
    earth_z = D_EM * math.cos(earth_elev_rad)
    earth_y = eye[1] + D_EM * math.sin(earth_elev_rad)
    earth_x = 0.5 * D_EM * math.tan(math.radians(-1.5)) # slight horizontal offset
    
    print(f"Earth center: ({earth_x:.2f}, {earth_y:.2f}, {earth_z:.2f})")
    
    # Camera target: point between lunar horizon and Earth
    # Camera vfov: 16 degrees (Apollo 70mm Hasselblad 250mm lens ~13-16 deg)
    vfov = 16.0
    target_pitch = -pitch_deg + 4.0
    target_pitch_rad = math.radians(target_pitch)
    target = (0.0, eye[1] + 10.0 * math.sin(target_pitch_rad), 10.0 * math.cos(target_pitch_rad))
    print(f"Camera eye: {eye}")
    print(f"Camera target: {target}")
    print(f"Camera vfov: {vfov}")

if __name__ == "__main__":
    test()
