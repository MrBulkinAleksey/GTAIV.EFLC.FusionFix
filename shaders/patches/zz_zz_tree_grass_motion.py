#!/usr/bin/env python3
# Writes zz_zz_tree_grass_motion.patch: motion vectors of what sways in the vertex shaders, written into the G-buffer.
# The camera's motion comes from the depth and moving entities are drawn again with their previous matrices and bones
# (temporal.ixx), but the trees' and bushes' sway in the wind and the grass's wind (world_grass_wind.patch) move the
# vertices themselves: TAA, DLSS, FSR and frame generation smeared the leaves. Their vertex shaders work out the
# previous position too, with the last frame's wind and view, the pixel shaders write the motion next to the material ID.
# Trees and grass stand still, so a frame's constants are enough (no draw is told apart).
#
# - gta_trees, gta_trees_extended VS0 (deferred_draw, deferredalphaclip_draw): the game's sway again with the last
#   frame's wind (c244.x for c233.x; the block is upstream's motion vector code), into the world (c0-c3) and through
#   the last frame's view and projection without jitter (c252-c255, rows) into TEXCOORD8. The bushes' shake
#   (tree_bush_shake.patch) is left out of the previous position.
# - gta_default VS4/VS5 (deferred_draw, deferredalphaclip_draw): world_grass_wind.patch's sway again with the last
#   frame's wind (c238, a copy of c240's), the rest as it stands; VS6 (skinned) has no previous position, TEXCOORD8.w
#   -1 says so.
# - The pixel shaders of those (gta_trees, gta_trees_extended PS1, gta_default PS3/PS4): the material ID goes to a
#   temporary, the motion (current minus previous position in texture coordinates, jitter excluded: c200 = 1 / size,
#   (0.5 - jitter) / size, c201 = 0.5, -0.5, 0.5) into its y and z, w -1 for "the G-buffer's own motion" where TEXCOORD8.w is
#   above 0, then all of it to COLOR3 (_STENCIL_BUFFER_, A16B16G16R16F: the game reads x only). PS_CameraMotion takes
#   it there.
# Registers: VS c238, c244, c252-c255 and PS c200, c201 set by temporal.ixx (and grasswind.ixx) each frame; nothing else
# reads them in these shaders (asserted).
#
# Usage: zz_zz_tree_grass_motion.py <FusionShaders checkout with all the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff). Windows lists it after zz_zz_shape_.
import re
import sys
from pathlib import Path

TAG = 'FusionFix zz_zz_tree_grass_motion.patch'
HERE = Path(__file__).parent

# The game's sway with the last frame's wind (c244.x), the swayed model position left in `out`
TREE_SWAY = {
    'gta_trees/gta_treesVS0.asm': ('r0', '''\
    mul r0.xyz, c4.zwzw, v0.zxyw
    mad r0.xyz, v0.yzxw, c4.wzzw, -r0
    add r0.xyz, r0, c5.x
    nrm r1.xyz, r0
    dp3 r0.x, v0, v0
    rsq r0.x, r0.x
    rcp r0.x, r0.x
    mad r0.y, r0.x, c4.x, c4.y
    mul r0.x, r0.x, c244.x
    mad r0.x, r0.x, c5.z, c5.y
    frc r0.x, r0.x
    mad r0.x, r0.x, c6.x, c6.y
    sincos r2.xy, r0.x
    mul r0.xzw, r1.xyyz, r0.y
    mul r1.xyz, r0.wxzw, v0.yzxw
    mad r1.xyz, r0.zwxw, v0.zxyw, -r1
    mad r0.xzw, r0, r2.y, v0.xyyz
    add r1.xyz, r1, c5.x
    nrm r3.xyz, r1
    mul r1.xyz, r0.y, r3
    mul r1.xyz, r1, c5.y
    mad r0.xyz, r1, r2.x, r0.xzww
'''),
    'gta_trees_extended/gta_trees_extendedVS0.asm': ('r3', '''\
    mul r0.xyz, c5.zwzw, v0.zxyw
    mad r0.xyz, v0.yzxw, c5.wzzw, -r0
    add r0.xyz, r0, c6.x
    nrm r1.xyz, r0
    dp3 r0.x, v0, v0
    rsq r0.x, r0.x
    rcp r0.x, r0.x
    mad r0.y, r0.x, c5.x, c5.y
    mul r0.x, r0.x, c244.x
    mad r0.x, r0.x, c6.z, c6.y
    frc r0.x, r0.x
    mad r0.x, r0.x, c7.x, c7.y
    sincos r2.xy, r0.x
    mul r0.xzw, r1.xyyz, r0.y
    mul r1.xyz, r0.wxzw, v0.yzxw
    mad r1.xyz, r0.zwxw, v0.zxyw, -r1
    mad r0.xzw, r0, r2.y, v0.xyyz
    add r1.xyz, r1, c6.x
    nrm r3.xyz, r1
    mul r1.xyz, r0.y, r3
    mul r1.xyz, r1, c6.y
    mad r3.xyz, r1, r2.x, r0.xzww
'''),
}

