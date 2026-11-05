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

export module ssr;

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
import backfacedepth;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxProfiler;
using namespace PostFxPass;

// Screen space reflections (SSR.fx) before deferred lighting, and the debug view over the frame.

export void RenderScreenSpaceReflections()
{
    auto& R = PostFxResources;
    R.bSSRHistoryThisFrame = false;
    R.bWaterDoneThisFrame = false;
    R.SSRResult = nullptr;
    R.bSSRValidThisFrame = false;
    R.bSSRReprojValid = false;
    R.bSSRDebugValid = false;
    R.bGlassFrameValid = false;
    R.bSSRDenoised = false;

    if (!R.SSRSurf)
    {
        SSRTrace::Line("ssr: left, no SSRSurf");
        return;
    }

    IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
    if (!pDevice)
        return;

    if (!R.SSREnabled())
    {
        SSRTrace::Line("ssr: left, off in the menu");
        R.nSSRAccumFrame = 0;
        return;
    }

    rage::grcViewport* vp = rage::GetCurrentViewport();
    if (vp)
        SSRTrace::Line("ssr: vp %p %dx%d near %.3f far %.1f", static_cast<void*>(vp), int(vp->mWidth), int(vp->mHeight), vp->mNearClip, vp->mFarClip);
    auto hasTexture = [](rage::grcRenderTargetPC* target) { return target && target->mD3DTexture; };
    if (!R.SSREffect || !hasTexture(R.mDepthRT) || !hasTexture(R.SSRHistoryTex) || !vp || R.fSSRIntensity <= 0.0f ||
        !hasTexture(R.SSRTex) || !R.SSRTraceSurf[0] || !hasTexture(R.SSRTraceTex[0]) ||
        !R.SSRFallbackSurf[0] || !hasTexture(R.SSRFallbackTex[0]) || !R.SSRMarchDepthSurf || !R.SSRMarchDepthTex ||
        !R.SSRMarchDepthTex->mD3DTexture)
    {
        SSRTrace::Line("ssr: left, effect %p depth %p history %p vp %p intensity %.2f trace %p fallback %p march depth %p",
            static_cast<void*>(R.SSREffect), static_cast<void*>(R.mDepthRT), static_cast<void*>(R.SSRHistoryTex),
            static_cast<void*>(vp), R.fSSRIntensity, static_cast<void*>(R.SSRTraceSurf[0]), static_cast<void*>(R.SSRFallbackSurf[0]),
            static_cast<void*>(R.SSRMarchDepthSurf));
        R.nSSRAccumFrame = 0;
        return;
    }

    // The trace samples the previous scene even when temporal accumulation is
    // disabled. A stale scene or a camera cut would reflect unrelated pixels.
    // The fog pass will capture a fresh scene later in this frame.
    if (!FrameHistory::CanReproject(R.nSSRHistoryFrame))
    {
        SSRTrace::Line("ssr: left, scene history cannot be reprojected");
        R.nSSRAccumFrame = 0;
        R.bSSRReprojValid = false;
        return;
    }

    // Put back in the reverse order as the function ends.
    SavedVertexInput savedVertexInput(pDevice);
    SavedTargets savedTargets(pDevice);

    pDevice->SetDepthStencilSurface(nullptr);
    pDevice->SetStreamSource(0, nullptr, 0, 0);
    pDevice->SetVertexDeclaration(nullptr);
    pDevice->SetFVF(kScreenVertexFVF);

    float width = float(RenderScale::ToRenderWidth(uint32_t(vp->mWidth)));
    float height = float(RenderScale::ToRenderHeight(uint32_t(vp->mHeight)));

    // Half: the march and the smoothing run on the half size targets, created as the full
    // size halved; the debug view stays full size.
    const bool half = R.SSRHalfRes() && R.SSRHalfSurf && hasTexture(R.SSRHalfTex) &&
        R.SSRTraceSurf[1] && hasTexture(R.SSRTraceTex[1]) && R.SSRFallbackSurf[1] && hasTexture(R.SSRFallbackTex[1]);
    IDirect3DSurface9* ssrSurf = half ? R.SSRHalfSurf : R.SSRSurf;
    IDirect3DTexture9* ssrTex = half ? R.SSRHalfTex->mD3DTexture : R.SSRTex->mD3DTexture;
    IDirect3DSurface9* denoisedSurf = half ? R.SSRHalfDenoisedSurf : R.SSRDenoisedSurf;
    IDirect3DTexture9* denoisedTex = half ? (hasTexture(R.SSRHalfDenoisedTex) ? R.SSRHalfDenoisedTex->mD3DTexture : nullptr)
                                          : (hasTexture(R.SSRDenoisedTex) ? R.SSRDenoisedTex->mD3DTexture : nullptr);

    auto& h = R.SSREffectHandles;
    ID3DXEffect* effect = R.SSREffect;
    const float invHistorySize[] = { 1.0f / width, 1.0f / height };
    effect->SetFloatArray(h.vec2InvHistorySize, invHistorySize, 2);
    D3DMATRIX proj = *(D3DMATRIX*)vp->mProjectionMatrix;

    ScreenQuad screenQuad = {};
    // Viewport, quad and the effect's pixel size and reconstruction basis for a w x h target.
    auto setPassSize = [&](float w, float hgt)
    {
        D3DVIEWPORT9 vpDesc = {};
        vpDesc.MaxZ = 1.0f;
        vpDesc.Width = DWORD(w);
        vpDesc.Height = DWORD(hgt);
        pDevice->SetViewport(&vpDesc);

        screenQuad = ScreenQuad::Full(w, hgt);
        SetTargetSize(effect, h, proj, w, hgt);
    };
    setPassSize(half ? float(DWORD(width) / 2) : width, half ? float(DWORD(height) / 2) : height);

    effect->SetTexture(h.DepthTex2D, LightingDepth());
    effect->SetTexture(h.HistoryTex2D, R.SSRHistoryTex->mD3DTexture);
    // Last frame's fog pass copied this depth along with the history; this frame's has not
    // run yet.
    const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;
    effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
    effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
    SetPrevDepthRange(effect, h);
    // SSR traces this frame's depth into the previous scene colour. Reproject the hit with its
    // own motion vector when available, rather than only following the reflecting surface.
    BindMotionVectors(effect, true);

    // _DEFERRED_GBUFFER_2_ is (specular intensity, gloss, AO). Gloss decides what reflects:
    // car paint sits around 0.8, roads and walls around 0.2-0.35. Car paint stores almost no
    // specular intensity here, so that channel is not used.
    bool hasSpecular = R.mSpecularRT && R.mSpecularRT->mD3DTexture;
    if (hasSpecular)
        effect->SetTexture(h.SpecularTex2D, R.mSpecularRT->mD3DTexture);
    effect->SetFloat(h.fGlossBoost, hasSpecular ? R.fSSRGlossBoost : 0.0f);
    effect->SetFloat(h.fGlossCutoff, hasSpecular ? R.fSSRGlossCutoff : -1.0f);

    SetDepthRange(effect, h, vp->mNearClip, vp->mFarClip);

    // Last frame's camera, which the scene history and the accumulation were rendered with
    D3DXMATRIX prevViewProj;
    if (FrameHistory::Previous().Valid)
        prevViewProj = FrameHistory::Previous().ViewProjection;
    else
        MatrixMultiply(prevViewProj, *(const D3DXMATRIX*)vp->mViewMatrix, *(const D3DXMATRIX*)vp->mProjectionMatrix);

    D3DXVECTOR4 reprojRows[4];
    ViewToClipRows(vp, prevViewProj, reprojRows);
    effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
    memcpy(R.SSRReprojRows, reprojRows, sizeof(reprojRows));
    R.bSSRReprojValid = true;

    effect->SetFloat(h.fMaxDistance, R.fSSRMaxDistance);
    effect->SetFloat(h.fThickness, R.fSSRThickness);
    effect->SetFloat(h.fHitFadeStart, R.fSSRHitFadeStart);
    effect->SetFloat(h.fThicknessByAngle, R.fSSRThicknessByAngle);
    auto backFaces = BackFaceDepth::Texture();
    effect->SetTexture(h.BackFaceDepthTex2D, backFaces);
    effect->SetFloat(h.fBackFacesOff, backFaces ? 0.0f : 1.0f);
    effect->SetFloat(h.fEdgeFade, R.fSSREdgeFade);
    effect->SetFloat(h.fIntensity, R.fSSRIntensity);
    effect->SetFloat(h.fPassThinObjects, R.bSSRPassThinObjects ? 1.0f : 0.0f);
    effect->SetFloat(h.fStepJitter, R.bSSRStepJitter ? 1.0f : 0.0f);
    effect->SetFloat(h.fStepsPerPixel, 1.0f / R.fSSRStepPixels);
    effect->SetFloat(h.fMarchFullDepth, R.SSRMarchFullDepth() ? 1.0f : 0.0f);
    const bool temporal = R.fSSRTemporalBlend > 0.0f && h.techSSRTemporal &&
        R.SSRAccumSurf[half][0] && R.SSRAccumSurf[half][1] &&
        R.SSRAccumTex[half][0] && R.SSRAccumTex[half][0]->mD3DTexture &&
        R.SSRAccumTex[half][1] && R.SSRAccumTex[half][1]->mD3DTexture &&
        R.SSRHitDistSurf[half] && R.SSRHitDistTex[half] && R.SSRHitDistTex[half]->mD3DTexture;
    const bool stochastic = R.SSRStochastic() && temporal && hasSpecular;
    static bool previousStochastic = false;
    if (stochastic != previousStochastic)
        R.nSSRAccumFrame = 0;
    previousStochastic = stochastic;
    effect->SetFloat(h.fStochasticSSR, stochastic ? 1.0f : 0.0f);
    SetNoiseOffset(effect, temporal && (R.bSSRTemporalJitter || stochastic));
    effect->SetFloat(h.fTowardCamera, R.fSSRTowardCamera);
    effect->SetFloat(h.fReflectionBlur, R.fSSRReflectionBlur);
    effect->SetFloat(h.fDistanceFade, R.fSSRDistanceFade);
    effect->SetFloat(h.fFallback, R.fSSRFallback);
    effect->SetFloat(h.fNeighbourFill, R.fSSRNeighbourFill);
    effect->SetFloat(h.fFillPixelScale, half ? 0.5f : 1.0f);
    const bool denoise = R.fSSRDenoiseRadius > 0.0f && denoisedSurf && denoisedTex && h.techSSRDenoise;
    const bool distanceAvailable = R.SSRHitDistSurf[half] && R.SSRHitDistTex[half] && R.SSRHitDistTex[half]->mD3DTexture;
    // The distance target serves temporal, the fill's gate and the spatial filter.
    const bool hitDistances = distanceAvailable && R.fSSRNeighbourFill > 0.0f && R.fSSRNeighbourFillMinDistance > 0.0f;
    const bool writeHitDistances = distanceAvailable && (temporal || hitDistances || denoise);
    effect->SetFloat(h.fFillMinDistance, hitDistances ? R.fSSRNeighbourFillMinDistance / (std::max)(R.fSSRMaxDistance, 0.01f) : 0.0f);
    effect->SetFloat(h.fDenoiseHitDistance, writeHitDistances && denoise ? 1.0f : 0.0f);
    // Car bodies: SSR's colour in the units of the game's parabolic map, see fSSRVehicleUnits (last frame's c39.w).
    const bool vehicleUnits = R.fGameReflectionScale > 0.0f && R.mMaterialIdRT && R.mMaterialIdRT->mD3DTexture;
    if (vehicleUnits)
        effect->SetTexture(h.SkinIDTex2D, R.mMaterialIdRT->mD3DTexture);
    effect->SetFloat(h.fVehicleScale, vehicleUnits ? (std::min)(R.fSSRVehicleUnits / (10.0f * R.fGameReflectionScale), 1.0f) : 1.0f);
    // Debug view 3 has SSRTrace_PS write where its rays hit in place of the hit, and SSR_PS pass it on.
    effect->SetFloat(h.fDebugMode, float(R.SSRDebugMode()));

    // World to reconstruction space rotation, for the G-buffer normals and the debug view.
    const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
    {
        D3DXVECTOR4 toView[3];
        WorldToViewRows(vp, toView);
        effect->SetVectorArray(h.vec4WaterToView, toView, 3);
    }
    const bool hasNormals = R.mNormalRT && R.mNormalRT->mD3DTexture;
    if (hasNormals)
        effect->SetTexture(h.NormalTex2D, R.mNormalRT->mD3DTexture);
    effect->SetFloat(h.fUseGBufferNormals, (hasNormals && R.bSSRGBufferNormals) ? 1.0f : 0.0f);
    // How wet the ground is, as the wet ground pass has it (RenderWetGround, which runs at the end
    // of the G-buffer pass every frame and keeps it up to date even while it draws nothing): it
    // follows CWeather::Rain over WetGroundWetting and WetGroundDrying seconds. Taken from the rain
    // itself, a shower turned on had SSR mirror the whole road at once, half a minute before the
    // ground pass made it wet.
    // Telling ground from walls takes the G-buffer normals, and the gloss the specular one.
    const bool wetGround = R.fSSRWetGround > 0.0f && hasNormals && hasSpecular;
    effect->SetFloat(h.fWetness, wetGround ? R.fWetness : 0.0f);
    effect->SetFloat(h.fWetGroundBoost, R.fSSRWetGround);

    UINT passes = 0;
    SavedTextures savedTextures(pDevice);
    SavedShaderConstants savedConstants(pDevice);
    AppliedRenderStates renderStates(pDevice, kSSRRenderStates);
    AppliedSamplerStates samplerStates(pDevice);
    {
        // A previous frame may have left a hierarchy texture bound to any shader sampler.
        // Unbind all saved slots before using those textures as render targets.
        for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
            SetTextureBoth(pDevice, slot, nullptr);

        // D3DX saving the state it changes, through state blocks, left the smoothing and the
        // accumulation in play drawing with what the game had bound: the textures they set
        // did not take. This function saves and puts back what the passes touch itself.
        if (FAILED(effect->SetTechnique(h.techSSR)) || FAILED(effect->Begin(&passes, D3DXFX_DONOTSAVESTATE)))
        {
            R.nSSRAccumFrame = 0;
            return;
        }
    }
    {
        // The march, the guess for its misses (skipped while SSRFallback is
        // 0, SSR_PS then does not read it), and the two together. Each draws every pixel of
        // the viewport, no blending, nothing discarded: nothing is cleared first.
        auto draw = [&](UINT pass, IDirect3DSurface9* target)
        {
            const HRESULT rtHr = pDevice->SetRenderTarget(0, target);
            if (FAILED(rtHr))
                return false;
            const HRESULT passHr = effect->BeginPass(pass);
            if (FAILED(passHr))
                return false;
            if (FAILED(effect->CommitChanges()))
            {
                effect->EndPass();
                return false;
            }
            BindEffectSamplers(pDevice, effect);
            SSRTrace::State(pDevice, "ssr pass");
            const HRESULT drawHr = screenQuad.Draw(pDevice);
            const HRESULT endHr = effect->EndPass();
            if (SSRTrace::Active())
            {
                D3DVIEWPORT9 view = {};
                pDevice->GetViewport(&view);
                SSRTrace::Line("ssr: pass %u of %u into %p (%ux%u view) rt %08x pass %08x draw %08x",
                    pass, passes, static_cast<void*>(target), unsigned(view.Width), unsigned(view.Height),
                    unsigned(rtHr), unsigned(passHr), unsigned(drawHr));
            }
            return SUCCEEDED(drawHr) && SUCCEEDED(endHr);
        };
        // Time depth preparation separately so a Hi-Z A/B includes its construction cost.
        ProfilerMark(pDevice, kProfSSRDepth, true);
        setPassSize(float(DWORD(width) / 2), float(DWORD(height) / 2));
        if (!draw(4, R.SSRMarchDepthSurf))
        {
            ProfilerMark(pDevice, kProfSSRDepth, false);
            effect->End();
            R.nSSRAccumFrame = 0;
            return;
        }
        ProfilerMark(pDevice, kProfSSRDepth, false);
        int hiZLevels = 0;
        IDirect3DTexture9* depthLevel = R.SSRMarchDepthTex->mD3DTexture;
        D3DSURFACE_DESC marchDesc = {};
        const bool marchSizeMatches = SUCCEEDED(R.SSRMarchDepthSurf->GetDesc(&marchDesc)) &&
            marchDesc.Width == DWORD(width) / 2 && marchDesc.Height == DWORD(height) / 2;
        // What the menu asks for, capped at what was allocated: a viewport not divisible by eight
        // only has the finer level. Live, so one level and two can be compared in one session.
        const int hiZWanted = (std::min)(R.SSRHiZLevels(), PostFxResource::kSSRHiZLevels);
        if (hiZWanted > 0 && !R.SSRMarchFullDepth() && marchSizeMatches)
        {
            ProfilerScope timedHiZ(pDevice, kProfSSRHiZ);
            for (int i = 0; i < hiZWanted; ++i)
            {
                // A level the effect has no parameter for would be drawn and then read through a
                // sampler nothing is bound to, so the traversal must not be told it is there.
                if (!h.HiZDepthTex2D[i] || !R.SSRHiZDepthTex[i] || !R.SSRHiZDepthTex[i]->mD3DTexture || !R.SSRHiZDepthSurf[i])
                    break;
                const DWORD divisor = 4u << i;
                D3DSURFACE_DESC levelDesc = {};
                if (FAILED(R.SSRHiZDepthSurf[i]->GetDesc(&levelDesc)) ||
                    levelDesc.Width != DWORD(width) / divisor || levelDesc.Height != DWORD(height) / divisor)
                    break;
                effect->SetTexture(h.MarchDepthTex2D, depthLevel);
                setPassSize(float(levelDesc.Width), float(levelDesc.Height));
                if (!draw(5, R.SSRHiZDepthSurf[i]))
                    break;
                depthLevel = R.SSRHiZDepthTex[i]->mD3DTexture;
                effect->SetTexture(h.HiZDepthTex2D[i], depthLevel);
                ++hiZLevels;
            }
        }
        effect->SetTexture(h.MarchDepthTex2D, R.SSRMarchDepthTex->mD3DTexture);
        // Level i covers the viewport divided by 4 << i, the grid the traversal walks.
        for (int i = 0; i < PostFxResource::kSSRHiZLevels; ++i)
        {
            const DWORD divisor = 4u << i;
            const D3DXVECTOR2 hiZSize(float(DWORD(width) / divisor), float(DWORD(height) / divisor));
            effect->SetFloatArray(h.vec2HiZSize[i], &hiZSize.x, 2);
        }
        effect->SetFloat(h.fUseHiZ, float(hiZLevels));
        SSRTrace::Line("ssr: Hi-Z levels %d, stochastic %d, march size matches %d",
                       hiZLevels, int(stochastic), int(marchSizeMatches));
        setPassSize(half ? float(DWORD(width) / 2) : width, half ? float(DWORD(height) / 2) : height);

        // The march also writes how far its rays went, for the accumulation and the fill's distance gate.
        ProfilerMark(pDevice, kProfSSRTrace, true);
        IDirect3DSurface9* oldTarget1 = nullptr;
        if (writeHitDistances)
        {
            pDevice->GetRenderTarget(1, &oldTarget1);
            if (FAILED(pDevice->SetRenderTarget(1, R.SSRHitDistSurf[half])))
            {
                SAFE_RELEASE(oldTarget1);
                ProfilerMark(pDevice, kProfSSRTrace, false);
                effect->End();
                R.nSSRAccumFrame = 0;
                return;
            }
        }
        const bool traceOk = draw(0, R.SSRTraceSurf[half]);
        ProfilerMark(pDevice, kProfSSRTrace, false);
        if (writeHitDistances)
        {
            pDevice->SetRenderTarget(1, oldTarget1);
            SAFE_RELEASE(oldTarget1);
        }
        if (!traceOk)
        {
            effect->End();
            R.nSSRAccumFrame = 0;
            return;
        }
        if (writeHitDistances)
            effect->SetTexture(h.SSRHitDistTex2D, R.SSRHitDistTex[half]->mD3DTexture);
        effect->SetTexture(h.SSRResultTex2D, R.SSRTraceTex[half]->mD3DTexture);
        IDirect3DTexture9* fill = R.SSRFallbackTex[half]->mD3DTexture;
        ProfilerMark(pDevice, kProfSSRFill, true);
        if (R.fSSRFallback > 0.0f || R.fSSRNeighbourFill > 0.0f)
        {
            const bool filled = draw(1, R.SSRFallbackSurf[half]);
            if (!filled)
            {
                effect->SetFloat(h.fFallback, 0.0f);
                effect->SetFloat(h.fNeighbourFill, 0.0f);
            }
            // The fill's cascade, each step twice as far out as the last (the first reached nine
            // pixels), taking turns between the two targets; in pixels of this size.
            if (filled && R.SSRSpreadSurf[half] && hasTexture(R.SSRSpreadTex[half]) && R.fSSRNeighbourFill > 0.0f)
            {
                IDirect3DSurface9* surfs[2] = { R.SSRSpreadSurf[half], R.SSRFallbackSurf[half] };
                IDirect3DTexture9* texs[2] = { R.SSRSpreadTex[half]->mD3DTexture, R.SSRFallbackTex[half]->mD3DTexture };
                for (int step = 0; step < 2; ++step)
                {
                    effect->SetTexture(h.SSRFallbackTex2D, fill);
                    effect->SetFloat(h.fSpreadRadius, step ? 36.0f : 18.0f);
                    if (!draw(2, surfs[step]))
                        break;
                    fill = texs[step];
                }
            }
        }
        ProfilerMark(pDevice, kProfSSRFill, false);
        effect->SetTexture(h.SSRFallbackTex2D, fill);
        ProfilerMark(pDevice, kProfSSRResolve, true);
        const bool resolved = draw(3, ssrSurf);
        ProfilerMark(pDevice, kProfSSRResolve, false);
        if (!resolved)
        {
            effect->End();
            R.nSSRAccumFrame = 0;
            return;
        }
    }
    effect->End();

    // Debug view, while the G-buffer still holds this frame. Changes nothing the game sees.
    // Smooth the result into the texture deferred_lighting reads.
    R.bSSRDenoised = false;
    if (denoise)
    {
        effect->SetTexture(h.SSRResultTex2D, ssrTex);
        // The radius is in full size pixels, so the blur covers the same part of the screen.
        effect->SetFloat(h.fDenoiseRadius, half ? R.fSSRDenoiseRadius * 0.5f : R.fSSRDenoiseRadius);
        effect->SetFloat(h.fDenoiseSSROnly, 1.0f);
        SSRTrace::Contents(pDevice, "denoise input", ssrTex);
        R.bSSRDenoised = DrawEffectPass(pDevice, effect, h.techSSRDenoise, denoisedSurf,
            half ? float(DWORD(width) / 2) : width, half ? float(DWORD(height) / 2) : height, kProfSSRDenoise);
        if (R.bSSRDenoised)
            SSRTrace::Contents(pDevice, "denoise output", denoisedTex);
    }
    IDirect3DTexture9* ssrResult = R.bSSRDenoised ? denoisedTex : ssrTex;

    if (temporal)
    {
        const int sizeIndex = half ? 1 : 0;
        if (R.bSSRAccumHalf != half)
            R.nSSRAccumFrame = 0;
        R.bSSRAccumHalf = half;
        const int prev = R.nSSRAccumIndex, next = prev ^ 1;
        effect->SetTexture(h.SSRResultTex2D, ssrResult);
        effect->SetTexture(h.SSRAccumTex2D, R.SSRAccumTex[sizeIndex][prev]->mD3DTexture);
        effect->SetTexture(h.SSRHitDistTex2D, R.SSRHitDistTex[sizeIndex]->mD3DTexture);
        const bool history = FrameHistory::CanReproject(R.nSSRAccumFrame);
        BindMotionVectors(effect, history);
        effect->SetFloat(h.fTemporalBlend, history ? R.fSSRTemporalBlend : 0.0f);
        effect->SetFloat(h.fTemporalAnySurface, 0.0f);
        if (SSRTrace::Active())
        {
            SSRTrace::Line("ssr temporal: input %s, history %s of scene %u, reprojects %d, motion %p, into %s",
                SSRTrace::TextureName(ssrResult).c_str(), SSRTrace::TextureName(R.SSRAccumTex[sizeIndex][prev]->mD3DTexture).c_str(),
                unsigned(R.nSSRAccumFrame), int(history), static_cast<void*>(history ? FrameHistory::MotionVectors() : nullptr),
                SSRTrace::TextureName(R.SSRAccumTex[sizeIndex][next]->mD3DTexture).c_str());
            SSRTrace::Contents(pDevice, "temporal input", ssrResult);
            SSRTrace::Contents(pDevice, "temporal history", R.SSRAccumTex[sizeIndex][prev]->mD3DTexture);
        }
        if (DrawEffectPass(pDevice, effect, h.techSSRTemporal, R.SSRAccumSurf[sizeIndex][next],
            half ? float(DWORD(width) / 2) : width, half ? float(DWORD(height) / 2) : height, kProfSSRTemporal))
        {
            ssrResult = R.SSRAccumTex[sizeIndex][next]->mD3DTexture;
            R.nSSRAccumIndex = next;
            R.nSSRAccumFrame = FrameHistory::Frame();
        }
        else
            R.nSSRAccumFrame = 0;
    }
    else
        R.nSSRAccumFrame = 0;
    // At half res the result reaches the full size along the depth (SSRUpsample_PS) instead of by
    // the bilinear stretch deferred_lighting gives it, so a reflection's edge follows the
    // geometry's edge. The full size smoothed target is free here, the smoothing having run at
    // the half size one.
    if (half && h.techSSRUpsample && R.SSRDenoisedSurf && hasTexture(R.SSRDenoisedTex))
    {
        effect->SetTexture(h.SSRResultTex2D, ssrResult);
        setPassSize(width, height);
        if (DrawEffectPass(pDevice, effect, h.techSSRUpsample, R.SSRDenoisedSurf, width, height, kProfSSRUpsample))
            ssrResult = R.SSRDenoisedTex->mD3DTexture;
    }
    R.SSRResult = ssrResult;
    if (SSRTrace::Active())
    {
        SSRTrace::Line("ssr: done, half %d denoised %d temporal %d result %s, history this frame %d",
            int(half), int(R.bSSRDenoised), int(temporal), SSRTrace::TextureName(ssrResult).c_str(), int(R.bSSRHistoryThisFrame));
        SSRTrace::Contents(pDevice, "trace", R.SSRTraceTex[half] ? R.SSRTraceTex[half]->mD3DTexture : nullptr);
        // What the march counted for the depth hierarchy, as numbers to compare two settings by.
        if (writeHitDistances)
            SSRTrace::HiZSummary(pDevice, "hit distances", R.SSRHitDistTex[half] ? R.SSRHitDistTex[half]->mD3DTexture : nullptr);
        SSRTrace::Contents(pDevice, "resolve", ssrTex);
        SSRTrace::Contents(pDevice, "result", ssrResult);
        SSRTrace::readbacksLeft.fetch_sub(1);
    }

    const int debugMode = R.SSRDebugMode();
    if (debugMode && (debugMode < R.kGlassDebugMode || debugMode >= 11) && R.SSRDebugSurf && h.techSSRDebug && hasNormals)
    {
        if (half)
            setPassSize(width, height);
        // View 3 reads where the rays hit straight from the march: smoothing and accumulation
        // would average the positions of neighbouring hits into places no ray went.
        effect->SetTexture(h.SSRResultTex2D, (debugMode == 3 || debugMode == 11 || debugMode == 13) ? ssrTex : ssrResult);
        // View 12 reads how many steps the depth hierarchy skipped from the march's second
        // target, which the passes above left bound only when something asked for it.
        if (distanceAvailable)
            effect->SetTexture(h.SSRHitDistTex2D, R.SSRHitDistTex[half]->mD3DTexture);
        effect->SetFloat(h.fDebugMode, float(debugMode));

        R.bSSRDebugValid = DrawEffectPass(pDevice, effect, h.techSSRDebug, R.SSRDebugSurf,
                                         width, height, kProfSSRDebug);
    }

    // Camera for the car glass shaders, which draw after lighting.
    if (R.bGlassReflections)
    {
        const float params[20] =
        {
            proj._11, proj._22, proj._31, proj._32,
            proj._34, R.fGlassReflectionsThickness, R.fGlassReflectionsLength, 12345.0f,
            viewInv.m[0][0], viewInv.m[0][1], viewInv.m[0][2], debugMode == R.kGlassDebugMode ? 1.0f : 0.0f,
            viewInv.m[1][0], viewInv.m[1][1], viewInv.m[1][2], height * 0.5f,
            vp->mNearClip, log2f(vp->mFarClip / vp->mNearClip), R.bGlassStepJitter ? 1.0f : 0.0f, R.fSSRTowardCamera,
        };
        memcpy(R.GlassParams, params, sizeof(params));
        R.bGlassFrameValid = true;
    }
    R.bSSRValidThisFrame = true;
}

