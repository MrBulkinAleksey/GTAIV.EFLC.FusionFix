module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

export module skytrace;

import common;
import comvars;
import vtablepatch;

// [DEBUG] SkyTrace: what the sky dome's draws (gta_atmoscatt_clouds, within postfx's DrawSky) are drawn with, to
// GTAIV.EFLC.FusionFix.SkyTrace.log: the shaders, samplers 0 to 2 (the stars, the clouds' noise and its detail) with
// their textures and states, and the vertex (c64-c76) and pixel (c64-c83) constants that hold the sky's parameters,
// read back from the device at each draw. The first draw of each kind is written whole, later ones only where they
// differ from the draw before them, so a value that changes as the camera turns shows up.
export namespace SkyTrace
{
    bool bEnabled = false;
    bool bInSky = false;
}

namespace SkyTrace
{
    constexpr UINT kVSFirst = 64, kVSCount = 13, kPSFirst = 64, kPSCount = 20, kSamplers = 3;
    constexpr D3DSAMPLERSTATETYPE kStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER };

    struct DrawState
    {
        void* vs = nullptr;
        void* ps = nullptr;
        void* texture[kSamplers] = {};
        UINT width[kSamplers] = {}, height[kSamplers] = {};
        DWORD format[kSamplers] = {};
        DWORD states[kSamplers][std::size(kStates)] = {};
        float vsc[kVSCount * 4] = {};
        float psc[kPSCount * 4] = {};
    };

    // Indexed by the draw's place within the sky's call, so each draw is compared with its own kind.
    constexpr int kSlots = 8;
    DrawState last[kSlots];
    bool seen[kSlots] = {};
    int drawInSky = 0;
    uint32_t frame = 0, lines = 0;

    HRESULT(__stdcall* RealDrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;

    void Read(IDirect3DDevice9* device, DrawState& s)
    {
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        device->GetVertexShader(&vs);
        device->GetPixelShader(&ps);
        s.vs = vs;
        s.ps = ps;
        SAFE_RELEASE(vs);
        SAFE_RELEASE(ps);
        for (DWORD i = 0; i < kSamplers; ++i)
        {
            IDirect3DBaseTexture9* texture = nullptr;
            device->GetTexture(i, &texture);
            s.texture[i] = texture;
            s.width[i] = s.height[i] = 0;
            s.format[i] = 0;
            if (texture && texture->GetType() == D3DRTYPE_TEXTURE)
            {
                D3DSURFACE_DESC desc{};
                if (SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc)))
                {
                    s.width[i] = desc.Width;
                    s.height[i] = desc.Height;
                    s.format[i] = desc.Format;
                }
            }
            SAFE_RELEASE(texture);
            for (size_t j = 0; j < std::size(kStates); ++j)
                device->GetSamplerState(i, kStates[j], &s.states[i][j]);
        }
        device->GetVertexShaderConstantF(kVSFirst, s.vsc, kVSCount);
        device->GetPixelShaderConstantF(kPSFirst, s.psc, kPSCount);
    }

    bool Differs(float a, float b)
    {
        return std::fabs(a - b) > 1e-4f * (std::max)(1.0f, (std::max)(std::fabs(a), std::fabs(b)));
    }

    void Write(int slot, const DrawState& s, const DrawState* before)
    {
        std::string text;
        char buf[256];
        if (!before || before->vs != s.vs || before->ps != s.ps)
        {
            std::snprintf(buf, sizeof(buf), " vs %p ps %p;", s.vs, s.ps);
            text += buf;
        }
        for (UINT i = 0; i < kSamplers; ++i)
        {
            bool changed = !before || before->texture[i] != s.texture[i] || std::memcmp(before->states[i], s.states[i], sizeof(s.states[i])) != 0;
            if (!changed)
                continue;
            std::snprintf(buf, sizeof(buf), " s%u %p %ux%u fmt %lu addr %lu/%lu filter %lu/%lu/%lu;", i, s.texture[i], s.width[i], s.height[i],
                          s.format[i], s.states[i][0], s.states[i][1], s.states[i][2], s.states[i][3], s.states[i][4]);
            text += buf;
        }
        auto constants = [&](const char* kind, UINT first, UINT count, const float* now, const float* then)
        {
            for (UINT r = 0; r < count; ++r)
            {
                const float* v = now + r * 4;
                bool changed = !then;
                for (int k = 0; k < 4 && !changed; ++k)
                    changed = Differs(v[k], then[r * 4 + k]);
                if (!changed)
                    continue;
                std::snprintf(buf, sizeof(buf), " %s%u (%g %g %g %g);", kind, first + r, v[0], v[1], v[2], v[3]);
                text += buf;
            }
        };
        constants("vc", kVSFirst, kVSCount, s.vsc, before ? before->vsc : nullptr);
        constants("pc", kPSFirst, kPSCount, s.psc, before ? before->psc : nullptr);
        if (text.empty())
            return;
        FusionLog::Write("SkyTrace", "Draw", "frame %u draw %d%s%s\n", frame, slot, before ? " changed:" : " first:", text.c_str());
        ++lines;
    }

    void OnDraw(IDirect3DDevice9* device)
    {
        // Some 20000 lines are plenty to turn the camera around in; the log stops there.
        if (!bInSky || lines > 20000)
            return;
        int slot = drawInSky++;
        if (slot >= kSlots)
            return;
        DrawState s;
        Read(device, s);
        Write(slot, s, seen[slot] ? &last[slot] : nullptr);
        last[slot] = s;
        seen[slot] = true;
    }

    HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count)
    {
        OnDraw(device);
        return RealDrawIndexedPrimitive(device, type, base, minIndex, vertices, start, count);
    }

    HRESULT __stdcall DrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count)
    {
        OnDraw(device);
        return RealDrawPrimitive(device, type, start, count);
    }
}

export namespace SkyTrace
{
    // Around the game's main sky draw (postfx's DrawSky); the device's draws are hooked on the first one.
    void Begin()
    {
        if (!bEnabled)
            return;
        static bool installed = false;
        if (!installed)
        {
            auto device = RageDirect3DDevice9::GetRuntimeDevice();
            if (!device)
                return;
            auto vtable = *reinterpret_cast<void***>(device);
            PatchVTable(vtable, 81, RealDrawPrimitive, &DrawPrimitive);
            PatchVTable(vtable, 82, RealDrawIndexedPrimitive, &DrawIndexedPrimitive);
            installed = true;
            FusionLog::Write("SkyTrace", "Init", "tracing the sky's draws: samplers 0-2, vertex c%u-c%u, pixel c%u-c%u\n",
                             kVSFirst, kVSFirst + kVSCount - 1, kPSFirst, kPSFirst + kPSCount - 1);
        }
        ++frame;
        drawInSky = 0;
        bInSky = true;
    }

    void End()
    {
        bInSky = false;
    }
}

class SkyTraceInit
{
public:
    SkyTraceInit()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            SkyTrace::bEnabled = iniReader.ReadInteger("DEBUG", "SkyTrace", 0) != 0;
        };
    }
} SkyTraceInitInstance;
