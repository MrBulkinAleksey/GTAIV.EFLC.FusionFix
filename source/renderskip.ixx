module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

export module renderskip;

import common;
import comvars;
import temporal;

// Passes of the game left out on the GPU, by the target they draw into. grcTextureFactoryPC::LockRenderTarget and
// UnlockRenderTarget (its vtable slots 15 and 16, index 0) say which target the game draws into; the device's draws,
// clears and copies (its vtable) are dropped while a target is locked whose pass is to be left out. In a profile at
// night on an integrated GPU, the frame took some 39 GPU milliseconds and these passes:
//  - CascadeShadowsAtNight = 0: the sun's cascades (CASCADE_TARGET_R32, CASCADE_ATLAS, some 7.5 ms) are not drawn from
//    22:00 to 05:00, when the moon casts no shadows to see. Their casters go; the game clears only the depth of
//    CASCADE_TARGET_R32 and none of the atlas, so its colour is cleared here to 1.0 (nothing in the light's way) and
//    the game's own draw puts that into the atlas: no shadows, instead of the last ones of the day.
//  - ReflectionsHalfRate = 1: the water's reflection and the reflection map (WATER_REFLECTION_COLOUR,
//    REFLECTION_MAP_COLOUR, some 2.3 ms with what is drawn into them) are drawn every other frame; in between their
//    targets keep the last one, nothing drawn, cleared or resolved into them. They are drawn every frame while the
//    camera turns or moves fast, where one frame behind would show.
// The lamps' shadow maps (SHADOW_MAP_COLOUR_POINT, some 3.8 ms at night) can't be left out so: the game hands its tiles
// to other lamps from one frame to the next, and a tile kept from the frame before lit a lamp with another's shadow.
//  - FarCascadesHalfRate = 1: by day the two far cascades of the four, three in four of the cascades' draws, take turns:
//    the third is drawn in one frame, the fourth in the next, while the camera keeps still since that cascade was drawn
//    (turned under half a degree, moved under 30 cm). Each cascade is drawn into CASCADE_TARGET_R32 (cleared, its
//    casters drawn) and put into its quarter of CASCADE_ATLAS by one draw, nearest first; left out, both go and its
//    quarter keeps the frame before. The game fits the cascades to the view anew each frame, turning them with the
//    camera's heading, so a cascade is left out only while the one kept can be matched to this frame's fit (FoldCascade):
//    in practice while the camera keeps still, where the game still moves the fit by a few texels a frame.
//  - CascadeMatrixTrace = 1: once, some 20 s in, 300 passes of the cascades' matrix, scales and offsets to the log.
//  - RenderSkipTrace = 1: once, a little after the world is in, two frames of locks, clears, viewports, draws and
//    copies to GTAIV.EFLC.FusionFix.RenderSkip.log, to tell how the game draws its passes.
// The slots are taken over as the other hooks of them do, each passing on to what was there before.
export namespace RenderSkip
{
    bool bCascadesAtNight = true;
    bool bReflectionsHalfRate = true;
    bool bFarCascadesHalfRate = true;
    bool bMidCascadeHalfRate = false;
    bool bCascadesIntoAtlas = false;
    bool bTrace = false;
    // CascadeSunElevation: degrees of the sun above the horizon below which its cascades are left out, by the sky's
    // SunDirection; below -90 (or before the sky has been drawn) by the clock instead, from 22:00 to 05:00
    float fSunLowBelow = 8.0f;

    float SunElevation();   // degrees, NaN while the sky has given none
    bool SunIsLow();        // the sun's shadows too faint to draw: the cascades and the clouds in reflections go
    bool SunShadowsDrawn(); // the game drew the sun's cascades in the last few frames, which it does only outside, and they
                            // weren't left out for a low sun
}

namespace RenderSkip
{
    constexpr uint8_t NightFrom = 22, NightTo = 5;
    constexpr float TurnCos = 0.99966f;     // 1.5 degrees
    constexpr float MoveSquared = 1.0f;     // metres

    enum Kind : uint8_t { None, CascadeTarget, CascadeAtlas, WaterReflection, ReflectionMap, Kinds };
    constexpr const char* KindNames[Kinds] = { "other", "cascades", "cascade atlas", "water reflection", "reflection map" };

    // A pass drawn every other frame: the scene frame it was decided for, and where the camera was when it was drawn
    struct HalfRate
    {
        uint32_t decided = ~0u, drawn = ~0u;
        bool skip = false;
        float position[3]{}, forward[3]{};
        uint32_t skipped = 0, kept = 0;
    };
    HalfRate halfRates[Kinds];

    float* pCamera = nullptr;               // CE 0x128e340, the game camera's position, where it looks 8 floats before

    bool CameraMovedSince(const float* position, const float* lastForward, float moveSquared, float turnCos)
    {
        if (!pCamera)
            return true;
        const float* forward = pCamera - 8;
        float dx = pCamera[0] - position[0], dy = pCamera[1] - position[1], dz = pCamera[2] - position[2];
        float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
        if (!(length > 0.5f))
            return true;
        float facing = (forward[0] * lastForward[0] + forward[1] * lastForward[1] + forward[2] * lastForward[2]) / length;
        return dx * dx + dy * dy + dz * dz > moveSquared || facing < turnCos;
    }

    void KeepCamera(float* position, float* lastForward)
    {
        if (!pCamera)
            return;
        const float* forward = pCamera - 8;
        float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
        for (int i = 0; i < 3; i++)
        {
            position[i] = pCamera[i];
            lastForward[i] = length > 0.0f ? forward[i] / length : 0.0f;
        }
    }

    // Once a scene frame for each kind: left out when it was drawn the frame before and the camera hardly moved since
    bool SkipThisFrame(Kind kind)
    {
        auto& h = halfRates[kind];
        uint32_t frame = TemporalAA::SceneFrame;
        if (!frame)
            return false;
        if (h.decided != frame)
        {
            h.decided = frame;
            h.skip = h.drawn + 1 == frame && !CameraMovedSince(h.position, h.forward, MoveSquared, TurnCos);
            if (h.skip)
                h.skipped++;
            else
            {
                h.kept++;
                h.drawn = frame;
                KeepCamera(h.position, h.forward);
            }
        }
        return h.skip;
    }

