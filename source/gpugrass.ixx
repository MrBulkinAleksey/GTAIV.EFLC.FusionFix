module;

#include <common.hxx>
#include <d3dx9tex.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "FusionLog.hpp"

export module gpugrass;

import common;
import comvars;
import d3dx9_43;
import settings;
import inireload;
import framehistory;
import postfxresources;
import postfxprofiler;
import procedural;
import grasswind;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

#define IDR_GPUGRASS_FX 143

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxProfiler;

// GPU Grass (Graphics > Lighting, [PROCEDURAL] GpuGrass): blades of grass drawn by the GPU (GpuGrass.fx) on the
// ground the procedural manager gathers, into the G-buffer at the end of its pass (before the depth copy, so SSR, AO and
// contact shadows see it), lit by the game like any surface.
//
// Where: the manager's ground triangles (ProceduralGround, main thread) whose surface's procedural tag makes grass or
// small plants: by procedural.dat's PROCOBJ records of that tag whose model is on the grass list (GrassShared), the
// sum of their 1 / spacing^2 times 12, at most 1; and the mix of grass, weeds and small plants among those props, which
// the blades follow (GrassShared::GrassKind). The triangles are cut into pieces of at most kPieceArea square metres
// when the set changes; each frame the pieces in view within the range take a share of the mesh's kBlades blades, full
// up to GpuGrassFullRange and none at GpuGrassRange, and go to the instance buffer: one instanced draw for them all.
// The blades take half their colour from the ground under them: _DEFERRED_GBUFFER_0_ is copied first (into Wet Ground's
// copy, which that pass makes again after the grass).
//
// [PROCEDURAL] GpuGrassRange (35 m), GpuGrassFullRange (12 m), GpuGrassDensity (0.1..1). GpuGrassDebug (Graphics >
// Debug) colours the blades by their surface's tag. GpuGrassLog = 1 ([DEBUG]) writes the triangles, pieces and blades
// to GTAIV.EFLC.FusionFix.GpuGrass.log every 600 frames.
namespace GpuGrass
{
    constexpr int kBlades = 96;             // per piece of ground
    constexpr int kVertsPerBlade = 7;       // three pairs up the blade and its tip
    constexpr int kIndicesPerBlade = 15;    // five triangles
    constexpr float kPieceArea = 1.5f;      // square metres: up to 64 blades a square metre
    constexpr int kMaxPieces = 65536;
    constexpr int kMaxInstances = 24576;
    constexpr float kForgetDistance = 400.0f;   // triangles further from the camera when the pieces are made are dropped

    struct MeshVertex { float blade[4]; float vertex[4]; };
    struct Instance { float corner[4]; float edge1[4]; float edge2[4]; float kinds[4]; };

    // Main thread: the manager's triangles by their record
    std::mutex groundMutex;
    std::unordered_map<const void*, ProceduralGround::Triangle> ground;
    uint32_t groundVersion = 0;

    // Settings
    bool bLog = false;
    float fRange = 35.0f, fFullRange = 12.0f, fDensity = 1.0f;

    // Render thread
    struct Piece { float corner[3], edge1[3], edge2[3]; float centre[3]; float radius; float share; float seed; float tag; int tagIndex; };
    std::vector<Piece> pieces;
    uint32_t piecesVersion = ~0u;
    std::array<float, 256> tagShare{};
    std::array<std::array<float, 4>, 256> tagKinds{};   // the share of grass, of weeds, of small plants; tagShare
    const uint8_t* tagsFrom = nullptr;
    uint32_t tagsCount = 0;

    ID3DXEffect* effect = nullptr;
    bool effectTried = false;
    IDirect3DVertexBuffer9* meshVB = nullptr;
    IDirect3DIndexBuffer9* meshIB = nullptr;
    IDirect3DVertexBuffer9* instanceVB = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    struct Handles { D3DXHANDLE technique, viewProj, wind, windDir, blade, eye, nearPlane, farDivNear; } h{};
    const char* status = "not drawn yet";
    bool depthLogged = false;

    // GpuGrassLog
    uint32_t logFrames = 0;
    uint64_t logInstances = 0, logBlades = 0;
    double logCpuMs = 0.0;
    size_t logGrassTriangles = 0;

