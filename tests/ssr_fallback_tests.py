#!/usr/bin/env python3
"""Exercise SSR.fx's actual fallback pass with synthetic hit/fill outcomes.

Requires a C++17 compiler; CXX may include compiler arguments.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

shader = (Path(__file__).resolve().parents[1] / 'source/resources/SSR.fx').read_text()
start = shader.index('float4 SSRFallback_PS(')
end = shader.index('\n// One step of a cascade', start)
body = re.sub(r'\[branch\]', '', shader[start:end])
body = body.replace(' : TEXCOORD0', '').replace(' : VPOS', '').replace(' : COLOR0', '')
prefix = r'''
#include <cmath>
#include <cstdio>
#include <cstdlib>
struct float2 { float x=0,y=0; };
struct float3 { float z; float3(float v=0):z(v){} };
struct float4 { float a; float4(float v=0):a(v){} float4(float2,int,int):a(0){} float4(float3,float w):a(w){} };
float4 tex2Dlod(int,float4) { return float4(0); }
bool NoSurface(float4) { return false; }
float kFallbackBelow=.1f, fFallback, fNeighbourFill; int SSRResultTex;
int surfaces, normalSamples, rays, screens, fills, depthSamples; float fillDepth, screenWeight;
float LinearDepth(float2) { ++depthSamples; return 7; }
float3 SSRSurface(float2,float2,float3&) { ++surfaces; return float3(7); }
float3 SSRMicroNormal(float2,float2,float3,float3) { ++normalSamples; return float3(1); }
struct ReflectionRay {};
bool SetupReflectionRay(float3,float3,float3,ReflectionRay&) { ++rays; return true; }
float3 normalize(float3 a) { return a; }
float3 reflect(float3 a,float3) { return a; }
float3 ScreenFallback(float3,float3,float& w) { ++screens; w=screenWeight; return float3(1); }
float3 NeighbourFill(float2,float2,float z,float& w) { ++fills; fillDepth=z; w=.4f; return float3(1); }
void reset() { surfaces=normalSamples=rays=screens=fills=depthSamples=0; fillDepth=0; }
'''
suffix = r'''
void require(bool ok,const char* msg) { if (!ok) { std::fprintf(stderr,"%s\n",msg); std::exit(1); } }
int main() {
    float2 uv;
    fFallback=0; fNeighbourFill=1; screenWeight=0; reset();
    float4 result=SSRFallback_PS(uv,uv);
    require(result.a==.4f && fills==1 && fillDepth==7,"neighbour-only fill changed");
    require(surfaces==0 && normalSamples==0 && rays==0 && screens==0 && depthSamples==1,"neighbour-only work not skipped");
    fFallback=1; fNeighbourFill=0; screenWeight=.7f; reset(); result=SSRFallback_PS(uv,uv);
    require(result.a==.7f && screens==1 && fills==0 && surfaces==1 && depthSamples==0,"screen-only result changed");
    screenWeight=0; reset(); result=SSRFallback_PS(uv,uv);
    require(result.a==0 && fills==0,"disabled neighbour fill still ran");
    fNeighbourFill=1; reset(); result=SSRFallback_PS(uv,uv);
    require(result.a==.4f && fills==1 && fillDepth==7 && depthSamples==0,"screen miss did not fall back to neighbours");
    fFallback=0; fNeighbourFill=0; reset(); result=SSRFallback_PS(uv,uv);
    require(result.a==0 && surfaces==0 && fills==0 && depthSamples==0,"disabled fill did work");
    std::puts("SSR fallback passed fill combinations and skipped-work checks");
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-fallback-') as directory:
    cpp, exe = Path(directory)/'test.cpp', Path(directory)/'test'
    cpp.write_text(prefix + body + suffix)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-O0', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