    // The sky's sun (gta_atmoscatt_clouds' SunDirection, y up): unlike gDirectionalLight it stays the sun at night
    size_t nSunDirectionIdx = SIZE_MAX;
    bool bSunLow = false;

    bool IsNight()
    {
        float elevation = SunElevation();
        if (fSunLowBelow > -90.0f && !std::isnan(elevation))
        {
            // Half a degree of hysteresis, so the cascades don't come and go on the threshold
            bSunLow = elevation < fSunLowBelow + (bSunLow ? 0.5f : 0.0f);
            return bSunLow;
        }
        if (!CClock::ms_nGameClockHours)
            return false;
        auto hours = CClock::GetHours();
        return hours >= NightFrom || hours < NightTo;
    }

    // By name each time: a target the game makes again, as on a device reset, can take the address of another
    Kind KindOf(rage::grcRenderTargetPC* rt)
    {
        if (!rt)
            return None;
        Kind kind = None;
        if (auto name = rt->mName)
        {
            if (!strcmp(name, "CASCADE_TARGET_R32"))
                kind = CascadeTarget;
            else if (!strcmp(name, "CASCADE_ATLAS"))
                kind = CascadeAtlas;
            else if (!strcmp(name, "WATER_REFLECTION_COLOUR"))
                kind = WaterReflection;
            else if (!strcmp(name, "REFLECTION_MAP_COLOUR"))
                kind = ReflectionMap;
        }
        return kind;
    }

    // What is dropped now: draws alone (the night's cascades), or everything that writes (a pass left out this frame)
    enum class Drop : uint8_t { Nothing, Draws, Everything };
    Drop drop = Drop::Nothing;

    // The locks of target index 0, innermost last, and what each drops
    struct Lock { Kind kind; Drop drop; rage::grcRenderTargetPC* target; int8_t intoAtlas = -1; bool statesTraced = false; };
    Lock stack[16]{};
    int depth = 0;
    uint32_t dropped[Kinds]{};

    // The far cascades by day: the cascades' passes counted, and where the camera was at each one's last draw
    constexpr float CascadeMoveSquared = 0.3f * 0.3f;
    constexpr float CascadeTurnCos = 0.99996f;  // half a degree
    struct FarCascade
    {
        uint32_t drawn = ~0u;
        float position[3]{}, forward[3]{};
        uint32_t skipped = 0, kept = 0;
    };
    FarCascade farCascades[3];  // the second, third and fourth
    uint32_t cascadeFrame = ~0u, cascadeCount = 0;
    uint32_t passSeq = 0;       // the cascades' passes, one a scene frame by day
    bool skippedInPass[4]{};
    bool bAtlasLeftOut = false;
    int lastCascade = -1;       // the index of the cascade CascadeDrop decided last, -1 at night
    uint32_t cascadeSide = 2048;                // CASCADE_TARGET_R32's, texels

    // CascadesIntoAtlas: each cascade is drawn straight into its quarter of CASCADE_ATLAS instead of into
    // CASCADE_TARGET_R32 and then copied there by one draw of cascadeConvert's convert pass 1 (CE 0x92cb30, some 1.5 ms
    // on an integrated GPU for the four 2048x2048 quarters). That pass stores 0.5 - 0.0005 x of the depth x the casters
    // write (their vertex shaders put gShadowMatrix's w column in a texcoord, the pixel shaders write it out), and the
    // shaders that read the atlas convert their own depth the same way (c139). So for the cascades' draws the matrix's
    // w column becomes 0.5 - 0.0005 w (in CE 0x92dd20 called from the cascade's own callback, CE 0x92dd70), the target
    // becomes the atlas with a depth buffer of its size, the viewport moves to the quarter, the game's fill of the target
    // with -3.4e38 (no caster, convert pass 0 in CE 0x92c740) becomes a clear to 1.0 (above any converted depth within
    // 1000 m), and the copy is left out.
    uintptr_t cascadeMatrixReturn = 0;      // where CE 0x92dd20 returns to in the cascade's callback
    bool pendingIntoAtlas[4]{};             // the cascade's matrix was made for the atlas
    rage::grcRenderTargetPC* atlasTarget = nullptr;
    IDirect3DSurface9* atlasDepth = nullptr;
    bool bFillPending = false;
    uint32_t intoAtlas = 0, intoAtlasFailed = 0;

    bool IntoAtlasReady()
    {
        return bCascadesIntoAtlas && atlasTarget && atlasTarget->mD3DTexture && (bCascadesAtNight || !IsNight());
    }

    // The quarter of the atlas a cascade drawn into it takes: clears and scissor go by it explicitly, as a clear by the
    // viewport alone or a scissor rectangle the game set for its own target would land in the first quarter
    D3DRECT Quarter(int index)
    {
        return { LONG(index) * LONG(cascadeSide), 0, LONG(index + 1) * LONG(cascadeSide), LONG(cascadeSide) };
    }

    // 0.5 - 0.0005 of the depth the casters write
    void MatrixForAtlas(float* matrix)
    {
        matrix[3] *= -0.0005f;
        matrix[7] *= -0.0005f;
        matrix[11] *= -0.0005f;
        matrix[15] = 0.5f - 0.0005f * matrix[15];
    }

    HRESULT(__stdcall* RealSetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*) = nullptr;
    HRESULT(__stdcall* RealSetScissorRect)(IDirect3DDevice9*, const RECT*) = nullptr;

