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

export module postfxpass;

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

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxProfiler;

// What the post fx passes share: the state they save and put back around their draws, the binding of their
// effects' samplers and constants, the camera parameters the screen space effects take, and their depth.
namespace PostFxPass
{
    export struct RenderStateValue { D3DRENDERSTATETYPE state; DWORD value; };
    export struct SamplerStateValue { D3DSAMPLERSTATETYPE state; DWORD value; };

    export constexpr RenderStateValue kSSRRenderStates[] =
    {
        { D3DRS_ZENABLE,          FALSE },
        { D3DRS_ZWRITEENABLE,     FALSE },
        { D3DRS_ALPHABLENDENABLE, FALSE },
        { D3DRS_ALPHATESTENABLE,  FALSE },
        { D3DRS_STENCILENABLE,    FALSE },
        { D3DRS_FOGENABLE,        FALSE },
        { D3DRS_CLIPPING,         FALSE },
        { D3DRS_CULLMODE,         D3DCULL_NONE },
        { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA },
    };

    export constexpr SamplerStateValue kSSRSamplerStates[] =
    {
        { D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP },
        { D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP },
        { D3DSAMP_MAGFILTER, D3DTEXF_POINT },
        { D3DSAMP_MINFILTER, D3DTEXF_POINT },
        { D3DSAMP_MIPFILTER, D3DTEXF_NONE },
        // Depths, normals and light: never sRGB, whatever the game's last draw left on the register
        { D3DSAMP_SRGBTEXTURE, FALSE },
    };
    export constexpr DWORD kSSRSamplerSlots = 8;
    export constexpr DWORD kSSRTextureSlots = 8;

    // The textures and the sampler states of the first kSSRTextureSlots samplers, saved on
    // construction and put back on destruction: deferred_lighting draws right after FusionFix's
    // passes and reads what the game bound there before them (the G-buffer normals on s1, and
    // more on s4). The AO pass left its own textures on s1 to s4 once GTAO used them.
    export struct SavedSamplerSlots
    {
        IDirect3DDevice9* device;
        IDirect3DBaseTexture9* textures[kSSRTextureSlots] = {};
        DWORD states[kSSRSamplerSlots][std::size(kSSRSamplerStates)] = {};

        explicit SavedSamplerSlots(IDirect3DDevice9* pDevice) : device(pDevice)
        {
            for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
                device->GetTexture(slot, &textures[slot]);
            for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
                for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                    device->GetSamplerState(slot, kSSRSamplerStates[i].state, &states[slot][i]);
        }

        ~SavedSamplerSlots()
        {
            for (DWORD slot = 0; slot < kSSRSamplerSlots; ++slot)
                for (size_t i = 0; i < std::size(kSSRSamplerStates); ++i)
                    device->SetSamplerState(slot, kSSRSamplerStates[i].state, states[slot][i]);
            for (DWORD slot = 0; slot < kSSRTextureSlots; ++slot)
            {
                SetTextureBoth(device, slot, textures[slot]);
                SAFE_RELEASE(textures[slot]);
            }
        }

        SavedSamplerSlots(const SavedSamplerSlots&) = delete;
        SavedSamplerSlots& operator=(const SavedSamplerSlots&) = delete;
    };

    // For effects begun with D3DXFX_DONOTSAVESTATE. D3DX's own state saving goes through state blocks on the game's
    // device wrapper, which drops a restore it takes for no change while the device behind it holds another value (see
    // SetTextureBoth): textures and shaders an effect had set stayed on the device. These take what the device itself
    // holds and put it back through the wrapper and on the device.
    export struct SavedShaders
    {
        IDirect3DDevice9* device;
        IDirect3DDevice9* real;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexShader9* vs = nullptr;

        explicit SavedShaders(IDirect3DDevice9* pDevice) : device(pDevice), real(RealDevice(pDevice))
        {
            real->GetPixelShader(&ps);
            real->GetVertexShader(&vs);
        }

        ~SavedShaders()
        {
            device->SetPixelShader(ps);
            device->SetVertexShader(vs);
            if (real != device)
            {
                real->SetPixelShader(ps);
                real->SetVertexShader(vs);
            }
            SAFE_RELEASE(ps);
            SAFE_RELEASE(vs);
        }

        SavedShaders(const SavedShaders&) = delete;
        SavedShaders& operator=(const SavedShaders&) = delete;
    };

