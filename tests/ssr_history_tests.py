#!/usr/bin/env python3
"""Execute SSR.fx's history filter against synthetic depth/colour boundaries.

CXX may contain a C++17 compiler and arguments. This tests the shader's filter
math on CPU; it does not test D3D sampling precision, reprojection or the game.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

shader = (Path(__file__).resolve().parents[1] / 'source/resources/SSR.fx').read_text()
start = shader.index('float3 SampleHitHistory(')
end = shader.index('\n// The colour of a hit', start)
body = shader[start:end]
prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
float max(float a,float b) { return std::max(a,b); }
struct float2 { float x,y; float2(float a=0,float b=0):x(a),y(b){} };
float2 operator+(float2 a,float b) { return {a.x+b,a.y+b}; }
float2 operator+(float2 a,float2 b) { return {a.x+b.x,a.y+b.y}; }
float2 operator-(float2 a,float b) { return {a.x-b,a.y-b}; }
float2 operator/(float2 a,float2 b) { return {a.x/b.x,a.y/b.y}; }
float2 operator*(float2 a,float2 b) { return {a.x*b.x,a.y*b.y}; }
float2 floor(float2 a) { return {std::floor(a.x),std::floor(a.y)}; }
float2 frac(float2 a) { return {a.x-std::floor(a.x),a.y-std::floor(a.y)}; }
struct float3 { float x,y,z; float3(float a=0):x(a),y(a),z(a){} };
float3 operator*(float3 a,float b) { return {a.x*b}; }
float3 operator+(float3 a,float3 b) { return {a.x+b.x}; }
float3 operator/(float3 a,float b) { return {a.x/b}; }
struct float4 {
    float x,y,z,w,r; float2 uv;
    float4(float a,float b,float c,float d):x(a),y(b),z(c),w(d),r(a){}
    float4(float2 p,int,int):x(0),y(0),z(0),w(0),r(0),uv(p){}
};
float4 operator-(float4 a,float b) { return {a.x-b,a.y-b,a.z-b,a.w-b}; }
float4 operator-(float b,float4 a) { return {b-a.x,b-a.y,b-a.z,b-a.w}; }
float4 operator*(float4 a,float4 b) { return {a.x*b.x,a.y*b.y,a.z*b.z,a.w*b.w}; }
float4 abs(float4 a) { return {std::abs(a.x),std::abs(a.y),std::abs(a.z),std::abs(a.w)}; }
float4 step(float e,float4 a) { return {a.x>=e?1.f:0.f,a.y>=e?1.f:0.f,a.z>=e?1.f:0.f,a.w>=e?1.f:0.f}; }
bool operator>(float4 a,float b) { return a.x>b&&a.y>b&&a.z>b&&a.w>b; }
bool all(bool b) { return b; }
float dot(float4 a,float b) { return (a.x+a.y+a.z+a.w)*b; }
float2 vec2InvHistorySize{1.f/6,1.f/6}; // the texels HIT_TEXEL_STEP apart are the test's two by two
float fUsePrevDepth=1;
int PrevDepthTex;
float depths[4],colours[4];
float sample(float* data,float2 uv) {
    float x=std::clamp(uv.x*2-0.5f,0.0f,1.0f),y=std::clamp(uv.y*2-0.5f,0.0f,1.0f);
    return data[0]*(1-x)*(1-y)+data[1]*x*(1-y)+data[2]*(1-x)*y+data[3]*x*y;
}
float4 tex2Dlod(int,float4 a) { a.r=sample(depths,a.uv);return a; }
float3 SampleHistory(float2 uv) { return {sample(colours,uv)}; }
float3 SampleHistoryPoint(float2 uv) { return SampleHistory(uv); }
#define HIT_TEXEL_STEP 3.0f
// What TraceHit leaves for SampleHitHistory.
float2 gHitTexel,gHitTexelFraction;
float4 gHitTexelApart{0,0,0,0};
'''
suffix = r'''
void close(float a,float b,const char* reason) {
    if (std::abs(a-b)>1e-5f) { std::fprintf(stderr,"%s: %f != %f\n",reason,a,b); std::exit(1); }
}
// As TraceHit: a ray that hit the surface of log depth surface, read at uv.
float Hit(float2 uv,float surface) {
    float2 pixel=uv/vec2InvHistorySize-0.5f;
    gHitTexelFraction=frac(pixel);
    gHitTexel=(floor(pixel)-0.5f)*vec2InvHistorySize;
    float a[4];
    for (int i=0;i<4;++i) a[i]=std::abs(depths[i]-surface);
    float nearest=std::min({a[0],a[1],a[2],a[3]});
    gHitTexelApart=float4(a[0]-nearest,a[1]-nearest,a[2]-nearest,a[3]-nearest);
    return SampleHitHistory(uv).x;
}
int main() {
    // Black foreground and white background: each side of the depth edge must
    // retain its own colour rather than the hardware bilinear grey.
    depths[0]=depths[2]=0.2f; depths[1]=depths[3]=0.8f;
    colours[0]=colours[2]=0; colours[1]=colours[3]=1;
    close(Hit({0.49f,0.5f},0.2f),0,"foreground contaminated");
    close(Hit({0.51f,0.5f},0.8f),1,"background contaminated");
    // A read that fell nearer the other side of the edge still takes the surface the ray hit.
    close(Hit({0.51f,0.5f},0.2f),0,"foreground hit read the background");
    close(Hit({0.49f,0.5f},0.8f),1,"background hit read the foreground");
    // The same colours on a continuous surface retain normal bilinear filtering.
    for (float& d:depths) d=0.2f;
    close(Hit({0.49f,0.5f},0.2f),SampleHistory({0.49f,0.5f}).x,"flat surface filtering changed");
    // Without previous depth the original bilinear result remains available.
    depths[1]=depths[3]=0.8f; fUsePrevDepth=0;
    close(SampleHitHistory({0.49f,0.5f}).x,SampleHistory({0.49f,0.5f}).x,"missing depth fallback changed");
    std::puts("history filter passed foreground, background, surface and fallback checks");
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-history-test-') as directory:
    directory = Path(directory)
    source, executable = directory/'test.cpp', directory/'test'
    source.write_text(prefix+body+suffix)
    subprocess.run(shlex.split(os.environ.get('CXX','c++')) + ['-std=c++17','-O0',str(source),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
