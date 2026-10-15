#!/usr/bin/env python3
# Shared by vehicle_paint.py and vehicle_glass_wet.py: rain drops on cars, as pixel shader asm.
#
# The vertex shaders pass the position, normal and world up in the car's own space (the bone's for skinned parts),
# so the drops stay on the bodywork as the car moves. On the surface a frame of (across, up) is built from the
# normal and world up; in it:
#  - beads, a drop at a random spot in some of the cells of a grid, more of them the harder it rains;
#  - running drops on the sides: columns with drops sliding down at their own speed, a wet trail above each.
# Their height bends the normal (bump mapping from screen space derivatives, Mikkelsen's surface gradient, so no
# tangent frame is needed).
#
# Constants (source/vehiclepaint.ixx): c176.z the share of cells holding a drop, c176.w how far running drops slid
# in metres, c177.x cells per metre, c177.y a drop's height in metres, c177.z the running drops' share.

TAG = 'FusionFix rain on cars'

DEFS = f'''\
    def c180, 13.6999998, 0.5, 0.100000001, 0.25 // {TAG}: presence hash, a half, a tenth, a quarter
    def c181, 0.5, 0.100000001, 0.25, 9.99999975e-005 // {TAG}: columns and rows of running drops per bead cell, the head's place in its cell, a tiny alpha
    def c182, 0.300000012, 0.349999994, 3.33333325, 14.2857141 // {TAG}: the head's place across its column, 1 / half width, 1 / half height
    def c183, 1.66666663, 0.100000001, 0.100000001, 0.5 // {TAG}: 1 / trail length, darkening under drops, a wet film's specular, a drop's
    def c184, 0.899999976, 0.970000029, 0.955000043, 0.0450000018 // {TAG}: a wet film's gloss, a drop's; Schlick's 1 - F0 and F0 for the clearcoat
    def c185, 0.212599993, 0.715200007, 0.0722000003, 0 // {TAG}: luminance, zero
    def c186, 0.0799999982, 0.0399999991, 0.119999997, 0 // {TAG}: the pearl tint
    def c187, 0, 0.5, 0.00100000005, 1 // {TAG}: specular as stored, flat tops, one
    def c188, 0.103100002, 0.103000000, 0.0973000005, 33.3300018 // {TAG}: the hash
    def c189, 0.400000006, 0.300000012, 0.180000007, 0.100000001 // {TAG}: a bead's centre within its cell, its radius
'''


def hash3(src, dst, tmp):
    # Dave Hoskins' hash without sine, three numbers in [0, 1) from the two in src.xy (as src.xyx)
    return f'''\
      mul {dst}.xyz, {src}, c188
      frc {dst}.xyz, {dst}
      add {tmp}.xyz, {dst}.yzxw, c188.w
      dp3 {tmp}.x, {dst}, {tmp}
      add {dst}.xyz, {dst}, {tmp}.x
      add {tmp}.xyz, {dst}.xxyw, {dst}.yzzw
      mul {tmp}.xyz, {tmp}, {dst}.zyxw
      frc {dst}.xyz, {tmp}
'''



