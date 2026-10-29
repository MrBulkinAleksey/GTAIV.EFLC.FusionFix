#!/usr/bin/env python3
# Writes world_wd_local_shadow_depth.patch: wd_local_draw / wd_local_masked_draw techniques for the static world
# shaders, so the map's buildings can be drawn into the dynamic (paraboloid) shadow maps of lights (Building Shadows,
# source/nightshadows/LibertyLighting20Runtime.inl). Ported from Liberty's Shadows 2.0 (OnyxOak), GPL-3.0, whose generator works
# on compiled .fxc files; this one works on the submodule's asm and XML.
#
# Each new technique keeps its material's wd_draw / wd_masked_draw pixel shader (texture alpha, stochastic cutout) and
# render states, and takes gta_vehicle_basic's local shadow vertex shader (VS9, the same projection the game uses for
# cars in these maps) with the texture coordinates passed on. The pixel shader writes the local linear depth and drops
# what is in front of the near plane, as the vehicle's PS9 does, instead of the sun's depth.
#
# Usage: world_wd_local_shadow_depth.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git add -A && git diff --cached).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8'
SHADERS = re.compile(r'^gta_(default|normal|spec|emissive|reflect|parallax|cutout_fence)')
TAG = 'FusionFix: world_wd_local_shadow_depth.patch'
VS_VARIABLES = [('float4', 'gShadowParam0123', 57), ('float4', 'gShadowParam891113', 59), ('float4x4', 'gShadowMatrix', 60),
                ('float2', 'facetMask', 208), ('float4x4', 'gWorld', 0), ('float4', 'gShadowParam14151617', 56)]


def body(path):
    return path.read_text().replace('\r\n', '\n')


def donor_vertex_shader():
    text = body(ROOT / 'gta_vehicle_basic' / 'gta_vehicle_basicVS9.asm')
    code = text[text.index('vs_3_0'):]
    lines = [line.strip() for line in code[:code.index('// approximately')].strip().split('\n')]
    out = []
    for line in lines:
        out.append(line)
        if line == 'dcl_position v0':
            out.append('dcl_texcoord v1')
        elif line == 'dcl_texcoord1 o1':
            out.append('dcl_texcoord o2.xyz')
    out += ['mov o2.x, r4.z // local depth', 'mov o2.yz, v1.xxyw // texture coordinates']
    if len(out) != len(lines) + 4 or 'r4' not in code:
        raise ValueError('donor vertex shader changed')
    # if/else blocks keep their indentation
    indented, depth = [], 1
    for line in out:
        if line.startswith(('else', 'endif')):
            depth -= 1
        indented.append('    ' * depth + line)
        if line.startswith(('if', 'else')):
            depth += 1
    return ('// ' + TAG + '\n// gta_vehicle_basic VS9 (local shadow depth) with the texture coordinates in o2.yz\n//\n' +
            '// Parameters:\n//\n' + ''.join(f'//   {t} {n};\n' for t, n, _ in VS_VARIABLES) + '//\n\n' + '\n'.join(indented) + '\n')


def pixel_shader(text, source):
    code = text.replace('\r\n', '\n')
    if re.search(r'\br24\b|\bc214\b|\bc56\b', code):
        raise ValueError(f'{source} already uses r24, c214 or c56')
    if len(re.findall(r'dcl_texcoord v0\.xyz\n', code)) != 1 or re.search(r'dcl_\w+ v1\b', code):
        raise ValueError(f'{source}: unknown interpolators')
    outputs = re.search(r'([ \t]*)mov oC0\.xyz, v0\.x\n[ \t]*mov oC0\.w, r0\.x\n', code)
    if not outputs or len(re.findall(r'mov oC0', code)) != 2:
        raise ValueError(f'{source}: unknown depth output')
    rest = code.replace(outputs.group(0), '')
    if re.search(r'\bv0\.x\b(?![yzw])', rest):
        raise ValueError(f'{source}: alpha depends on the sun depth')
    i = outputs.group(1)
    code = code.replace(outputs.group(0),
        f'{i}abs r24.x, c56.x\n{i}cmp r24.x, -r24.x, c214.y, c214.z\n{i}add r24.y, -v1.z, c214.x\n'
        f'{i}mul r24, r24.x, r24.y\n{i}texkill r24 // {TAG}: local depth, nothing before the near plane\n{i}mov oC0, v1.w\n')
    code = re.sub(r'([ \t]*)dcl_texcoord v0\.xyz\n', lambda m: f'{m.group(1)}dcl_texcoord v0.xyz\n{m.group(1)}dcl_texcoord1 v1.zw\n', code, count=1)
    code = re.sub(r'([ \t]*)ps_3_0\n', lambda m: f'{m.group(1)}ps_3_0\n{m.group(1)}def c214, 0.01, 1, 0, 0 // {TAG}\n', code, count=1)
    return f'// {TAG}: {source} writing the local linear depth of the paraboloid shadow maps\n' + code


def items(section):
    # Top-level <Item> blocks of a section's text, as (start, end) offsets.
    result, depth, start = [], 0, None
    for m in re.finditer(r'<Item>|</Item>', section):
        if m.group(0) == '<Item>':
            if depth == 0:
                start = m.start()
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                result.append((start, m.end()))
    return result


def section(xml, name):
    m = re.search(rf'<{name} value="(\d+)" >\n(.*?)\n(\t*)</{name}>', xml, re.S)
    if not m:
        raise ValueError('no ' + name)
    return m


def variables_xml(variables, indent):
    out = f'{indent}<Variables value="{len(variables)}" >\n'
    for t, n, index in variables:
        out += f'{indent}\t<Item>\n{indent}\t\t<Type>{t}</Type>\n{indent}\t\t<Name>{n}</Name>\n{indent}\t\t<Index value="{index}" />\n{indent}\t</Item>\n'
    return out + f'{indent}</Variables>\n'


