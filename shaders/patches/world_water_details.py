#!/usr/bin/env python3
# Writes world_water_details.patch: foam along the shores and caustics on what lies under the water (Water Details in
# Graphics > Lighting, source/waterdetails.ixx).
#
# Foam, in both of the water's pixel shaders (waterPS1, and waterPS2, the pass with the sun's shadows): the water
# already reads the scene's depth behind it for its alpha; where that depth is close behind the surface (a shore, a
# pier's leg, a hull) the foam comes in, broken up by two reads of the waves' own texture (s0) drifting against each
# other, more of it the nearer the bottom: noise > depth / width. It is lit as the sky's colour at the horizon
# (bottomSkyColour, c72 in PS1, c73 in PS2) and makes the water opaque under it.
# c187 (set before the water is drawn) = strength (0 = off), 1 / width in metres, brightness, -; defs c188, c189;
# c178.y is the clock the rain rings already take.
#
# Caustics, in deferred_lighting PS1/PS2 (the sun with cascades), after the cloud shadows: under the sea's level the
# sun's light is focused into the moving net of bright lines the waves make. The ray from the surface towards the sun
# is followed up to the water's level and the net is worked out there, two layers of warped sine waves whose
# zero crossings are the lines; brightest just under the surface, gone c196.x metres down. It multiplies the sun's
# shadow term, so what lies in shadow takes none. c195 (set before the lighting) = water level, seconds, strength
# (0 = off), waves per metre; c196 = 1 / the depth they fade over, -, -, -; defs c188-c191, and c157/c161 of the cloud
# shadows' patch.
#
# Usage: world_water_details.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8'
TAG = 'FusionFix world_water_details.patch'

FOAM_DEF = f'''\
    def c188, 0.150000006, 0.0299999993, 1.70000005, 0 // {TAG}: the foam's noise per metre, its drift a second, the second read's scale, zero
    def c189, -0.699999988, 1.60000002, 4, 1 // {TAG}: the noise's centre and contrast, the edge's sharpness, one
'''


def foam(diff, sky):
    return f'''\
    // --- {TAG}: foam where the bottom is close behind the surface ({diff} the depth behind it along the view, metres)
    mov r27.x, {diff}
    mul r28.xy, v2.xy, c188.x
    mov r27.w, c178.y
    mad r29.xy, r27.w, c188.y, r28
    mul r28.xy, r28, c188.z
    mad r30.xy, r27.w, -c188.y, r28
    texld r29, r29, s0
    texld r30, r30, s0
    add r28.x, r29.z, r30.w
    add r28.x, r28.x, c189.x
    mul_sat r28.x, r28.x, c189.y // the noise
    mul_sat r28.y, r27.x, c187.y // the depth in widths
    add r28.z, r28.x, -r28.y
    mul_sat r28.z, r28.z, c189.z
    add r28.y, -r28.y, c189.w
    mul r28.z, r28.z, r28.y
    mul r27.y, r28.z, c187.x // the foam
    mov r28.xyz, {sky}
    mul r27.xzw, r28.xyyz, c187.z // its colour
'''


FOAM_COLOUR = f'''\
    lrp r28.xyz, r27.y, r27.xzww, {{dst}} // {TAG}: the foam's colour
    mov {{dst}}.xyz, r28
'''

PS1 = 'water/waterPS1.asm'
PS2 = 'water/waterPS2.asm'

CAUSTICS_DEF = f'''\
    def c188, 0.800000012, 0.899999976, 0.699999988, -0.600000024 // {TAG}: the warps' waves per unit (along y, along x) and their drift a second
    def c189, 1.20000005, 0.5, -0.400000006, 1.10000002 // {TAG}: the warps' size, the first layer's drift a second, the second wave's scale
    def c190, 0.919238806, 0.919238806, 0.300000012, -0.600000024 // {TAG}: the second layer, turned 45 degrees, and its drift
    def c191, 1.60000002, 0.5, 1, 0 // {TAG}: the net's gain about its mean, a half, one, zero
'''

