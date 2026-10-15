#!/usr/bin/env python3
# Writes world_wd_local_shadow_skinned.patch, on top of world_wd_local_shadow_depth.patch:
#
# - wd_local_drawskinned / wd_local_masked_drawskinned for the world shaders that have wd_local_draw, so skinned world
#   models (fragments: lamp posts, signs, hydrants) can be drawn into the dynamic (paraboloid) shadow maps of lights
#   (Prop Shadows, source/nightshadows/LibertyLighting20Runtime.inl). The vertex shader is gta_vehicle_basic's skinned local shadow
#   one (VS10, as VS9 is for wd_local_draw) with the texture coordinates passed on; the pixel shaders are the ones
#   wd_local_draw / wd_local_masked_draw already use.
# - Those pixel shaders drop what is nearer to the light than NEAR metres, so a lamp's own head and glass don't cover
#   its light while the post below still casts. The map stores distance * -gShadowParam0123.w (VS9/VS10 r3.z).
#
# Usage: world_wd_local_shadow_skinned.py <FusionShaders checkout with the other patches applied>
# Commit that state first; the patch is the diff this script leaves (git add -A && git diff --cached).
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) / 'win32_30_nv8'
TAG = 'FusionFix: world_wd_local_shadow_skinned.patch'
NEAR = 0.5
VS_VARIABLES = [('float2', 'facetMask', 208), ('float4x3', 'gBoneMtx', 64), ('float4x4', 'gShadowMatrix', 60),
                ('float4', 'gShadowParam0123', 57), ('float4', 'gShadowParam14151617', 56),
                ('float4', 'gShadowParam891113', 59), ('float4x4', 'gWorld', 0)]


def body(path):
    return path.read_text().replace('\r\n', '\n')


def donor_vertex_shader():
    text = body(ROOT / 'gta_vehicle_basic' / 'gta_vehicle_basicVS10.asm')
    code = text[text.index('vs_3_0'):]
    lines = [line.strip() for line in code[:code.index('// approximately')].strip().split('\n')]
    out = []
    for line in lines:
        out.append(line)
        if line == 'dcl_blendindices v2':
            out.append('dcl_texcoord v3')
        elif line == 'dcl_texcoord1 o1':
            out.append('dcl_texcoord o2.xyz')
    out += ['mov o2.x, r4.z // local depth', 'mov o2.yz, v3.xxyw // texture coordinates']
    if len(out) != len(lines) + 4 or 'r4' not in code or 'v3' in code:
        raise ValueError('donor vertex shader changed')
    indented, depth = [], 1
    for line in out:
        if line.startswith(('else', 'endif')):
            depth -= 1
        indented.append('    ' * depth + line)
        if line.startswith(('if', 'else')):
            depth += 1
    return ('// ' + TAG + '\n// gta_vehicle_basic VS10 (skinned local shadow depth) with the texture coordinates in o2.yz\n//\n' +
            '// Parameters:\n//\n' + ''.join(f'//   {t} {n};\n' for t, n, _ in VS_VARIABLES) + '//\n\n' + '\n'.join(indented) + '\n')


KILL = re.compile(r'([ \t]*)add r24\.y, -v1\.z, c214\.x\n[ \t]*mul r24, r24\.x, r24\.y\n')


def near_cut(text, source):
    code = text.replace('\r\n', '\n')
    if re.search(r'\bc57\b', code) or len(KILL.findall(code)) != 1 or code.count('def c214, 0.01, 1, 0, 0 ') != 1:
        raise ValueError(f'{source}: unknown local depth pixel shader')
    code = code.replace('def c214, 0.01, 1, 0, 0 ', f'def c214, 0.01, 1, 0, {NEAR} ')
    code = KILL.sub(lambda m: f'{m.group(1)}add r24.y, -v1.z, c214.x\n'
                    f'{m.group(1)}mov r24.w, c214.w\n'
                    f'{m.group(1)}mad r24.z, c57.w, r24.w, v1.w // {TAG}: nothing nearer to the light than c214.w metres\n'
                    f'{m.group(1)}mul r24, r24.x, r24.yzzz\n', code)
    return code


