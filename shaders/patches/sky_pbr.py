#!/usr/bin/env python3
# Writes sky_pbr.patch: the sky dome (gta_atmoscatt_clouds PS1, 5, 7, 8 and 9: the scene's sky with and without the
# moon and stars, the reflections' paraboloids and the water's) takes its colour from the PBR sky's SkyView table
# (source/resources/SkyAtmosphere.fx, bound on s10 by pbrsky.ixx around the sky's draws) in place of the timecycle's
# gradient the vertex shader passes in v5. Its sun, moon, stars, clouds and fog stay the game's.
#
# c194.x is how much of the table's colour the sky takes: pbrsky.ixx sets it around the draws it binds the table for
# and puts it back to 0 after them, so a sky drawn any other way keeps the game's colour. No game shader has a
# parameter on c194. The game's colour is clamped to 1 (mul_sat); the table's is not, so the bright sky about the sun
# keeps its hue.
#
# The table's mapping, as SkyViewDir in SkyAtmosphere.fx: u the angle between the view and the sun seen from above,
# over pi; v 0.5 at the horizon, 0 at the zenith, 1 at the nadir, as the square root of the elevation's share of a
# right angle. acos by the usual cubic (|error| < 7e-5 rad).
#
# Usage: sky_pbr.py <FusionShaders checkout with the patches before this one applied>; the patch is the diff it leaves
# (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'gta_atmoscatt_clouds'
TAG = 'FusionFix sky_pbr.patch'
VARIANTS = (1, 5, 7, 8, 9)

DEFS = f'''\
    def c151, 1.57072878, -0.212114394, 0.0742610022, -0.0187292993 // {TAG}: acos cubic
    def c152, 1, -0.636619747, 0.5, -0.5 // {TAG}: one, -2 / pi, a half
    def c153, 3.14159274, 0.318309873, 9.99999997e-007, 0 // {TAG}: pi, 1 / pi, tiny, zero
'''

LOOKUP = f'''\
    // --- {TAG}: the PBR sky's colour this way (r26.xyz), from its SkyView table on s10
    nrm r24.xyz, v1
    mad r25.y, r24_abs.y, c151.w, c151.z
    mad r25.y, r25.y, r24_abs.y, c151.y
    mad r25.y, r25.y, r24_abs.y, c151.x
    add r25.z, -r24_abs.y, c152.x
    rsq r25.z, r25.z
    rcp r25.z, r25.z
    mul r25.y, r25.y, r25.z // acos |elevation sine|
    mad r25.y, r25.y, c152.y, c152.x // the elevation's share of a right angle
    rsq r25.y, r25.y
    rcp r25.y, r25.y
    cmp r25.w, r24.y, c152.w, c152.z
    mad r26.y, r25.w, r25.y, c152.z // v
    dp2add r25.x, r24.xzzw, r24.xzzw, c153.z
    rsq r25.x, r25.x
    mul r25.zw, r24.xxxz, r25.x // the view seen from above
    mov r24.xz, c65
    dp2add r25.x, r24.xzzw, r24.xzzw, c153.z
    rsq r25.x, r25.x
    mul r24.xz, r24, r25.x // the sun seen from above
    dp2add r25.x, r25.zwzw, r24.xzzw, c153.w // cos of the angle between them
    mad r25.y, r25_abs.x, c151.w, c151.z
    mad r25.y, r25.y, r25_abs.x, c151.y
    mad r25.y, r25.y, r25_abs.x, c151.x
    add r25.z, -r25_abs.x, c152.x
    rsq r25.z, r25.z
    rcp r25.z, r25.z
    mul r25.y, r25.y, r25.z
    add r25.z, -r25.y, c153.x
    cmp r25.y, r25.x, r25.y, r25.z // the angle
    mul r26.x, r25.y, c153.y // u
    texld r26, r26, s10
'''


def swizzle_into(mask):
    # The source swizzle that hands x, y and z in turn to the components of a write mask: xzw takes xxyz.
    letters = 'xyzw'
    out = ['x'] * 4
    for i, c in enumerate(mask):
        out[letters.index(c)] = letters[i]
    return ''.join(out)


def patch(n):
    path = ROOT / f'gta_atmoscatt_cloudsPS{n}.asm'
    text = path.read_text().replace('\r\n', '\n')
    if TAG in text:
        sys.exit(f'{path} is patched already')
    if re.search(r'\br2[456]\b|\bs10\b|\bc15[123]\b|\bc194\b', text):
        sys.exit(f'{path} already uses r24-r26, s10, c151-c153 or c194')
    sky = list(re.finditer(r'^([ \t]*)mul_sat_pp (r\d+)\.([xyzw]+), (r\d+\.[xyzw]), v5(?:\.([xyzw]+))?\n', text, re.M))
    if len(sky) != 1:
        sys.exit(f'{path}: {len(sky)} lines take the sky colour from v5')
    m = sky[0]
    dst, mask, factor = m.group(2), m.group(3), m.group(4)
    source = m.group(5) or 'xyzw'
    if len(mask) != 3 or any(source['xyzw'.index(c)] != 'xyz'[i] for i, c in enumerate(mask)):
        sys.exit(f'{path}: unexpected swizzle in {m.group(0).strip()}')
    into = swizzle_into(mask)
    own = ''.join(mask) + mask[-1] * (4 - len(mask))  # the destination's components read as xyz(w)
    # Times HDRSunExposure alone, not the factor, which is (1 - the sun's glow) times it: the game's sky gives way to
    # its sun disc there, and with the PBR sky's glow about the sun brighter than that disc, the disc came out a dull
    # hole in it. The sun goes over the PBR sky instead.
    exposure = re.search(r'^//\s+HDRSunExposure\s+(c\d+)', text, re.M)
    if not exposure:
        sys.exit(f'{path}: no HDRSunExposure register')
    blend = (f'    mul r25.xyz, {exposure.group(1)}.x, r26 // {TAG}: the PBR sky, unclamped, at HDRSunExposure: the sun goes over it\n'
             f'    add r25.xyz, r25, -{dst}.{own}\n'
             f'    mad {dst}.{mask}, c194.x, r25.{into}, {dst}\n')
    text = text[:m.start()] + LOOKUP + m.group(0) + blend + text[m.end():]
    # The sampler after the shader's own, the constants after its first def.
    last_dcl = list(re.finditer(r'^([ \t]*)dcl_2d s\d+\n', text, re.M))[-1]
    text = text[:last_dcl.end()] + f'{last_dcl.group(1)}dcl_2d s10 // {TAG}: the PBR sky\'s SkyView table\n' + text[last_dcl.end():]
    first_def = re.search(r'^[ \t]*def c\d+.*\n', text, re.M)
    text = text[:first_def.end()] + DEFS + text[first_def.end():]
    path.write_text(text)


for n in VARIANTS:
    patch(n)