    // The game's lock just bound CASCADE_TARGET_R32: the atlas and a depth buffer of its size instead
    bool BindAtlas(IDirect3DDevice9* device, int index)
    {
        IDirect3DSurface9* surface = nullptr;
        if (!device || !atlasTarget || !atlasTarget->mD3DTexture || FAILED(atlasTarget->mD3DTexture->GetSurfaceLevel(0, &surface)))
            return false;
        D3DSURFACE_DESC desc{}, depthDesc{};
        surface->GetDesc(&desc);
        if (atlasDepth)
        {
            atlasDepth->GetDesc(&depthDesc);
            if (depthDesc.Width != desc.Width || depthDesc.Height != desc.Height)
            {
                atlasDepth->Release();
                atlasDepth = nullptr;
            }
        }
        if (!atlasDepth)
            device->CreateDepthStencilSurface(desc.Width, desc.Height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &atlasDepth, nullptr);
        bool fits = atlasDepth && desc.Width == cascadeSide * 4 && desc.Height == cascadeSide;
        // Not while the atlas is still on a sampler (the lighting's s15 from the frame before): drawn into while bound
        // for reading, DXVK took every draw as a feedback loop and the frame lost a third of its rate. Through the game's
        // device wrapper and the device both, so the wrapper binds it again for the lighting.
        if (fits)
            if (auto wrapper = rage::grcDevice::GetD3DDevice())
            {
                static constexpr DWORD stages[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                    D3DVERTEXTEXTURESAMPLER0, D3DVERTEXTEXTURESAMPLER1, D3DVERTEXTEXTURESAMPLER2, D3DVERTEXTEXTURESAMPLER3 };
                for (DWORD stage : stages)
                {
                    IDirect3DBaseTexture9* bound = nullptr;
                    device->GetTexture(stage, &bound);
                    bool atlas = bound == atlasTarget->mD3DTexture ||
                        (RageDirect3DDevice9::g_TexturesBySampler && RageDirect3DDevice9::GetTexture(stage) == atlasTarget->mD3DTexture);
                    if (bound)
                        bound->Release();
                    if (atlas)
                        RageDirect3DDevice9::SetTextureBoth(wrapper, stage, nullptr);
                }
            }
        // The viewport the game's lock set for its own target (its depth range is kept)
        D3DVIEWPORT9 viewport{};
        device->GetViewport(&viewport);
        bool bound = fits && SUCCEEDED(device->SetRenderTarget(0, surface)) && SUCCEEDED(device->SetDepthStencilSurface(atlasDepth));
        surface->Release();
        // SetRenderTarget left the viewport and the scissor on the whole atlas, and the game's device wrapper does not
        // send again the viewport it takes as set already: the cascade was drawn over all four quarters
        if (bound && RealSetViewport && RealSetScissorRect)
        {
            D3DVIEWPORT9 quarter{ DWORD(index) * cascadeSide, 0, cascadeSide, cascadeSide, viewport.MinZ, viewport.MaxZ };
            RealSetViewport(device, &quarter);
            RECT rect{ LONG(index) * LONG(cascadeSide), 0, LONG(index + 1) * LONG(cascadeSide), LONG(cascadeSide) };
            RealSetScissorRect(device, &rect);
        }
        return bound;
    }
    bool bNightCascade = false;     // the cascade just locked was the night's, its draws left out
    uint32_t nightAtlasFrame = ~0u;  // the scene frame the night's atlas was last cleared in

    // A cascade left out is read with the matrix it was drawn with. The shaders take the cascades as one matrix
    // (gShadowMatrix: light x, y and depth of a world position) and, for each cascade, a scale and an offset of x and y
    // (gShadowParam0123, 4567, 891113 and 14151617, c57, c58, c59 and c56 in deferred_lighting: x and y scales, x and y
    // offsets, each four running sums over the cascades, the nearest first). The render thread sets them from the frame's
    // copy of the cascades (CE 0x11a1c50 + 0x270 a copy) in CE 0x92d7f0, after the cascades' pass: the matrix from it in
    // CE 0x92dd20, then the rest. The game fits the matrix to the view anew every frame: x and y turn with the camera's
    // heading, the depth moves with the camera, and even a still camera has it rescaled and moved by a few texels of the
    // fourth cascade from one frame to the next (a trace of 300 frames, CascadeMatrixTrace). A kept cascade is read
    // with this frame's matrix scaled and moved to match the one it was drawn with at the camera; what can't be matched
    // so (a turn, which needs a rotation the shaders don't have, and the depth, whose per-cascade biases are constants
    // of the shaders) has to stay under FoldTexels at the cascade's far edge and FoldDepth, or it is drawn.
    constexpr float FoldTexels = 0.5f;
    constexpr float FoldDepth = 0.05f;          // metres
    constexpr float CascadeReach[4] = { 10.0f, 30.0f, 85.0f, 256.0f };  // metres from the camera, as shadows.ixx sets them
    bool bFollow = false;                       // both hooks in place
    bool bMatrixTrace = false;
    struct CascadeLook
    {
        uint32_t seq = ~0u;
        float matrix[16]{};
        float values[4]{};  // x and y scales, x and y offsets
    };
    CascadeLook looks[4];
    float caughtMatrix[16]{};
    bool matrixCaught = false;
    float lastMatrix[16]{}, previousMatrix[16]{};
    float lastValues[4][4]{}, previousValues[4][4]{};   // the game's, a cascade each
    uint32_t matricesSeen = 0, processedSeq = ~0u;
    uintptr_t setterReturn = 0;     // where CE 0x92dd20 returns to in the setter
    uint32_t folded = 0, misses = 0;
    float largestTexels = 0.0f, largestDepth = 0.0f;

    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    // Cascade k drawn with matrix am and scales and offsets av, read through matrix bm: the scales and offsets that
    // match it at the camera, and how far it is off at the cascade's reach in texels and in depth in metres
    struct Fold { float values[4]; float texels; float depth; };
    Fold FoldCascade(int k, const float* am, const float* av, const float* bm)
    {
        Fold f{ { av[0], av[1], av[2], av[3] }, 1e9f, 1e9f };
        if (!pCamera)
            return f;
        const float* camera = pCamera;
        f.texels = 0.0f;
        for (int axis = 0; axis < 2; axis++)
        {
            float A[3] = { am[axis], am[4 + axis], am[8 + axis] }, B[3] = { bm[axis], bm[4 + axis], bm[8 + axis] };
            float bb = Dot(B, B);
            if (!(bb > 0.0f))
            {
                f.texels = 1e9f;
                return f;
            }
            float scale = av[axis] * Dot(A, B) / bb;
            float atA = Dot(A, camera) + am[12 + axis], atB = Dot(B, camera) + bm[12 + axis];
            f.values[axis] = scale;
            f.values[2 + axis] = av[2 + axis] + atA * av[axis] - atB * scale;
            float r[3] = { A[0] * av[axis] - B[0] * scale, A[1] * av[axis] - B[1] * scale, A[2] * av[axis] - B[2] * scale };
            // uv a metre the two still differ by; x spans the atlas's four cascades, y one
            float texelsPerUV = float(cascadeSide) * (axis == 0 ? 4.0f : 1.0f);
            f.texels = std::max(f.texels, std::sqrt(Dot(r, r)) * CascadeReach[k] * texelsPerUV);
        }
        float A[3] = { am[3], am[7], am[11] }, B[3] = { bm[3], bm[7], bm[11] };
        float perMetre = std::sqrt(Dot(B, B));
        if (!(perMetre > 0.0f))
            return f;
        float d[3] = { A[0] - B[0], A[1] - B[1], A[2] - B[2] };
        f.depth = (std::abs(Dot(A, camera) + am[15] - Dot(B, camera) - bm[15]) + std::sqrt(Dot(d, d)) * CascadeReach[k]) / perMetre;
        return f;
    }

