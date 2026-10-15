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

export module indirectlight;

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

// Screen space indirect light (SSGI) for deferred lighting.

// Before deferred lighting, after contact shadows: indirect light at half resolution,
// smoothed and accumulated, into GIResult for deferred_lighting. It reads the scene the fog
// pass copied last frame, with the camera of last frame (FrameHistory).
export void RenderIndirectLight()
{
    auto& R = PostFxResources;
    // What deferred_lighting added last frame, still in its target, null if nothing.
    IDirect3DTexture9* prevGI = R.GIResult;
    R.GIResult = nullptr;

    IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
    rage::grcViewport* vp = rage::GetCurrentViewport();
    auto& h = R.SSREffectHandles;
    ID3DXEffect* effect = R.SSREffect;
    if (!R.SSGIEnabled() || R.fGIIntensity <= 0.0f || !pDevice || !vp || !effect || !h.techSSGI || !h.techSSRTemporal ||
        !R.mDepthRT || !R.SSRHistoryTex || !R.GIRawSurf || !R.GIDenoisedSurf || !R.GIAccumSurf[0] || !R.GIAccumSurf[1])
    {
        R.nGIAccumFrame = 0;
        return;
    }

    if (SSRTrace::Active())
    {
        const auto& cur = FrameHistory::Current();
        const auto& prv = FrameHistory::Previous();
        SSRTrace::Line("gi: vp near %.4f far %.1f; camera frame %u valid %d near %.4f far %.1f; previous frame %u valid %d near %.4f far %.1f; "
                       "cut %d; scene history of %u reprojects %d; accumulation of %u reprojects %d",
                       vp->mNearClip, vp->mFarClip, cur.Frame, int(cur.Valid), cur.Near, cur.Far, prv.Frame, int(prv.Valid), prv.Near, prv.Far,
                       int(FrameHistory::IsCameraCut()), R.nSSRHistoryFrame, int(FrameHistory::CanReproject(R.nSSRHistoryFrame)),
                       R.nGIAccumFrame, int(FrameHistory::CanReproject(R.nGIAccumFrame)));
        auto size = [](rage::grcRenderTargetPC* rt) -> std::string
        {
            D3DSURFACE_DESC d = {};
            if (!rt || !rt->mD3DTexture || FAILED(rt->mD3DTexture->GetLevelDesc(0, &d)))
                return "none";
            return std::to_string(d.Width) + "x" + std::to_string(d.Height);
        };
        SSRTrace::Line("gi sizes: render scale active %d scale %.3f; game viewport %dx%d, screen %dx%d, ToRender %ux%u; depth %s, "
                       "depth copy %s, scene copy %s, history %s; GI raw %s denoised %s accum %s/%s full %s; SSR %s",
                       int(RenderScale::IsActive()), RenderScale::GetScale(), int(vp->mWidth), int(vp->mHeight),
                       rage::grcDevice::ms_nActiveWidth ? int(*rage::grcDevice::ms_nActiveWidth) : 0,
                       rage::grcDevice::ms_nActiveHeight ? int(*rage::grcDevice::ms_nActiveHeight) : 0,
                       RenderScale::ToRenderWidth(uint32_t(vp->mWidth)), RenderScale::ToRenderHeight(uint32_t(vp->mHeight)),
                       size(R.mDepthRT).c_str(), size(R.PreAlphaDepthCopyRT).c_str(), size(R.FullScreenTex_temp1).c_str(),
                       size(R.SSRHistoryTex).c_str(), size(R.GIRawTex).c_str(), size(R.GIDenoisedTex).c_str(),
                       size(R.GIAccumTex[0]).c_str(), size(R.GIAccumTex[1]).c_str(), size(R.GIFullTex).c_str(), size(R.SSRTex).c_str());
        SSRTrace::DepthProbe(pDevice, "gi depth", LightingDepth(), vp->mNearClip, vp->mFarClip);
        SSRTrace::DepthProbe(pDevice, "gi previous depth", R.PreAlphaDepthCopyRT ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr,
                             prv.Near, prv.Far);
    }

    // The rays read last frame's scene: none on the first frame on, after a cut of the camera it shows another shot
    if (!FrameHistory::CanReproject(R.nSSRHistoryFrame))
    {
        R.nGIAccumFrame = 0;
        return;
    }
    D3DXVECTOR4 reprojRows[4];
    ViewToClipRows(vp, FrameHistory::Previous().ViewProjection, reprojRows);

    const float fullWidth = float(RenderScale::ToRenderWidth(uint32_t(vp->mWidth)));
    const float fullHeight = float(RenderScale::ToRenderHeight(uint32_t(vp->mHeight)));
    const float width = float(DWORD(fullWidth) / 2);
    const float height = float(DWORD(fullHeight) / 2);
    const D3DMATRIX proj = *(D3DMATRIX*)vp->mProjectionMatrix;
    D3DXVECTOR4 toView[3];
    WorldToViewRows(vp, toView);
    SetTargetSize(effect, h, proj, width, height);

    const bool hasNormals = R.mNormalRT && R.mNormalRT->mD3DTexture;
    const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;
    effect->SetTexture(h.DepthTex2D, LightingDepth());
    effect->SetTexture(h.HistoryTex2D, R.SSRHistoryTex->mD3DTexture);
    effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
    effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
    SetPrevDepthRange(effect, h);
    if (hasNormals)
        effect->SetTexture(h.NormalTex2D, R.mNormalRT->mD3DTexture);
    effect->SetFloat(h.fUseGBufferNormals, (hasNormals && R.bSSRGBufferNormals) ? 1.0f : 0.0f);
    effect->SetVectorArray(h.vec4WaterToView, toView, 3);
    effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
    SetDepthRange(effect, h, vp->mNearClip, vp->mFarClip);
    SetNoiseOffset(effect, R.fGITemporalBlend > 0.0f);
    effect->SetFloat(h.fGIRayLength, R.fGIRayLength);
    effect->SetFloat(h.fGIThickness, R.fGIThickness);
    effect->SetFloat(h.fGIMaxViewDistance, R.fGIMaxDistance);
    effect->SetFloat(h.fGIIntensity, R.fGIIntensity);
    effect->SetFloat(h.fGIMaxBrightness, R.fGIMaxBrightness);
    effect->SetFloat(h.fGIOcclusion, R.fGIOcclusion);
    // Last frame's scene holds the surfaces' colour times the indirect light at fGIIntensity;
    // the rays take all but an intensity 1 share of it back out, see SSGI_PS.
    const bool albedo = R.mDiffuseRT && R.mDiffuseRT->mD3DTexture;
    effect->SetTexture(h.AlbedoTex2D, albedo ? R.mDiffuseRT->mD3DTexture : nullptr);
    effect->SetTexture(h.AlbedoLinearTex2D, albedo ? R.mDiffuseRT->mD3DTexture : nullptr);
    effect->SetTexture(h.GIPrevTex2D, prevGI);
    // The occlusion deferred_lighting already takes off the ambient, so SSGI_PS does not take it off again.
    const bool specular = R.mSpecularRT && R.mSpecularRT->mD3DTexture;
    effect->SetTexture(h.SpecularTex2D, specular ? R.mSpecularRT->mD3DTexture : nullptr);
    effect->SetFloat(h.fGIRespectAO, specular ? 1.0f : 0.0f);
    effect->SetFloat(h.fGIFeedback, (albedo && prevGI) ? (std::max)(1.0f - 1.0f / R.fGIIntensity, 0.0f) : 0.0f);

    // Put back in the reverse order as the function ends.
    SavedVertexInput savedVertexInput(pDevice);
    SavedTargets savedTargets(pDevice);
    SavedTextures savedTextures(pDevice);
    SavedShaderConstants savedConstants(pDevice);
    AppliedRenderStates renderStates(pDevice, kSSRRenderStates);
    AppliedSamplerStates samplerStates(pDevice);

    pDevice->SetDepthStencilSurface(nullptr);
    pDevice->SetStreamSource(0, nullptr, 0, 0);
    pDevice->SetVertexDeclaration(nullptr);
    pDevice->SetFVF(kScreenVertexFVF);
    D3DVIEWPORT9 vpDesc = {};
    vpDesc.Width = DWORD(width);
    vpDesc.Height = DWORD(height);
    vpDesc.MaxZ = 1.0f;
    pDevice->SetViewport(&vpDesc);

    DrawEffectPass(pDevice, effect, h.techSSGI, R.GIRawSurf, width, height, kProfGIMarch);

    // The same depth aware smoothing SSR uses; the gather has alpha 1 everywhere, so it is a
    // plain weighted blur. The radius is in full size pixels.
    IDirect3DTexture9* gathered = R.GIRawTex->mD3DTexture;
    if (R.fSSRDenoiseRadius > 0.0f && h.techSSRDenoise)
    {
        effect->SetTexture(h.SSRResultTex2D, gathered);
        effect->SetFloat(h.fDenoiseRadius, R.fSSRDenoiseRadius * 0.5f);
        effect->SetFloat(h.fDenoiseSSROnly, 0.0f);
        DrawEffectPass(pDevice, effect, h.techSSRDenoise, R.GIDenoisedSurf, width, height, kProfGIDenoise);
        gathered = R.GIDenoisedTex->mD3DTexture;
    }

    const int prev = R.nGIAccumIndex, next = prev ^ 1;
    effect->SetTexture(h.SSRResultTex2D, gathered);
    effect->SetTexture(h.SSRAccumTex2D, R.GIAccumTex[prev]->mD3DTexture);
    const bool history = FrameHistory::CanReproject(R.nGIAccumFrame);
    BindMotionVectors(effect, history);
    effect->SetFloat(h.fTemporalBlend, history ? R.fGITemporalBlend : 0.0f);
    effect->SetFloat(h.fTemporalAnySurface, 1.0f);
    // The SSR pass left the menu's debug view in fDebugMode; view 10 would turn this pass into its own.
    effect->SetFloat(h.fDebugMode, 0.0f);
    DrawEffectPass(pDevice, effect, h.techSSRTemporal, R.GIAccumSurf[next], width, height, kProfGITemporal);
    // View 10 runs the accumulation again into whichever half size target it does not read.
    IDirect3DTexture9* historyDebug = nullptr;
    if (R.SSRDebugMode() == R.kGIHistoryDebugMode && R.SSRDebugSurf && h.techSSRDebug)
    {
        const bool rawFree = gathered != R.GIRawTex->mD3DTexture;
        // While the trace runs, the numbers behind the colours (see GIHistoryDebug in SSR.fx), read back here.
        effect->SetFloat(h.fDebugMode, SSRTrace::Active() ? float(R.kGIHistoryDebugMode) + 0.75f : float(R.kGIHistoryDebugMode));
        DrawEffectPass(pDevice, effect, h.techSSRTemporal, rawFree ? R.GIRawSurf : R.GIDenoisedSurf, width, height);
        effect->SetFloat(h.fDebugMode, 0.0f);
        historyDebug = rawFree ? R.GIRawTex->mD3DTexture : R.GIDenoisedTex->mD3DTexture;
        SSRTrace::Line("gi history debug: %.0fx%.0f, z now / last frame's copy / expected through last frame's camera / flags", width, height);
        SSRTrace::PixelProbe(pDevice, "gi history", historyDebug);
    }
    effect->SetFloat(h.fTemporalAnySurface, 0.0f);
    R.nGIAccumIndex = next;
    R.nGIAccumFrame = FrameHistory::Frame();
    R.GIResult = R.GIAccumTex[next]->mD3DTexture;

    vpDesc.Width = DWORD(fullWidth);
    vpDesc.Height = DWORD(fullHeight);
    pDevice->SetViewport(&vpDesc);
    SetTargetSize(effect, h, proj, fullWidth, fullHeight);
    if (h.techGIUpsample && R.GIFullSurf)
    {
        effect->SetTexture(h.SSRResultTex2D, R.GIResult);
        DrawEffectPass(pDevice, effect, h.techGIUpsample, R.GIFullSurf, fullWidth, fullHeight, kProfGIUpsample);
        R.GIResult = R.GIFullTex->mD3DTexture;
    }

    if ((R.SSRDebugMode() == R.kGIDebugMode || historyDebug) && R.SSRDebugSurf && h.techSSRDebug)
    {
        effect->SetTexture(h.SSRResultTex2D, historyDebug ? historyDebug : R.GIResult);
        effect->SetFloat(h.fDebugMode, float(historyDebug ? R.kGIHistoryDebugMode : R.kGIDebugMode));
        DrawEffectPass(pDevice, effect, h.techSSRDebug, R.SSRDebugSurf, fullWidth, fullHeight);
        R.bSSRDebugValid = true;
    }

}
