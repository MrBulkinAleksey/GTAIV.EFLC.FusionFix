#!/usr/bin/env python3
# Writes zz_zz_zz_light_response.patch, from a FusionShaders checkout
# with every other patch applied (buildshaders.bat applies zz_z_shape_distance_softness after it; they touch different lines):
#
#  - Even Lamp Light (LampAlbedoResponse, c194 set in postfx.ixx): in each deferred_lighting light shader the albedo
#    that lights the diffuse term is scaled by 1 + response * (clamp(reference / luminance, 0.25, 4) - 1), so dark
#    surfaces such as asphalt return more of a lamp's light and bright ones less. The hue stays. Specular is not touched.
#    c194 all zero (off) gives a scale of 1.
#
#  - Soft Highlights (SoftHighlights, c195 set in shaders.ixx): in the composite shaders that carry the tone mapping,
#    before the exposed scene meets the bloom, a pixel's brightest channel past the knee rolls off towards a ceiling and
#    all three channels scale with it, so light adding up over white keeps its hue instead of clipping channel by channel.
#    The sky is left out: the sun disc and its halo stay as drawn. Bloom is added after, so it still sees the full light.
#    c195.y zero (off, HDR output) skips it.
#
# Usage: zz_zz_zz_light_response.py <FusionShaders checkout>; the patch is the diff it leaves (git diff).
import re
import sys
from pathlib import Path

root = Path(sys.argv[1]) / "win32_30_nv8"


def strip(line):
    return line.split("//")[0].strip()


def x_free(lines, start, reg):
    """reg.x holds nothing the code after start reads: the first later mention of its x is a plain write."""
    for raw in lines[start:]:
        line = strip(raw)
        if not line or line.startswith(("def", "dcl")):
            continue
        parts = re.split(r"[\s,]+", line)
        op, operands = parts[0], parts[1:]
        if not operands:
            continue
        reads = operands[1:] if not op.startswith(("if", "break", "call", "rep", "loop", "texkill")) else operands
        for read in reads:
            match = re.fullmatch(rf"-?{reg}(_abs)?(\.[xyzw]+)?", read)
            if match and (match.group(2) is None or "x" in match.group(2)):
                return False
        match = re.fullmatch(rf"{reg}(\.[xyzw]+)?", operands[0])
        if match and (match.group(1) is None or "x" in match.group(1)):
            return True
    return True


def temp(lines, start, avoid):
    for number in list(range(31, 11, -1)):
        reg = f"r{number}"
        if reg not in avoid and x_free(lines, start, reg):
            return reg
    raise SystemExit("no free temp")


def insert_defs(lines, text):
    for index, line in enumerate(lines):
        if line.strip().startswith("dcl_"):
            lines[index:index] = text
            return
    raise SystemExit("no dcl")


# Where each light shader has its diffuse albedo: (register, the line after which it is ready).
SITES = {
    6: ("r4", r"texld r4, r0, s0"),
    7: ("r2", r"texld r2, r0, s0"),
    8: ("r4", r"texld r4, r0, s0"),
    9: ("r2", r"texld r2, r0, s0"),
    10: ("r3", r"texld r3, r0, s0"),
    11: ("r2", r"texld r2, r0, s0"),
    12: ("r2", r"texld r2, r0, s0"),
    14: ("r4", r"texld r4, r0.zwzw, s0"),
    15: ("r0", r"texld r0, r0, s0"),
}
# PS16 and PS17 read a projected texture on s0: their albedo is r3, the diffuse scale goes right before it is used.
BEFORE = {16: ("r3", r"mul r2.xyz, r3, r6"), 17: ("r3", r"mul r2.xyz, r3, r6")}


def albedo(n):
    path = root / "deferred_lighting" / f"deferred_lightingPS{n}.asm"
    lines = path.read_text().split("\n")
    reg, pattern = SITES.get(n) or BEFORE[n]
    found = [i for i, line in enumerate(lines) if strip(line) == pattern]
    if len(found) != 1:
        raise SystemExit(f"PS{n}: {pattern}: {len(found)} matches")
    at = found[0] + (1 if n in SITES else 0)
    t = temp(lines, at, {reg})
    lines[at:at] = [
        "    // --- FusionFix Even Lamp Light (c194: response, reference, lowest scale): see zz_zz_zz_light_response.py",
        f"    dp3 {t}.x, {reg}, c191",
        f"    rcp {t}.x, {t}.x",
        f"    mul {t}.x, {t}.x, c194.y",
        f"    max {t}.x, {t}.x, c194.z",
        f"    min {t}.x, {t}.x, c191.w",
        f"    add {t}.x, {t}.x, c192.x",
        f"    mul {t}.x, {t}.x, c194.x",
        f"    add {t}.x, {t}.x, -c192.x",
        f"    mul {reg}.xyz, {reg}, {t}.x",
    ]
    insert_defs(lines, [
        "    def c191, 0.2126, 0.7152, 0.0722, 4 // FusionFix Even Lamp Light: luminance, the highest scale",
        "    def c192, -1, 0, 0, 0 // FusionFix Even Lamp Light",
    ])
    path.write_text("\n".join(lines))


def composite(n):
    path = root / "rage_postfx" / f"rage_postfxPS{n}.asm"
    lines = path.read_text().split("\n")
    target = "mul r0.xyz, r3, c66.x" if n == 28 else "mad r0.xyz, r3, c66.x, r1"
    found = [i for i, line in enumerate(lines) if strip(line) == target]
    if len(found) != 1:
        raise SystemExit(f"PS{n}: {target}: {len(found)} matches")
    if not any(strip(line).startswith("pow r19.x, c209_abs.z") for line in lines):
        raise SystemExit(f"PS{n}: no linear depth in r19.x")
    lines[found[0]:found[0]] = [
        "    // --- FusionFix Soft Highlights (c195: knee, knee to ceiling, -log2(e) / that, the sky's depth share):",
        "    // --- see zz_zz_zz_light_response.py. r19.x is the depth over the near plane, read for the depth of field.",
        "    if_ne -c195_abs.y, c195_abs.y",
        "      mul r9.xyz, r3, c66.x",
        "      max r10.x, r9.x, r9.y",
        "      max r10.x, r10.x, r9.z // the brightest channel, exposed",
        "      add r10.y, r10.x, -c195.x // past the knee",
        "      mul r10.z, r10.y, c195.z",
        "      exp r10.z, r10.z",
        "      add r10.z, -r10.z, c2.y",
        "      mad r10.z, r10.z, c195.y, c195.x // knee + range * (1 - exp(-past / range))",
        "      rcp r10.w, r10.x",
        "      mul r10.z, r10.z, r10.w",
        "      cmp r10.z, r10.y, r10.z, c2.y // under the knee as it is",
        "      mov r11.x, c209_abs.z",
        "      mad r10.y, r11.x, c195.w, r19.x",
        "      cmp r10.z, r10.y, c2.y, r10.z // the sky as it is",
        "      mul r3.xyz, r3, r10.z",
        "    endif",
    ]
    path.write_text("\n".join(lines))


for n in list(SITES) + list(BEFORE):
    albedo(n)
for n in (11, 12, 14, 24, 26, 28):
    composite(n)
