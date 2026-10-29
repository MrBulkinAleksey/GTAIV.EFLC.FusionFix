#!/usr/bin/env python3
# Writes sky_planar_clouds.patch: the sky dome's flat clouds and its high layer (gta_atmoscatt_clouds PS1, 5, 7, 8
# and 9) laid on the sky as a flat deck seen in perspective, not as the dome's own texture coordinates.
#
# The dome's texture coordinates (v0, the vertex shaders' v1 as it is) are a disc about (0.5, 0.5), out from it by the
# sine of the angle from the zenith, as the dome seen from straight above: towards the horizon a degree of the sky
# takes less and less of the texture, and the clouds came out drawn out in streaks towards the zenith wherever the view
# was some way off it. A deck over the camera takes the tangent instead; (1 + k) sin / (cos + k) keeps the zenith's
# scale, grows the disc K_GROWTH times at the horizon in place of without end, where the game fades the clouds out
# anyway (CloudFadeOut), and needs no more of the dome than its direction (v1, y up). Each texture coordinate moves out
# from where it was by (v0 - 0.5) * ((1 + k) / (max(y, 0) + k) - 1), so whatever each vertex shader adds about the
# middle (the self-shadow's offset in v2.xy, the high layer's middle and scale) stays as it was; the high layer is laid
# out TopCloudBiasDetailThresholdHeight.w times larger, so its move is that many times too. The detail's own
# coordinates (v2.zw, out from the middle by the square already) are left alone.
#
# Usage: sky_planar_clouds.py <FusionShaders checkout with the patches before this one applied>; the patch is the diff
# it leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'gta_atmoscatt_clouds'
TAG = 'FusionFix sky_planar_clouds.patch'
VARIANTS = (1, 5, 7, 8, 9)
K = 0.2
K_GROWTH = (1.0 + K) / K

DEFS = f'''\
    def c154, {1.0 + K:.8f}, {K:.8f}, -0.5, 1 // {TAG}: 1 + k, k, minus a half, one
    def c155, 0, 9.99999975e-006, 0, 0 // {TAG}: zero, tiny
'''


def remap(top):
    return f'''\
    // --- {TAG}: the clouds' coordinates as a flat deck's: r28 v0, r29 v2.xy, r30 v4.xy
    add r27.xyz, v1, c155.y
    nrm r28.xyz, r27
    max r27.x, r28.y, c155.x
    add r27.x, r27.x, c154.y
    rcp r27.x, r27.x
    mad r27.x, r27.x, c154.x, -c154.w // (1 + k) / (y + k) - 1
    add r27.yz, v0.xxyy, c154.z
    mul r27.yz, r27, r27.x // (v0 - 0.5) times that
    add r28.xy, v0, r27.yzzw
    add r29.xy, v2, r27.yzzw
    mul r27.yz, r27, {top}.w // the high layer is laid out TopCloudBiasDetailThresholdHeight.w times larger
    add r30.xy, v4, r27.yzzw
    // --- {TAG} end
'''


for n in VARIANTS:
    path = ROOT / f'gta_atmoscatt_cloudsPS{n}.asm'
    text = path.read_text()
    if TAG in text:
        continue
    for r in ('r27', 'r28', 'r29', 'r30', 'c154', 'c155'):
        if re.search(rf'\b{r}\b', text):
            sys.exit(f'{path.name}: {r} is taken')
    top = re.search(r'//\s+TopCloudBiasDetailThresholdHeight\s+(c\d+)', text).group(1)
    lines = text.split('\n')
    # The constants after ps_3_0's own defs, the remap after the last declaration.
    first_def = next(i for i, l in enumerate(lines) if l.strip().startswith('def '))
    lines[first_def:first_def] = DEFS.rstrip('\n').split('\n')
    last_dcl = max(i for i, l in enumerate(lines) if l.strip().startswith('dcl_'))
    lines[last_dcl + 1:last_dcl + 1] = remap(top).rstrip('\n').split('\n')
    body_start = last_dcl + 1 + remap(top).count('\n')
    counts = {'v0': 0, 'v2': 0, 'v4': 0, 'scale': 0}
    for i in range(body_start, len(lines)):
        l = lines[i]
        m = re.match(r'(\s*texld\w*\s+r\d+(\.\w+)?,\s*)(v0|v2|v4)(\s*,\s*s\d+.*)$', l)
        if m:
            reg = {'v0': 'r28', 'v2': 'r29', 'v4': 'r30'}[m.group(3)]
            lines[i] = f'{m.group(1)}{reg}{m.group(4)} // {TAG}'
            counts[m.group(3)] += 1
            continue
        # The detail's second read, at v2.xy times CloudThicknessEdgeSmoothDetailScaleStrength.z.
        m = re.match(r'(\s*mul\s+r\d+\.\w+,\s*c\d+\.z,\s*)v2(\.xxyw)?(\s*)$', l)
        if m:
            lines[i] = f'{m.group(1)}r29{m.group(2) or ""} // {TAG}'
            counts['scale'] += 1
    if counts != {'v0': 1, 'v2': 1, 'v4': 1, 'scale': 1}:
        sys.exit(f'{path.name}: unexpected reads {counts}')
    path.write_text('\n'.join(lines))
    print(f'{path.name}: high layer {top}')
