module;

#include <common.hxx>
#include <d3dx9tex.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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
import temporal;
import postfxresources;
import postfxprofiler;
import postfxpass;
import procedural;
import grasswind;
import nearbybodies;
import shaders;
import screenquad;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

#define IDR_GPUGRASS_FX 143

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxProfiler;

// GPU Grass (Improved Grass in Graphics > Lighting, [PROCEDURAL] GpuGrass): cards of grass, each a clump of thin blades from a
// texture made here (MakeCardTexture), drawn by the GPU (GpuGrass.fx) on the
// ground the procedural manager gathers, into the G-buffer at the end of its pass (before the depth copy, so SSR, AO and
// contact shadows see it), lit by the game like any surface.
//
// Where: the manager's ground triangles (ProceduralGround, main thread) whose surface's procedural tag makes grass or
// weeds: by procedural.dat's PROCOBJ records of that tag whose model is grass or a weed on the grass list (GrassShared),
// the sum of their 1 / spacing^2 times 12, at most 1; and the mix of grass and weeds among those props, which the blades
// follow (GrassShared::GrassKind). Small plants (flower beds) stay the game's props; a tag with only those grows sparser
// grass between them (kPlantGroundShare). Lawns of the GRASS and SHORT_GRASS
// materials whose tag has no props, which the manager gathers for us (GatherGrassSurfaces), grow grass of their own
// (LookOf). The triangles are cut into pieces of at most kPieceArea square metres
// when the set changes; each frame the pieces in view within the range take a share of the mesh's kCards cards
// (BladeShare: all up to kDenseRange, half at GpuGrassFullRange, where far cards are fewer and wider, none at
// GpuGrassRange), and go to the instance buffer: one instanced draw for them all.
// The blades take a little of their colour from the ground under them: _DEFERRED_GBUFFER_0_ is copied first (into Wet Ground's
// copy, which that pass makes again after the grass). Before that copy the ground itself under the grass is darkened
// towards a grassy floor (GroundTint in GpuGrass.fx, TintGround): pale soil and gravel textures showed between the
// blades. Where EdgeTex has grass, on ground facing up of the materials Wet Ground wets, fading out at GpuGrassRange;
// hidden [PROCEDURAL] GpuGrassGroundTint (0.5, live; 0 off) is how far at full cover.
//
// Temporal AA, DLAA and FSR: the grass is drawn before the motion vectors are made from depth (OnGBufferBeforeMotion),
// so they have the camera's motion of the blades, and after them drawn again into the motion target (RenderMotion)
// with the wind's own: where each vertex was at the wind's phases of the previous frame. Without it the swaying tips
// were taken for wrong history and the grass shimmered. Far off (GpuGrass.fx) the blades' normals and colours go over
// to the lawn's, and one in two goes to make the others twice as wide, so thin blades don't flicker.
//
// Bending (GpuGrass.fx): the bodies Grass Bending takes (GrassShared::Bodies), the kBodies nearest the camera within the
// range, and Grass Trample's grid as a vertex texture (vs s1), as the grass props have them.
//
// GpuGrassHideProps ([PROCEDURAL], 1): the game's grass props (procedural.dat's models on the grass list) give way where
// the blades grow (ProceduralGround::HideGrass): one is in the way while its place's random value is under four times the
// blades' share at its distance, so they come back as the blades thin out, far off. The generator doesn't make those (their queue entries
// wait parked, as outside the cone) and lets go of those the camera came near; the render lists leave out any made
// meanwhile.
//
// Their colours: hidden [PROCEDURAL] GpuGrassGreen, GpuGrassTip, GpuGrassDry ("r, g, b", 0..1, live): the greenest
// patches, the tips, dry blades and patches.
//
// [PROCEDURAL] GpuGrassRange (100 m; the manager gathers ground that far ahead, less behind: ProceduralGround::GatherAhead), GpuGrassFullRange (22 m), GpuGrassDensity (0.1..1). GpuGrassDebug (Graphics >
// Debug) colours the blades by their surface's tag. GpuGrassLog = 1 ([DEBUG]) writes the triangles, pieces and blades
// to GTAIV.EFLC.FusionFix.GpuGrass.log every 600 frames.
namespace GpuGrass
{
    constexpr int kCards = 80;              // per piece of ground
    constexpr int kVertsPerCard = 6;        // three pairs up the card
    constexpr int kIndicesPerCard = 12;     // four triangles
    constexpr float kPieceArea = 1.5f;      // square metres: up to 53 cards, some 1300 blades a square metre
    constexpr float kDenseRange = 8.0f;     // metres: all of them this near, half at GpuGrassFullRange
    constexpr int kMaxPieces = 65536;
    constexpr int kMaxInstances = 24576;
    constexpr float kForgetDistance = 400.0f;   // triangles further from the camera when the pieces are made are dropped
    constexpr int kBodies = 8;                  // as GpuGrass.fx
    constexpr float kPlantGroundShare = 0.5f;   // grass on ground whose props are only small plants

    struct MeshVertex { float blade[4]; float vertex[4]; };
    // Edges: within kEdgeFade of ground without grass the blades thin out and shorten, so a lawn ends softly instead of
    // along its collision triangles. A grid of kEdgeCell cells kEdgeReach around the camera marks the grassy triangles;
    // each cell gets its distance to an unmarked one, over kEdgeFade (1 off grid)
    constexpr float kEdgeFade = 2.0f;
    constexpr float kEdgeCell = 0.5f;
    constexpr float kEdgeReach = 110.0f;    // GpuGrassRange is 100 m by default
    constexpr int kEdgeSide = int(2.0f * kEdgeReach / kEdgeCell);

    // Rounder and less even than the cells: the distance blurred over a metre (contour lines round off at a lawn's
    // corners) and moved by a smooth noise of the world, so the edge neither follows the cells nor runs straight
    constexpr int kEdgeBlur = 2;                // cells each side
    constexpr float kEdgeWobble = 0.6f, kEdgeWobbleLength = 2.5f;      // metres
    constexpr float kEdgeRipple = 0.25f, kEdgeRippleLength = 0.9f;
    // The edge factor of each cell (0..1), for EdgeTex (R16F, vertex sampler 2, filtered, read at each blade's root;
    // without it the edges are hard)
    std::vector<float> edgeField;
    float edgeOrigin[2] = {};
    bool edgeDirty = false;
    IDirect3DTexture9* edgeTexture = nullptr;
    bool edgeTextureTried = false;

    struct Instance { float corner[4]; float edge1[4]; float edge2[4]; float kinds[4]; };

    // Main thread: the manager's triangles by their record
    std::mutex groundMutex;
    std::unordered_map<const void*, ProceduralGround::Triangle> ground;
    uint32_t groundVersion = 0;

    // Settings
    bool bLog = false;
    bool bHideProps = true;
    float fRange = 100.0f, fFullRange = 22.0f, fDensity = 1.0f;
    float fGroundTint = 0.5f;
    float green[3] = { 0.30f, 0.38f, 0.11f }, tip[3] = { 0.62f, 0.60f, 0.26f }, dry[3] = { 0.46f, 0.39f, 0.20f };
    constexpr float kGroundTintColour[3] = { 0.45f, 0.55f, 0.3f };   // what the ground is multiplied towards

    // Render thread
    // What grows on a triangle: how much, the share of grass, of weeds, of small plants, the height (0..1), the debug colour
    struct Look { float share; float kinds[4]; float tag; };
    struct Piece { float corner[3], edge1[3], edge2[3]; float centre[3]; float radius; float share; float seed; Look look; };
    std::vector<Piece> pieces;
    uint32_t piecesVersion = ~0u;
    std::array<float, 256> tagShare{};
    std::array<std::array<float, 4>, 256> tagKinds{};   // the share of grass, of weeds, of small plants; tagShare
    // By model index, read by the render lists (HideGrass): 1 for procedural.dat's grass models; whether the blades are
    // drawn (the last frame got as far as the draw)
    std::array<uint8_t, 32768> grassModel{};
    std::atomic<bool> drawing = false;
    const uint8_t* tagsFrom = nullptr;
    uint32_t tagsCount = 0;

