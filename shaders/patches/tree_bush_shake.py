#!/usr/bin/env python3
# Writes tree_bush_shake.patch: the bushes (gta_trees, gta_trees_extended) a person or a car goes through part away from
# it and shake while it moves. The deferred vertex shader (VS0: deferred_draw, deferredalphaclip_draw) moves each
# vertex after the game's own sway, before the projection (o0); source/grasswind.ixx sets b13 around the G-buffer draws
# of short models with a body in reach only; with b13 off the shader is the stock one.
#
# The offset is made in the world and taken back into the model's space through gWorld's rows (c0-c2, scaled
# rotations: each axis over its length squared), so it holds however the bush is turned:
#  - bodies (up to two, c248-c251, as world_grass_wind.patch has them: a car's footprint, a person's small square) push
#    the leaves away from them, up to c247.y at the top, falling off to nothing `reach` metres past the box;
#  - while a body moves the leaves shake, c247.z at the top by its speed, times the press: two waves with phases from
#    the vertex's world position and height, so neighbouring branches move apart; c247.w the time's phase (cycles);
#  - weighted by height / model height, so the base stays put, and the top drops by d^2 / 2h so the branches bend
#    rather than stretch.
# The world position (o4 / o6) stays as the game makes it, from the model's rest pose, as with the game's sway.
# Registers: b13; c247 (per draw) = 1 / height, the push and the shake at the top in metres, the shake's phase;
# c248/c249 and c250/c251 (per draw) = two bodies: centre.xy and unit forward.xy on the ground, half length, half width,
# 1 / reach, strength (0 = none); defs c12-c14.
#
# Usage: tree_bush_shake.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import re
import sys
from pathlib import Path

TAG = 'FusionFix tree_bush_shake.patch'
# The shader and the register its swayed model-space position is in, after the line that makes it
SHADERS = [
    ('gta_trees/gta_treesVS0.asm', 'r0', '    mad r0.xyz, r1, r2.x, r0.xzww'),
    ('gta_trees_extended/gta_trees_extendedVS0.asm', 'r3', '    mad r3.xyz, r1, r2.x, r0.xzww'),
]

def defs(tag=TAG, e='c14', s='c13', t='c12'):
    return f'''\
    def {e}, 9.99999975e-006, 1, 0, 0.300000012 // {tag}: epsilon, 1, 0, a body's rounding (m)
    def {s}, 0.899999976, 1.29999995, 0.699999988, 0.25 // {tag}: the shake's cycles a metre along x, y and up, the second wave's offset (cycles)
    def {t}, 6.28318548, -3.14159274, -0.5, 0 // {tag}: 2pi, -pi, -1 / 2
'''

BODY = '''\
      add r10.xy, r4, -c{a}
      mul r11.x, r10.x, c{a}.z
      mad r11.x, r10.y, c{a}.w, r11.x
      mul r11.y, r10.x, c{a}.w
      mad r11.y, r10.y, -c{a}.z, r11.y
      abs r10.xy, r11
      add r10.xy, r10, -c{b}
      max r10.xy, r10, {e}.z
      mul r10.z, r10.x, r10.x
      mad r10.z, r10.y, r10.y, r10.z
      add r10.z, r10.z, {e}.x
      rsq r10.z, r10.z
      rcp r10.z, r10.z
      mul r10.w, r10.z, c{b}.z
      add_sat r10.w, -r10.w, {e}.y
      mul r10.w, r10.w, r10.w
      mul r10.w, r10.w, c{b}.w
      mov r12.xy, c{b}
      add r12.xy, r12, {e}.w
      rcp r12.x, r12.x
      rcp r12.y, r12.y
      mul r12.xy, r11, r12
      mul r12.z, r12.x, r12.x
      mad r12.z, r12.y, r12.y, r12.z
      add r12.z, r12.z, {e}.x
      rsq r12.z, r12.z
      min r12.z, r12.z, {e}.y
      mul r12.xy, r12, r12.z
      mul r13.xy, r12.x, c{a}.zwzw
      mad r13.x, r12.y, c{a}.w, r13.x
      mad r13.y, -r12.y, c{a}.z, r13.y
      mad r9.xy, r13, r10.w, r9
      max r9.w, r9.w, r10.w
'''


