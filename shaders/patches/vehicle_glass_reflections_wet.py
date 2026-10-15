#!/usr/bin/env python3
# Writes vehicle_glass_reflections_wet.patch, on top of vehicle_glass_reflections.patch, for the car glass's forward
# pixel shaders (gta_vehicle_vehglass PS2-PS4) and their vertex shaders (VS1 drawn whole, VS2 skinned):
#  - the march's hit takes a thickness growing 2% a metre with the distance, so far windows find their hits;
#  - what the march found covers what is behind the glass as much as the game's Fresnel shows it: the glass's
#    alpha rises towards it, instead of the reflection being added over the full view through the glass;
#  - rain drops on the glass (vehicle_raindrops.py, the same as on the paint): each shows the scene behind it
#    refracted (the lit opaque scene the glass reflections copy, shifted by the drop's slope on the screen),
#    covers the view through the glass and catches the sun or the moon, in its shadow.
# The drops need the glass reflections' parameter texture and scene copy; without them only the cover is left out too.
#
# Usage: vehicle_glass_reflections_wet.py <FusionShaders checkout with the other patches applied>; the patch is the
# diff it leaves (git diff).
import re
import sys
from pathlib import Path

from vehicle_raindrops import DEFS, drops, drops_bend, drops_frame

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'gta_vehicle_vehglass'
TAG = 'FusionFix vehicle_glass_reflections_wet.patch'

PS_DEFS = DEFS + f'''\
    def c179, 0.0299999993, -0.0299999993, 0.899999976, 400 // {TAG}: refraction on the screen, a drop's cover, the glint's power
'''

PS_DCLS = f'''\
    dcl_texcoord4 v6 // {TAG}: position in the car's space, clip x
    dcl_texcoord5 v7 // {TAG}: normal in the car's space, clip y
    dcl_texcoord6 v8 // {TAG}: world up in the car's space, clip w
'''

COVER_AND_DROPS = f'''\
    mul r27.w, r17.w, r1.z // {TAG}: how much of the view through the glass the found reflection covers
    // Rain drops: r28 takes the scene they show and how much they cover, r30 the normal they bend
    mov r28, c185.w
    mov r30.xyz, r2
    mov r30.w, r17.w // the found reflection's share, kept from the drops' frame (r30.w is free after the march)
{drops_frame('v6', 'v7', 'v8')}\
    mov r24, c185.w
    mov r27.xyz, r2
    mov r8.x, c176.z
    if_lt c185.w, r8.x
{drops()}\
    endif
{drops_bend()}\
    mov r8.x, c176.z
    if_lt c185.w, r8.x
      mov r30.xyz, r27
      add r9.xyz, r27, -r2
      mov r10, c160
      texldl r10, r10, s9 // camera right axis
      mov r11, c161
      texldl r11, r11, s9 // camera up axis
      dp3 r12.x, r9, r10
      dp3 r12.y, r9, r11
      rcp r12.w, v8.w
      mul r13.x, v6.w, r12.w
      mul r13.y, v7.w, r12.w
      mad r13.xy, r13, c158, c158.z // where the glass is on the screen
      mad r13.xy, r12, c179, r13 // and where its drop looks through
      mov r13.zw, c185.w
      texldl r14, r13, s13
      mov r28.xyz, r14
      mul r9.z, r24.y, c180.y
      max r9.w, r24.x, r9.z // drops, and half as much in their trails
      mul r28.w, r9.w, c179.z
      mov r10, c156
      texldl r10, r10, s9
      add r10.w, r10.w, -c157.x
      mul r10.w, r10.w, r10.w
      add r10.w, r10.w, -c157.y
      cmp r28.w, r10.w, c185.w, r28.w // none without the glass reflections' scene copy
    endif
    mov r17.w, r30.w
'''

SHADOW = f'''\
    mov r27.x, r1.w // {TAG}: the sun's shadow, for the drops' glint
'''

