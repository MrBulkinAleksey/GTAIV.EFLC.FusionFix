#!/usr/bin/env python3
"""Run the actual SSR.fx traversal on synthetic depth buffers (no GPU required).

CXX may contain a compiler and arguments, e.g. CXX='zig c++'. This exercises the
scalar march/refinement state machine, not GPU sampling, code generation or timing.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

shader = (Path(__file__).resolve().parents[1] / 'source/resources/SSR.fx').read_text()
start = shader.index('    float hit = 0.0;', shader.index('float4 TraceHit('))
end = shader.index('    if (hit <= 0.0)', start)
body = re.sub(r'\[(?:branch|loop|fastopt)\]', '', shader[start:end])

prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using std::abs;
float max(float a, float b) { return std::max(a, b); }
float min(float a, float b) { return std::min(a, b); }
float saturate(float a) { return std::clamp(a, 0.0f, 1.0f); }
float lerp(float a, float b, float t) { return a + (b-a)*t; }
struct float2 { float x,y; };
float2 operator*(float2 a,float2 b) { return {a.x*b.x,a.y*b.y}; }
bool operator==(float2 a,float2 b) { return a.x==b.x && a.y==b.y; }
float2 floor(float2 a) { return {std::floor(a.x),std::floor(a.y)}; }
bool all(bool a) { return a; }
float2 lerp(float2 a,float2 b,float t) { return {lerp(a.x,b.x,t),lerp(a.y,b.y,t)}; }
struct float4 { float r; float2 uv; float4(float2 p,int,int):r(0),uv(p) {} };
int scene, reads;
float wall = 5.0f;
float LinearDepth(float2) { ++reads; return scene==0 ? 1000.0f : wall; }
float MarchDepth(float2 uv) {
    ++reads;
    // Min-reduction includes a pole in a neighbouring pixel, absent on the ray.
    if (scene==2 && uv.x>=0.1f && uv.x<0.3f) return 0.5f;
    return scene==0 ? 1000.0f : wall;
}
int HiZDepthTex;
float4 tex2Dlod(int,float4 p) {
    // Conservative min over the cell. The test covers both cells intersecting
    // the silhouette and cells in empty space, as a real reduction would.
    float cell=std::floor(p.uv.x*8.0f)/8.0f;
    p.r=(scene==2 && cell<0.3f && cell+0.125f>0.1f) ? 0.5f : (scene==0 ? 1000.0f : wall);
    ++reads; return p;
}
struct Result { bool hit; float z; int reads; };
Result trace(int which,float jitter,bool hiz,float pixels,float thickness,bool reverse=false,bool diagonal=false) {
    scene=which; reads=0;
    float2 uv0{reverse ? 1.0f : 0.0f,diagonal ? 0.2f : 0.5f};
    float2 uv1{reverse ? 0.0f : 1.0f,diagonal ? 0.8f : 0.5f};
    float2 dUV{uv1.x-uv0.x,uv1.y-uv0.y};
    float2 vec2InvViewportSize{1/pixels,1/pixels},vec2HiZSize{8,8};
    float invZ0=1.0f,invZ1=0.1f,tEnd=1.0f,steps=NUM_STEPS;
    float fThickness=thickness,fUseHiZ=hiz ? 1.0f : 0.0f,fPassThinObjects=1;
    struct { float len; } ray{9};
'''
suffix = r'''
    return {hit>0,hit>0 ? 1.0f/lerp(invZ0,invZ1,hitHi) : 0,reads};
}
void require(bool ok,const char* message) {
    if (!ok) { std::fprintf(stderr,"%s\n",message); std::exit(1); }
}
int main() {
    int cases=0;
    for (float jitter : {0.1f,0.5f,1.0f})
    for (float pixels : {0.01f,64.0f,1920.0f})
    for (float thickness : {0.02f,0.1f,0.5f})
    for (bool hiz : {false,true}) {
        auto empty=trace(0,jitter,hiz,pixels,thickness);
        require(!empty.hit,"empty ray produced a hit");
        auto plane=trace(1,jitter,hiz,pixels,thickness);
        require(plane.hit,"missed unobstructed plane");
        // Depth tolerance matters even when the projected ray is subpixel long.
        require(std::abs(plane.z-wall)<thickness,"refinement stopped too early in depth");
        auto pole=trace(2,jitter,hiz,pixels,thickness);
        require(pole.hit,"false coarse silhouette prevented a later valid hit");
        require(std::abs(pole.z-wall)<thickness,"resumed ray hit the false silhouette");
        require(pole.reads<8*(NUM_STEPS+2*(NUM_REFINE_STEPS+1)),"ray exceeded bounded work");
        cases+=3;
    }
    // Enabling conservative Hi-Z must leave the first accepted hit unchanged,
    // including negative directions and samples close to cell boundaries.
    for (int i=1;i<=97;++i)
    for (bool reverse : {false,true})
    for (bool diagonal : {false,true})
    for (int kind : {0,1,2}) {
        float jitter=i/98.0f;
        auto ordinary=trace(kind,jitter,false,1920.0f,0.1f,reverse,diagonal);
        auto hierarchical=trace(kind,jitter,true,1920.0f,0.1f,reverse,diagonal);
        require(ordinary.hit==hierarchical.hit,"Hi-Z changed hit validity");
        if (ordinary.hit)
            require(std::abs(ordinary.z-hierarchical.z)<1e-4f,"Hi-Z changed hit depth");
        ++cases;
    }
    std::printf("%d synthetic traversal cases passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-trace-test-') as directory:
    directory = Path(directory)
    source = directory / 'test.cpp'
    source.write_text(prefix + body + suffix)
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    for steps, refine in ((32, 8), (128, 16)):
        exe = directory / f'test-{steps}-{refine}'
        subprocess.run(compiler + ['-std=c++17', '-O0', f'-DNUM_STEPS={steps}',
                       f'-DNUM_REFINE_STEPS={refine}', str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
