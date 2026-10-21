#!/usr/bin/env python3
# Writes world_heat_haze.patch: the far scene wavers in the hot air of a clear summer day (Heat Haze in
# Graphics > Lighting, source/heathaze.ixx).
#
# The game's composite post fx pass (rage_postfx, GTACompositePostFx: PS11-18, 23-26 and 28, one per mix of depth of
# field, motion blur and the rest) reads everything at the pixel's own texture coordinate v0. Each of them first works
# out a wavering coordinate and reads everything there instead (the scene, its depth, the blur, the bloom, the
# stencil), so the whole picture bends together like light through the air:
#  - two sine waves running up the screen, the first warped by a third across it, offset the pixel by up to c188.xy
#    (texture units, a pixel or two);
#  - by the depth there (GBufferTextureSampler3, s1): nothing up to c189 start, full past start + range; the sky
#    (beyond c189.z) takes c189.w of it.
# A read inside a dynamic branch takes the coordinate through texldl at level 0 (the targets have one level), as a
# texld may not take a computed coordinate there.
# c188 (set by heathaze.ixx right before the pass) = amplitude x, y, seconds, strength (0 = off); c189 = -start / range,
# 1 / range, the sky's depth, the sky's share. Defs c190-c193 (free in these shaders).
#
# Usage: world_heat_haze.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'rage_postfx'
TAG = 'FusionFix world_heat_haze.patch'
SHADERS = [f'rage_postfxPS{i}.asm' for i in (11, 12, 13, 14, 15, 16, 17, 18, 23, 24, 25, 26, 28)]

DEF = f'''\
    def c190, 180, 23, 140, 31 // {TAG}: the waves across the screen: up, the warp across, the second up, its slant
    def c191, 3.0999999, 1.29999995, 2.29999995, 1.29999995 // {TAG}: their speeds (radians a second) and the warp's size
    def c192, 0.159154937, 0.5, 6.28318548, -3.14159274 // {TAG}: angles into -pi..pi for sincos
    def c193, 1, 0, 0, 0 // {TAG}: one, zero
'''

BLOCK = f'''\
    // --- {TAG}: the coordinate everything below is read at (r30), wavering with the hot air in the distance
    texld r31, v0, s1
    pow r29.x, c209_abs.z, r31.x
    mul r31.x, r29.x, c209.w // the depth
    mad_sat r31.y, r31.x, c189.y, c189.x
    add r31.z, r31.x, -c189.z
    mov r31.w, c189.w
    cmp r31.z, r31.z, r31.w, c193.x
    mul r31.y, r31.y, r31.z
    mul r31.y, r31.y, c188.w // how much it wavers here
    mov r28.w, c188.z // seconds
    mul r28.x, v0.x, c190.y
    mad r28.x, r28.w, c191.y, r28.x
    mad r28.x, r28.x, c192.x, c192.y
    frc r28.x, r28.x
    mad r28.x, r28.x, c192.z, c192.w
    sincos r29.xy, r28.x // the warp
    mul r28.y, v0.y, c190.x
    mad r28.y, r28.w, c191.x, r28.y
    mad r28.y, r29.y, c191.w, r28.y
    mul r28.z, v0.y, c190.z
    mad r28.z, v0.x, c190.w, r28.z
    mad r28.z, r28.w, c191.z, r28.z
    mad r28.yz, r28, c192.x, c192.y
    frc r28.yz, r28
    mad r28.yz, r28, c192.z, c192.w
    sincos r29.xy, r28.y
    mov r28.x, r29.y
    sincos r29.xy, r28.z
    mov r28.y, r29.y
    mul r28.xy, r28, c188
    mov r30.zw, c193.y
    mad r30.xy, r28, r31.y, v0
'''

INSTR = re.compile(r'^(\s*)([a-z_0-9]+)(\s.*)?$')


def patch(path):
    lines = path.read_text().splitlines(keepends=True)
    if any(TAG in l for l in lines):
        return
    out = []
    first_instr = None
    for i, l in enumerate(lines):
        code = l.split('//')[0].strip()
        if not code or code.startswith(('ps_3_0', 'def', 'dcl')):
            continue
        first_instr = i
        break
    # the defs go after the last def, before the dcls
    last_def = max(i for i, l in enumerate(lines[:first_instr]) if l.strip().startswith('def'))
    stack = []
    for i, l in enumerate(lines):
        if i < first_instr:
            out.append(l)
            if i == last_def:
                out.append(DEF)
            continue
        if i == first_instr:
            out.append(BLOCK)
        code, sep, comment = l.partition('//')
        m = INSTR.match(code.rstrip('\n'))
        op = m.group(2) if m else ''
        if op.startswith('if') and not (op == 'if' and code.split()[1].startswith('b')):
            stack.append('dyn')
        elif op in ('loop', 'rep'):
            stack.append('loop')
        elif op in ('endif', 'endloop', 'endrep') and stack:
            stack.pop()
        if re.search(r'\bv0\b', code):
            new = re.sub(r'\bv0\b', 'r30', code)
            if op == 'texld' and stack:
                # texld r#, v0, s# inside a branch: texldl at level 0 (r30.zw are zero)
                parts = [p.strip() for p in new.split(None, 1)[1].split(',')]
                if parts[1] != 'r30':
                    sys.exit(f'{path.name}:{i + 1}: texld with a swizzled v0 inside a branch')
                indent = re.match(r'\s*', code).group(0)
                new = f'{indent}texldl {parts[0]}, r30, {parts[2]}'
                new += '\n' if not sep else ' '
            l = new + (sep + comment if sep else '')
        out.append(l)
    path.write_text(''.join(out))


def main():
    for name in SHADERS:
        patch(ROOT / name)


if __name__ == '__main__':
    main()
