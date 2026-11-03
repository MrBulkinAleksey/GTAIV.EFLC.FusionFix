#!/usr/bin/env python3
"""Exercise the actual transparent texture fallback on allocation/lock failure.

Requires a C++17 compiler; CXX may include arguments. D3D9 is mocked.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'source/postfxresources.ixx').read_text()
start = source.index('    IDirect3DTexture9* TransparentTex()')
end = source.index('\n    int nSSRSteps', start)
body = source[start:end]
prefix = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define SUCCEEDED(hr) ((hr)>=0)
#define D3DFMT_A8R8G8B8 0
#define D3DPOOL_MANAGED 0
struct D3DLOCKED_RECT { void* pBits=nullptr; };
struct IDirect3DTexture9 {
    uint32_t pixel=0xffffffff; bool failLock=false; int releases=0,unlocks=0;
    int LockRect(int,D3DLOCKED_RECT* out,void*,int) { if (failLock) return -1; out->pBits=&pixel; return 0; }
    int UnlockRect(int) { ++unlocks; return 0; }
    void Release() { ++releases; }
};
struct Device {
    IDirect3DTexture9 texture; bool failCreate=false; int creates=0;
    int CreateTexture(int,int,int,int,int,int,IDirect3DTexture9** out,void*) {
        ++creates; if (failCreate) return -1; *out=&texture; return 0;
    }
} device;
namespace rage { struct grcDevice { static Device* GetD3DDevice() { return &device; } }; }
Device* RealDevice(Device* d) { return d; }
struct Resources { IDirect3DTexture9* TransparentTexture=nullptr;
'''
suffix = r'''
};
void require(bool ok,const char* msg) { if (!ok) { std::fprintf(stderr,"%s\n",msg); std::exit(1); } }
int main() {
    Resources r; device.failCreate=true;
    require(!r.TransparentTex(),"failed allocation published a texture");
    device.failCreate=false; device.texture.failLock=true;
    require(!r.TransparentTex() && device.texture.releases==1,"failed lock published uninitialised pixels");
    device.texture.failLock=false;
    require(r.TransparentTex()==&device.texture && device.texture.pixel==0 && device.texture.unlocks==1,"retry did not initialize transparent texture");
    int creates=device.creates; require(r.TransparentTex()==&device.texture && device.creates==creates,"initialized texture not reused");
    std::puts("transparent SSR fallback passed allocation, lock failure and retry checks");
}
'''
with tempfile.TemporaryDirectory(prefix='ssr-transparent-test-') as directory:
    cpp, exe = Path(directory)/'test.cpp', Path(directory)/'test'
    cpp.write_text(prefix + body + suffix)
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + ['-std=c++17', '-O0', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
