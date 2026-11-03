#!/usr/bin/env python3
"""Execute SSR.fx's history blur with full- and half-resolution pass sizes.

The harness checks sample positions, not GPU sampling or visual quality.
Requires a C++17 compiler; CXX may include arguments.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

shader = (Path(__file__).resolve().parents[1] / 'source/resources/SSR.fx').read_text()
start = shader.index('float3 SampleHistoryBlurred(')
end = shader.index('\n#ifndef NUM_STEPS', start)
body = shader[start:end]
prefix = r'''
#include <cmath>
#include <cstdio>
#include <cstdlib>
struct float2 { float x,y; float2(float a=0,float b=0):x(a),y(b){} };
float2 operator+(float2 a,float2 b) { return {a.x+b.x,a.y+b.y}; }
float2 operator-(float2 a,float2 b) { return {a.x-b.x,a.y-b.y}; }
float2 operator*(float a,float2 b) { return {a*b.x,a*b.y}; }
struct float3 { float x; float3(float a=0):x(a){} };
float3& operator+=(float3& a,float3 b) { a.x+=b.x; return a; }
float3 operator*(float3 a,float b) { return {a.x*b}; }
float2 vec2InvHistorySize{1.0f/1920,1.0f/1080}, vec2InvViewportSize;
float2 positions[5]; int count;
float3 SampleHistory(float2 p) { positions[count++]=p; return {1}; }
'''
suffix = r'''
void require(bool ok,const char* msg) { if (!ok) { std::fprintf(stderr,"%s\n",msg); std::exit(1); } }
void close(float a,float b,const char* msg) { require(std::abs(a-b)<1e-6f,msg); }
int main() {
    float2 full[5];
    for (int half=0;half<2;++half) {
        vec2InvViewportSize=half ? float2(1.0f/960,1.0f/540) : vec2InvHistorySize;
        count=0;
        close(SampleHistoryBlurred(float2(.5f,.5f),4).x,1,"uniform history changed");
        require(count==5,"wrong number of blur taps");
        if (!half) for (int i=0;i<5;++i) full[i]=positions[i];
        else for (int i=0;i<5;++i) {
            close(positions[i].x,full[i].x,"blur width changed at half resolution");
            close(positions[i].y,full[i].y,"blur height changed at half resolution");
        }
        close(positions[1].x-.5f,4.0f/1920,"incorrect horizontal radius");
        close(positions[1].y-.5f,4.0f/1080,"incorrect vertical radius");
    }
    count=0; SampleHistoryBlurred(float2(.5f,.5f),0);
    require(count==1,"zero radius still blurred");
    std::puts("history blur passed full/half resolution and zero-radius checks");
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-blur-test-') as directory:
    cpp, exe = Path(directory)/'test.cpp', Path(directory)/'test'
    cpp.write_text(prefix + body + suffix)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-O0', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
