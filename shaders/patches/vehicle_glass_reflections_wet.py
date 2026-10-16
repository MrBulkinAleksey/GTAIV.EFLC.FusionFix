#!/usr/bin/env python3
# Writes vehicle_glass_reflections_wet.patch, on top of vehicle_glass_reflections.patch, for the car glass's forward
# pixel shaders (gta_vehicle_vehglass PS2-PS4) and their vertex shaders (VS1 drawn whole, VS2 skinned):
#  - the march's hit takes a thickness growing 2% a metre with the distance, so far windows find their hits;
#  - what the march found covers what is behind the glass as much as the game's Fresnel shows it: the glass's
#    alpha rises towards it, instead of the reflection being added over the full view through the glass;
#  - rain drops on the glass (vehicle_raindrops.py, the same as on the paint): each shows the scene behind it
#    refracted (the lit opaque scene the glass reflections copy, shifted by the drop's slope on the screen),
#    covers the view through the glass and catches the sun or the moon, in its shadow. Its own Fresnel mixes in the
#    environment map along the view reflected off it, and its rim, where the dome turns away from the glass, shows
#    dark, as light there bends out of the drop or reflects inside it: refraction alone shows nothing on a dark window
#    at night, the reflection at the rim and the dark ring are what make drops on glass seen;
#  - from inside the car the glass keeps less of its tint, so the street shows through dark windows.
# The drops need the glass reflections' parameter texture and scene copy; without them only the cover is left out too.
#
# Usage: vehicle_glass_reflections_wet.py <FusionShaders checkout with the other patches applied>; the patch is the
# diff it leaves (git diff).
import re
import sys
from pathlib import Path

from vehicle_raindrops import DEFS, drops, drops_bend

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'gta_vehicle_vehglass'
TAG = 'FusionFix vehicle_glass_reflections_wet.patch'

PS_DEFS = DEFS + f'''\
    def c179, 0.0299999993, -0.0299999993, 0.899999976, 400 // {TAG}: refraction on the screen, a drop's cover, the glint's power
    def c198, 0.980000019, 0.0199999996, 4, 0.0799999982 // {TAG}: water's Fresnel (1 - F0, F0); the rim from 1 - n.n_glass 0.08, full by 0.33
    def c199, 0.850000024, 0.625, -0.25, 0.400000006 // {TAG}: how dark the rim; the game's Fresnel back from its 2 (0.8 F + 0.2); the tint from inside
'''

PS_DCLS = f'''\
    dcl_texcoord4 v6 // {TAG}: position in the car's space, clip x
    dcl_texcoord5 v7 // {TAG}: normal in the car's space, clip y
    dcl_texcoord6 v8 // {TAG}: world up in the car's space, clip w
'''

COVER_AND_DROPS = f'''\
    // {TAG}: how much of the view through the glass the found reflection covers: its share times the Fresnel.
    // The game's r1.z is 2 (0.8 F + 0.2) with F = (1 - |n.v|)^4, its reflection's strength, not a share: taken as one
    // it covered 40% face on and all of it at a glance, and windows showed nothing of the cabin. F back from it,
    // then 0.02 + 0.98 F.
    mad_sat r27.w, r1.z, c199.y, c199.z
    mad r27.w, r27.w, c198.x, c198.y
    mul r27.w, r27.w, r17.w
    // Rain drops: r28 takes the scene they show and how much they cover, r30 the normal they bend
    mov r28, c185.w
    mov r30.xyz, r2
    mov r30.w, r17.w // the found reflection's share, kept from the drops' frame (r30.w is free after the march)
    mov r19.xyz, r2 // the glass's own normal, for the drops' rims (r19 is free until the depth write)
    mov r24, c185.w
    mov r27.xyz, r2
    mov r8.x, c176.z
    if_lt c185.w, r8.x
{drops('v6', 'v7')}\
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

INSIDE = f'''\
    // {TAG}: seen from inside the car (the camera behind the glass's normal) the glass keeps 40% of its tint: through
    // dark windows the street was hard to see from the driver's seat, while from outside they stay as dark as they were.
    add r8.xyz, -v4, c15
    dp3 r8.w, r8, r19
    mov r8.x, c199.w
    cmp r8.w, r8.w, c187.w, r8.x
    mul r0.x, r0.x, r8.w
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
    if_lt c185.w, r28.w
      add r8.xyz, -v4, c15
      nrm r9.xyz, r8
      dp3 r8.w, r30, r9
      mov_sat r11.x, r8.w
      add r11.x, -r11.x, c187.w
      mul r11.y, r11.x, r11.x
      mul r11.y, r11.y, r11.y
      mul r11.x, r11.y, r11.x
      mad r11.x, r11.x, c198.x, c198.y // the drop's Fresnel
      add r8.w, r8.w, r8.w
      mad r10.xyz, r30, r8.w, -r9 // the view reflected off the drop
      // The environment map along it, read as the game reads it for the glass (with our constants: c1 differs
      // between PS2, PS3 and PS4)
      nrm r13.xyz, r10
      mov_sat r12.z, r13.z
      add r12.z, r12.z, c187.w
      add r12.z, r12.z, r12.z
      rcp r12.z, r12.z
      mad r12.xy, r13, r12.z, c180.y
      add r12.xy, -r12, c187.w
      mov r12.zw, c185.w
      texldl r12, r12, s4
      // Dark at the rim, where the dome turns away from the glass
      dp3 r11.y, r30, r19
      add r11.y, -r11.y, c187.w
      add r11.y, r11.y, -c198.w
      mul_sat r11.y, r11.y, c198.z
      mul r11.y, r11.y, c199.x
      add r11.y, -r11.y, c187.w
      mul r13.xyz, r28, r11.y
      lrp r14.xyz, r11.x, r12, r13
      mad r31.xyz, r14, r29.w, r31 // the drop: what it refracts, darker at the rim, and what it reflects
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
    text = insert(text, 'mov_sat r0.x, r0.x', INSIDE, path, before=True)
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
