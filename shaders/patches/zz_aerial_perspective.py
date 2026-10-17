#!/usr/bin/env python3
# Writes zz_aerial_perspective.patch: the air between the camera and what it sees (aerial perspective) in every shader
# with FusionFix's volumetric fog (the block from `mov r21.xyz, c212` to its last `lrp ..., r20.x, r21|r22, ...`):
# the fog pass (rage_postfx PS20) for the opaque world and the forward shaders (glass, peds, cars, decals, alpha) for
# what is drawn after it. Named zz_ so it applies after every other patch: it is generated on top of them.
#
# Under the Physical Sky (pbrsky.ixx binds its SkyView table on s14 for the frame after the lighting, which reads s14
# itself, and sets c104-c106):
#  - the fog's colour is the PBR sky's this way instead of the timecycle's gradient rebuilt from c212-c216, by
#    c106.w (the sky's share), so the fog melts into the sky drawn above it. Not the sky's own colour though: that is
#    the light of an endless path, at a low sun the reddened sunlight's yellow, while the few kilometres of air up to
#    a building glow as the sky times 1 - T over them (blue-grey, as the in-scatter below). Its brightness is the
#    sky's, the hue that of the path to the point, at any strength of the aerial perspective;
#  - Aerial Perspective: through d metres of air at the camera's height the light keeps T = exp(-k sigma(h) d) of
#    itself, per channel (Rayleigh's blue far more than Mie's grey haze), and gains the sky's own colour this way
#    times 1 - T: in air the same all along the ray that is exact, and the city's heights hardly change the air.
#    The scene, after the fog, becomes scene T + sky (1 - T). By c105.w (0 off).
#
# c104: xyz k sigma Rayleigh per metre times log2(e), w the same for Mie's extinction with the weather's haze
# c105: x, y -log2(e) / scale height in metres (Rayleigh, Mie), z the table's units on screen (HDRSunExposure.x times
#       HDRExposure.x, as gta_atmoscatt_clouds draws it), w the aerial perspective's strength
# c106: xy the sun seen from above (world x, y, unit), z 0, w the sky's share of the fog's colour; 0: skipped
# No pixel shader reads c104-c109 (game or FusionFix), and s14 only deferred_lighting's lamp shadow cache.
#
# The SkyView mapping, as sky_pbr.patch reads it: u the angle between the view and the sun seen from above over pi,
# v 0.5 - 0.5 sqrt(elevation / (pi / 2)), the view held 4 degrees above the horizon or higher. The horizon's own row is
# the light of an endless path, the sunlight's yellow-white at any hour; the fog and the air of a sea seen from a
# helicopter took it, kilometres of air leave little of the path's own hue, and the sea's far end came out an olive
# band under the blue sky just above it. 4 degrees up, the sky matches what is seen over that band.
#
# Usage: zz_aerial_perspective.py <FusionShaders checkout with every other patch applied>; the patch is the diff it
# leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8'
TAG = 'FusionFix zz_aerial_perspective.patch'
SKY_MASKED = {'rage_postfxPS20.asm': 'r1.x'}  # the fog pass: 0 on the sky, which has its air already

DEFS = f'''\
    def c107, 1.57072878, -0.212114394, 0.0742610022, -0.0187292993 // {TAG}: acos cubic
    def c108, 1, -0.636619747, 0.5, 0 // {TAG}: one, -2 / pi, a half, zero
    def c109, 0.318309873, 9.99999997e-007, 3.14159274, 0.0697564737 // {TAG}: 1 / pi, tiny, pi, sin 4 degrees
'''


def lookup(a, b, c, f, mask):
    masked = f'        mul {c}.yz, {c}, {mask} // none on the sky\n' if mask else ''
    return f'''\
      // --- {TAG}: {a}.xyz the light that gets through the air, {b}.xyz the PBR sky this way, {f}.xyz the fog's
      // colour: the sky's brightness in the hue of the air up to the point
      mov {a}.xyz, c108.x
      mov {b}.xyz, c108.w
      mov {f}.xyz, c108.w
      mov {c}.x, c106.w
      if_lt c108.w, {c}.x
        mul {c}.x, r20.z, r20.w
        mul {c}.x, {c}.x, c108.z
        add {c}.x, {c}.x, c15.z
        max {c}.x, {c}.x, c108.w // the height halfway, above the sea
        mul {c}.yz, {c}.x, c105.xxyy
        exp {c}.y, {c}.y
        exp {c}.z, {c}.z
        mul {c}.yz, {c}, r20.w
        mul {f}.xyz, {c}.y, c104
        mad {f}.xyz, {c}.z, c104.w, {f}
        exp {f}.x, -{f}.x
        exp {f}.y, -{f}.y
        exp {f}.z, -{f}.z
        add {f}.xyz, -{f}, c108.x // 1 - T up to the point, whatever the aerial perspective's strength
{masked}        mul {c}.yz, {c}, c105.w
        mul {a}.xyz, {c}.y, c104
        mad {a}.xyz, {c}.z, c104.w, {a}
        exp {a}.x, -{a}.x
        exp {a}.y, -{a}.y
        exp {a}.z, -{a}.z
        max {c}.w, r20.z, c109.w // 4 degrees up at least: the sky seen just above the horizon
        mad {c}.x, {c}.w, c107.w, c107.z
        mad {c}.x, {c}.x, {c}.w, c107.y
        mad {c}.x, {c}.x, {c}.w, c107.x
        add {c}.w, -{c}.w, c108.x
        rsq {c}.w, {c}.w
        rcp {c}.w, {c}.w
        mul {c}.x, {c}.x, {c}.w // acos of the elevation sine
        mad {c}.x, {c}.x, c108.y, c108.x // the elevation's share of a right angle
        rsq {c}.x, {c}.x
        rcp {c}.x, {c}.x
        mad {b}.y, {c}.x, -c108.z, c108.z // v
        dp2add {c}.x, r20, r20, c109.y
        rsq {c}.x, {c}.x
        mul {c}.zw, r20.xxxy, {c}.x // the view seen from above
        dp2add {c}.x, {c}.zwzw, c106, c106.z // cos of the angle to the sun seen from above
        mad {c}.y, {c}_abs.x, c107.w, c107.z
        mad {c}.y, {c}.y, {c}_abs.x, c107.y
        mad {c}.y, {c}.y, {c}_abs.x, c107.x
        add {c}.z, -{c}_abs.x, c108.x
        rsq {c}.z, {c}.z
        rcp {c}.z, {c}.z
        mul {c}.y, {c}.y, {c}.z
        add {c}.z, -{c}.y, c109.z
        cmp {c}.y, {c}.x, {c}.y, {c}.z // the angle
        mul {b}.x, {c}.y, c109.x // u
        mov {b}.zw, c108.w
        texldl {b}, {b}, s14
        mul {b}.xyz, {b}, c105.z // as the sky is drawn
        mul {f}.xyz, {f}, {b}
        dp3 {c}.x, {b}, c108.x
        dp3 {c}.y, {f}, c108.x
        max {c}.y, {c}.y, c109.y
        rcp {c}.y, {c}.y
        mul {c}.x, {c}.x, {c}.y
        mul {f}.xyz, {f}, {c}.x // the sky's channel sum in the path's hue
      endif
'''


