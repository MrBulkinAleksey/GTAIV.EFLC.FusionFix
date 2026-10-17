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
#  - bodies (cars as their footprint, people as a small square) push the blades away from them and lay them down under a
#    car; the wind dies where they press: up to two per draw, each falling off to nothing `reach` metres past its box.
#    The push's direction is the way out of the box rounded by c246.w, shortened to nothing at the box's middle rather
#    than turned to unit length, so the blades under a body don't flip side as its centre passes over them; there they
#    are pressed down instead, to half their height under the full press.
#  - trampled ground: a grid of 0.5 m cells around the player (vs s1, 128 x 128, wrapping every 64 m of the world) holds
#    where people walked and wheels rolled: the way the blades lie (r, g), how far down they still are (b) and the
#    ground's height there, wrapping every 64 m (a); it counts like a body under the vertex where its height is within
#    about 2 m of the prop's origin.
# Registers: c240 (per frame) = wind direction over the gust's wavelength (cycles a metre), the gust's phase, the
# flutter's phase (cycles); c241 (per model) = 1 / height, the bend and the flutter at the tip in metres, the push at the
# tip in metres; c242.xy (per frame) = the wind's unit direction; c248/c249 and c250/c251 (per draw) = two bodies:
# centre.xy and unit forward.xy on the ground, half length, half width, 1 / reach, strength (0 = none); c243 (per frame)
# = 1 / the grid's side in metres, the trample's strength (0 = off); defs c245-c247.
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
    def c245, 0.015625, 64, 2.5, 0 // {TAG}: 1 / and the trample grid's height wrap (m), the height it takes (m)
    def c246, 0.550000012, 0.600000024, 1.29999995, 0.300000012 // {TAG}: the gust's swing and floor, the flutter's cycles a metre, a body's rounding (m)
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
      // bodies: r9.xy = the push's direction times its strength, r9.w = the strongest
      mov r9, c4.z
      add r10.xy, r3, -c248
      mul r11.x, r10.x, c248.z
      mad r11.x, r10.y, c248.w, r11.x
      mul r11.y, r10.x, c248.w
      mad r11.y, r10.y, -c248.z, r11.y
      abs r10.xy, r11
      add r10.xy, r10, -c249
      max r10.xy, r10, c4.z
      mul r10.z, r10.x, r10.x
      mad r10.z, r10.y, r10.y, r10.z
      add r10.z, r10.z, c4.x
      rsq r10.z, r10.z
      rcp r10.z, r10.z
      mul r10.w, r10.z, c249.z
      add_sat r10.w, -r10.w, c4.y
      mul r10.w, r10.w, r10.w
      mul r10.w, r10.w, c249.w
      mov r12.xy, c249
      add r12.xy, r12, c246.w
      rcp r12.x, r12.x
      rcp r12.y, r12.y
      mul r12.xy, r11, r12
      mul r12.z, r12.x, r12.x
      mad r12.z, r12.y, r12.y, r12.z
      add r12.z, r12.z, c4.x
      rsq r12.z, r12.z
      min r12.z, r12.z, c4.y
      mul r12.xy, r12, r12.z
      mul r13.xy, r12.x, c248.zwzw
      mad r13.x, r12.y, c248.w, r13.x
      mad r13.y, -r12.y, c248.z, r13.y
      mad r9.xy, r13, r10.w, r9
      max r9.w, r9.w, r10.w
      add r10.xy, r3, -c250
      mul r11.x, r10.x, c250.z
      mad r11.x, r10.y, c250.w, r11.x
      mul r11.y, r10.x, c250.w
      mad r11.y, r10.y, -c250.z, r11.y
      abs r10.xy, r11
      add r10.xy, r10, -c251
      max r10.xy, r10, c4.z
      mul r10.z, r10.x, r10.x
      mad r10.z, r10.y, r10.y, r10.z
      add r10.z, r10.z, c4.x
      rsq r10.z, r10.z
      rcp r10.z, r10.z
      mul r10.w, r10.z, c251.z
      add_sat r10.w, -r10.w, c4.y
      mul r10.w, r10.w, r10.w
      mul r10.w, r10.w, c251.w
      mov r12.xy, c251
      add r12.xy, r12, c246.w
      rcp r12.x, r12.x
      rcp r12.y, r12.y
      mul r12.xy, r11, r12
      mul r12.z, r12.x, r12.x
      mad r12.z, r12.y, r12.y, r12.z
      add r12.z, r12.z, c4.x
      rsq r12.z, r12.z
      min r12.z, r12.z, c4.y
      mul r12.xy, r12, r12.z
      mul r13.xy, r12.x, c250.zwzw
      mad r13.x, r12.y, c250.w, r13.x
      mad r13.y, -r12.y, c250.z, r13.y
      mad r9.xy, r13, r10.w, r9
      max r9.w, r9.w, r10.w
      // trampled: the grid under the vertex, where its height is near the prop's origin
      mul r14.xy, r3, c243.x
      mov r14.zw, c4.z
      texldl r14, r14, s1
      mov r15.x, c3.z
      mad r15.x, r15.x, c245.x, -r14.w
      add r15.x, r15.x, c247.y
      frc r15.x, r15.x
      add r15.x, r15.x, -c247.y
      abs r15.x, r15.x
      mul r15.x, r15.x, -c245.y
      add_sat r15.x, r15.x, c245.z
      mul r15.x, r15.x, r14.z
      mul r15.x, r15.x, c243.y
      add r14.xy, r14, r14
      add r14.xy, r14, -c4.y
      mad r9.xy, r14, r15.x, r9
      max r9.w, r9.w, r15.x
      mul r10.z, r9.x, r9.x
      mad r10.z, r9.y, r9.y, r10.z
      add r10.z, r10.z, c4.x
      rsq r10.z, r10.z
      mul r10.z, r10.z, r9.w
      min r10.z, r10.z, c4.y
      mul r9.xy, r9, r10.z
      mad r6.xy, -r6, r9.w, r6
      mul r9.xy, r9, c241.w
      mad r6.xy, r9, r2.z, r6
      mul r6.z, r6.x, r6.x
      mad r6.z, r6.y, r6.y, r6.z
      mul r6.z, r6.z, c241.x
      mul r6.z, r6.z, -c247.y
      add r10.x, r3.z, -c3.z
      mul r10.x, r10.x, r9.w
      mad r6.z, r10.x, -c247.y, r6.z
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
    # The shader must only use r0, so r1-r13 are free.
    used = set(re.findall(r'\br(\d+)', body))
    assert used == {'0'}, (path, used)
    assert not re.search(r'\bs1\b', body), path
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
            out.append(f'    dcl_2d s1 // {TAG}: the trample grid')
            out.append(SWAY.rstrip('\n'))
    path.write_text(head + 'vs_3_0\n' + '\n'.join(out))


for name in SHADERS:
    patch(ROOT / name)
