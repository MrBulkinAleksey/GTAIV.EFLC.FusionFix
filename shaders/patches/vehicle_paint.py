#!/usr/bin/env python3
# Writes vehicle_paint.patch: a finish for car paint and rain drops on it, in the deferred pixel shader (PS5) of
# gta_vehicle_paint1/2/3 and their deferred vertex shaders (VS3 drawn whole, VS4 skinned). source/vehiclepaint.ixx
# sets c175-c177 at the start of every G-buffer pass; all zero leaves the stock paint.
#
# The finish, on clean paint only (dirt keeps the stock look):
#  - a clearcoat: Schlick's Fresnel from 0.045 head on raises the specular where it beats the paint's own (near the
#    silhouette), and a gloss floor sharpens the sun's highlight and the reflections;
#  - the sun highlight the game bakes into the albedo (specular2, which ignores shadows: it shows in tunnels and at
#    night) turned down; the lighting pass still draws the shadowed one;
#  - richer colour (saturation around the luminance) and a blue-violet tint growing towards the silhouette.
#
# Rain drops, while it rains: the vertex shaders pass the position, normal and world up in the car's own space
# (the bone's for skinned parts), so the drops stay on the bodywork as the car moves. On the surface a frame of
# (across, up) is built from the normal and world up; in it:
#  - beads, a drop at a random spot in some of the cells of a grid, more of them the harder it rains;
#  - running drops on the sides: columns with drops sliding down at their own speed, a wet trail above each.
# Their height bends the G-buffer normal (bump mapping from screen space derivatives, Mikkelsen's surface gradient,
# so no tangent frame is needed); drops and trails turn glossy, the whole car gets a wet film's gloss.
#
# Usage: vehicle_paint.py <FusionShaders checkout>; the patch is the diff it leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8'
TAG = 'FusionFix vehicle_paint.patch'

# The pixel shader's own registers: where each variant holds its cleanliness after the dirt, the baked highlight's
# colour before it goes into the albedo, and the normal it stores.
VARIANTS = {
    'gta_vehicle_paint1': dict(clean='r4.x', spec2='r5', spec2_add='mad r2.xyz, r5, r2.x, r0', normal='mad r1.xyz, r1, r1.w, c1.x'),
    'gta_vehicle_paint2': dict(clean='r4.x', spec2='r5', spec2_add='mad r2.xyz, r5, r2.x, r0', normal='mad r1.xyz, r1, r1.w, c1.x'),
    'gta_vehicle_paint3': dict(clean='r5.x', spec2='r4', spec2_add='mad r1.yzw, r4.xxyz, r1.y, r0.xxyz', normal='mad r1.xyz, r2, r2.w, c1.x'),
}

PS_DEFS = f'''\
    def c180, 13.6999998, 0.5, 0.100000001, 0.25 // {TAG}: presence hash, a half, a tenth, a quarter
    def c181, 0.5, 0.100000001, 0.25, 0 // {TAG}: columns and rows of running drops per bead cell, the head's place in its cell
    def c182, 0.300000012, 0.349999994, 3.33333325, 14.2857141 // {TAG}: the head's place across its column, 1 / half width, 1 / half height
    def c183, 1.66666663, 0.100000001, 0.100000001, 0.5 // {TAG}: 1 / trail length, darkening under drops, a wet film's specular, a drop's
    def c184, 0.899999976, 0.970000029, 0.955000043, 0.0450000018 // {TAG}: a wet film's gloss, a drop's; Schlick's 1 - F0 and F0 for the clearcoat
    def c185, 0.212599993, 0.715200007, 0.0722000003, 0 // {TAG}: luminance, zero
    def c186, 0.0799999982, 0.0399999991, 0.119999997, 0 // {TAG}: the pearl tint
    def c187, 0, 0.5, 0.00100000005, 1 // {TAG}: specular as stored, flat tops, one
    def c188, 0.103100002, 0.103000000, 0.0973000005, 33.3300018 // {TAG}: the hash
    def c189, 0.400000006, 0.300000012, 0.180000007, 0.100000001 // {TAG}: a bead's centre within its cell, its radius
'''

