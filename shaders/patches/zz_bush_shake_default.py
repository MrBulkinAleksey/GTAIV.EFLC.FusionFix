#!/usr/bin/env python3
# Writes zz_bush_shake_default.patch: Bush Shake for the bushes drawn with gta_default (gta_cutout.sps and the like, as
# the park bushes are), the same push and shake tree_bush_shake.patch gives gta_trees. The deferred vertex shaders of
# gta_default (VS4 deferred_draw, VS5 deferredalphaclip_draw) move r1, the position world_grass_wind.patch hands the
# rest of the shader, after the grass's block; source/grasswind.ixx sets b13 around the G-buffer draws of the models
# whose textures name a plant, with a body in reach only. Not the grass's code: that one lays the blades down to half
# their height under a body, which would sit a bush down.
# Registers as tree_bush_shake.patch has them (b13, c241, c248-c251), its defs in c12-c14: gta_default's c245-c247
# are the grass's, c252-c255 the previous view of zz_zz_tree_grass_motion.patch.
#
# Usage: zz_bush_shake_default.py <FusionShaders checkout with all the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff). Named to sort last, after the grass.
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from tree_bush_shake import defs, shake

TAG = 'FusionFix zz_bush_shake_default.patch'
AFTER = '    // --- FusionFix world_grass_wind.patch end'
SHADERS = ['gta_defaultVS4.asm', 'gta_defaultVS5.asm']
REGISTERS = dict(tag=TAG, e='c14', s='c13', t='c12')


def patch(path):
    text = path.read_text()
    if TAG in text:
        return
    head, body = text.split('vs_3_0\n', 1)
    # After the grass's block only r0 and r1 hold anything; c12-c14 and b13 are not read
    assert body.count(AFTER + '\n') == 1, path
    assert not re.search(r'\bc1[2-4]\b|\bb13\b', body), path
    lines = body.split('\n')
    first_def = next(i for i, line in enumerate(lines) if line.strip().startswith('def '))
    out = []
    for i, line in enumerate(lines):
        if i == first_def:
            out.append(defs(**REGISTERS).rstrip('\n'))
        out.append(line)
        if line == AFTER:
            out.append(shake('r1', **REGISTERS).rstrip('\n'))
    path.write_text(head + 'vs_3_0\n' + '\n'.join(out))


if __name__ == '__main__':
    root = Path(sys.argv[1]) / 'win32_30_nv8' / 'gta_default'
    for name in SHADERS:
        patch(root / name)
