module;

#include <common.hxx>
#include <d3dx9.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include "FusionLog.hpp"

export module waterfarlayer;

import common;
import comvars;
import d3dx9_43;
import inireload;
import postfxresources;
import renderskip;
import entityclass;
import framehistory;
import screenquad;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

#define IDR_WATERFARLAYER_FX 144

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;

// Water Far Layer ([PERFORMANCE] WaterFarLayer, 1, live; hidden WaterFarLayerDistance 400 m): the far part of the
// water's reflection drawn now and then into a layer of its own, and laid into the reflection from it every frame.
// The reflection is mostly the far city (a capture on a river bank: 2228 device draws, 85% of them past 400 m), and
// its pass cost 2.4 ms of the render thread on the frames it was drawn.
//
// In the reflection's pass (WATER_REFLECTION_COLOUR, not a frame RenderSkip leaves out) each entity command
// (EntityClass::SetCommandHook) further than WaterFarLayerDistance from the camera (less its model's radius) is left
// out, except on a refresh: then it is drawn into the layer (A16B16G16R16F and an INTZ depth of the reflection's size,
// on the real device) instead of the reflection, with the reflection's own camera, states and clip plane. A refresh
// comes when the layer is older than kRefreshSeconds, the reflection's camera moved kRefreshMove or turned more than
// kRefreshTurnCos, or its projection changed. As the reflection's target is unlocked (RenderSkip::pBeforeWaterUnlock)
// the layer is laid in (WaterFarLayer.fx): each pixel's direction taken into the layer's view (far off only the
// direction counts), only where the reflection's depth is still the far plane (sky and clouds write none). Turned
// since the layer was drawn, its edge has nothing: the sky shows there until the next refresh.
//
// The reflection's view has the mirror in it (its view inverse's position is (x, -z, -y) of the mirrored eye): all of
// this works in its own space, distances and turns are the same in it. GpuGrass-style log: [DEBUG] WaterFarLayerLog.
namespace WaterFarLayer
{
    constexpr double kRefreshSeconds = 2.0;
    constexpr float kRefreshMove = 10.0f;           // metres
    constexpr float kRefreshTurnCos = 0.99939f;     // 2 degrees

    bool bEnabled = true;
    bool bLog = false;
    float fDistance = 400.0f;

    IDirect3DTexture9* layerTexture = nullptr;
    IDirect3DSurface9* layerSurface = nullptr;
    IDirect3DTexture9* layerDepthTexture = nullptr;
    IDirect3DSurface9* layerDepthSurface = nullptr;
    UINT layerWidth = 0, layerHeight = 0;
    bool resourcesFailed = false;

    ID3DXEffect* effect = nullptr;
    bool effectTried = false;
    D3DXHANDLE hTechnique = nullptr, hReproject = nullptr, hSize = nullptr;

    // This pass (render thread): begun at its first command, ended as its target is unlocked
    enum class Pass : uint8_t { NotBegun, Active, Failed };
    Pass pass = Pass::NotBegun;
    bool inPass = false, refresh = false, layerBound = false, layerValid = false;
    IDirect3DSurface9* planarTarget = nullptr;
    IDirect3DSurface9* planarDepth = nullptr;

    // The camera the layer was drawn with: the reflection's view and projection
    float layerView[4][4] = {};
    float layerViewInverse[4][4] = {};
    float layerProjection[4][4] = {};
    std::chrono::steady_clock::time_point layerTime{};

    // WaterFarLayerLog, every 600 passes
    uint32_t logPasses = 0, logRefreshes = 0, logComposites = 0;
    uint64_t logNear = 0, logFarLeftOut = 0, logFarDrawn = 0;

    void ReadIni(CIniReader& iniReader)
    {
        bEnabled = iniReader.ReadInteger("PERFORMANCE", "WaterFarLayer", 1) != 0;
        fDistance = std::clamp(iniReader.ReadFloat("PERFORMANCE", "WaterFarLayerDistance", 400.0f), 50.0f, 5000.0f);
        bLog = iniReader.ReadInteger("DEBUG", "WaterFarLayerLog", 0) != 0;
        // A layer from before may not match what is set now
        layerValid = false;
    }

    void ReleaseLayer()
    {
        SAFE_RELEASE(layerSurface);
        SAFE_RELEASE(layerTexture);
        SAFE_RELEASE(layerDepthSurface);
        SAFE_RELEASE(layerDepthTexture);
        layerWidth = layerHeight = 0;
        layerValid = false;
    }