def drops(pos, nrm, up):
    """Expects r8.x = c176.z; leaves the drops in r24.x and the trails in r24.y. Uses r8-r18. May sit in a dynamic
    branch: the normal is bent by drops_bend() after it, outside any, as ps_3_0 takes no dsx/dsy of r# in one."""
    return f'''\
      // The surface's frame in the car's space: up along it (world up, or the car's x on flat tops) and across
      mov r10.xyz, {nrm}
      mov r11.xyz, {up}
      mov r12.xyz, {pos}
      nrm r13.xyz, r10
      dp3 r8.y, r13, r11
      mad r14.xyz, r13, -r8.y, r11
      add r14.x, r14.x, c187.z
      nrm r15.xyz, r14
      crs r16.xyz, r13, r15
      dp3 r17.x, r12, r16
      dp3 r17.y, r12, r15 // across and up, in metres
      // Beads: cells of 1 / c177.x metres, a drop at a random spot in some of them
      mul r17.zw, r17.xyxy, c177.x
      frc r18.xy, r17.zwzw
      add r18.zw, r17.zwzw, -r18.xyxy
{hash3('r18.zwzw', 'r9', 'r10')}\
      mad r10.xy, r9, c189.x, c189.y
      add r10.xy, r18, -r10
      mad r10.z, r9.z, c189.z, c189.w
      rcp r10.z, r10.z
      mul r10.xy, r10, r10.z
      dp2add r10.w, r10, r10, c185.w
      add_sat r10.w, -r10.w, c187.w // the bead's dome
      mul r9.w, r9.z, c180.x
      frc r9.w, r9.w
      add r9.w, r9.w, -r8.x
      cmp r9.w, r9.w, c187.x, c187.w
      mul r24.x, r10.w, r9.w
      // Running drops, on the sides: columns of half the bead density, each sliding at its own speed
      add r8.y, -r8_abs.y, c187.w
      add_sat r8.y, r8.y, r8.y
      mul r8.y, r8.y, c177.z
      mul r17.z, r17.x, c177.x
      mul r17.z, r17.z, c181.x
      frc r18.x, r17.z
      add r18.z, r17.z, -r18.x // the column
{hash3('r18.zzzz', 'r9', 'r10')}\
      add r9.w, r9.x, c180.y
      mul r9.w, r9.w, c176.w // how far it slid: 0.5 to 1.5 times c176.w metres
      add r9.w, r9.w, r17.y
      mul r9.w, r9.w, c177.x
      mad r9.w, r9.w, c181.y, r9.y
      frc r18.y, r9.w
      add r18.w, r9.w, -r18.y // the row
      mad r10.y, r9.z, c182.x, c182.y
      add r10.x, r18.x, -r10.y
      mul r10.x, r10.x, c182.z
      add r10.y, r18.y, -c181.z
      mul r10.z, r10.y, c182.w
      mul r10.w, r10.x, r10.x
      mad r10.w, r10.z, r10.z, r10.w
      add_sat r10.w, -r10.w, c187.w // the head's dome
      add_sat r11.x, -r10_abs.x, c187.w
      mul r11.y, r10.y, c183.x
      add_sat r11.z, -r11.y, c187.w
      cmp r11.z, r10.y, r11.z, c185.w
      mul r11.z, r11.z, r11.x // the trail above it
{hash3('r18.zwzw', 'r9', 'r12')}\
      add r9.w, r9.x, -r8.x
      cmp r9.w, r9.w, c187.x, c187.w
      mul r9.w, r9.w, r8.y
      mul r24.y, r11.z, r9.w
      mul r10.w, r10.w, r9.w
      max r24.x, r24.x, r10.w
'''


def drops_bend():
    """Expects r27.xyz = the surface's unit normal (world space), v2 = the camera minus the position and r24.x the
    drops (zero where drops() did not run); leaves the bent normal in r27.xyz. Uses r8.w, r9-r14. Outside any dynamic
    branch, for the derivatives."""
    return f'''\
    // The drops' height (c177.y metres) bends the normal: Mikkelsen's surface gradient, from the derivatives of
    // the position (v2 is the camera minus it, which flips the gradient's sign and so its sum below)
    mul r8.w, r24.x, c177.y
    dsx r9.xyz, v2
    dsy r10.xyz, v2
    crs r11.xyz, r10, r27
    crs r12.xyz, r27, r9
    dp3 r13.x, r9, r11
    dsx r13.y, r8.w
    dsy r13.z, r8.w
    mul r14.xyz, r11, r13.y
    mad r14.xyz, r12, r13.z, r14
    cmp r13.w, r13.x, c187.w, -c187.w
    mul r14.xyz, r14, r13.w
    mad r14.xyz, r27, r13_abs.x, r14
    nrm r27.xyz, r14
'''
