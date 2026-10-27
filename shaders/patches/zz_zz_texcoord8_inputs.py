#!/usr/bin/env python3
# Writes zz_zz_texcoord8_inputs.patch: upstream's motion vectors (aa_motion_vectors.patch) read the previous clip position
# from TEXCOORD8 in the first input register the stock pixel shader left free; where one of our patches declared that
# register for an input of its own (world_terrain_and_parallax: the world position in TEXCOORD6, texture_quality,
# vehicle_paint), TEXCOORD8 moves to the first register free after all of them. The motion block reads it in its first
# two lines only (rcp of .w, the mul by it).
#
# Usage: zz_zz_texcoord8_inputs.py <FusionShaders checkout with all the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff). Named to sort after the patches that
# declare inputs (Windows lists zz_zz_ before zz_z_).
import re
import sys
from pathlib import Path

TAG = 'FusionFix zz_zz_texcoord8_inputs.patch'
MARK = '    // Motion vectors next to the stencil'


def patch(path):
    text = path.read_text()
    if TAG in text or MARK not in text:
        return False
    decls = re.findall(r'^\s*dcl_\w+\s+v(\d+)', text, re.M)
    m = re.search(r'^    dcl_texcoord8 v(\d+)$', text, re.M)
    if not m or decls.count(m.group(1)) < 2:
        return False
    old = m.group(1)
    used = {int(d) for d in decls}
    free = next(i for i in range(10) if i not in used)
    head, motion = text.split(MARK, 1)
    head = head[:m.start()] + f'    dcl_texcoord8 v{free} // {TAG}: v{old} is ours' + head[m.end():]
    reads = re.subn(rf'\bv{old}\b', f'v{free}', motion)
    assert reads[1] == 2, (path, reads[1])
    path.write_text(head + MARK + reads[0])
    return True


if __name__ == '__main__':
    root = Path(sys.argv[1]) / 'win32_30_nv8'
    changed = [p for p in sorted(root.rglob('*PS*.asm')) if patch(p)]
    print(len(changed), 'shaders')
