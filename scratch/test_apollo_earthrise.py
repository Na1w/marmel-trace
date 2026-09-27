import math
import subprocess

def main():
    # Units: 1 unit = 1000 km
    R_earth = 6.371
    R_moon = 1.7374
    D_em = 384.4

    # Moon at origin (0, 0, 0)
    # Orbit altitude h = 180 km = 0.180 units
    # Spacecraft position: on Z-axis between Moon and Earth? 
    # Or in orbit around Moon looking at Earth as Earth rises over the limb:
    # Let spacecraft be at (0, R_moon + 0.180, 0) looking towards Earth in +Z direction.
    # But wait, if Moon is at (0, 0, 0), the horizon in +Z is around Z ~ sqrt(2*R_moon*h + h^2)
    # Let's place Moon center at (0, -1.7374, 0).
    # Then lunar surface apex is at (0, 0, 0).
    # Spacecraft is at (0, 0.180, 0), so altitude is 180 km above the surface apex.
    # Looking towards +Z (where Earth is at (0, 3.5, 384.4))
    
    # Earth angular diameter from Moon:
    # 2 * atan(6.371 / 384.4) * 180 / pi = 1.899 deg.
    
    # If camera vfov = 24 degrees:
    # Earth will take 1.899 / 24 = ~7.9% of height (about 57 pixels at 720p).
    # If camera vfov = 14 degrees (telephoto like 250mm lens on Apollo 70mm Hasselblad):
    # Earth takes 1.899 / 14 = ~13.6% of height (about 98 pixels at 720p).
    # If camera vfov = 9 degrees (telephoto zoom):
    # Earth takes 1.899 / 9 = ~21% of height (about 150 pixels at 720p).
    
    print(f"R_earth = {R_earth}, R_moon = {R_moon}, D_em = {D_em}")
    print(f"Earth angular diameter = {2 * math.atan(R_earth / D_em) * 180 / math.pi:.3f} degrees")

if __name__ == "__main__":
    main()
