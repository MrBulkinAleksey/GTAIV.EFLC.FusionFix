module;

#include <common.hxx>
#include <d3dx9tex.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <regex>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include "FusionLog.hpp"

export module ssrtrace;

import common;
import comvars;
import d3dx9_43;
import framegeneration;
import framehistory;
import hdr;
import natives;
import settings;
import shaders;
import renderscale;
import renderskip;
import temporal;
import upscaler;
import postfxprofiler;
import postfxresources;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxProfiler;

// For now: a trace of SSR's frames, to find why reflections show in the pause menu and not in
// play. Ctrl+Shift+F11 writes the settings and the next kFrames frames into
// GTAIV.EFLC.FusionFix.PostFx.log (PostFx.SSRTrace) next to the plugin: every call of the passes around SSR with its viewport, where it left and what
// D3D returned, what lighting gets on s3, and for the first frames what the SSR targets hold,
// read back from the card. The post fx pass arms and flushes it once a frame; the rest only adds
// lines while it is armed.
namespace SSRTrace
{
    static std::mutex mutex;
    static std::string text;
    static std::atomic<int> framesLeft{0};
    export std::atomic<int> readbacksLeft{0};
    static constexpr int kFrames = 8;
    static bool keyWasDown = false;

    export bool Active() { return framesLeft.load(std::memory_order_relaxed) > 0; }

