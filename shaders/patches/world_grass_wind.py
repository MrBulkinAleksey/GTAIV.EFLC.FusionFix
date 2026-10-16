#!/usr/bin/env python3
# Writes world_grass_wind.patch: the wind sways the grass, weeds and small plants the game scatters on the ground
# (procedural props: proc_grass*, *_proc_plant_*, *weed*), drawn with gta_cutout / gta_default. The deferred vertex
# shaders of gta_default (VS4 deferred_draw, VS5 deferredalphaclip_draw) move each vertex before the world position
# (o4) and the projection (o0) are made from it. source/grasswind.ixx sets b12 around the draws of those models only, in
# the G-buffer pass; with b12 off the shader is the stock one.
#
# The offset is made in the world and taken back into the model's space through gWorld's rows (c0-c2, scaled
# rotations: each axis over its length squared), so it holds however the prop is turned:
#  - along the wind: a gust wave travelling downwind, 0.05..1.15 of the bend, its phase from the vertex's world position;
#  - across it: a quicker flutter, its phase from the position too, so neighbouring clumps move apart;
#  - weighted by (height / model height)^2, so the base stays on the ground, and the tip drops by d^2 / 2h so the
#    blade bends rather than stretches.
# Registers: c240 (per frame) = wind direction over the gust's wavelength (cycles a metre), the gust's phase, the
# flutter's phase (cycles); c241 (per model) = 1 / height, the bend and the flutter at the tip in metres; c242.xy (per
# frame) = the wind's unit direction; def c247.
#
# Usage: world_grass_wind.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'gta_default'
TAG = 'FusionFix world_grass_wind.patch'
SHADERS = ['gta_defaultVS4.asm', 'gta_defaultVS5.asm']

DEF = f'''\
    def c247, 0.159154937, 0.5, 6.28318548, -3.14159274 // {TAG}: 1 / 2pi, 1 / 2, 2pi, -pi
    def c246, 0.550000012, 0.600000024, 1.29999995, 0 // {TAG}: the gust's swing and floor, the flutter's cycles a metre
'''

SWAY = f'''\
    // --- {TAG}: b12 = a grass prop. r1 = the position the rest of the shader takes instead of v0.
    mov r1, v0
    if b12
      mul r3.xyz, c1, v0.y
      mad r3.xyz, v0.x, c0, r3
      mad r3.xyz, v0.z, c2, r3
      add r3.xyz, r3, c3
      mul r2.x, r3.x, c240.x
      mad r2.x, r3.y, c240.y, r2.x
      add r2.x, r2.x, c240.z
      add r2.y, r3.x, r3.y
      mul r2.y, r2.y, c246.z
      add r2.y, r2.y, c240.w
      frc r2.xy, r2
      mad r2.xy, r2, c247.z, c247.w
      sincos r4.xy, r2.x
      sincos r5.xy, r2.y
      mul_sat r2.z, v0.z, c241.x
      mul r2.z, r2.z, r2.z
      mad r4.x, r4.y, c246.x, c246.y
      mul r4.x, r4.x, c241.y
      mul r4.y, r5.y, c241.z
      mul r4.xy, r4, r2.z
      mul r6.xy, r4.x, c242
      mad r6.x, -r4.y, c242.y, r6.x
      mad r6.y, r4.y, c242.x, r6.y
      mul r6.z, r6.x, r6.x
      mad r6.z, r6.y, r6.y, r6.z
      mul r6.z, r6.z, c241.x
      mul r6.z, r6.z, -c247.y
      dp3 r7.x, c0, c0
      dp3 r7.y, c1, c1
      dp3 r7.z, c2, c2
      rcp r7.x, r7.x
      rcp r7.y, r7.y
      rcp r7.z, r7.z
      dp3 r8.x, r6, c0
      dp3 r8.y, r6, c1
      dp3 r8.z, r6, c2
      mad r1.xyz, r8, r7, v0
    endif
    // --- {TAG} end
'''


def patch(path):
    text = path.read_text()
    if TAG in text:
        return
    head, body = text.split('vs_3_0\n', 1)
    lines = body.split('\n')
    # The shader must only use r0, so r1-r8 are free.
    used = set(re.findall(r'\br(\d+)', body))
    assert used == {'0'}, (path, used)
    last_dcl = max(i for i, line in enumerate(lines) if line.strip().startswith('dcl_'))
    first_def = next(i for i, line in enumerate(lines) if line.strip().startswith('def '))
    out = []
    for i, line in enumerate(lines):
        if i > last_dcl:
            line = re.sub(r'\bv0\b', 'r1', line)
        if i == first_def:
            out.append(DEF.rstrip('\n'))
        out.append(line)
        if i == last_dcl:
            out.append(SWAY.rstrip('\n'))
    path.write_text(head + 'vs_3_0\n' + '\n'.join(out))


for name in SHADERS:
    patch(ROOT / name)
