#!/usr/bin/env python3
"""Exercise SSR.fx's GGX visible-normal sampler with synthetic views and gloss.

Requires a C++17 compiler; CXX may contain a compiler and arguments.
This checks geometric invariants, not visual convergence or BRDF calibration.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

shader = (Path(__file__).resolve().parents[1] / 'source/resources/SSR.fx').read_text()
start = shader.index('float3 SSRMicroNormal(')
end = shader.index('\nTraceOutput SSRTrace_PS', start)
body = shader[start:end].replace('vPos.yx', 'swap(vPos)').replace('vec2NoiseOffset.yx', 'swap(vec2NoiseOffset)')
body = body.replace('seed.yx', 'swap(seed)')
body = body.replace('float3(alpha * localView.xy, localView.z)', 'float3(alpha * localView.x, alpha * localView.y, localView.z)')
body = body.replace('float3(alpha * visible.xy, max(visible.z, 0.0))', 'float3(alpha * visible.x, alpha * visible.y, max(visible.z, 0.0))')
body = body.replace('dot(stretchedView.xy, stretchedView.xy)', '(stretchedView.x*stretchedView.x + stretchedView.y*stretchedView.y)')

prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
float max(float a,float b) { return std::max(a,b); }
float min(float a,float b) { return std::min(a,b); }
float saturate(float a) { return std::clamp(a,0.0f,1.0f); }
float rsqrt(float a) { return 1.0f/std::sqrt(a); }
float lerp(float a,float b,float t) { return a+(b-a)*t; }
struct float2 { float x,y; float2(float a=0,float b=0):x(a),y(b){} };
float2 operator+(float2 a,float2 b) { return {a.x+b.x,a.y+b.y}; }
float2 operator*(float2 a,float b) { return {a.x*b,a.y*b}; }
float2 floor(float2 a) { return {std::floor(a.x),std::floor(a.y)}; }
float2 swap(float2 a) { return {a.y,a.x}; }
struct float3 { float x,y,z; float3(float a=0,float b=0,float c=0):x(a),y(b),z(c){} };
float3 operator+(float3 a,float3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
float3 operator-(float3 a) { return {-a.x,-a.y,-a.z}; }
float3 operator*(float a,float3 b) { return {a*b.x,a*b.y,a*b.z}; }
float3 operator*(float3 b,float a) { return a*b; }
float dot(float3 a,float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
float3 cross(float3 a,float3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float3 normalize(float3 a) { return rsqrt(dot(a,a))*a; }
void sincos(float a,float& s,float& c) { s=std::sin(a); c=std::cos(a); }
struct float4 { float y; float4(float2,int,int):y(0){} };
float glossValue, fStochasticSSR=1, fStochasticQuad=0;
float2 vec2NoiseOffset;
int SpecularTex;
float4 tex2Dlod(int,float4 a) { a.y=glossValue; return a; }
float PixelJitter(float2 p) {
    float x=std::sin(p.x*12.9898f+p.y*78.233f)*43758.5453f;
    return x-std::floor(x);
}
'''
suffix = r'''
void require(bool ok,const char* what) { if (!ok) { std::fprintf(stderr,"%s\n",what); std::exit(1); } }
int main() {
    int count=0;
    for (float nz : {0.0f,0.5f,0.9995f,1.0f}) {
        float3 n=normalize(float3(std::sqrt(max(1.0f-nz*nz,0.0f)),0,nz));
        float3 t=normalize(cross(n, float3(0,1,0)));
        for (float noV : {0.001f,0.05f,0.5f,1.0f})
        for (float gloss : {0.0f,0.2f,0.5f,0.8f,1.0f})
        for (int i=0;i<200;++i) {
            glossValue=gloss;
            float3 view=noV*n+std::sqrt(1-noV*noV)*t;
            float3 m=SSRMicroNormal(float2(0,0),float2(i*13.13f,i*4.1f),n,-view);
            require(std::isfinite(m.x)&&std::isfinite(m.y)&&std::isfinite(m.z),"non-finite micro-normal");
            require(std::abs(dot(m,m)-1.0f)<1e-4f,"micro-normal not normalized");
            require(dot(m,n)>=-1e-5f,"micro-normal below surface");
            require(dot(m,view)>=-1e-4f,"micro-normal invisible from view");
            ++count;
        }
    }
    // At the roughness clamp the lobe is narrow: almost every sample stays within a few degrees
    // of the geometric normal. An early out for mirror-like paint would be sound, but its branch
    // costs the march ps_3_0 slots it does not have to spare, so this only records how narrow it is.
    glossValue=1.0f;
    {
        float3 n=normalize(float3(0.3f,0.2f,0.93f));
        int near=0, samples=200;
        for (int i=0;i<samples;++i) {
            float3 m=SSRMicroNormal(float2(0,0),float2(i*13.13f,i*4.1f),n,float3(0.2f,0.1f,-2.0f));
            near += dot(m,n)>0.9962f ? 1 : 0; // within five degrees
        }
        require(near*10>=samples*9,"the clamped lobe is not narrow around the geometric normal");
    }
    // One sample per 2x2 block: the block's pixels must share a direction, and without the
    // switch they must not, or the experiment would compare a thing with itself.
    {
        float3 n=normalize(float3(0.2f,0.1f,0.97f));
        float3 C=float3(0.3f,0.2f,-2.0f);
        glossValue=0.8f;
        fStochasticQuad=1;
        float3 shared=SSRMicroNormal(float2(0,0),float2(5.0f,7.0f),n,C);
        for (float2 at : {float2(4.0f,6.0f),float2(5.0f,6.0f),float2(4.0f,7.0f),float2(5.0f,7.0f)}) {
            float3 m=SSRMicroNormal(float2(0,0),at,n,C);
            require(dot(m,shared)>0.99999f,"pixels of one block sampled different directions");
        }
        float3 nextBlock=SSRMicroNormal(float2(0,0),float2(6.0f,6.0f),n,C);
        require(dot(nextBlock,shared)<0.99999f,"every block sampled one and the same direction");
        fStochasticQuad=0;
        float3 own=SSRMicroNormal(float2(0,0),float2(4.0f,6.0f),n,C);
        float3 neighbour=SSRMicroNormal(float2(0,0),float2(5.0f,7.0f),n,C);
        require(dot(own,neighbour)<0.99999f,"per-pixel samples were already identical");
    }
    fStochasticSSR=0;
    float3 n{0,0,1};
    float3 m=SSRMicroNormal(float2(),float2(),n,float3(0,0,-1));
    require(dot(m,n)==1,"disabled sampler changed geometric normal");
    std::printf("%d VNDF samples satisfy geometric invariants\n",count);
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-vndf-test-') as directory:
    directory = Path(directory)
    source, executable = directory/'test.cpp', directory/'test'
    source.write_text(prefix+body+suffix)
    subprocess.run(shlex.split(os.environ.get('CXX','c++')) + ['-std=c++17','-O0',str(source),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