    bool FoldFits(const Fold& f)
    {
        return f.texels <= FoldTexels && f.depth <= FoldDepth;
    }

    // From the last two passes: cascade k kept from the one before would have fitted the last, so it likely will this one
    bool Followable(int k)
    {
        return bFollow && matricesSeen >= 2 && FoldFits(FoldCascade(k, previousMatrix, previousValues[k], lastMatrix));
    }

    Drop CascadeDrop()
    {
        bAtlasLeftOut = false;
        lastCascade = -1;
        bNightCascade = !bCascadesAtNight && IsNight();
        if (bNightCascade)
            return Drop::Draws;     // its quarter of the atlas is cleared by LockTarget instead
        uint32_t frame = TemporalAA::SceneFrame;
        if (frame != cascadeFrame)
        {
            cascadeFrame = frame;
            cascadeCount = 0;
            passSeq++;
            for (auto& s : skippedInPass)
                s = false;
        }
        uint32_t index = cascadeCount++;
        lastCascade = index < 4 ? int(index) : -1;
        // MidCascadeHalfRate: the second joins them, left out in the frames the fourth is drawn in
        bool farOne = bFarCascadesHalfRate && (index == 2 || index == 3);
        bool midOne = bMidCascadeHalfRate && index == 1;
        if (!frame || (!farOne && !midOne))
            return Drop::Nothing;
        // The third left out in even frames, the fourth in odd ones (the second in even ones), each only right after a
        // pass it was drawn in, and while the camera keeps still or the kept one can follow it
        auto& c = farCascades[index - 1];
        bool turn = ((frame + index + (index == 1 ? 1 : 0)) & 1) == 0;
        // Without the setter's hooks, only while the camera keeps still
        bool fits = bFollow ? Followable(index) : !CameraMovedSince(c.position, c.forward, CascadeMoveSquared, CascadeTurnCos);
        bool skip = turn && c.drawn + 1 == passSeq && fits;
        skippedInPass[index] = skip;
        if (skip)
            c.skipped++;
        else
        {
            c.kept++;
            c.drawn = passSeq;
            KeepCamera(c.position, c.forward);
        }
        bAtlasLeftOut = skip;
        return skip ? Drop::Everything : Drop::Nothing;
    }

    // The matrix trace: some passes of the matrix, the cascades' scales and offsets and what was left out
    std::vector<std::string> matrixTrace;
    uint32_t matrixTraceFrom = 0;
    constexpr uint32_t MatrixTracePasses = 300;

    void TraceMatrix(const float (&values)[4][4], const float* matrix, float texels, float depth)
    {
        if (!matrixTraceFrom || passSeq < matrixTraceFrom)
            return;
        if (passSeq >= matrixTraceFrom + MatrixTracePasses)
        {
            std::string text = "pass, camera position and forward, gShadowMatrix rows, how far the kept cascades are off at their reach, then for each cascade x/y scale and offset (* left out)\n";
            for (auto& line : matrixTrace)
                text += line + "\n";
            FusionLog::WriteText("RenderSkip", "Matrix", text);
            matrixTrace.clear();
            matrixTraceFrom = 0;
            return;
        }
        std::string line = std::format("{}", passSeq);
        if (pCamera)
            line += std::format(" cam {:.2f} {:.2f} {:.2f} fwd {:.3f} {:.3f} {:.3f}", pCamera[0], pCamera[1], pCamera[2], pCamera[-8], pCamera[-7], pCamera[-6]);
        line += " M";
        for (int i = 0; i < 16; i++)
            line += std::format("{}{:.6g}", i % 4 ? " " : " | ", matrix[i]);
        line += std::format(" | kept ones off {:.2f} texels, {:.3f} m deep;", texels, depth);
        for (int k = 0; k < 4; k++)
            line += std::format(" c{}{} {:.6g} {:.6g} {:.6g} {:.6g};", k + 1, skippedInPass[k] ? "*" : "", values[k][0], values[k][1], values[k][2], values[k][3]);
        matrixTrace.push_back(std::move(line));
    }

