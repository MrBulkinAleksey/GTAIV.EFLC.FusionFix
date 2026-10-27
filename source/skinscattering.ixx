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

export module skinscattering;

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

// Skin scattering in the fog pass.

// As the fog pass begins, once every light is drawn: scatters the light under the skin of
// scene, the lit scene the fog pass reads (see SkinLight_PS in SSR.fx), and returns the scene
// with it, or null when it does not run. SSR Debug 9 shows what counts as skin.
export IDirect3DBaseTexture9* RenderSkinScattering(IDirect3DDevice9* pDevice, IDirect3DBaseTexture9* scene)
{
    auto& R = PostFxResources;
    auto& h = R.SSREffectHandles;
    ID3DXEffect* effect = R.SSREffect;
    const bool scatter = R.SkinScatteringEnabled() && R.fSkinScatteringStrength > 0.0f;
    const bool debug = R.SSRDebugMode() == R.kSkinDebugMode && R.SSRDebugSurf && h.techSkinDebug;
    if ((!scatter && !debug) || !pDevice || !effect || !scene || scene->GetType() != D3DRTYPE_TEXTURE ||
        !h.techSkinLight || !h.techSkinScatter || !h.techSkinScatterFinal || !R.mMaterialIdRT || !R.mMaterialIdRT->mD3DTexture ||
        !R.mDiffuseRT || !R.mDiffuseRT->mD3DTexture || !R.mDepthRT || !R.mDepthRT->mD3DTexture ||
        !R.SkinLightSurf[0] || !R.SkinLightSurf[1] || R.SkinCamera[2] <= 0.0f)
        return nullptr;

    D3DSURFACE_DESC sceneDesc = {}, lightDesc = {};
    if (FAILED(static_cast<IDirect3DTexture9*>(scene)->GetLevelDesc(0, &sceneDesc)) || FAILED(R.SkinLightSurf[0]->GetDesc(&lightDesc)) ||
        sceneDesc.Width != lightDesc.Width || sceneDesc.Height != lightDesc.Height)
        return nullptr;
    const float width = float(lightDesc.Width);
    const float height = float(lightDesc.Height);

    effect->SetTexture(h.SceneTex2D, scene);
    effect->SetTexture(h.SkinIDTex2D, R.mMaterialIdRT->mD3DTexture);
    effect->SetTexture(h.AlbedoTex2D, R.mDiffuseRT->mD3DTexture);
    effect->SetTexture(h.DepthTex2D, LightingDepth());
    SetDepthRange(effect, h, R.SkinCamera[2], R.SkinCamera[3]);
    effect->SetFloat(h.fSkinStrength, R.fSkinScatteringStrength);

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

    IDirect3DBaseTexture9* result = nullptr;
    if (scatter)
    {
        DrawEffectPass(pDevice, effect, h.techSkinLight, R.SkinLightSurf[0], width, height, kProfSkinLight);
        // A kernel unit is half SkinScatteringWidth, and a metre at view depth 1 spans _11 / 2
        // of the screen across and _22 / 2 down.
        const float unit = R.fSkinScatteringWidth * 0.25f;
        D3DXVECTOR4 step(R.SkinCamera[0] * unit, 0.0f, 0.0f, R.fSkinScatteringWidth * 0.5f);
        effect->SetVector(h.vec4SkinStep, &step);
        effect->SetTexture(h.SkinLightTex2D, R.SkinLightTex[0]->mD3DTexture);
        DrawEffectPass(pDevice, effect, h.techSkinScatter, R.SkinLightSurf[1], width, height, kProfSkinScatter);
        step = D3DXVECTOR4(0.0f, R.SkinCamera[1] * unit, 0.0f, R.fSkinScatteringWidth * 0.5f);
        effect->SetVector(h.vec4SkinStep, &step);
        effect->SetTexture(h.SkinLightTex2D, R.SkinLightTex[1]->mD3DTexture);
        DrawEffectPass(pDevice, effect, h.techSkinScatterFinal, R.SkinLightSurf[0], width, height, kProfSkinFinal);
        result = R.SkinLightTex[0]->mD3DTexture;
    }
    SSRTrace::Line("skin: scatter %d, near %.4f far %.1f", int(scatter), R.SkinCamera[2], R.SkinCamera[3]);

    if (debug)
    {
        effect->SetTexture(h.SkinLightTex2D, result ? result : scene);
        DrawEffectPass(pDevice, effect, h.techSkinDebug, R.SSRDebugSurf, width, height);
        R.bSSRDebugValid = true;
    }

    return result;
}
