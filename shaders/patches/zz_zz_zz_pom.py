#!/usr/bin/env python3
# Writes zz_zz_zz_pom.patch from a FusionShaders checkout with every other patch applied; the patch is the diff it leaves.
#
# Two steps on the parallax occlusion mapping of gta_parallax* (height map) and of natural ground (the texture's brightness
# as the height), both under b15:
#  - Refine: past the march, two more reads between the last two steps pin down where the view hits the height, in place of
#    one straight line between them, which showed as layers at grazing angles.
#  - Depth: the pixel writes its depth at the hit, not on the flat polygon, so contact shadows, ambient occlusion, indirect
#    light and SSR see the relief and raised stones cover a ped's feet. Written as a share of the view depth, at most a
#    tenth. The height map's relief counts as kHeightMapMetres at full depth; natural ground's height is only the texture's
#    brightness, so it counts for half of its metres, or marks in its colour would turn into pits. c168.w: 1 on, 0 the flat
#    depth (ParallaxDepthWrite in texturequality.ixx).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / "win32_30_nv8"
TAG = "FusionFix POM"
HEIGHT_MAP_METRES = 0.04
NATURAL_SHARE = 0.5
REG = re.compile(r"^-?r(\d+)(?:_abs)?(?:\.([xyzw]+))?$")


def code(line):
    return line.split("//")[0].strip()


def components(text):
    """(register, component) pairs the code mentions, read or written."""
    used = set()
    for raw in text.split("\n"):
        line = code(raw)
        if not line or line.startswith(("def", "dcl")):
            continue
        for operand in re.split(r"[\s,]+", line)[1:]:
            match = REG.match(operand)
            if match:
                for comp in match.group(2) or "xyzw":
                    used.add((int(match.group(1)), comp))
    return used


def unused_component(text, avoid=()):
    used = components(text)
    for number in range(31, -1, -1):
        for comp in "wzyx":
            if (number, comp) not in used and (number, comp) not in avoid:
                return f"r{number}.{comp}", (number, comp)
    sys.exit("no unused component")


def free_constant(text):
    taken = set(int(c) for c in re.findall(r"\bc(\d+)", re.sub(r"//.*", "", text)))
    for number in range(150, 166):
        if number not in taken:
            return f"c{number}"
    sys.exit("no free constant")


def edit(rel, natural):
    path = ROOT / rel
    text = path.read_text()
    lines = text.split("\n")
    gap = [i for i, line in enumerate(lines) if code(line) == "add r29.x, r28.w, -r28.y"]
    rep = [i for i, line in enumerate(lines) if code(line) == "rep i1"]
    log = [i for i, line in enumerate(lines) if code(line) == "mul r19.x, v9.w, c209.x"]
    if len(gap) != 1 or len(rep) != 1 or len(log) != 1:
        sys.exit(f"{rel}: {len(gap)} {len(rep)} {len(log)}")
    end = next(i for i in range(rep[0], gap[0]) if code(lines[i]) == "endrep")
    body = lines[rep[0] + 1:end]
    first = next(i for i, line in enumerate(body) if code(line) == "min r28.x, r28.x, c187.y") + 1
    last = next(i for i, line in enumerate(body) if code(line).startswith("break_le"))
    hit, avoid = unused_component(text)
    t, _ = unused_component(text, (avoid,))
    copied = []
    for line in body[first:last]:
        new = re.sub(r"\br28\.x\b", t, line)
        new = new.replace("add r28.y, r29.x, -r29.w", "add r29.z, r29.x, -r29.w")
        copied.append(new)
    refine = [
        f"      // --- {TAG}: two more reads between the last two steps",
        "      if_le r28.y, c187.x",
        "        rep i2",
        "          add r29.x, r28.w, -r28.y",
        "          rcp r29.x, r29.x",
        "          mul r29.x, r28.w, r29.x",
        "          add r29.y, r28.x, -r28.z",
        f"          mad {t}, r29.x, r29.y, r28.z",
        f"          mov_sat {t}, {t}",
    ] + ["    " + line for line in copied] + [
        f"          cmp r28.z, r29.z, {t}, r28.z",
        "          cmp r28.w, r29.z, r29.z, r28.w",
        f"          cmp r28.x, r29.z, r28.x, {t}",
        "          cmp r28.y, r29.z, r28.y, r29.z",
        "        endrep",
        "      endif",
    ]
    hit_line = next(i for i in range(gap[0], len(lines)) if code(lines[i]).startswith("mad r29.y, r29.x, r29.y, r28.z"))
    constant = free_constant(text)
    if natural:
        # r11 still holds the way to the camera, r12.y one over the view's cosine, r8.w the depth in metres.
        depth = [
            f"        dp3 {hit}, r11, r11",
            f"        rsq {hit}, {hit}",
            f"        mul {hit}, {hit}, r29.y",
            f"        mul {hit}, {hit}, r8.w",
            f"        mul {hit}, {hit}, r12.y",
            f"        mul {hit}, {hit}, {constant}.x",
        ]
        metres = NATURAL_SHARE
    else:
        # v5 the way to the camera along the surface's frame: depth over its z is the share of the distance.
        depth = [
            f"      rcp {hit}, v5_abs.z",
            f"      mul {hit}, {hit}, r29.y",
            f"      mul {hit}, {hit}, {constant}.x",
        ]
        metres = HEIGHT_MAP_METRES
    indent = "        " if natural else "      "
    depth = [f"{indent}// --- {TAG}: the hit's depth, as a share of the view depth"] + depth + [
        f"{indent}mul {hit}, {hit}, c168.w",
        f"{indent}min {hit}, {hit}, {constant}.y",
        f"{indent}add {hit}, {hit}, c187.y",
    ]
    lines[log[0] + 1:log[0] + 1] = [f"    mul r19.x, r19.x, {hit} // {TAG}"]
    lines[hit_line + 1:hit_line + 1] = depth
    lines[gap[0]:gap[0]] = refine
    dcl = next(i for i, line in enumerate(lines) if line.strip().startswith("dcl_"))
    lines[dcl:dcl] = [
        f"    def {constant}, {metres}, 0.100000001, 0, 0 // {TAG}: the relief's metres or share of them, the most depth in a share of the view depth",
        f"    defi i2, 2, 0, 0, 0 // {TAG}: the refining reads",
    ]
    main = next(i for i in range(dcl + 2, len(lines)) if code(lines[i]) and not code(lines[i]).startswith(("def", "dcl")))
    lines[main:main] = [f"    mov {hit}, c187.y // {TAG}: no depth to add"]
    path.write_text("\n".join(lines))


for shader in ("gta_parallax", "gta_parallax_specmap", "gta_parallax_steep"):
    for n in (3, 4):
        edit(f"{shader}/{shader}PS{n}.asm", False)
for shader in ("gta_default", "gta_spec", "rage_default"):
    for n in (3, 4):
        edit(f"{shader}/{shader}PS{n}.asm", True)
