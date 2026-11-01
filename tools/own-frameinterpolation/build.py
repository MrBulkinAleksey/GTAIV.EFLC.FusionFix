#!/usr/bin/env python3
"""Build the standalone x64 DLL using the MSVC developer prompt or Zig cross C++."""
import argparse, os, pathlib, subprocess, sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
SOURCES=['source/frameinterpolation/own_frameinterpolation.cpp','source/frameinterpolation/shader_lookup.cpp','external/fidelityfx-src/backend/dx12/ffx_dx12.cpp','external/fidelityfx-src/api/internal/ffx_assert.cpp','external/fidelityfx-src/api/internal/ffx_message.cpp','external/fidelityfx-src/api/internal/ffx_object_management.cpp','external/fidelityfx-src/framegeneration/fsr3/internal/ffx_frameinterpolation.cpp','external/fidelityfx-src/framegeneration/fsr3/internal/ffx_opticalflow.cpp']
DEFINES=['OWN_FI_BUILD','OWN_FI_NO_PIX','_WINDOWS','WIN32_LEAN_AND_MEAN','NOMINMAX','NDEBUG','_CRT_SECURE_NO_WARNINGS']
def main():
 p=argparse.ArgumentParser();p.add_argument('--dxc',default=os.environ.get('OWN_FI_DXC'),required=not bool(os.environ.get('OWN_FI_DXC')));p.add_argument('--zig',action='store_true');p.add_argument('--output',type=pathlib.Path,default=ROOT/'bin');a=p.parse_args()
 subprocess.run([sys.executable,str(ROOT/'tools/own-frameinterpolation/verify_sources.py')],cwd=ROOT,check=True)
 subprocess.run([sys.executable,str(ROOT/'tools/own-frameinterpolation/compile_shaders.py'),'--dxc',a.dxc],cwd=ROOT,check=True)
 a.output.mkdir(parents=True,exist_ok=True);out=a.output/'GTAIV.EFLC.FusionFix.FrameInterpolation.dll'
 if a.zig:
  cmd=[sys.executable,'-m','ziglang','c++','-target','x86_64-windows-gnu','-std=c++20','-O2','-shared']+['-D'+d for d in DEFINES]+['-Ibuild/own-frameinterpolation']+SOURCES+['-ld3d12','-ldxgi','-ldxguid','-o',str(out)]
 else:
  cmd=['cl','/nologo','/std:c++20','/EHsc','/MT','/O2','/LD','/utf-8']+['/D'+d for d in DEFINES]+['/Ibuild/own-frameinterpolation','/Fobuild/own-frameinterpolation/']+SOURCES+['/link','d3d12.lib','dxgi.lib','dxguid.lib','/OUT:'+str(out)]
 subprocess.run(cmd,cwd=ROOT,check=True)
 print(out)
if __name__=='__main__':main()