    float Hash(float x, float y)
    {
        const float v = std::sin(x * 12.9898f + y * 78.233f) * 43758.5453f;
        return v - std::floor(v);
    }

    void ReadIni(CIniReader& iniReader)
    {
        fRange = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GpuGrassRange", 35.0f), 5.0f, 150.0f);
        fFullRange = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GpuGrassFullRange", 12.0f), 0.0f, fRange - 1.0f);
        fDensity = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GpuGrassDensity", 1.0f), 0.1f, 1.0f);
        bLog = iniReader.ReadInteger("DEBUG", "GpuGrassLog", 0) != 0;
    }

    // How much grass each procedural tag grows, from procedural.dat's PROCOBJ records; again when they are loaded anew
    bool MakeTagShares()
    {
        static const uintptr_t modelInfos = GameBase() + 0xE95CD8; // CE 0x1295cd8, model infos by index
        const uint8_t* defs = ProceduralGround::Definitions();
        if (!defs)
            return false;
        const uint32_t count = *reinterpret_cast<const uint32_t*>(defs + 4);
        if (defs == tagsFrom && count == tagsCount)
            return true;
        std::array<float, 256> sum{};
        std::array<std::array<float, 3>, 256> kindSum{};
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t* record = defs + 8 + i * 0x44;
            const int32_t tag = *reinterpret_cast<const int32_t*>(record);
            const int32_t model = *reinterpret_cast<const int32_t*>(record + 4);
            const float spacing = *reinterpret_cast<const float*>(record + 8);
            if (tag < 0 || tag > 255 || model < 0 || model >= 31000 || !(spacing > 0.1f))
                continue;
            const auto info = *reinterpret_cast<const uintptr_t*>(modelInfos + model * 4);
            const int kind = info ? GrassShared::GrassKind(*reinterpret_cast<const uint32_t*>(info + 0x3c)) : 0;
            if (kind > 0)
            {
                sum[tag] += 1.0f / (spacing * spacing);
                kindSum[tag][kind - 1] += 1.0f / (spacing * spacing);
            }
        }
        std::string shares;
        for (int t = 0; t < 256; ++t)
        {
            tagShare[t] = std::clamp(sum[t] * 12.0f, 0.0f, 1.0f);
            tagKinds[t] = { 1.0f, 0.0f, 0.0f, tagShare[t] };
            if (sum[t] > 0.0f)
                for (int k = 0; k < 3; ++k)
                    tagKinds[t][k] = kindSum[t][k] / sum[t];
            if (tagShare[t] > 0.0f)
                shares += std::format(" {}:{:.2f}(g{:.0f}/w{:.0f}/p{:.0f})", t, tagShare[t], tagKinds[t][0] * 100.0f, tagKinds[t][1] * 100.0f,
                                      tagKinds[t][2] * 100.0f);
        }
        if (bLog)
            FusionLog::Write("GpuGrass", "Tags", "%u PROCOBJ records; grass by tag (share of grass/weeds/plants %%):%s\n", count, shares.c_str());
        tagsFrom = defs;
        tagsCount = count;
        piecesVersion = ~0u;
        return true;
    }

    void AddPieces(const float* a, const float* b, const float* c, float share, int tag, int depth)
    {
        const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        const float twice = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float area = 0.5f * twice;
        if (!(area > 1e-3f) || pieces.size() >= kMaxPieces)
            return;
        if (area > kPieceArea && depth < 16)
        {
            // Split the longest edge
            const float ab = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
            const float ac = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
            const float bc = (c[0] - b[0]) * (c[0] - b[0]) + (c[1] - b[1]) * (c[1] - b[1]) + (c[2] - b[2]) * (c[2] - b[2]);
            const auto mid = [](const float* p, const float* q, float* m) { for (int k = 0; k < 3; ++k) m[k] = (p[k] + q[k]) * 0.5f; };
            float m[3];
            if (ab >= ac && ab >= bc)
            {
                mid(a, b, m);
                AddPieces(a, m, c, share, tag, depth + 1);
                AddPieces(m, b, c, share, tag, depth + 1);
            }
            else if (ac >= bc)
            {
                mid(a, c, m);
                AddPieces(a, b, m, share, tag, depth + 1);
                AddPieces(m, b, c, share, tag, depth + 1);
            }
            else
            {
                mid(b, c, m);
                AddPieces(a, b, m, share, tag, depth + 1);
                AddPieces(a, m, c, share, tag, depth + 1);
            }
            return;
        }
        Piece p{};
        for (int k = 0; k < 3; ++k)
        {
            p.corner[k] = a[k], p.edge1[k] = e1[k], p.edge2[k] = e2[k];
            p.centre[k] = (a[k] + b[k] + c[k]) / 3.0f;
        }
        float r2 = 0.0f;
        for (const float* q : { a, b, c })
            r2 = (std::max)(r2, (q[0] - p.centre[0]) * (q[0] - p.centre[0]) + (q[1] - p.centre[1]) * (q[1] - p.centre[1]) + (q[2] - p.centre[2]) * (q[2] - p.centre[2]));
        p.radius = std::sqrt(r2) + 1.0f; // the blades stand up to a metre above it
        p.share = share * (std::min)(area / kPieceArea, 1.0f);
        p.seed = Hash(p.centre[0], p.centre[1]);
        p.tag = tag / 255.0f;
        p.tagIndex = tag;
        pieces.push_back(p);
    }

    // The pieces of the grassy triangles, when the manager's set changed
    void MakePieces(const float* camera)
    {
        std::vector<ProceduralGround::Triangle> triangles;
        {
            std::lock_guard lock(groundMutex);
            if (piecesVersion == groundVersion)
                return;
            piecesVersion = groundVersion;
            triangles.reserve(ground.size());
            for (auto it = ground.begin(); it != ground.end();)
            {
                const auto& t = it->second;
                const float dx = t.corner[0][0] - camera[0], dy = t.corner[0][1] - camera[1];
                if (dx * dx + dy * dy > kForgetDistance * kForgetDistance)
                {
                    it = ground.erase(it);
                    continue;
                }
                if (tagShare[t.tag] > 0.0f)
                    triangles.push_back(t);
                ++it;
            }
        }
        pieces.clear();
        for (const auto& t : triangles)
        {
            // Ground facing up: not walls or steep banks
            const float e1[3] = { t.corner[1][0] - t.corner[0][0], t.corner[1][1] - t.corner[0][1], t.corner[1][2] - t.corner[0][2] };
            const float e2[3] = { t.corner[2][0] - t.corner[0][0], t.corner[2][1] - t.corner[0][1], t.corner[2][2] - t.corner[0][2] };
            const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(length > 0.0f) || std::abs(n[2]) / length < 0.6f)
                continue;
            AddPieces(t.corner[0], t.corner[1], t.corner[2], tagShare[t.tag], t.tag, 0);
        }
        logGrassTriangles = triangles.size();
    }

    bool MakeResources(IDirect3DDevice9* pDevice)
    {
        if (!effect && !effectTried)
        {
            effectTried = true;
            HMODULE hm = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&MakeResources), &hm);
            ID3DXBuffer* errors = nullptr;
            if (PostFxResource::CreateEffectCached(pDevice, hm, IDR_GPUGRASS_FX, "GpuGrass", nullptr, &effect, &errors) != S_OK)
            {
                effect = nullptr;
                FusionLog::Write("GpuGrass", "Effect", "GpuGrass.fx failed: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no error text");
            }
            SAFE_RELEASE(errors);
            if (effect)
            {
                h.technique = effect->GetTechniqueByName("Grass");
                h.viewProj = effect->GetParameterByName(nullptr, "matViewProj");
                h.wind = effect->GetParameterByName(nullptr, "vec4Wind");
                h.windDir = effect->GetParameterByName(nullptr, "vec4WindDir");
                h.blade = effect->GetParameterByName(nullptr, "vec4Blade");
                h.eye = effect->GetParameterByName(nullptr, "vec4Eye");
                h.nearPlane = effect->GetParameterByName(nullptr, "fNearPlane");
                h.farDivNear = effect->GetParameterByName(nullptr, "fFarDivNear");
            }
        }
        if (!effect)
            return false;
        auto real = RealDevice(pDevice);
        if (!declaration)
        {
            const D3DVERTEXELEMENT9 elements[] =
            {
                { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
                { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
                { 1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
                { 1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
                { 1, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 },
                { 1, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 },
                D3DDECL_END()
            };
            if (FAILED(real->CreateVertexDeclaration(elements, &declaration)))
                return false;
        }
        if (!meshVB)
        {
            // kBlades blades, each at its own random place on the piece, turned and sized at random; the order is
            // random too, so a piece keeping its first blades keeps them spread over it
            if (FAILED(real->CreateVertexBuffer(kBlades * kVertsPerBlade * sizeof(MeshVertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &meshVB, nullptr)))
                return false;
            MeshVertex* v = nullptr;
            if (FAILED(meshVB->Lock(0, 0, reinterpret_cast<void**>(&v), 0)))
                return false;
            uint32_t state = 0x9E3779B9u;
            const auto next = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return (state & 0xFFFFFF) / float(0x1000000); };
            for (int b = 0; b < kBlades; ++b)
            {
                const float u = next(), w = next(), turn = next() * 6.2831853f, height = next(), random = next();
                const float place = (b + 0.5f) / kBlades;
                static constexpr float kUp[kVertsPerBlade] = { 0.0f, 0.0f, 0.35f, 0.35f, 0.7f, 0.7f, 1.0f };
                static constexpr float kAcross[kVertsPerBlade] = { -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f };
                for (int k = 0; k < kVertsPerBlade; ++k)
                    v[b * kVertsPerBlade + k] = { { u, w, turn, height }, { kUp[k], kAcross[k], place, random } };
            }
            meshVB->Unlock();
        }
        if (!meshIB)
        {
            if (FAILED(real->CreateIndexBuffer(kBlades * kIndicesPerBlade * sizeof(uint16_t), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &meshIB, nullptr)))
                return false;
            uint16_t* i = nullptr;
            if (FAILED(meshIB->Lock(0, 0, reinterpret_cast<void**>(&i), 0)))
                return false;
            for (int b = 0; b < kBlades; ++b)
            {
                const uint16_t o = uint16_t(b * kVertsPerBlade);
                const uint16_t list[kIndicesPerBlade] = { o, uint16_t(o + 1), uint16_t(o + 2), uint16_t(o + 2), uint16_t(o + 1), uint16_t(o + 3),
                                                          uint16_t(o + 2), uint16_t(o + 3), uint16_t(o + 4), uint16_t(o + 4), uint16_t(o + 3), uint16_t(o + 5),
                                                          uint16_t(o + 4), uint16_t(o + 5), uint16_t(o + 6) };
                std::memcpy(i + b * kIndicesPerBlade, list, sizeof(list));
            }
            meshIB->Unlock();
        }
        if (!instanceVB && FAILED(real->CreateVertexBuffer(kMaxInstances * sizeof(Instance), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                D3DPOOL_DEFAULT, &instanceVB, nullptr)))
        {
            instanceVB = nullptr;
            return false;
        }
        return true;
    }

    IDirect3DSurface9* Surface(const char* name)
    {
        auto rt = rage::grcTextureFactoryPC::GetRTByName(name);
        IDirect3DSurface9* surface = nullptr;
        if (rt && rt->mD3DTexture)
            rt->mD3DTexture->GetSurfaceLevel(0, &surface);
        return surface;
    }
}

export namespace GpuGrass
{
    // Before a device reset: the instance buffer is in the default pool
    void OnDeviceLost()
    {
        SAFE_RELEASE(instanceVB);
        if (effect)
            effect->OnLostDevice();
    }

    // At the end of the G-buffer pass (TemporalAA::OnGBufferEnd, the device state saved around it), before the depth
    // copy: the grass into the G-buffer.
    void Render(IDirect3DDevice9* pDevice)
    {
        static auto pref = FusionFixSettings.GetRef("PREF_GPU_GRASS");
        static auto debugPref = FusionFixSettings.GetRef("PREF_GPU_GRASS_DEBUG");
        // GpuGrassLog: the last 600 frames, with how the last one went
        if (bLog && ++logFrames >= 600)
        {
            size_t held;
            {
                std::lock_guard lock(groundMutex);
                held = ground.size();
            }
            FusionLog::Write("GpuGrass", "Stats", "%zu triangles held, %zu grassy, %zu pieces; per frame: %.0f pieces drawn, %.0f blades, CPU %.3f ms; last: %s\n",
                held, logGrassTriangles, pieces.size(), double(logInstances) / logFrames, double(logBlades) / logFrames, logCpuMs / logFrames, status);
            logFrames = 0;
            logInstances = logBlades = 0;
            logCpuMs = 0.0;
        }
        if (!pDevice || !pref || pref->get() == 0)
            return void(status = "off");
        const auto start = std::chrono::steady_clock::now();
        const auto& camera = FrameHistory::Current();
        if (!camera.Valid || !(camera.Near > 0.0f) || !(camera.Far > camera.Near))
            return void(status = "no camera");
        if (!MakeTagShares())
            return void(status = "procedural.dat not loaded");
        if (!MakeResources(pDevice))
            return void(status = "no effect or buffers");

        // The camera: a rigid view, its position -t R^T
        const auto& v = camera.View;
        const float eye[3] = {
            -(v._41 * v._11 + v._42 * v._12 + v._43 * v._13),
            -(v._41 * v._21 + v._42 * v._22 + v._43 * v._23),
            -(v._41 * v._31 + v._42 * v._32 + v._43 * v._33),
        };
        MakePieces(eye);

        // The view's planes (row vectors: clip = p M), each (n, d) with n.p + d >= 0 inside
        const auto& m = camera.ViewProjection;
        const float planes[5][4] = {
            { m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41 },
            { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 },
            { m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42 },
            { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 },
            { m._13, m._23, m._33, m._43 },
        };
        float planeLength[5];
        for (int i = 0; i < 5; ++i)
            planeLength[i] = std::sqrt(planes[i][0] * planes[i][0] + planes[i][1] * planes[i][1] + planes[i][2] * planes[i][2]);

        Instance* out = nullptr;
        if (FAILED(instanceVB->Lock(0, 0, reinterpret_cast<void**>(&out), D3DLOCK_DISCARD)))
            return void(status = "instance buffer lock failed");
        int count = 0;
        double blades = 0.0;
        const float fade = 1.0f / (std::max)(fRange - fFullRange, 0.5f);
        for (const auto& p : pieces)
        {
            const float dx = p.centre[0] - eye[0], dy = p.centre[1] - eye[1], dz = p.centre[2] - eye[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - p.radius;
            if (distance > fRange)
                continue;
            bool inside = true;
            for (int i = 0; i < 5 && inside; ++i)
                inside = planes[i][0] * p.centre[0] + planes[i][1] * p.centre[1] + planes[i][2] * p.centre[2] + planes[i][3] >= -p.radius * planeLength[i];
            if (!inside)
                continue;
            const float share = p.share * fDensity * std::clamp((fRange - (std::max)(distance, 0.0f)) * fade, 0.0f, 1.0f);
            if (share * kBlades < 0.5f)
                continue;
            const auto& kinds = tagKinds[p.tagIndex];
            out[count] = { { p.corner[0], p.corner[1], p.corner[2], p.seed }, { p.edge1[0], p.edge1[1], p.edge1[2], share },
                           { p.edge2[0], p.edge2[1], p.edge2[2], p.tag }, { kinds[0], kinds[1], kinds[2], kinds[3] } };
            blades += share * kBlades;
            if (++count >= kMaxInstances)
                break;
        }
        instanceVB->Unlock();
        if (bLog)
        {
            logInstances += count;
            logBlades += uint64_t(blades);
        }
        if (!count)
        {
            status = pieces.empty() ? "no grassy ground near" : "none in view";
            return;
        }

        // The G-buffer's first three targets; its depth buffer as the pass left it, the material IDs untouched
        IDirect3DSurface9* targets[3] = { Surface("_DEFERRED_GBUFFER_0_"), Surface("_DEFERRED_GBUFFER_1_"), Surface("_DEFERRED_GBUFFER_2_") };
        IDirect3DSurface9* depth = nullptr;
        pDevice->GetDepthStencilSurface(&depth);
        if (!depthLogged && bLog)
        {
            depthLogged = true;
            IDirect3DSurface9* gbufferDepth = Surface("_DEFERRED_GBUFFER_3_");
            D3DSURFACE_DESC dd{}, td{};
            if (depth)
                depth->GetDesc(&dd);
            if (targets[0])
                targets[0]->GetDesc(&td);
            FusionLog::Write("GpuGrass", "Targets", "depth buffer %p %ux%u format %u%s, colour %ux%u\n", static_cast<void*>(depth), dd.Width, dd.Height,
                unsigned(dd.Format), depth && depth == gbufferDepth ? " (_DEFERRED_GBUFFER_3_)" : "", td.Width, td.Height);
            SAFE_RELEASE(gbufferDepth);
        }
        if (!targets[0] || !targets[1] || !targets[2] || !depth)
        {
            for (auto& t : targets)
                SAFE_RELEASE(t);
            SAFE_RELEASE(depth);
            return void(status = "no G-buffer");
        }
        D3DSURFACE_DESC desc{};
        targets[0]->GetDesc(&desc);

        ProfilerScope timed(pDevice, kProfGpuGrass);
        // The ground's colour before the grass, which the blades take half of: into Wet Ground's copy of the first target
        auto& R = PostFxResources;
        bool groundCopied = false;
        {
            D3DSURFACE_DESC copyDesc{};
            if (R.WetCopySurf[0] && SUCCEEDED(R.WetCopySurf[0]->GetDesc(&copyDesc)) &&
                (copyDesc.Width != desc.Width || copyDesc.Height != desc.Height || copyDesc.Format != desc.Format))
            {
                SAFE_RELEASE(R.WetCopySurf[0]);
                SAFE_RELEASE(R.WetCopyTex[0]);
            }
            if (!R.WetCopyTex[0] && (FAILED(RealDevice(pDevice)->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                    D3DPOOL_DEFAULT, &R.WetCopyTex[0], nullptr)) || FAILED(R.WetCopyTex[0]->GetSurfaceLevel(0, &R.WetCopySurf[0]))))
            {
                SAFE_RELEASE(R.WetCopySurf[0]);
                SAFE_RELEASE(R.WetCopyTex[0]);
            }
            groundCopied = R.WetCopySurf[0] && SUCCEEDED(pDevice->StretchRect(targets[0], nullptr, R.WetCopySurf[0], nullptr, D3DTEXF_NONE));
        }
        // Kept besides what StateBackup keeps: every float constant (D3DX places the effect's from c0, over the game's
        // and FusionFix's cached ones such as the log depth's), the third target's colour mask, the streams' frequencies
        static float vsConstants[256 * 4], psConstants[224 * 4];
        pDevice->GetVertexShaderConstantF(0, vsConstants, 256);
        pDevice->GetPixelShaderConstantF(0, psConstants, 224);
        static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
                                                                  D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
        static constexpr DWORD kSamplerValues[] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, FALSE };
        IDirect3DBaseTexture9* oldTexture = nullptr;
        DWORD oldSampler[std::size(kSamplerStates)] = {};
        pDevice->GetTexture(0, &oldTexture);
        for (size_t i = 0; i < std::size(kSamplerStates); ++i)
            pDevice->GetSamplerState(0, kSamplerStates[i], &oldSampler[i]);
        DWORD colourWrite2 = 0, zfunc = D3DCMP_LESSEQUAL;
        pDevice->GetRenderState(D3DRS_COLORWRITEENABLE2, &colourWrite2);
        pDevice->GetRenderState(D3DRS_ZFUNC, &zfunc);

        for (DWORD i = 0; i < 3; ++i)
            pDevice->SetRenderTarget(i, targets[i]);
        pDevice->SetRenderTarget(3, nullptr);
        pDevice->SetDepthStencilSurface(depth);
        D3DVIEWPORT9 viewport = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);
        // The depth test as the G-buffer pass had it (its last draw's function)
        static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kStates[] =
        {
            { D3DRS_ZENABLE, TRUE }, { D3DRS_ZWRITEENABLE, TRUE }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
            { D3DRS_STENCILENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_COLORWRITEENABLE, 0x0F },
            { D3DRS_COLORWRITEENABLE1, 0x0F }, { D3DRS_COLORWRITEENABLE2, 0x0F }, { D3DRS_SCISSORTESTENABLE, FALSE },
            { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
            { D3DRS_DEPTHBIAS, 0 }, { D3DRS_SLOPESCALEDEPTHBIAS, 0 },
        };
        for (auto [state, value] : kStates)
            pDevice->SetRenderState(state, value);
        pDevice->SetRenderState(D3DRS_ZFUNC, zfunc == D3DCMP_ALWAYS || zfunc == D3DCMP_NEVER ? D3DCMP_LESSEQUAL : zfunc);

        effect->SetMatrix(h.viewProj, &camera.ViewProjection);
        const float wl = GrassShared::GustWavelength;
        const D3DXVECTOR4 wind(GrassShared::Direction[0] / wl, GrassShared::Direction[1] / wl, -GrassShared::GustPhase, GrassShared::FlutterPhase);
        const D3DXVECTOR4 windDir(GrassShared::Direction[0], GrassShared::Direction[1], GrassShared::BendPerHeight, GrassShared::FlutterPerHeight);
        const bool debug = debugPref && debugPref->get() != 0;
        const D3DXVECTOR4 blade(1.0f, 1.0f, groundCopied ? 0.5f : 0.0f, debug ? 1.0f : 0.0f);
        const D3DXVECTOR4 eyeVector(eye[0], eye[1], eye[2], 0.0f);
        effect->SetVector(h.wind, &wind);
        effect->SetVector(h.windDir, &windDir);
        effect->SetVector(h.blade, &blade);
        effect->SetVector(h.eye, &eyeVector);
        effect->SetFloat(h.nearPlane, camera.Near);
        effect->SetFloat(h.farDivNear, camera.Far / camera.Near);

        pDevice->SetVertexDeclaration(declaration);
        pDevice->SetStreamSource(0, meshVB, 0, sizeof(MeshVertex));
        pDevice->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | UINT(count));
        pDevice->SetStreamSource(1, instanceVB, 0, sizeof(Instance));
        pDevice->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
        pDevice->SetIndices(meshIB);

        UINT passes = 0;
        effect->SetTechnique(h.technique);
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        SetTextureBoth(pDevice, 0, groundCopied ? R.WetCopyTex[0] : nullptr);
        for (size_t i = 0; i < std::size(kSamplerStates); ++i)
            SetSamplerStateBoth(pDevice, 0, kSamplerStates[i], kSamplerValues[i]);
        pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, kBlades * kVertsPerBlade, 0, kBlades * 3);
        effect->EndPass();
        effect->End();

        pDevice->SetStreamSourceFreq(0, 1);
        pDevice->SetStreamSourceFreq(1, 1);
        pDevice->SetStreamSource(1, nullptr, 0, 0);
        SetTextureBoth(pDevice, 0, oldTexture);
        SAFE_RELEASE(oldTexture);
        for (size_t i = 0; i < std::size(kSamplerStates); ++i)
            SetSamplerStateBoth(pDevice, 0, kSamplerStates[i], oldSampler[i]);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE2, colourWrite2);
        pDevice->SetVertexShaderConstantF(0, vsConstants, 256);
        pDevice->SetPixelShaderConstantF(0, psConstants, 224);
        for (auto& t : targets)
            SAFE_RELEASE(t);
        SAFE_RELEASE(depth);
        status = "drawn";
        if (bLog)
            logCpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    const char* Status()
    {
        return status;
    }
}

class GpuGrassInit
{
public:
    GpuGrassInit()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            GpuGrass::ReadIni(iniReader);
            IniReload::Add(GpuGrass::ReadIni);
        };
        ProceduralGround::OnAdded = [](const void* key, const ProceduralGround::Triangle& triangle)
        {
            std::lock_guard lock(GpuGrass::groundMutex);
            GpuGrass::ground[key] = triangle;
            ++GpuGrass::groundVersion;
        };
        ProceduralGround::OnRemoved = [](const void* key)
        {
            std::lock_guard lock(GpuGrass::groundMutex);
            if (GpuGrass::ground.erase(key))
                ++GpuGrass::groundVersion;
        };
    }
} GpuGrassInit;
