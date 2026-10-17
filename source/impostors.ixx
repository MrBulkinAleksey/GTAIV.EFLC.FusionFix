module;

#include <common.hxx>
#include <d3dx9.h>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

export module impostors;

import common;
import comvars;
import d3dx9_43;
import entityclass;
import settings;
import temporal;

// Impostors for the far map: the map's buildings 800 m and more out (where one is mostly 32-128 px and 3-5 ms of the
// G-buffer's CPU time goes) drawn as one draw of cards that read an atlas of what the game's own shaders wrote into
// the G-buffer for them, instead of a draw command (and its device draws) each.
//
// Baking: right after a far building's draw command (CDrawEntityDC) is executed in the scene's G-buffer
// (EntityClass::SetGBufferEntityHook), with every state of that draw still set, the command is executed again
// kViews x kViews times into a strip of bake targets: in place of the G-buffer's targets ours in their formats, our
// own INTZ depth buffer, a kFrame px viewport per view, and the current viewport (rage::GetCurrentViewport) turned
// into a narrow perspective camera looking at the model from that view's direction. The game recomputes the
// world-view-projection from the viewport for every entity it draws (grcViewport's transform update, which also
// sends our z-fighting fix's log depth constants from its near and far clip), so the command draws from our camera
// and writes the log depth of that camera. The viewport is copied back whole afterwards, and the targets, depth
// buffer and device viewport put back on the real device as they were. The frustum planes are opened up meanwhile in
// case the drawable culls its geometries against them. Up to kBakesPerFrame a frame, within kBakeBudgetMs.
//
// The views are hemi-octahedral over the upper hemisphere (world z up): the view (i, j) looks from the direction
// decoded from ((i + 0.5) / n, (j + 0.5) / n). A camera far off (kCameraDistance spheres) with the field of view just
// covering the sphere around the entity's place keeps the perspective close to orthographic.
//
// At the end of the G-buffer pass (TemporalAA::OnGBufferDrawn: before the motion vectors, the depth copy and all
// that reads the G-buffer) the frame's bakes go into the atlas: the layers by StretchRect, the depth turned into the
// distance off the card's plane along the bake's ray, in spheres (kEmpty where nothing was drawn). Then every far
// building the G-buffer left out this frame for a ready impostor (EntityClass::SetGBufferEntitySkip) is drawn as a
// card facing the bake view nearest to the camera's direction, at the entity's place: its pixel shader copies the
// layers into the G-buffer's targets and writes the log depth of the surface the bake saw there, so the lighting,
// fog, SSR and the depth tests take it for the building. Depth and stencil states are those of the game's first far
// building draw of the frame.
//
// Impostors ([PERFORMANCE] Impostors, also Graphics > Debug): bake, leave out and draw. ImpostorDebug (Graphics >
// Debug, [DEBUG] ImpostorDebug): bakes (with Impostors off too) and shows the first 16 tiles' albedo and normals in the
// top left corner of the frame. GTAIV.EFLC.FusionFix.Impostors.log: the atlas, the scene's states, the first bakes,
// and every kLogFrames the cards drawn, the bakes and the slots.
export namespace Impostors
{
    // Called by PostFX over the finished frame
    void DrawDebugOverlay();
}

namespace
{
    constexpr int kViews = 4;
    constexpr int kFrame = 64;
    constexpr int kTile = kViews * kFrame;
    constexpr int kAtlas = 4096;
    constexpr int kTilesAcross = kAtlas / kTile;
    constexpr int kSlots = kTilesAcross * kTilesAcross;
    constexpr int kLayers = 4;
    constexpr int kBakesPerFrame = 4;
    constexpr double kBakeBudgetMs = 1.5;
    constexpr float kFrom = 800.0f;
    constexpr float kCameraDistance = 6.0f;
    constexpr float kViewElevation = 0.14f;    // radians, some 8 degrees: far buildings are seen from about level
    constexpr float kMaxCardPixels = 1.5f * float(kFrame);    // bigger on screen: drawn as the game draws it
    constexpr float kCardScale = 1.1f;
    constexpr uint32_t kIdleFrames = 120;   // a slot not drawn for that long may be baked over
    constexpr uint32_t kLogFrames = 600;

    struct Slot
    {
        bool used = false;
        bool ready = false;
        uint16_t model = 0;
        float place[3]{};
        float reach = 0.0f;
        uint32_t lastUsed = 0;
        uint64_t key = 0;
    };
    Slot slots[kSlots];
    std::unordered_map<uint64_t, int> slotByKey;

    struct PendingBake
    {
        int slot;
        float zNear, zFar, distance, reach;
    };
    PendingBake pending[kBakesPerFrame];
    int pendingCount = 0;

    // The slots of the far buildings the G-buffer left out this frame
    std::vector<int> instances;

    // The atlas: the G-buffer's layers and the depth layer
    IDirect3DTexture9* layers[kLayers] = {};
    IDirect3DSurface9* layerSurfaces[kLayers] = {};
    IDirect3DTexture9* depthLayer = nullptr;
    IDirect3DSurface9* depthLayerSurface = nullptr;
    // A strip of kBakesPerFrame tiles the bakes are drawn into
    IDirect3DTexture9* bakeTargets[kLayers] = {};
    IDirect3DSurface9* bakeSurfaces[kLayers] = {};
    IDirect3DTexture9* bakeDepth = nullptr;
    IDirect3DSurface9* bakeDepthSurface = nullptr;
    D3DFORMAT layerFormats[kLayers] = {};
    int layerCount = 0;
    bool bFailed = false;