    // The setter, before the scales and offsets go out: once a pass, the ones drawn are kept and the ones left out get
    // the scales and the moved offsets of the pass they were drawn in
    void FollowCascades(float* frameCopy)
    {
        if (!matrixCaught)
            return;
        matrixCaught = false;
        if (processedSeq == passSeq)
            return;     // done in this pass's copy already, which keeps it
        processedSeq = passSeq;
        memcpy(previousMatrix, lastMatrix, sizeof(lastMatrix));
        memcpy(lastMatrix, caughtMatrix, sizeof(lastMatrix));
        matricesSeen++;

        // Four vectors of running sums: value of cascade k = the sum of the first k + 1 terms
        float values[4][4];
        for (int v = 0; v < 4; v++)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
            {
                sum += frameCopy[v * 4 + k];
                values[k][v] = sum;
            }
        }
        memcpy(previousValues, lastValues, sizeof(lastValues));
        memcpy(lastValues, values, sizeof(lastValues));
        float passTexels = 0.0f, passDepth = 0.0f;
        for (int k = 0; k < 4; k++)
        {
            auto& look = looks[k];
            if (!skippedInPass[k])
            {
                look.seq = passSeq;
                memcpy(look.matrix, lastMatrix, sizeof(look.matrix));
                memcpy(look.values, values[k], sizeof(look.values));
                continue;
            }
            if (look.seq + 1 != passSeq)
            {
                misses++;
                continue;
            }
            // Matched as well as it goes even when it is off more than it should: the game's own values are further off
            auto fold = FoldCascade(k, look.matrix, look.values, lastMatrix);
            memcpy(values[k], fold.values, sizeof(fold.values));
            passTexels = std::max(passTexels, fold.texels);
            passDepth = std::max(passDepth, fold.depth);
            if (FoldFits(fold))
                folded++;
            else
                misses++;
        }
        largestTexels = std::max(largestTexels, passTexels);
        largestDepth = std::max(largestDepth, passDepth);
        for (int v = 0; v < 4; v++)
            for (int k = 0; k < 4; k++)
                frameCopy[v * 4 + k] = values[k][v] - (k ? values[k - 1][v] : 0.0f);
        TraceMatrix(values, lastMatrix, passTexels, passDepth);
    }

    Drop DropFor(Kind kind)
    {
        switch (kind)
        {
        case CascadeTarget: return CascadeDrop();
        case CascadeAtlas:
        {
            // The draw that puts the cascade just drawn into its quarter (drawn there already with CascadesIntoAtlas); the
            // night's quarter is cleared by LockTarget
            bool leftOut = bAtlasLeftOut || bNightCascade;
            bAtlasLeftOut = false;
            return leftOut ? Drop::Everything : Drop::Nothing;
        }
        case WaterReflection:
        case ReflectionMap: return bReflectionsHalfRate && SkipThisFrame(kind) ? Drop::Everything : Drop::Nothing;
        default: return Drop::Nothing;
        }
    }

    void Refresh()
    {
        drop = depth > 0 ? stack[depth - 1].drop : Drop::Nothing;
    }

    // The trace: one line an event, for two scene frames
    std::vector<std::string> trace;
    uint32_t traceFrom = 0, traceUntil = 0;
    bool traceDone = false;
    uint32_t traceDraws = 0;

    bool Tracing()
    {
        if (!bTrace || traceDone || !traceFrom)
            return false;
        uint32_t frame = TemporalAA::SceneFrame;
        if (frame >= traceUntil)
        {
            if (traceDraws)
                trace.push_back(std::format("  {} draws", traceDraws));
            traceDraws = 0;
            std::string text = std::format("two frames from scene frame {}, at {:02}:00 on the game's clock:\n", traceFrom,
                CClock::ms_nGameClockHours ? CClock::GetHours() : 0);
            for (auto& line : trace)
                text += line + "\n";
            FusionLog::WriteText("RenderSkip", "Trace", text);
            trace.clear();
            traceDone = true;
            return false;
        }
        return frame >= traceFrom;
    }

    void TraceEvent(std::string&& line)
    {
        if (!Tracing())
            return;
        if (traceDraws)
            trace.push_back(std::format("  {} draws", traceDraws));
        traceDraws = 0;
        if (trace.size() < 4000)
            trace.push_back(std::move(line));
    }

    std::string TargetName(rage::grcRenderTargetPC* rt)
    {
        return rt ? (rt->mName ? rt->mName : "unnamed") : "none";
    }

    HRESULT(__stdcall* RealDrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawIndexedPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) = nullptr;
    HRESULT(__stdcall* RealClear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD) = nullptr;
    HRESULT(__stdcall* RealStretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE) = nullptr;
    HRESULT(__stdcall* RealColorFill)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR) = nullptr;

    using LockFn = void(__thiscall*)(void*, uint32_t, rage::grcRenderTargetPC*, rage::grcRenderTargetPC*, uint32_t, bool, uint32_t);
    using UnlockFn = void(__thiscall*)(void*, uint32_t, void*, int32_t);
    LockFn pfnLock = nullptr;
    UnlockFn pfnUnlock = nullptr;

    // The pass is decided before the game's lock, which may clear the target already
    void __fastcall LockTarget(void* factory, void*, uint32_t index, rage::grcRenderTargetPC* color, rage::grcRenderTargetPC* depthTarget, uint32_t a5, bool a6, uint32_t mip)
    {
        if (index == 0)
        {
            // A lock the game never undid would grow it for good; deeper than any nesting it uses, it starts over
            if (depth >= int(std::size(stack)))
                depth = 0;
            auto target = color ? color : depthTarget;
            auto kind = KindOf(target);
            if (kind == CascadeTarget && target->mWidth)
                cascadeSide = target->mWidth;
            if (kind == CascadeAtlas)
                atlasTarget = target;
            stack[depth++] = { kind, DropFor(kind), target };
            Refresh();
            if (Tracing())
                TraceEvent(std::format("lock {} (colour {}, depth {}){}", TargetName(target), TargetName(color), TargetName(depthTarget),
                    drop == Drop::Nothing ? "" : drop == Drop::Draws ? ", draws left out" : ", left out"));
        }
        pfnLock(factory, index, color, depthTarget, a5, a6, mip);
        // A cascade whose matrix was made for the atlas is drawn there; one left out this frame needs no target
        if (index == 0 && depth > 0 && stack[depth - 1].kind == CascadeTarget && lastCascade >= 0 && pendingIntoAtlas[lastCascade])
        {
            auto& lock = stack[depth - 1];
            if (lock.drop == Drop::Everything)
                bAtlasLeftOut = true;
            else if (lock.drop != Drop::Nothing)
                ;   // the night's: its quarter of the atlas is cleared below
            else if (BindAtlas(RageDirect3DDevice9::GetRuntimeDevice(), lastCascade))
            {
                lock.intoAtlas = int8_t(lastCascade);
                bAtlasLeftOut = true;
                intoAtlas++;
                if (Tracing())
                {
                    auto device = RageDirect3DDevice9::GetRuntimeDevice();
                    DWORD scissor = 0;
                    RECT rect{};
                    device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
                    device->GetScissorRect(&rect);
                    TraceEvent(std::format("cascade {} drawn into the atlas, quarter {}; scissor {} {} {} {} {}", lastCascade + 1, lastCascade,
                        scissor ? "on" : "off", rect.left, rect.top, rect.right, rect.bottom));
                }
            }
            else
            {
                // Its casters wrote converted depths into the target, which the copy would convert again: this frame's
                // cascade is wrong, the next ones go the game's way
                intoAtlasFailed++;
                bCascadesIntoAtlas = false;
            }
        }
        // The night's quarter of the atlas, bound now: 1.0 in it, which the lighting takes as lit for any depth. The atlas
        // holds 0.5 - 0.0005 z of the casters (cascadeConvert) and a point is lit where that is no less than its own; the
        // cascade cleared to 1.0 and copied there as before was a caster at z 1, which shadowed everything on one side of
        // that plane through the light's space: one long straight shadow over walls and roofs, moving with the camera
        if (index == 0 && depth > 0 && stack[depth - 1].kind == CascadeAtlas && stack[depth - 1].drop == Drop::Everything &&
            bNightCascade && RealClear)
        {
            bNightCascade = false;
            // All of it, once a frame: a clear takes only the viewport, which may still be the cascade's
            auto device = RageDirect3DDevice9::GetRuntimeDevice();
            IDirect3DSurface9* surface = nullptr;
            D3DSURFACE_DESC desc{};
            if (device && nightAtlasFrame != TemporalAA::SceneFrame && SUCCEEDED(device->GetRenderTarget(0, &surface)) && surface)
            {
                surface->GetDesc(&desc);
                surface->Release();
                nightAtlasFrame = TemporalAA::SceneFrame;
                D3DVIEWPORT9 saved{}, whole{ 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
                DWORD scissor = FALSE;
                device->GetViewport(&saved);
                device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
                device->SetViewport(&whole);
                device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
                RealClear(device, 0, nullptr, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);
                device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
                device->SetViewport(&saved);
            }
        }
    }

    // Unlocked while still counted: a resolve the game makes then goes with its pass
    void __fastcall UnlockTarget(void* factory, void*, uint32_t index, void* resolveFlags, int32_t unused)
    {
        pfnUnlock(factory, index, resolveFlags, unused);
        if (index != 0 || depth == 0)
            return;
        if (Tracing())
            TraceEvent(std::format("unlock {}", TargetName(stack[depth - 1].target)));
        depth--;
        Refresh();
    }


    bool DropDraw()
    {
        if (Tracing())
            traceDraws++;
        // The game's fill of a cascade drawn into the atlas: a clear of its quarter to 1.0 instead
        bool fill = bFillPending;
        if (!fill && depth > 0 && stack[depth - 1].kind == CascadeTarget && !stack[depth - 1].statesTraced && Tracing())
        {
            // The states and the shadow matrix's depth column the first caster of a cascade is drawn with, whether into
            // the atlas or the game's way, to tell the two apart
            stack[depth - 1].statesTraced = true;
            if (auto device = RageDirect3DDevice9::GetRuntimeDevice())
            {
                DWORD z = 0, zWrite = 0, zFunc = 0, blend = 0, op = 0, src = 0, dst = 0, mask = 0, scissor = 0;
                device->GetRenderState(D3DRS_ZENABLE, &z);
                device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
                device->GetRenderState(D3DRS_ZFUNC, &zFunc);
                device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
                device->GetRenderState(D3DRS_BLENDOP, &op);
                device->GetRenderState(D3DRS_SRCBLEND, &src);
                device->GetRenderState(D3DRS_DESTBLEND, &dst);
                device->GetRenderState(D3DRS_COLORWRITEENABLE, &mask);
                device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
                float m[16]{};
                device->GetVertexShaderConstantF(60, m, 4);
                D3DVIEWPORT9 vp{};
                device->GetViewport(&vp);
                TraceEvent(std::format("first caster{}: z {} write {} func {}, blend {} op {} {} {}, mask {:#x}, scissor {}, viewport {} {} {}x{}; "
                    "c60-63 w {:.6g} {:.6g} {:.6g} {:.6g}, x {:.6g} {:.6g} {:.6g} {:.6g}",
                    stack[depth - 1].intoAtlas >= 0 ? std::format(" into quarter {}", stack[depth - 1].intoAtlas) : std::string(" the game's way"),
                    z, zWrite, zFunc, blend, op, src, dst, mask, scissor, vp.X, vp.Y, vp.Width, vp.Height,
                    m[3], m[7], m[11], m[15], m[0], m[4], m[8], m[12]));
            }
        }
        if (fill)
        {
            bFillPending = false;
            if (drop == Drop::Nothing && depth > 0 && stack[depth - 1].intoAtlas >= 0 && RealClear)
                if (auto device = RageDirect3DDevice9::GetRuntimeDevice())
                {
                    auto quarter = Quarter(stack[depth - 1].intoAtlas);
                    RealClear(device, 1, &quarter, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);
                    if (Tracing())
                        TraceEvent(std::format("fill of quarter {} cleared to 1.0", stack[depth - 1].intoAtlas));
                    return true;
                }
        }
        if (drop == Drop::Nothing)
            return false;
        dropped[stack[depth - 1].kind]++;
        return true;
    }

    HRESULT __stdcall DrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count)
    {
        return DropDraw() ? D3D_OK : RealDrawPrimitive(device, type, start, count);
    }

    HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count)
    {
        return DropDraw() ? D3D_OK : RealDrawIndexedPrimitive(device, type, base, minIndex, vertices, start, count);
    }

    HRESULT __stdcall DrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
    {
        return DropDraw() ? D3D_OK : RealDrawPrimitiveUP(device, type, count, data, stride);
    }

    HRESULT __stdcall DrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT count, const void* indices,
        D3DFORMAT format, const void* data, UINT stride)
    {
        return DropDraw() ? D3D_OK : RealDrawIndexedPrimitiveUP(device, type, minIndex, vertices, count, indices, format, data, stride);
    }

    HRESULT __stdcall Clear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR colour, float z, DWORD stencil)
    {
        if (Tracing())
            TraceEvent(std::format("clear flags {:#x}, {} rects, colour {:#010x}, z {}{}", flags, count, colour, z, drop == Drop::Everything ? " (left out)" : ""));
        if (drop == Drop::Everything)
            return D3D_OK;
        if (count == 0 && depth > 0 && stack[depth - 1].intoAtlas >= 0)
        {
            auto quarter = Quarter(stack[depth - 1].intoAtlas);
            return RealClear(device, 1, &quarter, flags, colour, z, stencil);
        }
        return RealClear(device, count, rects, flags, colour, z, stencil);
    }

    HRESULT __stdcall SetScissorRect(IDirect3DDevice9* device, const RECT* rect)
    {
        if (rect && Tracing())
            TraceEvent(std::format("scissor {} {} {} {}", rect->left, rect->top, rect->right, rect->bottom));
        // A cascade's own scissor, moved to its quarter like its viewport
        if (rect && depth > 0 && stack[depth - 1].intoAtlas >= 0 && rect->right <= LONG(cascadeSide) && rect->bottom <= LONG(cascadeSide))
        {
            RECT moved = *rect;
            LONG x = LONG(stack[depth - 1].intoAtlas) * LONG(cascadeSide);
            moved.left += x;
            moved.right += x;
            return RealSetScissorRect(device, &moved);
        }
        return RealSetScissorRect(device, rect);
    }

    HRESULT __stdcall StretchRect(IDirect3DDevice9* device, IDirect3DSurface9* source, const RECT* sourceRect, IDirect3DSurface9* target, const RECT* targetRect,
        D3DTEXTUREFILTERTYPE filter)
    {
        if (Tracing())
        {
            D3DSURFACE_DESC from{}, to{};
            if (source)
                source->GetDesc(&from);
            if (target)
                target->GetDesc(&to);
            TraceEvent(std::format("stretch {}x{} -> {}x{}{}", from.Width, from.Height, to.Width, to.Height, drop == Drop::Everything ? " (left out)" : ""));
        }
        return drop == Drop::Everything ? D3D_OK : RealStretchRect(device, source, sourceRect, target, targetRect, filter);
    }

    HRESULT __stdcall ColorFill(IDirect3DDevice9* device, IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR colour)
    {
        return drop == Drop::Everything ? D3D_OK : RealColorFill(device, surface, rect, colour);
    }

    HRESULT __stdcall SetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport)
    {
        if (viewport && Tracing())
            TraceEvent(std::format("viewport {} {} {}x{}", viewport->X, viewport->Y, viewport->Width, viewport->Height));
        // The cascade's viewport, moved to its quarter of the atlas
        if (viewport && depth > 0 && stack[depth - 1].intoAtlas >= 0 && viewport->X == 0 && viewport->Y == 0 && viewport->Width == cascadeSide
            && viewport->Height == cascadeSide)
        {
            D3DVIEWPORT9 quarter = *viewport;
            quarter.X = DWORD(stack[depth - 1].intoAtlas) * cascadeSide;
            return RealSetViewport(device, &quarter);
        }
        return RealSetViewport(device, viewport);
    }

    template <class F>
    void Patch(void** vtable, int index, F& real, F hook)
    {
        if (vtable[index] == reinterpret_cast<void*>(hook))
            return;
        real = reinterpret_cast<F>(vtable[index]);
        injector::WriteMemory(&vtable[index], reinterpret_cast<void*>(hook), true);
    }

    bool Install()
    {
        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        auto factory = rage::grcTextureFactory::g_pTextureFactory ? rage::grcTextureFactoryPC::GetInstance() : nullptr;
        if (!device || !factory)
            return false;
        auto vft = *reinterpret_cast<void***>(factory);
        Patch(vft, 15, pfnLock, reinterpret_cast<LockFn>(&LockTarget));
        Patch(vft, 16, pfnUnlock, reinterpret_cast<UnlockFn>(&UnlockTarget));
        auto vtable = *reinterpret_cast<void***>(device);
        Patch(vtable, 34, RealStretchRect, &StretchRect);
        Patch(vtable, 35, RealColorFill, &ColorFill);
        Patch(vtable, 43, RealClear, &Clear);
        Patch(vtable, 47, RealSetViewport, &SetViewport);
        Patch(vtable, 75, RealSetScissorRect, &SetScissorRect);
        Patch(vtable, 81, RealDrawPrimitive, &DrawPrimitive);
        Patch(vtable, 82, RealDrawIndexedPrimitive, &DrawIndexedPrimitive);
        Patch(vtable, 83, RealDrawPrimitiveUP, &DrawPrimitiveUP);
        Patch(vtable, 84, RealDrawIndexedPrimitiveUP, &DrawIndexedPrimitiveUP);
        return true;
    }
}

