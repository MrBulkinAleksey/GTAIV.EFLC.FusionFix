#!/usr/bin/env python3
# Writes world_ssr_occlusion.patch: the ambient occlusion leaves alone what SSR found (source/ssrocclusion.ixx).
#
# deferred_lighting PS1-4 (the sun with and without cascades, and the ambient pass at night) multiply the reflection by
# the G-buffer's occlusion (GBuffer2.z: the vertex colour's sky exposure times the screen space AO written into it).
# That is right for the game's parabolic map, which holds only the sky and knows nothing of the corner it is seen
# from; but where SSR found the scene in the reflection, the occluder is already in it, and the occlusion darkened it
# twice: puddles along walls, car paint and shop windows in shaded streets reflected the street darker than it is.
# Here the occlusion fades out by SSR's weight (r10.w): ao + (1 - ao) x weight x c155.x.
# c155 (set as the lighting begins) = strength (0 = as the game), -, -, -. Def c192 (one; free there); r11.w free there.
#
# Usage: world_ssr_occlusion.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8' / 'deferred_lighting'
TAG = 'FusionFix world_ssr_occlusion.patch'
SHADERS = [f'deferred_lightingPS{i}.asm' for i in (1, 2, 3, 4)]

DEF = f'    def c192, 1, 0, 0, 0 // {TAG}: one\n'


def block(ao):
    return f'''\
      // --- {TAG}: the occlusion ({ao}) fades out where SSR found the scene (r10.w), the occluder already in it
      add r11.w, c192.x, -{ao}
      mul r11.w, r11.w, r10.w
      mul r11.w, r11.w, c155.x
      add r11.w, r11.w, {ao}
'''


def patch(path):
    lines = path.read_text().splitlines(keepends=True)
    if any(TAG in l for l in lines):
        return
    blend = [i for i, l in enumerate(lines) if re.match(r'\s*mad r0\.[xyzw], r11\.w, r10\.w, r0\.[xyzw]', l)]
    if len(blend) != 1:
        sys.exit(f'{path.name}: expected one SSR horizon blend, found {len(blend)}')
    target = None
    for i in range(blend[0] + 1, blend[0] + 6):
        m = re.match(r'(\s*)mul (\S+), (r[1-9][0-9]*\.z), (\S+)\s*$', lines[i])  # not r0.z, the horizon fade
        if m:
            target = (i, m)
            break
    if not target:
        sys.exit(f'{path.name}: no multiply by the occlusion after the SSR blend')
    i, m = target
    ao = m.group(3)
    lines[i] = f'{m.group(1)}mul {m.group(2)}, r11.w, {m.group(4)} // {TAG}: was {ao}\n'
    lines.insert(i, block(ao))
    last_def = max(j for j, l in enumerate(lines) if l.strip().startswith('def c'))
    lines.insert(last_def + 1, DEF)
    path.write_text(''.join(lines))


def main():
    for name in SHADERS:
        patch(ROOT / name)


if __name__ == '__main__':
    main()
