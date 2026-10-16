#!/usr/bin/env python3
# Writes ped_cloth_sheen_lights.patch: the sheen of people's clothes under lamps and headlights, deferred_lighting
# PS6-9, PS14, PS16 and PS17, as ped_cloth_sheen.patch gives it them in the sun.
#
# Clothes are an eighth of a step on the material ID (MarkPedSkin in postfx.ixx), which the skin lighting of these
# shaders (local_light_on_skin.patch) already reads from s11: saturate(16 f - 5) less the skin's.
#
# The light's sheen is the sun's lobe, Charlie at roughness 0.5 with Neubelt's visibility,
# pi D V N.L = (1 - N.H^2) N.L / (2 (N.L + N.V - N.L N.V)), times the light's colour and fall-off, c164.x
# (ClothSheenStrength) and the clothes' mask, in the square root of the albedo c164.z of the way to white. It joins
# the colour just before the shader's own last factors (the vertex AO, the light volume test), so the lamp's shadow
# map, contact shadows and box shadows, which scale r26 after it, shade it too. The game's highlight on clothes stays
# as it is under lamps.
#
# Registers: r22.w (PS17: r28.x, its shadow taps take r22) carries the clothes' mask, r22.w then the lobe; r22.xyz the lobe's scratch, then its light; r23 the
# lobe's scratch and the sheen's colour. Each shader's view ray is r12.xyz (local_light_specular_ggx.patch) and the
# skin lighting names the light and normal in its unclamped N.L.
#
# Usage: ped_cloth_sheen_lights.py <FusionShaders checkout with the patches that sort before this one applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'deferred_lighting'
TAG = 'FusionFix cloth sheen'

# Per shader: the albedo register and the line that writes r26 from the colour (None: right after the colour is in
# r26), with the register holding the colour there.
SHADERS = {
    6: dict(albedo='r4', last='mul r26, r1.x, r0', colour='r0'),
    7: dict(albedo='r2', last='mul r26, r1, r0.x', colour='r1'),
    8: dict(albedo='r4', last='mul r26, r1.y, r0', colour='r0'),
    9: dict(albedo='r2', last='mul r26, r1, r0.x', colour='r1'),
    14: dict(albedo='r4', last='mul r26, r1, r0.x', colour='r1'),
    16: dict(albedo='r3', last='mul r26, r0, r1.x', colour='r0'),
    17: dict(albedo='r3', last=None, colour='r26', mask='r28.x'),
}

CONTACT = '// --- FusionFix local contact shadows: a short ray from this pixel towards the light'


def code(line):
    return line.split('//')[0].strip()


def find_one(lines, text, start=0, end=None):
    end = len(lines) if end is None else end
    hits = [i for i in range(start, end) if code(lines[i]) == text]
    if len(hits) != 1:
        sys.exit(f'{text!r}: {len(hits)} matches in lines {start}-{end}')
    return hits[0]


def find_contact(lines, start):
    hits = [i for i in range(start, len(lines)) if lines[i].strip() == CONTACT]
    if len(hits) != 1:
        sys.exit(f'contact shadows: {len(hits)} matches after line {start}')
    return hits[0]


def find_re(lines, pattern, start=0):
    hits = [i for i in range(start, len(lines)) if re.fullmatch(pattern, code(lines[i]))]
    if len(hits) != 1:
        sys.exit(f'{pattern!r}: {len(hits)} matches after line {start}')
    return hits[0]


def writes(line, reg, comps):
    # Whether the line writes any of comps of reg (a subroutine call counts as writing whatever it touches).
    c = code(line)
    m = re.match(r'(\w+)\s+(r\d+)(?:\.([xyzw]+))?\s*,', c)
    if not m or m.group(1).startswith(('def', 'dcl', 'if', 'break')):
        return False
    return m.group(2) == reg and bool(set(m.group(3) or 'xyzw') & set(comps))


def check_untouched(lines, start, end, reg, comps, number, what):
    for i in range(start, end):
        if writes(lines[i], reg, comps):
            sys.exit(f'PS{number}: {reg}.{comps} ({what}) is written at line {i + 1}: {lines[i].strip()}')
    for i in range(start, end):
        m = re.match(r'call (l\d+)', code(lines[i]))
        if m:
            body = lines[find_one(lines, f'label {m.group(1)}'):]
            body = body[:next(k for k, l in enumerate(body) if code(l) == 'ret')]
            if any(writes(l, reg, comps) for l in body):
                sys.exit(f'PS{number}: {reg}.{comps} ({what}) is written in {m.group(1)}')


def indent(line):
    return line[:len(line) - len(line.lstrip())]