    IDirect3DVertexShader9* cardVS = nullptr;
    IDirect3DPixelShader9* cardPS = nullptr;
    IDirect3DPixelShader9* resolvePS = nullptr;
    bool bShadersFailed = false;

    // The scene's camera and G-buffer as the frame's G-buffer pass has them
    struct Scene
    {
        uint32_t frame = ~0u;
        bool valid = false;
        bool statesTaken = false;
        float viewProj[4][4]{};
        float eye[3]{};
        float back[3]{};
        float zNear = 0.0f, zFar = 0.0f;
        float pixelsPerReach = 0.0f;    // the sphere's diameter in pixels is reach / distance times this
        IDirect3DSurface9* targets[kLayers]{};
        IDirect3DSurface9* depth = nullptr;
        D3DVIEWPORT9 viewport{};
        DWORD zFunc = D3DCMP_LESSEQUAL;
    } scene;

    // The stencil states of the game's far building draws, set again for the cards
    constexpr D3DRENDERSTATETYPE kStencilStates[] =
    {
        D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
        D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_TWOSIDEDSTENCILMODE,
    };
    DWORD sceneStencil[std::size(kStencilStates)] = {};

    uint32_t frame = 0, bakeFrame = ~0u;
    double bakeMsThisFrame = 0.0;
    int logged = 0;
    bool loggedScene = false;
    double tickMs = 0.0;

    struct Stats
    {
        uint32_t from = 0, frames = 0;
        uint64_t cards = 0, bakes = 0, evicted = 0, noSlot = 0, failedDraws = 0;
        double bakeMs = 0.0, drawMs = 0.0;
    } stats;

    bool DebugOn()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_IMPOSTOR_DEBUG");
        return pref && pref->get() != 0;
    }

    bool ImpostorsOn()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_IMPOSTORS");
        return pref && pref->get() != 0 && !bFailed && !bShadersFailed;
    }

    double NowMs()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (tickMs == 0.0)
        {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            tickMs = 1000.0 / double(frequency.QuadPart);
        }
        return double(now.QuadPart) * tickMs;
    }

    void ReleaseScene()
    {
        for (auto& target : scene.targets)
            SAFE_RELEASE(target);
        SAFE_RELEASE(scene.depth);
        scene.valid = false;
    }

    void Release()
    {
        for (int i = 0; i < kLayers; ++i)
        {
            SAFE_RELEASE(layerSurfaces[i]);
            SAFE_RELEASE(layers[i]);
            SAFE_RELEASE(bakeSurfaces[i]);
            SAFE_RELEASE(bakeTargets[i]);
        }
        SAFE_RELEASE(depthLayerSurface);
        SAFE_RELEASE(depthLayer);
        SAFE_RELEASE(bakeDepthSurface);
        SAFE_RELEASE(bakeDepth);
        SAFE_RELEASE(cardVS);
        SAFE_RELEASE(cardPS);
        SAFE_RELEASE(resolvePS);
        ReleaseScene();
        scene.frame = ~0u;
        layerCount = 0;
        for (auto& slot : slots)
            slot = {};
        slotByKey.clear();
        pendingCount = 0;
        instances.clear();
    }

    // The atlas and the bake strip in the formats of the targets the game has bound for the G-buffer
    bool Create(IDirect3DDevice9* device, IDirect3DSurface9* const (&bound)[kLayers])
    {
        if (layerCount)
            return true;
        if (bFailed)
            return false;
        auto fail = [&](const char* what, int i, D3DFORMAT format)
        {
            FusionLog::Write("Impostors", "Atlas", "%s %d (format %u) could not be made\n", what, i, unsigned(format));
            Release();
            bFailed = true;
            return false;
        };
        int count = 0;
        for (int i = 0; i < kLayers && bound[i]; ++i, ++count)
        {
            D3DSURFACE_DESC desc{};
            bound[i]->GetDesc(&desc);
            layerFormats[i] = desc.Format;
            if (FAILED(device->CreateTexture(kAtlas, kAtlas, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &layers[i], nullptr)) ||
                FAILED(layers[i]->GetSurfaceLevel(0, &layerSurfaces[i])))
                return fail("atlas layer", i, desc.Format);
            if (FAILED(device->CreateTexture(kTile * kBakesPerFrame, kTile, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &bakeTargets[i], nullptr)) ||
                FAILED(bakeTargets[i]->GetSurfaceLevel(0, &bakeSurfaces[i])))
                return fail("bake target", i, desc.Format);
        }
        if (count != kLayers)
        {
            FusionLog::Write("Impostors", "Atlas", "the G-buffer has %d targets bound, not %d\n", count, kLayers);
            Release();
            bFailed = true;
            return false;
        }
        if (FAILED(device->CreateTexture(kAtlas, kAtlas, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R16F, D3DPOOL_DEFAULT, &depthLayer, nullptr)) ||
            FAILED(depthLayer->GetSurfaceLevel(0, &depthLayerSurface)))
            return fail("depth layer", 0, D3DFMT_R16F);
        constexpr auto INTZ = D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z'));
        if (FAILED(device->CreateTexture(kTile * kBakesPerFrame, kTile, 1, D3DUSAGE_DEPTHSTENCIL, INTZ, D3DPOOL_DEFAULT, &bakeDepth, nullptr)) ||
            FAILED(bakeDepth->GetSurfaceLevel(0, &bakeDepthSurface)))
            return fail("bake depth (INTZ)", 0, INTZ);
        layerCount = count;
        FusionLog::Write("Impostors", "Atlas", "%d px, layers in the G-buffer's formats %u %u %u %u and depth, %d tiles of %dx%d views of %d px, "
            "%d bakes a frame\n", kAtlas, unsigned(layerFormats[0]), unsigned(layerFormats[1]), unsigned(layerFormats[2]),
            unsigned(layerFormats[3]), kSlots, kViews, kViews, kFrame, kBakesPerFrame);
        return true;
    }

    // vs_3_0 / ps_3_0, compiled once on the real device
    constexpr char kCardVS[] = R"(