namespace RenderSkip
{
    float SunElevation()
    {
        if (nSunDirectionIdx == SIZE_MAX)
            return NAN;
        const auto& sun = rage::grmShaderInfo::getShaderParamData(nSunDirectionIdx);
        float length = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        if (!(length > 0.5f))
            return NAN;
        return std::asin(std::clamp(sun[1] / length, -1.0f, 1.0f)) * 57.29578f;
    }

    bool SunIsLow()
    {
        return IsNight();
    }

    bool SunShadowsDrawn()
    {
        // cascadeFrame moves on only past CascadeDrop's night check
        return cascadeFrame != ~0u && TemporalAA::SceneFrame - cascadeFrame <= 3;
    }
}

class RenderSkipFeature
{
public:
    RenderSkipFeature()
    {
        using namespace RenderSkip;

        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            bCascadesAtNight = iniReader.ReadInteger("SHADOWS", "CascadeShadowsAtNight", 1) != 0;
            bReflectionsHalfRate = iniReader.ReadInteger("MISC", "ReflectionsHalfRate", 1) != 0;
            bFarCascadesHalfRate = iniReader.ReadInteger("SHADOWS", "FarCascadesHalfRate", 1) != 0;
            bMidCascadeHalfRate = iniReader.ReadInteger("SHADOWS", "MidCascadeHalfRate", 0) != 0;
            bTrace = iniReader.ReadInteger("MISC", "RenderSkipTrace", 0) != 0;
            fSunLowBelow = iniReader.ReadFloat("SHADOWS", "CascadeSunElevation", 8.0f);
            // At init: the hook that fills the parameters walks the same vector
            nSunDirectionIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "SunDirection");
            // The game camera, where the procedural generator measures from too (movss xmm2, [0x128e340] in CE 0xc09440)
            if (auto camera = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ? 56 8B 75 0C"); !camera.empty())
                pCamera = *camera.get_first<float*>(12);
            bMatrixTrace = iniReader.ReadInteger("SHADOWS", "CascadeMatrixTrace", 0) != 0;

            bCascadesIntoAtlas = iniReader.ReadInteger("SHADOWS", "CascadesIntoAtlas", 0) != 0;

            // The matrix as CE 0x92dd20 hands it to its global (CE 0x92dd5d: eax the matrix, push eax), called from the
            // setter of the shaders' cascades (CE 0x92d856 returned to, esi the frame's copy there) and from each cascade's
            // own callback (CE 0x92e04f returned to, its index the callback's first argument)
            auto matrix = hook::pattern("50 FF 35 ? ? ? ? E8 ? ? ? ? 83 C4 18 8B E5 5D C3");
            auto setter = hook::pattern("56 FF 35 ? ? ? ? E8 ? ? ? ? 8D 46 10 50 FF 35");
            auto cascadeMatrix = hook::pattern("F3 0F 11 04 24 50 56 E8 ? ? ? ? 83 C4 0C 5E");
            auto fill = hook::pattern("51 56 6A 07 6A 01 51 C7 04 24 00 00 80 3F 6A 01 6A 00 6A 00 E8");
            bool follow = (bFarCascadesHalfRate || bMidCascadeHalfRate) && !setter.empty();
            if (cascadeMatrix.empty() || fill.empty())
                bCascadesIntoAtlas = false;
            if (matrix.empty() || (!follow && !bCascadesIntoAtlas))
            {
                bCascadesIntoAtlas = false;
                return;
            }
            if (follow)
                setterReturn = reinterpret_cast<uintptr_t>(setter.get_first(0));
            if (bCascadesIntoAtlas)
                cascadeMatrixReturn = reinterpret_cast<uintptr_t>(cascadeMatrix.get_first(12));
            static auto MatrixHook = safetyhook::create_mid(matrix.get_first(0), [](SafetyHookContext& regs)
            {
                uintptr_t from = *(uintptr_t*)(regs.ebp + 4);
                if (from == setterReturn)
                {
                    memcpy(caughtMatrix, (const void*)regs.eax, sizeof(caughtMatrix));
                    matrixCaught = true;
                }
                else if (from == cascadeMatrixReturn)
                {
                    int32_t index = *(int32_t*)(*(uintptr_t*)regs.ebp + 8);
                    if (index >= 0 && index < 4)
                    {
                        pendingIntoAtlas[index] = IntoAtlasReady();
                        if (pendingIntoAtlas[index])
                            MatrixForAtlas((float*)regs.eax);
                    }
                }
            });
            if (follow)
            {
                static auto SetterHook = safetyhook::create_mid(setter.get_first(0), [](SafetyHookContext& regs)
                {
                    FollowCascades((float*)regs.esi);
                });
                bFollow = true;
            }
            if (bCascadesIntoAtlas)
            {
                static auto FillHook = safetyhook::create_mid(fill.get_first(0), [](SafetyHookContext&)
                {
                    bFillPending = true;
                });
            }
        };

