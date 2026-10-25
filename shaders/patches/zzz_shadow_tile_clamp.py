#!/usr/bin/env python3
# Writes zzz_shadow_tile_clamp.patch: the local lights' shadow taps stay inside their map's tile of the atlas (s14).
# Each tap's coordinate inside the tile is saturated to 0-1 and then placed in the atlas (mad .., c77.w, c77); the game
# moves the tiles 0.1 texel out (CE 0x9268B0, [0x119D02C] / resolution, resolution at map record + 4) and the filter
# reaches half a texel further, so taps at a tile's far edge read the next light's map. From Liberty's Shadows 2.1.0
# (OnyxOak, GPL-3.0, HeadlightAtlas388, there for its headlights only): here every tap of the three shaders that read
# the atlas (PS14, PS15, PS17) is held 1/256 inside the tile, over half a texel past the offset at the smallest maps,
# four texels at the edge of the paraboloid at 1024.
#
# Usage: zzz_shadow_tile_clamp.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git add -A && git diff --cached).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'deferred_lighting'
TAG = 'FusionFix: zzz_shadow_tile_clamp.patch'
REGISTER = 'c198'
PLACE = re.compile(r'^(\s*)mad (r\d+)(\.[xyzw]+)?, (r\d+)(\.[xyzw]+)?, c77\.w, c77')


def used(dst_mask, swizzle):
    # The source components the placed coordinate reads, in the order of the destination's mask
    mask = dst_mask[1:] if dst_mask else 'xyzw'
    swz = swizzle[1:] if swizzle else 'xyzw'
    swz = (swz + swz[-1] * 4)[:4]
    picked = {swz['xyzw'.index(c)] for c in mask}
    return ''.join(c for c in 'xyzw' if c in picked)


for number in (14, 15, 17):
    path = ROOT / f'deferred_lightingPS{number}.asm'
    text = path.read_bytes().decode()
    newline = '\r\n' if '\r\n' in text else '\n'
    lines, out = text.split(newline), []
    assert REGISTER not in text, path
    for line in lines:
        m = PLACE.match(line)
        if m:
            indent, _, dst_mask, src, swizzle = m.groups()
            comps = used(dst_mask, swizzle)
            target = src if comps == 'xyzw' else f'{src}.{comps}'
            out.append(f'{indent}max {target}, {src}, {REGISTER}.x // {TAG}')
            out.append(f'{indent}min {target}, {src}, {REGISTER}.y // {TAG}')
        out.append(line)
        if line.strip().startswith('def c88,'):
            out.append(f'    def {REGISTER}, 0.00390625, 0.99609375, 0, 0 // {TAG}: 1/256 inside the tile either way')
    assert sum(TAG in l for l in out) == 1 + 2 * 12, path
    path.write_bytes(newline.join(out).encode())
