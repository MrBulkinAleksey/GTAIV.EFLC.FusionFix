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

export module sharpening;

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

// Sharpening (CAS) of the shown frame and in post processing.

export bool IsSharpeningActive()
{
    static auto sharpening = FusionFixSettings.GetRef("PREF_SHARPENING");
    auto& R = PostFxResources;
    return sharpening && sharpening->get() > 0 && R.CAS_PS && R.backBuffer && R.FullScreenTex_temp2 && R.FullScreenSurface_temp2;
}

// Low, medium and high; the peak CAS weighs the neighbours with is -1 / lerp(8, 5, sharpness).
static float SharpeningPeak()
{
    static auto sharpening = FusionFixSettings.GetRef("PREF_SHARPENING");
    static constexpr float kSharpness[] = { 0.3f, 0.6f, 1.0f };
    const float sharpness = kSharpness[std::clamp(sharpening->get(), 1, 3) - 1];
    return -1.0f / (8.0f - 3.0f * sharpness);
}

// With frame generation: sharpening of a frame as it is shown (ApplyCASMasked in CAS.hlsl), from frame into target.
// Sharpened before it, the frame generation took the sharpened shadow under a moving car along with the ground and
// showed it twice. Present and hudLess tell the HUD, which is left as it is. On the D3D9 runtime's own device, at
// the end of the frame or, deferred, inside the next one; what it changes is put back. Not with a state block of all
// the state: made and applied twice a frame, that cost DXVK the capture and the binding of everything again.
export bool SharpenShownFrame(IDirect3DDevice9* device, IDirect3DTexture9* frame, IDirect3DTexture9* present, IDirect3DTexture9* hudLess,
    IDirect3DSurface9* target)
{
    static auto sharpening = FusionFixSettings.GetRef("PREF_SHARPENING");
    auto& R = PostFxResources;
    D3DSURFACE_DESC desc{};
    if (!R.CASMasked_PS || !sharpening || sharpening->get() <= 0 || !device || !frame || !present || !hudLess || !target ||
        FAILED(target->GetDesc(&desc)))
        return false;

    static constexpr D3DRENDERSTATETYPE kStates[] =
    {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
        D3DRS_SCISSORTESTENABLE, D3DRS_CULLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE,
    };
    static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] =
    {
        D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
    };
    IDirect3DSurface9* oldTargets[4]{};
    D3DVIEWPORT9 oldViewport{};
    DWORD oldStates[std::size(kStates)]{};
    IDirect3DBaseTexture9* oldTextures[3]{};
    DWORD oldSamplerStates[3][std::size(kSamplerStates)]{};
    float oldConstants[2 * 4]{};
    IDirect3DPixelShader9* oldPixelShader = nullptr;
    IDirect3DVertexShader9* oldVertexShader = nullptr;
    for (DWORD i = 0; i < 4; ++i)
        device->GetRenderTarget(i, &oldTargets[i]);
    device->GetViewport(&oldViewport);
    for (size_t i = 0; i < std::size(kStates); ++i)
        device->GetRenderState(kStates[i], &oldStates[i]);
    for (DWORD i = 0; i < 3; ++i)
    {
        device->GetTexture(2 + i, &oldTextures[i]);
        for (size_t j = 0; j < std::size(kSamplerStates); ++j)
            device->GetSamplerState(2 + i, kSamplerStates[j], &oldSamplerStates[i][j]);
    }
    device->GetPixelShaderConstantF(200, oldConstants, 2);
    device->GetPixelShader(&oldPixelShader);
    device->GetVertexShader(&oldVertexShader);
    SavedVertexInput savedVertexInput(device);

    const float w = float(desc.Width), h = float(desc.Height);
    const float peak[4] = { SharpeningPeak(), 0.0f, 0.0f, 0.0f };
    // scRGB: 1 is 80 nits
    const float paperWhite = HDROutput::IsActive() ? HDROutput::GetPaperWhiteNits() : 0.0f;
    const float masked[4] = { paperWhite > 0.0f ? 80.0f / paperWhite : 0.0f, 0.0f, 1.0f / w, 1.0f / h };

    device->SetRenderTarget(0, target);
    for (DWORD i = 1; i < 4; ++i)
        device->SetRenderTarget(i, nullptr);
    D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
    device->SetViewport(&vp);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DTexture9* textures[] = { frame, present, hudLess };
    for (DWORD i = 0; i < 3; ++i)
    {
        device->SetTexture(2 + i, textures[i]);
        device->SetSamplerState(2 + i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(2 + i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(2 + i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(2 + i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(2 + i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(2 + i, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    device->SetPixelShaderConstantF(200, peak, 1);
    device->SetPixelShaderConstantF(201, masked, 1);
    device->SetPixelShader(R.CASMasked_PS);
    device->SetVertexShader(nullptr);
    device->SetFVF(kScreenVertexFVF);
    const ScreenQuad quad = ScreenQuad::Full(w, h);
    bool drawn = SUCCEEDED(quad.Draw(device));

    for (DWORD i = 0; i < 4; ++i)
        if (oldTargets[i] || i > 0)
            device->SetRenderTarget(i, oldTargets[i]);
    device->SetViewport(&oldViewport);
    for (size_t i = 0; i < std::size(kStates); ++i)
        device->SetRenderState(kStates[i], oldStates[i]);
    for (DWORD i = 0; i < 3; ++i)
    {
        device->SetTexture(2 + i, oldTextures[i]);
        for (size_t j = 0; j < std::size(kSamplerStates); ++j)
            device->SetSamplerState(2 + i, kSamplerStates[j], oldSamplerStates[i][j]);
    }
    device->SetPixelShaderConstantF(200, oldConstants, 2);
    device->SetPixelShader(oldPixelShader);
    device->SetVertexShader(oldVertexShader);
    for (auto& t : oldTargets)
        SAFE_RELEASE(t);
    for (auto& t : oldTextures)
        SAFE_RELEASE(t);
    SAFE_RELEASE(oldPixelShader);
    SAFE_RELEASE(oldVertexShader);
    return drawn;
}

// Sharpening (CAS.hlsl) of the finished frame, after anti-aliasing and before the HUD, from
// FullScreenTex_temp2 into the back buffer. Without anti-aliasing of its own the game's post
// processing drew the frame there already (inTemp2); after FXAA or SMAA, which read it from
// there, the frame is copied into it.
export void ApplySharpening(IDirect3DDevice9* pDevice, IDirect3DPixelShader9* pShader, IDirect3DVertexShader9* vShader, bool inTemp2)
{
    auto& R = PostFxResources;
    ProfilerScope timed(pDevice, kProfPostSharpen);
    if (!inTemp2 && FAILED(pDevice->StretchRect(R.backBuffer, nullptr, R.FullScreenSurface_temp2, nullptr, D3DTEXF_NONE)))
        return;

    const float params[4] = { SharpeningPeak(), 0.0f, 0.0f, 0.0f };

    static constexpr D3DSAMPLERSTATETYPE kStates[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE };
    static constexpr DWORD kValues[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE };
    DWORD saved[std::size(kStates)] = {};
    for (size_t i = 0; i < std::size(kStates); ++i)
    {
        pDevice->GetSamplerState(2, kStates[i], &saved[i]);
        pDevice->SetSamplerState(2, kStates[i], kValues[i]);
    }
    DWORD srgbWrite = FALSE;
    pDevice->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbWrite);
    pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

    // c200 is also the GGX lights' (BindLightingInputs): lights drawn before it is set again would read the peak
    float savedParams[4] = {};
    pDevice->GetPixelShaderConstantF(200, savedParams, 1);

    pDevice->SetRenderTarget(0, R.backBuffer);
    SetTextureBoth(pDevice, 2, R.FullScreenTex_temp2->mD3DTexture);
    pDevice->SetPixelShaderConstantF(200, params, 1);
    pDevice->SetPixelShader(R.CAS_PS);
    pDevice->SetVertexShader(vShader);
    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

    pDevice->SetPixelShaderConstantF(200, savedParams, 1);
    pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite);
    for (size_t i = 0; i < std::size(kStates); ++i)
        pDevice->SetSamplerState(2, kStates[i], saved[i]);
    pDevice->SetPixelShader(pShader);
}
