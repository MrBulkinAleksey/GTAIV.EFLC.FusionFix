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

export module contactshadows;

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

// Contact shadows towards the sun (SSR.fx's contact passes) for deferred lighting.

// Before deferred lighting, next to SSR: contact shadows towards the sun, smoothed into
// ContactTex and accumulated over frames into ContactAccumTex, for deferred_lighting (see
// BindLightingInputs). Independent of SSR being on.
export void RenderContactShadows()
{
    auto& R = PostFxResources;
    R.bContactValid = false;
    R.ContactResult = nullptr;
    // The history stays usable only if this frame accumulates too; any return below drops it.
    const bool accumWasValid = FrameHistory::CanReproject(R.nContactAccumFrame);
    R.nContactAccumFrame = 0;

    // For the light shaders, whatever becomes of the sun's pass below.
    {
        rage::grcViewport* camera = rage::GetCurrentViewport();
        // Only with every night shadow on in the menu (Extra Night Shadows with vehicle night
        // shadows): with fewer, cars have no night shadow for them to meet, only a dark halo.
        const bool local = camera && R.bLocalContactShadows && R.ContactShadowsEnabled() && R.AllNightShadowsEnabled() &&
                           R.fLocalContactShadowIntensity > 0.0f;
        const D3DMATRIX proj = camera ? *(const D3DMATRIX*)camera->mProjectionMatrix : D3DMATRIX{};
        const float consts[12] =
        {
            R.fLocalContactShadowLength, R.fLocalContactShadowThickness, R.fLocalContactShadowMaxDistance, R.fLocalContactShadowIntensity,
            proj._34, 0.0f, 0.0f, local ? 12345.0f : 0.0f,
            proj._11, proj._22, proj._31, proj._32,
        };
        memcpy(R.LocalContactShadowConsts, consts, sizeof(consts));

    }

    IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
    rage::grcViewport* vp = rage::GetCurrentViewport();
    auto& h = R.SSREffectHandles;
    ID3DXEffect* effect = R.SSREffect;
    if (!R.ContactShadowsEnabled() || R.fContactShadowIntensity <= 0.0f || !pDevice || !vp || !effect ||
        !h.techContactShadows || !R.mDepthRT || !R.ContactRawSurf || !R.ContactSurf)
        return;

    // gDirectionalLight is a RAGE global, and globals keep the same register in every
    // shader, so c17 holds the directional light the last lit draw used: the direction
    // the light travels, the reverse of the direction towards the sun.
    float light[4] = {};
    if (FAILED(pDevice->GetPixelShaderConstantF(17, light, 1)))
        return;
    const float lightLen = sqrtf(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    if (!(lightLen > 0.9f && lightLen < 1.1f))
        return;
    // With the sun below the horizon deferred_lighting, which lights by dot(n, -c17), shows
    // nothing, and the whole screen pass cost a frame or so for no shadow. The colour in c18
    // is no test: here it can still hold another shader's constant, and skipped the pass by day.
    if (-light[2] / lightLen <= 0.0f)
        return;
    // At night c17 is the moon's. ContactShadowsMoon 0: no contact shadows while its cascades are left out
    // (CascadeShadowsAtNight 0, the sun below CascadeSunElevation), which saves the screen pass; 1 (default)
    // keeps them, the moon's only shading of what stands on the ground then.
    if (!R.bContactShadowsMoon && !RenderSkip::bCascadesAtNight && RenderSkip::SunIsLow())
        return;

    const float width = float(RenderScale::ToRenderWidth(uint32_t(vp->mWidth)));
    const float height = float(RenderScale::ToRenderHeight(uint32_t(vp->mHeight)));
    const D3DMATRIX proj = *(D3DMATRIX*)vp->mProjectionMatrix;
    D3DXVECTOR4 toView[3];
    WorldToViewRows(vp, toView);
    D3DXVECTOR4 sun(0.0f, 0.0f, 0.0f, 1.0f);
    for (int row = 0; row < 3; ++row)
        (&sun.x)[row] = -(toView[row].x * light[0] + toView[row].y * light[1] + toView[row].z * light[2]) / lightLen;

    const bool hasNormals = R.mNormalRT && R.mNormalRT->mD3DTexture;
    effect->SetTexture(h.DepthTex2D, LightingDepth());
    if (hasNormals)
        effect->SetTexture(h.NormalTex2D, R.mNormalRT->mD3DTexture);
    effect->SetFloat(h.fUseGBufferNormals, (hasNormals && R.bSSRGBufferNormals) ? 1.0f : 0.0f);
    effect->SetVectorArray(h.vec4WaterToView, toView, 3);
    SetTargetSize(effect, h, proj, width, height);
    SetDepthRange(effect, h, vp->mNearClip, vp->mFarClip);
    effect->SetVector(h.vec4SunView, &sun);
    effect->SetFloat(h.fStepJitter, R.bContactShadowStepJitter ? 1.0f : 0.0f);
    const bool temporal = R.fContactTemporalBlend > 0.0f && h.techContactTemporal && R.ContactAccumSurf[0] &&
                          R.ContactAccumSurf[1];
    SetNoiseOffset(effect, temporal);
    effect->SetFloat(h.fCSLength, R.fContactShadowLength);
    effect->SetFloat(h.fCSThickness, R.fContactShadowThickness);
    effect->SetFloat(h.fCSMaxViewDistance, R.fContactShadowMaxDistance);
    effect->SetFloat(h.fCSIntensity, R.fContactShadowIntensity);

    IDirect3DSurface9* rt0 = nullptr;
    IDirect3DSurface9* ds = nullptr;
    D3DVIEWPORT9 oldViewport;
    SavedVertexInput savedVertexInput(pDevice);
    pDevice->GetRenderTarget(0, &rt0);
    pDevice->GetDepthStencilSurface(&ds);
    pDevice->GetViewport(&oldViewport);

    IDirect3DBaseTexture9* oldTextures[kSSRTextureSlots] = {};
    DWORD savedRenderStates[std::size(kSSRRenderStates)] = {};
    DWORD savedSamplerStates[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};
    for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
        pDevice->GetTexture(slot, &oldTextures[slot]);
    pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
    pDevice->GetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
    for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
    {
        pDevice->GetRenderState(kSSRRenderStates[i].state, &savedRenderStates[i]);
        pDevice->SetRenderState(kSSRRenderStates[i].state, kSSRRenderStates[i].value);
    }
    for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
        for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
        {
            pDevice->GetSamplerState(slot, kSSRSamplerStates[i].state, &savedSamplerStates[slot][i]);
            pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, kSSRSamplerStates[i].value);
        }

    pDevice->SetDepthStencilSurface(nullptr);
    pDevice->SetStreamSource(0, nullptr, 0, 0);
    pDevice->SetVertexDeclaration(nullptr);
    pDevice->SetFVF(kScreenVertexFVF);
    D3DVIEWPORT9 vpDesc = {};
    vpDesc.Width = DWORD(width);
    vpDesc.Height = DWORD(height);
    vpDesc.MaxZ = 1.0f;
    pDevice->SetViewport(&vpDesc);

    // At half size the march, the smoothing and the accumulation run on the half size targets, with the pixel
    // size and reconstruction basis of that size, and the result is brought up to full size last, as for the
    // indirect light. Should the half size march target be missing, the full size march goes on its own: the
    // smoothing and accumulation targets are of the half size.
    const bool halfTargets = R.bContactShadowsHalfRes;
    const bool half = halfTargets && R.ContactRawHalfSurf && h.techContactUpsample;
    const bool smooth = half || !halfTargets;
    const float passWidth = half ? float(DWORD(width) / 2) : width;
    const float passHeight = half ? float(DWORD(height) / 2) : height;
    if (half)
        SetTargetSize(effect, h, proj, passWidth, passHeight);
    DrawEffectPass(pDevice, effect, h.techContactShadows, half ? R.ContactRawHalfSurf : R.ContactRawSurf, passWidth, passHeight, kProfContactMarch);

    // The same depth aware smoothing SSR uses; the raw result has alpha 1 everywhere, so
    // it is a plain weighted blur. The radius is in full size pixels.
    IDirect3DTexture9* result = half ? R.ContactRawHalfTex->mD3DTexture : R.ContactRawTex->mD3DTexture;
    if (smooth && R.fSSRDenoiseRadius > 0.0f && h.techSSRDenoise)
    {
        effect->SetTexture(h.SSRResultTex2D, result);
        effect->SetFloat(h.fDenoiseRadius, half ? R.fSSRDenoiseRadius * 0.5f : R.fSSRDenoiseRadius);
        effect->SetFloat(h.fDenoiseSSROnly, 0.0f);
        DrawEffectPass(pDevice, effect, h.techSSRDenoise, R.ContactSurf, passWidth, passHeight, kProfContactDenoise);
        result = R.ContactTex->mD3DTexture;
    }

    if (temporal && smooth)
    {
        // Without a history to reproject the blend is 0, any camera does
        D3DXMATRIX prevViewProj;
        if (accumWasValid)
            prevViewProj = FrameHistory::Previous().ViewProjection;
        else
            MatrixMultiply(prevViewProj, *(const D3DXMATRIX*)vp->mViewMatrix, *(const D3DXMATRIX*)vp->mProjectionMatrix);
        D3DXVECTOR4 reprojRows[4];
        ViewToClipRows(vp, prevViewProj, reprojRows);
        // Last frame's fog pass copied its depth; this frame's has not run yet.
        const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;

        const int prev = R.nContactAccumIndex, next = prev ^ 1;
        effect->SetTexture(h.SSRResultTex2D, result);
        effect->SetTexture(h.SSRAccumTex2D, R.ContactAccumTex[prev]->mD3DTexture);
        effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
        effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
        SetPrevDepthRange(effect, h);
        effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
        BindMotionVectors(effect, accumWasValid);
        effect->SetFloat(h.fTemporalBlend, accumWasValid ? R.fContactTemporalBlend : 0.0f);
        DrawEffectPass(pDevice, effect, h.techContactTemporal, R.ContactAccumSurf[next], passWidth, passHeight, kProfContactTemporal);

        result = R.ContactAccumTex[next]->mD3DTexture;
        R.nContactAccumIndex = next;
        R.nContactAccumFrame = FrameHistory::Frame();
    }

    if (half)
    {
        SetTargetSize(effect, h, proj, width, height);
        effect->SetTexture(h.SSRResultTex2D, result);
        DrawEffectPass(pDevice, effect, h.techContactUpsample, R.ContactRawSurf, width, height, kProfContactUpsample);
        result = R.ContactRawTex->mD3DTexture;
    }
    R.ContactResult = result;

    if (R.SSRDebugMode() == R.kContactDebugMode && R.SSRDebugSurf && h.techSSRDebug)
    {
        effect->SetTexture(h.SSRResultTex2D, result);
        effect->SetFloat(h.fDebugMode, float(R.kContactDebugMode));
        DrawEffectPass(pDevice, effect, h.techSSRDebug, R.SSRDebugSurf, width, height);
        R.bSSRDebugValid = true;
    }

    for (size_t i = 0; i < std::size(kSSRRenderStates); ++i)
        pDevice->SetRenderState(kSSRRenderStates[i].state, savedRenderStates[i]);
    for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
        for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
            pDevice->SetSamplerState(slot, kSSRSamplerStates[i].state, savedSamplerStates[slot][i]);
    pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
    pDevice->SetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
    for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
    {
        SetTextureBoth(pDevice, slot, oldTextures[slot]);
        SAFE_RELEASE(oldTextures[slot]);
    }
    pDevice->SetRenderTarget(0, rt0);
    pDevice->SetDepthStencilSurface(ds);
    pDevice->SetViewport(&oldViewport);
    SAFE_RELEASE(rt0);
    SAFE_RELEASE(ds);

    R.bContactValid = true;
}
