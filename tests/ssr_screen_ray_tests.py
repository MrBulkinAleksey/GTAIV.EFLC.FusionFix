#!/usr/bin/env python3
"""Check SSR.fx's projected segment clipping using its actual HLSL helper.

Requires a C++17 compiler; CXX may include compiler arguments.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

shader = (Path(__file__).resolve().parents[1] / 'source/resources/SSR.fx').read_text()
start = shader.index('float SSRScreenRayEnd(')
end = shader.index('\nbool SetupReflectionRay', start)
body = shader[start:end]
prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
struct float2 { float x,y; };
float min(float a,float b) { return std::min(a,b); }
float clamp(float a,float lo,float hi) { return std::clamp(a,lo,hi); }
using std::abs;
'''
suffix = r'''
void close(float got,float want,const char* msg) {
    if (std::abs(got-want)>1e-5f) { std::fprintf(stderr,"%s: %.7f != %.7f\n",msg,got,want); std::exit(1); }
}
int main() {
    float2 centre{.5f,.5f};
    close(SSRScreenRayEnd(centre,{0,0}),1,"stationary ray");
    close(SSRScreenRayEnd(centre,{-1e-6f,.2f}),1,"nearly vertical left ray");
    close(SSRScreenRayEnd(centre,{.2f,-1e-6f}),1,"nearly horizontal upward ray");
    close(SSRScreenRayEnd(centre,{1e-6f,-.2f}),1,"nearly vertical right ray");
    close(SSRScreenRayEnd(centre,{-.2f,1e-6f}),1,"nearly horizontal downward ray");
    close(SSRScreenRayEnd({.1f,.5f},{-.2f,0}),.5f,"left edge");
    close(SSRScreenRayEnd({.9f,.5f},{.2f,0}),.5f,"right edge");
    close(SSRScreenRayEnd({.5f,.1f},{0,-.2f}),.5f,"top edge");
    close(SSRScreenRayEnd({.5f,.9f},{0,.2f}),.5f,"bottom edge");
    close(SSRScreenRayEnd({.000001f,.5f},{-2e-6f,.2f}),.5f,"small negative edge crossing");
    std::puts("screen ray clipping passed axis, near-zero and edge-crossing checks");
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-screen-ray-test-') as directory:
    cpp, exe = Path(directory)/'test.cpp', Path(directory)/'test'
    cpp.write_text(prefix + body + suffix)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-O0', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
