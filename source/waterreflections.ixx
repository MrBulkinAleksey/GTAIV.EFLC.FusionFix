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

export module waterreflections;

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

// SSR on the water, its mask and the rain's rings on it.

static constexpr RenderStateValue kWaterSSRRenderStates[] =
{
    { D3DRS_ZENABLE,          FALSE },
    { D3DRS_ZWRITEENABLE,     FALSE },
    { D3DRS_ALPHATESTENABLE,  FALSE },
    { D3DRS_STENCILENABLE,    FALSE },
    { D3DRS_FOGENABLE,        FALSE },
    { D3DRS_CLIPPING,         FALSE },
    { D3DRS_CULLMODE,         D3DCULL_NONE },
    { D3DRS_ALPHABLENDENABLE, TRUE },
    { D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
    { D3DRS_BLENDOP,          D3DBLENDOP_ADD },
    { D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA },
    { D3DRS_DESTBLEND,        D3DBLEND_ONE },
    // Additive and RGB only, so a miss adds nothing and the scene alpha is untouched.
    { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE },
};

// Copies the bound render target into WaterMaskTex[index], (re)creating both copies in
// its size and format when needed.
export bool CopyRenderTargetToWaterMask(int index)
{
    auto& R = PostFxResources;
    IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
    IDirect3DSurface9* rt = nullptr;
    if (!pDevice || FAILED(pDevice->GetRenderTarget(0, &rt)) || !rt)
        return false;

    D3DSURFACE_DESC desc = {};
    rt->GetDesc(&desc);
    if (R.WaterMaskTex[0] && (desc.Width != R.WaterMaskDesc.Width || desc.Height != R.WaterMaskDesc.Height ||
        desc.Format != R.WaterMaskDesc.Format))
        R.ReleaseWaterMask();

    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i)
    {
        if (!R.WaterMaskTex[i])
            ok = SUCCEEDED(RealDevice(pDevice)->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                                  D3DPOOL_DEFAULT, &R.WaterMaskTex[i], nullptr));
    }
    if (ok)
    {
        R.WaterMaskDesc = desc;
        IDirect3DSurface9* dst = nullptr;
        ok = SUCCEEDED(R.WaterMaskTex[index]->GetSurfaceLevel(0, &dst)) &&
             SUCCEEDED(pDevice->StretchRect(rt, nullptr, dst, nullptr, D3DTEXF_NONE));
        SAFE_RELEASE(dst);
    }
    else
    {
        R.ReleaseWaterMask();
    }

    SAFE_RELEASE(rt);
    return ok;
}

// Whether the bound render target is the size of the screen, as the main scene's is.
export bool RenderTargetIsScreenSized()
{
    auto& R = PostFxResources;
    IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
    IDirect3DSurface9* rt = nullptr;
    if (!pDevice || !R.SSRSurf || FAILED(pDevice->GetRenderTarget(0, &rt)) || !rt)
        return false;
    D3DSURFACE_DESC desc = {}, screen = {};
    rt->GetDesc(&desc);
    R.SSRSurf->GetDesc(&screen);
    SAFE_RELEASE(rt);
    return desc.Width == RenderScale::ToRenderWidth(screen.Width) && desc.Height == RenderScale::ToRenderHeight(screen.Height);
}

// The rain's rings on open water (water_rain_rings.patch, c178), set right before the water is
// drawn, through the game's device wrapper and the real device alike: set on the real device
// alone at the end of the G-buffer pass, the water never saw them. x their strength while it
// rains (WetGroundDebug 2: four times full, rain or not), y the clock, zw their fade from 25 to 40 m: the
// water is mostly seen from a quay, further off than puddles.
static std::array<float, 4> WaterRainRings()
{
    auto& R = PostFxResources;
    const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
    const float rain = CWeather::Rain ? std::clamp(*CWeather::Rain / 0.7f, 0.0f, 1.0f) : 0.0f;
    float rings = R.fWetGround > 0.0f && R.WetWeatherEnabled() ? rain * R.fWetGroundRipples : 0.0f;
    if (R.WetGroundDebug() == 2)
        rings = 4.0f; // unmissable, in any weather
    return { rings, float(std::fmod(seconds, 1000.0)), -1.0f / 15.0f, 40.0f / 15.0f };
}

export void SetWaterRainRings()
{
    auto pDevice = rage::grcDevice::GetD3DDevice();
    if (!pDevice)
        return;
    const auto c178 = WaterRainRings();
    pDevice->SetPixelShaderConstantF(178, c178.data(), 1);
    std::memcpy(PostFxResources.WaterRingsLast, c178.data(), sizeof(PostFxResources.WaterRingsLast));
    ++PostFxResources.nWaterRingDraws;
    if (auto real = RealDevice(pDevice); real != pDevice)
        real->SetPixelShaderConstantF(178, c178.data(), 1);
}

