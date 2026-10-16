module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

export module impostors;

import common;
import comvars;
import entityclass;
import settings;
import temporal;

// Impostors for the far map, step one: baking. The plan is to draw the map's far LOD models (800 m and more out, where
// one model is mostly 32-128 px and 3-5 ms of the G-buffer's CPU time goes) as one instanced draw of quads that read an
// atlas, but first the atlas has to be filled with what the game's own shaders write into the G-buffer.
//
// How a model is baked: right after its draw command (CDrawEntityDC) is executed in the scene's G-buffer
// (EntityClass::SetGBufferEntityHook), with every state of that draw still set, the command is executed again
// kViews x kViews times into a tile of the atlas: the atlas's layers in place of the G-buffer's targets (as many as
// the game had bound, in their formats), its own depth buffer, a kFrame px viewport per view, and the current
// viewport (rage::GetCurrentViewport) turned into a narrow perspective camera looking at the model's sphere from that
// view's direction. The game recomputes the world-view-projection from the viewport for every entity it draws
// (grcViewport's transform update, which also sends our z-fighting fix's log depth constants from its near and far
// clip), so the command draws from our camera. The viewport is copied back whole afterwards, and the targets, depth
// buffer and device viewport put back on the real device as they were. The frustum planes are opened up meanwhile in
// case the drawable culls its geometries against them.
//
// The views are hemi-octahedral over the upper hemisphere (world z up): the view (i, j) looks from the direction
// decoded from ((i + 0.5) / n, (j + 0.5) / n). A camera far off (kCameraDistance spheres) with the field of view just
// covering the sphere keeps the perspective close to orthographic and gives the log depth something to work with.
//
// ImpostorDebug (Graphics > Debug, [DEBUG] ImpostorDebug): bakes one far model a frame into the free tiles of a
// kAtlas px atlas (kSlots of them), then, once they are all used, one a second over the oldest, and shows the albedo
// layer and the normal layer side by side in the top left corner of the frame. A log of the first bakes goes to
// GTAIV.EFLC.FusionFix.Impostors.log.
export namespace Impostors
{
    // Called by PostFX over the finished frame
    void DrawDebugOverlay();
}

namespace
{
    constexpr int kViews = 4;
    constexpr int kFrame = 128;
    constexpr int kTile = kViews * kFrame;
    constexpr int kAtlas = 2048;
    constexpr int kTilesAcross = kAtlas / kTile;
    constexpr int kSlots = kTilesAcross * kTilesAcross;
    constexpr int kLayers = 4;
    constexpr float kFrom = 800.0f;
    constexpr float kCameraDistance = 6.0f;
    constexpr uint32_t kReplaceFrames = 60;

    struct Slot
    {
        bool used = false;
        uint16_t model = 0;
        float place[3]{};
        float reach = 0.0f;
        uint32_t bakedFrame = 0;
    };
    Slot slots[kSlots];

    IDirect3DTexture9* layers[kLayers] = {};
    IDirect3DSurface9* layerSurfaces[kLayers] = {};
    IDirect3DSurface9* depth = nullptr;
    D3DFORMAT layerFormats[kLayers] = {};
    int layerCount = 0;
    bool bFailed = false;
    uint32_t frame = 0, lastBakeFrame = ~0u, lastReplaceFrame = 0;
    int logged = 0;