PS_DCLS = f'''\
    dcl_texcoord2 v5 // {TAG}: position in the car's space
    dcl_texcoord4 v6 // {TAG}: normal in the car's space
    dcl_texcoord5 v7 // {TAG}: world up in the car's space
'''


def hash3(src, dst, tmp):
    # Dave Hoskins' hash without sine, three numbers in [0, 1) from the two in src.xy (as src.xyx)
    return f'''\
      mul {dst}.xyz, {src}, c188
      frc {dst}.xyz, {dst}
      add {tmp}.xyz, {dst}.yzxw, c188.w
      dp3 {tmp}.x, {dst}, {tmp}
      add {dst}.xyz, {dst}, {tmp}.x
      add {tmp}.xyz, {dst}.xxyw, {dst}.yzzw
      mul {tmp}.xyz, {tmp}, {dst}.zyxw
      frc {dst}.xyz, {tmp}
'''


def finish_and_drops(clean):
    return f'''\
    // --- {TAG}: the finish (c175: clearcoat strength and gloss floor, richness - 1, pearl tint; c176.xy: baked
    // --- highlight cut, clearcoat gloss share) on clean paint, and the rain (c176.z: share of cells with a drop,
    // --- c176.w: how far running drops slid; c177: cells per metre, drop height, running share, wet film).
    mov r20.w, {clean} // 1 where clean, less under dirt
    add r8.xyz, c0.x, v1
    nrm r27.xyz, r8 // the surface's normal, bent by the drops below
    add r8.xyz, c0.x, v2
    nrm r9.xyz, r8 // towards the camera
    dp3_sat r23.x, r27, r9
    add r23.x, -r23.x, c187.w // 1 - N.V
    mul r23.y, r23.x, r23.x
    mul r23.z, r23.y, r23.y
    mul r23.z, r23.z, r23.x // (1 - N.V)^5
    dp3 r8.x, r0, c185
    add r8.yzw, r0.xxyz, -r8.x
    mul r8.x, r20.w, c175.z
    mad r0.xyz, r8.yzww, r8.x, r0 // saturation around the luminance
    max r0.xyz, r0, c185.w
    mul r8.x, r23.y, c175.w
    mul r8.x, r8.x, r20.w
    mad r0.xyz, c186, r8.x, r0 // the pearl tint towards the silhouette
    mov_sat r20.x, c177.w // how wet: 0 dry, 1 in the rain
    mov r24, c185.w // drops in x, trails in y
    mov r8.x, c176.z // how many cells hold a drop
    if_lt c185.w, r8.x
      // The surface's frame in the car's space: up along it (world up, or the car's x on flat tops) and across
      mov r10.xyz, v6
      mov r11.xyz, v7
      mov r12.xyz, v5
      nrm r13.xyz, r10
      dp3 r8.y, r13, r11
      mad r14.xyz, r13, -r8.y, r11
      add r14.x, r14.x, c187.z
      nrm r15.xyz, r14
      crs r16.xyz, r13, r15
      dp3 r17.x, r12, r16
      dp3 r17.y, r12, r15 // across and up, in metres
      // Beads: cells of 1 / c177.x metres, a drop at a random spot in some of them
      mul r17.zw, r17.xyxy, c177.x
      frc r18.xy, r17.zwzw
      add r18.zw, r17.zwzw, -r18.xyxy
{hash3('r18.zwzw', 'r9', 'r10')}\
      mad r10.xy, r9, c189.x, c189.y
      add r10.xy, r18, -r10
      mad r10.z, r9.z, c189.z, c189.w
      rcp r10.z, r10.z
      mul r10.xy, r10, r10.z
      dp2add r10.w, r10, r10, c185.w
      add_sat r10.w, -r10.w, c187.w // the bead's dome
      mul r9.w, r9.z, c180.x
      frc r9.w, r9.w
      add r9.w, r9.w, -r8.x
      cmp r9.w, r9.w, c187.x, c187.w
      mul r24.x, r10.w, r9.w
      // Running drops, on the sides: columns of half the bead density, each sliding at its own speed
      add r8.y, -r8_abs.y, c187.w
      add_sat r8.y, r8.y, r8.y
      mul r8.y, r8.y, c177.z
      mul r17.z, r17.x, c177.x
      mul r17.z, r17.z, c181.x
      frc r18.x, r17.z
      add r18.z, r17.z, -r18.x // the column
{hash3('r18.zzzz', 'r9', 'r10')}\
      add r9.w, r9.x, c180.y
      mul r9.w, r9.w, c176.w // how far it slid: 0.5 to 1.5 times c176.w metres
      add r9.w, r9.w, r17.y
      mul r9.w, r9.w, c177.x
      mad r9.w, r9.w, c181.y, r9.y
      frc r18.y, r9.w
      add r18.w, r9.w, -r18.y // the row
      mad r10.y, r9.z, c182.x, c182.y
      add r10.x, r18.x, -r10.y
      mul r10.x, r10.x, c182.z
      add r10.y, r18.y, -c181.z
      mul r10.z, r10.y, c182.w
      mul r10.w, r10.x, r10.x
      mad r10.w, r10.z, r10.z, r10.w
      add_sat r10.w, -r10.w, c187.w // the head's dome
      add_sat r11.x, -r10_abs.x, c187.w
      mul r11.y, r10.y, c183.x
      add_sat r11.z, -r11.y, c187.w
      cmp r11.z, r10.y, r11.z, c185.w
      mul r11.z, r11.z, r11.x // the trail above it
{hash3('r18.zwzw', 'r9', 'r12')}\
      add r9.w, r9.x, -r8.x
      cmp r9.w, r9.w, c187.x, c187.w
      mul r9.w, r9.w, r8.y
      mul r24.y, r11.z, r9.w
      mul r10.w, r10.w, r9.w
      max r24.x, r24.x, r10.w
      // The drops' height (c177.y metres) bends the normal: Mikkelsen's surface gradient, from the derivatives of
      // the position (v2 is the camera minus it, which flips the gradient's sign and so its sum below)
      mul r8.w, r24.x, c177.y
      dsx r9.xyz, v2
      dsy r10.xyz, v2
      crs r11.xyz, r10, r27
      crs r12.xyz, r27, r9
      dp3 r13.x, r9, r11
      dsx r13.y, r8.w
      dsy r13.z, r8.w
      mul r14.xyz, r11, r13.y
      mad r14.xyz, r12, r13.z, r14
      cmp r13.w, r13.x, c187.w, -c187.w
      mul r14.xyz, r14, r13.w
      mad r14.xyz, r27, r13_abs.x, r14
      nrm r27.xyz, r14
      mul r8.x, r24.x, c183.y
      add r8.x, -r8.x, c187.w
      mul r0.xyz, r0, r8.x // a little darker under drops
    endif
'''


