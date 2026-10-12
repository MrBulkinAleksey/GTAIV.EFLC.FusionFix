module;

#include <common.hxx>
#include "FusionLog.hpp"
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
//    22:00 to 05:00, when the moon casts no shadows to see. Only the draws go, the clears stay: the maps are empty.
//  - ReflectionsHalfRate = 1: the water's reflection and the reflection map (WATER_REFLECTION_COLOUR,
//    REFLECTION_MAP_COLOUR, some 2.3 ms with what is drawn into them) are drawn every other frame; in between their
//    targets keep the last one, nothing drawn, cleared or resolved into them. They are drawn every frame while the
//    camera turns or moves fast, where one frame behind would show.
// The lamps' shadow maps (SHADOW_MAP_COLOUR_POINT, some 3.8 ms at night) can't be left out so: the game hands its tiles
// to other lamps from one frame to the next, and a tile kept from the frame before lit a lamp with another's shadow.
//  - RenderSkipTrace = 1: once, a little after the world is in, two frames of locks, clears, viewports, draws and
//    copies to GTAIV.EFLC.FusionFix.RenderSkip.log, to tell how the game draws its passes.
// The slots are taken over as the other hooks of them do, each passing on to what was there before.
export namespace RenderSkip
{
    bool bCascadesAtNight = false;
    bool bReflectionsHalfRate = true;
    bool bTrace = false;
}

namespace RenderSkip
{
    constexpr uint8_t NightFrom = 22, NightTo = 5;
    constexpr float TurnCos = 0.99966f;     // 1.5 degrees
    constexpr float MoveSquared = 1.0f;     // metres

    enum Kind : uint8_t { None, Cascade, WaterReflection, ReflectionMap, Kinds };
    constexpr const char* KindNames[Kinds] = { "other", "cascades", "water reflection", "reflection map" };

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

    bool CameraMovedSince(const HalfRate& h)
    {
        if (!pCamera)
            return true;
        const float* forward = pCamera - 8;
        float dx = pCamera[0] - h.position[0], dy = pCamera[1] - h.position[1], dz = pCamera[2] - h.position[2];
        float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
        if (!(length > 0.5f))
            return true;
        float facing = (forward[0] * h.forward[0] + forward[1] * h.forward[1] + forward[2] * h.forward[2]) / length;
        return dx * dx + dy * dy + dz * dz > MoveSquared || facing < TurnCos;
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
            h.skip = h.drawn + 1 == frame && !CameraMovedSince(h);
            if (h.skip)
                h.skipped++;
            else
            {
                h.kept++;
                h.drawn = frame;
                if (pCamera)
                {
                    const float* forward = pCamera - 8;
                    float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
                    for (int i = 0; i < 3; i++)
                    {
                        h.position[i] = pCamera[i];
                        h.forward[i] = length > 0.0f ? forward[i] / length : 0.0f;
                    }
                }
            }
        }
        return h.skip;
    }

    bool IsNight()
    {
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
            if (!strcmp(name, "CASCADE_TARGET_R32") || !strcmp(name, "CASCADE_ATLAS"))
                kind = Cascade;
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
    struct Lock { Kind kind; Drop drop; rage::grcRenderTargetPC* target; };
    Lock stack[16]{};
    int depth = 0;
    uint32_t dropped[Kinds]{};

    Drop DropFor(Kind kind)
    {
        switch (kind)
        {
        case Cascade: return !bCascadesAtNight && IsNight() ? Drop::Draws : Drop::Nothing;
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
            stack[depth++] = { kind, DropFor(kind), target };
            Refresh();
            if (Tracing())
                TraceEvent(std::format("lock {} (colour {}, depth {}){}", TargetName(target), TargetName(color), TargetName(depthTarget),
                    drop == Drop::Nothing ? "" : drop == Drop::Draws ? ", draws left out" : ", left out"));
        }
        pfnLock(factory, index, color, depthTarget, a5, a6, mip);
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

    HRESULT(__stdcall* RealDrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT) = nullptr;
    HRESULT(__stdcall* RealDrawIndexedPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) = nullptr;
    HRESULT(__stdcall* RealClear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD) = nullptr;
    HRESULT(__stdcall* RealStretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE) = nullptr;
    HRESULT(__stdcall* RealColorFill)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR) = nullptr;
    HRESULT(__stdcall* RealSetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*) = nullptr;

    bool DropDraw()
    {
        if (Tracing())
            traceDraws++;
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
        return drop == Drop::Everything ? D3D_OK : RealClear(device, count, rects, flags, colour, z, stencil);
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
        Patch(vtable, 81, RealDrawPrimitive, &DrawPrimitive);
        Patch(vtable, 82, RealDrawIndexedPrimitive, &DrawIndexedPrimitive);
        Patch(vtable, 83, RealDrawPrimitiveUP, &DrawPrimitiveUP);
        Patch(vtable, 84, RealDrawIndexedPrimitiveUP, &DrawIndexedPrimitiveUP);
        return true;
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
            bCascadesAtNight = iniReader.ReadInteger("SHADOWS", "CascadeShadowsAtNight", 0) != 0;
            bReflectionsHalfRate = iniReader.ReadInteger("MISC", "ReflectionsHalfRate", 1) != 0;
            bTrace = iniReader.ReadInteger("MISC", "RenderSkipTrace", 0) != 0;
            // The game camera, where the procedural generator measures from too (movss xmm2, [0x128e340] in CE 0xc09440)
            if (auto camera = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ? 56 8B 75 0C"); !camera.empty())
                pCamera = *camera.get_first<float*>(12);
        };

        FusionFix::onGameInitEvent() += []()
        {
            if (bCascadesAtNight && !bReflectionsHalfRate && !bTrace)
                return;
            bool installed = Install();
            FusionLog::Write("RenderSkip", "Settings", "%s: cascades %s, reflections %s\n",
                installed ? "in place" : "no device or texture factory yet, nothing is left out",
                bCascadesAtNight ? "drawn at night" : "not drawn from 22:00 to 05:00",
                bReflectionsHalfRate ? "every other frame" : "every frame");
        };

        // A trace starts some 20 seconds into the game; and every 3600 frames, what was left out
        FusionFix::onGameProcessEvent() += []()
        {
            static uint32_t frames = 0;
            ++frames;
            if (bTrace && !traceFrom && frames == 1200)
            {
                traceFrom = TemporalAA::SceneFrame + 2;
                traceUntil = traceFrom + 2;
            }
            if (frames % 3600)
                return;
            std::string text;
            for (int kind = Cascade; kind < Kinds; kind++)
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
