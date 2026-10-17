#!/usr/bin/env python3
# Writes ped_cloth_sheen.patch: the sun and sky light on people's clothes in deferred_lighting PS1-4.
#
# Clothes are where gta_ped and gta_ped_reflect add an eighth of a step to the material ID: the component loop
# of the ped draw (MarkPedSkin in postfx.ixx) sets c150.x to it for the components ClothComponents names, as it
# sets a quarter step for skin (ped_skin_scattering_mask.patch). The skin test of the light shaders,
# saturate(16 frac(ID 255 + 0.25) - 6), is 0 there, so only these four shaders tell clothes apart:
# saturate(16 f - 5) less the skin's.
#
# On clothes:
# - sheen: the fibres standing out of the cloth catch light at grazing angles, so the rim of a sleeve or a
#   trouser leg lights up, against the sun most of all. From the sun, the Charlie sheen lobe at roughness 0.5
#   with Neubelt's visibility (pi D V N.L = (1 - N.H^2) N.L / (2 (N.L + N.V - N.L N.V))) and its shadow; from
#   the ambient, (1 - N.V)^c164.y of it. c164.x times that, in a colour of its own: the square root of the
#   albedo, c164.z of the way to white. It is added after the albedo: a dark cloth's sheen is not much darker
#   (a first version lit the sheen as diffuse light, and on a brown jacket it made 2% of the lit side).
# - the game's highlight and reflection are scaled by 1 - c164.w: its Phong lobe made cotton look plastic.
# - leather and vinyl drawn as clothes are left out where the ID is written: gta_ped puts no eighth on a
#   texel whose specular intensity reaches ClothSpecularGate (ped_skin_scattering_mask_cloth_gate.patch). Every ped
#   writes the same gloss, so the gloss could not tell them apart.
# - c191.z 1 shows the masks (ClothDebug) on people, c191.w times as bright: red clothes, green clothes times
#   their specular intensity (four times the G-buffer's: it holds a quarter of it, plus half the game's ped
#   rim squared at the silhouette), blue skin; everything else keeps its colour.
#
# Usage: ped_cloth_sheen.py <FusionShaders checkout with the patches that sort before this one applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'deferred_lighting'
TAG = 'FusionFix cloth sheen'

# Per shader: the line that leaves 1 - |N.V| in a register, the view ray there (from the camera), the normal
# where N.L is taken, a constant holding 1, the line after which the
# G-buffer specular intensity is in a register, the sun's colour, its shadow, the ambient term (as it is written and as
# it is read), the line the ambient is added to the direct light in, the specular (written and read), and
# the line that writes the colour.
SHADERS = {
    1: dict(view='r3', normal='r2', rim=('add r1.x, -r1_abs.x, c2.w', 'r1.x'), one='c2.w', spec_in=('mov r18.x, r4.y', 'r4.x'),
            sun='r6', shadow='r0.y', amb=('r1.yzw', 'r1', 'r12.xxyz', 'r1.yzww'),
            before='mad r1.xyz, r2, r27, r1.yzww', spec=('r0.xzw', 'r12.xxyz'),
            final=('mad oC0.xyz, r2, r1, r0.xzww', 'mad r2.xyz, r2, r1, r0.xzww')),
    2: dict(view='r3', normal='r2', rim=('add r1.x, -r1_abs.x, c3.z', 'r1.x'), one='c3.z', spec_in=('mov r18.x, r4.y', 'r4.x'),
            sun='r5.yzww', shadow='r0.y', amb=('r2.xyz', 'r2', 'r12', 'r2'),
            before='mad r1.xyw, r1, r27.xyzz, r2.xyzz', spec=('r0.xzw', 'r12.xxyz'),
            final=('mad oC0.xyz, r2, r1.xyww, r0.xzww', 'mad r2.xyz, r2, r1.xyww, r0.xzww')),
    3: dict(view='r3', normal='r1', rim=('add r0.x, -r0_abs.x, c0.z', 'r0.x'), one='c0.z', spec_in=('texld r3, v0, s2', 'r3.x'),
            sun='r2', shadow='r29.x', amb=('r1.yzw', 'r1', 'r12.xxyz', 'r1.yzww'),
            before='mad r1.xyz, r2, r25, r1.yzww', spec=('r0.xyz', 'r12'),
            final=('mad oC0.xyz, r2, r1, r0', 'mad r2.xyz, r2, r1, r0')),
    4: dict(view='r3', normal='r1', rim=('add r0.x, -r0_abs.x, c3.z', 'r0.x'), one='c3.z', spec_in=('texld r3, v0, s2', 'r3.x'),
            sun='r3', shadow='r29.x', amb=('r1.yzw', 'r1', 'r12.xxyz', 'r1.yzww'),
            before='mad r1.xyz, r3, r25, r1.yzww', spec=('r0.xzw', 'r12.xxyz'),
            final=('mad oC0.xyz, r2, r1, r0.xzww', 'mad r2.xyz, r2, r1, r0.xzww')),
}

SKIN_FRC = 'frc r26.x, r26.x'
SKIN_MASK = 'mad_sat r26.x, r26.x, c156.z, c156.w'
DEF_AFTER = 'def c156, 255, 0.25, 16, -6'


def find_one(lines, text, start=0):
    hits = [i for i in range(start, len(lines)) if lines[i].strip().split(' //')[0].strip() == text]
    if len(hits) != 1:
        sys.exit(f'{text!r}: {len(hits)} matches after line {start}')
    return hits[0]


