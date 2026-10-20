#!/usr/bin/env python3
# Writes world_rain_light_shafts.patch: rain falling through the light shafts of street lamps and headlights (Rain
# Light Scattering in Graphics > Lighting, source/rainlight.ixx).
#
# The engine's light shafts (deferred_lighting, technique lightShafts, PS18-23) give the light scattered along the view
# ray through a lamp's cone as one value. While it rains the shaft is broken into falling streaks: columns of the
# screen a few pixels wide, each with its own hash, carry tapered streaks running down at their own speed; the shaft
# is brighter on a streak and dimmer between, the same on average. rainlight.ixx also makes the shafts brighter in
# the rain (the light's own intensity).
# c185 (set as the lighting begins) = strength (0 = off, the rain's), seconds, 1 / the column width in pixels,
# streaks per pixel down the screen. Defs c180-c182 (free in these shaders); r21, r22 free.
#
# Usage: world_rain_light_shafts.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'deferred_lighting'
TAG = 'FusionFix world_rain_light_shafts.patch'
SHADERS = [f'deferred_lightingPS{i}.asm' for i in range(18, 24)]

DEF = f'''\
    def c180, 0.103100002, 33.3300018, 0.600000024, 0.699999988 // {TAG}: the column hash, the speed's spread and least
    def c181, 0.150000006, 6.66666651, 6, -0.449999988 // {TAG}: a streak's length (of its cycle) and 1 / it, the gain on a streak and the shift between
    def c182, 0, 1, 0, 0 // {TAG}: zero, one
'''

BLOCK = f'''\
    // --- {TAG}: rain streaks falling through the shaft (r21.x the shaft's multiplier)
    mov r21.x, c182.x
    mov r21.w, c185.x
    if_lt r21.x, r21.w
      mul r21.xy, vPos.xy, c185.zw
      frc r21.z, r21.x
      add r21.x, r21.x, -r21.z // the column
      mul r21.z, r21.x, c180.x
      frc r21.z, r21.z
      add r22.x, r21.z, c180.y
      mul r21.z, r21.z, r22.x
      add r22.x, r21.z, r21.z
      mul r21.z, r21.z, r22.x
      frc r21.z, r21.z // its hash
      mad r22.y, r21.z, c180.z, c180.w
      mul r22.y, r22.y, c185.y
      add r22.y, r21.y, -r22.y
      mad r22.y, r21.z, c181.z, r22.y
      frc r22.y, r22.y // where in its cycle, falling
      add r22.y, -r22.y, c181.x
      mul_sat r22.y, r22.y, c181.y // on a streak, brightest at its head
      mad r22.y, r22.y, c181.z, c181.w
      mad r21.x, r22.y, r21.w, c182.y
      max r21.x, r21.x, c182.x
    else
      mov r21.x, c182.y
    endif
'''


def patch(path):
    text = path.read_text()
    if TAG in text:
        return
    lines = text.splitlines(keepends=True)
    last_def = max(i for i, l in enumerate(lines) if l.strip().startswith('def c'))
    out = []
    done_def = done_out = False
    for i, l in enumerate(lines):
        code = l.split('//')[0]
        if i == last_def:
            out.append(l)
            out.append(DEF)
            done_def = True
            continue
        if not done_out and re.search(r'\boC0\.xyz\b', code):
            out.append(BLOCK)
            out.append(l.replace('oC0.xyz', 'r22.xyz', 1))
            out.append(f'    mul oC0.xyz, r22, r21.x // {TAG}\n')
            done_out = True
            continue
        out.append(l)
    if not (done_def and done_out):
        sys.exit(f'{path.name}: anchors not found')
    path.write_text(''.join(out))


def main():
    for name in SHADERS:
        patch(ROOT / name)


if __name__ == '__main__':
    main()
