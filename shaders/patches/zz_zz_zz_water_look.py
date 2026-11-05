#!/usr/bin/env python3
# Writes zz_zz_zz_water_look.patch, Improved Water (Graphics > Lighting, source/waterlook.ixx), from a FusionShaders
# checkout with every other patch applied; the patch is the diff this script leaves (git diff).
#
# In both of the water's pixel shaders (waterPS1, and waterPS2, the pass with the sun's shadows). The sea stays flat and
# the physics as they are:
#  - Wind on the surface: warped sine waves some hundred metres across drift with the wind and scale the waves' slope:
#    above their middle rough patches, below it smooth ones that mirror the sky. c176: rough, smooth, drift x, y.
#  - Colour by depth: over a shallow bottom the water's own colour takes a tint, fading with the depth behind the surface.
#    c177.xyz the tint less 1; c187.w -log2(e) over the depth it fades over.
#  - Shore foam: where the bottom is close behind the surface, broken up by two reads of the waves' own texture drifting
#    against each other. It thins out where that depth changes faster than the ground does along the surface, a wall or a
#    pier's leg. c187: strength, 1 / width in metres, brightness.
#  - The sun's glint (PS2) widens with distance, as waves too small to see still turn the far surface: the exponent falls
#    and the peak with it. c177.w per metre.
#
# In deferred_lighting PS1/PS2 (the sun with cascades), after the cloud shadows:
#  - Caustics: under the water's level the sun's term takes a moving net of bright lines, worked out where the ray towards
#    the sun meets the surface. c186: water level, seconds, strength, waves per metre; c196.x 1 / the depth they fade over.
#
# All constants zero leaves the game's look. c186, c176 and c177 are free in these shaders; c188 to c192 are defs.
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / "win32_30_nv8"
TAG = "FusionFix Improved Water"

WATER_DEFS = f"""\
    def c188, 0.150000006, 0.0299999993, 1.70000005, 0 // {TAG}: the foam's noise per metre, its drift a second, the second read's scale, zero
    def c189, -0.699999988, 1.60000002, 4, 1 // {TAG}: the noise's centre and contrast, the edge's sharpness, one
    def c190, 0.0370000005, 0.0529999994, 0.0190000009, 0.0270000007 // {TAG}: the wind's waves per metre, and their warps'
    def c191, 0.159154937, 0.5, 6.28318548, -3.14159274 // {TAG}: radians to -pi..pi
    def c192, 1.29999995, 2.5, -0.25, 1.25 // {TAG}: the warp's size, the patches' contrast, the foam's fade on walls
"""


def wrap(reg):
    return f"""\
    mad {reg}, {reg}, c191.x, c191.y
    frc {reg}, {reg}
    mad {reg}, {reg}, c191.z, c191.w
"""


def wind(slope):
    return f"""\
    // --- {TAG}: wind on the surface, on the waves' slope ({slope})
    mov r27.xy, c176.zw
    add r27.xy, r27, v2
    mul r28.xy, r27.yx, c190.zw
{wrap("r28.xy")}\
    sincos r29.xy, r28.x
    sincos r30.xy, r28.y
    mul r28.x, r27.x, c190.x
    mad r28.x, r29.y, c192.x, r28.x
    mul r28.y, r27.y, c190.y
    mad r28.y, r30.y, c192.x, r28.y
{wrap("r28.xy")}\
    sincos r29.xy, r28.x
    sincos r30.xy, r28.y
    mul r29.x, r29.y, r30.y
    mul r29.x, r29.x, c192.y // about 0: rough above, smooth below
    mov_sat r29.y, r29.x
    mov_sat r29.z, -r29.x
    mul r29.y, r29.y, c176.x
    mul r29.z, r29.z, c176.y
    add r29.y, r29.y, -r29.z
    add r29.y, r29.y, c189.w
    mul {slope}.xy, {slope}, r29.y
"""


def depth_look(diff):
    return f"""\
    // --- {TAG}: colour by depth and shore foam ({diff} the depth behind the surface along the view, metres)
    max r31.w, {diff}, c188.w
    mul r31.w, r31.w, c187.w
    exp r31.w, r31.w
    mov r28.xyz, c177
    mad r31.xyz, r31.w, r28, c189.w // the tint on the water's colour
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
    dsx r29.x, {diff}
    dsy r29.y, {diff}
    dp2add r29.x, r29, r29, c188.w // the depth's change per pixel, squared
    dsx r30.xy, v2
    dp2add r30.x, r30, r30, c188.w
    dsy r28.xy, v2
    dp2add r30.y, r28, r28, c188.w
    max r30.x, r30.x, r30.y // the surface's metres per pixel, squared
    rcp r30.x, r30.x
    mul r29.x, r29.x, r30.x
    mad_sat r29.x, r29.x, c192.z, c192.w // gone on walls
    mul r27.y, r27.y, r29.x
"""


def foam_colour(sky, dst):
    return f"""\
    mov r28.xyz, {sky}
    mul r27.xzw, r28.xyyz, c187.z // {TAG}: the foam's colour
    lrp r28.xyz, r27.y, r27.xzww, {dst}
    mov {dst}.xyz, r28
"""