def indent(line):
    return line[:len(line) - len(line.lstrip())]


def patch(number, s):
    path = ROOT / f'deferred_lightingPS{number}.asm'
    lines = path.read_text().split('\n')
    one = s['one']

    i = find_one(lines, DEF_AFTER.split(' //')[0])
    lines.insert(i + 1, indent(lines[i]) + f'def c166, 16, -5, 0.0001, 0.5 // {TAG}: the clothes test; a floor and a half for the sheen lobe')

    branch = find_one(lines, 'mov r26.x, ' + ('r21.x' if number in (1, 3) else 'r4.x' if number == 2 else 'r22.x'))

    i = find_one(lines, s['rim'][0], branch)
    lines[i + 1:i + 1] = [indent(lines[i]) + f'mov r30.x, {s["rim"][1]} // {TAG}: 1 - N.V',
                          indent(lines[i]) + f'add r30.yzw, -c17.xxyz, -{s["view"]}.xxyz // {TAG}: towards the sun plus towards the camera']

    i = find_one(lines, s['spec_in'][0], branch)
    lines[i + 1:i + 1] = [indent(lines[i]) + f'add r26.z, {s["spec_in"][1]}, {s["spec_in"][1]} // {TAG}: the specular intensity, for ClothDebug',
                          indent(lines[i]) + 'add r26.z, r26.z, r26.z']

    i = find_one(lines, SKIN_FRC, branch)
    pad = indent(lines[i])
    lines[i + 1:i + 1] = [pad + f'mad_sat r26.y, r26.x, c166.x, c166.y // {TAG}: 1 on clothes (an eighth) and skin (a quarter), 0 elsewhere',
                          pad + f'dp3 r26.w, r30.yzww, r30.yzww // {TAG}: N.H',
                          pad + f'max r26.w, r26.w, c166.z',
                          pad + 'rsq r26.w, r26.w',
                          pad + f'dp3 r30.y, {s["normal"]}, r30.yzww',
                          pad + 'mul r30.y, r30.y, r26.w',
                          pad + f'mov r30.z, r1.x // {TAG}: N.L']
    i = find_one(lines, SKIN_MASK, i)
    lines.insert(i + 1, pad + f'add r26.y, r26.y, -r26.x // {TAG}: 1 on clothes only')

    amb_read = s['amb'][3]
    spec_dst = s['spec'][0]
    i = find_one(lines, s['before'], i)
    lines[i:i] = [pad + line for line in [
        f'// --- {TAG} (c164, c191: ClothSheen in postfx.ixx): fibres catch the light around a sleeve\'s rim, and the',
        f'// --- game\'s plastic highlight is toned down.',
        'mov r26.w, r26.y // clothes',
        'mul r30.w, r26.w, c164.w',
        f'add r30.w, -r30.w, {one}',
        f'mul {spec_dst}, r0, r30.w // the game\'s highlight and reflection',
        'mul r30.y, r30.y, r30.y',
        f'add r30.y, -r30.y, {one} // 1 - N.H^2',
        'mov_sat r30.z, r30.z',
        f'add r30.w, -r30.x, {one} // N.V',
        'mad r12.x, -r30.z, r30.w, r30.w',
        'add r12.x, r12.x, r30.z',
        'max r12.x, r12.x, c166.z',
        'rcp r12.x, r12.x',
        'mul r12.x, r12.x, r30.z',
        'mul r12.x, r12.x, r30.y',
        'mul r12.x, r12.x, c166.w // the sun\'s lobe: (1 - N.H^2) N.L / (2 (N.L + N.V - N.L N.V))',
        f'mul r12.x, r12.x, {s["shadow"]}',
        f'mul r12.xyz, {s["sun"]}, r12.x',
        'pow r30.x, r30.x, c164.y',
        f'mad r12.xyz, {amb_read}, r30.x, r12 // and the ambient around the rim',
        'mul r12.xyz, r12, r26.w',
        'mul r12.xyz, r12, c164.x // the light the fibres catch, kept in r12 until the albedo is known',
    ]]

    i = find_one(lines, s['final'][0], i)
    lines[i:i + 1] = [pad + line for line in [
        f'rsq r30.x, r2.x // {TAG}: the sheen\'s colour, the square root of the albedo, c164.z of the way to white',
        'rsq r30.y, r2.y',
        'rsq r30.z, r2.z',
        'rcp r30.x, r30.x',
        'rcp r30.y, r30.y',
        'rcp r30.z, r30.z',
        'mad r30.xyz, -r30, c164.z, r30',
        'add r30.xyz, r30, c164.z',
        s['final'][1],
        'mad r2.xyz, r12, r30, r2 // the sheen',
        f'mov r12.x, r26.y // {TAG}: ClothDebug shows clothes red, their specular intensity green and skin blue',
        'mul r12.y, r26.y, r26.z',
        'mov r12.z, r26.x',
        'mul r12.xyz, r12, c191.w // bright enough to read after the tone map',
        'max r30.w, r26.x, r26.y // only on people: the rest keeps its own colour',
        'mul r30.w, r30.w, c191.z',
        'lrp oC0.xyz, r30.w, r12, r2',
    ]]

    path.write_text('\n'.join(lines))


def main():
    for number, s in SHADERS.items():
        patch(number, s)


if __name__ == '__main__':
    main()
