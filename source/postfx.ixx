module;

#include <common.hxx>
#include "nightshadows/ShadowLookupLayout.hpp"
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

export module postfx;

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
import firstperson;
import vehicleboxshadows;
import ssr;
import ambientocclusion;
import contactshadows;
import indirectlight;
import skinscattering;
import volumetricclouds;
import pbrsky;
import wetground;
import headlightglints;
import waterreflections;
import sharpening;
import inireload;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

// Binding through both the game's device wrapper and the real device: see RageDirect3DDevice9::SetTextureBoth. SSGI's
// accumulation read the G-buffer normals as its depth while the right depth was "bound" through the wrapper alone.
using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxProfiler;
using namespace PostFxPass;

class PostFX
{
private:
    struct VertexFormat
    {
        float Pos[3];
        float TexCoord[2];
    };

    static inline IDirect3DVertexBuffer9* mQuadVertexBuffer;
    static inline IDirect3DVertexDeclaration9* mQuadVertexDecl;

    static inline UINT lastoffset = 0;
    static inline UINT laststride = 0;

    static inline IDirect3DVertexBuffer9* last_VertexBuffer = 0;
    static inline IDirect3DVertexDeclaration9* last_VertexDecl = 0;

    static inline IDirect3DBaseTexture9* prePostFx[PostfxTextureCount] = { 0 };
    static inline DWORD Samplers[PostfxTextureCount] = { D3DTEXF_LINEAR };
    static inline HMODULE hm = NULL;

    static void saveRenderState()
    {
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        for (int i = 0; i < PostfxTextureCount; i++)
        {
            pDevice->GetTexture(i, &prePostFx[i]);
            pDevice->GetSamplerState(i, D3DSAMP_MAGFILTER, &Samplers[i]);
        }
    }

