#!/usr/bin/env python3
# Shared by vehicle_paint.py and vehicle_glass_reflections_wet.py: rain drops on cars, as pixel shader asm.
#
# The vertex shaders pass the position and normal in the car's own space (the bone's for skinned parts), so the drops
# stay on the bodywork as the car moves; nothing of the world goes in, so they also stay put as the body pitches and
# rolls on its springs (with world up, the frame on the near flat bonnet swung with every bit of pitch and the beads
# swam while the car sped up). On the bodywork:
#  - beads in two sizes: small ones (0.8 to 1.7 mm of radius at the default density, anywhere in their cell: kept to
#    its middle third they lined up in rows) in some cells of a fine grid, and fewer big ones (2.5 to 5 mm; at 5 to
#    9 mm they read as small puddles) on a coarse one, more of both the harder it rains. The grid lies across the
#    body's forward axis (n x y, fixed to the body and defined everywhere but on faces straight ahead or behind, where
#    it only turns; a 3D grid of spheres was tried and left rows and empty bands on the near flat bonnet);
#  - running drops on the sides: sparse columns some 4 cm apart along the body's own vertical, a narrow head (3 x 8 mm)
#    sliding down now and then in each at its own speed, with a wet trail (2.4 mm, 7 to 22 cm long) above it that
#    meanders up to 4 mm the higher it goes (at 1.5 mm, a pixel and a half up close, trails stayed ruler straight);
#    none on flat tops. Short trails on a dense grid read as dirt.
# Each drop's height is a dome whose slope is known exactly, so the normal is bent from that slope carried to the
# screen by the derivatives of the smooth position (Mikkelsen's surface gradient, no tangent frame): the derivatives
# of the drops themselves, taken over 2 x 2 pixels across their edges, made them shimmer as cars moved. A drop's edge
# is softened over a pixel, and drops under 2 pixels across fade into their mean cover, so far cars show an even wet
# sheen instead of crawling dots.
#
# Constants (source/vehiclepaint.ixx): c176.z the share of cells holding a drop, c176.w how far running drops slid
# in metres, c177.x bead cells per metre, c177.y a drop's height in metres, c177.z the running drops' share.
#
# drops() may sit in a dynamic branch: it takes its derivatives from the position, a v#, which ps_3_0 allows there
# (of r# it does not, X6077). It leaves the cover in r24.x, the trails in r24.y and the height's screen slope (d/dx,
# d/dy) in r24.zw (zero it first); drops_bend(), outside any branch, bends r27.xyz by it.

TAG = 'FusionFix rain on cars'