// Replaces the finished frame with the SSR debug view chosen in the graphics menu.
export void DrawSSRDebugOverlay()
{
    auto& R = PostFxResources;
    SSRTrace::Line("debug overlay: valid %d", int(R.bSSRDebugValid));
    if (!R.bSSRDebugValid || !R.SSRDebugTex || !R.SSREffect || !R.SSREffectHandles.techSSRDebugCopy)
        return;
    R.bSSRDebugValid = false;

    IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
    if (!pDevice)
        return;

    IDirect3DSurface9* rt0 = nullptr;
    if (FAILED(pDevice->GetRenderTarget(0, &rt0)) || !rt0)
        return;
    D3DSURFACE_DESC desc = {};
    rt0->GetDesc(&desc);
    const float width = float(desc.Width);
    const float height = float(desc.Height);

    // Put back in the reverse order as the function ends; the overlay draws into the target it found.
    SavedVertexInput savedVertexInput(pDevice);
    SavedTargets savedTargets(pDevice, false);
    SavedTextures savedTextures(pDevice);
    SavedShaderConstants savedConstants(pDevice);
    AppliedRenderStates renderStates(pDevice, kSSRRenderStates);
    AppliedSamplerStates samplerStates(pDevice);

    pDevice->SetDepthStencilSurface(nullptr);
    pDevice->SetStreamSource(0, nullptr, 0, 0);
    pDevice->SetVertexDeclaration(nullptr);
    pDevice->SetFVF(kScreenVertexFVF);
    D3DVIEWPORT9 vpDesc = {};
    vpDesc.Width = desc.Width;
    vpDesc.Height = desc.Height;
    vpDesc.MaxZ = 1.0f;
    pDevice->SetViewport(&vpDesc);

    const ScreenQuad screenQuad = ScreenQuad::Full(width, height);

    auto& h = R.SSREffectHandles;
    ID3DXEffect* effect = R.SSREffect;
    effect->SetTexture(h.DebugTex2D, R.SSRDebugTex->mD3DTexture);
    UINT passes = 0;
    effect->SetTechnique(h.techSSRDebugCopy);
    effect->Begin(&passes, 0);
    effect->BeginPass(0);
    effect->CommitChanges();
    BindEffectSamplers(pDevice, effect);
    screenQuad.Draw(pDevice);
    effect->EndPass();
    effect->End();

    SAFE_RELEASE(rt0);
}