def shake(position, tag=TAG, e='c14', s='c13', t='c12'):
    return f'''\
    // --- {tag}: b13 = a bush a body is in. Moves {position}.xyz, the swayed position in the model's space.
    if b13
      mul r4.xyz, c1, v0.y
      mad r4.xyz, v0.x, c0, r4
      mad r4.xyz, v0.z, c2, r4
      add r4.xyz, r4, c3
      // bodies: r9.xy = the push's direction times its strength, r9.w = the strongest
      mov r9, {e}.z
{BODY.format(a=248, b=249, e=e)}{BODY.format(a=250, b=251, e=e)}\
      mul r10.z, r9.x, r9.x
      mad r10.z, r9.y, r9.y, r10.z
      add r10.z, r10.z, {e}.x
      rsq r10.z, r10.z
      mul r10.z, r10.z, r9.w
      min r10.z, r10.z, {e}.y
      mul r9.xy, r9, r10.z
      mul r6.xy, r9, c247.y
      // the shake: two waves, the second twice as quick
      mul r7.x, r4.x, {s}.x
      mad r7.x, r4.y, {s}.y, r7.x
      mad r7.x, v0.z, {s}.z, r7.x
      add r7.x, r7.x, c247.w
      add r7.y, r7.x, r7.x
      add r7.y, r7.y, {s}.w
      frc r7.xy, r7
      mad r7.xy, r7, {t}.x, {t}.y
      sincos r8.xy, r7.x
      sincos r11.xy, r7.y
      mov r8.x, r8.y
      mov r8.y, r11.y
      mul r8.xy, r8, c247.z
      mad r6.xy, r8, r9.w, r6
      // by the height, the top dropping as it bends
      mul_sat r10.w, v0.z, c247.x
      mul r6.xy, r6, r10.w
      mul r6.z, r6.x, r6.x
      mad r6.z, r6.y, r6.y, r6.z
      mul r6.z, r6.z, c247.x
      mul r6.z, r6.z, {t}.z
      dp3 r7.x, c0, c0
      dp3 r7.y, c1, c1
      dp3 r7.z, c2, c2
      rcp r7.x, r7.x
      rcp r7.y, r7.y
      rcp r7.z, r7.z
      dp3 r8.x, r6, c0
      dp3 r8.y, r6, c1
      dp3 r8.z, r6, c2
      mad {position}.xyz, r8, r7, {position}
    endif
    // --- {tag} end
'''


def patch(path, position, after):
    text = path.read_text()
    if TAG in text:
        return
    head, body = text.split('vs_3_0\n', 1)
    # Upstream's motion vectors (aa_motion_vectors.patch) sway the previous position again after this line: only the
    # current one is moved
    body, marker, motion = body.partition('\n    // Motion vectors\n')
    # r4-r13 must be free
    used = {int(n) for n in re.findall(r'\br(\d+)', body)}
    assert max(used) <= 3, (path, used)
    assert body.count(after + '\n') == 1, path
    # Its registers are read by nothing else (the motion vectors take c240-c246)
    assert not re.search(r'\bc1[2-4]\b|\bc247\b|\bb13\b', body), path
    lines = body.split('\n')
    first_def = next(i for i, line in enumerate(lines) if line.strip().startswith('def '))
    out = []
    for i, line in enumerate(lines):
        if i == first_def:
            out.append(defs().rstrip('\n'))
        out.append(line)
        if line == after:
            out.append(shake(position).rstrip('\n'))
    path.write_text(head + 'vs_3_0\n' + '\n'.join(out) + marker + motion)


if __name__ == '__main__':
    ROOT = Path(sys.argv[1]) / 'win32_30_nv8'
    for name, position, after in SHADERS:
        patch(ROOT / name, position, after)
