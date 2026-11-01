#!/usr/bin/env python3
"""Compile pinned AMD kernels and emit FfxShaderBlob tables from DXC reflection.
No precompiled input or external generator is used. Run twice with --verify to
compare a fresh compilation against the checked build output.
"""
import argparse, concurrent.futures, hashlib, json, pathlib, re, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
SDK = ROOT / 'external/fidelityfx-src'
FI = ['reconstruct_and_dilate','setup','reconstruct_previous_depth','game_motion_vector_field','optical_flow_vector_field','disocclusion_mask','','compute_inpainting_pyramid','inpainting','compute_game_vector_field_inpainting_pyramid','debug_view']
OF = ['prepare_luma','compute_luminance_pyramid','generate_scd_histogram','compute_scd_divergence','compute_optical_flow_advanced_pass_v5','filter_optical_flow_pass_v5','scale_optical_flow_advanced_pass_v5']
# These two official filenames contain "pass_v5", rather than "v5_pass".
def filename(kind, name):
    if name.endswith('_pass_v5'): return f'ffx_{kind}_{name}.hlsl'
    return f'ffx_{kind}_{name + "_" if name else ""}pass.hlsl'

def compile_one(job, args, scratch):
    kind, pi, opt, src = job
    wave = bool(opt & (8 if kind == 'frameinterpolation' else 1))
    half = bool(opt & (16 if kind == 'frameinterpolation' else 2))
    key = f'{kind}_{pi}_{opt}'
    binary = scratch / (key + '.dxil'); listing = scratch / (key + '.txt')
    defines = ['FFX_GPU=1','FFX_HLSL=1',f'FFX_HALF={int(half)}']
    if wave: defines += ['FFX_PREFER_WAVE64=[WaveSize(64)]']
    if kind == 'frameinterpolation':
        defines += [f'FFX_FRAMEINTERPOLATION_OPTION_{n}={int(bool(opt & b))}' for n,b in [('LOW_RES_MOTION_VECTORS',1),('JITTER_MOTION_VECTORS',2),('INVERTED_DEPTH',4)]]
    cmd = [args.dxc,'-T','cs_6_6' if wave else 'cs_6_2','-E','CS','-O3','-HV','2021','-Fo',str(binary),'-Fc',str(listing)]
    if half: cmd += ['-enable-16bit-types']
    for d in defines: cmd += ['-D',d]
    for inc in [SDK/'api/internal/gpu',SDK/'framegeneration/fsr3/include/gpu',ROOT/'source/frameinterpolation']: cmd += ['-I',str(inc)]
    cmd += [str(src)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode: raise RuntimeError(' '.join(cmd)+'\n'+result.stderr)
    b = binary.read_bytes(); reflection = listing.read_text()
    groups = [[] for _ in range(7)]
    table = reflection.split('; Resource Bindings:')[-1].split('; ViewId state:')[0]
    for line in table.splitlines():
        cols = line.removeprefix(';').split()
        if len(cols) < 7 or cols[0] in ('Name','-------'): continue
        m = re.fullmatch(r'(cb|[tus])(\d+)(?:,space(\d+))?', cols[-2])
        if not m: continue
        prefix, slot, space = m.groups(); typ, dim = cols[1], cols[3]
        group = 0 if prefix == 'cb' else 5 if prefix == 's' else (2 if prefix == 'u' else 1) if dim not in ('buf','r/o','r/w') else (4 if prefix == 'u' else 3)
        groups[group].append((cols[0],int(slot),int(cols[-1]),int(space or 0)))
    if not any(groups): raise RuntimeError('Missing reflection: '+key)
    return kind,pi,opt,b,groups

def emit(records):
    out=['// Generated from AMD FidelityFX SDK 2.3.0; AMD MIT notices are distributed alongside the DLL.','#pragma once']
    blobs={}; tables={'frameinterpolation':{},'opticalflow':{}}
    for kind,pi,opt,b,groups in records:
        digest=hashlib.sha256(b).hexdigest(); symbol='s_'+digest[:20]
        if digest not in blobs:
            blobs[digest]=symbol
            out += [f'static const uint8_t {symbol}[] = {{']
            out += [','.join(map(str,b[i:i+32]))+',' for i in range(0,len(b),32)]
            out += ['};']
            fields=[]
            for gi,group in enumerate(groups):
                if not group: fields += ['nullptr']*4; continue
                for fi,ctype in enumerate(['const char*','uint32_t','uint32_t','uint32_t']):
                    name=f'{symbol}_{gi}_{fi}'; vals=','.join(json.dumps(row[fi]) for row in group)
                    out += [f'static {ctype} {name}[] = {{{vals}}};']; fields.append(name)
            out += [f'static const FfxShaderBlob {symbol}_blob = {{'+','.join([symbol,str(len(b)),'"CS"']+[str(len(g)) for g in groups]+fields)+'};']
        tables[kind][pi,opt]=symbol+'_blob'
    for kind,table in tables.items():
        count=12 if kind=='frameinterpolation' else 7; permutations=32 if kind=='frameinterpolation' else 4
        out += [f'static const FfxShaderBlob own_{kind}[{count}][{permutations}] = {{']
        for pi in range(count): out += ['{'+','.join(table[pi,opt] for opt in range(permutations))+'},']
        out += ['};']
    return '\n'.join(out)+'\n'

def main():
    p=argparse.ArgumentParser();p.add_argument('--dxc',required=True);p.add_argument('--output',type=pathlib.Path,default=ROOT/'build/own-frameinterpolation');p.add_argument('--verify',action='store_true');p.add_argument('--jobs',type=int,default=8); a=p.parse_args()
    version=subprocess.check_output([a.dxc,'--version'],text=True)
    if '1.8.2502' not in version or 'b4711839' not in version: raise SystemExit('Expected pinned DXC v1.8.2502 (commit b4711839); got '+version)
    jobs=[]
    for kind,names in [('frameinterpolation',FI),('opticalflow',OF)]:
        for pi,name in enumerate(names):
            for opt in range(32 if kind=='frameinterpolation' else 4): jobs.append((kind,pi,opt,SDK/'framegeneration/fsr3/internal/shaders'/filename(kind,name)))
    for opt in range(32): jobs.append(('frameinterpolation',11,opt,ROOT/'source/frameinterpolation/fused.hlsl'))
    with tempfile.TemporaryDirectory() as td:
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool: records=list(pool.map(lambda job:compile_one(job,a,pathlib.Path(td)),jobs))
    data=emit(records); target=a.output/'own_shader_blobs.h'
    manifest={'compiler':version.strip(),'compiler_sha256':hashlib.sha256(pathlib.Path(a.dxc).read_bytes()).hexdigest(),'blob_header_sha256':hashlib.sha256(data.encode()).hexdigest(),'permutations':len(records),'containers':len({hashlib.sha256(r[3]).hexdigest() for r in records})}
    if a.verify:
        if target.read_bytes()!=data.encode('utf-8'): raise SystemExit('Shader regeneration differs')
    else:
        a.output.mkdir(parents=True,exist_ok=True);target.write_bytes(data.encode('utf-8'));(a.output/'shaders.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))
if __name__=='__main__': main()
