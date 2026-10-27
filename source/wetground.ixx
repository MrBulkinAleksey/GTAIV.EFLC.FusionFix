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

export module wetground;

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
import temporallayer;
import upscaler;
import postfxprofiler;
import postfxresources;
import screenquad;
import ssrtrace;
import postfxpass;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxProfiler;
using namespace PostFxPass;

// Wet ground (WetGround.fx) over the G-buffer in rain.

// Wet ground (WetGround.fx), right after CopySceneDepth at the end of the G-buffer pass: copies
// _DEFERRED_GBUFFER_0_ to _2_ and draws them back wet, darker and glossier, with puddles and the
// rain's rings, before any light reads them. Only while anything is wet; the wetness follows
// CWeather::Rain (0.3 drizzle, 0.7 rain, 1.0 a storm) up over WetGroundWetting seconds and down
// over WetGroundDrying. Leaves the device as it found it.
export void RenderWetGround(IDirect3DDevice9* pDevice)
{
    auto& R = PostFxResources;
    auto skip = [&](const char* why) { R.szWetGroundStatus = why; };

    // The wetness, in game time, so it stands still while the game is paused.
    const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
    const float rain = CWeather::Rain ? std::clamp(*CWeather::Rain / 0.7f, 0.0f, 1.0f) : 0.0f;
    {
        const bool first = R.fWetnessTime < 0.0;
        const double dt = first ? 0.0 : std::clamp(seconds - R.fWetnessTime, 0.0, 1.0);
        R.fWetnessTime = seconds;
        if (first)
            R.fWetness = rain; // a game started or loaded in the rain starts wet
        else if (rain > R.fWetness)
            R.fWetness = R.fWetGroundWetting > 0.0f ? (std::min)(rain, R.fWetness + float(dt) / R.fWetGroundWetting) : rain;
        else
            R.fWetness = R.fWetGroundDrying > 0.0f ? (std::max)(rain, R.fWetness - float(dt) / R.fWetGroundDrying) : rain;
    }

    if (R.fWetGround <= 0.0f)
        return skip("off in the ini");
    if (!R.WetWeatherEnabled())
        return skip("off in the menu");
    if (!R.WetGroundEffect)
        return skip("no effect");
    if (R.fWetness <= 0.0f && R.WetGroundDebug() != 1)
        return skip("dry");
    if (R.bInteriorScene && R.WetGroundDebug() != 1)
        return skip("interior");
    if (!pDevice || !R.mDiffuseRT || !R.mNormalRT || !R.mSpecularRT || !R.mMaterialIdRT || !R.mDiffuseRT->mD3DTexture ||
        !R.mNormalRT->mD3DTexture || !R.mSpecularRT->mD3DTexture || !R.mMaterialIdRT->mD3DTexture)
        return skip("no G-buffer");
    auto noise = R.CloudNoiseTex();
    if (!noise)
        return skip("no noise texture");
    rage::grcViewport* vp = rage::GetCurrentViewport();
    if (!vp)
        return skip("no viewport");

    ProfilerScope timed(pDevice, kProfWetGround);
    // The G-buffer's surfaces, and copies of them at their size and format.
    ProfilerMark(pDevice, kProfWetGroundCopies, true);
    IDirect3DTexture9* gbuffer[3] = { R.mDiffuseRT->mD3DTexture, R.mNormalRT->mD3DTexture, R.mSpecularRT->mD3DTexture };
    IDirect3DSurface9* gbufferSurf[3] = {};
    D3DSURFACE_DESC desc[3] = {};
    bool ok = true;
    for (int i = 0; i < 3 && ok; ++i)
        ok = SUCCEEDED(gbuffer[i]->GetSurfaceLevel(0, &gbufferSurf[i])) && SUCCEEDED(gbufferSurf[i]->GetDesc(&desc[i]));
    for (int i = 0; i < 3 && ok; ++i)
    {
        D3DSURFACE_DESC copyDesc = {};
        if (R.WetCopySurf[i] && SUCCEEDED(R.WetCopySurf[i]->GetDesc(&copyDesc)) &&
            (copyDesc.Width != desc[i].Width || copyDesc.Height != desc[i].Height || copyDesc.Format != desc[i].Format))
        {
            SAFE_RELEASE(R.WetCopySurf[i]);
            SAFE_RELEASE(R.WetCopyTex[i]);
        }
        if (!R.WetCopyTex[i])
            ok = SUCCEEDED(RealDevice(pDevice)->CreateTexture(desc[i].Width, desc[i].Height, 1, D3DUSAGE_RENDERTARGET, desc[i].Format,
                                                  D3DPOOL_DEFAULT, &R.WetCopyTex[i], nullptr)) &&
                 SUCCEEDED(R.WetCopyTex[i]->GetSurfaceLevel(0, &R.WetCopySurf[i]));
        if (ok)
            ok = SUCCEEDED(pDevice->StretchRect(gbufferSurf[i], nullptr, R.WetCopySurf[i], nullptr, D3DTEXF_NONE));
    }
    ProfilerMark(pDevice, kProfWetGroundCopies, false);
    if (!ok)
    {
        for (auto& surf : gbufferSurf)
            SAFE_RELEASE(surf);
        return skip("could not copy the G-buffer");
    }
    R.szWetGroundStatus = "drawn";

    ID3DXEffect* effect = R.WetGroundEffect;
    const float width = float(desc[0].Width), height = float(desc[0].Height);
    const D3DMATRIX& proj = *(const D3DMATRIX*)vp->mProjectionMatrix;
    const D3DXVECTOR4 projInfo = ProjInfo(proj, width, height);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4ProjInfo"), &projInfo);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fNearPlane"), vp->mNearClip);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fFarDivNear"), vp->mFarClip / vp->mNearClip);
    {
        const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
        D3DXVECTOR4 toView[3];
        WorldToViewRows(vp, toView);
        const D3DXVECTOR4 worldX(toView[0].x, toView[1].x, toView[2].x, viewInv.m[3][0]);
        const D3DXVECTOR4 worldY(toView[0].y, toView[1].y, toView[2].y, viewInv.m[3][1]);
        const D3DXVECTOR4 worldZ(toView[0].z, toView[1].z, toView[2].z, viewInv.m[3][2]);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4WorldX"), &worldX);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4WorldY"), &worldY);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4WorldZ"), &worldZ);
    }
    // The rings' clock wraps every 1000 s, where a frame's jump goes unseen among the rings.
    const D3DXVECTOR4 wet(R.fWetness * R.fWetGround, R.fWetGroundPuddles, rain, float(std::fmod(seconds, 1000.0)));
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Wet"), &wet);
    const D3DXVECTOR4 shape(1.0f / R.fWetGroundPuddleSize, R.fWetGroundRipples, 15.0f, R.fWetGroundDarkening);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Shape"), &shape);
    const int mask = R.nWetGroundMaterials;
    const D3DXVECTOR4 allow0(float(mask & 1), float((mask >> 1) & 1), float((mask >> 2) & 1), float((mask >> 3) & 1));
    const D3DXVECTOR4 allow1(float((mask >> 4) & 1), float((mask >> 5) & 1), float((mask >> 6) & 1), float((mask >> 7) & 1));
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Allow0"), &allow0);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Allow1"), &allow1);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fDebug"), float(R.WetGroundDebug()));

    // Saved besides what StateBackup keeps around OnGBufferEnd: the textures, sampler states and
    // constants the pass sets.
    static constexpr DWORD kSlots = 6;
    static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER,
                                                               D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
    IDirect3DBaseTexture9* oldTextures[kSlots] = {};
    DWORD savedSamplerStates[kSlots][std::size(kSamplerStates)] = {};
    for (DWORD slot = 0; slot < kSlots; ++slot)
    {
        pDevice->GetTexture(slot, &oldTextures[slot]);
        for (size_t i = 0; i < std::size(kSamplerStates); ++i)
            pDevice->GetSamplerState(slot, kSamplerStates[i], &savedSamplerStates[slot][i]);
    }
    pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
    DWORD colorWrite[3] = {};
    pDevice->GetRenderState(D3DRS_COLORWRITEENABLE1, &colorWrite[1]);
    pDevice->GetRenderState(D3DRS_COLORWRITEENABLE2, &colorWrite[2]);

    static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kStates[] =
    {
        { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
        { D3DRS_STENCILENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_COLORWRITEENABLE, 0x0F },
        { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID },
        { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
    };
    for (auto [state, value] : kStates)
        pDevice->SetRenderState(state, value);
    pDevice->SetRenderState(D3DRS_COLORWRITEENABLE1, 0x0F);
    pDevice->SetRenderState(D3DRS_COLORWRITEENABLE2, 0x0F);
    for (DWORD i = 0; i < 3; ++i)
        pDevice->SetRenderTarget(i, gbufferSurf[i]);
    pDevice->SetRenderTarget(3, nullptr);
    pDevice->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 viewport = { 0, 0, desc[0].Width, desc[0].Height, 0.0f, 1.0f };
    pDevice->SetViewport(&viewport);
    pDevice->SetVertexShader(nullptr);
    pDevice->SetFVF(kScreenVertexFVF);

    ProfilerScope timedPass(pDevice, kProfWetGroundPass);
    UINT passes = 0;
    effect->SetTechnique(effect->GetTechniqueByName("Wet"));
    effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
    effect->BeginPass(0);
    effect->CommitChanges();
    IDirect3DBaseTexture9* textures[kSlots] = { LightingDepth(), R.WetCopyTex[0], R.WetCopyTex[1], R.WetCopyTex[2],
                                                R.mMaterialIdRT->mD3DTexture, noise };
    for (DWORD slot = 0; slot < kSlots; ++slot)
    {
        const bool wrap = slot == 5;
        SetTextureBoth(pDevice, slot, textures[slot]);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSU, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSV, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MAGFILTER, wrap ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MINFILTER, wrap ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MIPFILTER, wrap ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    BindEffectConstantsOnly(pDevice, effect);
    const ScreenQuad quad = ScreenQuad::Full(width, height);
    quad.Draw(pDevice);
    effect->EndPass();
    effect->End();

    for (DWORD slot = 0; slot < kSlots; ++slot)
    {
        SetTextureBoth(pDevice, slot, oldTextures[slot]);
        SAFE_RELEASE(oldTextures[slot]);
        for (size_t i = 0; i < std::size(kSamplerStates); ++i)
            SetSamplerStateBoth(pDevice, slot, kSamplerStates[i], savedSamplerStates[slot][i]);
    }
    pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
    pDevice->SetRenderState(D3DRS_COLORWRITEENABLE1, colorWrite[1]);
    pDevice->SetRenderState(D3DRS_COLORWRITEENABLE2, colorWrite[2]);
    for (auto& surf : gbufferSurf)
        SAFE_RELEASE(surf);
}
