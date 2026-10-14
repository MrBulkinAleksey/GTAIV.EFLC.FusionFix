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

export module ambientocclusion;

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

// Ambient occlusion (AO.fx) into the G-buffer before deferred lighting.

// AO's targets all exist. Loading a game from the menu once drew a frame after OnDeviceLost released them and before
// OnDeviceReset made them again, and crashed on AOCamDepthTex
static bool AOTargetsReady()
{
    auto& R = PostFxResources;
    bool ready = R.mSpecularRT && R.mSpecularRT->mD3DTexture && R.AOCamDepthTex &&
                 R.AOCamDepthTex->mD3DTexture && R.AOTex && R.AOTex->mD3DTexture && R.AOBlurTex && R.AOBlurTex->mD3DTexture &&
                 R.AOSurf && R.AOBlurSurf;
    for (auto i = 0; ready && i < R.nAmbientOcclusionMaxMipLevel; ++i)
        ready = R.AOCamDepthSurf[i] != nullptr;
    static bool wasReady = true;
    if (ready != wasReady)
    {
        wasReady = ready;
        FusionLog::Write("PostFx", "AO", ready ? "targets back, AO drawn again"
                                               : "a target is missing (cam depth %p, ao %p, blur %p, specular %p), thread %lu: AO skipped",
                         (void*)R.AOCamDepthTex, (void*)R.AOTex, (void*)R.AOBlurTex, (void*)R.mSpecularRT, GetCurrentThreadId());
    }
    return ready;
}