    static void restoreRenderState()
    {
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        for (int i = 0; i < PostfxTextureCount; i++)
        {
            SetTextureBoth(pDevice, i, prePostFx[i]);
            pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, Samplers[i]);
            SAFE_RELEASE(prePostFx[i]);
        }
    }

    // SSR, contact shadow and debug targets are D3DPOOL_DEFAULT, and the SSR effect keeps a
    // reference to every texture set on it. Reset fails while any of that is still held, so
    // this runs on device loss, before Reset, and again ahead of recreating them.
    static void ReleaseScreenSpaceTargets()
    {
        if (auto effect = PostFxResources.SSREffect)
        {
            D3DXEFFECT_DESC effectDesc = {};
            if (SUCCEEDED(effect->GetDesc(&effectDesc)))
            {
                for (UINT i = 0; i < effectDesc.Parameters; ++i)
                {
                    D3DXHANDLE param = effect->GetParameter(nullptr, i);
                    D3DXPARAMETER_DESC paramDesc = {};
                    if (param && SUCCEEDED(effect->GetParameterDesc(param, &paramDesc)) &&
                        paramDesc.Type >= D3DXPT_TEXTURE && paramDesc.Type <= D3DXPT_TEXTURECUBE)
                        effect->SetTexture(param, nullptr);
                }
            }
        }
        if (auto pDevice = rage::grcDevice::GetD3DDevice())
        {
            SetTextureBoth(pDevice, 3, nullptr);
            SetTextureBoth(pDevice, 8, nullptr);
            SetTextureBoth(pDevice, 9, nullptr);
            SetTextureBoth(pDevice, 11, nullptr);
        }
        PostFxResources.bGIBound = false;
        PostFxResources.bMaterialIdBound = false;
        SAFE_RELEASE(PostFxResources.SSRSurf);
        if (PostFxResources.SSRTex)
        {
            PostFxResources.SSRTex->Destroy();
            PostFxResources.SSRTex = nullptr;
        }
        SAFE_RELEASE(PostFxResources.SSRHistorySurf);
        if (PostFxResources.SSRHistoryTex)
        {
            PostFxResources.SSRHistoryTex->Destroy();
            PostFxResources.SSRHistoryTex = nullptr;
        }
        SAFE_RELEASE(PostFxResources.SSRDenoisedSurf);
        if (PostFxResources.SSRDenoisedTex)
        {
            PostFxResources.SSRDenoisedTex->Destroy();
            PostFxResources.SSRDenoisedTex = nullptr;
        }
        PostFxResources.bSSRDenoised = false;
        SAFE_RELEASE(PostFxResources.SSRHalfSurf);
        SAFE_RELEASE(PostFxResources.SSRHalfDenoisedSurf);
        SAFE_RELEASE(PostFxResources.SSRMarchDepthSurf);
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(PostFxResources.SSRTraceSurf[i]);
            SAFE_RELEASE(PostFxResources.SSRFallbackSurf[i]);
            SAFE_RELEASE(PostFxResources.SSRSpreadSurf[i]);
            SAFE_RELEASE(PostFxResources.SSRHitDistSurf[i]);
        }
        for (auto* rt : { &PostFxResources.SSRHalfTex, &PostFxResources.SSRHalfDenoisedTex, &PostFxResources.SSRTraceTex[0],
                          &PostFxResources.SSRTraceTex[1], &PostFxResources.SSRFallbackTex[0], &PostFxResources.SSRFallbackTex[1],
                          &PostFxResources.SSRSpreadTex[0], &PostFxResources.SSRSpreadTex[1], &PostFxResources.SSRHitDistTex[0],
                          &PostFxResources.SSRHitDistTex[1], &PostFxResources.SSRMarchDepthTex })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        PostFxResources.SSRResult = nullptr;
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(PostFxResources.AOAccumSurf[i]);
            if (PostFxResources.AOAccumTex[i])
            {
                PostFxResources.AOAccumTex[i]->Destroy();
                PostFxResources.AOAccumTex[i] = nullptr;
            }
        }
        PostFxResources.nAOAccumFrame = 0;
        for (auto* rt : { &PostFxResources.ContactRawTex, &PostFxResources.ContactRawHalfTex, &PostFxResources.ContactTex,
                          &PostFxResources.ContactAccumTex[0], &PostFxResources.ContactAccumTex[1] })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        SAFE_RELEASE(PostFxResources.ContactRawSurf);
        SAFE_RELEASE(PostFxResources.ContactRawHalfSurf);
        SAFE_RELEASE(PostFxResources.ContactSurf);
        SAFE_RELEASE(PostFxResources.ContactAccumSurf[0]);
        SAFE_RELEASE(PostFxResources.ContactAccumSurf[1]);
        PostFxResources.ContactResult = nullptr;
        PostFxResources.nContactAccumFrame = 0;
        PostFxResources.bContactValid = false;
        PostFxResources.bGlassFrameValid = false;
        SAFE_RELEASE(PostFxResources.SSRDebugSurf);
        if (PostFxResources.SSRDebugTex)
        {
            PostFxResources.SSRDebugTex->Destroy();
            PostFxResources.SSRDebugTex = nullptr;
        }
        for (int half = 0; half < 2; ++half)
            for (int i = 0; i < 2; ++i)
            {
                SAFE_RELEASE(PostFxResources.SSRAccumSurf[half][i]);
                if (PostFxResources.SSRAccumTex[half][i])
                {
                    PostFxResources.SSRAccumTex[half][i]->Destroy();
                    PostFxResources.SSRAccumTex[half][i] = nullptr;
                }
            }
        PostFxResources.nSSRAccumFrame = 0;
        for (auto* rt : { &PostFxResources.GIRawTex, &PostFxResources.GIDenoisedTex, &PostFxResources.GIAccumTex[0], &PostFxResources.GIAccumTex[1], &PostFxResources.GIFullTex })
        {
            if (*rt)
            {
                (*rt)->Destroy();
                *rt = nullptr;
            }
        }
        SAFE_RELEASE(PostFxResources.GIRawSurf);
        SAFE_RELEASE(PostFxResources.GIDenoisedSurf);
        SAFE_RELEASE(PostFxResources.GIAccumSurf[0]);
        SAFE_RELEASE(PostFxResources.GIAccumSurf[1]);
        SAFE_RELEASE(PostFxResources.GIFullSurf);
        PostFxResources.GIResult = nullptr;
        SAFE_RELEASE(PostFxResources.SceneDepthSurf);
        if (PostFxResources.SceneDepthTex)
        {
            PostFxResources.SceneDepthTex->Destroy();
            PostFxResources.SceneDepthTex = nullptr;
        }
        PostFxResources.nSceneDepthFrame = 0;
        for (int i = 0; i < 3; ++i)
        {
            SAFE_RELEASE(PostFxResources.CloudSurf[i]);
            if (PostFxResources.CloudTex[i])
            {
                PostFxResources.CloudTex[i]->Destroy();
                PostFxResources.CloudTex[i] = nullptr;
            }
        }
        PostFxResources.nCloudAccumFrame = 0;
        for (auto& target : PostFxResources.CloudReflectionHalf)
        {
            SAFE_RELEASE(target.surf);
            SAFE_RELEASE(target.tex);
            target = {};
        }
        PostFxResources.CloudReflectionHalfSurf = nullptr;
        PostFxResources.CloudReflectionHalfTex = nullptr;
        SAFE_RELEASE(PostFxResources.CloudSkyRefSurf);
        if (PostFxResources.CloudSkyRefTex)
        {
            PostFxResources.CloudSkyRefTex->Destroy();
            PostFxResources.CloudSkyRefTex = nullptr;
        }
        {
            auto& R = PostFxResources;
            std::pair<rage::grcRenderTargetPC**, IDirect3DSurface9**> skyTables[] =
                { { &R.SkyTransmittanceTex, &R.SkyTransmittanceSurf }, { &R.SkyMultiScatterTex, &R.SkyMultiScatterSurf }, { &R.SkyViewTex, &R.SkyViewSurf } };
            for (auto [tex, surf] : skyTables)
            {
                SAFE_RELEASE(*surf);
                if (*tex)
                {
                    (*tex)->Destroy();
                    *tex = nullptr;
                }
            }
            R.fSkyTablesHaze = -1.0f;
            R.nSkyViewFrame = 0;
        }
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(PostFxResources.CloudMarchSurf[i]);
            if (PostFxResources.CloudMarchTex[i])
            {
                PostFxResources.CloudMarchTex[i]->Destroy();
                PostFxResources.CloudMarchTex[i] = nullptr;
            }
        }
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(PostFxResources.SkinLightSurf[i]);
            if (PostFxResources.SkinLightTex[i])
            {
                PostFxResources.SkinLightTex[i]->Destroy();
                PostFxResources.SkinLightTex[i] = nullptr;
            }
        }
        PostFxResources.nGIAccumFrame = 0;
        PostFxResources.bSSRDebugValid = false;
        PostFxResources.bSSRValidThisFrame = false;
        PostFxResources.nSSRHistoryFrame = 0;
        PostFxResources.bSSRReprojValid = false;
    }

    // From OnDeviceLost until OnDeviceReset has made the targets again. The game resets the device on the main thread
    // while the render thread still draws: a frame drawn in between found AO's targets released and crashed. Each hook
    // that starts our passes leaves them out meanwhile; it narrows the window, the passes still check their targets.
    static inline std::atomic<bool> bDeviceLost = false;

    static void __fastcall OnDeviceLost()
    {
        bDeviceLost = true;
        FusionLog::Write("PostFx", "Device", "lost, thread %lu", GetCurrentThreadId());
        PostFxResources.ReleaseTextures();
        PostFxResources.ReleaseWaterMask();
        UnbindGlassReflections();
        SAFE_RELEASE(PostFxResources.GlassParamsTex);
        ReleaseScreenSpaceTargets();
        TemporalAA::ReleaseResources();
        // PostFxResources.mSpecularAoRT    =nullptr;
        PostFxResources.mNormalRT = nullptr;
        PostFxResources.mDiffuseRT = nullptr;
        PostFxResources.mMaterialIdRT = nullptr;
        // PostFxResources.mSpecularRT      =nullptr;
        // PostFxResources.mDepthRT         =nullptr;
        // PostFxResources.mStencilRT       =nullptr;
        PostFxResources.mFullScreenRT = nullptr;
        // PostFxResources.mFullScreenRT2   =nullptr;
        PostFxResources.mHalfScreenRT = nullptr;
        // PostFxResources.mCascadeAtlasRT  =nullptr;
        // PostFxResources.mQuarterScreenRT =nullptr;
        // PostFxResources.mBloomRT         =nullptr;
        // PostFxResources.mCurrentLum      =nullptr;

        if (mQuadVertexBuffer)
        {
            mQuadVertexBuffer->Release();
            mQuadVertexBuffer = nullptr;
        }

        if (mQuadVertexDecl)
        {
            mQuadVertexDecl->Release();
            mQuadVertexDecl = nullptr;
        }
        if (PostFxResources.AOEffect)
            PostFxResources.AOEffect->OnLostDevice();
        if (PostFxResources.SSREffect)
            PostFxResources.SSREffect->OnLostDevice();
        if (PostFxResources.CloudsEffect)
            PostFxResources.CloudsEffect->OnLostDevice();
        if (PostFxResources.SkyEffect)
            PostFxResources.SkyEffect->OnLostDevice();
        EffectBindings().clear();
        if (PostFxResources.WetGroundEffect)
            PostFxResources.WetGroundEffect->OnLostDevice();
        if (PostFxResources.HeadlightGlintsEffect)
            PostFxResources.HeadlightGlintsEffect->OnLostDevice();
        PostFxResources.ReleaseWetCopies();
        ReleaseProfiler();

        for (auto i = 0; i < PostFxResources.nAmbientOcclusionMaxMipLevel; ++i)
            SAFE_RELEASE(PostFxResources.AOCamDepthSurf[i]);
        SAFE_RELEASE(PostFxResources.AOSurf);
        SAFE_RELEASE(PostFxResources.AOBlurSurf);

        if (PostFxResources.AOCamDepthTex)
        {
            PostFxResources.AOCamDepthTex->Destroy();
            PostFxResources.AOCamDepthTex = nullptr;
        }
        if (PostFxResources.AOTex)
        {
            PostFxResources.AOTex->Destroy();
            PostFxResources.AOTex = nullptr;
        }
        if (PostFxResources.AOBlurTex)
        {
            PostFxResources.AOBlurTex->Destroy();
            PostFxResources.AOBlurTex = nullptr;
        }
    }

    // The G-buffer targets, looked up by name once a frame (lighting phase, fog pass) and not only at a
    // device reset: the game creates them anew when the render scale changes, as turning FSR on or off
    // does, and the ones kept from before were another texture by then. SSR and SSGI read a depth that
    // was not this frame's, and the accumulations, testing it against last frame's copy, dropped their
    // history on every pixel.
    static void RefreshGBufferTargets()
    {
        PostFxResources.mNormalRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_1_");
        PostFxResources.mDiffuseRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_0_");
        PostFxResources.mSpecularRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_2_");
        PostFxResources.mDepthRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_3_");
        // Not the stencil buffer: the G-buffer pass writes each material's ID (whole steps of 1/255)
        // to it, as R32F or R16F, for the lighting and fog shaders.
        PostFxResources.mMaterialIdRT = rage::grcTextureFactoryPC::GetRTByName("_STENCIL_BUFFER_");
    }

    static void __fastcall OnDeviceReset()
    {
        RefreshGBufferTargets();
        // PostFxResources.mCascadeAtlasRT = rage::grcTextureFactoryPC::GetRTByName( "CASCADE_ATLAS"         );
        PostFxResources.mFullScreenRT = rage::grcTextureFactoryPC::GetRTByName("FullScreenCopy");
        // PostFxResources.mFullScreenRT2  = rage::grcTextureFactoryPC::GetRTByName( "FullScreenCopy2"       );
        PostFxResources.mHalfScreenRT = rage::grcTextureFactoryPC::GetRTByName("Quarter Screen 0");
        // PostFxResources.mQuarterScreenRT= rage::grcTextureFactoryPC::GetRTByName( "Blur Screen 0"         );
        // PostFxResources.mBloomRT        = rage::grcTextureFactoryPC::GetRTByName( "Blur Screen 2 Copy"    );
        // PostFxResources.mCurrentLum     = rage::grcTextureFactoryPC::GetRTByName( "Current Lum"           );

        auto width = *rage::grcDevice::ms_nActiveWidth;
        auto height = *rage::grcDevice::ms_nActiveHeight;

        PostFxResources.createTextures(width, height, hm);
        // Motion vectors, depth and the reactive mask at the size the scene renders at
        TemporalAA::CreateResources(RenderScale::ToRenderWidth(width), RenderScale::ToRenderHeight(height));

        D3DVERTEXELEMENT9 vertexDeclElements[] =
        {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END()
        };

        rage::grcDevice::GetD3DDevice()->CreateVertexDeclaration(vertexDeclElements, &mQuadVertexDecl);

        RealDevice(rage::grcDevice::GetD3DDevice())->CreateVertexBuffer(6 * sizeof(VertexFormat), 0, 0, D3DPOOL_DEFAULT, &mQuadVertexBuffer, NULL);

        VertexFormat* vertexData = 0;
        D3DXVECTOR2 pixelSize = D3DXVECTOR2(1.0f / width, 1.0f / height);

        mQuadVertexBuffer->Lock(0, 0, (void**)&vertexData, 0);

        vertexData[0] = { -1.0f - pixelSize.x, -1.0f + pixelSize.y, 0.0f, 0.0f, 1.0f };
        vertexData[1] = { -1.0f - pixelSize.x,  1.0f + pixelSize.y, 0.0f, 0.0f, 0.0f };
        vertexData[2] = { 1.0f - pixelSize.x, -1.0f + pixelSize.y, 0.0f, 1.0f, 1.0f };
        vertexData[3] = { -1.0f - pixelSize.x,  1.0f + pixelSize.y, 0.0f, 0.0f, 0.0f };
        vertexData[4] = { 1.0f - pixelSize.x,  1.0f + pixelSize.y, 0.0f, 1.0f, 0.0f };
        vertexData[5] = { 1.0f - pixelSize.x, -1.0f + pixelSize.y, 0.0f, 1.0f, 1.0f };

        mQuadVertexBuffer->Unlock();

        // The screen space effects work on the scene, at the size it renders at
        width = static_cast<int32_t>(RenderScale::ToRenderWidth(static_cast<uint32_t>(width)));
        height = static_cast<int32_t>(RenderScale::ToRenderHeight(static_cast<uint32_t>(height)));

        if (PostFxResources.AOEffect)
            PostFxResources.AOEffect->OnResetDevice();
        if (PostFxResources.SSREffect)
            PostFxResources.SSREffect->OnResetDevice();
        if (PostFxResources.CloudsEffect)
            PostFxResources.CloudsEffect->OnResetDevice();
        if (PostFxResources.SkyEffect)
            PostFxResources.SkyEffect->OnResetDevice();
        if (PostFxResources.WetGroundEffect)
            PostFxResources.WetGroundEffect->OnResetDevice();
        if (PostFxResources.HeadlightGlintsEffect)
            PostFxResources.HeadlightGlintsEffect->OnResetDevice();

        for (auto i = 0; i < PostFxResources.nAmbientOcclusionMaxMipLevel; ++i)
            SAFE_RELEASE(PostFxResources.AOCamDepthSurf[i]);
        SAFE_RELEASE(PostFxResources.AOSurf);
        SAFE_RELEASE(PostFxResources.AOBlurSurf);

        if (PostFxResources.AOCamDepthTex)
        {
            PostFxResources.AOCamDepthTex->Destroy();
            PostFxResources.AOCamDepthTex = nullptr;
        }
        if (PostFxResources.AOTex)
        {
            PostFxResources.AOTex->Destroy();
            PostFxResources.AOTex = nullptr;
        }
        if (PostFxResources.AOBlurTex)
        {
            PostFxResources.AOBlurTex->Destroy();
            PostFxResources.AOBlurTex = nullptr;
        }
        ReleaseScreenSpaceTargets();

        auto pDevice = rage::grcDevice::GetD3DDevice();

        auto aoDesc = rage::OwnRenderTargetDesc(rage::GRCFMT_UNKNOWN);

        aoDesc.mFormat = rage::GRCFMT_R32F;
        aoDesc.mLevels = PostFxResources.nAmbientOcclusionMaxMipLevel;
        PostFxResources.AOCamDepthTex = rage::CreateEmptyRenderTarget("AOCamDepthTex", width, height, 32, aoDesc);

        aoDesc.mLevels = 1;
        PostFxResources.SceneDepthTex = rage::CreateEmptyRenderTarget("SceneDepthCopy", width, height, 32, aoDesc, PostFxResources.SceneDepthSurf);
        PostFxResources.nSceneDepthFrame = 0;

        aoDesc.mFormat = rage::GRCFMT_L8;
        aoDesc.mLevels = 1;
        // At half size, the size of AOCamDepthTex's first mip
        const auto aoWidth = PostFxResources.bAmbientOcclusionHalfRes ? std::max(1, int(width) >> 1) : int(width);
        const auto aoHeight = PostFxResources.bAmbientOcclusionHalfRes ? std::max(1, int(height) >> 1) : int(height);
        PostFxResources.AOTex = rage::CreateEmptyRenderTarget("AOTex", aoWidth, aoHeight, 8, aoDesc);
        PostFxResources.AOBlurTex = rage::CreateEmptyRenderTarget("AOBlurTex", aoWidth, aoHeight, 8, aoDesc);
        if (PostFxResources.fAmbientOcclusionTemporal > 0.0f)
        {
            // 16-bit: blending a small share of each frame into 8 bits gets stuck on its steps.
            aoDesc.mFormat = rage::GRCFMT_R16F;
            static const char* names[2] = { "AOAccumTex0", "AOAccumTex1" };
            for (int i = 0; i < 2; ++i)
                PostFxResources.AOAccumTex[i] = rage::CreateEmptyRenderTarget(names[i], aoWidth, aoHeight, 16, aoDesc, PostFxResources.AOAccumSurf[i]);
        }

        {
            aoDesc.mFormat = rage::GRCFMT_A16B16G16R16F;
            aoDesc.mLevels = 1;
            PostFxResources.SSRTex = rage::CreateEmptyRenderTarget("SSRTex", width, height, 64, aoDesc, PostFxResources.SSRSurf);

            PostFxResources.SSRHistoryTex = rage::CreateEmptyRenderTarget("SSRHistoryTex", width, height, 64, aoDesc, PostFxResources.SSRHistorySurf);

            PostFxResources.SSRDenoisedTex = rage::CreateEmptyRenderTarget("SSRDenoisedTex", width, height, 64, aoDesc, PostFxResources.SSRDenoisedSurf);

            PostFxResources.SSRHalfTex = rage::CreateEmptyRenderTarget("SSRHalfTex", width / 2, height / 2, 64, aoDesc, PostFxResources.SSRHalfSurf);
            {
                static const char* cloudNames[3] = { "CloudTex", "CloudAccumTex0", "CloudAccumTex1" };
                for (int i = 0; i < 3; ++i)
                    PostFxResources.CloudTex[i] = rage::CreateEmptyRenderTarget(cloudNames[i], width / 2, height / 2, 64, aoDesc, PostFxResources.CloudSurf[i]);
                PostFxResources.nCloudAccumFrame = 0;
                PostFxResources.CloudSkyRefTex = rage::CreateEmptyRenderTarget("CloudSkyRefTex", 1, 1, 64, aoDesc, PostFxResources.CloudSkyRefSurf);
                static const char* marchNames[2] = { "CloudMarchTex0", "CloudMarchTex1" };
                for (int i = 0; i < 2; ++i)
                    PostFxResources.CloudMarchTex[i] = rage::CreateEmptyRenderTarget(marchNames[i], width / 2, height / 2, 64, aoDesc, PostFxResources.CloudMarchSurf[i]);
                // Cleared: it is only ever blended into, and a NaN left in it would stay for good.
                if (PostFxResources.CloudSkyRefSurf && pDevice)
                    pDevice->ColorFill(PostFxResources.CloudSkyRefSurf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
                PostFxResources.nCloudSkyRefFrame = 0;
            }
            {
                // The PBR sky's tables (pbrsky.ixx), of fixed sizes; drawn again before their first use.
                auto& R = PostFxResources;
                R.SkyTransmittanceTex = rage::CreateEmptyRenderTarget("SkyTransmittanceTex", 256, 64, 64, aoDesc, R.SkyTransmittanceSurf);
                R.SkyMultiScatterTex = rage::CreateEmptyRenderTarget("SkyMultiScatterTex", 32, 32, 64, aoDesc, R.SkyMultiScatterSurf);
                R.SkyViewTex = rage::CreateEmptyRenderTarget("SkyViewTex", 192, 108, 64, aoDesc, R.SkyViewSurf);
                R.fSkyTablesHaze = -1.0f;
                R.nSkyViewFrame = 0;
            }
            PostFxResources.SSRHalfDenoisedTex = rage::CreateEmptyRenderTarget("SSRHalfDenoisedTex", width / 2, height / 2, 64, aoDesc, PostFxResources.SSRHalfDenoisedSurf);
            {
                static const char* fallbackNames[2] = { "SSRFallbackTex", "SSRHalfFallbackTex" };
                static const char* traceNames[2] = { "SSRTraceTex", "SSRHalfTraceTex" };
                for (int half = 0; half < 2; ++half)
                {
                    const auto w = half ? width / 2 : width, hgt = half ? height / 2 : height;
                    PostFxResources.SSRFallbackTex[half] = rage::CreateEmptyRenderTarget(fallbackNames[half], w, hgt, 64, aoDesc,
                        PostFxResources.SSRFallbackSurf[half]);
                    PostFxResources.SSRSpreadTex[half] = rage::CreateEmptyRenderTarget(half ? "SSRHalfSpreadTex" : "SSRSpreadTex", w, hgt,
                        64, aoDesc, PostFxResources.SSRSpreadSurf[half]);
                    PostFxResources.SSRTraceTex[half] = rage::CreateEmptyRenderTarget(traceNames[half], w, hgt, 64, aoDesc,
                        PostFxResources.SSRTraceSurf[half]);
                    if (PostFxResources.fSSRTemporalBlend > 0.0f)
                        PostFxResources.SSRHitDistTex[half] = rage::CreateEmptyRenderTarget(half ? "SSRHalfHitDistTex" : "SSRHitDistTex",
                            w, hgt, 64, aoDesc, PostFxResources.SSRHitDistSurf[half]);
                }
                auto marchDesc = rage::OwnRenderTargetDesc(rage::GRCFMT_R32F);
                PostFxResources.SSRMarchDepthTex = rage::CreateEmptyRenderTarget("SSRMarchDepthTex", width / 2, height / 2, 32, marchDesc,
                    PostFxResources.SSRMarchDepthSurf);
            }

            PostFxResources.ContactRawTex = rage::CreateEmptyRenderTarget("ContactShadowRawTex", width, height, 64, aoDesc, PostFxResources.ContactRawSurf);
            if (PostFxResources.bContactShadowsHalfRes)
            {
                PostFxResources.ContactRawHalfTex = rage::CreateEmptyRenderTarget("ContactShadowRawHalfTex", width / 2, height / 2, 64, aoDesc, PostFxResources.ContactRawHalfSurf);
            }
            // Smoothed and accumulated at the size they are marched at, brought up to full size last
            const int contactWidth = PostFxResources.bContactShadowsHalfRes ? width / 2 : width;
            const int contactHeight = PostFxResources.bContactShadowsHalfRes ? height / 2 : height;
            PostFxResources.ContactTex = rage::CreateEmptyRenderTarget("ContactShadowTex", contactWidth, contactHeight, 64, aoDesc, PostFxResources.ContactSurf);
            if (PostFxResources.fContactTemporalBlend > 0.0f)
            {
                static const char* names[2] = { "ContactShadowAccumTex0", "ContactShadowAccumTex1" };
                for (int i = 0; i < 2; ++i)
                    PostFxResources.ContactAccumTex[i] = rage::CreateEmptyRenderTarget(names[i], contactWidth, contactHeight, 64, aoDesc, PostFxResources.ContactAccumSurf[i]);
            }

            if (PostFxResources.fSSRTemporalBlend > 0.0f)
            {
                static const char* names[2][2] = { { "SSRAccumTex0", "SSRAccumTex1" }, { "SSRHalfAccumTex0", "SSRHalfAccumTex1" } };
                for (int half = 0; half < 2; ++half)
                    for (int i = 0; i < 2; ++i)
                        PostFxResources.SSRAccumTex[half][i] = rage::CreateEmptyRenderTarget(names[half][i], half ? width / 2 : width,
                            half ? height / 2 : height, 64, aoDesc, PostFxResources.SSRAccumSurf[half][i]);
            }

            {
                auto create = [&](rage::grcRenderTargetPC*& rt, IDirect3DSurface9*& surf, const char* name)
                {
                    rt = rage::CreateEmptyRenderTarget(name, width / 2, height / 2, 64, aoDesc, surf);
                };
                create(PostFxResources.GIRawTex, PostFxResources.GIRawSurf, "GIRawTex");
                create(PostFxResources.GIDenoisedTex, PostFxResources.GIDenoisedSurf, "GIDenoisedTex");
                create(PostFxResources.GIAccumTex[0], PostFxResources.GIAccumSurf[0], "GIAccumTex0");
                create(PostFxResources.GIAccumTex[1], PostFxResources.GIAccumSurf[1], "GIAccumTex1");
                PostFxResources.GIFullTex = rage::CreateEmptyRenderTarget("GIFullTex", width, height, 64, aoDesc, PostFxResources.GIFullSurf);
            }

            for (int i = 0; i < 2; ++i)
            {
                PostFxResources.SkinLightTex[i] = rage::CreateEmptyRenderTarget(i ? "SkinLightTex1" : "SkinLightTex0", width, height, 64, aoDesc, PostFxResources.SkinLightSurf[i]);
            }

            PostFxResources.SSRDebugTex = rage::CreateEmptyRenderTarget("SSRDebugTex", width, height, 64, aoDesc, PostFxResources.SSRDebugSurf);

            IDirect3DSurface9* oldRT = nullptr;
            pDevice->GetRenderTarget(0, &oldRT);
            for (auto* surf : { PostFxResources.SSRSurf, PostFxResources.SSRHistorySurf })
            {
                if (!surf)
                    continue;
                pDevice->SetRenderTarget(0, surf);
                pDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
            }
            if (oldRT)
            {
                pDevice->SetRenderTarget(0, oldRT);
                oldRT->Release();
            }
        }

        auto& R = PostFxResources;
        if (R.AOCamDepthTex && R.AOCamDepthTex->mD3DTexture)
            for (auto i = 0; i < R.nAmbientOcclusionMaxMipLevel; ++i)
                R.AOCamDepthTex->mD3DTexture->GetSurfaceLevel(i, &R.AOCamDepthSurf[i]);
        if (R.AOTex && R.AOTex->mD3DTexture)
            R.AOTex->mD3DTexture->GetSurfaceLevel(0, &R.AOSurf);
        if (R.AOBlurTex && R.AOBlurTex->mD3DTexture)
            R.AOBlurTex->mD3DTexture->GetSurfaceLevel(0, &R.AOBlurSurf);
        FusionLog::Write("PostFx", "Device", "reset %dx%d, scene %dx%d, thread %lu, AO targets %s", int(*rage::grcDevice::ms_nActiveWidth),
                         int(*rage::grcDevice::ms_nActiveHeight), int(width), int(height), GetCurrentThreadId(), R.AOCamDepthSurf[0] && R.AOSurf && R.AOBlurSurf ? "made" : "MISSING");
        bDeviceLost = false;
    }

    static void Init()
    {
        static bool initialized = false;
        if (initialized)
            return;

        // none, fxaa, smaa, blend, edge
        UsePostFxAA = FusionFixSettings.GetRef("PREF_ANTIALIASING");
        IniReload::Add(OnIniReload);

        auto onLostCB = rage::grcDevice::Functor0(NULL, OnDeviceLost, NULL, 0);
        auto onResetCB = rage::grcDevice::Functor0(NULL, OnDeviceReset, NULL, 0);
        rage::grcDevice::RegisterDeviceCallbacks(onLostCB, onResetCB);

        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Init, &hm);

        PostFxResources.loadShaders(rage::grcDevice::GetD3DDevice(), hm);

        OnDeviceReset();

        TemporalAA::ProfileMotion = [](IDirect3DDevice9* device, bool begin) { ProfilerMark(device, kProfMotion, begin); };
        HDROutput::ProfileOutput = [](IDirect3DDevice9* device, bool begin) { ProfilerMark(device, kProfHDROutput, begin); };
        Upscaler::Profile = [](IDirect3DDevice9* device, Upscaler::ProfilePart part, bool begin)
        {
            static constexpr int kSections[] =
            {
                kProfResolveReactive, kProfResolveUpscale, kProfFrameGeneration, kProfFrameGenerationCopies,
                kProfFrameGenerationGenerate, kProfFrameGenerationShow, kProfFrameGenerationPresent,
            };
            const auto i = static_cast<size_t>(part);
            if (i < std::size(kSections))
                ProfilerMark(device, kSections[i], begin);
        };
        TemporalAA::OnGBufferEnd = [](IDirect3DDevice9* device)
        {
            CopySceneDepth(device);
            RenderWetGround(device);
        };
        RenderScale::TraceDepth = [](const char* what, IDirect3DSurface9* depth, IDirect3DSurface9* target, DWORD flags)
        {
            if (SSRTrace::Active())
                SSRTrace::Line("render scale: %s %s, target %s%s", what, SSRTrace::SurfaceName(depth).c_str(), SSRTrace::SurfaceName(target).c_str(),
                               flags ? (std::string(", flags ") + std::to_string(flags)).c_str() : "");
        };

        initialized = true;
    }

    // At the end of the G-buffer pass (TemporalAA::OnGBufferEnd, the device state saved around it): this frame's
    // depth into SceneDepthTex. With FSR's render scale, _DEFERRED_GBUFFER_3_ read in the lighting phase, where the
    // game keeps it bound as its depth buffer, came out in whole steps of 1/255 or as another depth altogether (15 m,
    // 983 m and 0.35 m in a room 3 to 6 m deep), while temporal AA's motion vectors, read from it here, and the fog
    // pass's copy, made after lighting, held the right depths. SSR, SSGI, contact shadows and SSAO marched and tested
    // their histories against that.
    static void CopySceneDepth(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        RefreshGBufferTargets();
        if (!pDevice || !R.SceneDepthSurf || !R.Blit_PS || !R.mDepthRT || !R.mDepthRT->mD3DTexture)
            return;
        ProfilerScope timed(pDevice, kProfDepthCopy);
        pDevice->SetRenderTarget(0, R.SceneDepthSurf);
        for (DWORD i = 1; i < 4; ++i)
            pDevice->SetRenderTarget(i, nullptr);
        pDevice->SetDepthStencilSurface(nullptr);
        static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kStates[] =
        {
            { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
            { D3DRS_STENCILENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_COLORWRITEENABLE, 0x0F },
            { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID },
            { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
        };
        for (auto [state, value] : kStates)
            pDevice->SetRenderState(state, value);
        SetTextureBoth(pDevice, 0, R.mDepthRT->mD3DTexture);
        pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        BlitToTarget(pDevice, R.SceneDepthSurf);
        R.nSceneDepthFrame = FrameHistory::Frame();
        if (SSRTrace::Active())
        {
            const auto& camera = FrameHistory::Current();
            SSRTrace::DepthProbe(pDevice, "G-buffer end depth copy", R.SceneDepthTex->mD3DTexture, camera.Near, camera.Far);
        }
    }

    // For the SSR trace, at the start of the lighting phase, before any FusionFix pass: what the game has on each
    // sampler and as its depth buffer.
    static void TraceLightingInputs(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        if (!SSRTrace::Active() || !pDevice)
            return;
        std::string bound;
        for (DWORD slot = 0; slot < 16; ++slot)
        {
            IDirect3DBaseTexture9* tex = nullptr;
            pDevice->GetTexture(slot, &tex);
            if (tex)
                bound += " s" + std::to_string(slot) + "=" + SSRTrace::TextureName(tex);
            SAFE_RELEASE(tex);
        }
        IDirect3DSurface9* ds = nullptr;
        IDirect3DSurface9* rt0 = nullptr;
        pDevice->GetDepthStencilSurface(&ds);
        pDevice->GetRenderTarget(0, &rt0);
        IDirect3DSurface9* depthSurface = nullptr;
        if (R.mDepthRT && R.mDepthRT->mD3DTexture)
            R.mDepthRT->mD3DTexture->GetSurfaceLevel(0, &depthSurface);
        D3DSURFACE_DESC dsDesc = {}, rtDesc = {};
        if (ds)
            ds->GetDesc(&dsDesc);
        if (rt0)
            rt0->GetDesc(&rtDesc);
        SSRTrace::Line("lighting inputs: depth buffer %p %ux%u format %u%s, target %ux%u format %u;%s", static_cast<void*>(ds),
            dsDesc.Width, dsDesc.Height, unsigned(dsDesc.Format), ds && ds == depthSurface ? " (is _DEFERRED_GBUFFER_3_)" : "",
            rtDesc.Width, rtDesc.Height, unsigned(rtDesc.Format), bound.c_str());

        SAFE_RELEASE(depthSurface);
        SAFE_RELEASE(ds);
        SAFE_RELEASE(rt0);
    }

    // Copies the texture on s0 over all of the bound target 0 through Blit_PS, with a quad of its own. The fog pass's
    // own quad was drawn by the game's vertex shader, which RenderScale feeds the render size (globalScreenSize): on a
    // target of the screen size it covered the copy otherwise than the texture, and with FSR's render scale last
    // frame's depth sat elsewhere than this frame's, so every accumulation dropped its history on every pixel.
    static void BlitToTarget(IDirect3DDevice9* pDevice, IDirect3DSurface9* target)
    {
        D3DSURFACE_DESC desc = {};
        if (!target || FAILED(target->GetDesc(&desc)))
            return;
        // DrawPrimitiveUP leaves stream 0 unbound, and the fog pass draws from it after the copies.
        IDirect3DVertexShader9* oldVS = nullptr;
        pDevice->GetVertexShader(&oldVS);
        SavedVertexInput savedVertexInput(pDevice);

        D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        pDevice->SetViewport(&vp);
        pDevice->SetVertexShader(nullptr);
        pDevice->SetFVF(kScreenVertexFVF);
        const float w = float(desc.Width), h = float(desc.Height);
        const ScreenQuad quad = ScreenQuad::Full(w, h);
        pDevice->SetPixelShader(PostFxResources.Blit_PS);
        quad.Draw(pDevice);

        pDevice->SetVertexShader(oldVS);
        SAFE_RELEASE(oldVS);
    }

    static void NewFog()
    {
        RefreshGBufferTargets();
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();
        ProfilerScope timed(pDevice, kProfFogPass);

        IDirect3DSurface9* prevSurface = nullptr;
        IDirect3DSurface9* prevDepthStencilSurface = nullptr;
        IDirect3DPixelShader9* prevPS = nullptr;

        IDirect3DBaseTexture9* prevTex[2] = { nullptr };

        // Previous sampler/render states
        DWORD prevMinFilter[2] = { D3DTEXF_NONE };
        DWORD prevMagFilter[2] = { D3DTEXF_NONE };
        DWORD prevMipFilter[2] = { D3DTEXF_NONE };
        DWORD prevAddressU[2] = { D3DTADDRESS_WRAP };
        DWORD prevAddressV[2] = { D3DTADDRESS_WRAP };

        // Store previous sampler states, renderstates, surfaces, textures, shaders
        pDevice->GetSamplerState(0, D3DSAMP_MINFILTER, &prevMinFilter[0]);
        pDevice->GetSamplerState(0, D3DSAMP_MAGFILTER, &prevMagFilter[0]);
        pDevice->GetSamplerState(0, D3DSAMP_MIPFILTER, &prevMipFilter[0]);
        pDevice->GetSamplerState(0, D3DSAMP_ADDRESSU, &prevAddressU[0]);
        pDevice->GetSamplerState(0, D3DSAMP_ADDRESSV, &prevAddressV[0]);

        pDevice->GetSamplerState(1, D3DSAMP_MINFILTER, &prevMinFilter[1]);
        pDevice->GetSamplerState(1, D3DSAMP_MAGFILTER, &prevMagFilter[1]);
        pDevice->GetSamplerState(1, D3DSAMP_MIPFILTER, &prevMipFilter[1]);
        pDevice->GetSamplerState(1, D3DSAMP_ADDRESSU, &prevAddressU[1]);
        pDevice->GetSamplerState(1, D3DSAMP_ADDRESSV, &prevAddressV[1]);

        pDevice->GetRenderTarget(0, &prevSurface);
        pDevice->GetDepthStencilSurface(&prevDepthStencilSurface);

        pDevice->GetTexture(0, &prevTex[0]);
        pDevice->GetTexture(1, &prevTex[1]);

        pDevice->GetPixelShader(&prevPS);

        {
            if (PostFxResources.PreAlphaDepthCopyRT)
            {
                PostFxResources.PreAlphaDepthCopyRT->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.PreAlphaDepthSurface);

                // Blit depth to texture 
                if (PostFxResources.Blit_PS)
                {
                    if (PostFxResources.PreAlphaDepthSurface)
                    {
                        pDevice->SetRenderTarget(0, PostFxResources.PreAlphaDepthSurface);
                        pDevice->SetDepthStencilSurface(nullptr);

                        // _DEFERRED_GBUFFER_3_, bound here and not taken from what the game left on s0: whatever that
                        // was, last frame's depth disagreed with this one's everywhere and every accumulation (SSR, SSGI,
                        // contact shadows, GTAO) dropped its history on every pixel.
                        if (SSRTrace::Active())
                        {
                            IDirect3DBaseTexture9* bound = nullptr;
                            pDevice->GetTexture(0, &bound);
                            SSRTrace::Line("fog pass: depth copy, s0 held %s", SSRTrace::TextureName(bound).c_str());
                            SAFE_RELEASE(bound);
                        }
                        if (PostFxResources.mDepthRT && PostFxResources.mDepthRT->mD3DTexture)
                        {
                            SetTextureBoth(pDevice, 0, PostFxResources.mDepthRT->mD3DTexture);
                            SetSamplerStateBoth(pDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                            SetSamplerStateBoth(pDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                            SetSamplerStateBoth(pDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                        }

                        BlitToTarget(pDevice, PostFxResources.PreAlphaDepthSurface);

                        if (SSRTrace::Active())
                        {
                            const auto& camera = FrameHistory::Current();
                            SSRTrace::DepthProbe(pDevice, "fog depth", PostFxResources.mDepthRT ? PostFxResources.mDepthRT->mD3DTexture : nullptr,
                                                 camera.Near, camera.Far);
                            SSRTrace::DepthProbe(pDevice, "fog depth copy", PostFxResources.PreAlphaDepthCopyRT->mD3DTexture, camera.Near, camera.Far);
                        }
                        SetTextureBoth(pDevice, 0, prevTex[0]);
                        SetSamplerStateBoth(pDevice, 0, D3DSAMP_MINFILTER, prevMinFilter[0]);
                        SetSamplerStateBoth(pDevice, 0, D3DSAMP_MAGFILTER, prevMagFilter[0]);
                        SetSamplerStateBoth(pDevice, 0, D3DSAMP_MIPFILTER, prevMipFilter[0]);
                    }
                }
            }

            // The lit scene, sampler 1 of the fog pass: the volumetric clouds drawn into the game's own scene first, then
            // the light scattered under the skin if that runs, from a copy that has the clouds; the fog, the copy below
            // and SSR's history all take it. The other way round the clouds drew into the skin passes' own target, and
            // without temporal anti-aliasing the whole picture shook.
            RenderVolumetricClouds(pDevice, prevTex[1]);
            IDirect3DBaseTexture9* scene = prevTex[1];
            ProfilerMark(pDevice, kProfSkin, true);
            if (auto skin = RenderSkinScattering(pDevice, prevTex[1]))
                scene = skin;
            ProfilerMark(pDevice, kProfSkin, false);

            if (PostFxResources.FullScreenTex_temp1)
            {
                // Get custom rendertarget D3D surfaces
                PostFxResources.FullScreenTex_temp1->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.HDRFullScreenSurface);

                // Copy HDR fullscreen buffer at this stage to use it later in raindrop refraction shader
                if (PostFxResources.HDRFullScreenSurface && PostFxResources.Blit_PS)
                {
                    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, prevMinFilter[1]);
                    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, prevMagFilter[1]);
                    pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, prevMipFilter[1]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, prevAddressU[1]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, prevAddressV[1]);

                    pDevice->SetRenderTarget(0, PostFxResources.HDRFullScreenSurface);
                    pDevice->SetDepthStencilSurface(nullptr);

                    SetTextureBoth(pDevice, 0, scene);

                    // Its own quad, as for the depth copy above: SSR's and SSGI's history is taken from this copy and
                    // read where last frame's depth copy is.
                    BlitToTarget(pDevice, PostFxResources.HDRFullScreenSurface);

                    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, prevMinFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, prevMagFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, prevMipFilter[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, prevAddressU[0]);
                    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, prevAddressV[0]);
                    SetTextureBoth(pDevice, 0, prevTex[0]);

                    if ((PostFxResources.SSREnabled() || PostFxResources.SSGIEnabled()) && PostFxResources.SSRHistorySurf && PostFxResources.SSRSurf)
                    {
                        D3DVIEWPORT9 vpBeforeCapture;
                        pDevice->GetViewport(&vpBeforeCapture);

                        pDevice->SetRenderTarget(0, PostFxResources.SSRSurf);
                        pDevice->StretchRect(PostFxResources.HDRFullScreenSurface, nullptr, PostFxResources.SSRHistorySurf, nullptr, D3DTEXF_NONE);
                        pDevice->SetRenderTarget(0, PostFxResources.HDRFullScreenSurface);
                        PostFxResources.bSSRHistoryThisFrame = true;
                        SSRTrace::Line("fog pass: history copied");
                        PostFxResources.nSSRHistoryFrame = FrameHistory::Frame();

                        pDevice->SetViewport(&vpBeforeCapture);
                    }
                }
            }

            // Restore fog pass
            if (prevPS)
            {
                if (prevSurface)
                {
                    pDevice->SetRenderTarget(0, prevSurface);
                    pDevice->SetDepthStencilSurface(prevDepthStencilSurface);

                    pDevice->SetPixelShader(prevPS);

                    SetTextureBoth(pDevice, 1, scene);
                    // The air the fog pass and the forward shaders after it look through
                    AerialPerspectiveBind(pDevice);
                    ProfilerMark(pDevice, kProfFog, true);
                    hbDrawPrimitivePostFX.fun();
                    ProfilerMark(pDevice, kProfFog, false);
                    SetTextureBoth(pDevice, 1, prevTex[1]);
                }
            }
        }

        SAFE_RELEASE(PostFxResources.PreAlphaDepthSurface);
        SAFE_RELEASE(PostFxResources.HDRFullScreenSurface);

        SAFE_RELEASE(prevSurface);
        SAFE_RELEASE(prevDepthStencilSurface);

        SAFE_RELEASE(prevTex[0]);
        SAFE_RELEASE(prevTex[1]);

        SAFE_RELEASE(prevPS);
    }

    static void NewPostFX()
    {
        ProfilerNextFrame(rage::grcDevice::GetD3DDevice());

        // The game's own post processing without the scene, e.g. while the render targets are recreated
        if (!PostFxResources.mFullScreenRT || !PostFxResources.mFullScreenRT->mD3DTexture)
        {
            hbDrawPrimitivePostFX.fun();
            return;
        }

        IDirect3DPixelShader9* oldps = 0;
        IDirect3DVertexShader9* oldvs = 0;
        IDirect3DDevice9* pDevice = rage::grcDevice::GetD3DDevice();

        // get textures
        // if(PostFxResources.mNormalRT->mD3DTexture == nullptr)
        //     return;
        // PostFxResources.NormalTex = PostFxResources.mNormalRT->mD3DTexture;
        PostFxResources.DiffuseTex = PostFxResources.mDiffuseRT->mD3DTexture;
        PostFxResources.SpecularTex = PostFxResources.mSpecularRT->mD3DTexture;
        // PostFxResources.StencilTex = PostFxResources.mStencilRT->mD3DTexture;
        // PostFxResources.BloomTex = PostFxResources.mBloomRT->mD3DTexture;
        // PostFxResources.CurrentLumTex = PostFxResources.mCurrentLum->mD3DTexture;
        PostFxResources.DepthTex = PostFxResources.mDepthRT->mD3DTexture;
        // if (PostFxResources.mCascadeAtlasRT)
        //     PostFxResources.CascadeAtlasTex = PostFxResources.mCascadeAtlasRT->mD3DTexture;

        PostFxResources.FullScreenTex = PostFxResources.mFullScreenRT->mD3DTexture;
        PostFxResources.HalfScreenTex = PostFxResources.mHalfScreenRT->mD3DTexture;
        // PostFxResources.pQuarterHDRTex = PostFxResources.mQuarterScreenRT->mD3DTexture;

        // get surfaces
        PostFxResources.FullScreenTex->GetSurfaceLevel(0, &PostFxResources.FullScreenSurface);

        PostFxResources.FullScreenTex_temp1->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.FullScreenSurface_temp1);
        PostFxResources.FullScreenTex_temp2->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.FullScreenSurface_temp2);
        PostFxResources.edgesTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.edgesSurf);
        PostFxResources.blendTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.blendSurf);

        PostFxResources.FullScreenDownsampleTex->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.FullScreenDownsampleSurf);
        PostFxResources.FullScreenDownsampleTex2->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.FullScreenDownsampleSurf2);

        // if (PostFxResources.CascadeAtlasTex)
        //     PostFxResources.CascadeAtlasTex->GetSurfaceLevel(0, &PostFxResources.CascadeAtlasSurf);
        //PostFxResources.pShadowBlurTex1->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.pShadowBlurSurf1);
        //PostFxResources.pShadowBlurTex2->mD3DTexture->GetSurfaceLevel(0, &PostFxResources.pShadowBlurSurf2);

        pDevice->GetRenderTarget(0, &PostFxResources.backBuffer);
        FrameGeneration::OnPostProcessing();
        pDevice->GetPixelShader(&oldps);
        pDevice->GetVertexShader(&oldvs);

        saveRenderState();

        pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

        // new 
        {
            ProfilerScope timed(pDevice, kProfPost);
            PostFx3(pDevice, oldps, oldvs);
        }

        restoreRenderState();

        // restore
        pDevice->SetPixelShader(oldps);
        pDevice->SetVertexShader(oldvs);
        pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

        // release
        SAFE_RELEASE(oldps);
        SAFE_RELEASE(oldvs);

        SAFE_RELEASE(last_VertexDecl);
        SAFE_RELEASE(last_VertexBuffer);

        SAFE_RELEASE(PostFxResources.backBuffer);
        SAFE_RELEASE(PostFxResources.FullScreenSurface_temp1);
        SAFE_RELEASE(PostFxResources.FullScreenSurface_temp2);
        SAFE_RELEASE(PostFxResources.FullScreenDownsampleSurf);
        SAFE_RELEASE(PostFxResources.FullScreenDownsampleSurf2);
        SAFE_RELEASE(PostFxResources.edgesSurf);
        SAFE_RELEASE(PostFxResources.blendSurf);
        // SAFE_RELEASE(PostFxResources.CascadeAtlasSurf);

        SAFE_RELEASE(PostFxResources.FullScreenSurface);

        //SAFE_RELEASE(PostFxResources.pShadowBlurSurf1);
        //SAFE_RELEASE(PostFxResources.pShadowBlurSurf2);

        PostFxResources.renderTargetTex = nullptr;
        PostFxResources.textureRead = nullptr;
        PostFxResources.renderTargetSurf = nullptr;
        PostFxResources.surfaceRead = nullptr;
    }

    static HRESULT PostFx3(LPDIRECT3DDEVICE9 pDevice, IDirect3DPixelShader9* pShader, IDirect3DVertexShader9* vShader)
    {
        auto currGrcViewport = rage::GetCurrentViewport();

        HRESULT hr = S_FALSE;

        DWORD OldSRGB = 0;
        DWORD OldSampler = 0;

        // save render state between post processing steps, 
        // in general each step expects the environment as the game 
        // leaves it at the time post processing is used, 
        // each step works individually from each other
        for (int i = 0; i < PostfxTextureCount; i++)
        {
            pDevice->GetTexture(i, &PostFxResources.prePostFx[i]);
            pDevice->GetSamplerState(i, D3DSAMP_MAGFILTER, &PostFxResources.Samplers[i]);
        }

        // main postfx passes
        {
            if (PostFxResources.FullScreenTex_temp1 && PostFxResources.FullScreenTex /*&& PostFxResources.aoTex*/)
            {
                PostFxResources.renderTargetTex = PostFxResources.FullScreenTex_temp1->mD3DTexture;

                PostFxResources.textureRead = PostFxResources.FullScreenTex;
                PostFxResources.surfaceRead = PostFxResources.FullScreenSurface;

                PostFxResources.renderTargetTex = PostFxResources.FullScreenTex_temp1->mD3DTexture;
                PostFxResources.renderTargetSurf = PostFxResources.FullScreenSurface_temp1;

                // ready for new post processing?
                if (PostFxResources.backBuffer && PostFxResources.renderTargetSurf && PostFxResources.surfaceRead)
                {
                    //if(PostFxResources.UseSSAO && PostFxResources.SSAO_gen_ps && PostFxResources.SSAO_blend_ps && PostFxResources.pShadowBlurSurf1 && PostFxResources.pShadowBlurSurf2) {
                    //
                    //    pDevice->SetPixelShader(PostFxResources.SSAO_gen_ps);
                    //    vec4[1] = PostFxResources.AoDistance;
                    //
                    //    //SetTextureBoth(pDevice, 2, 0);
                    //    pDevice->SetRenderTarget(0, PostFxResources.pShadowBlurSurf1);
                    //    //SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                    //    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    //    SetTextureBoth(pDevice, 3, 0);
                    //
                    //    pDevice->SetPixelShader(PostFxResources.DeferredShadowBlurCircle_ps);
                    //    pDevice->SetRenderTarget(0, PostFxResources.pShadowBlurSurf2);
                    //    SetTextureBoth(pDevice, 11, PostFxResources.pShadowBlurTex1->mD3DTexture);
                    //    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    //
                    //    SetTextureBoth(pDevice, 11, 0);
                    //
                    //    pDevice->SetPixelShader(PostFxResources.SSAO_blend_ps);
                    //    pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                    //    SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                    //    SetTextureBoth(pDevice, 3, PostFxResources.pShadowBlurTex2->mD3DTexture);
                    //    pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                    //    PostFxResources.swapbuffers();
                    //    SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                    //    SetTextureBoth(pDevice, 3, PostFxResources.prePostFx[3]);
                    //    pDevice->SetPixelShader(pShader);
                    //}

                    // Temporal anti-aliasing resolves the HDR scene before everything else. Normally ResolveScene did
                    // it before the game computed bloom and exposure.
                    if (TemporalAA::GetMode() != TemporalAA::Mode::Off && !TemporalAA::IsSceneResolved() && !RenderScale::IsActive())
                    {
                        ProfilerScope timed(pDevice, kProfPostTAA);
                        auto source = FilterStippleBeforeResolve(PostFxResources.textureRead);
                        if (TemporalAA::Resolve(pDevice, source, PostFxResources.renderTargetTex, PostFxResources.renderTargetSurf))
                        {
                            PostFxResources.swapbuffers();
                            pDevice->SetPixelShader(pShader);
                        }
                        else if (source != PostFxResources.textureRead)
                            TemporalAA::KeepStipple(PostFxResources.textureRead);
                    }

                    if (PostFxResources.useStippleFilter && PostFxResources.stipple_filter_ps && !TemporalAA::IsStippleFiltered())
                    {
                        ProfilerScope timed(pDevice, kProfPostStipple);
                        pDevice->SetPixelShader(PostFxResources.stipple_filter_ps);
                        pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                        SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                        pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                        PostFxResources.swapbuffers();
                        pDevice->SetPixelShader(pShader);
                    }

                    static auto dof = FusionFixSettings.GetRef("PREF_TCYC_DOF");
                    if (dof->get() > FusionFixSettings.DofText.eCutscenesOnly || (dof->get() == FusionFixSettings.DofText.eCutscenesOnly && CCutsceneManager::IsRunning()) || shouldModifyMapMenuBackground())
                    {
                        if (PostFxResources.dof_blur_ps && PostFxResources.depth_of_field_tent_ps && PostFxResources.dof_coc_ps)
                        {
                            if (PostFxResources.FullScreenDownsampleSurf && PostFxResources.FullScreenDownsampleSurf2)
                            {
                                ProfilerScope timed(pDevice, kProfPostDOF);
                                pDevice->SetSamplerState(8, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

                                pDevice->SetPixelShader(PostFxResources.dof_blur_ps);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf);
                                SetTextureBoth(pDevice, 8, PostFxResources.HalfScreenTex);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                pDevice->SetPixelShader(PostFxResources.depth_of_field_tent_ps);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf2);
                                SetTextureBoth(pDevice, 8, PostFxResources.FullScreenDownsampleTex->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                pDevice->SetPixelShader(PostFxResources.dof_coc_ps);
                                pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                                if (PostFxResources.bEnablePreAlphaDepth)
                                    SetTextureBoth(pDevice, 1, PostDepth());
                                SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                                SetTextureBoth(pDevice, 8, PostFxResources.FullScreenDownsampleTex2->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                                PostFxResources.swapbuffers();

                                pDevice->SetPixelShader(pShader);
                            }
                        }
                    }

                    static auto refSunShafts = FusionFixSettings.GetRef("PREF_SUNSHAFTS");
                    if (refSunShafts->get())
                    {
                        // 2 passes at half res
                        if (PostFxResources.SSPrepass_PS && PostFxResources.SSDraw_PS)
                        {
                            if (PostFxResources.FullScreenDownsampleSurf && PostFxResources.FullScreenDownsampleSurf2)
                            {
                                ProfilerScope timed(pDevice, kProfPostSunShafts);
                                pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(8, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(8, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(11, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(11, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(11, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(13, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                                pDevice->SetSamplerState(13, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                                pDevice->SetSamplerState(13, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

                                // crop around sun position
                                pDevice->SetPixelShader(PostFxResources.SSPrepass_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf);
                                if (PostFxResources.bEnablePreAlphaDepth)
                                    SetTextureBoth(pDevice, 1, PostDepth());
                                SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                                SetTextureBoth(pDevice, 13, PostFxResources.DiffuseTex);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                // sample sunshafts from a cropped texture
                                pDevice->SetPixelShader(PostFxResources.SSDraw_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf2);
                                SetTextureBoth(pDevice, 11, PostFxResources.FullScreenDownsampleTex->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                // second sunshafts pass
                                pDevice->SetPixelShader(PostFxResources.SSDraw_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.FullScreenDownsampleSurf);
                                SetTextureBoth(pDevice, 11, PostFxResources.FullScreenDownsampleTex2->mD3DTexture);
                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                                // add sunshafts to screen, with the sun flare placed by the view projection without jitter
                                {
                                    // c207: the sun's place on the screen (xy) and 1 (z) when the camera is known, else the
                                    // shader projects the sun with the game's matrix, jitter and all
                                    float flare[4] = {};
                                    if (TemporalAA::CurrentCamera.Valid)
                                    {
                                        auto& m = TemporalAA::CurrentCamera.ViewProjectionNoJitter.m;
                                        float clip[4];
                                        for (int j = 0; j < 4; ++j)
                                            clip[j] = static_cast<float>(SunDirectionWorld[0] * m[0][j] + SunDirectionWorld[1] * m[1][j] + SunDirectionWorld[2] * m[2][j]);
                                        if (clip[3] > 1e-6f)
                                        {
                                            flare[0] = clip[0] / clip[3] * 0.5f + 0.5f;
                                            flare[1] = -clip[1] / clip[3] * 0.5f + 0.5f;
                                            flare[2] = 1.0f;
                                        }
                                        else
                                            flare[2] = -1.0f; // behind the camera
                                    }
                                    pDevice->SetPixelShaderConstantF(207, flare, 1);
                                }
                                pDevice->SetPixelShader(PostFxResources.SSAdd_PS);
                                pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                                SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                                SetTextureBoth(pDevice, 11, PostFxResources.FullScreenDownsampleTex->mD3DTexture);

                                pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                                PostFxResources.swapbuffers();

                                pDevice->SetPixelShader(pShader);
                            }
                        }
                    }

                    // The sharpening reads the frame from FullScreenTex_temp2: without anti-aliasing of
                    // its own the game's post processing draws it there
                    // With frame generation the frames are sharpened as they are shown instead (SharpenShownFrame)
                    const bool sharpenNow = IsSharpeningActive() && !(PostFxResources.CASMasked_PS && FrameGeneration::DefersSharpening());
                    const bool sharpenFromTemp2 = !IsPostFxAA() && sharpenNow;

                    // game postfx
                    {
                        ProfilerScope timed(pDevice, kProfPostGame);
                        for (int i = 0; i < 4; i++)
                        {
                            SetTextureBoth(pDevice, i, PostFxResources.prePostFx[i]);
                            pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
                        }

                        if (IsPostFxAA() || sharpenFromTemp2)
                            pDevice->SetRenderTarget(0, PostFxResources.FullScreenSurface_temp2);
                        else
                            pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

                        if (PostFxResources.bEnablePreAlphaDepth)
                            SetTextureBoth(pDevice, 1, PostDepth());
                        SetTextureBoth(pDevice, 2, PostFxResources.textureRead);
                        pDevice->Clear(0, 0, D3DCLEAR_TARGET, 0, 0, 0);

                        pDevice->SetPixelShader(pShader);
                        pDevice->SetVertexShader(vShader);
                        //hr = pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                        hbDrawPrimitivePostFX.fun();
                        // if(UsePostFxAA->get() > FusionFixSettings.AntialiasingText.eMO_OFF)
                        //     PostFxResources.swapbuffers();
                    }

                    // Anti aliasing
                    if (IsPostFxAA())
                    {
                        ProfilerScope timed(pDevice, kProfPostAA);
                        // FXAA
                        if ((UsePostFxAA->get() == FusionFixSettings.AntialiasingText.eFXAA) && PostFxResources.FxaaPS)
                        {
                            pDevice->SetPixelShader(PostFxResources.FxaaPS);

                            // pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                            pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

                            SetTextureBoth(pDevice, 2, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            // SetTextureBoth(pDevice, 2, PostFxResources.textureRead);

                            hr = pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                            pDevice->SetPixelShader(pShader);
                        }

                        // SMAA
                        if (UsePostFxAA->get() == FusionFixSettings.AntialiasingText.eSMAA &&
                           PostFxResources.SMAA_EdgeDetection && PostFxResources.SMAA_BlendingWeightsCalculation && PostFxResources.SMAA_NeighborhoodBlending &&
                           PostFxResources.SMAA_EdgeDetectionVS && PostFxResources.SMAA_BlendingWeightsCalculationVS && PostFxResources.SMAA_NeighborhoodBlendingVS &&
                           PostFxResources.SMAA_areaTex && PostFxResources.SMAA_searchTex && PostFxResources.edgesTex && PostFxResources.blendTex
                           )
                        {
                            DWORD oldSample = 0;

                            for (int i = 0; i <= 4; ++i)
                            {
                                pDevice->GetSamplerState(i, D3DSAMP_MINFILTER, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_MAGFILTER, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_MIPFILTER, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_ADDRESSU, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_ADDRESSV, &oldSample);
                                pDevice->GetSamplerState(i, D3DSAMP_ADDRESSW, &oldSample);
                            }

                            // colorTex / colorGammaTex
                            pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
                            pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);

                            // edgesTex
                            pDevice->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(1, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);

                            // blendTex
                            pDevice->SetSamplerState(4, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(4, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(4, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(4, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(4, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(4, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);

                            // areaTex
                            pDevice->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                            pDevice->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(2, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);

                            // searchTex
                            pDevice->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                            pDevice->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                            pDevice->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
                            pDevice->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                            pDevice->SetSamplerState(3, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);

                            // SMAA_EdgeDetection
                            pDevice->SetPixelShader(PostFxResources.SMAA_EdgeDetection);
                            pDevice->SetVertexShader(PostFxResources.SMAA_EdgeDetectionVS);
                            pDevice->SetRenderTarget(0, PostFxResources.edgesSurf);
                            SetTextureBoth(pDevice, 0, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            pDevice->Clear(0, 0, D3DCLEAR_TARGET, 0, 0, 0);
                            pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                            // SMAA_BlendingWeightsCalculation
                            pDevice->SetPixelShader(PostFxResources.SMAA_BlendingWeightsCalculation);
                            pDevice->SetVertexShader(PostFxResources.SMAA_BlendingWeightsCalculationVS);
                            pDevice->SetRenderTarget(0, PostFxResources.blendSurf);
                            SetTextureBoth(pDevice, 1, PostFxResources.edgesTex->mD3DTexture);
                            SetTextureBoth(pDevice, 2, PostFxResources.SMAA_areaTex);
                            SetTextureBoth(pDevice, 3, PostFxResources.SMAA_searchTex);
                            pDevice->Clear(0, 0, D3DCLEAR_TARGET, 0, 0, 0);
                            pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                            // SMAA_NeighborhoodBlending
                            pDevice->SetPixelShader(PostFxResources.SMAA_NeighborhoodBlending);
                            pDevice->SetVertexShader(PostFxResources.SMAA_NeighborhoodBlendingVS);

                            // pDevice->SetRenderTarget(0, PostFxResources.renderTargetSurf);
                            pDevice->SetRenderTarget(0, PostFxResources.backBuffer);

                            SetTextureBoth(pDevice, 0, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            SetTextureBoth(pDevice, 4, PostFxResources.blendTex->mD3DTexture);

                            pDevice->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &oldSample);
                            pDevice->GetRenderState(D3DRS_SRGBWRITEENABLE, &OldSRGB); // save srgb state
                            pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 1);
                            pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, 1);

                            pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

                            pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, oldSample);
                            pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, OldSRGB); // restore srgb state

                            for (int i = 0; i <= 4; ++i)
                            {
                                pDevice->SetSamplerState(i, D3DSAMP_MINFILTER, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_MIPFILTER, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_ADDRESSU, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_ADDRESSV, oldSample);
                                pDevice->SetSamplerState(i, D3DSAMP_ADDRESSW, oldSample);
                            }

                            SetTextureBoth(pDevice, 0, PostFxResources.prePostFx[0]);
                            SetTextureBoth(pDevice, 1, PostFxResources.prePostFx[1]);
                            SetTextureBoth(pDevice, 2, PostFxResources.FullScreenTex_temp2->mD3DTexture);
                            SetTextureBoth(pDevice, 3, PostFxResources.prePostFx[3]);
                            SetTextureBoth(pDevice, 4, PostFxResources.prePostFx[4]);
                            pDevice->SetPixelShader(pShader);
                            pDevice->SetVertexShader(vShader);
                        }
                    }

                    if (sharpenNow)
                        ApplySharpening(pDevice, pShader, vShader, sharpenFromTemp2);
                    FrameGeneration::CaptureHudLess(pDevice, PostFxResources.backBuffer);

                    for (int i = 0; i < PostfxTextureCount; i++)
                    {
                        SetTextureBoth(pDevice, i, PostFxResources.prePostFx[i]);
                        pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
                        SAFE_RELEASE(PostFxResources.prePostFx[i]);
                    }
                    return S_OK;
                }

                for (int i = 0; i < PostfxTextureCount; i++)
                {
                    SetTextureBoth(pDevice, i, PostFxResources.prePostFx[i]);
                    pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
                    SAFE_RELEASE(PostFxResources.prePostFx[i]);
                }
                return S_FALSE;
            }
        }

        for (int i = 0; i < PostfxTextureCount; i++)
        {
            SetTextureBoth(pDevice, i, PostFxResources.prePostFx[i]);
            pDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, PostFxResources.Samplers[i]);
            SAFE_RELEASE(PostFxResources.prePostFx[i]);
        }
        return S_FALSE;
    }

    // Which part of the game's lighting is being drawn, for the profiler: 0 none, 1 the sun and
    // ambient, 2 the lamps, 3 the light shafts. It only moves on, so a part the game skips leaves
    // the one before running, and 0 closes whatever is open.
    static inline int nLightingStage = 0;
    static void SetLightingStage(IDirect3DDevice9* pDevice, int stage)
    {
        static constexpr int kSections[] = { -1, kProfLightSun, kProfLightLocal, kProfLightShafts };
        if (stage != 0 && stage <= nLightingStage)
            return;
        if (nLightingStage > 0)
            ProfilerMark(pDevice, kSections[nLightingStage], false);
        nLightingStage = stage;
        if (stage > 0)
            ProfilerMark(pDevice, kSections[stage], true);
    }

    static inline SafetyHookInline shWaterRender{};
    static void __cdecl WaterRenderHook(int a1)
    {
        auto& R = PostFxResources;
        SetWaterRainRings();
        auto pProfileDevice = rage::grcDevice::GetD3DDevice();
        ProfilerScope timedWater(pProfileDevice, kProfWaterAll);
        // The game renders water for other views too, into targets of other sizes: each call
        // took two full screen copies and a full screen pass, and a target of another size
        // had both mask copies released and created again, even with no water on screen.
        // Only the main scene's water gets reflections, once a frame.
        const bool mainScene = !bDeviceLost && R.SSREnabled() && R.SSREffect && R.fSSRWaterIntensity > 0.0f &&
                               !R.bWaterDoneThisFrame && RenderTargetIsScreenSized();
        R.bWaterMaskCaptured = mainScene && CopyRenderTargetToWaterMask(0);
        ProfilerMark(pProfileDevice, kProfWaterGame, true);
        shWaterRender.unsafe_ccall<void>(a1);
        ProfilerMark(pProfileDevice, kProfWaterGame, false);
        if (mainScene)
        {
            auto pDevice = rage::grcDevice::GetD3DDevice();
            ProfilerMark(pDevice, kProfWater, true);
            RenderWaterReflections();
            ProfilerMark(pDevice, kProfWater, false);
            R.bWaterDoneThisFrame = true;
        }
        R.bWaterMaskCaptured = false;
    }

    static inline thread_local bool bInsteadDrawPrimitiveFog = false;
    static inline injector::hook_back<void(__fastcall*)(void*, void*, int, int, int)> hbDrawCallFog;
    static void __fastcall DrawCallFog(void* _this, void* edx, int a2, int a3, int a4)
    {
        bInsteadDrawPrimitiveFog = true;
        hbDrawCallFog.fun(_this, edx, a2, a3, a4);
        bInsteadDrawPrimitiveFog = false;
    }

    static inline thread_local bool bInsteadDrawPrimitivePostFX = false;
    static inline injector::hook_back<void(__fastcall*)(void*, void*, int, int, int)> hbDrawCallPostFX;
    static void __fastcall DrawCallPostFX(void* _this, void* edx, int a2, int a3, int a4)
    {
        UnbindGlassReflections();
        if (auto pDevice = rage::grcDevice::GetD3DDevice(); pDevice && !bDeviceLost)
            SetGlassInsideTint(pDevice, false);
        bInsteadDrawPrimitivePostFX = true;
        hbDrawCallPostFX.fun(_this, edx, a2, a3, a4);
        bInsteadDrawPrimitivePostFX = false;
        if (!bDeviceLost)
            DrawSSRDebugOverlay();
        SSRTrace::Tick();
        TickIniReload();
    }

    // Ctrl+Shift+F10 (IniReload): the live settings (ReadLiveIni) read again, and the cloud values the shadows use
    // added to GTAIV.EFLC.FusionFix.PostFx.log next to the plugin.
    static void OnIniReload(CIniReader& iniReader)
    {
        {
            auto& R = PostFxResources;
            const bool profiling = PostFxProfiler::bEnabled;
            R.ReadLiveIni(iniReader);
            // A mark between the profiler's blocks, the next of which holds only frames with what was read.
            if (profiling && PostFxProfiler::bEnabled)
            {
                ResetProfilerSums();
                FusionLog::Block log("PostFx", "Profiler");
                log.Printf("Ctrl+Shift+F10: the ini read again, the next block starts afresh\n");
            }
            // The cloud values the shadows were cast with, to tune CloudShadowsCoverage by.
            if (FusionLog::Block log("PostFx", "CloudShadows"); true)
            {
                log.Printf("threshold %.4f  bias %.4f  thickness %.4f  (%s)  coverage %+.3f  strength %.2f\n", R.fCloudLastThreshold,
                        R.fCloudLastBias, R.fCloudLastThickness, R.bCloudLastFromGame ? "the game's" : "fallback, the sky not drawn yet",
                        R.fCloudShadowsCoverage, R.fCloudShadows);
                const auto& top = rage::grmShaderInfo::getShaderParamData(R.CloudColorIdx);
                const auto& sunset = rage::grmShaderInfo::getShaderParamData(R.SunsetColorIdx);
                const auto& sunDir = rage::grmShaderInfo::getShaderParamData(R.CloudSunDirectionIdx);
                const auto& sky = rage::grmShaderInfo::getShaderParamData(R.CloudSkyColorIdx);
                const auto& moon = rage::grmShaderInfo::getShaderParamData(R.CloudMoonPositionIdx);
                const float* k = R.CloudShadowConsts;
                log.Component("Clouds");
                log.Printf("clouds lit from %.3f %.3f %.3f; the sky's sun %.3f %.3f %.3f; the game's light %.3f %.3f %.3f (frame %u, now %u); sky axis signs %+.0f %+.0f\n",
                        R.CloudLastUsedSun[0], R.CloudLastUsedSun[1], R.CloudLastUsedSun[2], R.CloudLastSkySun[0], R.CloudLastSkySun[1],
                        R.CloudLastSkySun[2], R.CloudLightDir[0], R.CloudLightDir[1], R.CloudLightDir[2], R.nCloudLightFrame, FrameHistory::Frame(),
                        R.CloudSkyAxisSign[0], R.CloudSkyAxisSign[1]);
                log.Printf("clouds: density %.4f, absorption %.2f x %.2f, translucency %.2f, detail %.2f, shade %.2f, sky match %.2f x %.2f\n",
                        R.fVolumetricCloudsDensity * R.Cloud.density, R.fVolumetricCloudsAbsorption, R.Cloud.absorption,
                        R.fVolumetricCloudsTranslucency * R.Cloud.translucency, R.fVolumetricCloudsDetail * R.Cloud.detail,
                        R.fVolumetricCloudsShade, R.fVolumetricCloudsSkyMatch, R.Cloud.skyMatch);
                log.Printf("clouds drawn with lit %.2f %.2f %.2f, shade %.2f %.2f %.2f, ceiling %.2f; lit by the %s at %.2f; sky clamp %.2f %.2f %.2f; volumetric fog %d; sky HDR %d; sky match %.2f; debug %d\n",
                        R.CloudLastLit[0], R.CloudLastLit[1], R.CloudLastLit[2], R.CloudLastShade[0], R.CloudLastShade[1], R.CloudLastShade[2],
                        R.CloudLastCeiling, R.bCloudLastMoonlit ? "moon" : "sun", R.CloudLastLightStrength, R.CloudLastClamp[0], R.CloudLastClamp[1], R.CloudLastClamp[2],
                        [] { static auto fog = FusionFixSettings.GetRef("PREF_VOLUMETRICFOG"); return fog ? fog->get() : -1; }(), int(bSkyHDR), R.fVolumetricCloudsSkyMatch, R.VolumetricCloudsDebug());
                log.Component("CloudShadows");
                log.Printf("shadow constants: c197 %.3f %.3f %.1f %.6f  c198 %.3f %.3f %.3f %.3f  c199 %.3f %.3f  noise %s, %s after the lights  debug %d\n",
                        k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9], R.CloudNoiseTexture ? "made" : "missing",
                        R.bCloudNoiseSurvived ? "still bound" : "gone", R.CloudShadowsDebug());
                log.Printf("s12 before the shadows: srgb %lu  max mip %lu  min filter %lu  lod bias %.3f\n",
                        static_cast<unsigned long>(R.CloudSamplerBefore[0]), static_cast<unsigned long>(R.CloudSamplerBefore[1]),
                        static_cast<unsigned long>(R.CloudSamplerBefore[2]), std::bit_cast<float>(R.CloudSamplerBefore[3]));
                log.Component("GGX");
                log.Printf("GGX lights: %u headlights among %u lights drawn since the last log\n", R.nLightGGXHeadlights, R.nLightGGXLights);
                R.nLightGGXHeadlights = 0;
                R.nLightGGXLights = 0;
                log.Printf("headlight glints: %s; effect %s (hr 0x%08lX); %u headlights drawn; their intensity %.3f to %.3f, outer cone %.3f to %.3f\n",
                        R.szGlintsStatus, R.HeadlightGlintsEffect ? "built" : "missing", static_cast<unsigned long>(R.hrHeadlightGlintsEffect),
                        R.nGlintsLastLights, R.GlintsLastIntensity[0], R.GlintsLastIntensity[1], R.GlintsLastCone[0], R.GlintsLastCone[1]);
                log.Component("VehicleBoxShadows");
                log.Printf("vehicle box shadows: %s, hook %s; since the last log %u frames drawn with their boxes, %u without, %u lights shaded with boxes; light size %.2f, rounding %.2f, from car lights %d, with slots %d, size %.2f %.2f %.2f of the bounds, follow light %.2f, low light narrow %.2f\n",
                        R.VehicleBoxShadowsEnabled() ? "on" : "off", VehicleBoxShadows::lightListBuilt ? "installed" : "missing",
                        VehicleBoxShadows::framesMatched.exchange(0), VehicleBoxShadows::framesUnmatched.exchange(0), VehicleBoxShadows::lightsWithBoxes.exchange(0),
                        R.fVehicleBoxShadowLightSize, R.fVehicleBoxShadowRounding, int(R.bVehicleBoxShadowsFromCarLights), int(R.bVehicleBoxShadowsWithSlots),
                        R.fVehicleBoxShadowScale[0], R.fVehicleBoxShadowScale[1], R.fVehicleBoxShadowScale[2], R.fVehicleBoxShadowFollowLight,
                        R.fVehicleBoxShadowLowLightNarrow);
                log.Printf("ped capsule shadows: %s, %u people shaded by lights since the last log; radius %.2f, height %.2f, foot %.2f below the position\n",
                        R.bPedCapsuleShadows ? "on" : "off", VehicleBoxShadows::pedsWithShadows.exchange(0),
                        R.fPedCapsuleRadius, R.fPedCapsuleHeight, R.fPedCapsuleFootBelow);
                log.Printf("slot maps drawn without their lamp's boxed cars %u, cars left out %u\n",
                        VehicleBoxShadows::mapsWithoutBoxed.exchange(0), VehicleBoxShadows::carsLeftOut.exchange(0));
                {
                    const auto& l = VehicleBoxShadows::lastLowLight;
                    log.Printf("%u times a lamp no higher than a car's roof was left without its box; the last at %.1f %.1f %.1f, %.2f m above the box's bottom: type %d, flags 0x%X, radius %.1f, intensity %.2f, shadow key 0x%08X, cache %d\n",
                            VehicleBoxShadows::lowLights.exchange(0), l.mPosition.x, l.mPosition.y, l.mPosition.z, VehicleBoxShadows::lastLowHeight,
                            int(l.mType), l.mFlags, l.mRadius, l.mIntensity, uint32_t(l.mCastShadows), l.mShadowCacheIndex);
                }
                {
                    const auto& b = VehicleBoxShadows::nearestBounds;
                    const auto& x = VehicleBoxShadows::nearestBox;
                    log.Printf("nearest car %.1f m away: model bounds %.2f %.2f %.2f to %.2f %.2f %.2f (x right, y forward, z up); box half length %.2f, width %.2f, height %.2f, middle %.2f above its position\n",
                            VehicleBoxShadows::nearestDistance, b[0], b[1], b[2], b[3], b[4], b[5], x[0], x[1], x[2], x[3]);
                }
                log.Component("CloudReflections");
                log.Printf("clouds in reflections: %s; %u reflection map and %u water reflection skies since the last log; viewport %lu,%lu %lux%lu of a %ux%u target\n",
                        R.szCloudsReflectionStatus, R.nCloudReflectionCalls, R.nCloudWaterReflectionCalls, R.CloudReflectionViewport.X, R.CloudReflectionViewport.Y,
                        R.CloudReflectionViewport.Width, R.CloudReflectionViewport.Height, R.CloudReflectionTarget[0], R.CloudReflectionTarget[1]);
                R.nCloudReflectionCalls = 0;
                R.nCloudWaterReflectionCalls = 0;
                log.Component("WaterRings");
                log.Printf("water rings: c178 %.2f %.1f %.3f %.3f; %u water draws since the last log; SSR on the water %s\n",
                        R.WaterRingsLast[0], R.WaterRingsLast[1], R.WaterRingsLast[2], R.WaterRingsLast[3], R.nWaterRingDraws,
                        R.nWaterSsrSurface < 0 ? "not drawn" : R.nWaterSsrSurface ? "had the wave texture" : "had no wave texture (flat, no rings)");
                R.nWaterRingDraws = 0;
                log.Component("WetGround");
                log.Printf("wet ground: %s; effect %s (hr 0x%08lX); wetness %.3f, rain %.3f, materials 0x%02X, debug %d\n",
                        R.szWetGroundStatus, R.WetGroundEffect ? "built" : "missing", static_cast<unsigned long>(R.hrWetGroundEffect), R.fWetness,
                        CWeather::Rain ? *CWeather::Rain : -1.0f, unsigned(R.nWetGroundMaterials), R.WetGroundDebug());
                log.Component("Clouds");
                log.Printf("volumetric clouds: %s; effect %s (hr 0x%08lX); shadows follow them %d; HDRExposure %.3f; CloudColor %.3f %.3f %.3f; "
                             "SunsetColor %.3f %.3f %.3f; CloudInscatteringRange %.3f; SunDirection %.3f %.3f %.3f; SkyColor %.3f %.3f %.3f; MoonPosition %.3f %.3f %.3f\n",
                        R.szCloudsStatus, R.CloudsEffect ? "built" : "missing", static_cast<unsigned long>(R.hrCloudsEffect), int(R.VolumetricCloudsOn()),
                        rage::grmShaderInfo::getShaderParamData(R.CloudExposureIdx)[0], top[0], top[1], top[2], sunset[0], sunset[1], sunset[2],
                        rage::grmShaderInfo::getShaderParamData(R.CloudInscatteringIdx)[0], sunDir[0], sunDir[1], sunDir[2],
                        sky[0], sky[1], sky[2], moon[0], moon[1], moon[2]);
                log.Component("PbrSky");
                log.Printf("physical sky: %s; effect %s (hr 0x%08lX); share %.2f, sun height %.3f, haze %.2f; tables for haze %.2f, view drawn frame %u of %u; skies drawn %u\n",
                        R.szPbrSkyStatus, R.SkyEffect ? "built" : "missing", static_cast<unsigned long>(R.hrSkyEffect), R.fPbrSkyLastStrength,
                        R.fPbrSkyLastSunHeight, R.fPbrSkyLastHaze, R.fSkyTablesHaze, unsigned(R.nSkyViewFrame), unsigned(FrameHistory::Frame()),
                        unsigned(R.nPbrSkyDraws));
                log.Printf("physical sky in the clouds: share %.2f, table times %.2f on screen, sun hue %.2f %.2f %.2f\n",
                        R.CloudLastPbrShare, R.CloudLastSkyUnits, R.CloudLastSunHue[0], R.CloudLastSunHue[1], R.CloudLastSunHue[2]);
                log.Printf("physical sky in the fog: share %.2f, aerial perspective %.2f (%s), table times %.2f on screen, haze %.2f\n",
                        R.fAerialLastShare, R.fAerialLastStrength, R.AerialPerspectiveEnabled() ? (R.AerialPerspectiveDebug() ? "on, debug x25" : "on") : "off in the menu",
                        R.fAerialLastUnits, R.fAerialLastHaze);
                // The game's weather by name, and the cloud layer it gives, so a screenshot can be
                // matched to its weather.
                {
                    static const char* kWeatherNames[8] = { "EXTRASUNNY", "SUNNY", "SUNNY_WINDY", "CLOUDY", "RAIN", "DRIZZLE", "FOGGY", "LIGHTNING" };
                    auto name = [](const auto* type) {
                        const auto value = type ? static_cast<uint32_t>(*type) : 99u;
                        return value < 8 ? kWeatherNames[value] : "?";
                    };
                    const float k = CWeather::InterpolationValue ? *CWeather::InterpolationValue : 0.0f;
                    const auto& c = R.Cloud;
                    log.Printf("weather %s -> %s at %.2f; cloud layer: cover %.2f, base %.0f m, thickness %.0f m, sheet %.2f, wind %.2f\n",
                            name(CWeather::OldWeatherType), name(CWeather::NewWeatherType), k, c.coverage, c.base, c.thickness, c.stratus, c.wind);
                }
            }
        }
    }

    // Once a frame, from the post fx pass, which runs in the pause menu too: Ctrl+Shift+F10 reads the ini again
    // (IniReload), Ctrl+Shift+F9 moves the clouds somewhere else.
    static void TickIniReload()
    {
        IniReload::Poll();

        // Ctrl+Shift+F9: the clouds start somewhere else, as at a new game (a new place on the
        // map, a new phase of its morph and of the billows), to look at other skies without
        // restarting. The accumulation starts over, so the old clouds do not linger.
        static bool reseedWasDown = false;
        const bool reseed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
            (GetAsyncKeyState(VK_F9) & 0x8000);
        if (reseed && !reseedWasDown)
        {
            auto& R = PostFxResources;
            R.bCloudDriftSeeded = false;
            R.nCloudAccumFrame = 0;
            MessageBeep(MB_OK);
        }
        reseedWasDown = reseed;
    }

    // The first pass of the game's post processing downsamples the scene for bloom and exposure
    static inline thread_local bool bInsteadDrawPrimitiveDownsample = false;
    static inline injector::hook_back<void(__fastcall*)(void*, void*, int, int, int)> hbDrawCallDownsample;
    static void __fastcall DrawCallDownsample(void* _this, void* edx, int a2, int a3, int a4)
    {
        // Before the game binds FullScreenCopy and computes its texel size for the downsample
        UpscaleScene();
        bInsteadDrawPrimitiveDownsample = true;
        hbDrawCallDownsample.fun(_this, edx, a2, a3, a4);
        bInsteadDrawPrimitiveDownsample = false;
    }

    // Depth of field, sun shafts and the game's post processing read the depth after the resolve: averaged over the
    // jitter when temporal anti-aliasing has it, which keeps their edges still
    static IDirect3DTexture9* PostDepth()
    {
        if (auto steady = TemporalAA::GetSteadyDepth())
            return steady;
        return PostFxResources.PreAlphaDepthCopyRT->mD3DTexture;
    }

    // With temporal anti-aliasing, DLAA or FSR the stipple filter runs before them, on the scene at the render size,
    // instead of in the post processing after them. Returns what the resolve reads: the filtered copy, or the scene
    // when the filter did not run. A scene nothing resolved takes the copy with TemporalAA::KeepStipple.
    static IDirect3DTexture9* FilterStippleBeforeResolve(IDirect3DTexture9* scene)
    {
        if (TemporalAA::GetMode() == TemporalAA::Mode::Off || !PostFxResources.useStippleFilter || !PostFxResources.stipple_filter_ps)
            return scene;
        auto filtered = TemporalAA::FilterStipple(scene, PostFxResources.stipple_filter_ps);
        return filtered ? filtered : scene;
    }

    // Render scale: from here on FullScreenCopy is a texture of the screen size, with the scene upscaled by DLSS
    // or FSR, or stretched when neither runs
    static void UpscaleScene()
    {
        if (!RenderScale::IsActive() || !PostFxResources.FullScreenTex_temp1 || !PostFxResources.FullScreenTex_temp1->mD3DTexture)
            return;

        auto pDevice = rage::grcDevice::GetD3DDevice();
        ProfilerScope timed(pDevice, kProfResolve);
        IDirect3DTexture9* scene = nullptr;
        IDirect3DSurface9* sceneSurface = nullptr;
        IDirect3DSurface9* output = nullptr;
        if (!RenderScale::BeginPost(pDevice, scene, sceneSurface, output))
            return;
        // The scene at the render size is not read past here: the filtered copy stands in for it
        auto source = FilterStippleBeforeResolve(scene);

        // DLSS and FSR write straight into the full size texture
        IDirect3DTexture9* outputTexture = nullptr;
        output->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&outputTexture));

        auto mode = TemporalAA::GetMode();
        if (!outputTexture || (mode != TemporalAA::Mode::DLAA && mode != TemporalAA::Mode::FSR) ||
            !TemporalAA::Resolve(pDevice, source, outputTexture, output))
        {
            IDirect3DSurface9* sourceSurface = nullptr;
            if (SUCCEEDED(source->GetSurfaceLevel(0, &sourceSurface)) && sourceSurface)
                pDevice->StretchRect(sourceSurface, nullptr, output, nullptr, D3DTEXF_LINEAR);
            SAFE_RELEASE(sourceSurface);
        }
        SAFE_RELEASE(outputTexture);
    }

    // Temporal anti-aliasing resolves the scene before the game computes bloom from it, which would otherwise
    // follow the jitter. The result goes back into the scene copy that the game and PostFx3 read.
    static void ResolveScene()
    {
        // With the render scale UpscaleScene did it, on the scene at the render size
        if (TemporalAA::GetMode() == TemporalAA::Mode::Off || TemporalAA::IsSceneResolved() || RenderScale::IsActive())
            return;
        if (!PostFxResources.mFullScreenRT || !PostFxResources.mFullScreenRT->mD3DTexture ||
            !PostFxResources.FullScreenTex_temp1 || !PostFxResources.FullScreenTex_temp1->mD3DTexture)
            return;

        auto pDevice = rage::grcDevice::GetD3DDevice();
        ProfilerScope timed(pDevice, kProfResolve);
        auto scene = PostFxResources.mFullScreenRT->mD3DTexture;
        auto resolved = PostFxResources.FullScreenTex_temp1->mD3DTexture;
        IDirect3DSurface9* sceneSurface = nullptr;
        IDirect3DSurface9* resolvedSurface = nullptr;
        scene->GetSurfaceLevel(0, &sceneSurface);
        resolved->GetSurfaceLevel(0, &resolvedSurface);
        auto source = FilterStippleBeforeResolve(scene);
        // Straight into the scene when the resolve reads the stipple filter's copy, or when DLAA or FSR, which take
        // the scene in before they write, resolve it; temporal AA reading the scene itself writes into the copy
        auto mode = TemporalAA::GetMode();
        bool upscaler = mode == TemporalAA::Mode::DLAA || mode == TemporalAA::Mode::FSR;
        bool direct = sceneSurface && (source != scene || upscaler) && TemporalAA::Resolve(pDevice, source, scene, sceneSurface);
        if (!direct && source != scene)
            TemporalAA::KeepStipple(scene);
        else if (!direct && sceneSurface && resolvedSurface && TemporalAA::Resolve(pDevice, scene, resolved, resolvedSurface))
            pDevice->StretchRect(resolvedSurface, nullptr, sceneSurface, nullptr, D3DTEXF_POINT);
        SAFE_RELEASE(resolvedSurface);
        SAFE_RELEASE(sceneSurface);
    }

    static inline injector::hook_back<void(__stdcall*)()> hbDrawPrimitivePostFX;
    static void __stdcall DrawPrimitivePostFX()
    {
        if (bInsteadDrawPrimitiveFog)
        {
            bInsteadDrawPrimitiveFog = false;
            // Do not initialize shaders, RTs etc. here or we get a device reset error for some reason
            if (!bDeviceLost)
            {
                NewFog();
                // Transparent geometry and visual effects come next: the reactive mask of temporal AA starts here
                TemporalAA::OnFogDrawn();
            }
        }
        else if (bInsteadDrawPrimitivePostFX)
        {
            bInsteadDrawPrimitivePostFX = false;
            if (!bDeviceLost)
            {
                Init();
                NewPostFX();
            }
        }
        else if (bInsteadDrawPrimitiveDownsample)
        {
            bInsteadDrawPrimitiveDownsample = false;
            ResolveScene();
            hbDrawPrimitivePostFX.fun();
        }
        else
        {
            hbDrawPrimitivePostFX.fun();
        }
    }

    // Whether the bound target is the water's reflection (WATER_REFLECTION_COLOUR). Its sky is drawn
    // through the sky's main branch, not the reflection map's (REFLECTION_MAP_COLOUR, the paraboloids
    // the surfaces' sky reflection comes from, phases with flag 0x40000).
    static bool IsWaterReflectionTarget(IDirect3DDevice9* pDevice)
    {
        auto rt = rage::grcTextureFactoryPC::GetRTByName("WATER_REFLECTION_COLOUR");
        if (!pDevice || !rt || !rt->mD3DTexture)
            return false;
        IDirect3DSurface9* water = nullptr;
        IDirect3DSurface9* bound = nullptr;
        rt->mD3DTexture->GetSurfaceLevel(0, &water);
        pDevice->GetRenderTarget(0, &bound);
        const bool same = water && water == bound;
        SAFE_RELEASE(water);
        SAFE_RELEASE(bound);
        return same;
    }

    static inline injector::hook_back<int(__fastcall*)(int, void*, int, int, char, char, int, char)> hbDrawSkyHook;
    // The sky's main branch, which also draws the water reflection's sky: the volumetric clouds go in
    // right after it there, as into the reflection map (DrawSkyReflection).
    static int __fastcall DrawSky(int _this, void* edx, int a2, int a3, char a4, char a5, int a6, char a7)
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!bDeviceLost)
            PbrSkyBeforeDraw(pDevice);
        const int result = DrawSkyMain(_this, edx, a2, a3, a4, a5, a6, a7);
        PbrSkyAfterDraw(pDevice);
        if (pDevice && !bDeviceLost && IsWaterReflectionTarget(pDevice))
        {
            ++PostFxResources.nCloudWaterReflectionCalls;
            RenderVolumetricClouds(pDevice, nullptr, true, kProfCloudReflectionWater);
        }
        return result;
    }

    // The scene's sky also writes the clouds' mask for the sun shafts (gta_atmoscatt_clouds, oC1) into
    // _DEFERRED_GBUFFER_0_, bound as the second target. The scene is 64 bits a pixel, the G-buffer 32:
    // a device that takes targets of different bit depths together, blending, draws the sky once, into both;
    // any other draws it twice, with the G-buffer for the mask, then without for the scene, as before.
    // The water's reflection, another size than the G-buffer, draws its sky once without it.
    static int DrawSkyMain(int _this, void* edx, int a2, int a3, char a4, char a5, int a6, char a7)
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!PostFxResources.DiffuseTex)
            return hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);

        static const bool independentBitDepths = [&]()
        {
            D3DCAPS9 caps = {};
            // and blends with both bound, as the sky's layers may
            constexpr DWORD needed = D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING;
            return SUCCEEDED(pDevice->GetDeviceCaps(&caps)) && (caps.PrimitiveMiscCaps & needed) == needed;
        }();

        IDirect3DSurface9* DiffuseSurf = nullptr;
        IDirect3DSurface9* target = nullptr;
        PostFxResources.DiffuseTex->GetSurfaceLevel(0, &DiffuseSurf);
        pDevice->GetRenderTarget(0, &target);
        D3DSURFACE_DESC diffuseDesc = {}, targetDesc = {};
        const bool scene = DiffuseSurf && target && SUCCEEDED(DiffuseSurf->GetDesc(&diffuseDesc)) && SUCCEEDED(target->GetDesc(&targetDesc)) &&
                           diffuseDesc.Width == targetDesc.Width && diffuseDesc.Height == targetDesc.Height;
        SAFE_RELEASE(target);
        if (!scene)
        {
            SAFE_RELEASE(DiffuseSurf);
            return hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);
        }

        IDirect3DPixelShader9* pShader = nullptr;
        pDevice->GetPixelShader(&pShader);
        IDirect3DSurface9* oldRenderTarget1 = nullptr;
        pDevice->GetRenderTarget(1, &oldRenderTarget1);
        pDevice->SetRenderTarget(1, DiffuseSurf);
        int hr = hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);
        pDevice->SetRenderTarget(1, oldRenderTarget1);
        if (!independentBitDepths)
        {
            pDevice->SetPixelShader(pShader);
            hr = hbDrawSkyHook.fun(_this, edx, a2, a3, a4, a5, a6, a7);
        }
        SAFE_RELEASE(oldRenderTarget1);
        SAFE_RELEASE(DiffuseSurf);
        SAFE_RELEASE(pShader);
        return hr;
    }

    // The reflection map's sky (CE 0xdbc2ab, the sky draw's branch for render phases with flag
    // 0x40000): the volumetric clouds go in right after it, through the reflection's viewport.
    static inline injector::hook_back<int(__fastcall*)(int, void*, int, int, char, char, int, char)> hbDrawSkyReflection;
    static int __fastcall DrawSkyReflection(int _this, void* edx, int a2, int a3, char a4, char a5, int a6, char a7)
    {
        auto pSkyDevice = rage::grcDevice::GetD3DDevice();
        if (!bDeviceLost)
            PbrSkyBeforeDraw(pSkyDevice);
        const int result = hbDrawSkyReflection.fun(_this, edx, a2, a3, a4, a5, a6, a7);
        PbrSkyAfterDraw(pSkyDevice);
        ++PostFxResources.nCloudReflectionCalls;
        if (auto pDevice = rage::grcDevice::GetD3DDevice(); pDevice && !bDeviceLost)
            RenderVolumetricClouds(pDevice, nullptr, true);
        return result;
    }

    static inline SafetyHookInline RenderPedAndVehicleFakeShadowsInlineHook;
    static DWORD __cdecl RenderPedAndVehicleFakeShadows(DWORD a1)
    {
        DWORD result = RenderPedAndVehicleFakeShadowsInlineHook.unsafe_ccall<DWORD>(a1);
        if (bDeviceLost)
            return result;
        RefreshGBufferTargets();
        TraceLightingInputs(rage::grcDevice::GetD3DDevice());

        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (auto vp = rage::GetCurrentViewport())
        {
            const D3DMATRIX& proj = *(const D3DMATRIX*)vp->mProjectionMatrix;
            const float camera[4] = { fabsf(proj._11), fabsf(proj._22), vp->mNearClip, vp->mFarClip };
            memcpy(PostFxResources.SkinCamera, camera, sizeof(camera));
        }
        if (auto vp = rage::GetCurrentViewport())
            SSRTrace::Line("lighting phase: vp %p %dx%d", static_cast<void*>(vp), int(vp->mWidth), int(vp->mHeight));
        ProfilerMark(pDevice, kProfAO, true);
        RenderAmbientOcclusion();
        ProfilerMark(pDevice, kProfAO, false);
        ProfilerMark(pDevice, kProfSSR, true);
        RenderScreenSpaceReflections();
        ProfilerMark(pDevice, kProfSSR, false);
        ProfilerMark(pDevice, kProfContact, true);
        RenderContactShadows();
        ProfilerMark(pDevice, kProfContact, false);
        ProfilerMark(pDevice, kProfGI, true);
        RenderIndirectLight();
        ProfilerMark(pDevice, kProfGI, false);
        // deferred_lighting draws after this; BindSSRTexture bound last frame's results.
        if (pDevice)
        {
            BindLightingInputs(pDevice);
            SetLightingStage(pDevice, 1);
        }

        return result;
    }

    // The game's light shaft loop (CE 0xAC2A09) draws a shaft with the shadowed shaft technique
    // when a shadow map is found for its light. Headlights nearly always have one there, and a
    // shaft drawn with it shows nothing, so unless VolumetricLightHeadlightShadow is on, the draw
    // hook clears that choice for lights with the headlight flag and they take the plain one.
    static inline SafetyHookMid shShaftDraw{};
    // A headlight's cone is round and aimed level, so the upper half of its shaft rose metres
    // above the road. Before the loop draws a shaft (CE 0xAC4580) it copies the light's direction
    // and tangent to its stack; for headlights the aim hook turns those copies down by
    // VolumetricLightHeadlightPitch, about the level axis across the beam. The light itself, which
    // lights the road and casts the headlight's shadow, keeps its aim.
    static inline SafetyHookMid shShaftAim{};

    // Traffic lights and fires (0x200) get no shadow map, so their contact shadow stood alone: at
    // night, in the shadow of a street light, the light of a traffic light coloured the ground
    // right up to a car, and its contact shadow cut a black band along the car into that colour.
    // At the top of the loop that draws each of the frame's lights (CE 0xAC10B7, edi the light
    // + 0x28, its flags at edi + 0x20), contact shadows go off for those lights and back on after.
    static inline SafetyHookMid shLocalContactLight{};

    // c206 for the light about to be drawn: a headlight's (a spot light of 8 m or more with the
    // vehicle flag, as InstallShaftHooks tells them) the car's level right and half its lamps'
    // spacing, from which the shaders take its highlight as one from each lamp; other lights none.
    // Only the beam of both lamps is split: the game draws a car's two lamps as one light between
    // them with the "headlights" projected texture (CE 0xA3E070), and a lamp left alone by a broken
    // one as a light of its own at that lamp with none (0xA3DE90), whose highlight stays one.
    static bool IsTwinHeadlight(const rage::CLightSource& light)
    {
        return light.mProjTexHash != 0;
    }

    static void SetLightGGXShape(const rage::CLightSource& light)
    {
        auto& R = PostFxResources;
        float shape[4] = {};
        if (R.GGXLightingEnabled() && R.fLightsGGX > 0.0f && R.fLightsGGXHeadlights > 0.0f && light.mType == rage::LT_SPOT &&
            (light.mFlags & rage::LF_VEHICLE) && light.mRadius >= 8.0f)
        {
            // dir x up, level
            const float kx = light.mDirection.y, ky = -light.mDirection.x;
            const float len = std::sqrt(kx * kx + ky * ky);
            if (len > 1e-3f)
            {
                shape[0] = kx / len;
                shape[1] = ky / len;
                shape[3] = IsTwinHeadlight(light) ? R.fLightsGGXHeadlights : 0.0f;
                ++R.nLightGGXHeadlights;
            }
        }
        ++R.nLightGGXLights;
        if (std::memcmp(shape, R.LightGGXShape, sizeof(shape)) == 0)
            return;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        std::memcpy(R.LightGGXShape, shape, sizeof(shape));
        pDevice->SetPixelShaderConstantF(206, shape, 1);
    }

    // A headlight the light loop is about to draw, as SetLightGGXShape tells them, kept for its glints.
    static void CollectGlintLight(const rage::CLightSource& light)
    {
        auto& R = PostFxResources;
        if (R.nGlintCandidates >= R.kGlintCandidates || light.mType != rage::LT_SPOT || !(light.mFlags & rage::LF_VEHICLE) ||
            light.mRadius < 8.0f || light.mIntensity <= 0.0f)
            return;
        auto& g = R.GlintCandidates[R.nGlintCandidates++];
        g.position[0] = light.mPosition.x;
        g.position[1] = light.mPosition.y;
        g.position[2] = light.mPosition.z;
        g.direction[0] = light.mDirection.x;
        g.direction[1] = light.mDirection.y;
        g.direction[2] = light.mDirection.z;
        g.colour[0] = light.mColor.x * light.mIntensity;
        g.colour[1] = light.mColor.y * light.mIntensity;
        g.colour[2] = light.mColor.z * light.mIntensity;
        g.radius = light.mRadius;
        g.distance = 0.0f;
        g.twin = IsTwinHeadlight(light);
        // What the game gives headlights, for the log: the ranges of intensity and outer cone.
        if (R.nGlintCandidates == 1)
        {
            R.GlintsLastIntensity[0] = R.GlintsLastIntensity[1] = light.mIntensity;
            R.GlintsLastCone[0] = R.GlintsLastCone[1] = light.mOuterConeAngle;
        }
        R.GlintsLastIntensity[0] = (std::min)(R.GlintsLastIntensity[0], light.mIntensity);
        R.GlintsLastIntensity[1] = (std::max)(R.GlintsLastIntensity[1], light.mIntensity);
        R.GlintsLastCone[0] = (std::min)(R.GlintsLastCone[0], light.mOuterConeAngle);
        R.GlintsLastCone[1] = (std::max)(R.GlintsLastCone[1], light.mOuterConeAngle);
    }

    // Whether the game has a shadow map for this light this frame, read as its own lookup does (CE 0x925db0, which
    // deferred lighting calls for each light; called again here it would clear a cache entry's flag): off unless
    // [0x1036780]; buffer [0x1174794]; a cached map where (buffer * 16 + cache index) * 0x100 + 0x119d1d0 is set;
    // a dynamic one in the 7 keys at 0x119fc08 + buffer * 0x880, 0x110 apart. Without the layout, by the flags alone
    static inline uintptr_t ShadowLookupBase = 0;
    // Only a dynamic slot counts: a cached map holds the static world only.
    static bool HasShadowMap(const rage::CLightSource& light)
    {
        if (!(light.mFlags & (rage::LF_STATIC_SHADOW | rage::LF_DYNAMIC_SHADOW)))
            return false;
        if (!ShadowLookupBase)
            return true;
        const auto at = [](uintptr_t address) { return ShadowLookupBase + address - 0x400000; };
        if (!*reinterpret_cast<const uint8_t*>(at(0x1036780)))
            return false;
        const int buffer = *reinterpret_cast<const int*>(at(0x1174794));
        if (buffer < 0 || buffer > 1)
            return true;
        // A lamp of a pair on one post uses the other's map (PairedLampDistance)
        uint32_t keys[2] = { uint32_t(light.mCastShadows), VehicleBoxShadowsBridge::SharedMapKey ? VehicleBoxShadowsBridge::SharedMapKey(uint32_t(light.mCastShadows)) : 0u };
        if (light.mFlags & rage::LF_DYNAMIC_SHADOW)
            for (int i = 0; i < 7; i++)
                for (const auto key : keys)
                    if (key && *reinterpret_cast<const uint32_t*>(at(0x119FC08) + buffer * 0x880 + i * 0x110) == key)
                        return true;
        return false;
    }

    static void InstallLocalContactLightHook()
    {
        auto pattern = hook::pattern("83 C7 28 89 7C 24 1C 8B 47 1C 85 C0");
        if (pattern.empty())
            return;
        if (auto base = GameBase(); shadow_lookup_layout::Validate(base))
            ShadowLookupBase = base;
        shLocalContactLight = safetyhook::create_mid(pattern.get_first(7), [](SafetyHookContext& regs)
        {
            auto& R = PostFxResources;
            if (!R.bLocalContactPass)
                return;
            if (nLightingStage == 1)
                SetLightingStage(rage::grcDevice::GetD3DDevice(), 2);
            const auto& light = *reinterpret_cast<const rage::CLightSource*>(regs.edi - 0x28);
            SetLightGGXShape(light);
            CollectGlintLight(light);
            auto pDevice = rage::grcDevice::GetD3DDevice();
            if (!pDevice)
                return;
            // False where its map leaves the cars out for their boxes: the contact shadow then goes with the boxes.
            bool mapped = HasShadowMap(light);
            const float boxed = VehicleBoxShadows::SetForLight(pDevice, light, mapped);
            if (R.LocalContactShadowConsts[7] == 0.0f)
                return;
            // A contact shadow only belongs where a shadow shows the car: the light's own map in a dynamic slot, or
            // the boxes of the cars it reaches. Elsewhere (no slot, lit from its cache, lights casting no shadows) it
            // stood alone, a dark frame around a car with no shadow, and LocalContactShadowsWithoutShadowMap applies. In a
            // room scene, lights inside only (0x20 without 0x40) that cast no shadows at all keep theirs, as most in
            // buildings do. Tunnel lamps are flagged inside too, and there the frame ran along the car from back to front
            // under every lamp it passed.
            // Not while you drive: a tunnel counts as a room scene, its lamps are lights inside that cast no shadows,
            // and their contact shadows showed only as outlines round the cars, box shadows or not.
            const bool insideOnly = R.bInteriorScene && !R.bPlayerInCar && (light.mFlags & 0x60) == 0x20 &&
                !(light.mFlags & (rage::LF_STATIC_SHADOW | rage::LF_DYNAMIC_SHADOW));
            // With boxes it comes in and goes out with them: at full at once, its frame showed round the car before
            // the box's shadow under it had faded in, and stayed after it had faded out, a blink behind the car. Only
            // VehicleBoxShadowsContact of it: its sharp edge along the car's own outline did not meet the box's softer,
            // rounder shadow.
            // A light inside that boxes a car gives it up as the boxes come in too: tunnel lamps are such lights, and
            // a tunnel counts as a room scene.
            const float unshadowed = R.fLocalContactShadowUnshadowed;
            const float box = (std::min)(boxed, 1.0f);
            const float fromBoxes = box * R.fVehicleBoxShadowContact;
            const float intensity = (light.mFlags & 0x200) ? 0.0f
                : unshadowed >= 1.0f || mapped
                    ? R.fLocalContactShadowIntensity
                    : insideOnly
                        ? R.fLocalContactShadowIntensity * (1.0f - box * (1.0f - R.fVehicleBoxShadowContact))
                        : R.fLocalContactShadowIntensity * (unshadowed + (1.0f - unshadowed) * fromBoxes);
            const bool off = intensity <= 0.0f;
            if (!off && intensity != R.fLocalContactLightIntensity)
            {
                float consts[4] = { R.LocalContactShadowConsts[0], R.LocalContactShadowConsts[1], R.LocalContactShadowConsts[2], intensity };
                pDevice->SetPixelShaderConstantF(202, consts, 1);
                R.fLocalContactLightIntensity = intensity;
            }
            if (off == R.bLocalContactLightOff)
                return;
            R.bLocalContactLightOff = off;
            const float none[4] = {};
            pDevice->SetPixelShaderConstantF(203, off ? none : &R.LocalContactShadowConsts[4], 1);
        });
    }

    // The start of the game's light shaft loop (CE 0xac29cd, the shaft count read), for the profiler.
    static inline SafetyHookMid shShaftLoopStart{};

    static void InstallShaftLoopProfiler()
    {
        auto pattern = hook::pattern("8B 35 ? ? ? ? 89 74 24 38 E8 ? ? ? ? 50 E8 ? ? ? ? 83 C4 08 80 7D 08 00 74 0E FF 35 ? ? ? ? E8 ? ? ? ? 83 C4 04 85 F6 0F 8E");
        if (pattern.empty())
            return;
        shShaftLoopStart = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext&)
        {
            auto pDevice = rage::grcDevice::GetD3DDevice();
            if (nLightingStage > 0)
                SetLightingStage(pDevice, 3);
            // Every light of the main view is drawn by now, and its target is still the lit scene.
            auto& R = PostFxResources;
            if (R.bLocalContactPass && !R.bGlintsDone)
            {
                R.bGlintsDone = true;
                RenderHeadlightGlints(pDevice);
            }
        });
    }

    static void InstallShaftHooks()
    {
        auto& R = PostFxResources;
        if (!R.nVolumetricLightHeadlightFlag)
            return;
        if (!R.bVolumetricLightHeadlightShadow)
        {
            auto pattern = hook::pattern("A1 ? ? ? ? 0F 28 05 ? ? ? ? 0F 29 84 24 A0 00 00 00 F3 0F 10 44 06 70");
            if (!pattern.empty())
            {
                static uintptr_t* list = *pattern.get_first<uintptr_t*>(1);
                shShaftDraw = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                {
                    const auto& light = *reinterpret_cast<const rage::CLightSource*>(*list + regs.esi);
                    if (PostFxResources.VolumetricLight() && (light.mFlags & PostFxResources.nVolumetricLightHeadlightFlag))
                        *reinterpret_cast<uint8_t*>(regs.esp + 0xF) = 0;
                });
            }
        }
        if (R.fVolumetricLightHeadlightPitch != 0.0f)
        {
            // The copies of direction (esp + 0xF0) and tangent (esp + 0xE0), then push 0; the
            // hook sits on the load of the outer cone angle right after, eax the light list and
            // esi this light's offset in it.
            auto pattern = hook::pattern("F3 0F 10 04 06 F3 0F 11 84 24 F0 00 00 00 F3 0F 10 44 06 04 F3 0F 11 84 24 F4 00 00 00 "
                                         "F3 0F 10 44 06 08 F3 0F 11 84 24 F8 00 00 00 F3 0F 10 44 06 10 F3 0F 11 84 24 E0 00 00 00 "
                                         "F3 0F 10 44 06 14 F3 0F 11 84 24 E4 00 00 00 F3 0F 10 44 06 18 6A 00 "
                                         "F3 0F 11 84 24 EC 00 00 00 F3 0F 10 6C 06 5C");
            if (!pattern.empty())
            {
                static const float pitch = R.fVolumetricLightHeadlightPitch * 3.14159265f / 180.0f;
                static const float c = std::cos(pitch), s = -std::sin(pitch);
                shShaftAim = safetyhook::create_mid(pattern.get_first(0x5B), [](SafetyHookContext& regs)
                {
                    const auto& light = *reinterpret_cast<const rage::CLightSource*>(regs.eax + regs.esi);
                    if (!PostFxResources.VolumetricLight() || !(light.mFlags & PostFxResources.nVolumetricLightHeadlightFlag))
                        return;
                    // 4 bytes further off esp for the push 0.
                    float* dir = reinterpret_cast<float*>(regs.esp + 0xF4);
                    float* tangent = reinterpret_cast<float*>(regs.esp + 0xE4);
                    // The level axis across the beam, dir x up.
                    float kx = dir[1], ky = -dir[0];
                    const float len = std::sqrt(kx * kx + ky * ky);
                    if (len < 1e-3f)
                        return; // aimed straight up or down
                    kx /= len;
                    ky /= len;
                    // Rodrigues: v cos + (k x v) sin + k (k . v)(1 - cos), turning down.
                    auto turn = [&](float* v)
                    {
                        const float kv = kx * v[0] + ky * v[1];
                        const float cx = ky * v[2], cy = -kx * v[2], cz = kx * v[1] - ky * v[0];
                        v[0] = v[0] * c + cx * s + kx * kv * (1.0f - c);
                        v[1] = v[1] * c + cy * s + ky * kv * (1.0f - c);
                        v[2] = v[2] * c + cz * s;
                    };
                    turn(dir);
                    turn(tangent);
                });
            }
        }
    }

    // Most people's heads and every player's hands are drawn with gta_ped, the shader of clothes,
    // which cannot tell skin in a pixel; the component loops of the ped draw can. Each turn of
    // the loop (CE 0xAB7C20 for component peds, 0xAB7EB0 the other) sets c150.x for its
    // component: a quarter step for HEAD and HAND, which gta_ped and gta_ped_reflect add to the
    // material ID as the skin shaders do (ped_skin_scattering_mask.patch), 0 for the rest. FACE,
    // the last turn, leaves it at 0 for whatever is drawn next.
    static inline SafetyHookMid shPedSkinComponent[2]{};

    static void MarkPedSkin(uintptr_t component)
    {
        auto& R = PostFxResources;
        const bool on = R.SkinScatteringEnabled() || R.SSRDebugMode() == R.kSkinDebugMode;
        const float mark[4] = { (on && (component == 0 || component == 4)) ? 0.25f / 255.0f : 0.0f, 0.0f, 0.0f, 0.0f };
        if (auto pDevice = rage::grcDevice::GetD3DDevice())
            pDevice->SetPixelShaderConstantF(150, mark, 1);
    }

    static void InstallPedSkinHooks()
    {
        auto pattern = hook::pattern("8B 44 24 24 8B B6 28 01 00 00 0F B6 7C 28 5C 0F B7 46 0C");
        if (!pattern.empty())
            shPedSkinComponent[0] = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs) { MarkPedSkin(regs.ebp); });
        pattern = hook::pattern("80 78 5C 00 74 09 83 FE 03 0F 84 ? ? ? ? 80 BE ? ? ? ? 00 0F 84");
        if (!pattern.empty())
            shPedSkinComponent[1] = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs) { MarkPedSkin(regs.esi); });
    }

    // Runs as the game copies each light into the frame's draw list, on the main thread.
    static void OnAfterCopyLight(rage::CLightSource* light)
    {
        auto& R = PostFxResources;
        if (!light)
            return;
        // Building Fill Lights off: the large exterior map lights, no interior, vehicle, traffic
        // light or fire, that flood whole squares go dark.
        constexpr uint32_t fillMask = rage::LF_MAP | rage::LF_INTERIOR | rage::LF_EXTERIOR | rage::LF_VEHICLE | rage::LF_TRAFFIC;
        if (!R.bFillLights && (light->mFlags & fillMask) == (rage::LF_MAP | rage::LF_EXTERIOR) &&
            (light->mType == rage::LT_POINT || light->mType == rage::LT_SPOT) && light->mRadius >= R.fFillLightsMinRadius)
            light->mIntensity = 0.0f;
        if (!R.VolumetricLight() || R.fVolumetricLightIntensity <= 0.0f)
            return;

        float cameraPos[3]{};
        GameCamera::Position(cameraPos);
        const float dx = cameraPos[0] - light->mPosition.x;
        const float dy = cameraPos[1] - light->mPosition.y;
        const float dz = cameraPos[2] - light->mPosition.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Spot lights of 8 to 20 m, most of lamppost.img, none of a vehicle, traffic light, the
        // unculled source or LF_10, and none with a shaft already; headlights are the spot lights
        // of at least 8 m with the headlight flag (smaller ones are tail and brake lights),
        // whatever their other flags.
        if (light->mType != rage::LT_SPOT || (light->mFlags & rage::LF_SHAFT))
            return;
        const uint32_t headlightFlag = R.nVolumetricLightHeadlightFlag;
        const bool headlight = headlightFlag && (light->mFlags & headlightFlag) && light->mRadius >= 8.0f &&
                               R.fVolumetricLightHeadlightIntensity > 0.0f;
        constexpr uint32_t noShaft = rage::LF_TRAFFIC | rage::LF_VEHICLE | rage::LF_UNCULLED | rage::LF_10 | rage::LF_SHAFT;
        if (!headlight && (light->mRadius < 8.0f || light->mRadius > 20.0f || (light->mFlags & noShaft)))
            return;
        const float fadeStart = R.fVolumetricLightMaxDistance * 0.3f;
        const float x = std::clamp((distance - fadeStart) / (R.fVolumetricLightMaxDistance - fadeStart), 0.0f, 1.0f);
        const float fade = 1.0f - x * x * (3.0f - 2.0f * x);
        if (fade <= 0.0f)
            return;

        light->mFlags |= rage::LF_SHAFT;
        if (headlight)
            light->mFlags |= R.nVolumetricLightHeadlightAddFlags;
        light->mVolumeIntensity = (headlight ? R.fVolumetricLightHeadlightIntensity : R.fVolumetricLightIntensity) * fade;
        // The shaft mesh reaches about 0.66 to 0.85 of radius times scale (light shaft VS), so
        // headlights take a scale that gives the same length to low beams of 33 m and high
        // beams of 75 to 98 m, never past their own reach.
        light->mVolumeScale = headlight ? (std::min)(R.fVolumetricLightHeadlightLength / light->mRadius, 1.0f)
                                        : R.fVolumetricLightScale;
    }