float4x4 cViewProj : register(c0);
struct VSIn { float3 pos : POSITION; float4 place : TEXCOORD0; float4 dir : TEXCOORD1; float4 axis : TEXCOORD2; };
struct VSOut { float4 pos : POSITION; float4 place : TEXCOORD0; float4 dir : TEXCOORD1; float4 axis : TEXCOORD2; float3 world : TEXCOORD3; };
VSOut main(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), cViewProj);
    o.place = i.place;
    o.dir = i.dir;
    o.axis = i.axis;
    o.world = i.pos;
    return o;
}
)";

    // The card faces the camera; the camera's ray through this pixel is walked through the entity's sphere, each
    // point projected into the chosen bake view's frame from the bake's eye, until it is no nearer to that eye than
    // the surface the depth layer keeps there (in spheres off the frame's plane). The layers are read there, and the
    // log depth of that point is written as the game's G-buffer shaders write it.
    constexpr char kCardPS[] = R"(
sampler2D sAlbedo : register(s0);
sampler2D sNormal : register(s1);
sampler2D sSpecular : register(s2);
sampler2D sMaterial : register(s3);
sampler2D sDepth : register(s4);
float4 cEye : register(c0);
float4 cBack : register(c1);
float4 cDepth : register(c2);
float4 cFrame : register(c3);   // frame size in uv, bake bakeDist in spheres, frame halfSize size in spheres
struct PSIn { float4 place : TEXCOORD0; float4 dir : TEXCOORD1; float4 axis : TEXCOORD2; float3 world : TEXCOORD3; };
struct PSOut { float4 c0 : COLOR0; float4 c1 : COLOR1; float4 c2 : COLOR2; float4 c3 : COLOR3; float depth : DEPTH; };

static const int kSteps = 24;

// Where a point falls in the bake's frame (xy, uv) and how far off the frame's plane it is (z, spheres); w > 0 inside
float4 Project(float3 p, float3 eye, float3 d, float3 x, float3 y, float bakeDist, float halfSize, float reach, float2 origin)
{
    float3 rel = p - eye;
    float w = -dot(rel, d);
    float3 q = rel * (bakeDist / max(w, 1e-3)) + d * bakeDist;
    float2 f = float2(dot(q, x), dot(q, y)) / halfSize;
    float2 uv = origin + float2(f.x * 0.5 + 0.5, 0.5 - f.y * 0.5) * cFrame.x;
    float inside = (max(abs(f.x), abs(f.y)) <= 1.0) ? 1.0 : -1.0;
    return float4(uv, (w - bakeDist) / reach, inside);
}

PSOut main(PSIn i)
{
    PSOut o;
    float reach = i.place.w;
    float3 d = normalize(i.dir.xyz);
    float3 x = normalize(i.axis.xyz);
    float3 y = cross(d, x);
    float bakeDist = reach * cFrame.y;
    float halfSize = reach * cFrame.z;
    float3 eye = i.place.xyz + d * bakeDist;
    float2 origin = float2(i.dir.w, i.axis.w);
    float3 r = normalize(i.world - cEye.xyz);

    float t0 = -1.1 * reach;
    float dt = 2.2 * reach / kSteps;
    float hitT = 0.0;
    float found = -1.0;
    [loop] for (int k = 0; k < kSteps; ++k)
    {
        float t = t0 + dt * k;
        float4 p = Project(i.world + r * t, eye, d, x, y, bakeDist, halfSize, reach, origin);
        float s = tex2Dlod(sDepth, float4(p.xy, 0, 0)).r;
        if (p.w > 0.0 && s < 2.0 && p.z >= s)
        {
            hitT = t;
            found = 1.0;
            break;
        }
    }
    clip(found);

    // Halve the last step three times
    float lo = hitT - dt, hi = hitT;
    [unroll] for (int b = 0; b < 3; ++b)
    {
        float mid = 0.5 * (lo + hi);
        float4 p = Project(i.world + r * mid, eye, d, x, y, bakeDist, halfSize, reach, origin);
        float s = tex2Dlod(sDepth, float4(p.xy, 0, 0)).r;
        if (p.w > 0.0 && s < 2.0 && p.z >= s)
            hi = mid;
        else
            lo = mid;
    }
    float3 surface = i.world + r * hi;
    float4 hit = Project(surface, eye, d, x, y, bakeDist, halfSize, reach, origin);
    float4 uv = float4(hit.xy, 0, 0);

    float w = dot(cEye.xyz - surface, cBack.xyz);
    o.depth = saturate(log2(max(w * cDepth.x, 1.0)) * cDepth.y);
    o.c0 = tex2Dlod(sAlbedo, uv);
    o.c1 = tex2Dlod(sNormal, uv);
    o.c2 = tex2Dlod(sSpecular, uv);
    o.c3 = tex2Dlod(sMaterial, uv);
    return o;
}
)";

    // The bake's log depth into the depth layer: (w - distance) / reach, 4 where the depth stayed cleared
    constexpr char kResolvePS[] = R"(