def spec2_cut(reg):
    return f'''\
    mul r8.x, r20.w, c176.x // {TAG}: the baked highlight turned down on clean paint
    add r8.x, -r8.x, c187.w
    mul {reg}.xyz, {reg}, r8.x
'''


SPECULAR = f'''\
    mul r25.x, r0.x, c2.y // {TAG}: the paint's own specular, halved as stored
    mad r25.y, r23.z, c184.z, c184.w
    mul r25.y, r25.y, c187.y
    mul r25.y, r25.y, c175.x
    mul r25.y, r25.y, r20.w
    max r25.x, r25.x, r25.y // the clearcoat's where stronger
    mul r25.z, r20.x, c183.z
    max r25.x, r25.x, r25.z // a wet film
    lrp r26.x, r24.x, c183.w, r25.x // drops
    mov oC2.x, r26.x
'''

GLOSS = f'''\
    rcp r25.x, r0.x // {TAG}: the paint's own gloss
    max r25.y, r25.x, c175.y
    mul r25.z, r20.w, c176.y
    lrp r26.x, r25.z, r25.y, r25.x // the clearcoat's floor on clean paint
    mul r25.w, r20.x, c184.x
    max r26.y, r26.x, r25.w // a wet film
    max r26.z, r24.x, r24.y
    lrp r27.x, r26.z, c184.y, r26.y // drops and trails
    mov oC2.y, r27.x
'''


