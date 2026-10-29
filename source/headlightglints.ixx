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

export module headlightglints;

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

// Headlight glints (HeadlightGlints.fx) at the start of the light shaft loop.

// Headlight glints (HeadlightGlints.fx): at the start of the game's light shaft loop, once every light
// of the main view is drawn, added to the lit scene they were drawn into. Whatever the pass changes is
// put back for the shafts.
export void RenderHeadlightGlints(IDirect3DDevice9* pDevice)
{
    auto& R = PostFxResources;
    R.nGlintsLastLights = 0;
    auto skip = [&](const char* why) { R.szGlintsStatus = why; };
    if (!R.GGXLightingEnabled() || R.fLightsGGX <= 0.0f)
        return skip("GGX Lighting off");
    if (R.fLightsGGXGlints <= 0.0f)
        return skip("off in the ini");
    if (!R.HeadlightGlintsEffect)
        return skip("no effect");
    if (!R.nGlintCandidates)
        return skip("no headlights");
    if (!pDevice || !R.mNormalRT || !R.mSpecularRT || !R.mNormalRT->mD3DTexture || !R.mSpecularRT->mD3DTexture)
        return skip("no G-buffer");
    IDirect3DTexture9* depth = LightingDepth();
    if (!depth)
        return skip("no depth");
    rage::grcViewport* vp = rage::GetCurrentViewport();
    if (!vp)
        return skip("no viewport");
    D3DSURFACE_DESC desc = {};
    if (FAILED(R.mNormalRT->mD3DTexture->GetLevelDesc(0, &desc)))
        return skip("no G-buffer");

    ProfilerScope timed(pDevice, kProfGlints);
    // The nearest headlights.
    const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
    auto* candidates = R.GlintCandidates;
    const uint32_t found = R.nGlintCandidates;
    for (uint32_t i = 0; i < found; ++i)
    {
        const float dx = candidates[i].position[0] - viewInv.m[3][0];
        const float dy = candidates[i].position[1] - viewInv.m[3][1];
        const float dz = candidates[i].position[2] - viewInv.m[3][2];
        candidates[i].distance = dx * dx + dy * dy + dz * dz;
    }
    const uint32_t count = (std::min)(found, PostFxResource::kGlintLights);
    std::partial_sort(candidates, candidates + count, candidates + found,
                      [](const auto& a, const auto& b) { return a.distance < b.distance; });

    constexpr uint32_t kLights = PostFxResource::kGlintLights;
    D3DXVECTOR4 position[kLights] = {}, direction[kLights] = {}, right[kLights] = {}, colour[kLights] = {};
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& g = candidates[i];
        float d[3] = { g.direction[0], g.direction[1], g.direction[2] };
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > 1e-3f)
            for (auto& c : d)
                c /= len;
        // The car's level right, dir x up, as SetLightGGXShape takes it; a light aimed straight up
        // or down, or a lamp alone, keeps one lamp.
        const float kx = d[1], ky = -d[0];
        const float rlen = std::sqrt(kx * kx + ky * ky);
        const bool level = rlen > 1e-3f;
        // The game's own highlight ends at about two thirds of the light's radius.
        position[i] = D3DXVECTOR4(g.position[0], g.position[1], g.position[2], level && g.twin ? R.fLightsGGXHeadlights : 0.0f);
        direction[i] = D3DXVECTOR4(d[0], d[1], d[2], 1.0f / (0.66f * g.radius));
        right[i] = D3DXVECTOR4(level ? kx / rlen : 0.0f, level ? ky / rlen : 0.0f, 0.0f, 0.0f);
        colour[i] = D3DXVECTOR4(g.colour[0], g.colour[1], g.colour[2], 0.0f);
    }

    ID3DXEffect* effect = R.HeadlightGlintsEffect;
    const float width = float(desc.Width), height = float(desc.Height);
    const D3DMATRIX& proj = *(const D3DMATRIX*)vp->mProjectionMatrix;
    const D3DXVECTOR4 projInfo = ProjInfo(proj, width, height);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4ProjInfo"), &projInfo);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fNearPlane"), vp->mNearClip);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fFarDivNear"), vp->mFarClip / vp->mNearClip);
    {
        D3DXVECTOR4 toView[3];
        WorldToViewRows(vp, toView);
        const D3DXVECTOR4 worldX(toView[0].x, toView[1].x, toView[2].x, viewInv.m[3][0]);
        const D3DXVECTOR4 worldY(toView[0].y, toView[1].y, toView[2].y, viewInv.m[3][1]);
        const D3DXVECTOR4 worldZ(toView[0].z, toView[1].z, toView[2].z, viewInv.m[3][2]);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4WorldX"), &worldX);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4WorldY"), &worldY);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4WorldZ"), &worldZ);
    }
    const float invSize[2] = { 1.0f / width, 1.0f / height };
    effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec2InvSize"), invSize, 2);
    const D3DXVECTOR4 glint(R.fLightsGGXGlints, R.fLightsGGXGlintsGloss, R.fLightsGGXGlintsSize, R.fLightsGGXGlintsMax);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Glint"), &glint);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fLightCount"), float(count));
    effect->SetVectorArray(effect->GetParameterByName(nullptr, "vec4LightPos"), position, kLights);
    effect->SetVectorArray(effect->GetParameterByName(nullptr, "vec4LightDir"), direction, kLights);
    effect->SetVectorArray(effect->GetParameterByName(nullptr, "vec4LightRight"), right, kLights);
    effect->SetVectorArray(effect->GetParameterByName(nullptr, "vec4LightColour"), colour, kLights);

    // Saved: the textures, sampler states, constants, shaders, render states and geometry the pass sets.
    static constexpr DWORD kSlots = 3;
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
    IDirect3DPixelShader9* oldPS = nullptr;
    IDirect3DVertexShader9* oldVS = nullptr;
    D3DVIEWPORT9 oldViewport = {};
    pDevice->GetPixelShader(&oldPS);
    pDevice->GetVertexShader(&oldVS);
    SavedVertexInput savedVertexInput(pDevice);
    pDevice->GetViewport(&oldViewport);

    static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kStates[] =
    {
        { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE },
        { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_COLORWRITEENABLE, 0x07 }, { D3DRS_SCISSORTESTENABLE, FALSE },
        { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
        { D3DRS_ALPHABLENDENABLE, TRUE }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_BLENDOP, D3DBLENDOP_ADD },
        { D3DRS_SRCBLEND, D3DBLEND_ONE }, { D3DRS_DESTBLEND, D3DBLEND_ONE },
    };
    DWORD savedStates[std::size(kStates)] = {};
    for (size_t i = 0; i < std::size(kStates); ++i)
    {
        pDevice->GetRenderState(kStates[i].state, &savedStates[i]);
        pDevice->SetRenderState(kStates[i].state, kStates[i].value);
    }
    D3DVIEWPORT9 viewport = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
    pDevice->SetViewport(&viewport);
    pDevice->SetVertexShader(nullptr);
    pDevice->SetFVF(kScreenVertexFVF);

    UINT passes = 0;
    effect->SetTechnique(effect->GetTechniqueByName("Glints"));
    effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
    effect->BeginPass(0);
    effect->CommitChanges();
    IDirect3DBaseTexture9* textures[kSlots] = { depth, R.mNormalRT->mD3DTexture, R.mSpecularRT->mD3DTexture };
    for (DWORD slot = 0; slot < kSlots; ++slot)
    {
        SetTextureBoth(pDevice, slot, textures[slot]);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        SetSamplerStateBoth(pDevice, slot, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    BindEffectConstantsOnly(pDevice, effect);
    const ScreenQuad quad = ScreenQuad::Full(width, height);
    quad.Draw(pDevice);
    effect->EndPass();
    effect->End();
    R.szGlintsStatus = "drawn";
    R.nGlintsLastLights = count;

    for (DWORD slot = 0; slot < kSlots; ++slot)
    {
        SetTextureBoth(pDevice, slot, oldTextures[slot]);
        SAFE_RELEASE(oldTextures[slot]);
        for (size_t i = 0; i < std::size(kSamplerStates); ++i)
            SetSamplerStateBoth(pDevice, slot, kSamplerStates[i], savedSamplerStates[slot][i]);
    }
    pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
    for (size_t i = 0; i < std::size(kStates); ++i)
        pDevice->SetRenderState(kStates[i].state, savedStates[i]);
    pDevice->SetViewport(&oldViewport);
    pDevice->SetPixelShader(oldPS);
    pDevice->SetVertexShader(oldVS);
    SAFE_RELEASE(oldPS);
    SAFE_RELEASE(oldVS);
}