GLINT_BEFORE = f"""\
    // --- {TAG}: the glint widens with distance, its peak falling with the exponent
    mov r27.x, c177.w
    mul r27.x, r27.x, v0.z
    add r27.x, r27.x, c11.x
    rcp r27.x, r27.x
    mul r1.w, r1.w, r27.x
"""

CAUSTICS_DEF = f"""\
    def c188, 0.800000012, 0.899999976, 0.699999988, -0.600000024 // {TAG}: the warps' waves per unit (along y, along x) and their drift a second
    def c189, 1.20000005, 0.5, -0.400000006, 1.10000002 // {TAG}: the warps' size, the first layer's drift a second, the second wave's scale
    def c190, 0.919238806, 0.919238806, 0.300000012, -0.600000024 // {TAG}: the second layer, turned 45 degrees, and its drift
    def c191, 1.60000002, 0.5, 1, 0 // {TAG}: the net's gain about its mean, a half, one, zero
"""

CAUSTICS = f"""\
    // --- {TAG}: caustics under the water (r14 the world position), on the sun's term r0.y
    mov r15.x, c186.z
    add r15.y, c186.x, -r14.z // how deep under the surface
    min r15.x, r15.x, r15.y
    if_lt c158.z, r15.x
      mov r16.xyz, -c17 // towards the sun
      max r16.z, r16.z, c157.x
      rcp r16.z, r16.z
      mul r16.z, r16.z, r15.y
      mad r16.xy, r16, r16.z, r14 // where the ray meets the surface
      mul r16.xy, r16, c186.w
      mov r16.w, c186.y // seconds
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
      mul r16.y, r16.y, c186.z // the strength, fading with the depth
      mad r16.x, r16.x, c191.x, -c191.z
      mad r16.x, r16.x, r16.y, c191.z
      mul r0.y, r0.y, r16.x
    endif
"""

RINGS_DEF = "    def c186, 7, 17, 0.00100000005, 1 // FusionFix rain rings: the tilt, the second hash's offset, a small number, one\n"


def edit(rel, edits):
    path = ROOT / rel
    text = path.read_text()
    for anchor, new in edits:
        if text.count(anchor) != 1:
            sys.exit(f"{rel}: expected one {anchor.strip()!r}, found {text.count(anchor)}")
        text = text.replace(anchor, new)
    path.write_text(text)


edit("water/waterPS1.asm", [
    (RINGS_DEF, RINGS_DEF + WATER_DEFS),
    ("    add r0.xy, r0, c1.z\n", wind("r0") + "    add r0.xy, r0, c1.z\n"),
    ("    add r1.x, r1.x, -v0.z\n", "    add r1.x, r1.x, -v0.z\n" + depth_look("r1.x")),
    ("    max r1.x, r1.x, c204.z\n", f"    max r1.x, r1.x, c204.z\n    max r1.x, r1.x, r27.y // {TAG}: opaque under the foam\n"),
    ("    mad r0.xyz, c66, r0.w, r0\n", f"    mov r28.xyz, c66\n    mul r28.xyz, r28, r31 // {TAG}: the tint\n    mad r0.xyz, r28, r0.w, r0\n"),
    ("    mul oC0.xyz, r0, c39.y\n", foam_colour("c72", "r0") + "    mul oC0.xyz, r0, c39.y\n"),
])
edit("water/waterPS2.asm", [
    (RINGS_DEF, RINGS_DEF + WATER_DEFS),
    ("    add r4.xy, r1, c0.w\n", wind("r1") + "    add r4.xy, r1, c0.w\n"),
    ("    pow r2.w, r1.x, r1.w\n", GLINT_BEFORE + f"    pow r2.w, r1.x, r1.w\n    mul r2.w, r2.w, r27.x // {TAG}\n"),
    ("    add r0.x, r0.x, -v0.z\n", "    add r0.x, r0.x, -v0.z\n" + depth_look("r0.x")),
    ("      add_sat r0.x, r1.x, c0.x\n    endif\n", f"      add_sat r0.x, r1.x, c0.x\n    endif\n    max r0.x, r0.x, r27.y // {TAG}: opaque under the foam\n"),
    ("    mad r2.xyz, c72, r0.y, r2\n", f"    mov r28.xyz, c72\n    mul r28.xyz, r28, r31 // {TAG}: the tint\n    mad r2.xyz, r28, r0.y, r2\n"),
    ("    mul oC0.xyz, r2, c39.y\n", foam_colour("c73", "r2") + "    mul oC0.xyz, r2, c39.y\n"),
])
COUD = "    def c157, 0.05, 0, 0, 0 // FusionFix cloud shadows: the lowest sun height the plane is traced at\n"
for rel in ("deferred_lighting/deferred_lightingPS1.asm", "deferred_lighting/deferred_lightingPS2.asm"):
    edit(rel, [
        (COUD, COUD + CAUSTICS_DEF),
        ("      mul r0.y, r0.y, r17.x\n    endif\n", "      mul r0.y, r0.y, r17.x\n    endif\n" + CAUSTICS),
    ])
