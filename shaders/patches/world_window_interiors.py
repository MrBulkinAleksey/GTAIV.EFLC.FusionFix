#!/usr/bin/env python3
# Writes world_window_interiors.patch: rooms behind the lit windows at night (interior mapping). The glow of the night
# windows is drawn by the forward "draw" technique of gta_emissivenight, gta_glass_emissivenight and
# gta_normal_spec_reflect_emissivenight (PS1, the same code in all three; the deferred techniques only give them an
# albedo). Their vertex shaders already pass the world normal (texcoord1) and the world position (texcoord6), which the
# pixel shaders left out; with them each lit pixel of a wall looks into a box room behind it:
#  - the rooms are a grid on the wall: along it (the horizontal tangent, normal x up) every c162.x metres, up every
#    c162.y metres, c162.z metres deep;
#  - the view ray enters the room at the pixel and leaves through the nearest of its back wall, side walls, floor and
#    ceiling; each has its own brightness (lighter ceiling, darker floor), all lit by a lamp under the ceiling in the
#    middle of the room, falling off with the distance;
#  - each room has its own brightness and a warmer or cooler light (a hash of its cell), and some have the blind
#    pulled down part of the way (c162.w of them), lit flat from behind;
#  - the glow keeps its own colour and pattern and only takes the room's shading, so dark windows stay dark;
#  - left alone: roofs and floors (the normal more than c15.y up or down), saturated glows (neon signs, as the
#    saturation passes c17.z) and far walls (faded over c163.xy by the distance, where the rooms would shimmer).
# Registers set by source/windowinteriors.ixx before the transparent pass: c161 = camera position, strength (0 = off);
# c162 = 1 / room width, 1 / room height, 1 / room depth, part of the rooms with a blind; c163 = fade with the distance
# as a * distance + b, debug (1: rooms in grey, walls left alone in magenta), gain. Defs c12-c21 (free in PS1).
#
# Usage: world_window_interiors.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8'
TAG = 'FusionFix world_window_interiors.patch'
SHADERS = [
    'gta_emissivenight/gta_emissivenightPS1.asm',
    'gta_glass_emissivenight/gta_glass_emissivenightPS1.asm',
    'gta_normal_spec_reflect_emissivenight/gta_normal_spec_reflect_emissivenightPS1.asm',
]

DEF_AFTER = '    def c11, 0.99, 0, 0, 0\n'
DEF = f'''\
    def c12, 0.103100002, 0.103000000, 0.0973000005, 0.109899998 // {TAG}: the room hash's scales
    def c13, 33.3300018, 1, 0, 0.5 // {TAG}: the hash's offset, one, zero, half
    def c14, 0.5, 0.850000024, 0.5, 6 // {TAG}: the lamp in the room (along, up, deep), its fall-off
    def c15, 9.99999975e-006, 0.400000006, 0, 4 // {TAG}: a tiny step, the most the normal points up or down, -, the saturation gate's slope
    def c16, 0.649999976, 0.400000006, 0.949999988, 0.800000012 // {TAG}: side walls, floor, ceiling, back wall
    def c17, 0.300000012, 0, 0.699999988, 0.699999988 // {TAG}: ambient, -, the saturation that keeps the glow, the longest blind
    def c18, 0.699999988, 0.600000024, 0, 0 // {TAG}: the dimmest room, the rooms' brightness spread
    def c19, 0.400000006, 0, 0.400000006, 0 // {TAG}: the debug magenta
    def c20, 1.08000004, 0.970000029, 0.850000024, 0 // {TAG}: a warm room
    def c21, 0.899999976, 0.980000019, 1.10000002, 0 // {TAG}: a cool room
'''

DCL_AFTER = '    dcl_color v1\n'
DCL = f'''\
    dcl_texcoord1 v2.xyz // {TAG}: world normal
    dcl_texcoord6 v3.xyz // {TAG}: world position
'''