    bool DebugOn()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_IMPOSTOR_DEBUG");
        return pref && pref->get() != 0;
    }

    void Release()
    {
        for (int i = 0; i < kLayers; ++i)
        {
            SAFE_RELEASE(layerSurfaces[i]);
            SAFE_RELEASE(layers[i]);
        }
        SAFE_RELEASE(depth);
        layerCount = 0;
        for (auto& slot : slots)
            slot = {};
    }

    // The atlas in the formats of the targets the game has bound for the G-buffer
    bool Create(IDirect3DDevice9* device, IDirect3DSurface9* const (&bound)[kLayers])
    {
        if (layerCount)
            return true;
        if (bFailed)
            return false;
        int count = 0;
        for (int i = 0; i < kLayers && bound[i]; ++i, ++count)
        {
            D3DSURFACE_DESC desc{};
            bound[i]->GetDesc(&desc);
            layerFormats[i] = desc.Format;
            if (FAILED(device->CreateTexture(kAtlas, kAtlas, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &layers[i], nullptr)) ||
                FAILED(layers[i]->GetSurfaceLevel(0, &layerSurfaces[i])))
            {
                FusionLog::Write("Impostors", "Atlas", "layer %d (format %u) could not be made\n", i, unsigned(desc.Format));
                Release();
                bFailed = true;
                return false;
            }
        }
        if (!count || FAILED(device->CreateDepthStencilSurface(kAtlas, kAtlas, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth, nullptr)))
        {
            FusionLog::Write("Impostors", "Atlas", "no targets bound (%d) or no depth buffer\n", count);
            Release();
            bFailed = true;
            return false;
        }
        layerCount = count;
        FusionLog::Write("Impostors", "Atlas", "%d px, %d layers in the G-buffer's formats %u %u %u %u, %d tiles of %dx%d views of %d px\n",
            kAtlas, count, unsigned(layerFormats[0]), unsigned(layerFormats[1]), unsigned(layerFormats[2]), unsigned(layerFormats[3]),
            kSlots, kViews, kViews, kFrame);
        return true;
    }

    // Row-vector matrices as the viewport keeps them
    void SetIdentity(float (&m)[4][4])
    {
        std::memset(m, 0, sizeof(m));
        m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
    }

    void Normalize(float (&v)[3])
    {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length > 0.0f)
            v[0] /= length, v[1] /= length, v[2] /= length;
    }

    void Cross(const float (&a)[3], const float (&b)[3], float (&out)[3])
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    // The direction of view (i, j), from the model towards the camera
    void ViewDirection(int i, int j, float (&d)[3])
    {
        const float u = (float(i) + 0.5f) / float(kViews) * 2.0f - 1.0f;
        const float v = (float(j) + 0.5f) / float(kViews) * 2.0f - 1.0f;
        d[0] = (u + v) * 0.5f;
        d[1] = (u - v) * 0.5f;
        d[2] = 1.0f - std::fabs(d[0]) - std::fabs(d[1]);
        Normalize(d);
    }

    // The game's camera is right-handed and looks down its -z (the projection's [2][3] is -1); its depth range is
    // either the usual one or inverted, taken from the viewport's own projection
    void SetCamera(rage::grcViewport& viewport, const float (&centre)[3], float reach, const float (&d)[3], bool invertedZ)
    {
        const float distance = reach * kCameraDistance;
        const float eye[3] = { centre[0] + d[0] * distance, centre[1] + d[1] * distance, centre[2] + d[2] * distance };
        const float up[3] = { 0.0f, std::fabs(d[2]) > 0.99f ? 1.0f : 0.0f, std::fabs(d[2]) > 0.99f ? 0.0f : 1.0f };
        float x[3], y[3];
        Cross(up, d, x);
        Normalize(x);
        Cross(d, x, y);

        auto& inverse = viewport.mViewInverseMatrix;
        SetIdentity(inverse);
        for (int k = 0; k < 3; ++k)
        {
            inverse[0][k] = x[k];
            inverse[1][k] = y[k];
            inverse[2][k] = d[k];
            inverse[3][k] = eye[k];
        }
        std::memcpy(viewport.mCameraMatrix, inverse, sizeof(inverse));

        auto& view = viewport.mViewMatrix;
        SetIdentity(view);
        for (int k = 0; k < 3; ++k)
        {
            view[k][0] = x[k];
            view[k][1] = y[k];
            view[k][2] = d[k];
        }
        view[3][0] = -(eye[0] * x[0] + eye[1] * x[1] + eye[2] * x[2]);
        view[3][1] = -(eye[0] * y[0] + eye[1] * y[1] + eye[2] * y[2]);
        view[3][2] = -(eye[0] * d[0] + eye[1] * d[1] + eye[2] * d[2]);

        const float zNear = distance - reach * 1.05f, zFar = distance + reach * 1.05f;
        const float scale = std::sqrt(distance * distance - reach * reach) / reach;
        auto& proj = viewport.mProjectionMatrix;
        std::memset(proj, 0, sizeof(proj));
        proj[0][0] = scale;
        proj[1][1] = scale;
        proj[2][3] = -1.0f;
        if (invertedZ)
        {
            proj[2][2] = zNear / (zFar - zNear);
            proj[3][2] = zNear * zFar / (zFar - zNear);
        }
        else
        {
            proj[2][2] = zFar / (zNear - zFar);
            proj[3][2] = zNear * zFar / (zNear - zFar);
        }
        viewport.mNearClip = zNear;
        viewport.mFarClip = zFar;
        viewport.mFov = 2.0f * std::atan(1.0f / scale) * 57.29578f;
        viewport.mAspect = 1.0f;
        for (auto& plane : viewport.mFrustumClipPlanes)
            plane = { 0.0f, 0.0f, 0.0f, 1e9f };
    }

    int FreeSlot()
    {
        for (int i = 0; i < kSlots; ++i)
            if (!slots[i].used)
                return i;
        if (frame - lastReplaceFrame < kReplaceFrames)
            return -1;
        int oldest = 0;
        for (int i = 1; i < kSlots; ++i)
            if (slots[i].bakedFrame < slots[oldest].bakedFrame)
                oldest = i;
        return oldest;
    }

    bool Baked(uint16_t model, const float* place)
    {
        for (auto& slot : slots)
            if (slot.used && slot.model == model && std::fabs(slot.place[0] - place[0]) < 0.5f && std::fabs(slot.place[1] - place[1]) < 0.5f)
                return true;
        return false;
    }

    void Bake(void* dc, uint16_t model, const float* place, void(__thiscall* execute)(void*))
    {
        auto viewport = rage::GetCurrentViewport();
        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        if (!viewport || !device)
            return;
        frame = TemporalAA::SceneFrame;
        if (lastBakeFrame == frame)
            return;
        const float* eye = viewport->mViewInverseMatrix[3];
        const float dx = place[0] - eye[0], dy = place[1] - eye[1], dz = place[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz < kFrom * kFrom)
            return;
        const auto info = EntityClass::ModelInfo(model);
        if (!info)
            return;
        const float* centre = reinterpret_cast<const float*>(info + 0x10);
        const float radius = *reinterpret_cast<const float*>(info + 0x1c);
        const float reach = radius + std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
        if (!(reach > 1.0f) || Baked(model, place))
            return;
        const int slot = FreeSlot();
        if (slot < 0)
            return;

        IDirect3DSurface9* bound[kLayers] = {};
        for (int i = 0; i < kLayers; ++i)
            if (FAILED(device->GetRenderTarget(i, &bound[i])))
                bound[i] = nullptr;
        IDirect3DSurface9* boundDepth = nullptr;
        device->GetDepthStencilSurface(&boundDepth);
        D3DVIEWPORT9 boundViewport{};
        device->GetViewport(&boundViewport);
        DWORD scissor = FALSE;
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);

        if (Create(device, bound))
        {
            lastBakeFrame = frame;
            if (slots[slot].used)
                lastReplaceFrame = frame;
            const bool invertedZ = viewport->mInvertZInProjectionMatrix;
            if (logged < 8)
            {
                ++logged;
                DWORD zFunc = 0, zEnable = 0, zWrite = 0;
                device->GetRenderState(D3DRS_ZFUNC, &zFunc);
                device->GetRenderState(D3DRS_ZENABLE, &zEnable);
                device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
                const bool cameraIsInverse = !std::memcmp(viewport->mCameraMatrix, viewport->mViewInverseMatrix, sizeof(viewport->mCameraMatrix));
                FusionLog::Write("Impostors", "Bake", "slot %d: model %u at %.0f,%.0f,%.0f, %.0f m off, reach %.1f m; game's projection [2][2] %.5f "
                    "[2][3] %.2f [3][2] %.4f, near %.2f far %.0f, inverted z %d, camera matrix = view inverse %d; z %u func %u write %u; targets %d\n",
                    slot, unsigned(model), place[0], place[1], place[2], std::sqrt(dx * dx + dy * dy + dz * dz), reach,
                    viewport->mProjectionMatrix[2][2], viewport->mProjectionMatrix[2][3], viewport->mProjectionMatrix[3][2], viewport->mNearClip,
                    viewport->mFarClip, int(invertedZ), int(cameraIsInverse), unsigned(zEnable), unsigned(zFunc), unsigned(zWrite), layerCount);
            }

            static rage::grcViewport saved;
            std::memcpy(&saved, viewport, sizeof(saved));
            for (int i = 0; i < kLayers; ++i)
                device->SetRenderTarget(i, i < layerCount && bound[i] ? layerSurfaces[i] : nullptr);
            device->SetDepthStencilSurface(depth);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

            const int tileX = (slot % kTilesAcross) * kTile, tileY = (slot / kTilesAcross) * kTile;
            D3DVIEWPORT9 tile = { DWORD(tileX), DWORD(tileY), DWORD(kTile), DWORD(kTile), 0.0f, 1.0f };
            device->SetViewport(&tile);
            device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, invertedZ ? 0.0f : 1.0f, 0);

            const float centreWorld[3] = { place[0] + centre[0], place[1] + centre[1], place[2] + centre[2] };
            for (int j = 0; j < kViews; ++j)
                for (int i = 0; i < kViews; ++i)
                {
                    float d[3];
                    ViewDirection(i, j, d);
                    SetCamera(*viewport, centreWorld, reach, d, invertedZ);
                    D3DVIEWPORT9 view = { DWORD(tileX + i * kFrame), DWORD(tileY + j * kFrame), DWORD(kFrame), DWORD(kFrame), 0.0f, 1.0f };
                    device->SetViewport(&view);
                    execute(dc);
                }

            std::memcpy(viewport, &saved, sizeof(saved));
            auto& s = slots[slot];
            s.used = true;
            s.model = model;
            std::memcpy(s.place, place, sizeof(s.place));
            s.reach = reach;
            s.bakedFrame = frame;
        }

        for (int i = 0; i < kLayers; ++i)
            device->SetRenderTarget(i, bound[i]);
        device->SetDepthStencilSurface(boundDepth);
        device->SetViewport(&boundViewport);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
        for (auto& surface : bound)
            SAFE_RELEASE(surface);
        SAFE_RELEASE(boundDepth);
    }

    void OnGBufferEntity(void* dc, uint16_t model, const float* place, void(__thiscall* execute)(void*))
    {
        if (DebugOn())
            Bake(dc, model, place, execute);
    }
}

namespace Impostors
{
    void DrawDebugOverlay()
    {
        if (!DebugOn() || !layerCount)
            return;
        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        if (!device)
            return;
        IDirect3DSurface9* target = nullptr;
        if (FAILED(device->GetRenderTarget(0, &target)) || !target)
            return;
        D3DSURFACE_DESC desc{};
        target->GetDesc(&desc);
        // The albedo layer and the normal layer, each a square of the frame's height over two
        const LONG side = LONG(desc.Height / 2);
        for (int layer = 0; layer < 2 && layer < layerCount; ++layer)
        {
            const RECT to = { side * layer, 0, side * (layer + 1), side };
            device->StretchRect(layerSurfaces[layer], nullptr, target, &to, D3DTEXF_LINEAR);
        }
        target->Release();
    }
}

class ImpostorsInit
{
public:
    ImpostorsInit()
    {
        FusionFix::onInitEvent() += []()
        {
            EntityClass::SetGBufferEntityHook(&OnGBufferEntity);
        };
        FusionFix::onBeforeReset() += []()
        {
            Release();
        };
    }
} ImpostorsInit;