    // The layer at the reflection's size, on the real device (the game's wrapper keeps books of what it makes)
    bool MakeLayer(IDirect3DDevice9* real, UINT width, UINT height)
    {
        if (layerTexture && layerWidth == width && layerHeight == height)
            return true;
        ReleaseLayer();
        if (resourcesFailed)
            return false;
        const bool made =
            SUCCEEDED(real->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &layerTexture, nullptr)) &&
            SUCCEEDED(layerTexture->GetSurfaceLevel(0, &layerSurface)) &&
            SUCCEEDED(real->CreateTexture(width, height, 1, D3DUSAGE_DEPTHSTENCIL, D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT,
                &layerDepthTexture, nullptr)) &&
            SUCCEEDED(layerDepthTexture->GetSurfaceLevel(0, &layerDepthSurface));
        if (!made)
        {
            ReleaseLayer();
            resourcesFailed = true;
            FusionLog::Write("WaterFarLayer", "Resources", "the layer could not be made (%ux%u, A16B16G16R16F and INTZ)\n", width, height);
            return false;
        }
        layerWidth = width;
        layerHeight = height;
        return true;
    }

    bool MakeEffect(IDirect3DDevice9* pDevice)
    {
        if (!effect && !effectTried)
        {
            effectTried = true;
            HMODULE hm = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&MakeEffect), &hm);
            ID3DXBuffer* errors = nullptr;
            if (PostFxResource::CreateEffectCached(pDevice, hm, IDR_WATERFARLAYER_FX, "WaterFarLayer", nullptr, &effect, &errors) != S_OK)
            {
                effect = nullptr;
                FusionLog::Write("WaterFarLayer", "Effect", "WaterFarLayer.fx failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no error text");
            }
            SAFE_RELEASE(errors);
            if (effect)
            {
                hTechnique = effect->GetTechniqueByName("Composite");
                hReproject = effect->GetParameterByName(nullptr, "matReproject");
                hSize = effect->GetParameterByName(nullptr, "vec4Size");
            }
        }
        return effect != nullptr;
    }

    void BindLayer(IDirect3DDevice9* real)
    {
        real->SetRenderTarget(0, layerSurface);
        real->SetDepthStencilSurface(layerDepthSurface);
        layerBound = true;
    }

    void BindPlanar(IDirect3DDevice9* real)
    {
        real->SetRenderTarget(0, planarTarget);
        real->SetDepthStencilSurface(planarDepth);
        layerBound = false;
    }

    // The first command of the reflection's pass: its targets, whether the layer is drawn anew. Whether it is on is
    // taken here, once a pass, so a change of the ini meanwhile leaves no target bound
    bool BeginPass(IDirect3DDevice9* pDevice, IDirect3DDevice9* real)
    {
        if (pass != Pass::NotBegun)
            return pass == Pass::Active;
        pass = Pass::Failed;
        inPass = false;
        auto viewport = rage::GetCurrentViewport();
        if (!bEnabled || !viewport || !MakeEffect(pDevice))
            return false;
        SAFE_RELEASE(planarTarget);
        SAFE_RELEASE(planarDepth);
        real->GetRenderTarget(0, &planarTarget);
        real->GetDepthStencilSurface(&planarDepth);
        D3DSURFACE_DESC desc{};
        if (!planarTarget || !planarDepth || FAILED(planarTarget->GetDesc(&desc)) || !MakeLayer(real, desc.Width, desc.Height))
        {
            SAFE_RELEASE(planarTarget);
            SAFE_RELEASE(planarDepth);
            return false;
        }
        // Moved, turned, zoomed, or old: drawn anew
        const auto now = std::chrono::steady_clock::now();
        const float* eye = viewport->mViewInverseMatrix[3];
        const float* back = viewport->mViewInverseMatrix[2];
        const float dx = eye[0] - layerViewInverse[3][0], dy = eye[1] - layerViewInverse[3][1], dz = eye[2] - layerViewInverse[3][2];
        const float turn = back[0] * layerViewInverse[2][0] + back[1] * layerViewInverse[2][1] + back[2] * layerViewInverse[2][2];
        const float zoom = std::abs(viewport->mProjectionMatrix[0][0] - layerProjection[0][0]) +
                           std::abs(viewport->mProjectionMatrix[1][1] - layerProjection[1][1]);
        refresh = !layerValid || std::chrono::duration<double>(now - layerTime).count() >= kRefreshSeconds ||
                  dx * dx + dy * dy + dz * dz > kRefreshMove * kRefreshMove || turn < kRefreshTurnCos || zoom > 0.01f * layerProjection[1][1];
        if (refresh)
        {
            std::memcpy(layerView, viewport->mViewMatrix, sizeof(layerView));
            std::memcpy(layerViewInverse, viewport->mViewInverseMatrix, sizeof(layerViewInverse));
            std::memcpy(layerProjection, viewport->mProjectionMatrix, sizeof(layerProjection));
            layerTime = now;
            BindLayer(real);
            real->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
            BindPlanar(real);
            ++logRefreshes;
        }
        inPass = true;
        pass = Pass::Active;
        ++logPasses;
        return true;
    }

    // EntityClass::SetCommandHook: near ones as the game draws them, far ones into the layer on a refresh, else left out
    bool OnCommand(EntityClass::Kind, uint16_t model, const float* place)
    {
        if (RenderSkip::DropsDraws())
            return true;
        const char* target = RenderSkip::CurrentTarget();
        if (!target || std::strcmp(target, "WATER_REFLECTION_COLOUR"))
            return true;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        auto real = pDevice ? RealDevice(pDevice) : nullptr;
        if (!real || !BeginPass(pDevice, real))
            return true;
        const auto& camera = FrameHistory::Current();
        if (!camera.Valid)
            return true;
        // A rigid view: its position is -t R^T
        const auto& v = camera.View;
        const float eye[3] = {
            -(v._41 * v._11 + v._42 * v._12 + v._43 * v._13),
            -(v._41 * v._21 + v._42 * v._22 + v._43 * v._23),
            -(v._41 * v._31 + v._42 * v._32 + v._43 * v._33),
        };
        const auto info = EntityClass::ModelInfo(model);
        const float radius = info ? *reinterpret_cast<const float*>(info + 0x1c) : 0.0f;
        const float dx = place[0] - eye[0], dy = place[1] - eye[1], dz = place[2] - eye[2];
        const bool farOff = std::sqrt(dx * dx + dy * dy + dz * dz) - radius > fDistance;
        if (!farOff)
        {
            if (layerBound)
                BindPlanar(real);
            ++logNear;
            return true;
        }
        if (!refresh)
        {
            ++logFarLeftOut;
            return false;
        }
        if (!layerBound)
            BindLayer(real);
        ++logFarDrawn;
        return true;
    }

    // The layer into the reflection, where it drew nothing, before the target is resolved
    void Composite(IDirect3DDevice9* pDevice, IDirect3DDevice9* real)
    {
        auto viewport = rage::GetCurrentViewport();
        if (!viewport || !effect || !hTechnique)
            return;
        // A direction in the reflection's view now -> its space (the turn of the view inverse) -> the layer's view -> its clip
        D3DXMATRIX nowInverse, layerTurn, projection, reproject;
        std::memcpy(&nowInverse, viewport->mViewInverseMatrix, sizeof(nowInverse));
        std::memcpy(&layerTurn, layerView, sizeof(layerTurn));
        std::memcpy(&projection, layerProjection, sizeof(projection));
        nowInverse._41 = nowInverse._42 = nowInverse._43 = 0.0f;
        layerTurn._41 = layerTurn._42 = layerTurn._43 = 0.0f;
        reproject = nowInverse * layerTurn * projection;
        const D3DXVECTOR4 size(1.0f / float(layerWidth), 1.0f / float(layerHeight), 1.0f / viewport->mProjectionMatrix[0][0],
                               1.0f / viewport->mProjectionMatrix[1][1]);

        static float psConstants[224 * 4];
        pDevice->GetPixelShaderConstantF(0, psConstants, 224);
        static constexpr D3DRENDERSTATETYPE kStates[] = { D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
            D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE,
            D3DRS_FOGENABLE, D3DRS_FILLMODE };
        static constexpr DWORD kValues[] = { TRUE, D3DCMP_LESSEQUAL, FALSE, FALSE, FALSE, D3DCULL_NONE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE, 0, FALSE, FALSE, FALSE, FALSE, D3DFILL_SOLID };
        DWORD saved[std::size(kStates)] = {};
        for (size_t i = 0; i < std::size(kStates); ++i)
        {
            pDevice->GetRenderState(kStates[i], &saved[i]);
            pDevice->SetRenderState(kStates[i], kValues[i]);
        }
        static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
            D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
        static constexpr DWORD kSamplerValues[] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, FALSE };
        IDirect3DBaseTexture9* oldTexture[2] = {};
        DWORD oldSampler[2][std::size(kSamplerStates)] = {};
        for (DWORD s = 0; s < 2; ++s)
        {
            pDevice->GetTexture(s, &oldTexture[s]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                pDevice->GetSamplerState(s, kSamplerStates[i], &oldSampler[s][i]);
        }
        {
            SavedVertexInput vertexInput(pDevice);
            pDevice->SetVertexShader(nullptr);
            pDevice->SetFVF(kScreenVertexFVF);
            effect->SetMatrix(hReproject, &reproject);
            effect->SetVector(hSize, &size);
            UINT passes = 0;
            effect->SetTechnique(hTechnique);
            effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
            effect->BeginPass(0);
            effect->CommitChanges();
            IDirect3DBaseTexture9* textures[2] = { layerTexture, layerDepthTexture };
            for (DWORD s = 0; s < 2; ++s)
            {
                SetTextureBoth(pDevice, s, textures[s]);
                for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                    SetSamplerStateBoth(pDevice, s, kSamplerStates[i], kSamplerValues[i]);
            }
            // At the far plane: drawn only where the reflection's depth is still 1 (nothing nearer drawn there)
            ScreenQuad::Rect(0.0f, 0.0f, float(layerWidth), float(layerHeight), 1.0f).Draw(pDevice);
            effect->EndPass();
            effect->End();
        }
        for (DWORD s = 0; s < 2; ++s)
        {
            SetTextureBoth(pDevice, s, oldTexture[s]);
            SAFE_RELEASE(oldTexture[s]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                SetSamplerStateBoth(pDevice, s, kSamplerStates[i], oldSampler[s][i]);
        }
        for (size_t i = 0; i < std::size(kStates); ++i)
            pDevice->SetRenderState(kStates[i], saved[i]);
        pDevice->SetPixelShaderConstantF(0, psConstants, 224);
        ++logComposites;
    }

    // RenderSkip::pBeforeWaterUnlock
    void OnWaterUnlock(bool leftOut)
    {
        pass = Pass::NotBegun;
        if (!inPass)
            return;
        inPass = false;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        auto real = pDevice ? RealDevice(pDevice) : nullptr;
        if (real)
        {
            if (layerBound)
                BindPlanar(real);
            if (refresh)
                layerValid = true;
            if (!leftOut && layerValid)
                Composite(pDevice, real);
        }
        SAFE_RELEASE(planarTarget);
        SAFE_RELEASE(planarDepth);
        if (bLog && logPasses >= 600)
        {
            FusionLog::Write("WaterFarLayer", "Stats", "%u passes, %u refreshes, %u composites; per pass: near %.1f, far left out %.1f, far drawn into the layer %.1f "
                "(commands); distance %.0f m\n", logPasses, logRefreshes, logComposites, double(logNear) / logPasses, double(logFarLeftOut) / logPasses,
                double(logFarDrawn) / logPasses, fDistance);
            logPasses = logRefreshes = logComposites = 0;
            logNear = logFarLeftOut = logFarDrawn = 0;
        }
    }
}

export namespace WaterFarLayer
{
    // Before a device reset: the layer is in the default pool
    void OnDeviceLost()
    {
        ReleaseLayer();
        SAFE_RELEASE(planarTarget);
        SAFE_RELEASE(planarDepth);
        inPass = false;
        pass = Pass::NotBegun;
        if (effect)
            effect->OnLostDevice();
    }

    void OnDeviceReset()
    {
        if (effect)
            effect->OnResetDevice();
    }
}

class WaterFarLayerInit
{
public:
    WaterFarLayerInit()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            WaterFarLayer::ReadIni(iniReader);
            IniReload::Add(WaterFarLayer::ReadIni);
        };
        FusionFix::onInitEvent() += []()
        {
            EntityClass::SetCommandHook(&WaterFarLayer::OnCommand);
            RenderSkip::pBeforeWaterUnlock = &WaterFarLayer::OnWaterUnlock;
        };
    }
} WaterFarLayerInit;