def patch(number, s):
    path = ROOT / f'deferred_lightingPS{number}.asm'
    lines = path.read_text().split('\n')
    if any('c166' in code(l) for l in lines):
        sys.exit(f'PS{number}: c166 is taken')
    if not any(code(l).startswith('def c171, 1,') for l in lines):
        sys.exit(f'PS{number}: no c171.x of 1 (local_light_specular_ggx.patch)')

    i = find_one(lines, 'def c156, 255, 0.25, 16, -6')
    lines.insert(i + 1, indent(lines[i]) + f'def c166, 16, -5, 0.0001, 0.5 // {TAG}: the clothes test; a floor and a half for the sheen lobe')

    # The clothes' mask, beside the skin's.
    i = find_re(lines, r'frc (r20\.[xw]), \1')
    skin = code(lines[i]).split()[1].rstrip(',')
    pad = indent(lines[i])
    mask = s.get('mask', 'r22.w')
    lines.insert(i + 1, pad + f'mad_sat {mask}, {skin}, c166.x, c166.y // {TAG}: 1 on clothes (an eighth) and skin (a quarter), 0 elsewhere')
    i = find_one(lines, f'mad_sat {skin}, {skin}, c156.z, c156.w', i)
    lines.insert(i + 1, pad + f'add {mask}, {mask}, -{skin} // {TAG}: 1 on clothes only')
    mask_start = i + 2

    # The lobe, where the skin lighting has the light's direction and the normal.
    i = find_re(lines, r'dp3 r21\.w, r\d+, r\d+', i)
    light_dir, normal = [t.strip() for t in code(lines[i]).split(',')[1:]]
    i = find_one(lines, 'add_sat r21.xyz, r21, r22', i)
    check_untouched(lines, mask_start, i, *mask.split('.'), number, 'the clothes\' mask')
    view = max(k for k in range(i) if code(lines[k]).startswith('mov r12.xyz,'))
    check_untouched(lines, view + 1, i, 'r12', 'xyz', number, 'the view ray')
    lines[i + 1:i + 1] = [pad + line for line in [
        f'// --- {TAG} (c164: ClothSheen in postfx.ixx): fibres catch the light around a sleeve\'s rim, the Charlie lobe',
        f'add r22.xyz, {light_dir}, -r12 // towards the light plus towards the camera',
        'nrm r23.xyz, r22',
        f'dp3 r22.x, {normal}, r23',
        'mad r22.x, -r22.x, r22.x, c171.x // 1 - N.H^2',
        f'dp3_sat r22.y, {light_dir}, {normal} // N.L',
        f'dp3 r22.z, {normal}, r12 // -N.V',
        'mad r23.w, -r22.y, r22_abs.z, r22_abs.z',
        'add r23.w, r23.w, r22.y',
        'max r23.w, r23.w, c166.z',
        'rcp r23.w, r23.w',
        'mul r23.w, r23.w, r22.y',
        'mul r23.w, r23.w, r22.x',
        'mul r23.w, r23.w, c166.w // (1 - N.H^2) N.L / (2 (N.L + N.V - N.L N.V))',
        f'mul r22.w, {mask}, r23.w',
        'mul r22.w, r22.w, c164.x // on clothes only, ClothSheenStrength times',
    ]]
    lobe_end = i + 1 + 16

    # Its light: the lamp's colour and fall-off, before N.L joins them.
    hits = [k for k in range(lobe_end, len(lines)) if 'the wrapped N.L per channel' in lines[k]]
    if len(hits) != 1:
        sys.exit(f'PS{number}: {len(hits)} wrapped N.L lines')
    i = hits[0]
    ops = [t.strip() for t in code(lines[i]).split(',')[1:]]
    light = next(o for o in ops if not o.startswith('r21')).split('.')[0]
    check_untouched(lines, lobe_end, i, 'r22', 'w', number, 'the lobe')
    lines.insert(i, pad + f'mul r22.xyz, {light}, r22.w // {TAG}: the light the fibres catch')
    light_at = i + 1

    # Its colour, and into the shader's colour before its last factors.
    if s['last'] is None:
        i = find_contact(lines, light_at)
    else:
        i = find_one(lines, s['last'], light_at)
        if find_contact(lines, i) != i + 1:
            sys.exit(f'PS{number}: {s["last"]!r} is not right before the contact shadows')
    check_untouched(lines, light_at, i, 'r22', 'xyz', number, 'the sheen\'s light')
    albedo = max(k for k in range(i) if re.fullmatch(rf'texld {s["albedo"]}, r\d+(\.\w+)?, s[01]', code(lines[k])))
    check_untouched(lines, albedo + 1, i, s['albedo'], 'xyz', number, 'the albedo')
    a, c = s['albedo'], s['colour']
    lines[i:i] = [pad + line for line in [
        f'rsq r23.x, {a}.x // {TAG}: the sheen\'s colour, the square root of the albedo, c164.z of the way to white',
        f'rsq r23.y, {a}.y',
        f'rsq r23.z, {a}.z',
        'rcp r23.x, r23.x',
        'rcp r23.y, r23.y',
        'rcp r23.z, r23.z',
        'mad r23.xyz, -r23, c164.z, r23',
        'add r23.xyz, r23, c164.z',
        f'mad {c}.xyz, r22, r23, {c} // the sheen',
    ]]

    path.write_text('\n'.join(lines))


def main():
    for number, s in SHADERS.items():
        patch(number, s)


if __name__ == '__main__':
    main()
