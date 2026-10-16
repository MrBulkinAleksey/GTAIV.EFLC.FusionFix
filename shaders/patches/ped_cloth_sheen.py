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
#   trouser leg turns towards the light around it. (1 - N.V)^c164.y times c164.x, lit by the ambient and by
#   the sun wrapped half past the terminator with its shadow, coloured by the cloth (it joins the diffuse
#   light, which the albedo multiplies) and c164.z of it white (it joins the specular).
# - the game's highlight and reflection are scaled by 1 - c164.w: its Phong lobe made cotton look plastic.
# - c191.xy fades both out above a G-buffer gloss (c191.x where it starts, c191.y one over its width), for
#   leather and vinyl drawn as clothes; c191.x 1 lets everything through.
# - c191.z 1 shows the masks (ClothDebug): red clothes, green clothes times their gloss, blue skin.
#
# Usage: ped_cloth_sheen.py <FusionShaders checkout with the patches that sort before this one applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'deferred_lighting'
TAG = 'FusionFix cloth sheen'

# Per shader: the line that leaves 1 - |N.V| in a register, a constant holding 1, the line after which the
# G-buffer gloss is in a register, the sun's colour, its shadow, the ambient term (as it is written and as
# it is read), the line the ambient is added to the direct light in, the specular (written and read), and
# the line that writes the colour.
SHADERS = {
    1: dict(rim=('add r1.x, -r1_abs.x, c2.w', 'r1.x'), one='c2.w', gloss=('mov r18.x, r4.y', 'r4.y'),
            sun='r6', shadow='r0.y', amb=('r1.yzw', 'r1', 'r12.xxyz', 'r1.yzww'),
            before='mad r1.xyz, r2, r27, r1.yzww', spec=('r0.xzw', 'r12.xxyz'),
            final=('mad oC0.xyz, r2, r1, r0.xzww', 'mad r2.xyz, r2, r1, r0.xzww')),
    2: dict(rim=('add r1.x, -r1_abs.x, c3.z', 'r1.x'), one='c3.z', gloss=('mov r18.x, r4.y', 'r4.y'),
            sun='r5.yzww', shadow='r0.y', amb=('r2.xyz', 'r2', 'r12', 'r2'),
            before='mad r1.xyw, r1, r27.xyzz, r2.xyzz', spec=('r0.xzw', 'r12.xxyz'),
            final=('mad oC0.xyz, r2, r1.xyww, r0.xzww', 'mad r2.xyz, r2, r1.xyww, r0.xzww')),
    3: dict(rim=('add r0.x, -r0_abs.x, c0.z', 'r0.x'), one='c0.z', gloss=('texld r3, v0, s2', 'r3.y'),
            sun='r2', shadow='r29.x', amb=('r1.yzw', 'r1', 'r12.xxyz', 'r1.yzww'),
            before='mad r1.xyz, r2, r25, r1.yzww', spec=('r0.xyz', 'r12'),
            final=('mad oC0.xyz, r2, r1, r0', 'mad r2.xyz, r2, r1, r0')),
    4: dict(rim=('add r0.x, -r0_abs.x, c3.z', 'r0.x'), one='c3.z', gloss=('texld r3, v0, s2', 'r3.y'),
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
    lines.insert(i + 1, indent(lines[i]) + f'def c166, 16, -5, 0.666666687, 0.333333343 // {TAG}: the clothes test; the sun wrapped half past the terminator')

    branch = find_one(lines, 'mov r26.x, ' + ('r21.x' if number in (1, 3) else 'r4.x' if number == 2 else 'r22.x'))

    i = find_one(lines, s['rim'][0], branch)
    lines.insert(i + 1, indent(lines[i]) + f'mov r30.x, {s["rim"][1]} // {TAG}: 1 - N.V')

    i = find_one(lines, s['gloss'][0], branch)
    lines.insert(i + 1, indent(lines[i]) + f'mov r26.z, {s["gloss"][1]} // {TAG}: the G-buffer gloss')

    i = find_one(lines, SKIN_FRC, branch)
    pad = indent(lines[i])
    lines[i + 1:i + 1] = [pad + f'mad_sat r26.y, r26.x, c166.x, c166.y // {TAG}: 1 on clothes (an eighth) and skin (a quarter), 0 elsewhere',
                          pad + f'mov r30.z, r1.x // {TAG}: N.L']
    i = find_one(lines, SKIN_MASK, i)
    lines.insert(i + 1, pad + f'add r26.y, r26.y, -r26.x // {TAG}: 1 on clothes only')

    amb_dst, amb_src, sheen_amb, amb_read = s['amb']
    spec_dst, sheen_spec = s['spec']
    i = find_one(lines, s['before'], i)
    lines[i:i] = [pad + line for line in [
        f'// --- {TAG} (c164, c191: ClothSheen in postfx.ixx): fibres catch the light around a sleeve\'s rim, and the',
        f'// --- game\'s plastic highlight is toned down; gloss above c191.x (leather, vinyl) keeps the game\'s look.',
        'add r30.w, r26.z, -c191.x',
        'mul_sat r30.w, r30.w, c191.y',
        f'add r30.w, -r30.w, {one}',
        'mul r30.y, r26.y, r30.w // clothes, as far as their gloss lets them',
        'mul r30.w, r30.y, c164.w',
        f'add r30.w, -r30.w, {one}',
        f'mul {spec_dst}, r0, r30.w // the game\'s highlight and reflection',
        'pow r30.x, r30.x, c164.y',
        'mul r30.x, r30.x, r30.y',
        'mul r30.x, r30.x, c164.x // the sheen',
        'mad_sat r30.z, r30.z, c166.z, c166.w',
        f'mul r30.z, r30.z, {s["shadow"]}',
        f'mul r12.xyz, {s["sun"]}, r30.z',
        f'add r12.xyz, r12, {amb_read}',
        'mul r12.xyz, r12, r30.x // the light it catches',
        f'mad {spec_dst}, {sheen_spec}, c164.z, r0 // a little of it white',
        f'add {amb_dst}, {amb_src}, {sheen_amb} // the rest the colour of the cloth',
    ]]

    i = find_one(lines, s['final'][0], i)
    lines[i:i + 1] = [pad + line for line in [
        s['final'][1],
        f'mov r12.x, r26.y // {TAG}: ClothDebug shows clothes red, their gloss green and skin blue',
        'mul r12.y, r26.y, r26.z',
        'mov r12.z, r26.x',
        'lrp oC0.xyz, c191.z, r12, r2',
    ]]

    path.write_text('\n'.join(lines))


def main():
    for number, s in SHADERS.items():
        patch(number, s)


if __name__ == '__main__':
    main()
