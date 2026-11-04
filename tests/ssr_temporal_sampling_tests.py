#!/usr/bin/env python3
"""Run SSR.fx's temporal history sampling on synthetic confidence edges.

Requires a C++17 compiler. This models bilinear reads, not GPU precision.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

shader = (Path(__file__).resolve().parents[1] / 'source/resources/SSR.fx').read_text()
start = shader.index('float4 TemporalPremultiply(')
end = shader.index('\n// Debug view 10', start)
body = shader[start:end].replace('float4(c.rgb * c.a, c.a)', 'float4(c.r*c.a,c.g*c.a,c.b*c.a,c.a)')
body = body.replace('float4(c.a > 1e-4 ? c.rgb / c.a : 0.0, c.a)',
                    'float4(c.a > 1e-4 ? c.r/c.a : 0.0, c.a > 1e-4 ? c.g/c.a : 0.0, c.a > 1e-4 ? c.b/c.a : 0.0, c.a)')
prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
struct float2 { float x,y; float2(float a=0,float b=0):x(a),y(b){} };
float2 operator+(float2 a,float2 b) { return {a.x+b.x,a.y+b.y}; }
float2 operator+(float2 a,float b) { return {a.x+b,a.y+b}; }
float2 operator-(float2 a,float b) { return {a.x-b,a.y-b}; }
float2 operator/(float2 a,float2 b) { return {a.x/b.x,a.y/b.y}; }
float2 operator*(float2 a,float2 b) { return {a.x*b.x,a.y*b.y}; }
float2 floor(float2 a) { return {std::floor(a.x),std::floor(a.y)}; }
float2 frac(float2 a) { return {a.x-std::floor(a.x),a.y-std::floor(a.y)}; }
struct float4 {
    float r,g,b,a; float2 uv;
    float4(float r=0,float g=0,float b=0,float a=0):r(r),g(g),b(b),a(a){}
    float4(float2 p,int,int):r(0),g(0),b(0),a(0),uv(p){}
};
float4 lerp(float4 a,float4 b,float t) {
    return {a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t,a.a+(b.a-a.a)*t};
}
float fTemporalAnySurface=0; float2 vec2InvViewportSize{.5f,.5f}; int SSRAccumTex;
float4 texels[4];
float4 tex2Dlod(int,float4 p) {
    float x=std::clamp(p.uv.x*2-.5f,0.0f,1.0f), y=std::clamp(p.uv.y*2-.5f,0.0f,1.0f);
    return lerp(lerp(texels[0],texels[1],x),lerp(texels[2],texels[3],x),y);
}
'''
suffix = r'''
void close(float a,float b,const char* msg) { if (std::abs(a-b)>1e-5f) { std::fprintf(stderr,"%s: %f != %f\n",msg,a,b); std::exit(1); } }
int main() {
    // A red hit beside a fully transparent white miss: the white RGB must not
    // contaminate the red hit when the history is sampled halfway between.
    texels[0]=texels[2]=float4(1,0,0,1); texels[1]=texels[3]=float4(1,1,1,0);
    float4 edge=SSRTemporalHistory({.5f,.5f});
    close(edge.r,.5f,"red premultiplied edge"); close(edge.g,0,"transparent green leaked");
    close(edge.b,0,"transparent blue leaked"); close(edge.a,.5f,"edge confidence");
    float4 straight=TemporalResult(edge); close(straight.r,1,"hit darkened"); close(straight.g,0,"hit contaminated");
    // GI is stored in plain channels and retains the original bilinear lookup.
    fTemporalAnySurface=1; float4 gi=SSRTemporalHistory({.5f,.5f});
    close(gi.r,1,"GI red changed"); close(gi.g,.5f,"GI green changed"); close(gi.a,.5f,"GI alpha changed");
    // Uniform SSR history is unchanged and sampling outside the texture clamps.
    fTemporalAnySurface=0; for (float4& t:texels) t=float4(.2f,.4f,.6f,.5f);
    float4 uniform=SSRTemporalHistory({-.1f,1.1f});
    close(uniform.r,.1f,"uniform red"); close(uniform.g,.2f,"uniform green"); close(uniform.a,.5f,"uniform alpha");
    std::puts("temporal history passed premultiplied edge, GI and clamp checks");
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-temporal-sampling-') as directory:
    cpp, exe = Path(directory)/'test.cpp', Path(directory)/'test'
    cpp.write_text(prefix + body + suffix)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-O0', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