def items(section):
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
    if 'wd_local_draw' not in xml or 'wd_local_drawskinned' in xml:
        raise ValueError('no wd_local_draw or already skinned')
    techniques = section(xml, 'Techniques')
    by_name = {}
    for s, e in items(techniques.group(2)):
        block = techniques.group(2)[s:e]
        by_name[re.search(r'<Name>(.*?)</Name>', block).group(1)] = block
    vertex_items = items(section(xml, 'VertexShaders').group(2))
    new_vs_index = len(vertex_items)
    vs_file = f'{name}VS{new_vs_index}.asm'
    files = {xml_path.parent / name / vs_file: vs_text}

    # The local pixel shaders get the near cut and gShadowParam0123
    pixel = section(xml, 'PixelShaders')
    pixel_text = pixel.group(2)
    local_ps = sorted({int(re.search(r'<PixelShader>(\d+)</PixelShader>', by_name[t]).group(1))
                       for t in ('wd_local_draw', 'wd_local_masked_draw') if t in by_name}, reverse=True)
    pixel_items = items(pixel_text)
    for ps_index in local_ps:
        s, e = pixel_items[ps_index - 1]
        ps_item = pixel_text[s:e]
        ps_source = re.search(r'<File>.*?\\(.*?)</File>', ps_item).group(1)
        files[xml_path.parent / name / ps_source] = near_cut(body(xml_path.parent / name / ps_source), ps_source)
        count = re.search(r'<Variables value="(\d+)" >\n', ps_item)
        indent = re.search(r'\n(\t*)<Item>', ps_item[count.end() - 1:]).group(1)
        variable = f'{indent}<Item>\n{indent}\t<Type>float4</Type>\n{indent}\t<Name>gShadowParam0123</Name>\n{indent}\t<Index value="57" />\n{indent}</Item>\n'
        ps_item = (ps_item[:count.start(1)] + str(int(count.group(1)) + 1) + ps_item[count.end(1):count.end()] + variable +
                   ps_item[count.end():])
        pixel_text = pixel_text[:s] + ps_item + pixel_text[e:]

    new_techniques = ''
    for wd, local in (('wd_drawskinned', 'wd_local_draw'), ('wd_masked_drawskinned', 'wd_local_masked_draw')):
        if wd not in by_name or local not in by_name:
            continue
        block = by_name[wd]
        if block.count('<VertexShader>') != 1:
            raise ValueError(wd + ' has more than one pass')
        vs_index = int(re.search(r'<VertexShader>(\d+)</VertexShader>', block).group(1))
        ps_index = int(re.search(r'<PixelShader>(\d+)</PixelShader>', block).group(1))
        inputs = re.findall(r'dcl_\S+ v\d.*', body(xml_path.parent / name / f'{name}VS{vs_index}.asm'))
        if inputs != ['dcl_position v0', 'dcl_blendweight v1', 'dcl_blendindices v2', 'dcl_texcoord v3']:
            raise ValueError(f'{wd}: vertex inputs {inputs}')
        local_ps_index = int(re.search(r'<PixelShader>(\d+)</PixelShader>', by_name[local]).group(1))
        tech = block.replace(f'<Name>{wd}</Name>', '<Name>' + wd.replace('wd_', 'wd_local_', 1) + '</Name>')
        tech = tech.replace(f'<VertexShader>{vs_index}</VertexShader>', f'<VertexShader>{new_vs_index}</VertexShader>')
        tech = tech.replace(f'<PixelShader>{ps_index}</PixelShader>', f'<PixelShader>{local_ps_index}</PixelShader>')
        new_techniques += '\n\t\t' + tech
    if not new_techniques:
        raise ValueError('no wd_drawskinned')

    added_tech = new_techniques.count('<Name>')
    t = section(xml, 'Techniques')
    xml = xml[:t.end(2)] + new_techniques + xml[t.end(2):]
    xml = xml[:t.start(1)] + str(int(t.group(1)) + added_tech) + xml[t.end(1):]
    p = section(xml, 'PixelShaders')
    xml = xml[:p.start(2)] + pixel_text + xml[p.end(2):]
    v = section(xml, 'VertexShaders')
    vs_item_indent = '\t\t\t'
    new_vertex = (f'\n{vs_item_indent}<Item>\n{vs_item_indent}\t<DataType>asm</DataType>\n{vs_item_indent}\t<File>{name}\\{vs_file}</File>\n' +
                  variables_xml(VS_VARIABLES, vs_item_indent + '\t') + f'{vs_item_indent}</Item>')
    xml = xml[:v.end(2)] + new_vertex + xml[v.end(2):]
    xml = xml[:v.start(1)] + str(int(v.group(1)) + 1) + xml[v.end(1):]
    files[xml_path] = xml
    return files, added_tech


def main():
    vs_text = donor_vertex_shader()
    done, refused = [], []
    for xml_path in sorted(ROOT.glob('gta_*.fxc.xml')):
        if 'wd_local_draw' not in body(xml_path) or xml_path.name.startswith('gta_vehicle'):
            continue
        name = xml_path.name[:-len('.fxc.xml')]
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
