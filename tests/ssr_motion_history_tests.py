#!/usr/bin/env python3
"""Run SSR.fx's hit motion and history weighting on synthetic motion vectors.

Requires a C++17 compiler. The harness does not emulate a D3D device.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

shader = (Path(__file__).resolve().parents[1] / 'source/resources/SSR.fx').read_text()
start = shader.index('float2 TemporalHistoryUVWithMotion(')
end = shader.index('\nfloat3 ViewPosFromUVZ', start)
reprojection = re.sub(r'\[branch\]', '', shader[start:end])
reprojection = reprojection.replace('out bool ', 'bool& ').replace('out float2 ', 'float2& ')
start = shader.index('float SSRMovingHitHistoryWeight(')
end = shader.index('\n// Each pixel decides', start)
weight = shader[start:end]
prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
float min(float a,float b) { return std::min(a,b); }
float max(float a,float b) { return std::max(a,b); }
struct float2 { float x,y; float2(float a=0,float b=0):x(a),y(b){} };
float2 operator+(float2 a,float2 b) { return {a.x+b.x,a.y+b.y}; }
float2 operator-(float2 a,float2 b) { return {a.x-b.x,a.y-b.y}; }
float2 operator*(float a,float2 b) { return {a*b.x,a*b.y}; }
float2 abs(float2 a) { return {std::abs(a.x),std::abs(a.y)}; }
bool operator>(float2 a,float2 b) { return a.x>b.x || a.y>b.y; }
bool any(bool b) { return b; }
struct float3 {};
struct float4 { float2 xy; float4(float2,int,int):xy(){} };
float fUseMotion; float2 vec2MotionJitter,vec2InvViewportSize{.001f,.001f},cameraUV,motion;
int MotionTex;
float2 HistoryUV(float3) { return cameraUV; }
float4 tex2Dlod(int,float4 v) { v.xy=motion; return v; }
float smoothstep(float lo,float hi,float x) {
    float t=std::clamp((x-lo)/(hi-lo),0.0f,1.0f); return t*t*(3-2*t);
}
'''
suffix = r'''
void require(bool ok,const char* msg) { if (!ok) { std::fprintf(stderr,"%s\n",msg); std::exit(1); } }
void close(float a,float b,const char* msg) { require(std::abs(a-b)<1e-5f,msg); }
int main() {
    float2 uv{.2f,.3f}, object; float3 C; bool check;
    cameraUV={.25f,.3f}; motion={.05f,0}; fUseMotion=1;
    float2 prev=TemporalHistoryUVWithMotion(uv,C,check,object);
    close(prev.x,.25f,"camera motion reprojection"); close(object.x,0,"camera motion marked as object motion");
    require(check,"static surface skipped depth check");
    motion={.049f,0}; vec2MotionJitter={.001f,0};
    prev=TemporalHistoryUVWithMotion(uv,C,check,object);
    close(object.x,0,"jitter marked static surface as moving"); require(check,"jitter disabled static depth test");
    vec2MotionJitter={0,0};
    motion={.06f,0}; prev=TemporalHistoryUVWithMotion(uv,C,check,object);
    close(object.x,.01f,"own motion not isolated"); require(!check,"moving hit accepted static depth test");
    close(SSRMovingHitHistoryWeight(0),1,"static history discarded");
    require(SSRMovingHitHistoryWeight(4)>0 && SSRMovingHitHistoryWeight(4)<1,"moving history not attenuated");
    close(SSRMovingHitHistoryWeight(10),0,"fast moving history retained");
    close(SSRTemporalMotionWeight(0,0,true,0,2),1,"static stochastic miss lost history");
    close(SSRTemporalMotionWeight(0,0,true,20,2),0,"moving stochastic miss retained stale history");
    close(SSRTemporalMotionWeight(0,0,true,0,0),1,"miss without evidence lost history");
    close(SSRTemporalMotionWeight(.1f,10,false,0,0),0,"moving hit retained stale history");
    close(SSRStochasticMissKeep(.85f,.85f,true,0,1),.95f,"stochastic miss erased confidence too fast");
    close(SSRStochasticMissKeep(0,.85f,true,0,1),0,"invalid history revived");
    close(SSRStochasticMissKeep(.425f,.85f,true,0,1),.48875f,"motion rejection not preserved");
    close(SSRStochasticMissKeep(.85f,.85f,true,0,0),.85f,"miss without neighbours changed");
    close(SSRStochasticMissKeep(.85f,.85f,true,.2f,1),.85f,"valid fallback was ignored");
    close(SSRStochasticMissKeep(.85f,.85f,false,0,1),.85f,"non-stochastic accumulation changed");
    close(SSRStochasticMissKeep(.001f,.001f,true,0,1),.001999f,"low temporal setting ignored");
    close(SSRStochasticMissKeep(.97f,.97f,true,0,1),.97f,"high temporal setting increased or decreased");
    // One miss surrounded by eight solid hits: the 3x3 variance window admits
    // history alpha 1. The new blend should preserve its coverage more closely.
    float mean=8.0f/9.0f, spread=std::sqrt(mean-mean*mean);
    require(mean+1.5f*spread>=1,"test neighbourhood clips solid history");
    float oldAlpha=.85f, newAlpha=SSRStochasticMissKeep(.85f,.85f,true,0,8);
    require(newAlpha>oldAlpha && newAlpha<1,"stochastic confidence not stabilized");
    fUseMotion=0; prev=TemporalHistoryUVWithMotion(uv,C,check,object);
    close(prev.x,.25f,"no-vector camera fallback"); close(object.x,0,"missing vector rejected history"); require(check,"no-vector depth test disabled");
    std::puts("reflected hit motion passed camera, object and missing-vector checks");
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-motion-history-') as directory:
    cpp, exe = Path(directory)/'test.cpp', Path(directory)/'test'
    cpp.write_text(prefix + reprojection + weight + suffix)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-O0', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