# The game's own sway reads c233.x where the block above reads c244.x: the same code, checked
TREE_SWAY_GAME = {'gta_trees/gta_treesVS0.asm': '    mul r0.x, r0.x, c233.x', 'gta_trees_extended/gta_trees_extendedVS0.asm': '    mul r0.x, r0.x, c233.x'}

GRASS_VS = ['gta_default/gta_defaultVS4.asm', 'gta_default/gta_defaultVS5.asm']
SKINNED_VS = ['gta_default/gta_defaultVS6.asm']
PIXEL_SHADERS = ['gta_trees/gta_treesPS1.asm', 'gta_trees_extended/gta_trees_extendedPS1.asm',
                 'gta_default/gta_defaultPS3.asm', 'gta_default/gta_defaultPS4.asm']
VS_RESERVED = r'\bc238\b|\bc244\b|\bc25[2-5]\b'
PS_RESERVED = r'\bc20[0-2]\b'


def previous_clip(position, temp):
    """`position` (a model position in xyz) into the world, then into the last frame's clip space, to o11"""
    return f'''\
    mul {temp}.xyz, c1, {position}.y
    mad {temp}.xyz, {position}.x, c0, {temp}
    mad {temp}.xyz, {position}.z, c2, {temp}
    add {temp}.xyz, {temp}, c3
    mul {position}, {temp}.y, c253
    mad {position}, {temp}.x, c252, {position}
    mad {position}, {temp}.z, c254, {position}
    add o11, {position}, c255
'''


def grass_sway():
    """world_grass_wind.patch's sway, read from its generator, with the last frame's wind"""
    text = (HERE / 'world_grass_wind.py').read_text()
    source = text[:text.index('\ndef patch(')]
    scope = {'__name__': 'world_grass_wind_text'}
    exec(compile(source.replace('ROOT = Path(sys.argv[1])', 'ROOT = Path(".")'), 'world_grass_wind.py', 'exec'), scope)
    sway = scope['SWAY'].replace(scope['TAG'], TAG + ' (the last frame)')
    assert sway.count('c240') > 0
    return re.sub(r'\bc240\b', 'c238', sway)


def split(path):
    text = path.read_text()
    assert TAG not in text, f'{path} is patched already'
    head, body = text.split('_3_0\n', 1)
    return head + '_3_0\n', body


def add_dcl(body, line):
    lines = body.split('\n')
    last = max(i for i, l in enumerate(lines) if l.strip().startswith('dcl'))
    lines.insert(last + 1, line)
    return '\n'.join(lines)


def free_register(body, prefix, start, limit):
    used = {int(n) for n in re.findall(rf'\b{prefix}(\d+)\b', body)}
    return next(i for i in range(start, limit) if i not in used)


def patch_vs(root, name, block):
    path = root / name
    head, body = split(path)
    assert not re.search(VS_RESERVED, body), (path, 'reads a register of this patch')
    assert not re.search(r'\bo11\b', body), path
    body = add_dcl(body, f'    dcl_texcoord8 o11 // {TAG}: the last frame\'s clip position')
    body = body.rstrip('\n') + f'\n\n    // --- {TAG}: the position of the last frame, in its clip space\n' + block + f'    // --- {TAG} end\n'
    path.write_text(head + body)