export void RenderWaterReflections()
{
    auto& R = PostFxResources;

    if (!R.SSREnabled() || !R.SSREffect || !R.mDepthRT || !R.SSRHistoryTex)
        return;
    if (!R.bSSRReprojValid || R.fSSRWaterIntensity <= 0.0f)
        return;

    IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
    rage::grcViewport* vp = rage::GetCurrentViewport();
    if (!pDevice || !vp)
        return;

    const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;

    float waterLevel = (R.pWaterLevel ? *R.pWaterLevel : 0.0f) + R.fSSRWaterLevelOffset;

    if (viewInv.m[3][2] <= waterLevel)
        return;

    D3DMATRIX proj = *(D3DMATRIX*)vp->mProjectionMatrix;
    const float axisSign[3] = { -1.0f, 1.0f, (proj._34 < 0.0f) ? -1.0f : 1.0f };

    D3DXVECTOR4 plane(viewInv.m[0][2] * axisSign[0],
                      viewInv.m[1][2] * axisSign[1],
                      viewInv.m[2][2] * axisSign[2],
                      viewInv.m[3][2] - waterLevel);

    float width = float(RenderScale::ToRenderWidth(uint32_t(vp->mWidth)));
    float height = float(RenderScale::ToRenderHeight(uint32_t(vp->mHeight)));

    auto& h = R.SSREffectHandles;
    ID3DXEffect* effect = R.SSREffect;

    effect->SetTexture(h.DepthTex2D, LightingDepth());
    effect->SetTexture(h.HistoryTex2D, R.SSRHistoryTex->mD3DTexture);

    SetTargetSize(effect, h, proj, width, height);
    SetDepthRange(effect, h, vp->mNearClip, vp->mFarClip);

    // The history and its depth come from one fog pass, this frame's if it already ran.
    if (R.bSSRHistoryThisFrame)
    {
        D3DXMATRIX viewProj;
        MatrixMultiply(viewProj, *(const D3DXMATRIX*)vp->mViewMatrix, *(const D3DXMATRIX*)vp->mProjectionMatrix);
        D3DXVECTOR4 rows[4];
        ViewToClipRows(vp, viewProj, rows);
        effect->SetVectorArray(h.vec4ViewToPrevClip, rows, 4);
    }
    else
        effect->SetVectorArray(h.vec4ViewToPrevClip, R.SSRReprojRows, 4);
    effect->SetVector(h.vec4WaterPlane, &plane);
    effect->SetTexture(h.PrevDepthTex2D, R.PreAlphaDepthCopyRT ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
    effect->SetFloat(h.fUsePrevDepth, R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture ? 1.0f : 0.0f);
    SetPrevDepthRange(effect, h);

    // Without both copies the pass falls back to the whole water plane.
    const bool waterMask = R.bWaterMaskCaptured && CopyRenderTargetToWaterMask(1);
    effect->SetTexture(h.PreWaterTex2D, waterMask ? R.WaterMaskTex[0] : nullptr);
    effect->SetTexture(h.PostWaterTex2D, waterMask ? R.WaterMaskTex[1] : nullptr);
    effect->SetFloat(h.fUseWaterMask, waterMask ? 1.0f : 0.0f);

    D3DXVECTOR4 toView[3];
    WorldToViewRows(vp, toView);
    effect->SetVectorArray(h.vec4WaterToView, toView, 3);

    D3DXVECTOR4 worldX(toView[0].x, toView[1].x, toView[2].x, viewInv.m[3][0]);
    D3DXVECTOR4 worldY(toView[0].y, toView[1].y, toView[2].y, viewInv.m[3][1]);
    effect->SetVector(h.vec4WaterWorldX, &worldX);
    effect->SetVector(h.vec4WaterWorldY, &worldY);

    effect->SetFloat(h.fMaxDistance, R.fSSRMaxDistance);
    effect->SetFloat(h.fThickness, R.fSSRThickness);
    effect->SetFloat(h.fEdgeFade, R.fSSREdgeFade);
    effect->SetFloat(h.fWaterIntensity, R.fSSRWaterIntensity);
    effect->SetFloat(h.fWaterBlur, R.fSSRWaterBlur);

    // Put back in the reverse order as the function ends; the reflection draws into the target it found.
    SavedVertexInput savedVertexInput(pDevice);
    SavedTargets savedTargets(pDevice, false);

    pDevice->SetDepthStencilSurface(nullptr);
    pDevice->SetStreamSource(0, nullptr, 0, 0);
    pDevice->SetVertexDeclaration(nullptr);
    pDevice->SetFVF(kScreenVertexFVF);

    const ScreenQuad screenQuad = ScreenQuad::Full(width, height);

    UINT passes = 0;
    SavedTextures savedTextures(pDevice);
    SetTextureBoth(pDevice, 3, nullptr);

    // The water's surface, as the game's water shader had it on s0
    IDirect3DBaseTexture9* surface = savedTextures.Texture(0);
    effect->SetTexture(h.SurfaceTex2D, surface);
    effect->SetFloat(h.fWaterNormalStrength, surface ? R.fSSRWaterNormalStrength : 0.0f);
    // The rain's rings, as the game's water shader takes them (WaterRainRings): its reflection is
    // drawn over by this one, which hid them.
    {
        const auto rings = WaterRainRings();
        R.nWaterSsrSurface = surface ? 1 : 0;
        const D3DXVECTOR4 v(surface ? rings[0] : 0.0f, rings[1], rings[2], rings[3]);
        effect->SetVector(h.vec4WaterRings, &v);
    }

    SavedShaderConstants savedConstants(pDevice);
    AppliedRenderStates renderStates(pDevice, kWaterSSRRenderStates);
    AppliedSamplerStates samplerStates(pDevice);

    // The shaders back on both devices, D3DX saves nothing (see SavedShaders); the rest is saved above
    SavedShaders shaders(pDevice);
    effect->SetTechnique(h.techSSRWater);
    effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
    effect->BeginPass(0);
    effect->CommitChanges();
    BindEffectSamplers(pDevice, effect);
    screenQuad.Draw(pDevice);
    effect->EndPass();
    effect->End();
}
