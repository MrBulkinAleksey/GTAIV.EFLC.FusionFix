#!/usr/bin/env python3
# Writes world_hair_strands.patch: hair takes its highlight in strands (Hair Strands in Graphics > Lighting,
# source/hairstrands.ixx).
#
# Hair is drawn into the G-buffer (gta_hair_sorted_alpha PS5, gta_hair_sorted_alpha_exp PS6, the deferred techniques)
# and lit with everything else, so its highlight is one smooth blob. Here the normal it writes leans along the strand
# by an amount of its own for each strand, waves across the strands (two sines across the texture), a little towards
# the root on the whole as Kajiya-Kay's shift has it: the sun's and the lamps' highlights break up into streaks along
# the hair. The main shader's gloss is raised with it (x 1.6 at full strength), the highlight sharper.
# c186 (set as the G-buffer pass begins) = strength (0 = off), how far each strand leans, the shift towards the root,
# the strands' direction in the texture (0: along v, so they vary across u; 1: along u). Defs c5-c7 (free there).
#
# Usage: world_hair_strands.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8'
TAG = 'FusionFix world_hair_strands.patch'

DEF = f'''\
    def c5, 37, 89, 0.370000005, 0 // {TAG}: the waves across the strands per texture unit, the second's phase
    def c6, 6.28318548, -3.14159274, 0.600000024, 0.400000006 // {TAG}: cycles into -pi..pi, the two waves' shares
    def c7, 1, 0.600000024, 0, 0 // {TAG}: one, the gloss raised at full strength
'''

STRANDS = f'''\
    // --- {TAG}: the normal (r1, r1.w its 1 / length) leans along the strand, by a wave across the strands
    mul r1.xyz, r1, r1.w
    mov r5.y, v0.y
    lrp r5.x, c186.w, r5.y, v0.x // across the strands
    mul r5.xy, r5.x, c5
    add r5.y, r5.y, c5.z
    frc r5.xy, r5
    mad r5.xy, r5, c6.x, c6.y
    sincos r6.xy, r5.x
    mov r5.z, r6.y
    sincos r6.xy, r5.y
    mul r5.z, r5.z, c6.z
    mad r5.z, r6.y, c6.w, r5.z // -1..1, each strand its own
    mad r5.z, r5.z, c186.y, c186.z
    mul r5.z, r5.z, c186.x
    mov r6.xyz, v3
    lrp r7.xyz, c186.w, v2, r6
    nrm r6.xyz, r7 // along the strand
    mad r1.xyz, r6, r5.z, r1
    dp3 r1.w, r1, r1
    rsq r1.w, r1.w
'''

GLOSS = f'''\
    mov r5.w, c186.x // {TAG}: a sharper highlight
    mad r5.w, r5.w, c7.y, c7.x
    mul r0.y, r0.y, r5.w
'''

DCL4 = '    dcl_texcoord4 v2.xyz\n'


def patch(rel, def_after, edits):
    path = ROOT / rel
    text = path.read_text()
    if TAG in text:
        return
    for anchor, block, before in [(def_after, DEF, False)] + edits:
        if text.count(anchor) != 1:
            sys.exit(f'{rel}: expected one "{anchor.strip()}"')
        text = text.replace(anchor, block + anchor if before else anchor + block)
    if DCL4 not in text:
        sys.exit(f'{rel}: no tangent input')
    path.write_text(text)


def main():
    patch('gta_hair_sorted_alpha/gta_hair_sorted_alphaPS5.asm', '    def c4, 32, 4, 1, 0.00390625\n', [
        ('    rsq r1.w, r1.w\n', STRANDS, False),
        ('    rsq r0.x, r0.y\n', GLOSS, True),
    ])
    patch('gta_hair_sorted_alpha_exp/gta_hair_sorted_alpha_expPS6.asm', '    def c2, 0, 1, 3.99600005, 4\n', [
        ('    rsq r1.w, r1.w\n', STRANDS, False),
    ])


if __name__ == '__main__':
    main()