def patch_skinned(root, name):
    path = root / name
    head, body = split(path)
    assert not re.search(r'\bo11\b', body), path
    reg = free_register(body, 'c', 12, 64)
    body = add_dcl(body, f'    dcl_texcoord8 o11 // {TAG}: no previous position (w -1)')
    lines = body.split('\n')
    first_def = next((i for i, l in enumerate(lines) if l.strip().startswith('def ')), None)
    defline = f'    def c{reg}, 0, 0, 0, -1 // {TAG}'
    if first_def is None:
        first_dcl = next(i for i, l in enumerate(lines) if l.strip().startswith('dcl'))
        lines.insert(first_dcl, defline)
    else:
        lines.insert(first_def, defline)
    body = '\n'.join(lines).rstrip('\n') + f'\n\n    mov o11, c{reg} // {TAG}: skinned, no previous position\n'
    path.write_text(head + body)


def patch_ps(root, name):
    path = root / name
    head, body = split(path)
    assert not re.search(PS_RESERVED, body), (path, 'reads a register of this patch')
    lines = body.split('\n')
    writes = [i for i, l in enumerate(lines) if re.match(r'\s*\w+(_sat)?\s+oC3\b', l.split('//')[0])]
    assert len(writes) == 1, (path, writes)
    at = writes[0]
    after = '\n'.join(l.split('//')[0] for l in lines[at + 1:])
    later = {int(n) for n in re.findall(r'\br(\d+)\b', after)}
    free = [r for r in range(31, -1, -1) if r not in later]
    assert len(free) >= 3, (path, later)
    rid, ra, rb = (f'r{n}' for n in free[:3])
    lines[at] = re.sub(r'\boC3\b', rid, lines[at], count=1) + f' // {TAG}: the material ID, to COLOR3 at the end'
    declared = {int(n) for n in re.findall(r'^\s*dcl\w*\s+v(\d+)', body, re.M)}
    vin = next(i for i in range(10) if i not in declared)
    body = '\n'.join(lines)
    body = add_dcl(body, f'    dcl_texcoord8 v{vin} // {TAG}: the last frame\'s clip position')
    if not re.search(r'^\s*dcl vPos', body, re.M):
        body = add_dcl(body, '    dcl vPos.xy')
    lines = body.split('\n')
    first_def = next((i for i, l in enumerate(lines) if l.strip().startswith('def ')), None)
    defline = f'    def c202, -1, 0, 0, 0 // {TAG}: w of COLOR3 where the motion is the G-buffer\'s own'
    lines.insert(first_def if first_def is not None else next(i for i, l in enumerate(lines) if l.strip().startswith('dcl')), defline)
    body = '\n'.join(lines).rstrip('\n') + f'''

    // --- {TAG}: current minus previous position in texture coordinates, next to the material ID
    rcp {ra}.x, v{vin}.w
    mul {ra}.xy, v{vin}, {ra}.x
    mad {ra}.xy, {ra}, c201.xy, c201.z
    mad {rb}.xy, vPos, c200.xy, c200.zw
    add {rid}.yz, {rb}.xxy, -{ra}.xxy
    cmp {rid}.w, -v{vin}.w, {rid}.w, c202.x
    mov oC3, {rid}
'''
    path.write_text(head + body)


if __name__ == '__main__':
    root = Path(sys.argv[1]) / 'win32_30_nv8'
    for name, (position, sway) in TREE_SWAY.items():
        game = (root / name).read_text()
        assert TREE_SWAY_GAME[name] in game, name
        assert sway.replace('c244.x', 'c233.x').split('\n')[0] in game, name
        patch_vs(root, name, sway + previous_clip(position, 'r1' if position == 'r0' else 'r2'))
    sway = grass_sway()
    for name in GRASS_VS:
        patch_vs(root, name, sway + previous_clip('r1', 'r2'))
    for name in SKINNED_VS:
        patch_skinned(root, name)
    for name in PIXEL_SHADERS:
        patch_ps(root, name)