DEFS = f'''\
    def c180, 13.6999998, 0.5, 0.699999988, 0.769230783 // {TAG}: presence hash, a half; drops fade out under 2 pixels across
    def c181, 0.600000024, 3, 0.300000012, 9.99999975e-005 // {TAG}: running drop columns per bead cell, rows per metre, the head's place in its row, a tiny alpha
    def c182, 0.400000006, 0.300000012, 0.333333343, -2 // {TAG}: the head's place across its column, a row's length in metres, -2
    def c183, 4.5, 0.100000001, 0.100000001, 0.150000006 // {TAG}: 1 / trail length, darkening under drops, a wet film's specular, a drop's
    def c184, 0.899999976, 0.970000029, 0.955000043, 0.0450000018 // {TAG}: a wet film's gloss, a drop's; Schlick's 1 - F0 and F0 for the clearcoat
    def c185, 0.212599993, 0.715200007, 0.0722000003, 0 // {TAG}: luminance, zero
    def c186, 0.0799999982, 0.0399999991, 0.119999997, 0 // {TAG}: the pearl tint
    def c187, 0, 0.5, 0.00100000005, 1 // {TAG}: zero, flat tops, one
    def c188, 0.103100002, 0.103000000, 0.0973000005, 33.3300018 // {TAG}: the hash
    def c189, 3, 0.100000001, 0.100000001, 0.699999988 // {TAG}: small beads: cells per bead cell (some 8 mm), radius from, radius range (cells), share
    def c190, 0.600000024, 0.200000003, 0.0733000040, 100000 // {TAG}: their centre's range and start, mean cover of a cell; most pixels per metre
    def c191, 0.800000012, 0.0799999982, 0.0799999982, 0.25 // {TAG}: big beads: cells per bead cell, radius from, radius range (cells), share
    def c192, 0.680000007, 0.159999996, 0.0469999984, 1 // {TAG}: their centre's range and start, mean cover of a cell; running heads' share
    def c193, 666.666687, 250, 0.00150000001, 0.00120000006 // {TAG}: 1 / a running head's half width and half height, its half width, a trail's (m)
    def c194, 20, 4, 0, 0 // {TAG}: the clearcoat in full from a material specular of 0.05 (paint); running drops gone from |n.z| 0.75 over a quarter
    def c195, 0, 1, 0, 0 // {TAG}: the body's forward axis
    def c196, 0.00100000005, 0, 1, 0.75 // {TAG}: the body's vertical, a little across so flat tops keep a frame
    def c197, 8, 6.28318548, -3.14159274, 0.00400000019 // {TAG}: a trail's meander: turns per metre, 2 pi, -pi, its reach (m)
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


def bead_layer(pos, sizes, place, cover, slope):
    # One grid of beads in the frame r17 (across) / r18 (up): sizes = (cells per bead cell, radius from, radius range,
    # share), place = (centre range, centre start, mean cover). Leaves its cover in `cover` and the height's screen slope
    # in `slope`.
    spread = '.xxxy' if slope.endswith('.zw') else ''
    return f'''\
      mov r9.x, c177.x
      mul r9.x, r9.x, {sizes}.x // cells per metre
      rcp r9.y, r9.x
      dp3 r10.x, {pos}, r17
      dp3 r10.y, {pos}, r18
      mul r10.xy, r10, r9.x
      frc r11.xy, r10
      add r11.zw, r10.xyxy, -r11.xyxy // the cell
{hash3('r11.zwzw', 'r12', 'r13')}\
      mad r13.xy, r12, {place}.x, {place}.y
      add r13.xy, r11, -r13 // from the bead's centre, in cells
      mad r13.z, r12.z, {sizes}.z, {sizes}.y // its radius
      rcp r13.w, r13.z
      mul r13.xy, r13, r13.w
      dp2add r14.x, r13, r13, c187.x
      add r14.y, -r14.x, c187.w // its dome
      mul r14.z, r13.z, r9.y
      mul r14.z, r14.z, r8.z // its radius in pixels
      mul r14.w, r14.y, r14.z
      mul_sat r14.w, r14.w, c180.y // its edge, softened over a pixel
      add r15.x, r14.z, -c180.z
      mul_sat r15.x, r15.x, c180.w // under 2 pixels across it fades into the mean
      mul r15.y, r12.z, c180.x
      frc r15.y, r15.y
      mul r15.z, r8.x, {sizes}.w
      add r15.y, r15.y, -r15.z
      cmp r15.y, r15.y, c187.x, c187.w // whether the cell holds one
      mul r15.w, r15.z, {place}.z
      mul r14.w, r14.w, r15.y
      lrp {cover}, r15.x, r14.w, r15.w
      cmp r15.z, r14.y, c187.w, c187.x
      mul r15.z, r15.z, r15.y
      mul r15.z, r15.z, r15.x
      mul r15.z, r15.z, c177.y
      mul r13.xy, r13, r13.w
      mul r13.xy, r13, r9.x
      mul r13.xy, r13, r15.z // h d / r, in metres per metre along the frame
      mul r14.xyz, r17, r13.x
      mad r14.xyz, r18, r13.y, r14
      dsx r13.xyz, {pos}
      dsy r12.xyz, {pos}
      dp3 r15.x, r14, r13
      dp3 r15.y, r14, r12
      mul {slope}, r15{spread}, c182.w // the height's screen slope: -2 h d / r along the position's derivatives
