module;

#include <common.hxx>
#include <d3dx9tex.h>

export module framehistory;

import common;
import comvars;
import temporal;

// What the effects of a frame share about the frames before it, for any effect that keeps a history (an
// accumulation, a copy of the scene) and reprojects it: the camera of this scene and of the previous one, cuts
// between them, and the frame counter a history is stamped with. An effect keeps the frame its history was
// written in and asks CanReproject(), instead of tracking cameras and validity of its own.
//
// Temporal AA tracks the camera of every scene of the game's viewport at the start of its G-buffer pass, whatever
// the antialiasing (see temporal.ixx); this reads it from there. Render thread, once the G-buffer pass of the scene
// started: in deferred lighting, post processing and later.

export namespace FrameHistory
{
    struct Camera
    {
        D3DXMATRIX View;
        D3DXMATRIX Projection;              // as rendered, jittered with temporal AA
        D3DXMATRIX ViewProjection;          // View * Projection
        D3DXMATRIX ViewProjectionNoJitter;
        float JitterPixels[2]{};            // how far the rendered content moved, in pixels (y down)
        float Near = 0.0f;
        float Far = 0.0f;
        int32_t Width = 0;
        int32_t Height = 0;
        uint32_t Frame = 0;
        bool Valid = false;
    };
}

namespace FrameHistoryDetail
{
    void Convert(const TemporalAA::FrameCamera& from, FrameHistory::Camera& to)
    {
        from.View.To(&to.View._11);
        from.Projection.To(&to.Projection._11);
        (from.View * from.Projection).To(&to.ViewProjection._11);
        from.ViewProjectionNoJitter.To(&to.ViewProjectionNoJitter._11);
        to.JitterPixels[0] = from.JitterPixels[0];
        to.JitterPixels[1] = from.JitterPixels[1];
        to.Near = from.Near;
        to.Far = from.Far;
        to.Width = from.Width;
        to.Height = from.Height;
        to.Frame = from.Frame;
        to.Valid = from.Valid;
    }

    FrameHistory::Camera current;
    FrameHistory::Camera previous;
    uint32_t converted = 0;

    // Same threshold as the history of temporal AA: [TEMPORAL] CameraCutDistance, meters in one frame
    float CutDistance()
    {
        static float distance = []
        {
            CIniReader iniReader("");
            return std::max(iniReader.ReadFloat("TEMPORAL", "CameraCutDistance", 25.0f), 1.0f);
        }();
        return distance;
    }

    void Update()
    {
        if (converted == TemporalAA::SceneFrame)
            return;
        converted = TemporalAA::SceneFrame;
        Convert(TemporalAA::CurrentCamera, current);
        Convert(TemporalAA::PreviousCamera, previous);
    }
}

export namespace FrameHistory
{
    // Counts the scenes of the game's viewport, 0 before the first one. Stamps histories.
    uint32_t Frame()
    {
        return TemporalAA::SceneFrame;
    }

    const Camera& Current()
    {
        FrameHistoryDetail::Update();
        return FrameHistoryDetail::current;
    }

    // The camera of the scene before this one, Valid only if there was one
    const Camera& Previous()
    {
        FrameHistoryDetail::Update();
        return FrameHistoryDetail::previous;
    }

    // The camera of this scene jumped from the previous one (a cutscene shot, a teleport), or there is no previous one.
    // False while no camera is known. The same test as temporal AA's own.
    bool IsCameraCut()
    {
        using namespace TemporalAA;
        if (!CurrentCamera.Valid)
            return false;
        if (!PreviousCamera.Valid || PreviousCamera.Frame + 1 != CurrentCamera.Frame)
            return true;

        auto current = CurrentCamera.View.Inverse();
        auto previous = PreviousCamera.View.Inverse();
        auto dx = current.m[3][0] - previous.m[3][0];
        auto dy = current.m[3][1] - previous.m[3][1];
        auto dz = current.m[3][2] - previous.m[3][2];
        auto distance = FrameHistoryDetail::CutDistance();
        if (dx * dx + dy * dy + dz * dz > distance * distance)
            return true;

        // forward vectors more than ~60 degrees apart
        auto dot = current.m[2][0] * previous.m[2][0] + current.m[2][1] * previous.m[2][1] + current.m[2][2] * previous.m[2][2];
        return dot < 0.5;
    }

    // A history stamped with Frame() when it was written (0: none) can be reprojected into this scene: it is from the
    // scene right before, through the same shot of the camera
    bool CanReproject(uint32_t written)
    {
        auto& camera = Previous();
        return written != 0 && written + 1 == Frame() && camera.Valid && camera.Frame == written && !IsCameraCut();
    }

    // The projection of this scene is offset by temporal AA (TAA, DLAA or FSR), and their resolve smooths the frame
    bool IsJittered()
    {
        auto& camera = Current();
        return camera.Valid && (camera.JitterPixels[0] != 0.0f || camera.JitterPixels[1] != 0.0f);
    }

    // How far the content of the previous scene sits from this one's through the jitter alone, in texture coordinates
    // (y down): added to a jitter-free motion vector, it reaches a history that was rendered jittered. 0 without jitter.
    std::array<float, 2> JitterDeltaUV()
    {
        auto& current = Current();
        auto& previous = Previous();
        if (!current.Valid || !previous.Valid || current.Width <= 0 || current.Height <= 0)
            return {};
        return { (previous.JitterPixels[0] - current.JitterPixels[0]) / static_cast<float>(current.Width),
                 (previous.JitterPixels[1] - current.JitterPixels[1]) / static_cast<float>(current.Height) };
    }

    // Where effects that accumulate over frames look up their screen space noise this frame, in pixels: a pattern read
    // at pixel + offset gives every pixel another value each frame, which the accumulation averages. The offsets are a
    // hash of the frame, anywhere within 64 pixels, so successive frames jump about the pattern without any steady step
    // between them. A steady step moves the pattern: stepping the values by the golden ratio moved the interleaved
    // gradient noise of SSR.fx by two pixels up every frame, which showed as noise flowing up the screen.
    std::array<float, 2> NoiseOffset()
    {
        auto hash = [](uint32_t x)
        {
            x ^= x >> 16;
            x *= 0x7FEB352Du;
            x ^= x >> 15;
            x *= 0x846CA68Bu;
            x ^= x >> 16;
            return static_cast<float>(x >> 8) * (64.0f / 16777216.0f);
        };
        const auto frame = Frame();
        return { hash(frame), hash(frame ^ 0x9E3779B9u) };
    }

    // Motion of every pixel since the previous scene, in texture coordinates (previous - current), drawn by temporal
    // AA after the G-buffer pass while it is on. Null otherwise.
    IDirect3DTexture9* MotionVectors()
    {
        return TemporalAA::IsMotionVectorsReady() ? TemporalAA::GetMotionVectors() : nullptr;
    }
}