    // The render states the passes of FusionFix's effects set (AO.fx's are the most)
    export struct SavedEffectPassStates
    {
        static constexpr D3DRENDERSTATETYPE kStates[] =
        {
            D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE,
            D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_CLIPPING, D3DRS_COLORWRITEENABLE,
        };
        IDirect3DDevice9* device;
        IDirect3DDevice9* real;
        DWORD values[std::size(kStates)] = {};

        explicit SavedEffectPassStates(IDirect3DDevice9* pDevice) : device(pDevice), real(RealDevice(pDevice))
        {
            for (size_t i = 0; i < std::size(kStates); ++i)
                real->GetRenderState(kStates[i], &values[i]);
        }

        ~SavedEffectPassStates()
        {
            for (size_t i = 0; i < std::size(kStates); ++i)
            {
                device->SetRenderState(kStates[i], values[i]);
                if (real != device)
                    real->SetRenderState(kStates[i], values[i]);
            }
        }

        SavedEffectPassStates(const SavedEffectPassStates&) = delete;
        SavedEffectPassStates& operator=(const SavedEffectPassStates&) = delete;
    };
    export constexpr UINT kPSConstCount = 224;
    export constexpr UINT kVSConstCount = 256;
    export float savedPSConsts[kPSConstCount * 4];
    export float savedVSConsts[kVSConstCount * 4];

    // The float shader constants, saved on construction and put back on destruction. An
    // effect writes its parameters into them, over what the game set for deferred_lighting,
    // which draws right after FusionFix's passes: once GTAO's accumulation took a dozen more,
    // the sun's went and it stopped lighting anything.
    export struct SavedShaderConstants
    {
        IDirect3DDevice9* device;
        std::vector<float> ps = std::vector<float>(kPSConstCount * 4);
        std::vector<float> vs = std::vector<float>(kVSConstCount * 4);

        explicit SavedShaderConstants(IDirect3DDevice9* pDevice) : device(pDevice)
        {
            device->GetPixelShaderConstantF(0, ps.data(), kPSConstCount);
            device->GetVertexShaderConstantF(0, vs.data(), kVSConstCount);
        }

        ~SavedShaderConstants()
        {
            device->SetPixelShaderConstantF(0, ps.data(), kPSConstCount);
            device->SetVertexShaderConstantF(0, vs.data(), kVSConstCount);
        }

        SavedShaderConstants(const SavedShaderConstants&) = delete;
        SavedShaderConstants& operator=(const SavedShaderConstants&) = delete;
    };

