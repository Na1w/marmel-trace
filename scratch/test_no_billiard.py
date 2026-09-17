#!/usr/bin/env python3
import subprocess

# Let's test Earth WITHOUT the glass sphere first
with open('scenes/space_solarsystem.scene', 'r') as f:
    text = f.read()

import re
# Remove the mat_atmo_glass sphere
text_no_glass = re.sub(r'sphere\s*\{[^}]*mat_atmo_glass[^}]*\}', '', text)

with open('scratch/test_no_glass.scene', 'w') as f:
    f.write(text_no_glass)