def body(path):
    return path.read_text().replace('\r\n', '\n')


def replace_once(text, old, new, what):
    lines = text.split('\n')
    hits = [i for i, l in enumerate(lines) if l.strip() == old]
    if len(hits) != 1:
        sys.exit(f'{what}: "{old}" found {len(hits)} times')
    indent = lines[hits[0]][:len(lines[hits[0]]) - len(lines[hits[0]].lstrip())]
    lines[hits[0]] = new.rstrip('\n') if new.startswith(' ') else indent + new
    return '\n'.join(lines)


def insert_after(text, pattern, block, what, before=False):
    lines = text.split('\n')
    hits = [i for i, l in enumerate(lines) if re.match(pattern, l.strip())]
    if len(hits) != 1:
        sys.exit(f'{what}: /{pattern}/ found {len(hits)} times')
    i = hits[0] if before else hits[0] + 1
    lines[i:i] = block.rstrip('\n').split('\n')
    return '\n'.join(lines)


def patch_ps(name, v):
    path = ROOT / name / f'{name}PS5.asm'
    text = body(path)
    if TAG in text:
        sys.exit(f'{path} is patched already')
    text = insert_after(text, r'def c100,', PS_DEFS, path)
    text = insert_after(text, r'dcl_texcoord9 v9', PS_DCLS, path)
    # The finish and the drops right after the dirt, which leaves the albedo in r0 and the cleanliness behind
    lines = text.split('\n')
    endif = next(i for i, l in enumerate(lines) if l.strip() == 'endif')
    lines[endif + 1:endif + 1] = finish_and_drops(v['clean']).rstrip('\n').split('\n')
    text = '\n'.join(lines)
    text = insert_after(text, re.escape(v['spec2_add']), spec2_cut(v['spec2']), path, before=True)
    text = replace_once(text, v['normal'], f'add r1.xyz, r27, c1.x // {TAG}: the normal the drops bent', path)
    text = replace_once(text, 'mul oC2.x, r0.x, c2.y', SPECULAR, path)
    text = replace_once(text, 'rcp oC2.y, r0.x', GLOSS, path)
    path.write_text(text)


VS_DCLS = f'''\
    dcl_texcoord2 o7.xyz // {TAG}: position in the car's space
    dcl_texcoord4 o8.xyz // {TAG}: normal in the car's space
    dcl_texcoord5 o9.xyz // {TAG}: world up in the car's space
'''

# VS3: the world matrix's columns c0-c2 are where the car's axes point, so world up in the car's space is their z.
# VS4: the bone's rows c64-c66[a0] give world x, y and z, so world up in the bone's space is the third row.
VS_OUT = {
    3: f'''\
    mov o7.xyz, r3 // {TAG}: the position (dented), the normal and world up in the car's space
    mov o8.xyz, v4
    mov r8.x, c0.z
    mov r8.y, c1.z
    mov r8.z, c2.z
    nrm r7.xyz, r8
    mov o9.xyz, r7
''',
    4: f'''\
    mov o7.xyz, r3 // {TAG}: the position (dented), the normal and world up in the bone's space
    mov o8.xyz, r4
    mov r8.xyz, c66[a0.x]
    nrm r7.xyz, r8
    mov o9.xyz, r7
''',
}


def patch_vs(name, n):
    path = ROOT / name / f'{name}VS{n}.asm'
    text = body(path)
    if TAG in text:
        sys.exit(f'{path} is patched already')
    text = insert_after(text, r'dcl_texcoord9 o10', VS_DCLS, path)
    text = insert_after(text, re.escape('dp3 o10.xy, c10.xyw, c10.xyw'), VS_OUT[n], path, before=True)
    path.write_text(text)


for name, v in VARIANTS.items():
    patch_ps(name, v)
    patch_vs(name, 3)
    patch_vs(name, 4)