def build(xml_path, vs_text):
    name = xml_path.name[:-len('.fxc.xml')]
    xml = body(xml_path)
    if 'wd_local' in xml:
        raise ValueError('already has wd_local')
    declared = xml[xml.index('<GlobalVariables'):xml.index('<Techniques')]
    for _, n, _ in VS_VARIABLES:
        if f'<Name1>{n}</Name1>' not in declared:
            raise ValueError('effect does not declare ' + n)
    techniques = section(xml, 'Techniques')
    tech_text = techniques.group(2)
    by_name = {}
    for s, e in items(tech_text):
        block = tech_text[s:e]
        by_name[re.search(r'<Name>(.*?)</Name>', block).group(1)] = block
    if 'wd_draw' not in by_name:
        raise ValueError('no wd_draw')
    pixel = section(xml, 'PixelShaders')
    pixel_items = items(pixel.group(2))
    vertex = section(xml, 'VertexShaders')
    vertex_items = items(vertex.group(2))

    new_vs_index = len(vertex_items)
    vs_file = f'{name}VS{new_vs_index}.asm'
    vs_item_indent = '\t\t\t'
    new_vertex = (f'\n{vs_item_indent}<Item>\n{vs_item_indent}\t<DataType>asm</DataType>\n{vs_item_indent}\t<File>{name}\\{vs_file}</File>\n' +
                  variables_xml(VS_VARIABLES, vs_item_indent + '\t') + f'{vs_item_indent}</Item>')
    files = {xml_path.parent / name / vs_file: vs_text}
    new_pixels, new_techniques = '', ''
    next_ps = len(pixel_items) + 1
    for wd in ('wd_draw', 'wd_masked_draw'):
        if wd not in by_name:
            continue
        block = by_name[wd]
        if block.count('<VertexShader>') != 1:
            raise ValueError(wd + ' has more than one pass')
        vs_index = int(re.search(r'<VertexShader>(\d+)</VertexShader>', block).group(1))
        ps_index = int(re.search(r'<PixelShader>(\d+)</PixelShader>', block).group(1))
        authored_vs = body(xml_path.parent / name / f'{name}VS{vs_index}.asm')
        inputs = re.findall(r'dcl_\S+ v\d.*', authored_vs)
        if inputs != ['dcl_position v0', 'dcl_texcoord v1']:
            raise ValueError(f'{wd}: vertex inputs {inputs}')
        if not re.search(r'//\s+gWorld\s+c0\s', authored_vs):
            raise ValueError(f'{wd}: no gWorld at c0')
        ps_item = pixel.group(2)[pixel_items[ps_index - 1][0]:pixel_items[ps_index - 1][1]]
        ps_source = re.search(r'<File>.*?\\(.*?)</File>', ps_item).group(1)
        ps_text = pixel_shader(body(xml_path.parent / name / ps_source), ps_source)
        ps_file = f'{name}PS{next_ps}.asm'
        files[xml_path.parent / name / ps_file] = ps_text
        ps_variables = re.findall(r'<Type>(.*?)</Type>\s*<Name>(.*?)</Name>\s*<Index value="(\d+)" />', ps_item)
        ps_variables = [(t, n, int(i)) for t, n, i in ps_variables] + [('float4', 'gShadowParam14151617', 56)]
        new_pixels += (f'\n{vs_item_indent}<Item>\n{vs_item_indent}\t<DataType>asm</DataType>\n{vs_item_indent}\t<File>{name}\\{ps_file}</File>\n' +
                       variables_xml(ps_variables, vs_item_indent + '\t') + f'{vs_item_indent}</Item>')
        local = block.replace(f'<Name>{wd}</Name>', '<Name>' + wd.replace('wd_', 'wd_local_', 1) + '</Name>')
        local = local.replace(f'<VertexShader>{vs_index}</VertexShader>', f'<VertexShader>{new_vs_index}</VertexShader>')
        local = local.replace(f'<PixelShader>{ps_index}</PixelShader>', f'<PixelShader>{next_ps}</PixelShader>')
        new_techniques += '\n\t\t' + local
        next_ps += 1

    added_ps = next_ps - 1 - len(pixel_items)
    added_tech = new_techniques.count('<Name>')
    # Later sections first, so the earlier offsets stay valid.
    t = section(xml, 'Techniques')
    xml = xml[:t.end(2)] + new_techniques + xml[t.end(2):]
    xml = xml[:t.start(1)] + str(int(t.group(1)) + added_tech) + xml[t.end(1):]
    p = section(xml, 'PixelShaders')
    xml = xml[:p.end(2)] + new_pixels + xml[p.end(2):]
    xml = xml[:p.start(1)] + str(int(p.group(1)) + added_ps) + xml[p.end(1):]
    v = section(xml, 'VertexShaders')
    xml = xml[:v.end(2)] + new_vertex + xml[v.end(2):]
    xml = xml[:v.start(1)] + str(int(v.group(1)) + 1) + xml[v.end(1):]
    files[xml_path] = xml
    return files, added_tech


def main():
    vs_text = donor_vertex_shader()
    done, refused = [], []
    for xml_path in sorted(ROOT.glob('gta_*.fxc.xml')):
        name = xml_path.name[:-len('.fxc.xml')]
        if not SHADERS.match(name):
            continue
        try:
            files, techniques = build(xml_path, vs_text)
        except ValueError as error:
            refused.append(f'{name}: {error}')
            continue
        for path, text in files.items():
            path.write_text(text)
        done.append(f'{name} ({techniques})')
    print('patched', len(done), ', '.join(done))
    print('refused', len(refused))
    for r in refused:
        print('  ' + r)


if __name__ == '__main__':
    main()
