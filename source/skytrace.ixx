module;

#include <common.hxx>
#include <d3dx9tex.h>
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
import d3dx9_43;
import vtablepatch;

// [DEBUG] SkyTrace: what the sky dome's draws (gta_atmoscatt_clouds, within postfx's DrawSky) are drawn with, to
// GTAIV.EFLC.FusionFix.SkyTrace.log: the shaders, samplers 0 to 5 (the moon's technique reads the clouds' noise from s3-s5) with
// their textures and states, and the vertex (c64-c76) and pixel (c64-c87) constants that hold the sky's parameters,
// read back from the device at each draw. The first draw of each kind is written whole, later ones only where they
// differ from the draw before them, so a value that changes as the camera turns shows up.
export namespace SkyTrace
{
    bool bEnabled = false;
    bool bInSky = false;
}

namespace SkyTrace
{
    constexpr UINT kVSFirst = 64, kVSCount = 13, kPSFirst = 64, kPSCount = 24, kSamplers = 6;
    constexpr D3DSAMPLERSTATETYPE kStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER };

    constexpr D3DRENDERSTATETYPE kRenderStates[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_COLORWRITEENABLE,
                                                     D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE };

    struct DrawState
    {
        DWORD render[std::size(kRenderStates)] = {};
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

    // Ctrl+Shift+F6 saves what the sky's calls of the next frame leave in the target: the sky as the game drew it,
    // before the fog, the post effects and the upscaler, to set beside a screenshot taken at the same time.
    bool captureWasDown = false;
    int captureArmed = 0, captureCount = 0, captureCall = 0;

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
        if (vs) vs->Release();
        if (ps) ps->Release();
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
            if (texture) texture->Release();
            for (size_t j = 0; j < std::size(kStates); ++j)
                device->GetSamplerState(i, kStates[j], &s.states[i][j]);
        }
        for (size_t j = 0; j < std::size(kRenderStates); ++j)
            device->GetRenderState(kRenderStates[j], &s.render[j]);
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
        if (!before || std::memcmp(before->render, s.render, sizeof(s.render)) != 0)
        {
            std::snprintf(buf, sizeof(buf), " blend %lu src %lu dest %lu write %lx z %lu/%lu alphatest %lu;", s.render[0], s.render[1], s.render[2],
                          s.render[3], s.render[4], s.render[5], s.render[6]);
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

    // The dome's world matrix (gWorld, vertex c0-c3) against the camera the game renders with: the dome is meant to be
    // centred on the eye, and one that lags behind it or sits elsewhere would show the clouds from another place as the
    // camera swings around the player. Written at every draw, as it changes with the camera.
    void WriteGeometry(IDirect3DDevice9* device, int slot)
    {
        float world[16] = {};
        device->GetVertexShaderConstantF(0, world, 4);
        const float* eye = nullptr;
        const float* forward = nullptr;
        if (auto vp = rage::pCurrentViewport ? rage::GetCurrentViewport() : nullptr)
        {
            eye = vp->mViewInverseMatrix[3];
            forward = vp->mViewInverseMatrix[2];
        }
        const float zero[4] = {};
        if (!eye)
            eye = forward = zero;
        // gWorld is row_major in the shader and set transposed: the translation is in the fourth column.
        const float tx = world[3], ty = world[7], tz = world[11];
        FusionLog::Write("SkyTrace", "Geometry", "frame %u draw %d world x (%g %g %g) y (%g %g %g) z (%g %g %g) t (%g %g %g) row3 (%g %g %g %g) eye (%g %g %g) forward (%g %g %g) eye - t (%g %g %g)\n",
                         frame, slot, world[0], world[1], world[2], world[4], world[5], world[6], world[8], world[9], world[10], tx, ty, tz,
                         world[12], world[13], world[14], world[15], eye[0], eye[1], eye[2], forward[0], forward[1], forward[2],
                         eye[0] - tx, eye[1] - ty, eye[2] - tz);
        ++lines;
    }

    void OnDraw(IDirect3DDevice9* device)
    {
        // Some 40000 lines are plenty to turn the camera around in; the log stops there.
        if (!bInSky || lines > 40000)
            return;
        int slot = drawInSky++;
        if (slot >= kSlots)
            return;
        DrawState s;
        Read(device, s);
        Write(slot, s, seen[slot] ? &last[slot] : nullptr);
        WriteGeometry(device, slot);
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
            FusionLog::Write("SkyTrace", "Init", "tracing the sky's draws: samplers 0-5, blending, vertex c%u-c%u, pixel c%u-c%u\n",
                             kVSFirst, kVSFirst + kVSCount - 1, kPSFirst, kPSFirst + kPSCount - 1);
        }
        ++frame;
        drawInSky = 0;
        bInSky = true;

        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
            (GetAsyncKeyState(VK_F6) & 0x8000);
        if (down && !captureWasDown && !captureArmed)
        {
            // The main view's sky and the water reflection's take a call each; a few calls catch both.
            captureArmed = 4;
            captureCall = 0;
            ++captureCount;
        }
        captureWasDown = down;
    }

    void End()
    {
        bInSky = false;
        if (!captureArmed)
            return;
        --captureArmed;
        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        IDirect3DSurface9* target = nullptr;
        if (!device || FAILED(device->GetRenderTarget(0, &target)) || !target)
            return;
        D3DSURFACE_DESC desc{};
        target->GetDesc(&desc);
        const int call = captureCall++;
        char name[64];
        std::snprintf(name, sizeof(name), "SkyCapture%d_%d", captureCount, call);
        const auto path = FusionLog::PathFor(name, L".png");
        const HRESULT hr = D3DXSaveSurfaceToFileW(path.c_str(), D3DXIFF_PNG, target, nullptr, nullptr);
        FusionLog::Write("SkyTrace", "Capture", "capture %d call %d frame %u: target %p %ux%u fmt %lu -> %s (hr %08lx)\n", captureCount, call,
                         frame, static_cast<void*>(target), desc.Width, desc.Height, static_cast<unsigned long>(desc.Format),
                         path.filename().string().c_str(), static_cast<unsigned long>(hr));
        target->Release();
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
