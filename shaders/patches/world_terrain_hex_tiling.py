#!/usr/bin/env python3
# Writes world_terrain_hex_tiling.patch, on top of world_terrain_and_parallax.patch: Ground Surfaces' anti-tiling in
# the generic world shaders (gta_default, gta_spec, rage_default, gta_normal, gta_normal_spec; PS3 and PS4) becomes
# hexagonal tiling, and only takes textures that tile evenly.
#  - Before: a second read at a fixed offset, mixed in patches of a world space noise. Both reads repeat with the
#    same period, so the repeat still showed.
#  - Now: the texture coordinates are cut into a hexagonal lattice (a cell about a repeat across); every lattice
#    vertex shifts the texture by its own random offset and each pixel blends the three around it, weighted towards
#    the nearest. The blend keeps the texture's contrast: its departure from the texture's average is divided by
#    sqrt(sum of the weights squared), so blended spots do not wash out (Heitz and Neyret's variance preserving blend).
#  - c179.x is how evenly the bound diffuse texture tiles, from texturequality.ixx's look at its small mips (even
#    at large scale, natural colours, no stripes): a path with tracks, a lawn's edge or a pattern stays as it is.
# Usage: world_terrain_hex_tiling.py <FusionShaders checkout with the other patches applied>; the patch is the diff
# it leaves (git diff).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8'
TAG = 'FusionFix world_terrain_hex_tiling.patch'
SHADERS = ['gta_default', 'gta_spec', 'rage_default', 'gta_normal', 'gta_normal_spec']

DEFS = f'''\
    def c197, 0.103100002, 0.103000000, 0.0973000005, 33.3300018 // {TAG}: the hash
    def c198, 3.46410155, -0.577350259, 1.15470052, 0 // {TAG}: the hexagonal lattice: cells per repeat, the skew
'''


def hash3(src):
    # Dave Hoskins' hash without sine, three numbers in [0, 1) in r10 from src.xy (as src.xyx); r15 is a temporary
    return f'''\
        mul r10.xyz, {src}, c197
        frc r10.xyz, r10
        add r15.xyz, r10.yzxw, c197.w
        dp3 r15.x, r10, r15
        add r10.xyz, r10, r15.x
        add r15.xyz, r10.xxyw, r10.yzzw
        mul r15.xyz, r15, r10.zyxw
        frc r10.xyz, r15
'''


def hex_tiling(uv, col):
    return f'''\
      mul r8.z, r8.z, c179.x // {TAG}: and how evenly it tiles (texturequality.ixx)
      mul r8.w, r8.z, r4.w
      dsx r14.xy, {uv}.xyxy
      dsy r17.xy, {uv}.xyxy
      mov r11.xyz, {col}
      mul r9.w, r8.w, c166.z
      if_gt r9.w, c187.x
        // The lattice, skewed so that its cells are hexagons, and the triangle of three vertices around this pixel
        mul r9.xy, {uv}, c198.x
        mad r9.x, r9.y, c198.y, r9.x
        mul r9.y, r9.y, c198.z
        frc r10.xy, r9
        add r9.xy, r9, -r10
        add r10.z, r10.x, -r10.y
        add r12.xy, -r10, c187.y
        cmp r13.x, r10.z, r12.x, r12.y
        abs r13.y, r10.z
        cmp r13.z, r10.z, r10.y, r10.x
        cmp r12.xy, r10.z, c187.yxxx, c187.xyyy
        add r12.xy, r9, r12
        add r12.zw, r9.xyxy, c187.y
        mul r13.xyz, r13, r13
        mul r13.xyz, r13, r13 // weights to the fourth: sharper towards the nearest vertex
        dp3 r13.w, r13, c187.y
        rcp r13.w, r13.w
        mul r13.xyz, r13, r13.w
        // Each vertex's own random shift of the texture
{hash3('r9.xyxy')}\
        add r10.xy, {uv}, r10
        texldd r15, r10, s0, r14, r17
        mul r18.xyz, r15, r13.x
{hash3('r12.xyxy')}\
        add r10.xy, {uv}, r10
        texldd r15, r10, s0, r14, r17
        mad r18.xyz, r15, r13.y, r18
{hash3('r12.zwzw')}\
        add r10.xy, {uv}, r10
        texldd r15, r10, s0, r14, r17
        mad r18.xyz, r15, r13.z, r18
        // The texture's contrast kept: its average (r7) plus the blend's departure from it over sqrt(sum w^2)
        dp3 r13.w, r13, r13
        rsq r13.w, r13.w
        add r18.xyz, r18, -r7
        mad_sat r18.xyz, r18, r13.w, r7
        lrp r11.xyz, r9.w, r18, {col}
      endif'''


def body(path):
    return path.read_text().replace('\r\n', '\n')


def patch(path):
    text = body(path)
    if TAG in text:
        sys.exit(f'{path} is patched already')
    lines = text.split('\n')
    start = [i for i, l in enumerate(lines) if l.strip() == 'mul r6.xy, v5, c167.y']
    end = [i for i, l in enumerate(lines) if re.fullmatch(r'lrp r11\.xyz, r10\.z, r9, (r0|r1)', l.strip())]
    if len(start) != 1 or len(end) != 1 or end[0] < start[0]:
        sys.exit(f'{path}: no anti-tiling block from world_terrain_and_parallax.patch')
    block = lines[start[0]:end[0] + 1]
    col = lines[end[0]].strip().split(', ')[-1]
    uv = next(l.strip().split(', ')[1] for l in block if l.strip().startswith('mov r7.xy, '))
    natural = next(i for i, l in enumerate(block) if l.strip().startswith('mul_sat r8.z, r8.z, c184.z'))
    if block[natural + 1].strip() != 'mul r8.w, r8.z, r4.w':
        sys.exit(f'{path}: unexpected anti-tiling block')
    # Keep the average colour's read and the naturalness from the saturation; the second read, its patches of noise
    # and their blend go.
    kept = [l for l in block[3:natural + 1]]
    lines[start[0]:end[0] + 1] = kept + hex_tiling(uv, col).split('\n')
    text = '\n'.join(lines)
    defs = [i for i, l in enumerate(text.split('\n')) if l.strip().startswith('def c219,')]
    lines = text.split('\n')
    lines[defs[0] + 1:defs[0] + 1] = DEFS.rstrip('\n').split('\n')
    path.write_text('\n'.join(lines))


for name in SHADERS:
    for n in (3, 4):
        patch(ROOT / name / f'{name}PS{n}.asm')
