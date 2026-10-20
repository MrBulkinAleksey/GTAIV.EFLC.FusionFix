#!/usr/bin/env python3
# Writes world_lens_dirt.patch: dirt on the lens lit up by bright lights, and horizontal streaks from them (Lens Dirt
# in Graphics > Lighting, source/lensdirt.ixx).
#
# Every variant of the game's composite post fx pass (rage_postfx PS11-18, 23-26, 28) adds, right before it writes the
# picture, the bloom (BloomSampler) at the pixel times a dirt texture (s11: smudges and specks, made by lensdirt.ixx)
# and a horizontal streak of the bloom (six reads either side, falling off), tinted cool, both at the frame's exposure
# (the shader's own: its exposure constant over the adapted luminance, read as it reads them).
# c180 (set right before the pass) = dirt strength, streak strength, the streak's step in texture units, -; defs
# c182-c184 (free in these shaders); r27-r29 free there.
#
# Usage: world_lens_dirt.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'rage_postfx'
TAG = 'FusionFix world_lens_dirt.patch'
SHADERS = [f'rage_postfxPS{i}.asm' for i in (11, 12, 13, 14, 15, 16, 17, 18, 23, 24, 25, 26, 28)]

DEF = f'''\
    def c182, 1, 2, 3, 0 // {TAG}: the streak's taps
    def c183, 0.5, 0.300000012, 0.200000003, 0 // {TAG}: their weights
    def c184, 0.600000024, 0.800000012, 1.20000005, 0 // {TAG}: the streak's tint
'''


def block(bloom, adapt_read, exposure_const):
    # adapt_read: 'texld rX, cN.c, sM' -> coordinate and sampler
    m = re.match(r'texld r[0-9]+, (c[0-9]+\.[xyzw]), (s[0-9]+)', adapt_read)
    coord, adapt = m.group(1), m.group(2)
    return f'''\
    // --- {TAG}: the lens's dirt lit by the bloom, and a horizontal streak of it, at the frame's exposure (r0.xyz)
    texld r27, {coord}, {adapt}
    rcp r27.x, r27.x
    mul r27.x, r27.x, {exposure_const}.y // the exposure
    texld r28, v0, {bloom}
    texld r29, v0, s11
    mul r28.xyz, r28, r29
    mul r28.xyz, r28, c180.x // the dirt
    mov r27.yzw, c182.w
    mov r29.zw, c182.w
    mov r29.x, c182.x
    mul r29.x, r29.x, c180.z
    add r29.y, v0.y, c182.w
    add r29.x, v0.x, r29.x
    texldl r29, r29, {bloom}
    mad r27.yzw, r29.xxyz, c183.x, r27
    mov r29.x, -c182.x
    mul r29.x, r29.x, c180.z
    add r29.x, v0.x, r29.x
    mov r29.y, v0.y
    mov r29.zw, c182.w
    texldl r29, r29, {bloom}
    mad r27.yzw, r29.xxyz, c183.x, r27
    mov r29.x, c182.y
    mul r29.x, r29.x, c180.z
    add r29.x, v0.x, r29.x
    mov r29.y, v0.y
    mov r29.zw, c182.w
    texldl r29, r29, {bloom}
    mad r27.yzw, r29.xxyz, c183.y, r27
    mov r29.x, -c182.y
    mul r29.x, r29.x, c180.z
    add r29.x, v0.x, r29.x
    mov r29.y, v0.y
    mov r29.zw, c182.w
    texldl r29, r29, {bloom}
    mad r27.yzw, r29.xxyz, c183.y, r27
    mov r29.x, c182.z
    mul r29.x, r29.x, c180.z
    add r29.x, v0.x, r29.x
    mov r29.y, v0.y
    mov r29.zw, c182.w
    texldl r29, r29, {bloom}
    mad r27.yzw, r29.xxyz, c183.z, r27
    mov r29.x, -c182.z
    mul r29.x, r29.x, c180.z
    add r29.x, v0.x, r29.x
    mov r29.y, v0.y
    mov r29.zw, c182.w
    texldl r29, r29, {bloom}
    mad r27.yzw, r29.xxyz, c183.z, r27
    mul r27.yzw, r27, c184.xxyz
    mul r27.yzw, r27, c180.y // the streak
    add r28.xyz, r28, r27.yzww
    mad r0.xyz, r28, r27.x, r0
'''


def patch(path):
    text = path.read_text()
    if TAG in text:
        return
    lines = text.splitlines(keepends=True)
    bloom = re.search(r'^//\s+BloomSampler\s+(s[0-9]+)', text, re.M).group(1)
    reads = [l.strip() for l in lines if re.match(r'\s*texld r[0-9]+, c[0-9]+\.[xyzw], s[0-9]+', l)]
    if len(reads) != 1:
        sys.exit(f'{path.name}: expected one read of the adapted luminance')
    exposure = re.search(r'rcp r0\.w, r[0-9]+\.x\s*\n\s*mul r0\.w, r0\.w, (c[0-9]+)\.y', text)
    if not exposure:
        sys.exit(f'{path.name}: no exposure')
    out = []
    last_def = max(i for i, l in enumerate(lines) if l.strip().startswith('def c'))
    last_dcl = max(i for i, l in enumerate(lines) if l.strip().startswith('dcl_2d s'))
    done = False
    for i, l in enumerate(lines):
        if not done and re.match(r'\s*(mul|max|mov) oC0\.xyz, r0\b', l):
            out.append(block(bloom, reads[0], exposure.group(1)))
            done = True
        out.append(l)
        if i == last_def:
            out.append(DEF)
        if i == last_dcl:
            out.append(f'    dcl_2d s11 // {TAG}: the lens dirt\n')
    if not done:
        sys.exit(f'{path.name}: no output')
    path.write_text(''.join(out))


def main():
    for name in SHADERS:
        patch(ROOT / name)


if __name__ == '__main__':
    main()