    ID3DXEffect* effect = nullptr;
    bool effectTried = false;
    IDirect3DVertexBuffer9* meshVB = nullptr;
    IDirect3DIndexBuffer9* meshIB = nullptr;
    IDirect3DVertexBuffer9* instanceVB = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    IDirect3DTexture9* cardTexture = nullptr;
    struct Handles
    {
        D3DXHANDLE technique, viewProj, wind, windDir, blade, eye, nearPlane, farDivNear, bodies, bend, sun;
        D3DXHANDLE motion, viewProjNoJitter, prevViewProj, windPrev, depthBias, edgeGrid, ditherFrame;
        D3DXHANDLE green, tip, dry;
        D3DXHANDLE groundTint, projInfo, worldX, worldY, worldZ, groundTintColour, tintAllow0, tintAllow1, tintRange;
    } h{};
    // What RenderMotion draws again: this frame's instances, if Render drew them, the depth test it drew with, and
    // the wind's vector of this frame and the one before
    uint32_t drawnFrame = 0;
    int drawnCount = 0;
    DWORD drawnZFunc = D3DCMP_LESSEQUAL;
    D3DXVECTOR4 windNow(0.0f, 0.0f, 0.0f, 0.0f), windBefore(0.0f, 0.0f, 0.0f, 0.0f);
    bool windKnown = false;
    const char* status = "not drawn yet";
    bool depthLogged = false;

    // GpuGrassLog
    uint32_t logFrames = 0;
    uint64_t logInstances = 0, logBlades = 0;
    double logCpuMs = 0.0;
    size_t logGrassTriangles = 0;

    // The share of a piece's blades drawn at `distance`: all up to kDenseRange, half at GpuGrassFullRange (where, further
    // off, each blade is a tuft of three: GpuGrass.fx), none at GpuGrassRange
    float BladeShare(float distance)
    {
        const float d = (std::max)(distance, 0.0f);
        if (d <= kDenseRange)
            return 1.0f;
        if (d <= fFullRange)
            return 1.0f - 0.5f * (d - kDenseRange) / (std::max)(fFullRange - kDenseRange, 0.5f);
        return 0.5f * std::clamp((fRange - d) / (std::max)(fRange - fFullRange, 0.5f), 0.0f, 1.0f);
    }

    // 1 / the share of a piece's blades over which the last ones kept rise out of the ground (GpuGrass.fx MakeBlade):
    // what the share grows by over kRiseDistance as the camera comes nearer, so a blade rises over that much of the
    // way at any distance; BladeShare's slopes blend around GpuGrassFullRange so no blade jumps in height there
    float RiseScale(float distance, float pieceShare)
    {
        constexpr float kRiseDistance = 2.0f;
        const float nearSlope = 0.5f / (std::max)(fFullRange - kDenseRange, 0.5f);
        const float farSlope = 0.5f / (std::max)(fRange - fFullRange, 0.5f);
        const float blend = std::clamp((distance - fFullRange + 4.0f) / 8.0f, 0.0f, 1.0f);
        const float slope = pieceShare * (nearSlope + (farSlope - nearSlope) * blend * blend * (3.0f - 2.0f * blend));
        return (std::max)(12.0f, 1.0f / (std::max)(slope * kRiseDistance, 1e-4f));
    }

    float Hash(float x, float y)
    {
        const float v = std::sin(x * 12.9898f + y * 78.233f) * 43758.5453f;
        return v - std::floor(v);
    }

    void ReadIni(CIniReader& iniReader)
    {
        fRange = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GpuGrassRange", 100.0f), 5.0f, 150.0f);
        fFullRange = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GpuGrassFullRange", 22.0f), 0.0f, fRange - 1.0f);
        bHideProps = iniReader.ReadInteger("PROCEDURAL", "GpuGrassHideProps", 1) != 0;
        fDensity = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GpuGrassDensity", 1.0f), 0.1f, 1.0f);
        fGroundTint = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GpuGrassGroundTint", 0.5f), 0.0f, 1.0f);
        bLog = iniReader.ReadInteger("DEBUG", "GpuGrassLog", 0) != 0;
        const auto colour = [&](const char* key, float* rgb, const float* fallback)
        {
            const std::string text = iniReader.ReadString("PROCEDURAL", key, "");
            float v[3];
            const bool read = std::sscanf(text.c_str(), "%f , %f , %f", &v[0], &v[1], &v[2]) == 3;
            for (int k = 0; k < 3; ++k)
                rgb[k] = read ? std::clamp(v[k], 0.0f, 1.0f) : fallback[k];
        };
        static constexpr float kGreen[3] = { 0.30f, 0.38f, 0.11f }, kTip[3] = { 0.62f, 0.60f, 0.26f }, kDry[3] = { 0.46f, 0.39f, 0.20f };
        colour("GpuGrassGreen", green, kGreen);
        colour("GpuGrassTip", tip, kTip);
        colour("GpuGrassDry", dry, kDry);
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
        std::array<bool, 256> plants{};
        grassModel.fill(0);
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
            // Small plants (flower beds) stay the game's props
            if (kind == 1 || kind == 2)
            {
                grassModel[model] = 1;
                sum[tag] += 1.0f / (spacing * spacing);
                kindSum[tag][kind - 1] += 1.0f / (spacing * spacing);
            }
            else if (kind == 3)
                plants[tag] = true;
        }
        std::string shares;
        for (int t = 0; t < 256; ++t)
        {
            tagShare[t] = std::clamp(sum[t] * 12.0f, 0.0f, 1.0f);
            tagKinds[t] = { 1.0f, 0.0f, 0.0f, tagShare[t] };
            if (sum[t] > 0.0f)
                for (int k = 0; k < 3; ++k)
                    tagKinds[t][k] = kindSum[t][k] / sum[t];
            else if (plants[t])
            {
                // Small plants alone: the ground between them is a meadow too (whole collision triangles of it were bare
                // amid grass), sparser, shorter, some weeds; the plants stay the game's props
                tagShare[t] = kPlantGroundShare;
                tagKinds[t] = { 0.7f, 0.3f, 0.0f, 0.4f };
            }
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

    // By the triangle's procedural tag where its props have grass or weeds, else by its material: lawns of GRASS and
    // SHORT_GRASS without props (ProceduralGround::GatherGrassSurfaces), all grass, the short one lower and sparser
    bool LookOf(const ProceduralGround::Triangle& t, Look& look)
    {
        if (tagShare[t.tag] > 0.0f)
        {
            const auto& k = tagKinds[t.tag];
            look = { tagShare[t.tag], { k[0], k[1], k[2], k[3] }, t.tag / 255.0f };
            return true;
        }
        if (t.surface == 1)
            look = { 1.0f, { 1.0f, 0.0f, 0.0f, 0.8f }, 251.0f / 255.0f };
        else if (t.surface == 2)
            look = { 0.75f, { 1.0f, 0.0f, 0.0f, 0.15f }, 252.0f / 255.0f };
        return t.surface != 0;
    }

    void AddPieces(const float* a, const float* b, const float* c, const Look& look, int depth)
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
                AddPieces(a, m, c, look, depth + 1);
                AddPieces(m, b, c, look, depth + 1);
            }
            else if (ac >= bc)
            {
                mid(a, c, m);
                AddPieces(a, b, m, look, depth + 1);
                AddPieces(m, b, c, look, depth + 1);
            }
            else
            {
                mid(b, c, m);
                AddPieces(a, b, m, look, depth + 1);
                AddPieces(a, m, c, look, depth + 1);
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
        p.share = look.share * (std::min)(area / kPieceArea, 1.0f);
        p.seed = Hash(p.centre[0], p.centre[1]);
        p.look = look;
        pieces.push_back(p);
    }