CAUSTICS = f'''\
    // --- {TAG}: caustics under the water (c195, c196; waterdetails.ixx): the sun's light focused by the waves, worked
    // --- out where the ray towards the sun meets the water's level (r14 the world position), on the sun's term r0.y
    mov r15.x, c195.z
    add r15.y, c195.x, -r14.z // how deep under the surface
    min r15.x, r15.x, r15.y
    if_lt c158.z, r15.x
      mov r16.xyz, -c17 // towards the sun
      max r16.z, r16.z, c157.x
      rcp r16.z, r16.z
      mul r16.z, r16.z, r15.y
      mad r16.xy, r16, r16.z, r14 // where the ray meets the surface
      mul r16.xy, r16, c195.w
      mov r16.w, c195.y // seconds
      mul r17.x, r16.y, c188.x
      mad r17.x, r16.w, c188.z, r17.x
      mul r17.y, r16.x, c188.y
      mad r17.y, r16.w, c188.w, r17.y
      mad r17.xy, r17, c161.x, c161.y
      frc r17.xy, r17
      mad r17.xy, r17, c161.z, c161.w
      sincos r12.xy, r17.x
      sincos r13.xy, r17.y // the warps
      mad r17.x, r12.y, c189.x, r16.x
      mad r17.x, r16.w, c189.y, r17.x
      mul r17.y, r16.y, c189.w
      mad r17.y, r13.y, c189.x, r17.y
      mad r17.y, r16.w, c189.z, r17.y
      add r17.z, r16.x, r16.y
      mul r17.z, r17.z, c190.x
      mad r17.z, r13.x, c189.x, r17.z
      mad r17.z, r16.w, c190.z, r17.z
      add r17.w, r16.y, -r16.x
      mul r17.w, r17.w, c190.y
      mad r17.w, r12.x, c189.x, r17.w
      mad r17.w, r16.w, c190.w, r17.w
      mad r17, r17, c161.x, c161.y
      frc r17, r17
      mad r17, r17, c161.z, c161.w
      sincos r12.xy, r17.x
      sincos r13.xy, r17.y
      add r16.x, r12.y, r13.y
      sincos r12.xy, r17.z
      sincos r13.xy, r17.w
      add r16.y, r12.y, r13.y
      mad r16.xy, r16_abs, -c191.y, c191.z // near 1 on the lines, where the waves cancel
      mul r16.zw, r16.xyxy, r16.xyxy
      mul r12.xy, r16.zwzw, r16.zwzw
      mul r16.xy, r12, r16.zwzw // to the 6th: thin lines
      add r16.x, r16.x, r16.y
      mul_sat r16.y, r15.y, c196.x
      add r16.y, -r16.y, c191.z
      mul r16.y, r16.y, c195.z // the strength, fading with the depth
      mad r16.x, r16.x, c191.x, -c191.z
      mad r16.x, r16.x, r16.y, c191.z
      mul r0.y, r0.y, r16.x
    endif
'''


def insert(text, anchor, block, rel, before=False):
    if text.count(anchor) != 1:
        sys.exit(f'{rel}: expected one "{anchor.strip()}"')
    return text.replace(anchor, block + anchor if before else anchor + block)


def patch(rel, edits):
    path = ROOT / rel
    text = path.read_text()
    if TAG in text:
        return
    for anchor, block, before in edits:
        text = insert(text, anchor, block, rel, before)
    path.write_text(text)


def main():
    patch(PS1, [
        ('    def c186, 7, 17, 0.00100000005, 1 // FusionFix rain rings: the tilt, the second hash\'s offset, a small number, one\n', FOAM_DEF, False),
        ('    add r1.x, r1.x, -v0.z\n', foam('r1.x', 'c72'), False),
        ('    max r1.x, r1.x, c204.z\n', f'    max r1.x, r1.x, r27.y // {TAG}: opaque under the foam\n', False),
        ('    mul oC0.xyz, r0, c39.y\n', FOAM_COLOUR.format(dst='r0'), True),
    ])
    patch(PS2, [
        ('    def c186, 7, 17, 0.00100000005, 1 // FusionFix rain rings: the tilt, the second hash\'s offset, a small number, one\n', FOAM_DEF, False),
        ('    add r0.x, r0.x, -v0.z\n', foam('r0.x', 'c73'), False),
        ('      add_sat r0.x, r1.x, c0.x\n    endif\n', f'    max r0.x, r0.x, r27.y // {TAG}: opaque under the foam\n', False),
        ('    mul oC0.xyz, r2, c39.y\n', FOAM_COLOUR.format(dst='r2'), True),
    ])
    for rel in ('deferred_lighting/deferred_lightingPS1.asm', 'deferred_lighting/deferred_lightingPS2.asm'):
        patch(rel, [
            ('    def c157, 0.05, 0, 0, 0 // FusionFix cloud shadows: the lowest sun height the plane is traced at\n', CAUSTICS_DEF, False),
            ('      mul r0.y, r0.y, r17.x\n    endif\n', CAUSTICS, False),
        ])


if __name__ == '__main__':
    main()