export void RenderAmbientOcclusion()
{
    static auto AO = FusionFixSettings.GetRef("PREF_SAO");
    if (PostFxResources.AOEffect && PostFxResources.AOEnabled && AO->get() && AOTargetsReady())
    { // AO
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        // Nothing saved by D3DX (see SavedShaders): textures, samplers, constants, the passes' render states and the
        // shaders are saved here
        SavedSamplerSlots savedSamplers(pDevice);
        SavedShaderConstants savedConstants(pDevice);
        SavedEffectPassStates savedStates(pDevice);
        SavedShaders savedShaders(pDevice);

        IDirect3DSurface9* rt0 = nullptr;
        IDirect3DSurface9* ds = nullptr;
        D3DVIEWPORT9 oldViewport;

        SavedVertexInput savedVertexInput(pDevice);
        pDevice->GetRenderTarget(0, &rt0);
        pDevice->GetDepthStencilSurface(&ds);
        pDevice->GetViewport(&oldViewport);

        pDevice->SetDepthStencilSurface(nullptr);
        pDevice->SetStreamSource(0, nullptr, 0, 0);
        pDevice->SetVertexDeclaration(nullptr);
        pDevice->SetFVF(kScreenVertexFVF); // fullscreen fvf

        PostFxResources.SpecularTex = PostFxResources.mSpecularRT->mD3DTexture;
        PostFxResources.DepthTex = LightingDepth();
        IDirect3DSurface9* SpecularRT;
        PostFxResources.SpecularTex->GetSurfaceLevel(0, &SpecularRT);

        UINT passes = 0;
        ID3DXEffect* effect = PostFxResources.AOEffect;
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE); assert(passes == 7);
        {
            rage::grcViewport* currGrcViewport = rage::GetCurrentViewport();

            IDirect3DTexture9* camDepthTex = PostFxResources.AOCamDepthTex->mD3DTexture;
            IDirect3DTexture9* aoTex = PostFxResources.AOTex->mD3DTexture;
            IDirect3DTexture9* aoBlurTex = PostFxResources.AOBlurTex->mD3DTexture;
            auto& camDepthSurf = PostFxResources.AOCamDepthSurf;
            IDirect3DSurface9* aoSurf = PostFxResources.AOSurf;
            IDirect3DSurface9* aoBlurSurf = PostFxResources.AOBlurSurf;

            float width = float(RenderScale::ToRenderWidth(uint32_t(currGrcViewport->mWidth)));
            float height = float(RenderScale::ToRenderHeight(uint32_t(currGrcViewport->mHeight)));

            D3DVIEWPORT9 vp = {};
            vp.MaxZ = 1.0;
            vp.Height = DWORD(height);
            vp.Width = DWORD(width);
            pDevice->SetViewport(&vp);

            auto& h = PostFxResources.AOEffectHandles;

            const ScreenQuad screenQuad = ScreenQuad::Full(width, height);

            float invViewportSize[] = { 1.0f / width, 1.0f / height };

            effect->SetTexture(h.DepthTex2D, PostFxResources.DepthTex);
            SetTargetSize(effect, h, *(const D3DMATRIX*)currGrcViewport->mProjectionMatrix, width, height);
            SetDepthRange(effect, h, currGrcViewport->mNearClip, currGrcViewport->mFarClip);
            effect->SetFloat(h.fFarPlane, currGrcViewport->mFarClip);

            ProfilerMark(pDevice, kProfAODepth, true);
            pDevice->SetRenderTarget(0, camDepthSurf[0]);
            effect->BeginPass(0);
            BindEffectSamplers(pDevice, effect);
            screenQuad.Draw(pDevice);
            effect->EndPass();

            effect->SetTexture(h.AOCamDepthTexture2D, camDepthTex);
            effect->BeginPass(1);
            for (auto i = 1; i < PostFxResources.nAmbientOcclusionMaxMipLevel; ++i)
            {
                float prevMipDimensions[] = { float((int)width >> (i - 1)), float((int)height >> (i - 1)) };
                float prevMipTexel[] = { float(1.0f / prevMipDimensions[0]), float(1.0f / prevMipDimensions[1]) };
                float curMipWidth = float(int(width) >> i);
                float curMipHeight = float(int(height) >> i);

                effect->SetFloatArray(h.vec2PrevMipSize, prevMipDimensions, 2);
                effect->SetFloatArray(h.vec2PrevMipTexel, prevMipTexel, 2);
                effect->SetInt(h.iPreviousMip, i - 1);

                const ScreenQuad mipQuad = ScreenQuad::Full(curMipWidth, curMipHeight);

                effect->CommitChanges();

                pDevice->SetRenderTarget(0, PostFxResources.AOCamDepthSurf[i]);
                BindEffectSamplers(pDevice, effect);
                mipQuad.Draw(pDevice);
            }
            effect->EndPass();
            ProfilerMark(pDevice, kProfAODepth, false);

            // From here to the output, at the size of the AO targets: half with AmbientOcclusionHalfResolution
            D3DSURFACE_DESC aoDesc{};
            aoSurf->GetDesc(&aoDesc);
            const bool half = aoDesc.Width < UINT(width);
            const float aoWidth = half ? float(aoDesc.Width) : width, aoHeight = half ? float(aoDesc.Height) : height;
            const ScreenQuad aoQuad = ScreenQuad::Full(aoWidth, aoHeight);
            const float aoTexel[] = { 1.0f / aoWidth, 1.0f / aoHeight };
            if (half)
            {
                D3DVIEWPORT9 aoVp = vp;
                aoVp.Width = DWORD(aoWidth);
                aoVp.Height = DWORD(aoHeight);
                pDevice->SetViewport(&aoVp);
                SetTargetSize(effect, h, *(const D3DMATRIX*)currGrcViewport->mProjectionMatrix, aoWidth, aoHeight);
            }

            pDevice->SetRenderTarget(0, aoSurf);
            pDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_COLORVALUE(1.0, 0.0, 0, 1.0), 1.0f, 0);

            D3DMATRIX proj = *(D3DMATRIX*)currGrcViewport->mProjectionMatrix;

            effect->SetFloat(h.fRadius, PostFxResources.fAmbientOcclusionRadius);
            // GTAO's normals, from the G-buffer when there is one
            {
                const bool normals = PostFxResources.mNormalRT && PostFxResources.mNormalRT->mD3DTexture;
                effect->SetTexture(h.NormalTex2D, normals ? PostFxResources.mNormalRT->mD3DTexture : nullptr);
                effect->SetFloat(h.fUseNormals, normals ? 1.0f : 0.0f);
                D3DXVECTOR4 toView[3];
                WorldToViewRows(currGrcViewport, toView);
                effect->SetVectorArray(h.vec4WorldToView, toView, 3);
                effect->SetFloat(h.fGTAOStrength, PostFxResources.fAmbientOcclusionGTAOStrength);
                effect->SetFloat(h.fThinOccluders, PostFxResources.fAmbientOcclusionGTAOThinOccluders);
            }
            auto& R = PostFxResources;
            const bool gtao = AO->get() == 2;
            const bool temporal = gtao && R.fAmbientOcclusionTemporal > 0.0f && R.AOAccumSurf[0] && R.AOAccumSurf[1];
            {
                // While GTAO accumulates, its noise is read elsewhere every frame (FrameHistory::NoiseOffset).
                auto offset = temporal ? FrameHistory::NoiseOffset() : std::array<float, 2>{};
                effect->SetFloatArray(h.vec2NoiseOffset, offset.data(), 2);
                const bool albedo = gtao && R.bAmbientOcclusionMultiBounce && R.mDiffuseRT && R.mDiffuseRT->mD3DTexture;
                effect->SetTexture(h.AlbedoTex2D, albedo ? R.mDiffuseRT->mD3DTexture : nullptr);
                effect->SetFloat(h.fMultiBounce, albedo ? 1.0f : 0.0f);
            }
            effect->SetFloat(h.fBias, PostFxResources.fAmbientOcclusionBias);
            effect->SetFloat(h.fIntensity, PostFxResources.fAmbientOcclusionIntensity);
            effect->SetFloat(h.fProjScale, proj._22 * 0.5f * aoHeight);

            effect->CommitChanges();

            ProfilerMark(pDevice, kProfAOMain, true);
            effect->BeginPass(gtao ? 5 : 2); // 1 SAO, 2 GTAO
            BindEffectSamplers(pDevice, effect);
            aoQuad.Draw(pDevice);
            effect->EndPass();
            ProfilerMark(pDevice, kProfAOMain, false);

            // GTAO's accumulation over frames, before the blur: the history is this frame's
            // raw GTAO blended with last frame's, followed to where each pixel was.
            IDirect3DTexture9* blurSource = aoTex;
            if (temporal)
            {
                const int prev = R.nAOAccumIndex, next = prev ^ 1;
                const bool history = FrameHistory::CanReproject(R.nAOAccumFrame);
                D3DXMATRIX prevViewProj;
                if (FrameHistory::Previous().Valid)
                    prevViewProj = FrameHistory::Previous().ViewProjection;
                else
                    MatrixMultiply(prevViewProj, *(const D3DXMATRIX*)currGrcViewport->mViewMatrix, *(const D3DXMATRIX*)currGrcViewport->mProjectionMatrix);
                D3DXVECTOR4 reprojRows[4];
                ViewToClipRows(currGrcViewport, prevViewProj, reprojRows);
                effect->SetVectorArray(h.vec4ViewToPrevClip, reprojRows, 4);
                const bool prevDepth = R.PreAlphaDepthCopyRT && R.PreAlphaDepthCopyRT->mD3DTexture;
                effect->SetTexture(h.PrevDepthTex2D, prevDepth ? R.PreAlphaDepthCopyRT->mD3DTexture : nullptr);
                effect->SetFloat(h.fUsePrevDepth, prevDepth ? 1.0f : 0.0f);
                SetPrevDepthRange(effect, h);
                auto motion = history ? FrameHistory::MotionVectors() : nullptr;
                auto jitter = FrameHistory::JitterDeltaUV();
                effect->SetTexture(h.MotionTex2D, motion);
                effect->SetFloat(h.fUseMotion, motion ? 1.0f : 0.0f);
                effect->SetFloatArray(h.vec2MotionJitter, jitter.data(), 2);
                effect->SetFloat(h.fTemporalBlend, history ? R.fAmbientOcclusionTemporal : 0.0f);
                effect->SetTexture(h.AOTexture2D, aoTex);
                effect->SetTexture(h.AOHistoryTex2D, R.AOAccumTex[prev]->mD3DTexture);
                effect->CommitChanges();

                pDevice->SetRenderTarget(0, R.AOAccumSurf[next]);
                ProfilerMark(pDevice, kProfAOTemporal, true);
                effect->BeginPass(6);
                BindEffectSamplers(pDevice, effect);
                aoQuad.Draw(pDevice);
                effect->EndPass();
                ProfilerMark(pDevice, kProfAOTemporal, false);

                R.nAOAccumIndex = next;
                R.nAOAccumFrame = FrameHistory::Frame();
                blurSource = R.AOAccumTex[next]->mD3DTexture;
            }
            else
                R.nAOAccumFrame = 0;

            float hor[2] = { aoTexel[0] * PostFxResources.fAmbientOcclusionBlurRadius, 0.0 };
            float ver[2] = { 0.0, aoTexel[1] * PostFxResources.fAmbientOcclusionBlurRadius };
            ProfilerMark(pDevice, kProfAOBlur, true);
            effect->BeginPass(3);
            for (auto i = 0; i < PostFxResources.nAmbientOcclusionBlurPasses; ++i)
            {
                pDevice->SetRenderTarget(0, aoBlurSurf);
                effect->SetTexture(h.AOTexture2D, i == 0 ? blurSource : aoTex); // blur pass 1
                effect->SetFloatArray(h.vec2BlurDirection, hor, 2);
                effect->CommitChanges();

                BindEffectSamplers(pDevice, effect);
                aoQuad.Draw(pDevice);

                pDevice->SetRenderTarget(0, aoSurf);
                effect->SetTexture(h.AOTexture2D, aoBlurTex); // blur pass 2
                effect->SetFloatArray(h.vec2BlurDirection, ver, 2);
                effect->CommitChanges();

                BindEffectSamplers(pDevice, effect);
                aoQuad.Draw(pDevice);
            }
            effect->EndPass();
            ProfilerMark(pDevice, kProfAOBlur, false);

            // final output, at the screen's size again
            pDevice->SetRenderTarget(0, SpecularRT);
            effect->SetTexture(h.AOTexture2D, PostFxResources.nAmbientOcclusionBlurPasses > 0 ? aoTex : blurSource);
            effect->SetFloatArray(h.vec2AOTexelSize, aoTexel, 2);
            effect->SetFloat(h.fUpsample, half ? 1.0f : 0.0f);
            effect->SetFloat(h.fUpsampleMip, half && PostFxResources.nAmbientOcclusionMaxMipLevel > 1 ? 1.0f : 0.0f);
            if (half)
            {
                pDevice->SetViewport(&vp);
                SetTargetSize(effect, h, *(const D3DMATRIX*)currGrcViewport->mProjectionMatrix, width, height);
            }
            effect->CommitChanges();

            ProfilerMark(pDevice, kProfAOApply, true);
            effect->BeginPass(4);
            BindEffectSamplers(pDevice, effect);
            screenQuad.Draw(pDevice);
            effect->EndPass();
            ProfilerMark(pDevice, kProfAOApply, false);
        }
        effect->End();

        pDevice->SetRenderTarget(0, rt0);
        pDevice->SetDepthStencilSurface(ds);
        pDevice->SetViewport(&oldViewport);

        SAFE_RELEASE(SpecularRT);
        SAFE_RELEASE(rt0);
        SAFE_RELEASE(ds);
    }
}