        // The atlas's depth buffer goes with the device's other default-pool resources; the targets are found anew
        FusionFix::onBeforeReset() += []()
        {
            if (atlasDepth)
                atlasDepth->Release();
            atlasDepth = nullptr;
            atlasTarget = nullptr;
            for (auto& p : pendingIntoAtlas)
                p = false;
        };

        FusionFix::onGameInitEvent() += []()
        {
            if (bCascadesAtNight && !bReflectionsHalfRate && !bFarCascadesHalfRate && !bMidCascadeHalfRate && !bCascadesIntoAtlas && !bTrace)
                return;
            bool installed = Install();
            FusionLog::Write("RenderSkip", "Settings", "%s: cascades %s, the far ones by day %s, reflections %s\n",
                installed ? "in place" : "no device or texture factory yet, nothing is left out",
                bCascadesAtNight ? "drawn at night" : fSunLowBelow > -90.0f ? "not drawn while the sun is low" : "not drawn from 22:00 to 05:00",
                bFarCascadesHalfRate ? (bFollow ? "in turns, following the camera" : "in turns while the camera keeps still") : "every frame",
                bReflectionsHalfRate ? "every other frame" : "every frame");
            FusionLog::Write("RenderSkip", "Settings", "cascades %s\n", bCascadesIntoAtlas ? "drawn straight into the atlas" : "copied into the atlas");
        };