COMPOSE = f'''\
    // --- {TAG}: the found reflection, then the drops, cover what is behind the glass; the drops add the scene they
    // --- refract and the sun's glint. r0.x is the glass's alpha, r0.yzw its colour, which alpha blending weighs by it.
    mul r31.xyz, r0.yzww, r0.x
    add r29.y, -r0.x, c157.y
    mad r29.z, r29.y, r27.w, r0.x
    add r29.y, -r29.z, c157.y
    mul r29.w, r29.y, r28.w
    add r29.z, r29.z, r29.w
    mad r31.xyz, r28, r29.w, r31
    if_lt c185.w, r28.w
      add r8.xyz, -v4, c15
      nrm r9.xyz, r8
      dp3 r8.w, r30, r9
      add r8.w, r8.w, r8.w
      mad r10.xyz, r30, r8.w, -r9 // the view reflected off the drop
      dp3_sat r8.w, r10, -c17
      pow r8.w, r8.w, c179.w
      mul r8.w, r8.w, r28.w
      mul r8.w, r8.w, r27.x
      mul r10.xyz, c18, c18.w
      mad r31.xyz, r10, r8.w, r31
    endif
    max r29.z, r29.z, c181.w
    rcp r29.w, r29.z
    mul r0.yzw, r31.xxyz, r29.w
    mov r0.x, r29.z
'''


def body(path):
    return path.read_text().replace('\r\n', '\n')


def lines_matching(lines, text):
    return [i for i, l in enumerate(lines) if l.strip() == text]


def insert(text, anchor, block, what, before=False):
    lines = text.split('\n')
    hits = lines_matching(lines, anchor)
    if len(hits) != 1:
        sys.exit(f'{what}: "{anchor}" found {len(hits)} times')
    i = hits[0] if before else hits[0] + 1
    lines[i:i] = block.rstrip('\n').split('\n')
    return '\n'.join(lines)


def patch_ps(n):
    path = ROOT / f'gta_vehicle_vehglassPS{n}.asm'
    text = body(path)
    if TAG in text:
        sys.exit(f'{path} is patched already')
    old = 'def c164, 12, 48, 9.99999997e-007, 0 // step count limits'
    if text.count(old) != 1:
        sys.exit(f'{path}: no glass reflections patch under it')
    text = text.replace(old, 'def c164, 12, 48, 9.99999997e-007, 0.0199999996 // step count limits, thickness growth a metre')
    text = insert(text, 'defi i1, 6, 0, 0, 0', PS_DEFS, path, before=True)
    text = insert(text, 'dcl_texcoord9 v9', PS_DCLS, path)
    text = insert(text, 'add r29.x, r29.x, r28.y', f'''\
          mul r29.z, r16.w, c164.w // {TAG}: thicker the further away
          mad r29.x, r28.y, r29.z, r29.x''', path)
    text = insert(text, 'mov r4.xyz, r15', COVER_AND_DROPS, path)
    text = insert(text, 'lrp r1.w, r20.y, c110.y, r20.x', SHADOW, path)
    text = insert(text, 'mul oC0.w, r0.x, c39.x', COMPOSE, path, before=True)
    path.write_text(text)


VS_DCLS = f'''\
    dcl_texcoord4 o7 // {TAG}: position in the car's space, clip x
    dcl_texcoord5 o8 // {TAG}: normal in the car's space, clip y
    dcl_texcoord6 o9 // {TAG}: world up in the car's space, clip w
'''

# VS1: the world matrix's columns c0-c2 are where the car's axes point; VS2: the bone's rows c64-c66[a0] give world
# x, y and z. The clip position is in r1 (VS1) or r0 (VS2) by then.
VS_OUT = {
    1: f'''\
    mov o7.xyz, r3 // {TAG}: the position, normal and world up in the car's space, and the clip position
    mov o8.xyz, v4
    mov r8.x, c0.z
    mov r8.y, c1.z
    mov r8.z, c2.z
    nrm r7.xyz, r8
    mov o9.xyz, r7
    mov o7.w, r1.x
    mov o8.w, r1.y
    mov o9.w, r1.w
''',
    2: f'''\
    mov o7.xyz, r3 // {TAG}: the position, normal and world up in the bone's space, and the clip position
    mov o8.xyz, r4
    mov r8.xyz, c66[a0.x]
    nrm r7.xyz, r8
    mov o9.xyz, r7
    mov o7.w, r0.x
    mov o8.w, r0.y
    mov o9.w, r0.w
''',
}


def patch_vs(n):
    path = ROOT / f'gta_vehicle_vehglassVS{n}.asm'
    text = body(path)
    if TAG in text:
        sys.exit(f'{path} is patched already')
    text = insert(text, 'dcl_texcoord9 o10 // LogDepth Interpolator', VS_DCLS, path)
    text = insert(text, 'dp3 o10.xy, c10.xyw, c10.xyw', VS_OUT[n], path, before=True)
    path.write_text(text)


for n in (2, 3, 4):
    patch_ps(n)
for n in (1, 2):
    patch_vs(n)
