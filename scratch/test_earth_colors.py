#!/usr/bin/env python3
import subprocess

test_script = """camera {
  eye    = 0.0 0.0 -10.0
  target = 0.0 0.0 0.0
  up     = 0.0 1.0 0.0
  vfov   = 38.0
}

sky {
  sun_dir            = -0.4907 0.0245 0.8709
  sun_color          = 1.00 0.80 0.30
  sun_radius         = 0.0
  horizon_color      = 0.0 0.0 0.0
  zenith_color       = 0.0 0.0 0.0
  gradient_gamma     = 1.0
  sun_glow_exponent  = 200.0
  sun_glow_strength  = 1.10
  cloud_coverage     = 2.0
}

material mat_sun_core {
  pbr = 1
  albedo = 0.1 0.1 0.1
  emissive = 3.5 2.4 0.35
  roughness = 0.5
}

# Earth: Authentic deep ocean-blue planet, completely diffuse (NO billiard ball reflection)
material mat_earth_body {
  pbr = 1
  albedo = 0.18 0.48 0.82
  roughness = 0.70
  metallic = 0.0
}

material mat_moon {
  pbr = 1
  albedo = 0.42 0.40 0.38
  roughness = 0.88
  metallic = 0.02
}

material mat_distant_planet {
  pbr = 1
  albedo = 0.52 0.54 0.58
  roughness = 0.78
  metallic = 0.0
}

# Sun
sphere {
  center   = -7.000 0.350 11.000
  radius   = 1.150
  material = mat_sun_core
}

# Transiting Moon
sphere {
  center   = -4.350 0.120 3.800
  radius   = 0.400
  material = mat_moon
}

# Distant Moon
sphere {
  center   = -1.800 2.600 11.000
  radius   = 0.780
  material = mat_distant_planet
}

# Earth: Clean, majestic, non-reflective planetary sphere
sphere {
  center   = 3.500 -0.220 0.000
  radius   = 2.800
  material = mat_earth_body
}
"""

with open('scratch/test_earth_colors.scene', 'w') as f:
    f.write(test_script)