    // Smooth value noise of the world, 0..1
    float ValueNoise(float x, float y)
    {
        const float fx0 = std::floor(x), fy0 = std::floor(y);
        const int ix = int(fx0), iy = int(fy0);
        const auto lattice = [](int i, int j)
        {
            uint32_t h = uint32_t(i) * 374761393u + uint32_t(j) * 668265263u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return float((h ^ (h >> 16)) & 0xFFFFFF) / float(0x1000000);
        };
        float fx = x - fx0, fy = y - fy0;
        fx = fx * fx * (3.0f - 2.0f * fx);
        fy = fy * fy * (3.0f - 2.0f * fy);
        const float a = lattice(ix, iy), b = lattice(ix + 1, iy), c = lattice(ix, iy + 1), d = lattice(ix + 1, iy + 1);
        return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
    }

    // The edge grid's cells whose centre lies on the triangle seen from above, or within a fifth of a cell of it, so
    // slivers and the seams between triangles leave no false edges
    void MarkGrass(const ProceduralGround::Triangle& t, float originX, float originY, std::vector<float>& grid)
    {
        float x[3], y[3];
        for (int k = 0; k < 3; ++k)
            x[k] = (t.corner[k][0] - originX) / kEdgeCell, y[k] = (t.corner[k][1] - originY) / kEdgeCell;
        const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (std::abs(area) < 1e-6f)
            return;
        const float sign = area > 0.0f ? 1.0f : -1.0f;
        float edgeLength[3];
        for (int k = 0; k < 3; ++k)
        {
            const int j = (k + 1) % 3;
            edgeLength[k] = std::sqrt((x[j] - x[k]) * (x[j] - x[k]) + (y[j] - y[k]) * (y[j] - y[k]));
        }
        const int x0 = (std::max)(int(std::floor((std::min)({ x[0], x[1], x[2] }))), 0);
        const int x1 = (std::min)(int(std::ceil((std::max)({ x[0], x[1], x[2] }))), kEdgeSide - 1);
        const int y0 = (std::max)(int(std::floor((std::min)({ y[0], y[1], y[2] }))), 0);
        const int y1 = (std::min)(int(std::ceil((std::max)({ y[0], y[1], y[2] }))), kEdgeSide - 1);
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx)
            {
                const float px = float(cx) + 0.5f, py = float(cy) + 0.5f;
                bool inside = true;
                for (int k = 0; k < 3 && inside; ++k)
                {
                    const int j = (k + 1) % 3;
                    const float side = sign * ((x[j] - x[k]) * (py - y[k]) - (y[j] - y[k]) * (px - x[k]));
                    inside = side >= -0.2f * edgeLength[k];
                }
                if (inside)
                    grid[size_t(cy) * kEdgeSide + cx] = 1.0f;
            }
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
                if (tagShare[t.tag] > 0.0f || t.surface)
                    triangles.push_back(t);
                ++it;
            }
        }
        pieces.clear();
        // In whole cells of the world: a grid following the camera by fractions of a cell marked other cells of the same
        // triangles each time, and the blades at an edge came and went as the camera moved
        const float originX = std::floor((camera[0] - kEdgeReach) / kEdgeCell) * kEdgeCell;
        const float originY = std::floor((camera[1] - kEdgeReach) / kEdgeCell) * kEdgeCell;
        static std::vector<float> distance;     // in cells; 0 where no grass grows
        distance.assign(size_t(kEdgeSide) * kEdgeSide, 0.0f);
        for (const auto& t : triangles)
        {
            // Ground facing up: not walls or steep banks
            const float e1[3] = { t.corner[1][0] - t.corner[0][0], t.corner[1][1] - t.corner[0][1], t.corner[1][2] - t.corner[0][2] };
            const float e2[3] = { t.corner[2][0] - t.corner[0][0], t.corner[2][1] - t.corner[0][1], t.corner[2][2] - t.corner[0][2] };
            const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            Look look;
            if (!(length > 0.0f) || std::abs(n[2]) / length < 0.6f || !LookOf(t, look))
                continue;
            AddPieces(t.corner[0], t.corner[1], t.corner[2], look, 0);
            MarkGrass(t, originX, originY, distance);
        }
        logGrassTriangles = triangles.size();

        // Each marked cell's distance to an unmarked one (chamfer, 1 and sqrt 2), then the pieces' corners
        constexpr float kBig = 1e6f, kDiagonal = 1.41421356f;
        for (auto& d : distance)
            d = d > 0.0f ? kBig : 0.0f;
        const auto at = [](int x, int y) { return size_t(y) * kEdgeSide + x; };
        for (int y = 0; y < kEdgeSide; ++y)
            for (int x = 0; x < kEdgeSide; ++x)
            {
                float& d = distance[at(x, y)];
                if (d == 0.0f)
                    continue;
                // Off the grid counts as grass, so the grid's own border is no edge
                const float left = x > 0 ? distance[at(x - 1, y)] : kBig, up = y > 0 ? distance[at(x, y - 1)] : kBig;
                const float upLeft = x > 0 && y > 0 ? distance[at(x - 1, y - 1)] : kBig;
                const float upRight = x + 1 < kEdgeSide && y > 0 ? distance[at(x + 1, y - 1)] : kBig;
                d = (std::min)({ d, left + 1.0f, up + 1.0f, upLeft + kDiagonal, upRight + kDiagonal });
            }
        for (int y = kEdgeSide - 1; y >= 0; --y)
            for (int x = kEdgeSide - 1; x >= 0; --x)
            {
                float& d = distance[at(x, y)];
                if (d == 0.0f)
                    continue;
                const float right = x + 1 < kEdgeSide ? distance[at(x + 1, y)] : kBig, down = y + 1 < kEdgeSide ? distance[at(x, y + 1)] : kBig;
                const float downRight = x + 1 < kEdgeSide && y + 1 < kEdgeSide ? distance[at(x + 1, y + 1)] : kBig;
                const float downLeft = x > 0 && y + 1 < kEdgeSide ? distance[at(x - 1, y + 1)] : kBig;
                d = (std::min)({ d, right + 1.0f, down + 1.0f, downRight + kDiagonal, downLeft + kDiagonal });
            }
        // Blurred (a running box, rows then columns; past what the fade and the wobble reach it makes no difference),
        // wobbled, as the edge factor
        constexpr float kReach = (kEdgeFade + kEdgeWobble + kEdgeRipple) / kEdgeCell + 2.0f * kEdgeBlur + 1.0f;
        for (auto& d : distance)
            d = (std::min)(d, kReach);
        static std::vector<float> blurred;
        blurred.assign(distance.size(), 0.0f);
        constexpr float kBox = 1.0f / float(2 * kEdgeBlur + 1);
        for (int pass = 0; pass < 2; ++pass)
        {
            const auto& from = pass == 0 ? distance : blurred;
            auto& to = pass == 0 ? blurred : distance;
            for (int line = 0; line < kEdgeSide; ++line)
            {
                // Off the grid counts as grass, as for the distance
                const auto value = [&](int i) { return i < 0 || i >= kEdgeSide ? kReach : (pass == 0 ? from[at(i, line)] : from[at(line, i)]); };
                float sum = 0.0f;
                for (int i = -kEdgeBlur; i <= kEdgeBlur; ++i)
                    sum += value(i);
                for (int i = 0; i < kEdgeSide; ++i)
                {
                    (pass == 0 ? to[at(i, line)] : to[at(line, i)]) = sum * kBox;
                    sum += value(i + kEdgeBlur + 1) - value(i - kEdgeBlur);
                }
            }
        }
        edgeField.assign(distance.size(), 0.0f);
        for (int y = 0; y < kEdgeSide; ++y)
            for (int x = 0; x < kEdgeSide; ++x)
            {
                const float d = distance[at(x, y)];
                float& e = edgeField[at(x, y)];
                // A marked cell next to an unmarked one is 1: the edge itself lies half a cell nearer
                const float metres = (d - 0.5f) * kEdgeCell;
                if (d <= 0.0f)
                    e = 0.0f;
                else if (metres - kEdgeWobble - kEdgeRipple >= kEdgeFade)
                    e = 1.0f;
                else
                {
                    const float wx = originX + (float(x) + 0.5f) * kEdgeCell, wy = originY + (float(y) + 0.5f) * kEdgeCell;
                    const float wobble = (ValueNoise(wx / kEdgeWobbleLength, wy / kEdgeWobbleLength) - 0.5f) * 2.0f * kEdgeWobble +
                                         (ValueNoise(wx / kEdgeRippleLength + 17.3f, wy / kEdgeRippleLength + 5.1f) - 0.5f) * 2.0f * kEdgeRipple;
                    e = std::clamp((metres + wobble) / kEdgeFade, 0.0f, 1.0f);
                }
            }
        edgeOrigin[0] = originX;
        edgeOrigin[1] = originY;
        edgeDirty = true;
    }

    // EdgeTex: made once where the GPU reads filtered R16F vertex textures, filled again when the edges were made anew.
    // False while it doesn't hold this frame's edges (hard edges then)
    bool UpdateEdgeTexture(IDirect3DDevice9* pDevice)
    {
        auto real = RealDevice(pDevice);
        if (!edgeTexture && !edgeTextureTried)
        {
            edgeTextureTried = true;
            IDirect3D9* d3d = nullptr;
            bool supported = false;
            if (SUCCEEDED(real->GetDirect3D(&d3d)) && d3d)
            {
                D3DDEVICE_CREATION_PARAMETERS cp{};
                D3DDISPLAYMODE mode{};
                real->GetCreationParameters(&cp);
                d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode);
                supported = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                    D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_R16F));
                d3d->Release();
            }
            if (supported && FAILED(real->CreateTexture(kEdgeSide, kEdgeSide, 1, 0, D3DFMT_R16F, D3DPOOL_MANAGED, &edgeTexture, nullptr)))
                edgeTexture = nullptr;
            if (bLog)
                FusionLog::Write("GpuGrass", "Edges", "edge texture %s\n", edgeTexture ? "made" : supported ? "failed" : "not supported (filtered R16F vertex texture)");
        }
        if (!edgeTexture || edgeField.size() != size_t(kEdgeSide) * kEdgeSide)
            return false;
        if (edgeDirty)
        {
            D3DLOCKED_RECT locked{};
            if (FAILED(edgeTexture->LockRect(0, &locked, nullptr, 0)))
                return false;
            for (int y = 0; y < kEdgeSide; ++y)
                D3DXFloat32To16Array(reinterpret_cast<D3DXFLOAT16*>(static_cast<uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch),
                    edgeField.data() + size_t(y) * kEdgeSide, kEdgeSide);
            edgeTexture->UnlockRect(0);
            edgeDirty = false;
        }
        return true;
    }

    // CardTex (GpuGrass.fx): four clumps of blades side by side, kCardColumn × kCardHeight each: two of grass (dense
    // and fine, sparser and longer), one of weeds and broad leaves, one of grass with a few flowers. Not colours: each
    // texel holds its blade's shade (r: 0..0.9, flower heads 1), how far up the blade (g), where across it (b, 0..1),
    // coverage (a). The mips are averaged, then their coverage scaled so as many texels pass the shader's clip at a
    // half as at the full size: averaged alone, thin blades faded away with distance.
    constexpr int kCardColumn = 256, kCardHeight = 512, kCardColumns = 4, kCardMips = 7;

    bool MakeCardTexture(IDirect3DDevice9* real)
    {
        constexpr int width = kCardColumn * kCardColumns;
        std::vector<float> texels(size_t(width) * kCardHeight * 4, 0.0f);
        uint32_t state = 0x2545F491u;
        const auto next = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return (state & 0xFFFFFF) / float(0x1000000); };
        // One blade: its root across the column (0..1), height (0..1 of the card), how far its tip goes across, its
        // width at the root (texels), its shade; leaves widest in the middle
        const auto blade = [&](int column, float root, float height, float lean, float rootWidth, float shade, bool leaf)
        {
            const int rows = (std::max)(int(height * kCardHeight), 2);
            for (int row = 0; row < rows; ++row)
            {
                const float t = float(row) / float(rows - 1);
                const float centre = (root + lean * std::pow(t, 1.6f)) * kCardColumn;
                const float half = 0.5f * (leaf ? rootWidth * (0.25f + std::sin(t * 3.1415927f) * 0.9f) * (1.0f - t * 0.5f)
                                                : rootWidth * std::pow(1.0f - t, 0.7f) + 0.35f);
                const int y = kCardHeight - 1 - row;
                for (int x = int(std::floor(centre - half - 1.0f)); x <= int(std::ceil(centre + half + 1.0f)); ++x)
                {
                    // Blades stay three texels clear of the column's sides, which the filter would carry over
                    if (x < 3 || x >= kCardColumn - 3)
                        continue;
                    const float offset = float(x) + 0.5f - centre;
                    const float cover = std::clamp(half + 0.5f - std::abs(offset), 0.0f, 1.0f);
                    if (cover <= 0.0f)
                        continue;
                    float* texel = &texels[(size_t(y) * width + size_t(column * kCardColumn + x)) * 4];
                    // The blade in front: drawn later, over those before it
                    if (cover >= 0.5f || texel[3] < 0.5f)
                    {
                        texel[0] = shade;
                        texel[1] = t;
                        texel[2] = std::clamp(offset / (std::max)(half, 0.5f) * 0.5f + 0.5f, 0.0f, 1.0f);
                    }
                    texel[3] = (std::max)(texel[3], cover);
                }
            }
        };
        // Roots gathered towards the middle, tips spreading out (as a clump grows)
        const auto clump = [&](int column, int count, float minHeight, float spread, float rootWidth, bool leaves)
        {
            for (int i = 0; i < count; ++i)
            {
                const float root = 0.5f + (next() + next() - 1.0f) * 0.3f;
                const float height = minHeight + (1.0f - minHeight) * std::sqrt(next());
                const float lean = (root - 0.5f) * spread + (next() - 0.5f) * 0.25f;
                const float tipAt = std::clamp(root + lean, 0.06f, 0.94f);
                blade(column, root, height, tipAt - root, rootWidth * (0.75f + 0.5f * next()), next() * 0.9f, leaves);
            }
        };
        clump(0, 46, 0.45f, 0.9f, 5.0f, false);
        clump(1, 30, 0.6f, 1.3f, 6.0f, false);
        clump(2, 9, 0.5f, 1.4f, 26.0f, true);
        clump(2, 10, 0.4f, 1.0f, 6.0f, false);
        clump(3, 32, 0.4f, 0.9f, 5.0f, false);
        // Flowers: a thin stalk, its head a disc at the top (shade 1)
        for (int i = 0; i < 4; ++i)
        {
            const float root = 0.15f + 0.7f * next(), height = 0.82f + 0.15f * next(), lean = (next() - 0.5f) * 0.2f;
            blade(3, root, height, lean, 2.5f, 0.5f, false);
            const float cx = (root + lean) * kCardColumn, cy = float(kCardHeight) * (1.0f - height), radius = 6.0f + 3.0f * next();
            for (int y = int(cy - radius - 1.0f); y <= int(cy + radius + 1.0f); ++y)
                for (int x = int(cx - radius - 1.0f); x <= int(cx + radius + 1.0f); ++x)
                {
                    if (x < 3 || x >= kCardColumn - 3 || y < 1 || y >= kCardHeight)
                        continue;
                    const float d = std::sqrt((float(x) + 0.5f - cx) * (float(x) + 0.5f - cx) + (float(y) + 0.5f - cy) * (float(y) + 0.5f - cy) * 1.6f);
                    const float cover = std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
                    if (cover <= 0.0f)
                        continue;
                    float* texel = &texels[(size_t(y) * width + size_t(3 * kCardColumn + x)) * 4];
                    texel[0] = 1.0f;
                    texel[1] = 1.0f;
                    texel[2] = std::clamp((float(x) + 0.5f - cx) / radius * 0.5f + 0.5f, 0.0f, 1.0f);
                    texel[3] = (std::max)(texel[3], cover);
                }
        }

        if (FAILED(real->CreateTexture(width, kCardHeight, kCardMips, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cardTexture, nullptr)))
        {
            cardTexture = nullptr;
            return false;
        }
        const auto passing = [](const std::vector<float>& level, float scale)
        {
            size_t n = 0;
            for (size_t i = 3; i < level.size(); i += 4)
                n += level[i] * scale >= 0.5f;
            return n;
        };
        std::vector<float> level = texels;
        int w = width, h = kCardHeight;
        const double coverage = double(passing(level, 1.0f)) / double(size_t(w) * h);
        for (int mip = 0; mip < kCardMips; ++mip)
        {
            float scale = 1.0f;
            if (mip > 0)
            {
                // Averaged from the level above, colours by their coverage
                std::vector<float> smaller(size_t(w / 2) * (h / 2) * 4, 0.0f);
                for (int y = 0; y < h / 2; ++y)
                    for (int x = 0; x < w / 2; ++x)
                    {
                        float sum[4] = {};
                        for (int k = 0; k < 4; ++k)
                        {
                            const float* texel = &level[(size_t(y * 2 + k / 2) * w + size_t(x * 2 + k % 2)) * 4];
                            for (int c = 0; c < 3; ++c)
                                sum[c] += texel[c] * texel[3];
                            sum[3] += texel[3];
                        }
                        float* out = &smaller[(size_t(y) * (w / 2) + x) * 4];
                        for (int c = 0; c < 3; ++c)
                            out[c] = sum[3] > 0.0f ? sum[c] / sum[3] : 0.0f;
                        out[3] = sum[3] * 0.25f;
                    }
                level.swap(smaller);
                w /= 2, h /= 2;
                float low = 0.5f, high = 8.0f;
                const double wanted = coverage * double(size_t(w) * h);
                for (int i = 0; i < 16; ++i)
                {
                    scale = 0.5f * (low + high);
                    (double(passing(level, scale)) < wanted ? low : high) = scale;
                }
            }
            D3DLOCKED_RECT locked{};
            if (FAILED(cardTexture->LockRect(mip, &locked, nullptr, 0)))
            {
                SAFE_RELEASE(cardTexture);
                return false;
            }
            for (int y = 0; y < h; ++y)
            {
                auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch);
                for (int x = 0; x < w; ++x)
                {
                    const float* texel = &level[(size_t(y) * w + x) * 4];
                    const auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    // A8R8G8B8: the shader's r, g, b, a
                    row[x] = (byte(texel[3] * scale) << 24) | (byte(texel[0]) << 16) | (byte(texel[1]) << 8) | byte(texel[2]);
                }
            }
            cardTexture->UnlockRect(mip);
        }
        if (bLog)
            FusionLog::Write("GpuGrass", "Cards", "card texture %dx%d, %d mips, %.1f%% covered\n", width, kCardHeight, kCardMips, coverage * 100.0);
        return true;
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
                h.bodies = effect->GetParameterByName(nullptr, "vec4Bodies");
                h.bend = effect->GetParameterByName(nullptr, "vec4Bend");
                h.sun = effect->GetParameterByName(nullptr, "vec4Sun");
                h.nearPlane = effect->GetParameterByName(nullptr, "fNearPlane");
                h.farDivNear = effect->GetParameterByName(nullptr, "fFarDivNear");
                h.motion = effect->GetTechniqueByName("Motion");
                h.viewProjNoJitter = effect->GetParameterByName(nullptr, "matViewProjNoJitter");
                h.prevViewProj = effect->GetParameterByName(nullptr, "matPrevViewProj");
                h.windPrev = effect->GetParameterByName(nullptr, "vec4WindPrev");
                h.depthBias = effect->GetParameterByName(nullptr, "fDepthBias");
                h.edgeGrid = effect->GetParameterByName(nullptr, "vec4EdgeGrid");
                h.ditherFrame = effect->GetParameterByName(nullptr, "fDitherFrame");
                h.green = effect->GetParameterByName(nullptr, "vec4Green");
                h.tip = effect->GetParameterByName(nullptr, "vec4Tip");
                h.dry = effect->GetParameterByName(nullptr, "vec4Dry");
                h.groundTint = effect->GetTechniqueByName("GroundTint");
                h.projInfo = effect->GetParameterByName(nullptr, "vec4ProjInfo");
                h.worldX = effect->GetParameterByName(nullptr, "vec4WorldX");
                h.worldY = effect->GetParameterByName(nullptr, "vec4WorldY");
                h.worldZ = effect->GetParameterByName(nullptr, "vec4WorldZ");
                h.groundTintColour = effect->GetParameterByName(nullptr, "vec4GroundTint");
                h.tintAllow0 = effect->GetParameterByName(nullptr, "vec4TintAllow0");
                h.tintAllow1 = effect->GetParameterByName(nullptr, "vec4TintAllow1");
                h.tintRange = effect->GetParameterByName(nullptr, "fTintRange");
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
            // kCards cards, each at its own random place on the piece, turned and sized at random; the order is
            // random too, so a piece keeping its first cards keeps them spread over it
            if (FAILED(real->CreateVertexBuffer(kCards * kVertsPerCard * sizeof(MeshVertex), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &meshVB, nullptr)))
                return false;
            MeshVertex* v = nullptr;
            if (FAILED(meshVB->Lock(0, 0, reinterpret_cast<void**>(&v), 0)))
                return false;
            uint32_t state = 0x9E3779B9u;
            const auto next = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return (state & 0xFFFFFF) / float(0x1000000); };
            for (int b = 0; b < kCards; ++b)
            {
                const float u = next(), w = next(), turn = next() * 6.2831853f, height = next(), random = next();
                const float place = (b + 0.5f) / kCards;
                // A row in the middle, so the wind bends the card
                static constexpr float kUp[kVertsPerCard] = { 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f };
                static constexpr float kAcross[kVertsPerCard] = { -1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f };
                for (int k = 0; k < kVertsPerCard; ++k)
                    v[b * kVertsPerCard + k] = { { u, w, turn, height }, { kUp[k], kAcross[k], place, random } };
            }
            meshVB->Unlock();
        }
        if (!meshIB)
        {
            if (FAILED(real->CreateIndexBuffer(kCards * kIndicesPerCard * sizeof(uint16_t), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &meshIB, nullptr)))
                return false;
            uint16_t* i = nullptr;
            if (FAILED(meshIB->Lock(0, 0, reinterpret_cast<void**>(&i), 0)))
                return false;
            for (int b = 0; b < kCards; ++b)
            {
                const uint16_t o = uint16_t(b * kVertsPerCard);
                uint16_t list[kIndicesPerCard];
                int n = 0;
                for (int pair = 0; pair < 2; ++pair)
                {
                    const uint16_t q = uint16_t(o + pair * 2);
                    for (uint16_t index : { q, uint16_t(q + 1), uint16_t(q + 2), uint16_t(q + 2), uint16_t(q + 1), uint16_t(q + 3) })
                        list[n++] = index;
                }
                std::memcpy(i + b * kIndicesPerCard, list, sizeof(list));
            }
            meshIB->Unlock();
        }
        if (!cardTexture && !MakeCardTexture(real))
            return false;
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

    // The ground under the grass multiplied towards kGroundTintColour in `colour` (_DEFERRED_GBUFFER_0_, RGB only: its
    // alpha is the game's), reading the depth buffer the pass drew into (unbound meanwhile, Render binds it again), the
    // normals and the material IDs; the samplers and the blend put back
    void TintGround(IDirect3DDevice9* pDevice, IDirect3DSurface9* colour, const D3DSURFACE_DESC& desc, IDirect3DSurface9* depth)
    {
        auto& R = PostFxResources;
        rage::grcViewport* vp = rage::GetCurrentViewport();
        if (!(fGroundTint > 0.0f) || !h.groundTint || !edgeTexture || !vp || !R.mNormalRT || !R.mMaterialIdRT ||
            !R.mNormalRT->mD3DTexture || !R.mMaterialIdRT->mD3DTexture)
            return;
        // (the depth buffer as a texture, at the colour's size; under a render scale not always _DEFERRED_GBUFFER_3_)
        IDirect3DTexture9* depthTexture = nullptr;
        D3DSURFACE_DESC depthDesc{};
        if (FAILED(depth->GetDesc(&depthDesc)) || depthDesc.Width != desc.Width || depthDesc.Height != desc.Height ||
            FAILED(depth->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&depthTexture))) || !depthTexture)
            return;
        const float width = float(desc.Width), height = float(desc.Height);
        const D3DXVECTOR4 projInfo = PostFxPass::ProjInfo(*(const D3DMATRIX*)vp->mProjectionMatrix, width, height);
        effect->SetVector(h.projInfo, &projInfo);
        {
            const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
            D3DXVECTOR4 toView[3];
            PostFxPass::WorldToViewRows(vp, toView);
            const D3DXVECTOR4 worldX(toView[0].x, toView[1].x, toView[2].x, viewInv.m[3][0]);
            const D3DXVECTOR4 worldY(toView[0].y, toView[1].y, toView[2].y, viewInv.m[3][1]);
            const D3DXVECTOR4 worldZ(toView[0].z, toView[1].z, toView[2].z, viewInv.m[3][2]);
            effect->SetVector(h.worldX, &worldX);
            effect->SetVector(h.worldY, &worldY);
            effect->SetVector(h.worldZ, &worldZ);
        }
        effect->SetFloat(h.nearPlane, vp->mNearClip);
        effect->SetFloat(h.farDivNear, vp->mFarClip / vp->mNearClip);
        const D3DXVECTOR4 edgeGrid(edgeOrigin[0], edgeOrigin[1], 1.0f / (kEdgeCell * kEdgeSide), 1.0f);
        effect->SetVector(h.edgeGrid, &edgeGrid);
        const D3DXVECTOR4 tint(kGroundTintColour[0], kGroundTintColour[1], kGroundTintColour[2], fGroundTint);
        effect->SetVector(h.groundTintColour, &tint);
        const int mask = R.nWetGroundMaterials;
        const D3DXVECTOR4 allow0(float(mask & 1), float((mask >> 1) & 1), float((mask >> 2) & 1), float((mask >> 3) & 1));
        const D3DXVECTOR4 allow1(float((mask >> 4) & 1), float((mask >> 5) & 1), float((mask >> 6) & 1), float((mask >> 7) & 1));
        effect->SetVector(h.tintAllow0, &allow0);
        effect->SetVector(h.tintAllow1, &allow1);
        effect->SetFloat(h.tintRange, fRange);

        static constexpr DWORD kFirstSlot = 3, kSlots = 4;
        static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
                                                                  D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
        IDirect3DBaseTexture9* oldTexture[kSlots] = {};
        DWORD oldSampler[kSlots][std::size(kSamplerStates)] = {};
        for (DWORD s = 0; s < kSlots; ++s)
        {
            pDevice->GetTexture(kFirstSlot + s, &oldTexture[s]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                pDevice->GetSamplerState(kFirstSlot + s, kSamplerStates[i], &oldSampler[s][i]);
        }
        static constexpr D3DRENDERSTATETYPE kBlendStates[] = { D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE };
        DWORD oldBlend[std::size(kBlendStates)] = {};
        for (size_t i = 0; i < std::size(kBlendStates); ++i)
            pDevice->GetRenderState(kBlendStates[i], &oldBlend[i]);
        SavedVertexInput vertexInput(pDevice);

        pDevice->SetRenderTarget(0, colour);
        for (DWORD i = 1; i < 4; ++i)
            pDevice->SetRenderTarget(i, nullptr);
        pDevice->SetDepthStencilSurface(nullptr);
        D3DVIEWPORT9 viewport = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);
        // The ground's colour times the shader's
        static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kStates[] =
        {
            { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, TRUE }, { D3DRS_SRCBLEND, D3DBLEND_ZERO },
            { D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR }, { D3DRS_BLENDOP, D3DBLENDOP_ADD }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
            { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
            { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE },
            { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID },
            { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
        };
        for (auto [state, value] : kStates)
            pDevice->SetRenderState(state, value);
        pDevice->SetVertexShader(nullptr);
        pDevice->SetFVF(kScreenVertexFVF);

        UINT passes = 0;
        effect->SetTechnique(h.groundTint);
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        IDirect3DBaseTexture9* textures[kSlots] = { depthTexture, R.mNormalRT->mD3DTexture, R.mMaterialIdRT->mD3DTexture, edgeTexture };
        for (DWORD s = 0; s < kSlots; ++s)
        {
            const bool edges = s == 3;
            SetTextureBoth(pDevice, kFirstSlot + s, textures[s]);
            SetSamplerStateBoth(pDevice, kFirstSlot + s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, kFirstSlot + s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, kFirstSlot + s, D3DSAMP_MAGFILTER, edges ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            SetSamplerStateBoth(pDevice, kFirstSlot + s, D3DSAMP_MINFILTER, edges ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            SetSamplerStateBoth(pDevice, kFirstSlot + s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            SetSamplerStateBoth(pDevice, kFirstSlot + s, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        PostFxPass::BindEffectConstantsOnly(pDevice, effect);
        ScreenQuad::Full(width, height).Draw(pDevice);
        effect->EndPass();
        effect->End();

        pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        for (size_t i = 0; i < std::size(kBlendStates); ++i)
            pDevice->SetRenderState(kBlendStates[i], oldBlend[i]);
        for (DWORD s = 0; s < kSlots; ++s)
        {
            SetTextureBoth(pDevice, kFirstSlot + s, oldTexture[s]);
            SAFE_RELEASE(oldTexture[s]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                SetSamplerStateBoth(pDevice, kFirstSlot + s, kSamplerStates[i], oldSampler[s][i]);
        }
        depthTexture->Release();
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

    // The instances of this frame with `technique`: the ground's colour copy on s0 (or none), the cards' texture on s7,
    // the trample grid on the vertex sampler s1, the edges on s2; the samplers and the streams' frequencies put back
    void DrawBlades(IDirect3DDevice9* pDevice, D3DXHANDLE technique, IDirect3DBaseTexture9* ground, int count)
    {
        static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
                                                                  D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
        static constexpr DWORD kSamplerValues[] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, FALSE };
        static constexpr DWORD kTrampleValues[] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, FALSE };
        static constexpr DWORD kEdgeValues[] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, FALSE };
        static constexpr DWORD kCardValues[] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, FALSE };
        static constexpr DWORD kSamplers[4] = { 0, D3DVERTEXTEXTURESAMPLER1, D3DVERTEXTEXTURESAMPLER2, 7 };
        IDirect3DBaseTexture9* oldTexture[4] = {};
        DWORD oldSampler[4][std::size(kSamplerStates)] = {};
        for (int s = 0; s < 4; ++s)
        {
            pDevice->GetTexture(kSamplers[s], &oldTexture[s]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                pDevice->GetSamplerState(kSamplers[s], kSamplerStates[i], &oldSampler[s][i]);
        }
        auto trample = GrassShared::TrampleTexture;

        pDevice->SetVertexDeclaration(declaration);
        pDevice->SetStreamSource(0, meshVB, 0, sizeof(MeshVertex));
        pDevice->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | UINT(count));
        pDevice->SetStreamSource(1, instanceVB, 0, sizeof(Instance));
        pDevice->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
        pDevice->SetIndices(meshIB);

        UINT passes = 0;
        effect->SetTechnique(technique);
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        SetTextureBoth(pDevice, 0, ground);
        SetTextureBoth(pDevice, D3DVERTEXTEXTURESAMPLER1, trample);
        SetTextureBoth(pDevice, D3DVERTEXTEXTURESAMPLER2, edgeTexture);
        SetTextureBoth(pDevice, 7, cardTexture);
        for (size_t i = 0; i < std::size(kSamplerStates); ++i)
        {
            SetSamplerStateBoth(pDevice, 7, kSamplerStates[i], kCardValues[i]);
            SetSamplerStateBoth(pDevice, 0, kSamplerStates[i], kSamplerValues[i]);
            SetSamplerStateBoth(pDevice, D3DVERTEXTEXTURESAMPLER1, kSamplerStates[i], kTrampleValues[i]);
            SetSamplerStateBoth(pDevice, D3DVERTEXTEXTURESAMPLER2, kSamplerStates[i], kEdgeValues[i]);
        }
        pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, kCards * kVertsPerCard, 0, kCards * kIndicesPerCard / 3);
        effect->EndPass();
        effect->End();

        pDevice->SetStreamSourceFreq(0, 1);
        pDevice->SetStreamSourceFreq(1, 1);
        pDevice->SetStreamSource(1, nullptr, 0, 0);
        for (int s = 0; s < 4; ++s)
        {
            SetTextureBoth(pDevice, kSamplers[s], oldTexture[s]);
            SAFE_RELEASE(oldTexture[s]);
            for (size_t i = 0; i < std::size(kSamplerStates); ++i)
                SetSamplerStateBoth(pDevice, kSamplers[s], kSamplerStates[i], oldSampler[s][i]);
        }
    }

    // At the end of the G-buffer pass, before temporal AA's motion vectors (TemporalAA::OnGBufferBeforeMotion, the
    // device state saved around it), so the camera's motion is made from depth with the grass in it: the grass into
    // the G-buffer.
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
            FusionLog::Write("GpuGrass", "Stats", "%zu triangles held, %zu grassy, %zu pieces; per frame: %.0f pieces drawn, %.0f cards, CPU %.3f ms; last: %s\n",
                held, logGrassTriangles, pieces.size(), double(logInstances) / logFrames, double(logBlades) / logFrames, logCpuMs / logFrames, status);
            logFrames = 0;
            logInstances = logBlades = 0;
            logCpuMs = 0.0;
        }
        if (!pDevice || !pref || pref->get() == 0)
        {
            drawing = false;
            windKnown = false;
            ProceduralGround::GatherAhead = 0.0f;
            return void(status = "off");
        }
        const auto start = std::chrono::steady_clock::now();
        const auto& camera = FrameHistory::Current();
        if (!camera.Valid || !(camera.Near > 0.0f) || !(camera.Far > camera.Near))
            return void(status = "no camera");
        if (!MakeTagShares())
            return void(status = "procedural.dat not loaded");
        if (!MakeResources(pDevice))
        {
            drawing = false;
            return void(status = "no effect or buffers");
        }
        drawing = true;
        ProceduralGround::GatherAhead = fRange;

        // The camera: a rigid view, its position -t R^T
        const auto& v = camera.View;
        const float eye[3] = {
            -(v._41 * v._11 + v._42 * v._12 + v._43 * v._13),
            -(v._41 * v._21 + v._42 * v._22 + v._43 * v._23),
            -(v._41 * v._31 + v._42 * v._32 + v._43 * v._33),
        };
        MakePieces(eye);
        const bool edgeTextureReady = UpdateEdgeTexture(pDevice);

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
            const float share = p.share * fDensity * BladeShare(distance);
            if (share * kCards < 0.5f)
                continue;
            const auto& k = p.look.kinds;
            out[count] = { { p.corner[0], p.corner[1], p.corner[2], p.seed }, { p.edge1[0], p.edge1[1], p.edge1[2], share },
                           { p.edge2[0], p.edge2[1], p.edge2[2], p.look.tag }, { k[0], k[1], RiseScale(distance, p.share * fDensity), k[3] } };
            blades += share * kCards;
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
        // Kept besides what StateBackup keeps: every float constant (D3DX places the effect's from c0, over the game's
        // and FusionFix's cached ones such as the log depth's), the third target's colour mask, the streams' frequencies
        static float vsConstants[256 * 4], psConstants[224 * 4];
        pDevice->GetVertexShaderConstantF(0, vsConstants, 256);
        pDevice->GetPixelShaderConstantF(0, psConstants, 224);
        DWORD colourWrite2 = 0, zfunc = D3DCMP_LESSEQUAL;
        pDevice->GetRenderState(D3DRS_COLORWRITEENABLE2, &colourWrite2);
        pDevice->GetRenderState(D3DRS_ZFUNC, &zfunc);

        // The ground under the grass darkened first, then its colour before the grass, which the blades take a little
        // of: into Wet Ground's copy of the first target
        if (edgeTextureReady)
            TintGround(pDevice, targets[0], desc, depth);
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
        const D3DXVECTOR4 blade(1.0f, 1.0f, groundCopied ? 0.12f : 0.0f, debug ? 1.0f : 0.0f);
        const D3DXVECTOR4 eyeVector(eye[0], eye[1], eye[2], 0.0f);
        effect->SetVector(h.wind, &wind);
        windBefore = windKnown ? windNow : wind;
        windNow = wind;
        windKnown = true;
        effect->SetVector(h.windDir, &windDir);
        effect->SetVector(h.blade, &blade);
        effect->SetVector(h.eye, &eyeVector);
        const D3DXVECTOR4 greenVector(green[0], green[1], green[2], 0.0f), tipVector(tip[0], tip[1], tip[2], 0.0f), dryVector(dry[0], dry[1], dry[2], 0.0f);
        effect->SetVector(h.green, &greenVector);
        effect->SetVector(h.tip, &tipVector);
        effect->SetVector(h.dry, &dryVector);
        // The fade's dither moves on each frame (the motion pass draws in this frame's)
        static uint32_t ditherFrame = 0;
        effect->SetFloat(h.ditherFrame, float(ditherFrame++ & 63));
        // The edge grid: its origin, 1 / its side, whether EdgeTex holds it
        const D3DXVECTOR4 edgeGrid(edgeOrigin[0], edgeOrigin[1], 1.0f / (kEdgeCell * kEdgeSide), edgeTextureReady ? 1.0f : 0.0f);
        effect->SetVector(h.edgeGrid, &edgeGrid);
        // Towards the sun (c216's, which shaders.ixx keeps); the light through the blades fades as it sets
        {
            const float* sun = SunDirectionWorld;
            const float length = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
            const D3DXVECTOR4 sunVector = length > 0.01f ? D3DXVECTOR4(sun[0] / length, sun[1] / length, sun[2] / length,
                std::clamp(sun[2] / length * 6.0f, 0.0f, 1.0f)) : D3DXVECTOR4(0.0f, 0.0f, 1.0f, 0.0f);
            effect->SetVector(h.sun, &sunVector);
        }
        // The bodies that press grass in range, the nearest the camera first; their strength fades over the last 10 m
        // of NearbyBodies' range, as the props have it
        D3DXVECTOR4 bodyValues[kBodies * 3] = {};
        if (GrassShared::Bodies && GrassShared::PushPerHeight > 0.0f)
        {
            struct Near { float distance; const NearbyBodies::Body* body; };
            std::vector<Near> pressing;
            for (const auto& body : *GrassShared::Bodies)
            {
                const float dx = body.centre[0] - eye[0], dy = body.centre[1] - eye[1];
                const float reach = body.kind == NearbyBodies::Body::Rotor ? 18.0f : body.reach;
                const float distance = std::sqrt(dx * dx + dy * dy) - (std::max)(body.halfLength, body.halfWidth) - reach;
                if (distance < fRange)
                    pressing.push_back({ distance, &body });
            }
            std::sort(pressing.begin(), pressing.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
            for (size_t i = 0; i < pressing.size() && i < kBodies; ++i)
            {
                const auto& body = *pressing[i].body;
                const bool rotor = body.kind == NearbyBodies::Body::Rotor;
                float strength = std::clamp((NearbyBodies::kRange - std::sqrt(body.distance2)) * 0.1f, 0.0f, 1.0f);
                if (rotor)
                    strength *= GrassShared::RotorPulse * body.rotorSpeed;
                bodyValues[i * 3 + 0] = D3DXVECTOR4(body.centre[0], body.centre[1], body.forward[0], body.forward[1]);
                bodyValues[i * 3 + 1] = D3DXVECTOR4(body.halfLength, body.halfWidth, rotor ? 0.0f : 1.0f / (std::max)(body.reach, 0.05f), strength);
                bodyValues[i * 3 + 2] = D3DXVECTOR4(body.bottom, rotor ? 1.0f : 0.0f, 0.0f, 0.0f);
            }
        }
        auto trample = GrassShared::TrampleTexture;
        // Without the grid its coordinates go far outside it, where the clamped sampler reads its empty edge
        const D3DXVECTOR4 bend(GrassShared::PushPerHeight, 1.0f / GrassShared::TrampleGridSide, trample ? GrassShared::TrampleUVOffset[0] : 1000.0f,
                               trample ? GrassShared::TrampleUVOffset[1] : 1000.0f);
        effect->SetVectorArray(h.bodies, bodyValues, kBodies * 3);
        effect->SetVector(h.bend, &bend);
        effect->SetFloat(h.nearPlane, camera.Near);
        effect->SetFloat(h.farDivNear, camera.Far / camera.Near);

        DrawBlades(pDevice, h.technique, groundCopied ? R.WetCopyTex[0] : nullptr, count);
        drawnFrame = FrameHistory::Frame();
        drawnCount = count;
        drawnZFunc = zfunc == D3DCMP_ALWAYS || zfunc == D3DCMP_NEVER ? D3DCMP_LESSEQUAL : zfunc;
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

    // After the motion vectors (TemporalAA::OnGBufferEnd): the blades drawn this frame again into the motion target,
    // with their wind's motion over the camera's, where the depth test finds them what is seen
    void RenderMotion(IDirect3DDevice9* pDevice)
    {
        if (!pDevice || !effect || !h.motion || drawnFrame != FrameHistory::Frame() || drawnCount <= 0 || !TemporalAA::IsMotionVectorsReady())
            return;
        auto motionTexture = TemporalAA::GetMotionVectors();
        const auto& camera = FrameHistory::Current();
        const auto& previous = FrameHistory::Previous();
        if (!motionTexture || !camera.Valid)
            return;
        IDirect3DSurface9* motion = nullptr;
        IDirect3DSurface9* depth = nullptr;
        motionTexture->GetSurfaceLevel(0, &motion);
        pDevice->GetDepthStencilSurface(&depth);
        D3DSURFACE_DESC motionDesc{}, depthDesc{};
        if (!motion || !depth || FAILED(motion->GetDesc(&motionDesc)) || FAILED(depth->GetDesc(&depthDesc)) ||
            motionDesc.Width != depthDesc.Width || motionDesc.Height != depthDesc.Height)
        {
            SAFE_RELEASE(motion);
            SAFE_RELEASE(depth);
            return;
        }

        ProfilerScope timed(pDevice, kProfGpuGrass);
        static float vsConstants[256 * 4], psConstants[224 * 4];
        pDevice->GetVertexShaderConstantF(0, vsConstants, 256);
        pDevice->GetPixelShaderConstantF(0, psConstants, 224);
        pDevice->SetRenderTarget(0, motion);
        for (DWORD i = 1; i < 4; ++i)
            pDevice->SetRenderTarget(i, nullptr);
        pDevice->SetDepthStencilSurface(depth);
        D3DVIEWPORT9 viewport = { 0, 0, motionDesc.Width, motionDesc.Height, 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);
        static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kStates[] =
        {
            { D3DRS_ZENABLE, TRUE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
            { D3DRS_STENCILENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
            { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN }, { D3DRS_SCISSORTESTENABLE, FALSE },
            { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FOGENABLE, FALSE },
            { D3DRS_DEPTHBIAS, 0 }, { D3DRS_SLOPESCALEDEPTHBIAS, 0 },
        };
        for (auto [state, value] : kStates)
            pDevice->SetRenderState(state, value);
        pDevice->SetRenderState(D3DRS_ZFUNC, drawnZFunc);

        // The rest of the effect's values are Render's of this frame
        effect->SetMatrix(h.viewProjNoJitter, &camera.ViewProjectionNoJitter);
        effect->SetMatrix(h.prevViewProj, previous.Valid ? &previous.ViewProjectionNoJitter : &camera.ViewProjectionNoJitter);
        effect->SetVector(h.windPrev, &windBefore);
        effect->SetFloat(h.depthBias, TemporalAA::ObjectDepthBias());
        DrawBlades(pDevice, h.motion, nullptr, drawnCount);

        pDevice->SetVertexShaderConstantF(0, vsConstants, 256);
        pDevice->SetPixelShaderConstantF(0, psConstants, 224);
        SAFE_RELEASE(motion);
        SAFE_RELEASE(depth);
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
        ProceduralGround::GatherGrassSurfaces = true;
        ProceduralGround::HideGrass = [](uint16_t model, float distance, float keep)
        {
            using namespace GpuGrass;
            if (!bHideProps || !drawing || model >= grassModel.size() || !grassModel[model])
                return false;
            // All of them until the blades are down to a quarter (61 m at the defaults), fewer as they thin out further: the
            // props and the blades trade places far off, where the change shows least
            return keep < (std::min)(BladeShare(distance) * 4.0f, 1.0f);
        };
        ProceduralGround::OnRemoved = [](const void* key)
        {
            std::lock_guard lock(GpuGrass::groundMutex);
            if (GpuGrass::ground.erase(key))
                ++GpuGrass::groundVersion;
        };
    }
} GpuGrassInit;
