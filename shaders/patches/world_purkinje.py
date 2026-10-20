#!/usr/bin/env python3
# Writes world_purkinje.patch: the eye's night vision (Night Vision Shift in Graphics > Lighting, source/purkinje.ixx).
#
# In the dark the eye sees with its rods: colours fade and what is left leans to blue (the Purkinje shift). Every
# variant of the game's composite post fx pass (rage_postfx PS11-18, 23-26, 28), right before it writes the picture,
# moves the dark pixels towards their own luminance in a cool tint, the more the darker they are (none above
# 1 / c181.y), by c181.x (the night's share, from the clock, set by purkinje.ixx). Bright lights keep their colour.
# c181 (set right before the pass) = strength (0 = off), 1 / the luminance where it ends, -, -; defs c185, c186
# (free in these shaders); r29-r31 free there.
#
# Usage: world_purkinje.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'rage_postfx'
TAG = 'FusionFix world_purkinje.patch'
SHADERS = [f'rage_postfxPS{i}.asm' for i in (11, 12, 13, 14, 15, 16, 17, 18, 23, 24, 25, 26, 28)]

DEF = f'''\
    def c185, 0.212599993, 0.715200007, 0.0722000003, 1 // {TAG}: luminance, one
    def c186, 0.720000029, 0.949999988, 1.39999998, 0 // {TAG}: the rods' cool tint
'''

BLOCK = f'''\
    // --- {TAG}: dark pixels (r0.xyz) towards their luminance in a cool tint, as the rods see
    dp3 r30.w, r0, c185
    mul_sat r29.w, r30.w, c181.y
    add r29.w, -r29.w, c185.w
    mul r29.w, r29.w, r29.w
    mul r29.w, r29.w, c181.x
    mul r30.xyz, r30.w, c186
    lrp r31.xyz, r29.w, r30, r0
    mov r0.xyz, r31
'''


def patch(path):
    lines = path.read_text().splitlines(keepends=True)
    if any(TAG in l for l in lines):
        return
    last_def = max(i for i, l in enumerate(lines) if l.strip().startswith('def c'))
    out, done = [], False
    for i, l in enumerate(lines):
        if not done and re.match(r'\s*(mul|max|mov) oC0\.xyz, r0\b', l):
            out.append(BLOCK)
            done = True
        out.append(l)
        if i == last_def:
            out.append(DEF)
    if not done:
        sys.exit(f'{path.name}: no output')
    path.write_text(''.join(out))


def main():
    for name in SHADERS:
        patch(ROOT / name)


if __name__ == '__main__':
    main()