    export void Line(const char* format, ...)
    {
        if (!Active())
            return;
        char line[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        std::lock_guard lock(mutex);
        text += std::to_string(GetTickCount64()) + " scene " + std::to_string(FrameHistory::Frame()) + " left " +
            std::to_string(framesLeft.load(std::memory_order_relaxed)) + " " + line + "\n";
    }

    static float Half(uint16_t h)
    {
        const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        uint32_t bits;
        if (exponent == 0)
        {
            if (!mantissa) bits = sign;
            else
            {
                float f = std::ldexp(float(mantissa), -24);
                return (h & 0x8000u) ? -f : f;
            }
        }
        else if (exponent == 31)
            bits = sign | 0x7F800000u | (mantissa << 13);
        else
            bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    // What a render target holds, read back from the card: its size and format, the pixels with
    // alpha over 0.01, those not finite, the largest colour and alpha and the mean alpha.
    export void Contents(IDirect3DDevice9* pDevice, const char* name, IDirect3DTexture9* texture)
    {
        if (!Active() || readbacksLeft.load(std::memory_order_relaxed) <= 0)
            return;
        if (!texture)
        {
            Line("  %s: none", name);
            return;
        }
        IDirect3DSurface9* surface = nullptr;
        if (FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface)
        {
            Line("  %s: no surface", name);
            return;
        }
        D3DSURFACE_DESC desc = {};
        surface->GetDesc(&desc);
        IDirect3DSurface9* copy = nullptr;
        HRESULT hr = pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
        if (SUCCEEDED(hr))
            hr = pDevice->GetRenderTargetData(surface, copy);
        D3DLOCKED_RECT locked = {};
        if (SUCCEEDED(hr))
            hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr))
        {
            Line("  %s: %ux%u format %u, read back failed %08x", name, desc.Width, desc.Height, unsigned(desc.Format), unsigned(hr));
            SAFE_RELEASE(copy);
            surface->Release();
            return;
        }
        uint32_t lit = 0, bad = 0;
        float maxColour = 0.0f, maxAlpha = 0.0f;
        double sumAlpha = 0.0;
        const bool isHalf = desc.Format == D3DFMT_A16B16G16R16F, isFixed = desc.Format == D3DFMT_A16B16G16R16;
        for (UINT y = 0; y < desc.Height && (isHalf || isFixed); ++y)
        {
            auto row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch);
            for (UINT x = 0; x < desc.Width; ++x)
            {
                float v[4];
                for (int c = 0; c < 4; ++c)
                    v[c] = isHalf ? Half(row[x * 4 + c]) : row[x * 4 + c] / 65535.0f;
                if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || !std::isfinite(v[3]))
                {
                    ++bad;
                    continue;
                }
                maxColour = (std::max)({ maxColour, v[0], v[1], v[2] });
                maxAlpha = (std::max)(maxAlpha, v[3]);
                sumAlpha += v[3];
                if (v[3] > 0.01f)
                    ++lit;
            }
        }
        copy->UnlockRect();
        copy->Release();
        surface->Release();
        Line("  %s: %ux%u format %u, alpha>0.01 %u, not finite %u, max colour %.3f, max alpha %.3f, mean alpha %.4f%s", name,
             desc.Width, desc.Height, unsigned(desc.Format), lit, bad, maxColour, maxAlpha,
             desc.Width * desc.Height ? sumAlpha / (double(desc.Width) * desc.Height) : 0.0, isHalf || isFixed ? "" : " (format not read)");
    }

    // The first channel of a texture at five points (the centre and halfway to each corner), as stored and as metres
    // through near and far the way SSR.fx decodes the log depth, to compare this frame's depth with last frame's copy.
    export void DepthProbe(IDirect3DDevice9* pDevice, const char* name, IDirect3DTexture9* texture, float nearClip, float farClip)
    {
        if (!Active())
            return;
        IDirect3DSurface9* surface = nullptr;
        if (!texture || FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface)
        {
            Line("  %s probe: no surface", name);
            return;
        }
        D3DSURFACE_DESC desc = {};
        surface->GetDesc(&desc);
        IDirect3DSurface9* copy = nullptr;
        HRESULT hr = pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr);
        if (SUCCEEDED(hr))
            hr = pDevice->GetRenderTargetData(surface, copy);
        D3DLOCKED_RECT locked = {};
        if (SUCCEEDED(hr))
            hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr))
        {
            Line("  %s probe: %ux%u format %u, read back failed %08x", name, desc.Width, desc.Height, unsigned(desc.Format), unsigned(hr));
            SAFE_RELEASE(copy);
            surface->Release();
            return;
        }
        auto read = [&](UINT x, UINT y) -> float
        {
            auto row = static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch;
            switch (desc.Format)
            {
            case D3DFMT_R32F: return reinterpret_cast<const float*>(row)[x];
            case D3DFMT_G32R32F: return reinterpret_cast<const float*>(row)[x * 2];
            case D3DFMT_A32B32G32R32F: return reinterpret_cast<const float*>(row)[x * 4];
            case D3DFMT_R16F: return Half(reinterpret_cast<const uint16_t*>(row)[x]);
            case D3DFMT_G16R16F: return Half(reinterpret_cast<const uint16_t*>(row)[x * 2]);
            case D3DFMT_A16B16G16R16F: return Half(reinterpret_cast<const uint16_t*>(row)[x * 4]);
            case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return row[x * 4 + 2] / 255.0f;
            default: return -1.0f;
            }
        };
        std::string text;
        const float at[5][2] = { { 0.5f, 0.5f }, { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };
        for (auto& p : at)
        {
            const float raw = read(UINT(p[0] * desc.Width), UINT(p[1] * desc.Height));
            const float metres = nearClip > 0.0f ? nearClip * std::pow(farClip / nearClip, raw) : 0.0f;
            char item[64];
            snprintf(item, sizeof(item), " (%.2f,%.2f) %.6f = %.2f m", p[0], p[1], raw, metres);
            text += item;
        }
        copy->UnlockRect();
        copy->Release();
        surface->Release();
        Line("  %s probe: %ux%u format %u:%s", name, desc.Width, desc.Height, unsigned(desc.Format), text.c_str());
    }

    // All four channels of a half float target at five points (the centre and halfway to each corner).
    export void PixelProbe(IDirect3DDevice9* pDevice, const char* name, IDirect3DTexture9* texture)
    {
        if (!Active())
            return;
        IDirect3DSurface9* surface = nullptr;
        if (!texture || FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface)
            return;
        D3DSURFACE_DESC desc = {};
        surface->GetDesc(&desc);
        IDirect3DSurface9* copy = nullptr;
        HRESULT hr = desc.Format == D3DFMT_A16B16G16R16F
            ? pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr) : E_FAIL;
        if (SUCCEEDED(hr))
            hr = pDevice->GetRenderTargetData(surface, copy);
        D3DLOCKED_RECT locked = {};
        if (SUCCEEDED(hr))
            hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr))
        {
            Line("  %s probe: %ux%u format %u, read back failed %08x", name, desc.Width, desc.Height, unsigned(desc.Format), unsigned(hr));
            SAFE_RELEASE(copy);
            surface->Release();
            return;
        }
        std::string text;
        const float at[5][2] = { { 0.5f, 0.5f }, { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };
        for (auto& p : at)
        {
            auto row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(locked.pBits) + UINT(p[1] * desc.Height) * locked.Pitch);
            auto px = row + UINT(p[0] * desc.Width) * 4;
            char item[96];
            snprintf(item, sizeof(item), " (%.2f,%.2f) %.3f %.3f %.3f %.0f", p[0], p[1], Half(px[0]), Half(px[1]), Half(px[2]), Half(px[3]));
            text += item;
        }
        copy->UnlockRect();
        copy->Release();
        surface->Release();
        Line("  %s probe: %ux%u:%s", name, desc.Width, desc.Height, text.c_str());
    }

    // A surface by the render target it belongs to, FusionFix's or the game's, or the full size depth buffer RenderScale
    // stands in with.
    export std::string SurfaceName(IDirect3DSurface9* surface)
    {
        if (!surface)
            return "none";
        for (auto& [name, rt] : rage::grcTextureFactoryPC::RTCache)
        {
            if (!rt)
                continue;
            if (rt->mD3DSurface == surface)
                return name;
            IDirect3DSurface9* level = nullptr;
            if (rt->mD3DTexture && SUCCEEDED(rt->mD3DTexture->GetSurfaceLevel(0, &level)) && level)
            {
                const bool same = level == surface;
                level->Release();
                if (same)
                    return name;
            }
        }
        D3DSURFACE_DESC desc = {};
        surface->GetDesc(&desc);
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%p %ux%u format %u", static_cast<void*>(surface), desc.Width, desc.Height, unsigned(desc.Format));
        return buffer;
    }

    export std::string TextureName(IDirect3DBaseTexture9* texture)
    {
        auto& R = PostFxResources;
        if (!texture) return "none";
        const std::pair<rage::grcRenderTargetPC*, const char*> known[] = {
            { R.SSRTex, "SSRTex" }, { R.SSRHalfTex, "SSRHalfTex" }, { R.SSRDenoisedTex, "SSRDenoisedTex" },
            { R.SSRHalfDenoisedTex, "SSRHalfDenoisedTex" }, { R.SSRAccumTex[0][0], "SSRAccumTex0" }, { R.SSRAccumTex[0][1], "SSRAccumTex1" },
            { R.SSRAccumTex[1][0], "SSRHalfAccumTex0" }, { R.SSRAccumTex[1][1], "SSRHalfAccumTex1" }, { R.SSRTraceTex[0], "SSRTraceTex" },
            { R.SSRTraceTex[1], "SSRHalfTraceTex" }, { R.SSRHistoryTex, "SSRHistoryTex" }, { R.SSRFallbackTex[0], "SSRFallbackTex" },
            { R.SSRFallbackTex[1], "SSRHalfFallbackTex" }, { R.SSRSpreadTex[0], "SSRSpreadTex" }, { R.SSRSpreadTex[1], "SSRHalfSpreadTex" },
            { R.SSRHitDistTex[0], "SSRHitDistTex" }, { R.SSRHitDistTex[1], "SSRHalfHitDistTex" },
            { R.mDepthRT, "depth" }, { R.mSpecularRT, "specular" }, { R.mNormalRT, "normal" }, { R.PreAlphaDepthCopyRT, "prevDepth" } };
        for (auto [rt, name] : known)
            if (rt && rt->mD3DTexture == texture)
                return name;
        // The game's own targets by the name they were created with
        for (auto& [name, rt] : rage::grcTextureFactoryPC::RTCache)
            if (rt && rt->mD3DTexture == texture)
                return "game:" + name;
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%p", static_cast<void*>(texture));
        return buffer;
    }

    // The device state a draw runs with: render target, viewport, the states that could keep it
    // from writing, and what every sampler holds.
    export void State(IDirect3DDevice9* pDevice, const char* what)
    {
        if (!Active())
            return;
        IDirect3DSurface9* rt = nullptr;
        pDevice->GetRenderTarget(0, &rt);
        // The texture whose top level the target is, among FusionFix's own; none for another.
        IDirect3DBaseTexture9* rtTexture = nullptr;
        auto& R = PostFxResources;
        for (auto* known : { R.SSRTex, R.SSRHalfTex, R.SSRDenoisedTex, R.SSRHalfDenoisedTex, R.SSRAccumTex[0][0], R.SSRAccumTex[0][1],
                             R.SSRAccumTex[1][0], R.SSRAccumTex[1][1], R.SSRTraceTex[0], R.SSRTraceTex[1], R.SSRFallbackTex[0],
                             R.SSRFallbackTex[1], R.SSRSpreadTex[0], R.SSRSpreadTex[1] })
        {
            IDirect3DSurface9* level = nullptr;
            if (rt && known && known->mD3DTexture && SUCCEEDED(known->mD3DTexture->GetSurfaceLevel(0, &level)) && level)
            {
                if (level == rt)
                    rtTexture = known->mD3DTexture;
                level->Release();
            }
        }
        D3DVIEWPORT9 view = {};
        pDevice->GetViewport(&view);
        DWORD v[10] = {};
        const D3DRENDERSTATETYPE states[10] = { D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
            D3DRS_ALPHATESTENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE, D3DRS_ZENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE };
        for (int i = 0; i < 10; ++i)
            pDevice->GetRenderState(states[i], &v[i]);
        RECT scissor = {};
        pDevice->GetScissorRect(&scissor);
        std::string samplers;
        for (DWORD slot = 0; slot < 16; ++slot)
        {
            IDirect3DBaseTexture9* t = nullptr;
            pDevice->GetTexture(slot, &t);
            if (t)
            {
                samplers += " s" + std::to_string(slot) + "=" + TextureName(t) + (t == rtTexture ? "(TARGET)" : "");
                t->Release();
            }
        }
        // Whether a texture set now takes: one set on s15 and read back, then put back.
        const char* takes = "?";
        if (rtTexture || R.SSRHistoryTex)
        {
            IDirect3DBaseTexture9* old15 = nullptr;
            pDevice->GetTexture(15, &old15);
            IDirect3DBaseTexture9* probe = R.SSRHistoryTex ? R.SSRHistoryTex->mD3DTexture : rtTexture;
            if (probe == old15)
                probe = nullptr;
            SetTextureBoth(pDevice, 15, probe);
            IDirect3DBaseTexture9* now15 = nullptr;
            pDevice->GetTexture(15, &now15);
            takes = now15 == probe ? "yes" : "NO";
            SAFE_RELEASE(now15);
            SetTextureBoth(pDevice, 15, old15);
            SAFE_RELEASE(old15);
        }
        IDirect3DPixelShader9* ps = nullptr;
        pDevice->GetPixelShader(&ps);
        Line("  set texture takes: %s", takes);
        Line("  state %s: target %s view %ux%u+%u+%u write %x blend %u (%u,%u) test %u scissor %u (%d,%d,%d,%d) stencil %u z %u srgb %u sepalpha %u ps %p;%s",
            what, TextureName(rtTexture).c_str(), unsigned(view.Width), unsigned(view.Height), unsigned(view.X), unsigned(view.Y),
            unsigned(v[0]), unsigned(v[1]), unsigned(v[2]), unsigned(v[3]), unsigned(v[4]), unsigned(v[5]), int(scissor.left), int(scissor.top),
            int(scissor.right), int(scissor.bottom), unsigned(v[6]), unsigned(v[7]), unsigned(v[8]), unsigned(v[9]), static_cast<void*>(ps), samplers.c_str());
        SAFE_RELEASE(ps);
        SAFE_RELEASE(rt);
    }

    // Once a frame, from the post fx pass, which runs in the pause menu too, where the game does
    // not tick: Ctrl+Shift+F11 arms the trace with the settings; it is written once the frames
    // are done.
    export void Tick()
    {
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
            (GetAsyncKeyState(VK_F11) & 0x8000);
        if (down && !keyWasDown && !Active())
        {
            auto& R = PostFxResources;
            auto pref = [](const char* name) { auto p = FusionFixSettings.GetRef(name); return p ? p->get() : -1; };
            {
                std::lock_guard lock(mutex);
                text.clear();
            }
            framesLeft = kFrames;
            readbacksLeft = 2;
            Line("=== trace: menu SSR %d debug %d AO %d SSGI %d contact %d skin %d; ini intensity %.2f max distance %.1f fallback %.2f "
                 "temporal %.2f denoise %.1f; active %ux%u",
                 pref("PREF_SSR"), pref("PREF_SSR_DEBUG"), pref("PREF_SAO"), pref("PREF_SSGI"), pref("PREF_CONTACTSHADOWS"),
                 pref("PREF_SKIN_SSS"), R.fSSRIntensity, R.fSSRMaxDistance, R.fSSRFallback, R.fSSRTemporalBlend, R.fSSRDenoiseRadius,
                 unsigned(rage::grcDevice::ms_nActiveWidth ? *rage::grcDevice::ms_nActiveWidth : 0),
                 unsigned(rage::grcDevice::ms_nActiveHeight ? *rage::grcDevice::ms_nActiveHeight : 0));
            Line("targets: trace %p/%p fallback %p/%p spread %p/%p ssr %p half %p effect %p history %p depth %p",
                 static_cast<void*>(R.SSRTraceSurf[0]), static_cast<void*>(R.SSRTraceSurf[1]), static_cast<void*>(R.SSRFallbackSurf[0]),
                 static_cast<void*>(R.SSRFallbackSurf[1]), static_cast<void*>(R.SSRSpreadSurf[0]), static_cast<void*>(R.SSRSpreadSurf[1]),
                 static_cast<void*>(R.SSRSurf), static_cast<void*>(R.SSRHalfSurf), static_cast<void*>(R.SSREffect),
                 static_cast<void*>(R.SSRHistoryTex), static_cast<void*>(R.mDepthRT));
        }
        keyWasDown = down;
        if (!Active())
            return;
        if (framesLeft.fetch_sub(1) == 1)
        {
            std::lock_guard lock(mutex);
            // Written at once when the trace ends; each line keeps the tick it was taken at.
            FusionLog::WriteText("PostFx", "SSRTrace", text);
            text.clear();
        }
    }
}