public:
    // What deferred_lighting reads besides the game's own inputs: s3 the SSR result (the cleared
    // SSR target while SSR is off, else a transparent 1x1), s9 the contact shadows while they
    // are valid, s8 the indirect light, c202-c204 the local light contact shadow constants, and
    // c201, c205 and s11 (the material IDs) the light on skin.
    static void BindLightingInputs(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        // gDirectionalLight is a RAGE global, the same register in every shader: the direction the
        // sun's (or moon's) light travels. The clouds take the reverse of it.
        {
            float light[4] = {};
            if (SUCCEEDED(pDevice->GetPixelShaderConstantF(17, light, 1)))
            {
                const float len = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
                if (len > 0.9f && len < 1.1f)
                {
                    for (int i = 0; i < 3; ++i)
                        R.CloudLightDir[i] = -light[i] / len;
                    R.nCloudLightFrame = FrameHistory::Frame();
                }
            }
        }
        // s9 is read by no game shader, and the car glass takes it over right after lighting.
        if (R.bContactValid && R.ContactResult)
        {
            BindSampler(pDevice, 9, R.ContactResult, D3DTEXF_POINT);
            R.bContactBound = true;
        }
        else if (R.bContactBound)
        {
            SetTextureBoth(pDevice, 9, nullptr);
            R.bContactBound = false;
        }
        pDevice->SetPixelShaderConstantF(202, R.LocalContactShadowConsts, 3);
        R.bLocalContactPass = true;
        VehicleBoxShadows::Off(pDevice);
        R.bLocalContactLightOff = false;
        R.fLocalContactLightIntensity = R.LocalContactShadowConsts[3];

        // The sun on skin: c201 the scale of the N.L curve less 1, c205 its offset and the red penumbra.
        {
            const float k = R.SkinScatteringEnabled() ? R.fSkinLighting : 0.0f;
            const float wrap[3] = { 0.35f * k, 0.25f * k, 0.2f * k };
            float scale[4] = {}, offset[4] = {};
            for (int i = 0; i < 3; ++i)
            {
                scale[i] = 1.0f / (1.0f + wrap[i]) - 1.0f;
                offset[i] = wrap[i] / (1.0f + wrap[i]);
            }
            offset[3] = (std::min)(0.2f * k, 1.0f);
            pDevice->SetPixelShaderConstantF(201, scale, 1);
            pDevice->SetPixelShaderConstantF(205, offset, 1);
        }
        // The GGX highlights. c200: x the strength (0 the game's own), halved since the shaders
        // divide by twice the visibility's denominator, y the Fresnel rise, z the lights' radius
        // squared, w the lobe's stretch along the light, squared. c165: the fill lights' share,
        // the sun's (0 its own highlight), the environment BRDF's blend, the highlight's ceiling. c206, the headlights'
        // spacing, is set per light (InstallLocalContactLightHook); s13 the G-buffer's specular for
        // the fill lights, read by no game shader while the lights are drawn.
        {
            const bool enabled = R.GGXLightingEnabled();
            const bool on = enabled && R.fLightsGGX > 0.0f;
            const float stretch = 1.0f + R.fLightsGGXStretch;
            const float c200[4] = { on ? R.fLightsGGX * 0.5f : 0.0f, R.fLightsGGXFresnel, R.fLightsGGXSize * R.fLightsGGXSize, stretch * stretch };
            const float c165[4] = { on ? R.fLightsGGXFillLights : 0.0f, on ? R.fLightsGGXSun : 0.0f, enabled ? R.fLightsGGXEnvironment : 0.0f, R.fLightsGGXMax };
            pDevice->SetPixelShaderConstantF(200, c200, 1);
            pDevice->SetPixelShaderConstantF(165, c165, 1);
            // c183: alpha squared = w / (x n + 2), the peak times y + z alpha squared; c184: x a widening
            // added to alpha squared, y how far the stretch turns from the light to the view.
            const float soft = R.fLightsGGXSoft;
            const float c183[4] = { 4.0f - 3.0f * soft, 1.0f - soft, 32.0f * soft, 2.0f + 1.38f * soft };
            const float c184[4] = { 0.04f * soft, R.fLightsGGXStretchView, 0.0f, 0.0f };
            pDevice->SetPixelShaderConstantF(183, c183, 1);
            pDevice->SetPixelShaderConstantF(184, c184, 1);
            std::memset(R.LightGGXShape, 0, sizeof(R.LightGGXShape));
            pDevice->SetPixelShaderConstantF(206, R.LightGGXShape, 1);
            R.nGlintCandidates = 0;
            R.bGlintsDone = false;
            if (R.mSpecularRT && R.mSpecularRT->mD3DTexture)
            {
                BindSampler(pDevice, 13, R.mSpecularRT->mD3DTexture, D3DTEXF_POINT);
                R.bSpecularBound = true;
            }
        }
        // The sun on materials with no specular map (x) and the cloud shadows (yzw, c198, c199, s12).
        {
            float threshold = 0.0f, bias = 0.0f, thickness = 0.0f;
            if (R.bCloudParamsRegistered)
            {
                threshold = rage::grmShaderInfo::getShaderParamData(R.CloudThresholdIdx)[0];
                bias = rage::grmShaderInfo::getShaderParamData(R.CloudBiasIdx)[0];
                thickness = rage::grmShaderInfo::getShaderParamData(R.CloudThicknessIdx)[0];
            }
            // Until the sky has been drawn once its parameters read zero, which is no cloud at all.
            R.bCloudLastFromGame = !(threshold == 0.0f && bias == 0.0f);
            if (threshold == 0.0f && bias == 0.0f)
            {
                threshold = 1.6f;
                bias = 0.6f;
            }
            if (thickness <= 0.0f)
                thickness = 1.0f;
            float coverageShift = R.fCloudShadowsCoverage;
            float deckHeight = R.fCloudShadowsHeight;
            const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
            R.UpdateCloudLayer(seconds);
            // The game's flat clouds under the volumetric ones, by VolumetricCloudsVanilla:
            // gta_atmoscatt_clouds draws them where CloudThreshold * noise - CloudBias is above
            // 0, so threshold * v and bias towards 1 by 1 - v fade them out at 0. Its high layer
            // stays.
            if (R.bCloudParamsRegistered)
            {
                const bool fade = R.VolumetricCloudsOn() && R.fVolumetricCloudsVanilla < 1.0f;
                const float v = R.fVolumetricCloudsVanilla;
                auto& thresholdOverride = rage::grmShaderInfo::getShaderParamOverride(R.CloudThresholdIdx);
                auto& biasOverride = rage::grmShaderInfo::getShaderParamOverride(R.CloudBiasIdx);
                thresholdOverride.mul = { v, 1.0f, 1.0f, 1.0f };
                thresholdOverride.add = {};
                biasOverride.mul = { v, 1.0f, 1.0f, 1.0f };
                biasOverride.add = { 1.0f - v, 0.0f, 0.0f, 0.0f };
                thresholdOverride.on = fade;
                biasOverride.on = fade;
            }
            // The shadows work out the clouds' coverage as Clouds.fx does (deferred_lighting_sun_under_clouds
            // patch): from the volumetric clouds' layer while they are on, otherwise from CloudShadowsCoverage
            // about a cover of 0.4 at CloudShadowsHeight. The game's threshold, bias and thickness are only
            // logged.
            float cover = std::clamp(0.4f + coverageShift, 0.02f, 1.0f);
            if (R.VolumetricCloudsOn())
            {
                // An overcast sheet's evened coverage is left out: under it the sun is weak anyway.
                cover = (std::max)(R.Cloud.coverage, 0.02f);
                deckHeight = R.Cloud.base + R.Cloud.thickness * 0.33f;
            }
            R.fCloudLastThreshold = threshold;
            R.fCloudLastBias = bias;
            R.fCloudLastThickness = thickness;

            auto noise = (R.fCloudShadows > 0.0f || R.VolumetricCloudsOn()) ? R.CloudNoiseTex() : nullptr;
            const float strength = noise ? R.fCloudShadows : 0.0f; // 0 leaves the shadows out
            const float invScale = 1.0f / R.fCloudShadowsScale;
            // The wind blows the same way all the time; only how far it has carried the noise
            // changes, wrapped to one tile so the offset keeps its precision.
            const float windX = static_cast<float>(std::fmod(R.fCloudDrift * 0.93, 1.0));
            const float windY = static_cast<float>(std::fmod(R.fCloudDrift * 0.37, 1.0));

            R.fCloudWindX = windX;
            R.fCloudWindY = windY;
            R.fCloudSeconds = seconds;
            float c197[4] = { R.fSpecularSheen, strength, deckHeight, invScale };
            float c198[4] = { windX, windY, cover, R.kCloudWeatherReach };
            float c199[4] = { 0.0f, R.fCloudShadowsSoftness, R.CloudMorphPhase(), R.kCloudMorphReach / R.fCloudShadowsScale };
            if (R.CloudShadowsDebug() == 1)
            {
                // Full cover everywhere.
                c199[0] = 1.0f;
            }
            else if (R.CloudShadowsDebug() == 2)
            {
                // Cover 1, so the coverage is close to the map itself, twenty times finer, so its
                // blotches show around the player; at the clouds' scale they spanned hundreds of
                // metres and the ground looked evenly lit.
                c198[2] = 1.0f;
                c198[3] = 0.0f;
                c197[3] *= 20.0f;
            }
            pDevice->SetPixelShaderConstantF(197, c197, 1);
            pDevice->SetPixelShaderConstantF(198, c198, 1);
            pDevice->SetPixelShaderConstantF(199, c199, 1);
            std::memcpy(R.CloudShadowConsts, c197, sizeof(c197));
            std::memcpy(R.CloudShadowConsts + 4, c198, sizeof(c198));
            std::memcpy(R.CloudShadowConsts + 8, c199, sizeof(c199));
            if (noise)
            {
                // s12 is read only by G-buffer and particle shaders, none of which draw while the
                // lights do. On s7, rage_postfx's, the sun read another texture and the shadows
                // never showed.
                // Every state: the G-buffer pass leaves texturequality's detail texture states on s12
                // (kDetailStage), and the sun read zero from the noise although it was bound.
                DWORD srgb = 0, maxMip = 0, minFilter = 0, lodBias = 0;
                pDevice->GetSamplerState(12, D3DSAMP_SRGBTEXTURE, &srgb);
                pDevice->GetSamplerState(12, D3DSAMP_MAXMIPLEVEL, &maxMip);
                pDevice->GetSamplerState(12, D3DSAMP_MINFILTER, &minFilter);
                pDevice->GetSamplerState(12, D3DSAMP_MIPMAPLODBIAS, &lodBias);
                R.CloudSamplerBefore[0] = srgb;
                R.CloudSamplerBefore[1] = maxMip;
                R.CloudSamplerBefore[2] = minFilter;
                R.CloudSamplerBefore[3] = lodBias;
                SetTextureBoth(pDevice, 12, noise);
                pDevice->SetSamplerState(12, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                pDevice->SetSamplerState(12, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                pDevice->SetSamplerState(12, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
                pDevice->SetSamplerState(12, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                pDevice->SetSamplerState(12, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                pDevice->SetSamplerState(12, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                pDevice->SetSamplerState(12, D3DSAMP_MAXMIPLEVEL, 0);
                pDevice->SetSamplerState(12, D3DSAMP_MIPMAPLODBIAS, 0);
                pDevice->SetSamplerState(12, D3DSAMP_MAXANISOTROPY, 1);
                pDevice->SetSamplerState(12, D3DSAMP_SRGBTEXTURE, FALSE);
                R.bCloudNoiseBound = true;
            }
        }
        // The light volumes do not read the material IDs themselves (local_light_on_skin.patch).
        // s11 is read by no game shader, and the car glass takes it over right after lighting;
        // s10 is the game's StippleTexture, which the final post fx pass reads as its colour LUT.
        if (R.mMaterialIdRT && R.mMaterialIdRT->mD3DTexture)
        {
            BindSampler(pDevice, 11, R.mMaterialIdRT->mD3DTexture, D3DTEXF_POINT);
            R.bMaterialIdBound = true;
        }

        // Indirect light, black while there is none; unbound after lighting.
        BindSampler(pDevice, 8, R.GIResult ? static_cast<IDirect3DBaseTexture9*>(R.GIResult) : R.TransparentTex(), D3DTEXF_LINEAR);
        R.bGIBound = true;

        IDirect3DBaseTexture9* tex = R.TransparentTex();
        if (R.SSRResult)
            tex = R.SSRResult;
        else if (R.SSRTex && R.SSRTex->mD3DTexture)
            tex = R.SSRTex->mD3DTexture; // cleared while SSR is off
        SetTextureBoth(pDevice, 3, tex);
        SSRTrace::Line("bind for lighting: s3 %s, ssr valid this frame %d", SSRTrace::TextureName(tex).c_str(), int(R.bSSRValidThisFrame));
        pDevice->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        pDevice->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        pDevice->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        pDevice->SetSamplerState(3, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }

    // Runs as the first command of the lighting phase's list (OnBuildRenderList), before the
    // command in that list that renders AO, SSR and contact shadows, so what it binds is last
    // frame's. For the accumulated SSR that was the other target of the pair, and lighting
    // showed last frame's reflections, which swung off the car while the camera turned and came
    // back when it stopped; the contact shadows' validity and constants were a frame old too.
    // RenderPedAndVehicleFakeShadows binds this frame's once they are done.
    static void BindSSRTexture()
    {
        if (auto pDevice = rage::grcDevice::GetD3DDevice(); pDevice && !bDeviceLost)
        {
            nLightingStage = 0; // a frame that never closed its lighting leaves nothing open here
            ProfilerMark(pDevice, kProfLighting, true);
            BindLightingInputs(pDevice);
        }
    }

    static void BindSampler(IDirect3DDevice9* pDevice, DWORD slot, IDirect3DBaseTexture9* tex, DWORD filter)
    {
        SetTextureBoth(pDevice, slot, tex);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MAGFILTER, filter);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MINFILTER, filter);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }

    // c204 for the car glass (vehicle_glass_reflections_wet.patch): from the driver's seat in first person the glass
    // within 2.5 m of the camera, the car's own, keeps 40% of its tint; zeros (glass in reflections and mirrors, drawn
    // once the main scene is done) keep all of it.
    static void SetGlassInsideTint(IDirect3DDevice9* pDevice, bool inside)
    {
        const float tint[4] = { inside ? 0.4f : 0.0f, inside ? 2.5f * 2.5f : 0.0f, 0.0f, 0.0f };
        pDevice->SetPixelShaderConstantF(204, tint, 1);
        if (auto real = RealDevice(pDevice); real != pDevice)
            real->SetPixelShaderConstantF(204, tint, 1);
    }

    // Right after deferred lighting: binds this frame's camera, and the textures the fog pass
    // copies the depth and the lit opaque scene into, for the patched car glass shaders. Anything missing
    // leaves s9 empty, and the glass keeps the game's environment map.
    static void PrepareGlassReflections()
    {
        auto& R = PostFxResources;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        SetLightingStage(pDevice, 0);
        if (bDeviceLost)
            return;
        ProfilerMark(pDevice, kProfLighting, false);

        if (R.bContactBound)
        {
            SetTextureBoth(pDevice, 9, nullptr);
            R.bContactBound = false;
        }
        if (R.bGIBound)
        {
            SetTextureBoth(pDevice, 8, nullptr);
            R.bGIBound = false;
        }
        if (R.bMaterialIdBound)
        {
            SetTextureBoth(pDevice, 11, nullptr);
            R.bMaterialIdBound = false;
        }
        if (R.bSpecularBound)
        {
            SetTextureBoth(pDevice, 13, nullptr);
            R.bSpecularBound = false;
        }
        if (R.bCloudNoiseBound)
        {
            // Whether the noise was still there once the lights were drawn, for the log.
            IDirect3DBaseTexture9* bound = nullptr;
            pDevice->GetTexture(12, &bound);
            R.bCloudNoiseSurvived = bound && bound == R.CloudNoiseTexture;
            SAFE_RELEASE(bound);
            SetTextureBoth(pDevice, 12, nullptr);
            R.bCloudNoiseBound = false;
        }
        // Lights drawn for other views (reflections, mirrors) must not march with this camera,
        // nor light skin by this view's material IDs.
        const float noLocalContactShadows[4] = {};
        pDevice->SetPixelShaderConstantF(203, noLocalContactShadows, 1);
        R.bLocalContactPass = false;
        VehicleBoxShadows::Off(pDevice);
        pDevice->SetPixelShaderConstantF(201, noLocalContactShadows, 1);
        pDevice->SetPixelShaderConstantF(205, noLocalContactShadows, 1);
        pDevice->SetPixelShaderConstantF(197, noLocalContactShadows, 1);
        pDevice->SetPixelShaderConstantF(200, noLocalContactShadows, 1);
        pDevice->SetPixelShaderConstantF(165, noLocalContactShadows, 1);
        SetGlassInsideTint(pDevice, FirstPersonInVehicle());

        bool ok = R.bGlassFrameValid && R.bGlassReflections && R.SSREnabled() && R.PreAlphaDepthCopyRT &&
                  R.PreAlphaDepthCopyRT->mD3DTexture && R.SSRHistoryTex && R.SSRHistoryTex->mD3DTexture;
        R.bGlassFrameValid = false;

        if (ok && !R.GlassParamsTex)
            ok = SUCCEEDED(RealDevice(pDevice)->CreateTexture(5, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &R.GlassParamsTex, nullptr));
        if (ok)
        {
            D3DLOCKED_RECT locked = {};
            ok = SUCCEEDED(R.GlassParamsTex->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD));
            if (ok)
            {
                // A32B32G32R32F stores each texel as r, g, b, a floats.
                memcpy(locked.pBits, R.GlassParams, sizeof(R.GlassParams));
                R.GlassParamsTex->UnlockRect(0);
            }
        }

        if (!ok)
        {
            UnbindGlassReflections();
            return;
        }

        BindSampler(pDevice, 9, R.GlassParamsTex, D3DTEXF_POINT);
        BindSampler(pDevice, 11, R.PreAlphaDepthCopyRT->mD3DTexture, D3DTEXF_POINT);
        BindSampler(pDevice, 13, R.SSRHistoryTex->mD3DTexture, D3DTEXF_LINEAR);
        R.bGlassBound = true;
    }

    // Once the main scene is done, so car glass drawn by other render phases (reflections,
    // mirrors) never marches with this camera's data.
    static void UnbindGlassReflections()
    {
        auto& R = PostFxResources;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice || !R.bGlassBound)
            return;
        SetTextureBoth(pDevice, 9, nullptr);
        SetTextureBoth(pDevice, 11, nullptr);
        SetTextureBoth(pDevice, 13, nullptr);
        R.bGlassBound = false;
    }

    PostFX()
    {
        FusionFix::onInitEventAsync() += []()
        {
            if (GetD3DX9_43DLL())
            {
                PostFxResources.Readini();
                PostFxResources.RegisterCloudParams();
                RegisterPbrSkyParams();
                FusionFix::onEndScene() += []()
                {
                    AerialPerspectiveEndFrame(rage::grcDevice::GetD3DDevice());
                };
                FrameGeneration::SetSharpener(SharpenShownFrame);

                auto pattern = find_pattern("E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 5F", "E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 8B 4F ? E8 ? ? ? ? 33 C0");
                hbDrawPrimitivePostFX.fun = injector::MakeCALL(pattern.get_first(0), DrawPrimitivePostFX).get();

                pattern = find_pattern("E8 ? ? ? ? 8D 44 24 ? 50 8B CF E8 ? ? ? ? 8D 84 24", "E8 ? ? ? ? 8D 44 24 ? 50 8B CE E8 ? ? ? ? 8D 8C 24 ? ? ? ? 51 8B CE E8 ? ? ? ? 8D 94 24");
                hbDrawSkyHook.fun = injector::MakeCALL(pattern.get_first(0), DrawSky).get();

                pattern = hook::pattern("6A 00 6A 00 6A 00 6A 00 6A 03 57 E8 ? ? ? ? 5F 5E 8B E5 5D C3");
                if (!pattern.empty())
                    hbDrawSkyReflection.fun = injector::MakeCALL(pattern.get_first(11), DrawSkyReflection).get();

                pattern = find_pattern("E8 ? ? ? ? 6A ? FF B7 ? ? ? ? 8B CF FF 77 ? E8 ? ? ? ? 5F", "E8 ? ? ? ? 8B 8E ? ? ? ? 8B 56 ? 6A ? 51");
                hbDrawCallPostFX.fun = injector::MakeCALL(pattern.get_first(0), DrawCallPostFX).get();

                // Downsampling of the scene for bloom and exposure, the first pass of the game's post processing
                pattern = hook::pattern("6A 00 FF B7 ? ? ? ? FF 77 18 E8 ? ? ? ? FF 77 18 8B 4F 60");
                if (!pattern.empty())
                    hbDrawCallDownsample.fun = injector::MakeCALL(pattern.get_first(11), DrawCallDownsample).get();
                else
                {
                    pattern = hook::pattern("8B 46 18 6A 00 52 50 8B CE E8 ? ? ? ? 8B 4E 18 8B 46 60");
                    if (!pattern.empty())
                        hbDrawCallDownsample.fun = injector::MakeCALL(pattern.get_first(9), DrawCallDownsample).get();
                }

                pattern = find_pattern("55 8B EC 83 E4 F0 81 EC D8 00 00 00 56 57 E8 ? ? ? ?");
                if (!pattern.empty())
                    shWaterRender = safetyhook::create_inline(pattern.get_first(0), WaterRenderHook);

                pattern = find_pattern("F3 0F 10 05 ? ? ? ? F3 0F 11 44 24 08 FF 74 24 08");
                if (!pattern.empty())
                    PostFxResources.pWaterLevel = *pattern.get_first<const float*>(4);

                {
                    CRenderPhaseDeferredLighting_LightsToScreen::OnAfterCopyLight() += OnAfterCopyLight;
                    InstallShaftHooks();
                    InstallLocalContactLightHook();
                    VehicleBoxShadows::Install();
                    InstallShaftLoopProfiler();
                    InstallPedSkinHooks();
                    CRenderPhaseDeferredLighting_LightsToScreen::OnBuildRenderList() += []()
                    {
                        PostFxResources.bInteriorScene = Natives::IsInteriorScene();
                        PostFxResources.bPlayerInCar = CPlayer::findPlayerCar && CPlayer::findPlayerCar() != 0;
                        auto cb = new T_CB_Generic_NoArgs(BindSSRTexture);
                        if (cb)
                            cb->Append();
                    };
                    CRenderPhaseDeferredLighting_LightsToScreen::OnAfterBuildRenderList() += []()
                    {
                        auto cb = new T_CB_Generic_NoArgs(PrepareGlassReflections);
                        if (cb)
                            cb->Append();
                    };
                }

                // The fog pass takes the pre-alpha depth copy and SSR's scene history, so it is
                // hooked whatever EnablePreAlphaDepth says; without it SSR never saw the scene.
                {
                    pattern = hook::pattern("FF B6 ? ? ? ? 6A ? E8 ? ? ? ? 5E 8B E5 5D C3");
                    if (!pattern.empty())
                    {
                        hbDrawCallFog.fun = injector::MakeCALL(pattern.get_first(8), DrawCallFog).get();
                    }
                    else
                    {
                        pattern = hook::pattern("6A ? 8B CE E8 ? ? ? ? 5E 8B E5");
                        hbDrawCallFog.fun = injector::MakeCALL(pattern.get_first(4), DrawCallFog).get();
                    }
                }

                pattern = find_pattern("55 8B EC 83 E4 ? 8B 0D ? ? ? ? 8B 15 ? ? ? ? 8B 41", "55 8B EC 83 E4 ? 8B 0D ? ? ? ? 8B 41 ? 8B 15");
                RenderPedAndVehicleFakeShadowsInlineHook = safetyhook::create_inline(pattern.get_first(0), RenderPedAndVehicleFakeShadows); 
            }
        };
    }
} PostFX;