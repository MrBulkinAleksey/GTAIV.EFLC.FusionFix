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
steps_start = shader.index('float SSRStepCount(')
steps_fn = shader[steps_start:shader.index('// Marches the reflected ray', steps_start)]
prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using std::abs;
float max(float a, float b) { return std::max(a, b); }
float min(float a, float b) { return std::min(a, b); }
float clamp(float a, float lo, float hi) { return std::clamp(a, lo, hi); }
using std::ceil;
float saturate(float a) { return std::clamp(a, 0.0f, 1.0f); }
float lerp(float a, float b, float t) { return a + (b-a)*t; }
struct float2 { float x,y; };
float2 operator*(float2 a,float2 b) { return {a.x*b.x,a.y*b.y}; }
float2 operator/(float2 a,float2 b) { return {a.x/b.x,a.y/b.y}; }
float length(float2 a) { return std::sqrt(a.x*a.x+a.y*a.y); }
float fStepsPerPixel=0.5f;
bool operator==(float2 a,float2 b) { return a.x==b.x && a.y==b.y; }
float2 floor(float2 a) { return {std::floor(a.x),std::floor(a.y)}; }
bool all(bool a) { return a; }
float2 lerp(float2 a,float2 b,float t) { return {lerp(a.x,b.x,t),lerp(a.y,b.y,t)}; }
struct float4 { float r; float y=0; float2 uv; float4(float2 p,int,int):r(0),uv(p) {} };
int scene, reads;
float gTraceSkipShare = 0.0f, gTraceHiZLevel = 0.0f;
float glossValue = 1.0f, fGlossCutoff = 0.5f;
int SpecularTex=1;
float wall = 5.0f;
float LinearDepth(float2) { ++reads; return scene==0 ? 1000.0f : wall; }
float MarchDepth(float2 uv) {
    ++reads;
    // Min-reduction includes a pole in a neighbouring pixel, absent on the ray.
    if (scene==2 && uv.x>=0.1f && uv.x<0.3f) return 0.5f;
    return scene==0 ? 1000.0f : wall;
}
// Distinct ids: the stub tells the levels apart by them, and zero is the specular sampler's
// absence in the other harnesses, so none of them may sit at zero here.
int HiZDepthTex=2, HiZDepthTex1=3;
float hiZGrid=8.0f;
float4 tex2Dlod(int which,float4 p) {
    // The gloss the receiver's paint stores; the march widens its hit bracket by it.
    if (which==SpecularTex) { p.y=glossValue; return p; }
    // Conservative min over the cell of whichever level the ray chose, the coarse one's cells
    // twice as wide. The test covers both cells intersecting the silhouette and cells in empty
    // space, as a real reduction would.
    float grid=which==HiZDepthTex1 ? hiZGrid*0.5f : hiZGrid, cell=1.0f/grid;
    float at=std::floor(p.uv.x*grid)/grid;
    p.r=(scene==2 && at<0.3f && at+cell>0.1f) ? 0.5f : (scene==0 ? 1000.0f : wall);
    ++reads; return p;
}
struct Result { bool hit; float z; int reads; float skips; float level; };
Result trace(int which,float jitter,bool hiz,float pixels,float thickness,bool reverse=false,bool diagonal=false,int levels=1,float grid=8.0f,float gloss=glossValue) {
    scene=which; reads=0; gTraceSkipShare=0.0f; gTraceHiZLevel=0.0f; hiZGrid=grid;
    float2 uv0{reverse ? 1.0f : 0.0f,diagonal ? 0.2f : 0.5f};
    float2 uv1{reverse ? 0.0f : 1.0f,diagonal ? 0.8f : 0.5f};
    float2 dUV{uv1.x-uv0.x,uv1.y-uv0.y};
    float2 vec2InvViewportSize{1/pixels,1/pixels},vec2HiZSize{grid,grid},vec2HiZSize1{grid*0.5f,grid*0.5f};
    float invZ0=1.0f,invZ1=0.1f,tEnd=1.0f,steps=NUM_STEPS; // the march's own budget is tested below in SSRStepCount
    float fThickness=thickness,fUseHiZ=hiz ? (float)levels : 0.0f,fPassThinObjects=1,fRayTries=3;
    struct { float len; } ray{9};
'''
suffix = r'''
    return {hit>0,hit>0 ? 1.0f/lerp(invZ0,invZ1,hitHi) : 0,reads,gTraceSkipShare,gTraceHiZLevel};
}
void require(bool ok,const char* message) {
    if (!ok) { std::fprintf(stderr,"%s\n",message); std::exit(1); }
}
int main() {
    int cases=0;
    // The step budget: four samples for a ray shorter than a pixel, the ray's own half while that
    // is under twelve, the cap beyond, and never fewer steps for a longer ray.
    require(SSRStepCount(0.01f)==4.0f,"a sub-pixel ray lost its four samples");
    require(SSRStepCount(8.0f)==8.0f,"an eight pixel ray did not get eight steps");
    require(SSRStepCount(24.0f)==12.0f,"a 24 pixel ray did not get the floor of twelve");
    require(SSRStepCount(64.0f)==min(32.0f,(float)NUM_STEPS),"a 64 pixel ray did not get one step per two pixels");
    require(SSRStepCount(4.0f*NUM_STEPS)==(float)NUM_STEPS,"a ray long enough did not reach the cap");
    for (float px=0.5f,prev=0.0f; px<=4096.0f; px*=1.5f) {
        float s=SSRStepCount(px);
        require(s>=prev,"a longer ray got fewer steps");
        require(s<=(float)NUM_STEPS,"steps exceeded the cap");
        require(s<=max(px,4.0f),"more steps than the ray has pixels, past the floor");
        prev=s;
    }
    cases+=8;
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
    // The hierarchy's own bookkeeping: it reports the share of steps it took away, and in
    // an empty scene that share is above zero and the depth reads do fall.
    auto plain=trace(0,0.5f,false,1920.0f,0.1f);
    require(plain.skips==0.0f,"steps counted as skipped without the hierarchy");
    auto hierarchical=trace(0,0.5f,true,1920.0f,0.1f);
    require(hierarchical.skips>0.0f && hierarchical.skips<=1.0f,"skip share is not a share of the steps");
    require(hierarchical.reads<plain.reads,"hierarchy did not reduce depth reads");
    cases+=3;
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
    // The coarser level, which a long ray takes, is a minimum over the same depth, so it must
    // find the same hits; over an empty ray its wider cells must also skip strictly more, or the
    // ray never chose it and the comparison says nothing. Four cells per step is well past the
    // threshold at either step count, and is what a long ray over a full size grid looks like.
    const float grid=4.0f*NUM_STEPS;
    for (int i=1;i<=13;++i)
    for (int kind : {0,1,2}) {
        float jitter=i/14.0f;
        auto fine=trace(kind,jitter,true,1920.0f,0.1f,false,false,1,grid);
        auto coarse=trace(kind,jitter,true,1920.0f,0.1f,false,false,2,grid);
        require(fine.hit==coarse.hit,"the coarse level changed hit validity");
        require(fine.level<0.5f,"the coarse level was taken with one level built");
        require(coarse.level>0.5f,"a long ray with two levels built kept the fine one");
        if (fine.hit)
            require(std::abs(fine.z-coarse.z)<1e-4f,"the coarse level changed hit depth");
        if (kind==0) {
            require(coarse.skips>fine.skips,"the coarse level skipped no more than the fine one");
            require(coarse.reads<=fine.reads,"the coarse level read more depth than the fine one");
        }
        ++cases;
    }
    // A ray that crosses few cells keeps the finer level: its cells already hold several steps,
    // and the coarser one would only loosen the depth a jump proves clear.
    for (int kind : {0,1,2}) {
        auto wide=trace(kind,0.5f,true,1920.0f,0.1f,false,false,2,4.0f);
        require(wide.level<0.5f,"a ray over wide cells took the coarse level");
        ++cases;
    }
    // Widening the bracket for rough paint must not let a coarse silhouette through as a hit:
    // the pole sits 4.5 m off the ray, which no roughness multiple of the test's thickness
    // reaches, so the resumed ray still ends on the wall, at every gloss.
    for (float g : {1.0f, 0.4f, 0.0f}) {
        glossValue=g;
        auto pole=trace(2,0.5f,false,1920.0f,0.1f);
        require(pole.hit && std::abs(pole.z-wall)<0.1f,"rough thickness accepted a coarse silhouette");
        ++cases;
    }
    glossValue=1.0f;
    std::printf("%d synthetic traversal cases passed\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-trace-test-') as directory:
    directory = Path(directory)
    source = directory / 'test.cpp'
    # SSRStepCount is a free function: it goes before trace(), not into it.
    source.write_text(prefix.replace('struct Result {', steps_fn + 'struct Result {', 1) + body + suffix)
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    for steps, refine in ((32, 8), (128, 16)):
        exe = directory / f'test-{steps}-{refine}'
        subprocess.run(compiler + ['-std=c++17', '-O0', f'-DNUM_STEPS={steps}',
                       f'-DNUM_REFINE_STEPS={refine}', str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