'''


def drops(pos, nrm):
    """pos and nrm are the car-space position and normal (v#), r8.x = c176.z, r24 zeroed; leaves the cover in r24.x, the
    trails in r24.y and the height's screen slope in r24.zw. Uses r8.yzw, r9-r18. May sit in a dynamic branch."""
    return f'''\
      // Pixels per metre, from the position's own derivatives
      dsx r9.xyz, {pos}
      dsy r10.xyz, {pos}
      dp3 r9.w, r9, r9
      dp3 r10.w, r10, r10
      max r9.w, r9.w, r10.w
      rsq r9.w, r9.w
      min r8.z, r9.w, c190.w
      // The beads' frame: across the body's forward axis, and up the surface from there
      nrm r13.xyz, {nrm}
      mov r11.xyz, c195
      crs r14.xyz, r13, r11
      add r14.z, r14.z, c196.x
      nrm r17.xyz, r14
      crs r18.xyz, r13, r17
      // Beads: small ones on a fine grid, big ones on a coarse one
{bead_layer(pos, 'c189', 'c190', 'r24.x', 'r24.zw')}\
{bead_layer(pos, 'c191', 'c192', 'r16.z', 'r16.xy')}\
      max r24.x, r24.x, r16.z
      add r24.zw, r24, r16.xxxy
      // Running drops, on the sides only (none on tops flatter than |n.z| 0.75), in a frame up the body's vertical
      nrm r13.xyz, {nrm}
      add r8.y, -r13_abs.z, c196.w
      mul_sat r8.y, r8.y, c194.y
      mul r8.y, r8.y, c177.z
      mad r14.xyz, r13, -r13.z, c196
      nrm r18.xyz, r14
      crs r17.xyz, r13, r18
      dp3 r16.z, {pos}, r17
      dp3 r8.w, {pos}, r18
      // columns 0.6 times the bead density (some 4 cm apart), narrow heads sliding at the column's speed
      mov r9.x, c177.x
      mul r9.x, r9.x, c181.x
      rcp r9.y, r9.x
      mul r10.x, r16.z, r9.x
      frc r11.x, r10.x
      add r11.z, r10.x, -r11.x // the column
{hash3('r11.zzzz', 'r12', 'r13')}\
      add r9.w, r12.x, c180.y
      mul r9.w, r9.w, c176.w // how far it slid: 0.5 to 1.5 times c176.w metres
      add r9.w, r9.w, r8.w
      mul r9.w, r9.w, c181.y
      add r9.w, r9.w, r12.y
      frc r11.y, r9.w
      add r11.w, r9.w, -r11.y // the row
      mad r13.x, r12.z, c182.x, c182.y
      add r13.x, r11.x, -r13.x
      mul r13.x, r13.x, r9.y // from the head, across, in metres
      add r13.y, r11.y, -c181.z
      mul r13.y, r13.y, c182.z // and up
      mul r14.xy, r13, c193
      dp2add r14.z, r14, r14, c187.x
      add r14.z, -r14.z, c187.w // the head's dome
      mul r14.w, r8.z, c193.z // its half width in pixels
      mul r15.x, r14.z, r14.w
      mul_sat r15.x, r15.x, c180.y
      add r15.y, r14.w, -c180.z
      mul_sat r15.y, r15.y, c180.w
{hash3('r11.zwzw', 'r12', 'r10')}\
      mul r16.x, r8.x, c192.w
      add r12.x, r12.x, -r16.x
      cmp r12.x, r12.x, c187.x, c187.w // whether the row holds one
      mul r12.x, r12.x, r8.y
      // Its trail above it meanders more the higher it goes, and runs 7 to 22 cm, by the row
      mad r10.w, r13.y, c197.x, r12.z
      frc r10.w, r10.w
      mad r10.w, r10.w, c197.y, c197.z
      sincos r16.xy, r10.w
      mul_sat r10.w, r13.y, c183.x
      mul r16.y, r16.y, r10.w
      mad r10.w, r16.y, c197.w, r13.x // from the trail, across, in metres
      // thinner than a pixel, a pixel wide and fainter
      mul r15.z, r8.z, c193.w
      max r15.w, r15.z, c187.w
      rcp r15.w, r15.w
      mul r15.w, r15.w, r8.z
      mul r15.w, r10_abs.w, r15.w
      add_sat r15.w, -r15.w, c187.w
      min r15.z, r15.z, c187.w
      mul r15.w, r15.w, r15.z
      mul r16.y, r13.y, c183.x
      add r16.z, r12.y, r12.y
      add r16.z, r16.z, c187.w
      mul r16.y, r16.y, r16.z
      add_sat r16.y, -r16.y, c187.w
      cmp r16.y, r13.y, r16.y, c187.x
      mul r15.w, r15.w, r16.y
      mul r24.y, r15.w, r12.x
      mul r15.x, r15.x, r12.x
      mul r15.x, r15.x, r15.y
      max r24.x, r24.x, r15.x
      cmp r16.w, r14.z, c187.w, c187.x
      mul r16.w, r16.w, r12.x
      mul r16.w, r16.w, r15.y
      mul r16.w, r16.w, c177.y
      mul r14.xy, r14, c193
      mul r14.xy, r14, r16.w
      mul r15.xyz, r17, r14.x
      mad r15.xyz, r18, r14.y, r15
      dsx r13.xyz, {pos}
      dsy r12.xyz, {pos}
      dp3 r16.x, r15, r13
      dp3 r16.y, r15, r12 // carried to the screen
      mad r24.zw, r16.xxxy, c182.w, r24
'''


def drops_bend():
    """Expects r27.xyz = the surface's unit normal (world space), v2 = the camera minus the position and r24.zw the
    height's screen slope (zero where drops() did not run); leaves the bent normal in r27.xyz.
    Uses r9-r14. Outside any dynamic branch, for the derivatives."""
    return f'''\
    // The drops' height bends the normal: Mikkelsen's surface gradient from its screen slope (v2 is the camera minus
    // the position, which flips the gradient's sign and so its sum)
    mov r13.yz, r24.xzww
    dsx r9.xyz, v2
    dsy r10.xyz, v2
    crs r11.xyz, r10, r27
    crs r12.xyz, r27, r9
    dp3 r13.x, r9, r11
    mul r14.xyz, r11, r13.y
    mad r14.xyz, r12, r13.z, r14
    cmp r13.w, r13.x, c187.w, -c187.w
    mul r14.xyz, r14, r13.w
    mad r14.xyz, r27, r13_abs.x, r14
    nrm r27.xyz, r14
'''