    export void MatrixMultiply(D3DXMATRIX& out, const D3DXMATRIX& a, const D3DXMATRIX& b)
    {
        D3DXMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j]
                          + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        out = r;
    }

    // Binds every sampler of the pixel shader of the pass just begun to the texture its effect
    // parameter holds, found through the shader's constant table (sampler X reads X2D in SSR.fx
    // and AO.fx). D3DX left some holding what the game had bound, about four a frame in the SSR
    // passes: a diagnostic pass read a G-buffer texture where it sampled the depth.
    // Which register takes which parameter is read once per effect and shader from the shader's
    // constant table and kept (EffectBindings), as the effects keep their shaders for the whole
    // game; cleared on a lost device. What the registers get is written for every draw, through the
    // game's device wrapper and the device both: SSR coming out empty in play, while it worked in the
    // pause menu, was the wrapper dropping textures it had on record (SetTextureBoth), not this map.

    // The same holds for the float constants: with the ones D3DX left, SSGI's accumulation decoded the right depth
    // (6.1 m) as 15.3 m through fNearPlane and fFarDivNear while vec2PrevDepthRange, in another register, came through.
    // Each float constant of the shader is written from its parameter, a register per vector or array element.
    export struct EffectConstant
    {
        UINT reg; UINT count; D3DXHANDLE param; std::string name; D3DXREGISTER_SET set;
        // From the parameter's description: a scalar or vector of floats, integers or booleans, and its elements
        bool usable = false;
        D3DXPARAMETER_TYPE type = D3DXPT_FLOAT;
        UINT columns = 0;
        std::vector<D3DXHANDLE> elements;
    };

    export struct EffectSampler
    {
        UINT reg; D3DXHANDLE param; std::string name;
        bool setStates = false;     // SSR.fx's samplers below kSSRSamplerSlots, whose states its callers save
        SamplerStates states{};
    };

    export struct EffectBinding
    {
        std::vector<EffectSampler> samplers;
        std::vector<EffectConstant> constants;
    };

    export std::map<std::pair<ID3DXEffect*, IDirect3DPixelShader9*>, EffectBinding>& EffectBindings()
    {
        static std::map<std::pair<ID3DXEffect*, IDirect3DPixelShader9*>, EffectBinding> bindings;
        return bindings;
    }

    export std::vector<EffectSampler> FindEffectSamplers(ID3DXEffect* effect, IDirect3DPixelShader9* ps,
                                                         std::vector<EffectConstant>* constants = nullptr)
    {
        std::vector<EffectSampler> samplers;
        std::vector<DWORD> function;
        UINT size = 0;
        if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size)
        {
            function.resize((size + 3) / 4);
            if (FAILED(ps->GetFunction(function.data(), &size)))
                function.clear();
        }
        // Large address aware, as the game is: without it D3DX takes a handle of a table above 2 GB for the name of a
        // constant and reads it as a string, which crashed in GetConstantDesc
        ID3DXConstantTable* table = nullptr;
        if (function.empty() || FAILED(D3DXGetShaderConstantTableEx(function.data(), D3DXCONSTTABLE_LARGEADDRESSAWARE, &table)) || !table)
            return samplers;

        D3DXCONSTANTTABLE_DESC tableDesc = {};
        table->GetDesc(&tableDesc);
        for (UINT i = 0; i < tableDesc.Constants; ++i)
        {
            D3DXCONSTANT_DESC desc = {};
            UINT count = 1;
            if (FAILED(table->GetConstantDesc(table->GetConstant(nullptr, i), &desc, &count)) || !desc.Name)
                continue;
            if (desc.RegisterSet != D3DXRS_SAMPLER && constants)
            {
                if (D3DXHANDLE param = effect->GetParameterByName(nullptr, desc.Name))
                    constants->push_back({ desc.RegisterIndex, desc.RegisterCount, param, desc.Name, desc.RegisterSet });
                continue;
            }
            if (desc.RegisterSet != D3DXRS_SAMPLER)
                continue;
            if (D3DXHANDLE param = effect->GetParameterByName(nullptr, (std::string(desc.Name) + "2D").c_str()))
                samplers.push_back({ desc.RegisterIndex, param, desc.Name });
        }
        table->Release();
        return samplers;
    }

    export void BindEffectConstants(IDirect3DDevice9* pDevice, ID3DXEffect* effect, const std::vector<EffectConstant>& constants)
    {
        std::string traced;
        for (const auto& c : constants)
        {
            if (!c.usable)
                continue;
            const UINT elements = UINT(c.elements.size());
            const UINT columns = c.columns;
            // Integers and booleans in their own register sets (loop counters, static branches)
            if (c.set == D3DXRS_INT4 || c.set == D3DXRS_BOOL)
            {
                int values[16 * 4] = {};
                for (UINT e = 0; e < elements; ++e)
                {
                    D3DXHANDLE h = c.elements[e];
                    if (c.type == D3DXPT_FLOAT)
                    {
                        float f[4] = {};
                        effect->GetFloatArray(h, f, columns);
                        for (UINT k = 0; k < columns; ++k)
                            values[e * 4 + k] = int(f[k]);
                    }
                    else
                        effect->GetIntArray(h, &values[e * 4], columns);
                }
                if (c.set == D3DXRS_INT4)
                {
                    pDevice->SetPixelShaderConstantI(c.reg, values, c.count);
                    if (auto real = RealDevice(pDevice); real != pDevice)
                        real->SetPixelShaderConstantI(c.reg, values, c.count);
                }
                else
                {
                    BOOL b[16] = {};
                    for (UINT e = 0; e < (std::min)(c.count, 16u); ++e)
                        b[e] = values[e * 4] != 0;
                    pDevice->SetPixelShaderConstantB(c.reg, b, c.count);
                    if (auto real = RealDevice(pDevice); real != pDevice)
                        real->SetPixelShaderConstantB(c.reg, b, c.count);
                }
                continue;
            }
            if (c.set != D3DXRS_FLOAT4)
                continue;
            float want[16 * 4] = {};
            for (UINT e = 0; e < elements; ++e)
            {
                D3DXHANDLE h = c.elements[e];
                if (c.type == D3DXPT_FLOAT)
                    effect->GetFloatArray(h, &want[e * 4], columns);
                else
                {
                    int v[4] = {};
                    effect->GetIntArray(h, v, columns);
                    for (UINT k = 0; k < columns; ++k)
                        want[e * 4 + k] = float(v[k]);
                }
            }
            float have[16 * 4] = {};
            RealDevice(pDevice)->GetPixelShaderConstantF(c.reg, have, c.count);
            bool differs = false;
            for (UINT e = 0; e < elements; ++e)
                for (UINT k = 0; k < columns; ++k)
                    differs |= want[e * 4 + k] != have[e * 4 + k];
            if (differs)
            {
                // The components the shader does not read keep what the register held
                for (UINT e = 0; e < c.count; ++e)
                    for (UINT k = (e < elements ? columns : 0u); k < 4; ++k)
                        want[e * 4 + k] = have[e * 4 + k];
                pDevice->SetPixelShaderConstantF(c.reg, want, c.count);
                if (auto real = RealDevice(pDevice); real != pDevice)
                    real->SetPixelShaderConstantF(c.reg, want, c.count);
                if (SSRTrace::Active())
                {
                    char item[160];
                    snprintf(item, sizeof(item), " c%u %s=%g (was %g)", c.reg, c.name.c_str(), want[0], have[0]);
                    traced += item;
                }
            }
        }
        if (SSRTrace::Active() && !traced.empty())
            SSRTrace::Line("  constants set:%s", traced.c_str());
    }

    // The registers of a shader of an effect and the parameters they take, read once
    export const EffectBinding& GetEffectBinding(ID3DXEffect* effect, IDirect3DPixelShader9* ps)
    {
        auto& bindings = EffectBindings();
        const auto key = std::make_pair(effect, ps);
        if (auto it = bindings.find(key); it != bindings.end())
            return it->second;

        EffectBinding binding;
        binding.samplers = FindEffectSamplers(effect, ps, &binding.constants);
        for (auto& c : binding.constants)
        {
            D3DXPARAMETER_DESC pd = {};
            c.usable = SUCCEEDED(effect->GetParameterDesc(c.param, &pd)) &&
                (pd.Type == D3DXPT_FLOAT || pd.Type == D3DXPT_INT || pd.Type == D3DXPT_BOOL) &&
                (pd.Class == D3DXPC_SCALAR || pd.Class == D3DXPC_VECTOR) && c.count > 0 && c.count <= 16;
            if (!c.usable)
                continue;
            c.type = pd.Type;
            c.columns = (std::min)(pd.Columns, 4u);
            const UINT elements = pd.Elements ? (std::min)(pd.Elements, c.count) : 1;
            for (UINT e = 0; e < elements; ++e)
                c.elements.push_back(pd.Elements ? effect->GetParameterElement(c.param, e) : c.param);
        }
        if (effect == PostFxResources.SSREffect)
        {
            for (auto& sampler : binding.samplers)
            {
                if (sampler.reg >= kSSRSamplerSlots)
                    continue;
                auto found = SSRSamplerStates.find(sampler.name);
                sampler.setStates = true;
                sampler.states = found != SSRSamplerStates.end() ? found->second
                    : SamplerStates{ D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP };
            }
        }
        // A shader whose table could not be read is tried again next time
        if (binding.samplers.empty() && binding.constants.empty())
        {
            static EffectBinding none;
            return none;
        }
        return bindings.emplace(key, std::move(binding)).first->second;
    }

    // Writes the textures (and the float, int and bool constants) of the bound pixel shader from the effect's parameters,
    // and for SSR.fx, whose callers save samplers 0 to kSSRSamplerSlots - 1 around their passes, the sampler states.
    export void BindEffectConstantsOnly(IDirect3DDevice9* pDevice, ID3DXEffect* effect)
    {
        IDirect3DPixelShader9* ps = nullptr;
        if (FAILED(pDevice->GetPixelShader(&ps)) || !ps)
            return;
        const auto& binding = GetEffectBinding(effect, ps);
        ps->Release();
        BindEffectConstants(pDevice, effect, binding.constants);
    }

    export void BindEffectSamplers(IDirect3DDevice9* pDevice, ID3DXEffect* effect)
    {
        IDirect3DPixelShader9* ps = nullptr;
        if (FAILED(pDevice->GetPixelShader(&ps)) || !ps)
            return;
        const auto& binding = GetEffectBinding(effect, ps);
        const auto& samplers = binding.samplers;
        BindEffectConstants(pDevice, effect, binding.constants);
        const void* shader = ps;
        ps->Release();

        std::string traced;
        for (const auto& sampler : samplers)
        {
            const UINT reg = sampler.reg;
            const D3DXHANDLE param = sampler.param;
            if (sampler.setStates)
            {
                const SamplerStates& st = sampler.states;
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_MINFILTER, st[0]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_MAGFILTER, st[1]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_MIPFILTER, st[2]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_ADDRESSU, st[3]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_ADDRESSV, st[4]);
                SetSamplerStateBoth(pDevice, reg, D3DSAMP_SRGBTEXTURE, FALSE);
            }
            IDirect3DBaseTexture9* want = nullptr;
            IDirect3DBaseTexture9* have = nullptr;
            effect->GetTexture(param, &want);
            if (SSRTrace::Active())
                RealDevice(pDevice)->GetTexture(reg, &have);
            // Always, through both: the wrapper may hold want on record while the device has another
            SetTextureBoth(pDevice, reg, want);
            if (SSRTrace::Active())
            {
                D3DXPARAMETER_DESC desc = {};
                effect->GetParameterDesc(param, &desc);
                DWORD minFilter = 0, srgb = 0;
                RealDevice(pDevice)->GetSamplerState(reg, D3DSAMP_MINFILTER, &minFilter);
                RealDevice(pDevice)->GetSamplerState(reg, D3DSAMP_SRGBTEXTURE, &srgb);
                traced += " s" + std::to_string(reg) + "=" + (desc.Name ? desc.Name : "?") + ":" + SSRTrace::TextureName(want) +
                    (want != have ? "(was " + SSRTrace::TextureName(have) + ")" : "") + (minFilter == D3DTEXF_LINEAR ? "/lin" : "/pt") + (srgb ? "/SRGB" : "");
            }
            SAFE_RELEASE(want);
            SAFE_RELEASE(have);
        }
        if (SSRTrace::Active())
            SSRTrace::Line("  bind ps %p, %u samplers:%s", shader, unsigned(samplers.size()), traced.c_str());
    }

    // Where the passes of SSR.fx read their noise this frame: moved on every frame while they accumulate, so the
    // accumulation averages it out, else the same every frame.
    export void SetNoiseOffset(ID3DXEffect* effect, bool accumulating)
    {
        auto offset = accumulating ? FrameHistory::NoiseOffset() : std::array<float, 2>{};
        effect->SetFloatArray(PostFxResources.SSREffectHandles.vec2NoiseOffset, offset.data(), 2);
    }

    // The accumulation passes of SSR.fx follow temporal AA's motion vectors, when it drew them this frame and there is
    // a history to take from them (see TemporalHistoryUV), else the camera alone.
    export void BindMotionVectors(ID3DXEffect* effect, bool history)
    {
        auto& h = PostFxResources.SSREffectHandles;
        auto motion = history ? FrameHistory::MotionVectors() : nullptr;
        auto jitter = FrameHistory::JitterDeltaUV();
        effect->SetTexture(h.MotionTex2D, motion);
        effect->SetFloat(h.fUseMotion, motion ? 1.0f : 0.0f);
        effect->SetFloatArray(h.vec2MotionJitter, jitter.data(), 2);
    }

    // The camera parameters the screen space effects share, under the same names in each effect.

    // View space from depth for a width x height target: the scale and offset that turn a pixel
    // into its view ray.
    export D3DXVECTOR4 ProjInfo(const D3DMATRIX& proj, float width, float height)
    {
        return D3DXVECTOR4(-2.0f / (width * proj._11), -2.0f / (height * proj._22),
                           (1.0f - proj._31) / proj._11, (1.0f + proj._32) / proj._22);
    }

    // vec2InvViewportSize and vec4ProjInfo: pixel size and reconstruction basis for a width x
    // height target.
    export template <typename Handles>
    void SetTargetSize(ID3DXEffect* effect, const Handles& h, const D3DMATRIX& proj, float width, float height)
    {
        const float invViewportSize[] = { 1.0f / width, 1.0f / height };
        effect->SetFloatArray(h.vec2InvViewportSize, invViewportSize, 2);
        const auto projInfo = ProjInfo(proj, width, height);
        effect->SetVector(h.vec4ProjInfo, &projInfo);
    }

    // fNearPlane and fFarDivNear, for linear depth from the depth buffer.
    export template <typename Handles>
    void SetDepthRange(ID3DXEffect* effect, const Handles& h, float nearClip, float farClip)
    {
        effect->SetFloat(h.fNearPlane, nearClip);
        effect->SetFloat(h.fFarDivNear, farClip / nearClip);
    }

    // vec2PrevDepthRange: fNearPlane and fFarDivNear of the scene PrevDepthTex was drawn in, the one
    // before this. The game moves the near plane as the camera closes in on a wall, and last frame's
    // depth decoded with this frame's planes failed every history's depth test.
    export template <typename Handles>
    void SetPrevDepthRange(ID3DXEffect* effect, const Handles& h)
    {
        const auto& prev = FrameHistory::Previous();
        const auto& camera = prev.Valid ? prev : FrameHistory::Current();
        const float range[] = { camera.Near, camera.Near > 0.0f ? camera.Far / camera.Near : 1.0f };
        effect->SetFloatArray(h.vec2PrevDepthRange, range, 2);
    }

    // World to reconstruction space rotation rows: the view's axes with x (and z, when _34 is
    // negative) flipped, as ViewToClipRows below.
    export void WorldToViewRows(const rage::grcViewport* vp, D3DXVECTOR4 rows[3])
    {
        const auto& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
        const float axisSign[3] = { -1.0f, 1.0f, (((const D3DMATRIX*)vp->mProjectionMatrix)->_34 < 0.0f) ? -1.0f : 1.0f };
        for (int row = 0; row < 3; ++row)
            rows[row] = D3DXVECTOR4(viewInv.m[row][0] * axisSign[row], viewInv.m[row][1] * axisSign[row],
                                    viewInv.m[row][2] * axisSign[row], 0.0f);
    }

    // vec4ViewToPrevClip: from SSR.fx's reconstruction space, whose x (and z, when _34 is
    // negative) run opposite to the game's view space, to viewProj's clip space.
    export void ViewToClipRows(const rage::grcViewport* vp, const D3DXMATRIX& viewProj, D3DXVECTOR4 rows[4])
    {
        D3DXMATRIX m;
        MatrixMultiply(m, *(const D3DXMATRIX*)vp->mViewInverseMatrix, viewProj);
        const float axisSign[4] = { -1.0f, 1.0f, (((const D3DMATRIX*)vp->mProjectionMatrix)->_34 < 0.0f) ? -1.0f : 1.0f, 1.0f };
        for (int row = 0; row < 4; ++row)
        {
            float s = axisSign[row];
            rows[row] = D3DXVECTOR4(m.m[row][0] * s, m.m[row][1] * s, m.m[row][2] * s, m.m[row][3] * s);
        }
    }

    // The depth the passes of the lighting phase read: the copy from the end of this frame's G-buffer pass, or
    // _DEFERRED_GBUFFER_3_ where there is none.
    export IDirect3DTexture9* LightingDepth()
    {
        auto& R = PostFxResources;
        if (R.SceneDepthTex && R.SceneDepthTex->mD3DTexture && R.nSceneDepthFrame && R.nSceneDepthFrame == FrameHistory::Frame())
            return R.SceneDepthTex->mD3DTexture;
        return R.mDepthRT ? R.mDepthRT->mD3DTexture : nullptr;
    }

    // Draws one full screen quad of the SSR effect's technique into target, then restores the
    // device state it touched. The draw is timed as profile, see ProfilerSection.
    export void DrawEffectPass(IDirect3DDevice9* pDevice, ID3DXEffect* effect, D3DXHANDLE technique, IDirect3DSurface9* target, float width, float height,
                               int profile = -1)
    {
        const ScreenQuad screenQuad = ScreenQuad::Full(width, height);
        pDevice->SetRenderTarget(0, target);

        // No state saving by D3DX (see SavedShaders): the callers save and restore textures, samplers, render states
        // and constants themselves, the shaders, which the effect sets, are put back here.
        SavedShaders shaders(pDevice);

        UINT passes = 0;
        effect->SetTechnique(technique);
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        BindEffectSamplers(pDevice, effect);
        if (SSRTrace::Active())
        {
            // The viewport the device holds against the target's size and the quad's: a quad or viewport of another
            // size than the target draws part of it, and the passes rebuild positions on the wrong grid.
            D3DXTECHNIQUE_DESC tech = {};
            effect->GetTechniqueDesc(technique, &tech);
            D3DSURFACE_DESC desc = {};
            if (target)
                target->GetDesc(&desc);
            D3DVIEWPORT9 view = {};
            pDevice->GetViewport(&view);
            SSRTrace::Line("  draw %s: target %ux%u, viewport %ux%u at %u,%u, quad %.0fx%.0f", tech.Name ? tech.Name : "?",
                desc.Width, desc.Height, unsigned(view.Width), unsigned(view.Height), unsigned(view.X), unsigned(view.Y), width, height);
        }
        ProfilerScope timed(pDevice, profile);
        screenQuad.Draw(pDevice);
        effect->EndPass();
        effect->End();
    }
}