sampler2D sZ : register(s0);
float4 cBake : register(c0);
float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float z = tex2D(sZ, uv).r;
    float w = cBake.x * exp2(z * cBake.y);
    return z >= 0.99999 ? 4.0 : (w - cBake.z) / cBake.w;
}
)";

    bool CreateShaders(IDirect3DDevice9* device)
    {
        if (cardVS && cardPS && resolvePS)
            return true;
        if (bShadersFailed)
            return false;
        auto compile = [&](const char* source, size_t size, const char* profile, const char* name) -> ID3DXBuffer*
        {
            ID3DXBuffer* code = nullptr;
            ID3DXBuffer* errors = nullptr;
            const HRESULT hr = D3DXCompileShader(source, UINT(size - 1), nullptr, nullptr, "main", profile, 0, &code, &errors, nullptr);
            if (FAILED(hr) || !code)
            {
                FusionLog::Write("Impostors", "Shaders", "%s: compiling failed, 0x%08lX %s\n", name, hr,
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                SAFE_RELEASE(code);
            }
            SAFE_RELEASE(errors);
            return code;
        };
        ID3DXBuffer* vs = compile(kCardVS, sizeof(kCardVS), "vs_3_0", "card VS");
        ID3DXBuffer* ps = compile(kCardPS, sizeof(kCardPS), "ps_3_0", "card PS");
        ID3DXBuffer* resolve = compile(kResolvePS, sizeof(kResolvePS), "ps_3_0", "resolve PS");
        if (vs)
            device->CreateVertexShader(static_cast<const DWORD*>(vs->GetBufferPointer()), &cardVS);
        if (ps)
            device->CreatePixelShader(static_cast<const DWORD*>(ps->GetBufferPointer()), &cardPS);
        if (resolve)
            device->CreatePixelShader(static_cast<const DWORD*>(resolve->GetBufferPointer()), &resolvePS);
        SAFE_RELEASE(vs);
        SAFE_RELEASE(ps);
        SAFE_RELEASE(resolve);
        if (!cardVS || !cardPS || !resolvePS)
        {
            FusionLog::Write("Impostors", "Shaders", "not made, impostors off\n");
            SAFE_RELEASE(cardVS);
            SAFE_RELEASE(cardPS);
            SAFE_RELEASE(resolvePS);
            bShadersFailed = true;
            return false;
        }
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

    constexpr int kViewCount = kViews * kViews;

    // The direction of view k, from the model towards the camera: a ring of azimuths kViewElevation above level
    void ViewDirection(int k, float (&d)[3])
    {
        const float azimuth = float(k) * 6.2831853f / float(kViewCount);
        d[0] = std::cos(kViewElevation) * std::cos(azimuth);
        d[1] = std::cos(kViewElevation) * std::sin(azimuth);
        d[2] = std::sin(kViewElevation);
    }

    // The view nearest in azimuth to a direction from the model
    int NearestView(const float (&direction)[3])
    {
        const float turns = std::atan2(direction[1], direction[0]) / 6.2831853f;
        const int k = int(std::floor(turns * float(kViewCount) + 0.5f));
        return ((k % kViewCount) + kViewCount) % kViewCount;
    }

    // The bake camera's right and up for a view direction
    void ViewAxes(const float (&d)[3], float (&x)[3], float (&y)[3])
    {
        const float up[3] = { 0.0f, std::fabs(d[2]) > 0.99f ? 1.0f : 0.0f, std::fabs(d[2]) > 0.99f ? 0.0f : 1.0f };
        Cross(up, d, x);
        Normalize(x);
        Cross(d, x, y);
    }

    // The game's camera is right-handed and looks down its -z (the projection's [2][3] is -1); its depth range is
    // either the usual one or inverted, taken from the viewport's own projection
    void SetCamera(rage::grcViewport& viewport, const float* centre, float reach, const float (&d)[3], bool invertedZ)
    {
        const float distance = reach * kCameraDistance;
        const float eye[3] = { centre[0] + d[0] * distance, centre[1] + d[1] * distance, centre[2] + d[2] * distance };
        float x[3], y[3];
        ViewAxes(d, x, y);

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

    uint64_t KeyOf(uint16_t model, const float* place)
    {
        uint64_t key = model;
        for (int i = 0; i < 3; ++i)
            key = key * 0x9E3779B97F4A7C15ull ^ uint64_t(int64_t(std::lround(place[i] * 4.0f)));
        return key;
    }

    float DistanceSquared(const float* place)
    {
        const float dx = place[0] - scene.eye[0], dy = place[1] - scene.eye[1], dz = place[2] - scene.eye[2];
        return dx * dx + dy * dy + dz * dz;
    }

    // Far enough, and small enough on screen for a frame of kFrame px
    bool FarAndSmall(const float* place, float reach)
    {
        const float distance2 = DistanceSquared(place);
        if (distance2 < kFrom * kFrom)
            return false;
        const float pixels = reach * scene.pixelsPerReach;
        return pixels * pixels <= kMaxCardPixels * kMaxCardPixels * distance2;
    }

    // A free slot, else the ready one drawn longest ago if it has not been drawn for kIdleFrames; -1 when none
    int FreeSlot()
    {
        int oldest = -1;
        for (int i = 0; i < kSlots; ++i)
        {
            if (!slots[i].used)
                return i;
            if (slots[i].ready && frame - slots[i].lastUsed > kIdleFrames && (oldest < 0 || slots[i].lastUsed < slots[oldest].lastUsed))
                oldest = i;
        }
        if (oldest >= 0)
        {
            slotByKey.erase(slots[oldest].key);
            slots[oldest] = {};
            stats.evicted++;
        }
        return oldest;
    }

    void WriteStats()
    {
        const double frames = (std::max)(stats.frames, 1u);
        int used = 0, ready = 0;
        for (const auto& slot : slots)
        {
            used += slot.used;
            ready += slot.ready;
        }
        FusionLog::Write("Impostors", "Stats", "over %u scene frames, per frame: %.1f cards drawn (%.3f ms), %.2f bakes (%.3f ms); "
            "slots %d used, %d ready of %d; %llu baked over, %llu bakes found no slot, %llu frames the cards could not be drawn\n",
            stats.frames, double(stats.cards) / frames, stats.drawMs / frames, double(stats.bakes) / frames, stats.bakeMs / frames,
            used, ready, kSlots, (unsigned long long)stats.evicted, (unsigned long long)stats.noSlot, (unsigned long long)stats.failedDraws);
        const uint32_t from = frame;
        stats = {};
        stats.from = from;
    }

    // The first far building of a frame's G-buffer: the camera and the targets the cards go into
    void BeginFrame(IDirect3DDevice9* device)
    {
        frame = TemporalAA::SceneFrame;
        if (scene.frame == frame)
            return;
        scene.frame = frame;
        scene.statesTaken = false;
        instances.clear();
        ReleaseScene();
        stats.frames++;
        if (frame - stats.from >= kLogFrames)
            WriteStats();

        auto viewport = rage::GetCurrentViewport();
        if (!viewport || !device)
            return;
        const auto& view = viewport->mViewMatrix;
        const auto& proj = viewport->mProjectionMatrix;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                scene.viewProj[r][c] = view[r][0] * proj[0][c] + view[r][1] * proj[1][c] + view[r][2] * proj[2][c] + view[r][3] * proj[3][c];
        for (int k = 0; k < 3; ++k)
        {
            scene.eye[k] = viewport->mViewInverseMatrix[3][k];
            scene.back[k] = viewport->mViewInverseMatrix[2][k];
        }
        scene.zNear = viewport->mNearClip;
        scene.zFar = viewport->mFarClip;
        for (int i = 0; i < kLayers; ++i)
            if (FAILED(device->GetRenderTarget(i, &scene.targets[i])))
                scene.targets[i] = nullptr;
        device->GetDepthStencilSurface(&scene.depth);
        device->GetViewport(&scene.viewport);
        scene.pixelsPerReach = proj[1][1] * float(scene.viewport.Height);
        scene.valid = scene.targets[0] && scene.depth && scene.zNear > 0.0f && scene.zFar > scene.zNear;
    }

    // After a far building's draw: the depth and stencil states it drew with, once a frame
    void TakeStates(IDirect3DDevice9* device)
    {
        if (scene.statesTaken)
            return;
        scene.statesTaken = true;
        device->GetRenderState(D3DRS_ZFUNC, &scene.zFunc);
        for (size_t i = 0; i < std::size(kStencilStates); ++i)
            device->GetRenderState(kStencilStates[i], &sceneStencil[i]);
        if (!loggedScene)
        {
            loggedScene = true;
            D3DSURFACE_DESC desc{};
            scene.targets[0]->GetDesc(&desc);
            FusionLog::Write("Impostors", "Scene", "G-buffer %ux%u, viewport %lu,%lu %lux%lu, near %.3f far %.0f; z func %lu; stencil enable %lu "
                "func %lu ref %lu mask %lx write mask %lx pass %lu fail %lu z fail %lu two sided %lu\n", desc.Width, desc.Height,
                scene.viewport.X, scene.viewport.Y, scene.viewport.Width, scene.viewport.Height, scene.zNear, scene.zFar, scene.zFunc,
                sceneStencil[0], sceneStencil[1], sceneStencil[2], sceneStencil[3], sceneStencil[4], sceneStencil[5], sceneStencil[6],
                sceneStencil[7], sceneStencil[8]);
        }
    }

    bool OnGBufferSkip(void* dc, EntityClass::Kind kind, uint16_t model, const float* place)
    {
        if (kind != EntityClass::Kind::Building || !ImpostorsOn() || !layerCount)
            return false;
        BeginFrame(RageDirect3DDevice9::GetRuntimeDevice());
        if (!scene.valid || !scene.statesTaken || DistanceSquared(place) < kFrom * kFrom)
            return false;
        const auto found = slotByKey.find(KeyOf(model, place));
        if (found == slotByKey.end())
            return false;
        auto& slot = slots[found->second];
        if (!slot.ready || !FarAndSmall(place, slot.reach))
            return false;
        // An entity may have a command in more than one of the pass's buckets: one card
        if (slot.lastUsed != frame)
        {
            slot.lastUsed = frame;
            instances.push_back(found->second);
        }
        return true;
    }

    void Bake(void* dc, uint16_t model, const float* place, void(__thiscall* execute)(void*))
    {
        auto viewport = rage::GetCurrentViewport();
        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        if (!viewport || !device)
            return;
        if (bakeFrame != frame)
        {
            bakeFrame = frame;
            bakeMsThisFrame = 0.0;
        }
        if (pendingCount >= kBakesPerFrame || bakeMsThisFrame >= kBakeBudgetMs)
            return;
        const uint64_t key = KeyOf(model, place);
        if (slotByKey.count(key))
            return;
        const auto info = EntityClass::ModelInfo(model);
        if (!info)
            return;
        const float* centre = reinterpret_cast<const float*>(info + 0x10);
        const float radius = *reinterpret_cast<const float*>(info + 0x1c);
        // Around the entity's place, which its sphere, however the entity is turned, never leaves by more
        const float reach = radius + std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
        if (!(reach > 1.0f) || !FarAndSmall(place, reach))
            return;

        IDirect3DSurface9* bound[kLayers] = {};
        for (int i = 0; i < kLayers; ++i)
            if (FAILED(device->GetRenderTarget(i, &bound[i])))
                bound[i] = nullptr;
        auto releaseBound = [&]
        {
            for (auto& surface : bound)
                SAFE_RELEASE(surface);
        };
        if (!Create(device, bound))
            return releaseBound();
        const int slot = FreeSlot();
        if (slot < 0)
        {
            stats.noSlot++;
            return releaseBound();
        }
        const double start = NowMs();

        IDirect3DSurface9* boundDepth = nullptr;
        device->GetDepthStencilSurface(&boundDepth);
        D3DVIEWPORT9 boundViewport{};
        device->GetViewport(&boundViewport);
        DWORD scissor = FALSE;
        device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
        const bool invertedZ = viewport->mInvertZInProjectionMatrix;
        const float distance = reach * kCameraDistance;
        if (logged < 8)
        {
            ++logged;
            FusionLog::Write("Impostors", "Bake", "slot %d: model %u at %.0f,%.0f,%.0f, %.0f m off, reach %.1f m; inverted z %d\n", slot,
                unsigned(model), place[0], place[1], place[2], std::sqrt(DistanceSquared(place)), reach, int(invertedZ));
        }

        static rage::grcViewport saved;
        std::memcpy(&saved, viewport, sizeof(saved));
        for (int i = 0; i < kLayers; ++i)
            device->SetRenderTarget(i, bakeSurfaces[i]);
        device->SetDepthStencilSurface(bakeDepthSurface);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

        const int stripX = pendingCount * kTile;
        D3DVIEWPORT9 tile = { DWORD(stripX), 0, DWORD(kTile), DWORD(kTile), 0.0f, 1.0f };
        device->SetViewport(&tile);
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, invertedZ ? 0.0f : 1.0f, 0);

        for (int j = 0; j < kViews; ++j)
            for (int i = 0; i < kViews; ++i)
            {
                float d[3];
                ViewDirection(j * kViews + i, d);
                SetCamera(*viewport, place, reach, d, invertedZ);
                D3DVIEWPORT9 view = { DWORD(stripX + i * kFrame), DWORD(j * kFrame), DWORD(kFrame), DWORD(kFrame), 0.0f, 1.0f };
                device->SetViewport(&view);
                execute(dc);
            }

        std::memcpy(viewport, &saved, sizeof(saved));
        auto& s = slots[slot];
        s = {};
        s.used = true;
        s.model = model;
        std::memcpy(s.place, place, sizeof(s.place));
        s.reach = reach;
        s.lastUsed = frame;
        s.key = key;
        slotByKey[key] = slot;
        pending[pendingCount++] = { slot, distance - reach * 1.05f, distance + reach * 1.05f, distance, reach };

        for (int i = 0; i < kLayers; ++i)
            device->SetRenderTarget(i, bound[i]);
        device->SetDepthStencilSurface(boundDepth);
        device->SetViewport(&boundViewport);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
        releaseBound();
        SAFE_RELEASE(boundDepth);

        const double ms = NowMs() - start;
        bakeMsThisFrame += ms;
        stats.bakeMs += ms;
        stats.bakes++;
    }

    void OnGBufferEntity(void* dc, EntityClass::Kind kind, uint16_t model, const float* place, void(__thiscall* execute)(void*))
    {
        if (kind != EntityClass::Kind::Building || bFailed || !(ImpostorsOn() || DebugOn()))
            return;
        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        BeginFrame(device);
        if (!scene.valid || DistanceSquared(place) < kFrom * kFrom)
            return;
        TakeStates(device);
        Bake(dc, model, place, execute);
    }

    // What the passes here change beyond what TemporalAA's StateBackup keeps (sampler 0, the usual render states)
    struct ExtraBackup
    {
        static constexpr DWORD kSamplers = kLayers + 1;
        static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] =
        {
            D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE,
        };
        static constexpr D3DRENDERSTATETYPE kStates[] =
        {
            D3DRS_STENCILENABLE, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
            D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_TWOSIDEDSTENCILMODE,
            D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3,
        };
        IDirect3DDevice9* device;
        IDirect3DBaseTexture9* textures[kSamplers]{};
        DWORD samplerStates[kSamplers][std::size(kSamplerStates)]{};
        DWORD states[std::size(kStates)]{};
        float vsConstants[4 * 4]{};
        float psConstants[4 * 4]{};

        explicit ExtraBackup(IDirect3DDevice9* d) : device(d)
        {
            for (DWORD s = 0; s < kSamplers; ++s)
            {
                device->GetTexture(s, &textures[s]);
                for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                    device->GetSamplerState(s, kSamplerStates[i], &samplerStates[s][i]);
            }
            for (size_t i = 0; i < std::size(kStates); ++i)
                device->GetRenderState(kStates[i], &states[i]);
            device->GetVertexShaderConstantF(0, vsConstants, 4);
            device->GetPixelShaderConstantF(0, psConstants, 4);
        }

        ~ExtraBackup()
        {
            for (DWORD s = 0; s < kSamplers; ++s)
            {
                device->SetTexture(s, textures[s]);
                SAFE_RELEASE(textures[s]);
                for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                    device->SetSamplerState(s, kSamplerStates[i], samplerStates[s][i]);
            }
            for (size_t i = 0; i < std::size(kStates); ++i)
                device->SetRenderState(kStates[i], states[i]);
            device->SetVertexShaderConstantF(0, vsConstants, 4);
            device->SetPixelShaderConstantF(0, psConstants, 4);
        }
    };

    void SetPointSampler(IDirect3DDevice9* device, DWORD s, IDirect3DBaseTexture9* texture)
    {
        device->SetTexture(s, texture);
        device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    void SetPlainStates(IDirect3DDevice9* device)
    {
        static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kPlain[] =
        {
            { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
            { D3DRS_COLORWRITEENABLE, 0x0F }, { D3DRS_COLORWRITEENABLE1, 0x0F }, { D3DRS_COLORWRITEENABLE2, 0x0F },
            { D3DRS_COLORWRITEENABLE3, 0x0F }, { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE },
            { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
            { D3DRS_DEPTHBIAS, 0 }, { D3DRS_SLOPESCALEDEPTHBIAS, 0 }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
        };
        for (auto [state, value] : kPlain)
            device->SetRenderState(state, value);
    }

    // The frame's bakes into the atlas: the layers copied, the depth turned into the depth layer
    void ResolveBakes(IDirect3DDevice9* device)
    {
        if (!pendingCount)
            return;
        for (int p = 0; p < pendingCount; ++p)
        {
            const auto& bake = pending[p];
            const RECT from = { p * kTile, 0, (p + 1) * kTile, kTile };
            const LONG x = (bake.slot % kTilesAcross) * kTile, y = (bake.slot / kTilesAcross) * kTile;
            const RECT to = { x, y, x + kTile, y + kTile };
            for (int i = 0; i < kLayers; ++i)
                device->StretchRect(bakeSurfaces[i], &from, layerSurfaces[i], &to, D3DTEXF_POINT);
        }

        device->SetRenderTarget(0, depthLayerSurface);
        for (DWORD i = 1; i < kLayers; ++i)
            device->SetRenderTarget(i, nullptr);
        device->SetDepthStencilSurface(nullptr);
        const D3DVIEWPORT9 whole = { 0, 0, DWORD(kAtlas), DWORD(kAtlas), 0.0f, 1.0f };
        device->SetViewport(&whole);
        SetPlainStates(device);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(resolvePS);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        SetPointSampler(device, 0, bakeDepth);
        struct Vertex { float x, y, z, rhw, u, v; };
        const float stripWidth = float(kTile * kBakesPerFrame);
        for (int p = 0; p < pendingCount; ++p)
        {
            const auto& bake = pending[p];
            const float constants[4] = { bake.zNear, std::log2(bake.zFar / bake.zNear), bake.distance, bake.reach };
            device->SetPixelShaderConstantF(0, constants, 1);
            // Pixel centres on the tile's texels
            const float x = float((bake.slot % kTilesAcross) * kTile) - 0.5f, y = float((bake.slot / kTilesAcross) * kTile) - 0.5f;
            const float u0 = float(p * kTile) / stripWidth, u1 = float((p + 1) * kTile) / stripWidth;
            const Vertex quad[4] =
            {
                { x, y, 0.0f, 1.0f, u0, 0.0f },
                { x + kTile, y, 0.0f, 1.0f, u1, 0.0f },
                { x, y + kTile, 0.0f, 1.0f, u0, 1.0f },
                { x + kTile, y + kTile, 0.0f, 1.0f, u1, 1.0f },
            };
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
            slots[bake.slot].ready = true;
        }
        device->SetTexture(0, nullptr);
        pendingCount = 0;
    }

    struct CardVertex
    {
        float position[3];
        float place[4];     // the entity's place, its reach
        float dir[4];       // the bake view's direction, its frame's u in the atlas
        float axis[4];      // the bake view's right, its frame's v
    };
    std::vector<CardVertex> cardVertices;

    // The cards of the far buildings the G-buffer left out, into its targets
    void DrawCards(IDirect3DDevice9* device)
    {
        if (instances.empty())
            return;
        if (!scene.valid)
        {
            stats.failedDraws++;
            return;
        }
        const double start = NowMs();
        // Half a texel: a D3D9 pixel holds what is at its corner, the frame's k / n
        const float halfTexel = 0.5f / float(kAtlas);
        cardVertices.clear();
        cardVertices.reserve(instances.size() * 6);
        for (const int index : instances)
        {
            const auto& slot = slots[index];
            float toEye[3] = { scene.eye[0] - slot.place[0], scene.eye[1] - slot.place[1], scene.eye[2] - slot.place[2] };
            Normalize(toEye);
            const int view = NearestView(toEye);
            float d[3], x[3], y[3];
            ViewDirection(view, d);
            ViewAxes(d, x, y);
            // The card faces the camera
            float cu[3], cv[3];
            ViewAxes(toEye, cu, cv);
            const float h = slot.reach * kCardScale;
            const float u0 = float((index % kTilesAcross) * kTile + (view % kViews) * kFrame) / float(kAtlas) + halfTexel;
            const float v0 = float((index / kTilesAcross) * kTile + (view / kViews) * kFrame) / float(kAtlas) + halfTexel;
            CardVertex corners[4];
            for (int c = 0; c < 4; ++c)
            {
                const float sx = (c & 1) ? 1.0f : -1.0f, sy = (c & 2) ? -1.0f : 1.0f;
                auto& v = corners[c];
                for (int k = 0; k < 3; ++k)
                {
                    v.position[k] = slot.place[k] + (cu[k] * sx + cv[k] * sy) * h;
                    v.place[k] = slot.place[k];
                    v.dir[k] = d[k];
                    v.axis[k] = x[k];
                }
                v.place[3] = slot.reach;
                v.dir[3] = u0;
                v.axis[3] = v0;
            }
            for (int c : { 0, 1, 2, 2, 1, 3 })
                cardVertices.push_back(corners[c]);
        }

        for (int i = 0; i < kLayers; ++i)
            device->SetRenderTarget(i, scene.targets[i]);
        device->SetDepthStencilSurface(scene.depth);
        device->SetViewport(&scene.viewport);
        SetPlainStates(device);
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, scene.zFunc);
        for (size_t i = 0; i < std::size(kStencilStates); ++i)
            device->SetRenderState(kStencilStates[i], sceneStencil[i]);
        device->SetVertexShader(cardVS);
        device->SetPixelShader(cardPS);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX3 | D3DFVF_TEXCOORDSIZE4(0) | D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE4(2));
        // HLSL takes the matrix by columns
        float columns[4][4];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                columns[c][r] = scene.viewProj[r][c];
        device->SetVertexShaderConstantF(0, &columns[0][0], 4);
        const float psConstants[4][4] =
        {
            { scene.eye[0], scene.eye[1], scene.eye[2], 0.0f },
            { scene.back[0], scene.back[1], scene.back[2], 0.0f },
            { 1.0f / scene.zNear, 1.0f / std::log2(scene.zFar / scene.zNear), 0.0f, 0.0f },
            { float(kFrame) / float(kAtlas), kCameraDistance, kCameraDistance / std::sqrt(kCameraDistance * kCameraDistance - 1.0f), 0.0f },
        };
        device->SetPixelShaderConstantF(0, &psConstants[0][0], 4);
        for (DWORD i = 0; i < kLayers; ++i)
            SetPointSampler(device, i, layers[i]);
        SetPointSampler(device, kLayers, depthLayer);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(cardVertices.size() / 3), cardVertices.data(), sizeof(CardVertex));
        for (DWORD i = 0; i <= kLayers; ++i)
            device->SetTexture(i, nullptr);

        stats.cards += instances.size();
        stats.drawMs += NowMs() - start;
    }

    void OnGBufferDrawn(IDirect3DDevice9* device)
    {
        if (!layerCount || scene.frame != TemporalAA::SceneFrame)
        {
            instances.clear();
            return;
        }
        if (!CreateShaders(device))
        {
            // Left out this frame with nothing to draw them; from the next frame they are drawn again
            if (!instances.empty())
                stats.failedDraws++;
            instances.clear();
            pendingCount = 0;
            return;
        }
        {
            ExtraBackup extra(device);
            ResolveBakes(device);
            DrawCards(device);
        }
        instances.clear();
        ReleaseScene();
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
        // The first 4x4 tiles of the albedo layer and of the normal layer, each a square of the frame's height over two
        const LONG side = LONG(desc.Height / 2);
        const RECT from = { 0, 0, 4 * kTile, 4 * kTile };
        for (int layer = 0; layer < 2 && layer < layerCount; ++layer)
        {
            const RECT to = { side * layer, 0, side * (layer + 1), side };
            device->StretchRect(layerSurfaces[layer], &from, target, &to, D3DTEXF_LINEAR);
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
            EntityClass::SetGBufferEntitySkip(&OnGBufferSkip);
            TemporalAA::OnGBufferDrawn = &OnGBufferDrawn;
        };
        FusionFix::onBeforeReset() += []()
        {
            Release();
        };
    }
} ImpostorsInit;