        // A trace starts some 20 seconds into the game; and every 3600 frames, what was left out
        FusionFix::onGameProcessEvent() += []()
        {
            static uint32_t frames = 0;
            ++frames;
            // In a frame the cascades are drawn in: a save is loaded at night before the clock is set to day, so with
            // CascadesIntoAtlas it waits for them however long it takes, otherwise until frame 6000
            if (bTrace && !traceFrom && frames >= 1200 && (cascadeFrame + 2 >= TemporalAA::SceneFrame || (!bCascadesIntoAtlas && frames >= 6000)))
            {
                traceFrom = TemporalAA::SceneFrame + 2;
                traceUntil = traceFrom + 2;
            }
            if (bMatrixTrace && bFollow && frames == 1200)
                matrixTraceFrom = passSeq + 2;
            // The sun's height at each hour of the game's clock, to set CascadeSunElevation by
            static int lastHour = -1;
            if (CClock::ms_nGameClockHours && CClock::GetHours() != lastHour)
            {
                lastHour = CClock::GetHours();
                float elevation = SunElevation();
                if (!std::isnan(elevation))
                    FusionLog::Write("RenderSkip", "Sun", "%02d:00 the sun %.1f degrees above the horizon, its shadows %s\n", lastHour, elevation,
                        IsNight() ? "left out" : "drawn");
            }
            if (frames % 3600)
                return;
            std::string text;
            for (int i = 0; i < 3; i++)
                if (farCascades[i].skipped || farCascades[i].kept)
                    text += std::format("cascade {}: {} frames left out, {} drawn; ", i + 2, farCascades[i].skipped, farCascades[i].kept);
            for (int i = 0; i < 3; i++)
                farCascades[i].skipped = farCascades[i].kept = 0;
            if (bFollow)
                text += std::format("cascades left out matched to their matrix: {}, off more than {} texels or {} m: {} (worst {:.1f} texels, {:.3f} m); ",
                    folded, FoldTexels, FoldDepth, misses, largestTexels, largestDepth);
            folded = misses = 0;
            largestTexels = largestDepth = 0.0f;
            if (intoAtlas || intoAtlasFailed)
                text += std::format("cascades drawn into the atlas: {}{}; ", intoAtlas, intoAtlasFailed ? ", then could not bind it and stopped" : "");
            intoAtlas = 0;
            for (int kind = CascadeTarget; kind < Kinds; kind++)
                if (dropped[kind] || halfRates[kind].skipped || halfRates[kind].kept)
                    text += std::format("{}: {} calls left out, {} frames left out, {} drawn; ", KindNames[kind], dropped[kind],
                        halfRates[kind].skipped, halfRates[kind].kept);
            if (!text.empty())
                FusionLog::WriteText("RenderSkip", "Stats", text);
            for (int kind = 0; kind < Kinds; kind++)
            {
                dropped[kind] = 0;
                halfRates[kind].skipped = halfRates[kind].kept = 0;
            }
        };
    }
} RenderSkipFeature;
