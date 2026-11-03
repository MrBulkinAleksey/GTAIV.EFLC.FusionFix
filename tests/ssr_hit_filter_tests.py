#!/usr/bin/env python3
"""Test SSR.fx's hit-distance weights on reflected foreground/background edges.

The C++ harness executes the actual HLSL helper; it does not emulate the GPU or
verify temporal reprojection. Set CXX to a C++17 compiler if needed.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'source/resources/SSR.fx').read_text()
start = source.index('float SSRHitDistanceWeight(')
end = source.index('\n// Each pixel decides', start)
helper = source[start:end]
prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
float fMaxDistance=24.0f;
float min(float a,float b) { return std::min(a,b); }
using std::abs;
float smoothstep(float lo,float hi,float x) {
    float t=std::clamp((x-lo)/(hi-lo),0.0f,1.0f); return t*t*(3-2*t);
}
'''
suffix = r'''
void check(bool ok,const char* msg) { if (!ok) { std::fprintf(stderr,"%s\n",msg); std::exit(1); } }
int main() {
    auto dist=[](float metres) { return metres/fMaxDistance; };
    check(SSRHitDistanceWeight(dist(.8f),dist(.8f))==1,"same hit lost");
    check(SSRHitDistanceWeight(dist(.8f),dist(.9f))==1,"nearby samples separated");
    check(SSRHitDistanceWeight(dist(.8f),dist(3.0f))==0,"background mixed into foreground");
    check(SSRHitDistanceWeight(dist(3.0f),dist(.8f))==0,"filter not symmetric");
    check(SSRHitDistanceWeight(0,dist(.8f))==0,"miss mixed with hit");
    check(SSRHitDistanceWeight(0,0)==1,"two misses separated");
    // The historical unweighted moment of a dark person plus bright road taps
    // allows bright history in the clamp. Only same-hit taps should count.
    float centre=0.25f, bg=4.0f, w=SSRHitDistanceWeight(dist(.8f),dist(3.0f));
    float mean=(centre+8*w*bg)/(1+8*w);
    check(std::abs(mean-centre)<1e-6,"background widened temporal clamp");
    // A hit beside empty taps must not lose confidence solely because the
    // denoiser includes their weights. Low confidence exposes the bright map.
    float missWeight=SSRHitDistanceWeight(dist(.8f),0.0f);
    float filteredAlpha=1.0f/(1.0f+4.0f*missWeight);
    check(std::abs(filteredAlpha-1.0f)<1e-6,"miss darkened hit confidence");
    // A single stochastic miss surrounded by hits must retain a nonzero
    // temporal window, while a deterministic miss and the hit side of the
    // same edge keep the strict foreground/background separation.
    float borrowed=SSRTemporalHitWeight(0,dist(.8f),1);
    check(borrowed==1,"stochastic miss discarded surrounding hits");
    check(SSRTemporalHitWeight(0,dist(.8f),0)==0,"deterministic miss borrowed hits");
    check(SSRTemporalHitWeight(dist(.8f),0,1)==0,"stochastic hit borrowed misses");
    check(SSRTemporalHitWeight(0,0,1)==1,"empty neighbourhood changed");
    check(SSRTemporalHitWeight(-1,dist(.8f),1)==0,"untraced surface borrowed hits");
    float alphaMean=8*borrowed/(1+8*borrowed);
    float alphaSpread=std::sqrt(alphaMean-alphaMean*alphaMean);
    check(alphaMean+1.5f*alphaSpread>0,"stochastic miss cannot retain history");
    // A distance-only temporal rejection cannot identify the reflected object.
    // Two distinct points on the same range sphere are indistinguishable.
    float objectA=std::hypot(3.0f,4.0f), objectB=std::hypot(-3.0f,4.0f);
    check(SSRHitDistanceWeight(dist(objectA),dist(objectB))==1,"equal ranges should be indistinguishable");
    // Conversely, two stochastic rays can hit the same flat wall at different
    // angles. The spatial threshold rejects them despite the shared surface.
    const float pi=3.14159265358979323846f;
    float wallA=5.0f/std::cos(60.0f*pi/180.0f);
    float wallB=5.0f/std::cos(68.0f*pi/180.0f);
    check(SSRHitDistanceWeight(dist(wallA),dist(wallB))==0,"wall example no longer separates");
    for (int i=0;i<100;++i) {
        float a=dist(.1f+i*.2f), b=dist(.1f+(99-i)*.2f);
        float w1=SSRHitDistanceWeight(a,b),w2=SSRHitDistanceWeight(b,a);
        check(w1>=0&&w1<=1&&std::abs(w1-w2)<1e-6,"invalid/symmetric weight");
    }
    std::puts("hit-distance filter passed foreground/background, miss and symmetry checks");
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-hit-test-') as directory:
    directory = Path(directory)
    cpp, exe = directory/'test.cpp', directory/'test'
    cpp.write_text(prefix+helper+suffix)
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++17','-O0',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
