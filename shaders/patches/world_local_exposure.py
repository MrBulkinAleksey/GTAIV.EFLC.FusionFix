#!/usr/bin/env python3
# Writes world_local_exposure.patch: local exposure (Local Exposure in Graphics > Lighting, source/localexposure.ixx).
#
# The game exposes the whole frame by one adapted luminance, so at night a lit shop front burns out while the alley
# beside it sinks into black. Every variant of the game's composite post fx pass (rage_postfx PS11-18, 23-26, 28),
# right before it writes the picture, takes the scene's luminance around the pixel from a small copy of it (s12, a
# thirty-second of the screen, made by localexposure.ixx; four bilinear reads 0.75 texels off, a 4 x 4 tent) and
# exposes the pixel by (adapted / local)^k, between c157.y and c157.z: bright surroundings a little darker, dark ones a
# little lighter.
# c157 (set right before the pass) = k (0 = off), the least and the most it scales by, -; c158 = one texel of the copy;
# defs c159, c160 (free in these shaders); r27-r29 free there.
#
# Usage: world_local_exposure.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'rage_postfx'
TAG = 'FusionFix world_local_exposure.patch'
SHADERS = [f'rage_postfxPS{i}.asm' for i in (11, 12, 13, 14, 15, 16, 17, 18, 23, 24, 25, 26, 28)]

DEF = f'''\
    def c159, 0.75, -0.75, 9.99999975e-005, 0.25 // {TAG}: the taps' offset in texels, a tiny luminance, a quarter
    def c160, 0.212599993, 0.715200007, 0.0722000003, 0 // {TAG}: luminance
'''


def block(adapt_read):
    m = re.match(r'texld r[0-9]+, (c[0-9]+\.[xyzw]), (s[0-9]+)', adapt_read)
    coord, adapt = m.group(1), m.group(2)
    return f'''\
    // --- {TAG}: the pixel (r0.xyz) exposed by the adapted luminance over the luminance around it
    mov r27.x, c157.x
    if_ne r27.x, c159.w
      texldl r27, {coord}, {adapt} // the adapted luminance (a constant coordinate, level 0)
      mov r27.zw, c158.xxxy // a texel of the copy
      mul r28.xy, r27.zwzw, c159.x
      add r29.xy, v0, r28
      mov r29.zw, c160.w
      texldl r29, r29, s12
      dp3 r28.z, r29, c160
      mul r28.xy, r27.zwzw, c159.xyzw
      add r29.xy, v0, r28
      mov r29.zw, c160.w
      texldl r29, r29, s12
      dp3 r28.w, r29, c160
      add r28.z, r28.z, r28.w
      mul r28.xy, r27.zwzw, c159.yxzw
      add r29.xy, v0, r28
      mov r29.zw, c160.w
      texldl r29, r29, s12
      dp3 r28.w, r29, c160
      add r28.z, r28.z, r28.w
      mul r28.xy, r27.zwzw, c159.y
      add r29.xy, v0, r28
      mov r29.zw, c160.w
      texldl r29, r29, s12
      dp3 r28.w, r29, c160
      add r28.z, r28.z, r28.w
      mul r28.z, r28.z, c159.w // the luminance around
      max r28.z, r28.z, c159.z
      rcp r28.z, r28.z
      mul r28.z, r28.z, r27.x
      max r28.z, r28.z, c159.z
      log r28.z, r28.z
      mul r28.z, r28.z, c157.x
      exp r28.z, r28.z
      max r28.z, r28.z, c157.y
      min r28.z, r28.z, c157.z
      mul r0.xyz, r0, r28.z
    endif
'''


def patch(path):
    text = path.read_text()
    lines = text.splitlines(keepends=True)
    if TAG in text:
        return
    reads = [l.strip() for l in lines if re.match(r'\s*texld r[0-9]+, c[0-9]+\.[xyzw], s[0-9]+', l)]
    if len(reads) != 1:
        sys.exit(f'{path.name}: expected one read of the adapted luminance')
    last_def = max(i for i, l in enumerate(lines) if l.strip().startswith('def c'))
    last_dcl = max(i for i, l in enumerate(lines) if l.strip().startswith('dcl_2d s'))
    out, done = [], False
    for i, l in enumerate(lines):
        if not done and re.match(r'\s*(mul|max|mov) oC0\.xyz, r0\b', l):
            out.append(block(reads[0]))
            done = True
        out.append(l)
        if i == last_def:
            out.append(DEF)
        if i == last_dcl:
            out.append(f'    dcl_2d s12 // {TAG}: the scene, a thirty-second of the screen\n')
    if not done:
        sys.exit(f'{path.name}: no output')
    path.write_text(''.join(out))


def main():
    for name in SHADERS:
        patch(ROOT / name)


if __name__ == '__main__':
    main()