def swizzle_into(mask):
    # The source swizzle that hands x, y and z in turn to the components of a write mask: xzw takes xxyz.
    letters = 'xyzw'
    out = ['x'] * 4
    for i, ch in enumerate(mask):
        out[letters.index(ch)] = letters[i]
    return ''.join(out)


def free_registers(text, count):
    # Temporaries not read from the fog block on (none there either): dead there, whatever the shader did with them
    # before. Patched shaders such as the car glass take all 32 somewhere.
    used = {int(n) for n in re.findall(r'\br(\d+)\b', text)}
    free = [n for n in range(31, -1, -1) if n not in used]
    if len(free) < count:
        return None
    return [f'r{n}' for n in free[:count]]


def patch(path):
    text = path.read_text().replace('\r\n', '\n')
    if TAG in text:
        sys.exit(f'{path} is patched already')
    if re.search(r'\bs14\b|\bc10[4-9]\b', text):
        sys.exit(f'{path} already uses s14 or c104-c109')
    start = re.search(r'^([ \t]*)mov r21\.xyz, c212\b.*\n', text, re.M)
    regs = free_registers(text[start.start():], 4)
    if not regs:
        sys.exit(f'{path}: no four free temporaries')
    a, b, c, f = regs

    end_of_block = text.index('endif', start.end())
    block = text[start.start():end_of_block]

    # The fog's colour: the PBR sky's by the sky's share
    clamp = re.search(r'^[ \t]*min r21\.xyz, r21, c141\.w.*\n', block, re.M)
    if not clamp:
        sys.exit(f'{path}: no clamp of the rebuilt sky')
    colour = (f'      lrp {c}.xyz, c106.w, {f}, r21 // {TAG}: the PBR air\'s colour by the sky\'s share\n'
              f'      mov r21.xyz, {c}\n')
    block = block[:clamp.end()] + colour + block[clamp.end():]

    # The last blend with the fog: into a temporary, then through the air
    finals = list(re.finditer(r'^([ \t]*)lrp (\S+?)\.([xyzw]+), (r20\.x, r2[12], \S+)(.*)\n', block, re.M))
    if not finals:
        sys.exit(f'{path}: no final fog blend')
    m = finals[-1]
    dst, mask = m.group(2), m.group(3)
    if len(mask) != 3:
        sys.exit(f'{path}: unexpected mask in {m.group(0).strip()}')
    into = swizzle_into(mask)
    air = (f'{m.group(1)}lrp {c}.xyz, {m.group(4)}{m.group(5)}\n'
           f'{m.group(1)}mad {b}.xyz, {b}, -{a}, {b} // {TAG}: the sky\'s light scattered in on the way\n'
           f'{m.group(1)}mad {dst}.{mask}, {c}.{into}, {a}.{into}, {b}.{into}\n')
    block = block[:m.start()] + air + block[m.end():]

    text = text[:start.start()] + lookup(a, b, c, f, SKY_MASKED.get(path.name)) + block + text[end_of_block:]

    # The sampler after the shader's own, the constants after its first def (or before its first dcl)
    last_dcl = list(re.finditer(r'^([ \t]*)dcl_2d s\d+.*\n', text, re.M))[-1]
    text = text[:last_dcl.end()] + f'{last_dcl.group(1)}dcl_2d s14 // {TAG}: the PBR sky\'s SkyView table\n' + text[last_dcl.end():]
    first_def = re.search(r'^[ \t]*def c\d+.*\n', text, re.M)
    if first_def:
        text = text[:first_def.end()] + DEFS + text[first_def.end():]
    else:
        first = re.search(r'^[ \t]*dcl_.*\n', text, re.M)
        text = text[:first.start()] + DEFS + text[first.start():]
    path.write_text(text)
    return path


files = sorted(p for p in ROOT.glob('*/*PS*.asm') if 'mov r21.xyz, c212' in p.read_text())
for p in files:
    patch(p)
print(f'{len(files)} shaders')
