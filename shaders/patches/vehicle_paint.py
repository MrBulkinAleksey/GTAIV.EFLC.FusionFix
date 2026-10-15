#!/usr/bin/env python3
# Writes vehicle_paint.patch: a finish for car paint and rain drops on it, in the deferred pixel shader (PS5) of
# gta_vehicle_paint1/2/3 and their deferred vertex shaders (VS3 drawn whole, VS4 skinned), and a mirror's gloss for
# the car glass drawn into the G-buffer (gta_vehicle_vehglass PS5). source/vehiclepaint.ixx
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

from vehicle_raindrops import DEFS as PS_DEFS, drops, drops_bend

TAG = 'FusionFix vehicle_paint.patch'

ROOT = Path(sys.argv[1]) / 'win32_30_nv8'

# The pixel shader's own registers: where each variant holds its cleanliness after the dirt, the baked highlight's
# colour before it goes into the albedo, and the normal it stores.
VARIANTS = {
    'gta_vehicle_paint1': dict(clean='r4.x', spec2='r5', spec2_add='mad r2.xyz, r5, r2.x, r0', normal='mad r1.xyz, r1, r1.w, c1.x'),
    'gta_vehicle_paint2': dict(clean='r4.x', spec2='r5', spec2_add='mad r2.xyz, r5, r2.x, r0', normal='mad r1.xyz, r1, r1.w, c1.x'),
    'gta_vehicle_paint3': dict(clean='r5.x', spec2='r4', spec2_add='mad r1.yzw, r4.xxyz, r1.y, r0.xxyz', normal='mad r1.xyz, r2, r2.w, c1.x'),
}


PS_DCLS = f'''\
    dcl_texcoord2 v5 // {TAG}: position in the car's space
    dcl_texcoord4 v6 // {TAG}: normal in the car's space
    dcl_texcoord5 v7 // {TAG}: world up in the car's space
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
{drops('v5', 'v6', 'v7')}      mul r8.x, r24.x, c183.y
      add r8.x, -r8.x, c187.w
      mul r0.xyz, r0, r8.x // a little darker under drops
    endif
{drops_bend()}'''


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


# Glass drawn into the G-buffer (gta_vehicle_vehglass PS5, deferred_draw) gets a mirror's gloss and a little specular
# where clean, so SSR reflects in it as in the forward glass; c176.y is 0 with Vehicle Paint off.
GLASS_DEFS = f'''\
    def c184, 0.0599999987, 0.970000029, 0, 0 // {TAG}: deferred glass's specular and gloss floors
'''

GLASS_SPECULAR = f'''\
    mul r20.x, r2.x, c1.x // {TAG}: the glass's own specular, halved as stored
    mul r20.y, r4.x, c176.y // clean glass, with Vehicle Paint on
    mul r20.z, r20.y, c184.x
    max oC2.x, r20.x, r20.z
'''

GLASS_GLOSS = f'''\
    rcp r20.x, r1.x // {TAG}: the glass's own gloss, raised to a mirror's
    max r20.z, r20.x, c184.y
    lrp r21.x, r20.y, r20.z, r20.x
    mov oC2.y, r21.x
'''


def patch_deferred_glass():
    path = ROOT / 'gta_vehicle_vehglass' / 'gta_vehicle_vehglassPS5.asm'
    text = body(path)
    if TAG in text:
        sys.exit(f'{path} is patched already')
    text = insert_after(text, r'def c1, 0\.5, 0\.37109375', GLASS_DEFS, path)
    text = replace_once(text, 'mul oC2.x, r2.x, c1.x', GLASS_SPECULAR, path)
    text = replace_once(text, 'rcp oC2.y, r1.x', GLASS_GLOSS, path)
    path.write_text(text)


for name, v in VARIANTS.items():
    patch_ps(name, v)
    patch_vs(name, 3)
    patch_vs(name, 4)
patch_deferred_glass()