CODE_AFTER = '    mul r1.xyz, r0, r1.x\n'
CODE = f'''\
    // --- {TAG}: the room behind a lit window (r1.xyz, the glow)
    nrm r2.xyz, v2
    add r9.x, -r2_abs.z, c15.y
    mov r9.y, c161.w
    min r9.x, r9.x, r9.y
    if_gt r9.x, c13.z
      mov r3.x, -r2.y
      mov r3.y, r2.x
      mov r3.z, c13.z
      nrm r4.xyz, r3 // along the wall
      add r5.xyz, v3, -c161
      dp3 r5.w, r5, r5
      rsq r6.w, r5.w
      mul r5.xyz, r5, r6.w // the view ray
      rcp r6.w, r6.w // the distance
      dp3 r6.x, v3, r4
      mov r6.y, v3.z
      mul r6.xy, r6, c162
      frc r7.xy, r6
      add r6.xy, r6, -r7 // the room's cell
      mov r6.z, r7.y // the height in the window, for the blind
      dp3 r8.x, r5, r4
      mov r8.y, r5.z
      dp3 r8.z, r5, -r2
      mul r8.xyz, r8, c162
      add r8.xyz, r8, c15.x // the ray in rooms, into the wall along z
      cmp r3.xyz, r8, c13.y, c13.z
      mov r7.z, c13.z
      add r3.xyz, r3, -r7
      rcp r9.x, r8.x
      rcp r9.y, r8.y
      rcp r9.z, r8.z
      mul r3.xyz, r3, r9 // how far to each wall
      min r3.w, r3.x, r3.y
      min r3.w, r3.w, r3.z
      max r3.w, r3.w, c13.z
      mad r7.xyz, r8, r3.w, r7 // where the ray meets the room
      mov r9.w, c16.w
      add r9.x, r7.y, -c13.w
      cmp r9.x, r9.x, c16.z, c16.y
      add r9.y, r3.w, -r3.y
      cmp r9.w, r9.y, r9.x, r9.w
      add r9.y, r3.w, -r3.x
      cmp r9.w, r9.y, c16.x, r9.w // the wall's brightness
      add r3.xyz, r7, -c14
      dp3 r3.x, r3, r3
      mul r3.x, r3.x, c14.w
      add r3.x, r3.x, c13.y
      rcp r3.x, r3.x
      add r3.x, r3.x, c17.x
      mul r9.w, r9.w, r3.x // lit by the lamp
      mul r3, r6.xyxy, c12
      frc r3, r3
      add r4, r3.wzxy, c13.x
      dp4 r4.x, r3, r4
      add r3, r3, r4.x
      add r4, r3.xxyz, r3.yzzw
      mul r4, r4, r3.zywx
      frc r4, r4 // four numbers of the room
      mad r3.w, r4.w, c17.w, r6.z
      add r3.w, r3.w, -c13.y
      add r8.w, c162.w, -r4.z
      min r3.w, r3.w, r8.w
      cmp r9.w, r3.w, c16.z, r9.w // the blind
      mad r3.w, r4.x, c18.y, c18.x
      mul r9.w, r9.w, r3.w
      mul r9.w, r9.w, c163.w
      mov r5.xyz, c21
      lrp r3.xyz, r4.y, c20, r5
      mul r3.xyz, r3, r9.w // the room's shading
      mad_sat r8.x, r6.w, c163.x, c163.y
      mul r8.x, r8.x, c161.w
      max r8.y, r1.x, r1.y
      max r8.y, r8.y, r1.z
      min r8.z, r1.x, r1.y
      min r8.z, r8.z, r1.z
      add r8.z, r8.y, -r8.z
      max r8.y, r8.y, c15.x
      rcp r8.y, r8.y
      mul r8.z, r8.z, r8.y
      add r8.z, -r8.z, c17.z
      mul_sat r8.z, r8.z, c15.w
      mul r8.x, r8.x, r8.z // how much the glow takes it
      add r3.xyz, r3, -c13.y
      mad r3.xyz, r8.x, r3, c13.y
      mov r8.w, c163.z
      if_gt r8.w, c13.z
        mul r3.xyz, r3, c13.w
        mov r5.xyz, c19
        lrp r1.xyz, r8.x, r3, r5
      else
        mul r1.xyz, r1, r3
      endif
    endif
'''


def main():
    for rel in SHADERS:
        path = ROOT / rel
        text = path.read_text()
        if TAG in text:
            continue
        for anchor, insert in ((DEF_AFTER, DEF), (DCL_AFTER, DCL), (CODE_AFTER, CODE)):
            if text.count(anchor) != 1:
                sys.exit(f'{rel}: expected one "{anchor.strip()}"')
            text = text.replace(anchor, anchor + insert)
        path.write_text(text)


if __name__ == '__main__':
    main()
