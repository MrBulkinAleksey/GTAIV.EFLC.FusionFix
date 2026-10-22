module;

#include <common.hxx>
#include "FusionLog.hpp"
#include "carmirrorstable.hxx"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

export module carmirrors;

import common;
import comvars;
import natives;
import settings;
import inireload;
import firstperson;
import coronas;
import entityclass;
import renderskip;

// Real reflections in the player car's mirrors, from the driver's seat in first person (Car Mirrors in the menu).
// The game has planar mirrors for interiors (bathrooms): each frame CE 0xB59DD0 asks 0xA0F820 for the nearest
// visible one an interior registered, which writes its plane (n, d) and the four corners of its glass, then whether a
// mirror is on (0x166DA20) and the camera behind it (0x166DA21). The mirror render phase (CRenderPhaseMirrorReflection)
// draws the scene from the camera reflected in that plane, clipped by it, into MIRROR_RT, and the main scene draws
// the glass as a quad on those corners (0xB59900, in front of the plane along n by a float in its code) with mirror.fx, which reads
// MIRROR_RT at the screen position. With no interior mirror on, the mirror of the player's car the camera looks at
// most goes in instead: its glass from carmirrorstable.hxx (tools/vehicle_mirrors, from vehicles.img), in the car's
// space, placed with the car's matrix, cut to the glass's outline and widened as a convex mirror by
// mirror_car_shape.patch. The game reflects one mirror at a time: the car's others are dark glass, and the change
// from one to another fades out and in.
namespace CarMirrors
{
    static float* plane = nullptr;    // n.xyz, d
    static float* corners = nullptr;  // 4 x xyzw in order round the glass (the game draws them as a strip 0, 1, 3, 2)
    static uint8_t* mirrorOn = nullptr;
    static uint8_t* cameraBehind = nullptr;
    static SafetyHookInline updateHook;
    static SafetyHookInline phaseBuildHook;
    static SafetyHookInline phaseVisibilityHook;
    static SafetyHookInline phaseViewHook;
    static SafetyHookMid waterGateHooks[2];
    static SafetyHookMid phaseWaterHook;
    static SafetyHookMid gatherHook;
    static SafetyHookMid waterTargetHooks[2];

    static bool reverse = false;            // CarMirrorsReverse (hidden): the corners the other way round, if the quad is culled
    static bool rounded = true;             // CarMirrorsRounded (hidden): 0 draws the plain quad, not cut to the glass's outline
    static bool byDrawnCar = true;          // CarMirrorsByDrawnCar (hidden): 0 keeps the update's corners, no dark glass
    static float zoomOut = 1.5f;            // CarMirrorsZoomOut (hidden): how much wider than a flat mirror they show, 1 flat
    static bool water = true;               // CarMirrorsWater (hidden): 0 leaves the water out of the reflection
    static bool farModels = true;           // CarMirrorsFarModels (hidden): 0 leaves the far LOD models out of the reflection, as the game's mirror
    static bool waterReflection = true;     // CarMirrorsWaterReflection (hidden): 0 keeps the game's rule, no water reflection while a mirror is on
    static bool reflectionShaders = true;   // CarMirrorsReflectionShaders (hidden): 0 draws the mirror's entities with the main view's techniques
    static bool extraBuckets = true;        // CarMirrorsExtraBuckets (hidden): 0 leaves out the main scene's buckets 5 and 4 the fourth way
    static bool lampCoronas = true;         // CarMirrorsLampCoronas (hidden): 0 leaves the lamps only the mirror sees unreadied (no corona, no light)
    // The game puts the quad along n by a float its quad code reads (0.035 m on PC, 0.0125 with shaders.ixx's Xbox
    // value): read where the code points as the corners are placed, which lands the quad 5 mm in front of the glass.
    // Taken as 0.035, the quad sat 1.75 cm behind the glass, inside the mirror's housing.
    static float* const* quadOffset = nullptr;
    static constexpr float InFrontOfGlass = 0.005f;
    // The plane the reflection is made in and clipped by stands this far in front of the glass: the housing's rim stood
    // in front of the glass by 1-2 cm and, kept by the clip, covered the reflected view right by the eye (a piece of the
    // housing over the side mirrors, an interior mirror black).
    static constexpr float ClipAhead = 0.025f;
    // The mirrors set as a driver sets them: the plane turned halfway between the way to the eye and the way to what it
    // should show, 10 m straight back along the car turned by the hidden ini's degrees, X across and Y up:
    // CarMirrorsSideAimX in towards the car, CarMirrorsInteriorAimX to the car's right, CarMirrorsSideAimY and
    // CarMirrorsInteriorAimY up; the quad stays on the glass. The eye is kept in the car's space (the driver's seat),
    // the model's glass faced anywhere.
    static float sideAimX = 0.0f, sideAimY = -1.5f, interiorAimX = 0.0f, interiorAimY = -3.0f;
    static float eyeInCar[3]{};
    static float eyeInWorld[3]{};            // the camera's, as the mirror was last set (diagnostics)
    static bool eyeKnown = false;
    static constexpr float MinLook = 0.3f;  // cosine: a mirror further off the view than ~73 degrees is not taken
    static constexpr float Keep = 0.15f;    // the mirror reflected stays until another is this much nearer the view

    // What the car's mirrors show: the one the reflection is made for (the game has one mirror) fades out over FadeSeconds
    // when the view goes to another, which then fades in; every other mirror of the car, and all of them while none is
    // looked at, are dark glass, as the game's own look of them (an environment map's shine) was bad.
    static constexpr float FadeSeconds = 0.15f;
    static const CarMirrorGlass* active = nullptr; // the mirror reflected, main thread
    static float shown = 0.0f;                     // how much of its reflection shows, 0..1
    static const CarMirrorGlass* carGlasses[4]{};  // this frame's car's mirrors
    static int carGlassCount = 0;
    static uint32_t usedModel = 0;
    static bool usedThisFrame = false;             // a reflection is made this frame
    static bool quadsThisFrame = false;            // the car's mirrors are drawn this frame (reflected or dark)
    static float activeDarkness = 1.0f;

    // The mirror phase's CPU time while a car mirror is on, for the log every 300 frames (CarMirrorsLog, hidden, 0).
    static bool logStats = false;
    static double buildMs = 0.0, visibilityMs = 0.0;
    static uint32_t timedFrames = 0;
    static uint32_t outdoorScans = 0;      // frames the outdoor scan ran for the mirror
    static uint32_t waterQueued = 0;       // the water's commands put in the mirror phase's draw list
    static uint32_t waterGathered = 0, waterRolledBack = 0;
    static uint32_t mirrorWaterItems[3]{}, mainWaterItems[3]{}, drawnWaterItems[3]{};
    static uint32_t waterFill[3]{};        // the most of each buffer the frame used, after the mirror's
    static uint32_t waterDrawn = 0;        // the mirror pass's water draws (render thread)
    static uint32_t techniqueDraws = 0;    // frames the mirror's entities went in the reflection techniques
    static uint32_t skyDraws = 0;          // the car mirror's skies drawn (render thread)
    static uint32_t coronaDraws = 0;       // and its coronas
    static uint32_t mirrorCoronaFrames = 0, mirrorCoronaTested = 0, mirrorCoronaListed = 0; // listed for the mirror's view (main thread)
    // within NearCorona of the mirror, registered on its side of the plane: [directional (headlights), the rest],
    // all of them, those the game camera's view left out, those the mirror's view lists
    static constexpr float NearCorona = 60.0f;
    static uint32_t nearRegistered[2]{}, nearOutOfView[2]{}, nearListed[2]{};
    static uintptr_t mirrorPhase = 0;      // CRenderPhaseMirrorReflection, as PhaseView last saw it for our mirror
    // The game's render states as the techniques' pass states have them (0 ZENABLE, 2 ZWRITEENABLE, 3 ALPHATESTENABLE,
    // 4 SRCBLEND, 5 DESTBLEND, 6 CULLMODE, 7 ZFUNC, 8 ALPHAREF, 9 ALPHAFUNC, 10 ALPHABLENDENABLE, 19 COLORWRITEENABLE,
    // 23 BLENDOP; CE 0x437140 sets one on the device, its value kept at [0x108B420 + 4 x type]). Not CE 0x432C20's own
    // numbers (the draw lists' render state commands, 0x8DBF20): there 2 is a blend preset (0 source alpha over the
    // rest, 1 one and one), 5 the depth compare by its own table (2 less, 3 less or equal), 8 the blend on.
    static void (__cdecl* setPassState)(uint32_t type, uint32_t value) = nullptr;
    static const uint32_t* passStateCache = nullptr;
    // A frame capture (2026-10-20) showed nearly every entity of the mirror's pass drawn with blending one and one, the
    // alpha reference 100 and here and there no depth write: what a technique sets stays for every entity after it, the
    // reflection techniques setting nothing of their own. The one and one came from this code (the mirror's blend preset
    // command, taken for a depth write, made 1); the reference and the rest from the techniques of what the mirror draws
    // that the game's mirrors never did (peds, CarMirrorsReflectionShaders' group, hair turning the depth write off).
    // CarMirrorsEntityStates (hidden, 0 leaves it): each entity of the mirror's pass gets the states it began with put
    // back after it, as if every technique had set only its own.
    static bool isolateStates = true;
    static constexpr uint32_t IsolatedTypes[] = { 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 19, 23 };
    static uint32_t isolatedSaved[std::size(IsolatedTypes)]{};
    static bool isolatedOn = false;      // render thread
    static uint32_t extraBucketDraws = 0;  // and the main scene's extra buckets (ExtraBuckets)
    static uint32_t waterFlagMissing = 0;  // the mirror's water draws whose draw list had no flag 0x10 (render thread)
    static const uint32_t* waterTotals[2]{}; // CE 0x1550EAC, 0x154E300: water.dat's quads and triangles
    // The car, for the draw list's snapshots and the phase's plane.
    static std::atomic<uintptr_t> drawnVehicle{ 0 };
    static SafetyHookMid quadHook;
    static std::vector<SafetyHookMid> queueHooks;
    static void* quadFunction = nullptr;   // CE 0xB59900
    static uint32_t quadDrawn = 0, quadBySnapshot = 0, quadsDark = 0;
    // The corners as the quad is put in the scene's draw list (main thread, CE 0xADD526 / 0xADD930), with the car where
    // its own draws of that list are made: the quad's command hands its parameter to 0xB59900, which never reads it, so
    // it carries the snapshot's number to the render thread. The car's other mirrors go in as commands of our own
    // calling 0xB59900 with theirs. A snapshot holds the corners on the glass, the glass (its outline) and how dark.
    struct Snapshot
    {
        std::atomic<uint32_t> number{ 0 };
        float corners[16]{};
        const CarMirrorGlass* glass = nullptr;
        float darkness = 0.0f;
    };
    static Snapshot snapshots[32];
    static uint32_t nextSnapshot = 1;

    // A callback command of the game's own (vtable CE 0xE81E0C: its execute calls fn(parameter) on the render
    // thread), made as the draw lists make the mirror quad's: allocate 0x10 bytes (0x8DC3A0), the base vtable, the
    // command's number from the counter, the vtable, fn and its parameter, then add it (0x499E30).
    struct GameCallback
    {
        void* (__cdecl* allocate)(uint32_t size, uint32_t flags) = nullptr;
        void (__cdecl* add)(void* command) = nullptr;
        uint32_t baseVtable = 0, vtable = 0;
        uint32_t* counter = nullptr;

        void Queue(const void* fn, uint32_t parameter) const
        {
            if (!allocate || !add || !fn)
                return;
            auto command = static_cast<uint32_t*>(allocate(0x10, 0));
            if (command)
            {
                command[0] = baseVtable;
                command[1] ^= (command[1] ^ *counter) & 0x3FFF;
                ++*counter;
                command[0] = vtable;
                command[2] = reinterpret_cast<uint32_t>(fn);
                command[3] = parameter;
            }
            add(command);
        }
    };
    static GameCallback callback;

    // Render thread, around the mirror phase's draws: c204 for the car glass (vehicle_glass_reflections_wet.patch,
    // set for the main view by postfx.ixx): the player's car's own glass keeps 40% of its tint in the mirror too, within
    // 4 m of the reflected eye, which stands further from the rear window than the driver; an interior mirror showed
    // the dark tinted rear window and little else.
    // The car glass's screen space reflections (vehicle_glass_reflections.patch) read the main view's camera (s9), its
    // depth (s11) and its lit scene (s13), bound by postfx.ixx after the deferred lighting until the post processing:
    // the mirror's phase, drawn in between, had its glass reflect the main view's screen at the mirror's pixels (an
    // interior mirror's rear window showed the phone and ghosts of trees). Unbound as the mirror's pass starts, s9
    // empty keeping the game's environment map; not put back (no reference held over a device reset): the main
    // scene's glass is drawn by then, and the next frame binds them again.
    // c204.z 1 has the water opaque and reflect the sky's colour at the horizon, not the main view's water reflection read
    // at the mirror's pixels (water_mirror_opaque.patch, see MirrorWaterPass).
    // The phase clears depth and stencil only (CE 0xD769CE, its colour flag 0), the sky meant to cover the rest: in a
    // tunnel nothing did, and the mirror kept its frames before where nothing was drawn (trails). Cleared to black
    // as the pass starts, MIRROR_RT bound and its viewport set by then.
    // Low byte 2: the interior mirror's pass, the player's car's position (as its draw command keeps it) in ownPlaces at
    // the index in the second byte: MirrorDrawBegin leaves its glass out (vehicle_glass_reflections_wet_mirror.patch).
    static float ownPlaces[4][3] = {};
    static uint32_t ownPlaceNext = 0;
    static bool interiorPass = false; // render thread
    static bool mirrorPass = false;   // render thread, any car mirror's pass
    static float ownPlace[3] = {};
    static bool ownDrawOn = false;

    static void SetGlassConstant(IDirect3DDevice9* device, bool on, bool leaveOut)
    {
        const float tint[4] = { on ? 0.4f : 0.0f, on ? 4.0f * 4.0f : 0.0f, on ? 1.0f : 0.0f, leaveOut ? 1.0f : 0.0f };
        device->SetPixelShaderConstantF(204, tint, 1);
        if (auto real = RageDirect3DDevice9::RealDevice(device); real != device)
            real->SetPixelShaderConstantF(204, tint, 1);
    }

    static void __cdecl MirrorPassTint(uint32_t on)
    {
        auto device = rage::grcDevice::GetD3DDevice();
        if (!device)
            return;
        if (on)
            device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        mirrorPass = on != 0;
        interiorPass = (on & 0xff) == 2;
        if (interiorPass)
            std::copy_n(ownPlaces[(on >> 8) & 3], 3, ownPlace);
        ownDrawOn = false;
        SetGlassConstant(device, on != 0, false);
        if (on)
            for (DWORD stage : { 9u, 11u, 13u })
                RageDirect3DDevice9::SetTextureBoth(device, stage, nullptr);
    }

    // The technique group drawn with (CE [0x106B308], -1 the default group; its setter CE 0x42F0A0).
    static int32_t* currentTechniqueGroup = nullptr;
    static int32_t pedSavedGroup = -1;
    static bool pedGroupSwapped = false;

    // Entity draw hooks (render thread).
    // Peds in a car mirror's pass are drawn in the default technique group, as in the game's own mirrors: in the
    // "reflection" group (PushReflectionTechniques) the ped shaders (gta_ped, gta_ped_skin, the hair) have no technique,
    // and a shader without one in the group draws nothing (CE 0x42EBCB); the parts drawn with gta_default and its kin
    // took their reflection_drawskinned, whose vertex shader is the unskinned one: peds walked in the bind pose.
    // In the interior mirror's pass, the player's car's draws (a vehicle's command placed where the car stood as the
    // mirror's list was made) have c204.w 1, and its glass shaders draw none of it: the rear window covered the
    // mirror's sky black even with the blend states of vehicle_glass_reflection_blend.patch, and the sky was there once
    // the window was broken.
    static void MirrorDrawBegin(EntityClass::Kind kind)
    {
        if (mirrorPass)
        {
            if (isolateStates && passStateCache && setPassState && !isolatedOn)
            {
                for (size_t i = 0; i < std::size(IsolatedTypes); ++i)
                    isolatedSaved[i] = passStateCache[IsolatedTypes[i]];
                isolatedOn = true;
            }
        }
        if (mirrorPass && kind == EntityClass::Kind::Ped && currentTechniqueGroup && *currentTechniqueGroup != -1)
        {
            pedSavedGroup = *currentTechniqueGroup;
            *currentTechniqueGroup = -1;
            pedGroupSwapped = true;
        }
        if (!interiorPass || kind != EntityClass::Kind::Vehicle)
            return;
        uint16_t model;
        const float* place;
        if (!EntityClass::CurrentModel(model, place))
            return;
        float d2 = 0.0f;
        for (int i = 0; i < 3; ++i)
            d2 += (place[i] - ownPlace[i]) * (place[i] - ownPlace[i]);
        if (d2 > 0.25f * 0.25f)
            return;
        if (auto device = rage::grcDevice::GetD3DDevice())
        {
            SetGlassConstant(device, true, true);
            ownDrawOn = true;
        }
    }

    static void MirrorDrawEnd(EntityClass::Kind)
    {
        if (isolatedOn)
        {
            for (size_t i = 0; i < std::size(IsolatedTypes); ++i)
                if (passStateCache[IsolatedTypes[i]] != isolatedSaved[i])
                    setPassState(IsolatedTypes[i], isolatedSaved[i]);
            isolatedOn = false;
        }
        if (pedGroupSwapped)
        {
            *currentTechniqueGroup = pedSavedGroup;
            pedGroupSwapped = false;
        }
        if (!ownDrawOn)
            return;
        ownDrawOn = false;
        if (auto device = rage::grcDevice::GetD3DDevice())
            SetGlassConstant(device, true, false);
    }

    static double NowMs()
    {
        static LARGE_INTEGER frequency{};
        if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return double(now.QuadPart) * 1000.0 / double(frequency.QuadPart);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_CAR_MIRRORS");
        return !pref || pref->get() != 0;
    }

    static void Transform(const float* m, const float (&v)[3], float (&out)[3], bool point)
    {
        for (int i = 0; i < 3; ++i)
            out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + (point ? m[12 + i] : 0.0f);
    }

    static float Dot(const float (&a)[3], const float (&b)[3])
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    static void Normalize(float (&v)[3])
    {
        const float length = std::sqrt(Dot(v, v));
        if (length > 0.0f)
            for (auto& x : v) x /= length;
    }

    // The plane's normal in the car's space: halfway between the way to the eye and the way to what the mirror shows.
    static void Aim(const CarMirrorGlass& glass, float (&out)[3])
    {
        std::copy_n(glass.normal, 3, out);
        if (!eyeKnown)
            return;
        const float* c = glass.centre;
        const bool side = glass.kind != 2;
        // across: towards the car for side mirrors (+x for the left one), to the car's right (+x) for the interior one
        const float across = side ? (c[0] < 0.0f ? 1.0f : -1.0f) * std::tan(sideAimX * 0.01745329f)
                                  : std::tan(interiorAimX * 0.01745329f);
        const float rise = std::tan((side ? sideAimY : interiorAimY) * 0.01745329f);
        const float toTarget[3] = { 10.0f * across, -10.0f, 10.0f * rise };
        float toEye[3] = { eyeInCar[0] - c[0], eyeInCar[1] - c[1], eyeInCar[2] - c[2] };
        const float eyeLength = std::sqrt(Dot(toEye, toEye)), targetLength = std::sqrt(Dot(toTarget, toTarget));
        if (eyeLength < 0.05f)
            return;
        float n[3];
        for (int i = 0; i < 3; ++i) n[i] = toEye[i] / eyeLength + toTarget[i] / targetLength;
        const float length = std::sqrt(Dot(n, n));
        if (length < 1e-3f || Dot(n, glass.normal) <= 0.2f * length)
            return; // a turn this far would show the glass from its edge: keep the model's
        for (int i = 0; i < 3; ++i) out[i] = n[i] / length;
    }

    // The mirror's plane in the world by the car's matrix m, as set (Aim), ClipAhead in front of the glass's corner
    // that stands furthest out along it: turned from the glass, the plane through the middle had one edge of the
    // housing's rim in front of it again, kept in the reflection.
    static void PlacePlane(const float* m, const CarMirrorGlass& glass)
    {
        float aimed[3], centre[3], normal[3], right[3], up[3];
        Aim(glass, aimed);
        Transform(m, glass.centre, centre, true);
        Transform(m, aimed, normal, false);
        Transform(m, glass.right, right, false);
        Transform(m, glass.up, up, false);
        Normalize(normal);
        // the most a corner of the glass's rectangle stands out along the plane's normal
        const float out = std::abs(Dot(normal, right)) + std::abs(Dot(normal, up));
        for (int i = 0; i < 3; ++i) plane[i] = normal[i];
        plane[3] = Dot(normal, centre) + out + ClipAhead;
    }

    // The glass's corners in the world by the car's matrix m, round it as the driver sees it (bottom left, bottom
    // right, top right, top left), InFrontOfGlass in front of it; the game's own push along the plane is taken off as
    // the quad is drawn (Compensate).
    static void GlassCorners(const float* m, const CarMirrorGlass& glass, float* out)
    {
        float centre[3], normal[3], right[3], up[3];
        Transform(m, glass.centre, centre, true);
        Transform(m, glass.normal, normal, false);
        Transform(m, glass.right, right, false);
        Transform(m, glass.up, up, false);
        Normalize(normal);
        static constexpr float signs[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
        for (int k = 0; k < 4; ++k)
        {
            const auto& s = signs[reverse ? (5 - k) % 4 : k];
            for (int i = 0; i < 3; ++i)
                out[k * 4 + i] = centre[i] + right[i] * s[0] + up[i] * s[1] + normal[i] * InFrontOfGlass;
            out[k * 4 + 3] = 1.0f;
        }
    }

    // Render thread, right before the game draws a quad: the corners it will move along the plane's normal by its
    // offset (both read as it reads them), moved back.
    static void Compensate(const float* onGlass)
    {
        float n[3] = { plane[0], plane[1], plane[2] };
        Normalize(n);
        const float offset = quadOffset && *quadOffset ? **quadOffset : 0.035f;
        for (int k = 0; k < 4; ++k)
        {
            for (int i = 0; i < 3; ++i)
                corners[k * 4 + i] = onGlass[k * 4 + i] - n[i] * offset;
            corners[k * 4 + 3] = 1.0f;
        }
    }

    static void Off(const char* why)
    {
        if (quadsThisFrame || active)
            FusionLog::Write("CarMirrors", "Select", "off: %s\n", why);
        active = nullptr;
        shown = 0.0f;
        carGlassCount = 0;
        usedModel = 0;
        quadsThisFrame = false;
        drawnVehicle.store(0, std::memory_order_relaxed);
    }

    static const char* KindName(const CarMirrorGlass* glass)
    {
        static constexpr const char* kinds[] = { "left", "right", "interior" };
        return glass ? kinds[glass->kind] : "none";
    }

    // The far LOD models (entity +0x24 & 0x40) go only to a phase with the flag 0x100 (CE 0xAE7906), which also has the
    // scan walk the far cells (CE 0xAE9EFC); the water reflection's phase has it, the mirror's not: the city across
    // the river was missing behind the near streets, the bare sky and the water in its place. The scan only marks the
    // far cells for the phase's list (CE 0xAE93C0); their models are sorted to the phases later, with the flag read
    // then, so it stays on the phase from its scan to its draw list (CarMirrorsFarModels, hidden).
    static constexpr uint32_t PhaseFarModelsFlag = 0x100;
    static uintptr_t farModelsPhase = 0;    // main thread: the phase given the flag, until its draw list or the next frame

    static void TakeFarModelsFlag()
    {
        if (!farModelsPhase)
            return;
        *reinterpret_cast<uint32_t*>(farModelsPhase + 0x8E8) &= ~PhaseFarModelsFlag;
        farModelsPhase = 0;
    }

    // Main thread, once a frame, after the game looked for an interior mirror.
    static void Update()
    {
        updateHook.unsafe_call<void>();
        usedThisFrame = false;
        TakeFarModelsFlag();
        static double lastMs = 0.0;
        const double nowMs = NowMs();
        const float seconds = lastMs > 0.0 ? std::clamp(float(nowMs - lastMs) * 0.001f, 0.0f, 0.1f) : 0.0f;
        lastMs = nowMs;
        if (!plane || !corners || !mirrorOn || !cameraBehind)
            return;
        if (*mirrorOn)
            return Off("an interior mirror is on");
        if (!Enabled())
            return Off("switched off");
        if (!FirstPersonInVehicle())
            return Off("not in first person in a car");

        Ped ped = 0;
        Natives::GetPlayerChar(Natives::GetPlayerId(), &ped);
        if (!ped || !Natives::IsCharSittingInAnyCar(ped))
            return Off("the player is not sitting in a car");
        Vehicle handle = 0;
        Natives::GetCarCharIsUsing(ped, &handle);
        const auto pool = CVehicle::GetCheckedVehiclePool();
        if (!handle || !pool)
            return Off("no vehicle");
        const auto vehicle = reinterpret_cast<uintptr_t>(pool->GetAt(handle));
        const float* m = vehicle ? CEntity::GetMatrix(vehicle) : nullptr;
        if (!m)
            return Off("no vehicle matrix");
        uint32_t model = 0;
        Natives::GetCarModel(handle, &model);

        Cam camera = 0;
        Natives::GetRootCam(&camera);
        if (!camera)
            return Off("no camera");
        float eye[3]{}, rotation[3]{};
        Natives::GetCamPos(camera, &eye[0], &eye[1], &eye[2]);
        std::copy_n(eye, 3, eyeInWorld);
        Natives::GetCamRot(camera, &rotation[0], &rotation[1], &rotation[2]);
        const float pitch = rotation[0] * 0.01745329f, heading = rotation[2] * 0.01745329f;
        const float forward[3] = { -std::sin(heading) * std::cos(pitch), std::cos(heading) * std::cos(pitch), std::sin(pitch) };

        if (model != usedModel)
        {
            active = nullptr;
            shown = 0.0f;
            carGlassCount = 0;
            for (const auto& glass : CarMirrorTable)
                if (glass.model == model && carGlassCount < int(std::size(carGlasses)))
                    carGlasses[carGlassCount++] = &glass;
            FusionLog::Write("CarMirrors", "Select", "model %08X: %d mirrors in the table\n", model, carGlassCount);
            usedModel = model;
        }
        if (!carGlassCount)
        {
            quadsThisFrame = false;
            return; // the game's own mirrors on a car the table does not know
        }

        // the mirror looked at: nearest the view's middle, the one shown kept until another is Keep nearer
        const CarMirrorGlass* target = nullptr;
        float bestScore = -2.0f, targetLook = -2.0f;
        for (int g = 0; g < carGlassCount; ++g)
        {
            const auto& glass = *carGlasses[g];
            float centre[3], normal[3], toEye[3];
            Transform(m, glass.centre, centre, true);
            Transform(m, glass.normal, normal, false);
            for (int i = 0; i < 3; ++i) toEye[i] = eye[i] - centre[i];
            const float distance = std::sqrt(Dot(toEye, toEye));
            if (Dot(normal, toEye) <= 0.0f || distance < 0.05f)
                continue; // seen from behind
            const float toGlass[3] = { -toEye[0] / distance, -toEye[1] / distance, -toEye[2] / distance };
            const float look = Dot(forward, toGlass);
            const float score = look + (&glass == active ? Keep : 0.0f);
            if (look >= MinLook && score > bestScore)
            {
                bestScore = score;
                targetLook = look;
                target = &glass;
            }
        }

        // the reflected mirror fades out before another takes its place, and in after
        const CarMirrorGlass* before = active;
        if (target != active)
        {
            shown -= seconds / FadeSeconds;
            if (shown <= 0.0f || !active)
            {
                shown = 0.0f;
                active = target;
            }
        }
        else if (active)
            shown = (std::min)(shown + seconds / FadeSeconds, 1.0f);
        if (active != before)
            FusionLog::Write("CarMirrors", "Select", "model %08X: the %s mirror reflected (%.2f off the view), was %s\n",
                model, KindName(active), targetLook, KindName(before));
        const float eased = shown * shown * (3.0f - 2.0f * shown);
        activeDarkness = 1.0f - eased;

        // the eye in the car's space, the mirrors set for it
        for (int j = 0; j < 3; ++j)
            eyeInCar[j] = (eye[0] - m[12]) * m[4 * j] + (eye[1] - m[13]) * m[4 * j + 1] + (eye[2] - m[14]) * m[4 * j + 2];
        eyeKnown = std::isfinite(eyeInCar[0]) && std::isfinite(eyeInCar[1]) && std::isfinite(eyeInCar[2]);

        // the game's quad goes on the reflected mirror, else on the first, dark; no reflection made without one
        const CarMirrorGlass* primary = active ? active : carGlasses[0];
        PlacePlane(m, *primary);
        GlassCorners(m, *primary, corners);
        drawnVehicle.store(vehicle, std::memory_order_relaxed);
        *mirrorOn = 1;
        *cameraBehind = active ? 0 : 1; // with it set the phase makes nothing, the quads are still drawn
        usedThisFrame = active != nullptr;
        quadsThisFrame = true;
    }

    // What the sky is drawn with, the last time (render thread, read by LogTiming): the reflected camera, its
    // projection's depth column, the near and far planes, the clip plane, the target's viewport.
    static struct
    {
        float camera[3], forward[3], up[3], depthColumn[4], nearFar[2], clipPlane[4], plane[4];
        DWORD clipEnable, cull;
        D3DVIEWPORT9 viewport;
        bool known;
    } skyState{};

    static void WriteStats();
    static void ResetStats();

    static void LogTiming()
    {
        if (++timedFrames < 300)
            return;
        if (logStats)
            WriteStats();
        ResetStats();
    }

    static void WriteStats()
    {
        FusionLog::Write("CarMirrors", "Cost", "mirror phase CPU per frame: draw list %.3f ms, visibility %.3f ms; outdoor scans %u of %u frames; "
            "quads drawn %u, by the draw list's car %u, dark %u; entities in the reflection techniques %u, skies %u, coronas %u, extra buckets %u\n",
            buildMs / timedFrames, visibilityMs / timedFrames, outdoorScans, timedFrames, quadDrawn, quadBySnapshot, quadsDark, techniqueDraws, skyDraws, coronaDraws, extraBucketDraws);
        // per mirror draw: the coronas the game listed this frame, how many of them behind its camera, how many drawn
        if (coronaDraws)
            FusionLog::Write("CarMirrors", "Coronas", "per mirror draw: %.1f listed, %.1f of them behind the game camera, %.1f drawn\n",
                float(CoronaRender::nOtherListed) / coronaDraws, float(CoronaRender::nOtherBehind) / coronaDraws,
                float(CoronaRender::nOtherDrawn) / coronaDraws);
        if (mirrorCoronaFrames)
            FusionLog::Write("CarMirrors", "Coronas", "listed for the mirror per frame: %.1f tested in front of its plane, %.1f seen (%u frames)\n",
                float(mirrorCoronaTested) / mirrorCoronaFrames, float(mirrorCoronaListed) / mirrorCoronaFrames, mirrorCoronaFrames);
        if (mirrorCoronaFrames)
            FusionLog::Write("CarMirrors", "Coronas", "within %.0f m of the mirror per frame, headlights / the rest: registered %.1f / %.1f, "
                "of them out of the game camera's view %.1f / %.1f, listed by the mirror's %.1f / %.1f\n", NearCorona,
                float(nearRegistered[0]) / mirrorCoronaFrames, float(nearRegistered[1]) / mirrorCoronaFrames,
                float(nearOutOfView[0]) / mirrorCoronaFrames, float(nearOutOfView[1]) / mirrorCoronaFrames,
                float(nearListed[0]) / mirrorCoronaFrames, float(nearListed[1]) / mirrorCoronaFrames);
        // items: the water's quads, triangles and far cells, summed over the frames
        FusionLog::Write("CarMirrors", "Water", "water.dat %u quads, %u triangles; gathered %u frames, rolled back %u (over the buffers); "
            "the mirror's items %u/%u/%u, the main scene's %u/%u/%u; buffers filled up to %u/1000, %u/40, %u/400; queued %u, drawn %u, "
            "items drawn %u/%u/%u, without the flag in the draw list %u\n", waterTotals[0] ? *waterTotals[0] : 0, waterTotals[1] ? *waterTotals[1] : 0,
            waterGathered, waterRolledBack, mirrorWaterItems[0], mirrorWaterItems[1], mirrorWaterItems[2],
            mainWaterItems[0], mainWaterItems[1], mainWaterItems[2], waterFill[0], waterFill[1], waterFill[2],
            waterQueued, waterDrawn, drawnWaterItems[0], drawnWaterItems[1], drawnWaterItems[2], waterFlagMissing);
        if (skyState.known)
        {
            const auto& k = skyState;
            const float eyeToPlane = k.plane[0] * eyeInWorld[0] + k.plane[1] * eyeInWorld[1] + k.plane[2] * eyeInWorld[2] - k.plane[3];
            const float cameraToPlane = k.plane[0] * k.camera[0] + k.plane[1] * k.camera[1] + k.plane[2] * k.camera[2] - k.plane[3];
            FusionLog::Write("CarMirrors", "Sky", "%s mirror: eye %.2f %.2f %.2f (%.2f m from the plane), reflected camera %.2f %.2f %.2f "
                "(%.2f m), forward %.2f %.2f %.2f, up %.2f %.2f %.2f; plane %.3f %.3f %.3f %.2f; near %.3f far %.1f, projection depth column "
                "%.4f %.4f %.4f %.4f; clip planes %lu, plane 0 %.3f %.3f %.3f %.3f; cull %lu; viewport %lu %lu %lu x %lu, z %.2f..%.2f\n",
                KindName(active), eyeInWorld[0], eyeInWorld[1], eyeInWorld[2], eyeToPlane, k.camera[0], k.camera[1], k.camera[2], cameraToPlane,
                k.forward[0], k.forward[1], k.forward[2], k.up[0], k.up[1], k.up[2], k.plane[0], k.plane[1], k.plane[2], k.plane[3],
                k.nearFar[0], k.nearFar[1], k.depthColumn[0], k.depthColumn[1], k.depthColumn[2], k.depthColumn[3], k.clipEnable,
                k.clipPlane[0], k.clipPlane[1], k.clipPlane[2], k.clipPlane[3], k.cull, k.viewport.X, k.viewport.Y, k.viewport.Width,
                k.viewport.Height, k.viewport.MinZ, k.viewport.MaxZ);
        }
    }

    static void ResetStats()
    {
        mirrorCoronaFrames = mirrorCoronaTested = mirrorCoronaListed = 0;
        nearRegistered[0] = nearRegistered[1] = nearOutOfView[0] = nearOutOfView[1] = nearListed[0] = nearListed[1] = 0;
        CoronaRender::nOtherListed = CoronaRender::nOtherBehind = CoronaRender::nOtherDrawn = 0;
        outdoorScans = quadDrawn = quadBySnapshot = quadsDark = waterQueued = waterGathered = waterRolledBack = waterDrawn = techniqueDraws = skyDraws = coronaDraws = extraBucketDraws = waterFlagMissing = 0;
        for (int i = 0; i < 3; ++i)
            mirrorWaterItems[i] = mainWaterItems[i] = drawnWaterItems[i] = waterFill[i] = 0;
        buildMs = visibilityMs = 0.0;
        timedFrames = 0;
    }

    // The phase's flags (+0x8E8, the draw list takes a copy as it is made): 0x10 has the scan gather the water seen
    // in the phase's list (CE 0xB1E810 -> 0xAD7D80) and the water's draw (CE 0xAD7EF0) draw it; the mirror phase has
    // it not. Set from the mirror's water gathered (GatherMirrorWater) to its draw list's end, with the water queued
    // in that list (PhaseWater).
    static constexpr uint32_t PhaseWaterFlag = 0x10;
    static uintptr_t waterFlagPhase = 0;      // the phase the flag was set on by us, 0 none
    static void* waterFunction = nullptr;     // CE 0xAD7EF0, cdecl(the phase's list)

    static void SetWaterFlag(uintptr_t phase)
    {
        auto& flags = *reinterpret_cast<uint32_t*>(phase + 0x8E8);
        if (flags & PhaseWaterFlag)
            return; // the game's own
        flags |= PhaseWaterFlag;
        waterFlagPhase = phase;
    }

    static void ClearWaterFlag()
    {
        if (!waterFlagPhase)
            return;
        *reinterpret_cast<uint32_t*>(waterFlagPhase + 0x8E8) &= ~PhaseWaterFlag;
        waterFlagPhase = 0;
    }

    // The mirror phase draws no sky (a bathroom mirror sees none): the car mirror's showed whatever MIRROR_RT held,
    // black once it was cleared. The water reflection's sky is a callback (CE 0xC107A0, queued at 0xD76D38) drawing it
    // (CE 0xDBC170, clip planes off round it) for draw lists with flag 0x20; in the sky the draw list's 0x20000 sets
    // the timecycle's sky colours (0xDBC2C6) and picks how it is drawn (0xDBC743), 0x40000 the reflection map's branch
    // (left out, the sky goes through the main branch and the volumetric clouds follow, postfx.ixx). The car mirror's
    // pass queues the same callback (MirrorSkyDraw) and its three tests are given flags 0x20 | 0x20000 for it.
    static SafetyHookMid skyHooks[3];
    static constexpr uint32_t SkyFlags = 0x20 | 0x20000;
    static void* skyFunction = nullptr;   // CE 0xC107A0
    static bool inMirrorSky = false;      // render thread

    static void MirrorSkyFlags(SafetyHookContext& regs)
    {
        if (inMirrorSky)
            regs.eax = reinterpret_cast<uintptr_t>(&SkyFlags);
    }

    static void CaptureSkyState()
    {
        auto device = rage::grcDevice::GetD3DDevice();
        auto* vp = rage::GetCurrentViewport();
        if (!device || !vp)
            return;
        for (int i = 0; i < 3; ++i)
        {
            skyState.camera[i] = vp->mViewInverseMatrix[3][i];
            skyState.forward[i] = vp->mViewInverseMatrix[2][i];
            skyState.up[i] = vp->mViewInverseMatrix[1][i];
        }
        for (int i = 0; i < 4; ++i)
        {
            skyState.depthColumn[i] = vp->mProjectionMatrix[i][2];
            skyState.plane[i] = plane ? plane[i] : 0.0f;
        }
        skyState.nearFar[0] = vp->mNearClip;
        skyState.nearFar[1] = vp->mFarClip;
        auto real = RageDirect3DDevice9::RealDevice(device);
        real->GetRenderState(D3DRS_CLIPPLANEENABLE, &skyState.clipEnable);
        real->GetRenderState(D3DRS_CULLMODE, &skyState.cull);
        real->GetClipPlane(0, skyState.clipPlane);
        real->GetViewport(&skyState.viewport);
        skyState.known = true;
    }

    static void __cdecl MirrorSkyDraw(uint32_t)
    {
        CaptureSkyState();
        inMirrorSky = true;
        reinterpret_cast<void (__cdecl*)()>(skyFunction)();
        inMirrorSky = false;
        ++skyDraws;
    }

    // CRenderPhaseMirrorReflection: its draw list (vtable slot 8, CE 0xD76950) and its visibility (CE 0xD77430).
    // The plane again as the phase's draw list is made, with the car where its draws of it are: the update's plane is
    // from before the car moved this frame, and the reflected view then lagged the car's own side in the mirror.
    static void __fastcall PhaseBuild(uintptr_t phase, void*)
    {
        TakeFarModelsFlag();
        const double start = usedThisFrame ? NowMs() : 0.0;
        if (usedThisFrame)
        {
            const auto vehicle = drawnVehicle.load(std::memory_order_relaxed);
            if (const float* m = active && vehicle ? CEntity::GetMatrix(vehicle) : nullptr)
                PlacePlane(m, *active);
            uint32_t pass = 1;
            if (const float* m = active && active->kind == 2 && vehicle ? CEntity::GetMatrix(vehicle) : nullptr)
            {
                const uint32_t slot = ownPlaceNext++ & 3;
                std::copy_n(m + 12, 3, ownPlaces[slot]);
                pass = 2 | slot << 8;
            }
            callback.Queue(reinterpret_cast<const void*>(&MirrorPassTint), pass);
            if (skyHooks[2])
                callback.Queue(reinterpret_cast<const void*>(&MirrorSkyDraw), 0);
        }
        phaseBuildHook.unsafe_thiscall<void>(phase);
        ClearWaterFlag();
        if (usedThisFrame)
        {
            callback.Queue(reinterpret_cast<const void*>(&MirrorPassTint), 0);
            buildMs += NowMs() - start;
            LogTiming();
        }
    }

    // CRenderPhaseMirrorReflection's view (vtable slot 4, CE 0xD77600): it copies the camera's view (0x5DC220), reflects
    // it in the plane (0xA87B50) and clips by it. The plane goes in right before, by the car as it is now: from the
    // update's plane, a frame old where this runs first, the reflected view was off by twice the car's travel in a
    // frame (0.7 m at 20 m/s), the reflection lagging and jerking while driving.
    // The view it copies (the camera's grcViewport at view + 0x10) is widened for the call by Widen
    // (grcViewport::Perspective, CE 0x431470, the field of view at +0x2B8) and put back byte for byte after: the zoom
    // out reads the reflection further out than the screen it was made for, and a mirror by the screen's edge smeared
    // its last pixels. Not the phase's own views after the call: the clip there (CE 0x432470) is an oblique near plane
    // in the projection, which Perspective's recompute (0x4314C0, the projection +0x1C0, the lens +0x2A0..0x2EF)
    // would wipe. The put back keeps whatever else was in the camera's projection (a jitter).
    static void (__fastcall* perspective)(uintptr_t viewport, void*, float fov, float aspect, float zNear, float zFar) = nullptr;
    static std::atomic<float> widened{ 1.0f };

    static float Widen()
    {
        return 1.0f + (zoomOut - 1.0f) * 0.8f;
    }

    static void __fastcall PhaseView(uintptr_t phase, void*, uintptr_t view)
    {
        const bool ours = quadsThisFrame && active;
        if (ours)
        {
            mirrorPhase = phase;
            const auto vehicle = drawnVehicle.load(std::memory_order_relaxed);
            if (const float* m = vehicle ? CEntity::GetMatrix(vehicle) : nullptr)
                PlacePlane(m, *active);
        }
        float widen = ours && perspective && view ? Widen() : 1.0f;
        const auto camera = view + 0x10;
        uint8_t projection[0x40], lens[0x50];
        if (widen > 1.0f)
        {
            std::memcpy(projection, reinterpret_cast<const void*>(camera + 0x1C0), sizeof(projection));
            std::memcpy(lens, reinterpret_cast<const void*>(camera + 0x2A0), sizeof(lens));
            const auto* fov = reinterpret_cast<const float*>(camera + 0x2B8); // fov, aspect, near, far
            const bool degrees = fov[0] > 3.2f;
            const float half = (degrees ? fov[0] * 0.01745329f : fov[0]) * 0.5f;
            if (half > 0.0f && half < 1.5f)
            {
                const float wider = 2.0f * std::atan(std::tan(half) * widen);
                perspective(camera, nullptr, degrees ? wider * 57.29578f : wider, fov[1], fov[2], fov[3]);
            }
            else
                widen = 1.0f;
        }
        phaseViewHook.unsafe_thiscall<void>(phase, view);
        if (widen > 1.0f)
        {
            std::memcpy(reinterpret_cast<void*>(camera + 0x1C0), projection, sizeof(projection));
            std::memcpy(reinterpret_cast<void*>(camera + 0x2A0), lens, sizeof(lens));
        }
        widened.store(widen, std::memory_order_relaxed);
    }

    // CRenderPhaseWaterReflection's visibility (vtable slot 3, CE 0xD77550): the same scan from the phase's own
    // viewport, with no interior gate.
    static void (__fastcall* waterVisibility)(uintptr_t phase, void*) = nullptr;
    // It also skips while bit 0 of +0x22 of the object at this pointer is set (CE 0x16DD67C, read in it right after the
    // mirror's gate): in places the game keeps without a water reflection (one 20 x 200 m box along a street, the
    // car under an elevated line), and the car's mirror, through this scan, went white there and stayed so (its
    // visibility 0.001 ms, 0.03 ms with the bit cleared by hand). Cleared for the mirror's scan and put back.
    static uintptr_t* waterReflectionZone = nullptr;

    // The water reflection's visibility (CE 0xD77550) and draw list (CE 0xD76C10) are skipped while a mirror is on
    // (0x166DA20 == 1): a bathroom mirror has no water to see. A car's mirror is on all the time in first person, and
    // the water by the road lost its reflection (and the mirrors the water). Each gate's je, right after the compare,
    // is not taken while the mirror on is the car's: a flag in the compare's result, not the mirror flag itself,
    // which the render thread reads as it draws the mirror's quad. CarMirrorsWaterReflection 0 keeps the game's rule
    // for the water reflection's own phase; the car mirror's scan (the water's, see PhaseVisibility) still goes through.
    static bool inMirrorScan = false;

    static void WaterVisibilityGate(SafetyHookContext& regs)
    {
        if (quadsThisFrame && (waterReflection || inMirrorScan))
            regs.eflags &= ~0x40u; // ZF
    }

    static void WaterBuildGate(SafetyHookContext& regs)
    {
        if (quadsThisFrame && waterReflection)
            regs.eflags &= ~0x40u; // ZF
    }

    // The mirror's scan runs only from an interior's room: it skips unless the phase's scene start (+0x940) has an
    // interior (+0x30) and a room above 0 (+0x38), as a bathroom mirror's has. Outdoors the car mirror's phase then
    // had nothing to draw (0.000 ms), so there it takes the water's scan, which starts from the phase's viewport like
    // the main camera (its mirror gate let through by WaterVisibilityGate). Its water is gathered after every phase's
    // scan (GatherMirrorWater).
    static uintptr_t waterScanPhase = 0;      // the phase the outdoor scan ran for this frame, for GatherMirrorWater
    // The map's lamps register their lights and coronas as an entity is readied for the frame (CEntity vtable +0x84,
    // CE 0xA32640 -> 0xC1DB60), skipped beyond 35 m of the camera unless the entity is seen by a phase in the lights'
    // mask ([0x1FBB75C], CE 0xC1DBA7). The scan puts a phase in that mask by its flag 0x2000000 (+0x8E8, read in
    // CE 0xAE4880 under 0xB1E810), which the mirror phase has not: no lamp behind the camera had a corona for the
    // mirror (14-27 within 60 m of it registered, 1-10 of them out of the game camera's view). Given for its scan.
    static constexpr uint32_t PhaseLightsFlag = 0x2000000;

    static void __fastcall PhaseVisibility(uintptr_t phase, void*)
    {
        ClearWaterFlag();
        const double start = usedThisFrame ? NowMs() : 0.0;
        const auto scene = *reinterpret_cast<const uintptr_t*>(phase + 0x940);
        const bool inRoom = scene && *reinterpret_cast<const uintptr_t*>(scene + 0x30) &&
                            *reinterpret_cast<const int32_t*>(scene + 0x38) > 0;
        if (usedThisFrame && !inRoom && waterVisibility)
        {
            const bool gated = !waterGateHooks[0];
            if (gated) *mirrorOn = 0;
            auto& flags = *reinterpret_cast<uint32_t*>(phase + 0x8E8);
            const uint32_t before = flags;
            if (lampCoronas)
                flags |= PhaseLightsFlag;
            if (farModels && !(flags & PhaseFarModelsFlag))
            {
                flags |= PhaseFarModelsFlag;
                farModelsPhase = phase;
            }
            uint8_t* zoneFlags = waterReflectionZone && *waterReflectionZone ? reinterpret_cast<uint8_t*>(*waterReflectionZone + 0x22) : nullptr;
            const uint8_t zoneBefore = zoneFlags ? *zoneFlags : 0;
            if (zoneFlags)
                *zoneFlags = zoneBefore & ~1u;
            inMirrorScan = true;
            waterVisibility(phase, nullptr);
            inMirrorScan = false;
            if (zoneFlags)
                *zoneFlags = zoneBefore;
            flags = (flags & ~PhaseLightsFlag) | (before & PhaseLightsFlag);
            if (gated) *mirrorOn = 1;
            ++outdoorScans;
            waterScanPhase = phase;
        }
        else
            phaseVisibilityHook.unsafe_thiscall<void>(phase);
        if (usedThisFrame)
            visibilityMs += NowMs() - start;
    }

    // The water seen by a phase (CE 0xAD7D80, cdecl(the phase's list), the phase in [0x12FB1B8]) goes into one buffer
    // of the frame (CE 0xAD61A0, main thread; the render thread's other one CE 0xAD5CD0, 0x10C4 bytes each): quads
    // (+0, 1000 of them, count +0xE60), triangles (+0x7D0, 40, +0xE64) and far cells (+0x820 / +0xB40, 400, +0xE68), a
    // list's start and count of each at +0xE6C / +0xF8C, +0xECC / +0xFEC, +0xF2C / +0x104C (+ 4 x list, 24 lists).
    // Only the far cells' 400 are checked; the game gathers the main scene's alone. The mirror's, gathered among the
    // scans, could run the buffers over for whichever came after it (the 40 triangles into the far cells, the main
    // scene's water then left out where they were overwritten, the frame before showing through), so it is gathered
    // last, as every phase's scan (CE 0x8F7400) is done (the water reflection's after the others), and put back from
    // a copy if it ran over.
    static uintptr_t (__cdecl* waterBuffer)() = nullptr;        // CE 0xAD61A0
    static uintptr_t (__cdecl* waterRenderBuffer)() = nullptr;  // CE 0xAD5CD0
    static void (__cdecl* gatherWater)(uint32_t list) = nullptr; // CE 0xAD7D80
    static uintptr_t* scanningPhase = nullptr;                 // CE 0x12FB1B8
    static const uintptr_t* gameState = nullptr;               // CE 0x16DD67C, +0x22 & 1: no water gathered
    static const uint8_t* noWater = nullptr;                   // CE 0x12BD0F2
    static constexpr uint32_t WaterBufferSize = 0x10C4, WaterLists = 24;
    static constexpr uint32_t WaterCountOffsets[3] = { 0xF8C, 0xFEC, 0x104C };
    static constexpr uint32_t WaterStartOffsets[3] = { 0xE6C, 0xECC, 0xF2C };
    static constexpr uint32_t WaterCapacity[3] = { 1000, 40, 400 };
    static uint8_t waterBufferCopy[WaterBufferSize];

    static uint32_t& WaterField(uintptr_t buffer, uint32_t offset)
    {
        return *reinterpret_cast<uint32_t*>(buffer + offset);
    }

    static void GatherMirrorWater(SafetyHookContext& regs)
    {
        const uintptr_t phase = waterScanPhase;
        waterScanPhase = 0;
        if (!phase || !usedThisFrame || !water || !waterFunction || !phaseWaterHook || !gatherWater || !waterBuffer || !scanningPhase)
            return;
        if (*noWater || (*gameState && (*reinterpret_cast<const uint8_t*>(*gameState + 0x22) & 1)))
            return; // as the scan's own gather
        const uint32_t list = *reinterpret_cast<const uint32_t*>(phase + 0x938);
        if (list >= WaterLists)
            return;
        const uintptr_t buffer = waterBuffer();
        if (const uintptr_t main = regs.ebx) // the main scene's phase (its kind 0x1F), as the loop found it
        {
            const uint32_t mainList = *reinterpret_cast<const uint32_t*>(main + 0x938);
            if (mainList < WaterLists)
                for (int i = 0; i < 3; ++i)
                    mainWaterItems[i] += WaterField(buffer, WaterCountOffsets[i] + 4 * mainList);
        }
        std::memcpy(waterBufferCopy, reinterpret_cast<const void*>(buffer), WaterBufferSize);
        const auto flags = *reinterpret_cast<uint32_t*>(phase + 0x8E8);
        *reinterpret_cast<uint32_t*>(phase + 0x8E8) = flags | PhaseWaterFlag;
        *scanningPhase = phase;
        gatherWater(list);
        *scanningPhase = 0;
        *reinterpret_cast<uint32_t*>(phase + 0x8E8) = flags;
        bool over = false;
        for (int i = 0; i < 3; ++i)
        {
            const uint32_t used = WaterField(buffer, 0xE60 + 4 * i);
            waterFill[i] = (std::max)(waterFill[i], used);
            over |= used > WaterCapacity[i];
        }
        if (over)
        {
            std::memcpy(reinterpret_cast<void*>(buffer), waterBufferCopy, WaterBufferSize);
            for (int i = 0; i < 3; ++i)
                WaterField(buffer, WaterCountOffsets[i] + 4 * list) = WaterField(buffer, WaterStartOffsets[i] + 4 * list) = 0;
            ++waterRolledBack;
            return;
        }
        for (int i = 0; i < 3; ++i)
            mirrorWaterItems[i] += WaterField(buffer, WaterCountOffsets[i] + 4 * list);
        ++waterGathered;
        SetWaterFlag(phase);
    }

    // Render thread, the water in the mirror's pass. In some of the game's modes ([0x1045538] 1 or 5) its draw sets
    // the material ID target (_STENCIL_BUFFER_, the main view's, screen sized) as the second target and writes the
    // water's ID there: from the mirror's view, into the main frame's IDs. The two compares of that mode (CE 0xAD7F11,
    // 0xAD88AA, each cmp eax, 1 / je) are made to fail for it.
    static bool inMirrorWater = false;

    static void WaterTargetMode(SafetyHookContext& regs)
    {
        if (inMirrorWater)
            regs.eax = 0;
    }

    // The water's draw goes on only with the draw list's flag 0x10 (test byte [eax], 10 at CE 0xAD7F03, eax the
    // flags the draw list took as it was made): set on the phase from the gather to its draw list's end, it is given
    // here too, in case the copy was taken without it. waterFlagMissing counts the draws it was not in the copy.
    static SafetyHookMid waterFlagHook;

    // The water's look comes from how deep it is: its pixel shader (water.fxc, waterPS1) takes its alpha from the scene's
    // depth (DepthBufferSampler, s2) behind it. That is the main view's depth, read at the mirror's pixels, mostly
    // near: the water came out nearly clear over the mirror's black, cleared target. water_mirror_opaque.patch keeps
    // it opaque with c204.z 1 (MirrorPassTint). The draw picks its second pass (waterPS2, with the sun's shadow from
    // cascades fitted to the main view) when [0x103F4D0] is 1: in the mirror the first, its jne at CE 0xAD8576
    // taken (push 0 for the pass, not push 1).
    static SafetyHookMid waterPassHook;

    static void MirrorWaterPass(SafetyHookContext& regs)
    {
        if (inMirrorWater)
            regs.eflags &= ~0x40u; // ZF
    }


    static void MirrorWaterFlag(SafetyHookContext& regs)
    {
        static constexpr uint32_t flags = PhaseWaterFlag;
        if (!inMirrorWater)
            return;
        if (!(*reinterpret_cast<const uint8_t*>(regs.eax) & PhaseWaterFlag))
            ++waterFlagMissing;
        regs.eax = reinterpret_cast<uintptr_t>(&flags);
    }

    static void __cdecl MirrorWater(uint32_t list)
    {
        if (waterRenderBuffer && list < WaterLists)
        {
            const uintptr_t buffer = waterRenderBuffer();
            for (int i = 0; i < 3; ++i)
                drawnWaterItems[i] += WaterField(buffer, WaterCountOffsets[i] + 4 * list);
        }
        ++waterDrawn;
        inMirrorWater = true;
        reinterpret_cast<void (__cdecl*)(uint32_t)>(waterFunction)(list);
        inMirrorWater = false;
    }

    // The mirror phase draws its entities (CE 0xB1DEE0 at 0xD76B9A) with the shaders' main techniques, which outdoors
    // read the sun's shadow cascades fitted to the main camera: shadows in the mirror that moved and changed with the
    // camera's turn. The water reflection draws its own in the "reflection" technique group (CE 0xD76E31: a push
    // command with the group in [0x1797690], and a pop command after, vtable CE 0xEA76C4); the car mirror's the same.
    struct TechniqueGroup
    {
        const uint32_t* group = nullptr;
        void (__fastcall* pushCommand)(void* command, void*, uint32_t group) = nullptr; // CE 0x8DC290
        uint32_t popVtable = 0;
    };
    static TechniqueGroup techniqueGroup;
    static SafetyHookMid techniqueHooks[2];
    static bool techniquePushed = false;

    static void PushReflectionTechniques(SafetyHookContext& regs)
    {
        techniquePushed = false;
        if (!usedThisFrame || !reflectionShaders || !callback.allocate || !callback.add || !*techniqueGroup.group)
            return;
        if (auto command = callback.allocate(0xC, 0))
        {
            techniqueGroup.pushCommand(command, nullptr, *techniqueGroup.group);
            callback.add(command);
            techniquePushed = true;
            ++techniqueDraws;
        }
    }

    // The entities' render buckets (the draw's second argument, 0x307 at 0xD76B8F) leave out bit 8, bucket 4, which
    // the water reflection's 0x90D has: the trees' leaves, the car mirror showed bare branches. Given to the car's.
    static SafetyHookMid bucketHook;

    static void MirrorBuckets(SafetyHookContext& regs)
    {
        if (!usedThisFrame)
            return;
        *reinterpret_cast<uint32_t*>(regs.esp) |= 8; // the buckets
    }

    // The coronas (CE 0xC106F0: CE 0xC85080 the coronas, CE 0xD8C8E0 the lens flares, clip planes off round them) are
    // drawn for draw lists with flag 8, which the mirror phase has not; it queues them before its entities (0xD76AEC),
    // which drawn after would cover them (they write no depth). The car mirror's pass queues them again after what is
    // drawn with blending (MirrorCoronasDraw, at 0xD76BBF), its flag test (0xC106F5) given 8 for it and the lens
    // flares' gate (the je at 0xD8C8E7) taken: a flare across the screen made no sense in a small mirror.
    static SafetyHookMid coronaHooks[3];
    static constexpr uint32_t CoronaFlags = 8;
    static void* coronaFunction = nullptr;  // CE 0xC106F0
    // The lamps' coronas themselves (CCoronas::Render, CE 0xC1F170, coronas.ixx's in its place): what is drawn with
    // blending (CE 0xC0FA50, called at 0xD76BB7) queues them only for draw lists with flag 0x20 (0xC0FA61), which the
    // mirror's has not; 0xC106F0 draws other lights. Drawn after it: none had been in a car's mirror, 0 listed.
    static void* coronaSprites = nullptr;
    static bool inMirrorCoronas = false;    // render thread

    static void MirrorCoronaFlags(SafetyHookContext& regs)
    {
        if (inMirrorCoronas)
            regs.eax = reinterpret_cast<uintptr_t>(&CoronaFlags);
    }

    static void MirrorNoFlares(SafetyHookContext& regs)
    {
        if (inMirrorCoronas)
            regs.eflags |= 0x40u; // ZF: the je over the flares taken
    }

    // The coronas' vertex shader (deferred_lighting VS11) hides a corona whose depth is behind the depth texture's
    // (vs s0) round its spot on screen: the main view's, which at the mirror's pixels is the car's inside, every
    // corona hidden. A 1 x 1 texture far away takes its place for the mirror's; the mirror's own depth buffer still
    // hides them behind what it drew.
    static IDirect3DTexture9* farDepth = nullptr;

    static void BindFarDepth()
    {
        auto device = rage::grcDevice::GetD3DDevice();
        if (!device)
            return;
        if (!farDepth)
        {
            if (FAILED(RageDirect3DDevice9::RealDevice(device)->CreateTexture(1, 1, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &farDepth, nullptr)) || !farDepth)
            {
                farDepth = nullptr;
                return;
            }
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(farDepth->LockRect(0, &locked, nullptr, 0)))
            {
                *static_cast<float*>(locked.pBits) = 1e6f;
                farDepth->UnlockRect(0);
            }
        }
        RageDirect3DDevice9::SetTextureBoth(device, D3DVERTEXTEXTURESAMPLER0, farDepth);
    }

    // The coronas are added to what is drawn (the corona technique's blend: one and one). In the mirror's pass they
    // came out opaque, their quads' black corners over the reflection: the city's far lights made a black band with
    // dots along the horizon. The blend is set on the device right before each batch and put back after the coronas.
    // The depth test of the technique's second pass went the same way (headlights through the car's inside and through
    // other cars in the mirror): on, less or equal, no depth written, likewise.
    static constexpr D3DRENDERSTATETYPE CoronaStates[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
                                                           D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_ZWRITEENABLE };
    static constexpr DWORD CoronaValues[] = { TRUE, D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD, D3DZB_TRUE, D3DCMP_LESSEQUAL, FALSE };
    static DWORD coronaSavedStates[std::size(CoronaStates)]{};
    static bool coronaStatesSaved = false;

    static void MirrorCoronaBatch()
    {
        BindFarDepth();
        auto device = rage::grcDevice::GetD3DDevice();
        if (!device)
            return;
        auto real = RageDirect3DDevice9::RealDevice(device);
        if (!coronaStatesSaved)
        {
            for (size_t i = 0; i < std::size(CoronaStates); ++i)
                real->GetRenderState(CoronaStates[i], &coronaSavedStates[i]);
            coronaStatesSaved = true;
        }
        for (size_t i = 0; i < std::size(CoronaStates); ++i)
            real->SetRenderState(CoronaStates[i], CoronaValues[i]);
    }

    static void RestoreCoronaStates()
    {
        if (!coronaStatesSaved)
            return;
        coronaStatesSaved = false;
        if (auto device = rage::grcDevice::GetD3DDevice())
        {
            auto real = RageDirect3DDevice9::RealDevice(device);
            for (size_t i = 0; i < std::size(CoronaStates); ++i)
                real->SetRenderState(CoronaStates[i], coronaSavedStates[i]);
        }
    }

    static void __cdecl MirrorCoronasDraw(uint32_t)
    {
        inMirrorCoronas = true;
        CoronaRender::pBeforeDraw = &MirrorCoronaBatch;
        CoronaRender::bOtherView = true;
        reinterpret_cast<void (__cdecl*)()>(coronaFunction)();
        if (coronaSprites)
            reinterpret_cast<void (__cdecl*)()>(coronaSprites)();
        CoronaRender::bOtherView = false;
        CoronaRender::pBeforeDraw = nullptr;
        RestoreCoronaStates();
        inMirrorCoronas = false;
        ++coronaDraws;
    }

    // The coronas the mirror sees. The game lists a corona for drawing (CE 0xC1EE40, main thread, once a frame) only if
    // its ball is in the game camera's view (0x431E40 on the camera's grcViewport), a headlight's brightness times
    // the cosine between its beam and the camera's axis: what a car's mirror sees, behind the camera, was never listed
    // (15800 listed at night, ~12 behind the camera), and cars behind had their headlights pointed away. Right as the
    // list is closed, every corona registered this frame is put through the same test and brightness with the mirror
    // phase's own view (phase + 0xB0, reflected, clipped and widened by PhaseView), those it sees written after the
    // game camera's in the same buffer; the count stays the camera's, and the mirror's pass draws only its own part
    // (CoronaRender::pOtherRange).
    struct CoronaCopy
    {
        int32_t* registered = nullptr;  // the coronas registered this frame
        uint8_t* source = nullptr;      // their list, 0x40 bytes each
        float* sizeScale = nullptr;
        float* ballExtra = nullptr;     // added to the ball's radius unless the corona's flag 0x20
        int (__fastcall* visible)(uintptr_t viewport, void*, float x, float y, float z, float radius, int) = nullptr;
        int32_t cap = 0;
        uint8_t* shift = nullptr;       // the list's size exponent (3 << shift a buffer)
        uint8_t* list = nullptr;        // the drawing list, 0x20 bytes each
    };
    static CoronaCopy coronaCopy;
    static SafetyHookMid coronaCopyHook;
    static int32_t mirrorCoronaFirst[4]{}, mirrorCoronaCount[4]{};

    // mov [counts + eax * 4], ecx: eax the buffer, ecx the game camera's count
    static void MirrorCoronaList(SafetyHookContext& regs)
    {
        const uint32_t buffer = regs.eax & 3;
        const int32_t count = int32_t(regs.ecx);
        mirrorCoronaFirst[buffer] = count;
        mirrorCoronaCount[buffer] = 0;
        if (!usedThisFrame || !mirrorPhase || !plane || !coronaSprites)
            return;
        const auto& c = coronaCopy;
        const uintptr_t viewport = mirrorPhase + 0xB0;
        const float* axis = reinterpret_cast<const float*>(viewport + 0x60);
        uint8_t* out = c.list + size_t(regs.eax) * (3u << *c.shift) * 0x20;
        const int32_t registered = *c.registered;
        int32_t n = count;
        for (int32_t i = 0; i < registered && n < c.cap; ++i)
        {
            const uint8_t* item = c.source + size_t(i) * 0x40;
            if (!*reinterpret_cast<const uint32_t*>(item + 0x20))
                continue;
            const float* pos = reinterpret_cast<const float*>(item);
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(item + 0x3C);
            const float size = *reinterpret_cast<const float*>(item + 0x24) * *c.sizeScale;
            const float radius = (flags & 0x20) ? size : size + *c.ballExtra;
            // behind the mirror's plane: clipped away
            if (plane[0] * pos[0] + plane[1] * pos[1] + plane[2] * pos[2] - plane[3] < -radius)
                continue;
            ++mirrorCoronaTested;
            const float* eye = reinterpret_cast<const float*>(viewport + 0x70);
            const float dx = pos[0] - eye[0], dy = pos[1] - eye[1], dz = pos[2] - eye[2];
            const bool nearby = dx * dx + dy * dy + dz * dz < NearCorona * NearCorona;
            const int kind = (flags & 0x10) ? 0 : 1;
            if (nearby)
            {
                ++nearRegistered[kind];
                if (flags & 2)
                    ++nearOutOfView[kind];
            }
            if (!c.visible(viewport, nullptr, pos[0], pos[1], pos[2], radius, 0))
                continue;
            if (nearby)
                ++nearListed[kind];
            float brightness = *reinterpret_cast<const float*>(item + 0x34) * *reinterpret_cast<const float*>(item + 0x30);
            if (flags & 0x10)
            {
                const float* beam = reinterpret_cast<const float*>(item + 0x10);
                brightness *= beam[0] * axis[0] + beam[1] * axis[1] + beam[2] * axis[2];
            }
            if (!(brightness > 0.0f))
                continue;
            uint8_t* to = out + size_t(n) * 0x20;
            auto* f = reinterpret_cast<float*>(to);
            f[0] = pos[0];
            f[1] = pos[1];
            f[2] = pos[2];
            f[3] = size;
            f[4] = brightness;
            *reinterpret_cast<uint32_t*>(to + 0x14) = *reinterpret_cast<const uint32_t*>(item + 0x28);
            to[0x18] = item[0x38];
            to[0x19] = item[0x39];
            to[0x1A] = item[0x3A];
            to[0x1B] = uint8_t((flags >> 5) & 1);
            ++n;
        }
        mirrorCoronaCount[buffer] = n - count;
        mirrorCoronaListed += uint32_t(n - count);
        ++mirrorCoronaFrames;
    }

    // Render thread, the mirror's pass (CoronaRender::bOtherView)
    static bool MirrorCoronaRange(uint32_t buffer, int32_t& first, int32_t& count)
    {
        first = mirrorCoronaFirst[buffer & 3];
        count = mirrorCoronaCount[buffer & 3];
        return count > 0;
    }

    // Main thread, in the mirror phase's draw list right after what is drawn with blending
    static void QueueMirrorCoronas(SafetyHookContext&)
    {
        if (usedThisFrame && coronaHooks[2])
            callback.Queue(reinterpret_cast<const void*>(&MirrorCoronasDraw), 0);
    }

    // The main scene's G-buffer (mode 4, CE 0xB1E0A8) also draws bucket 5 and bucket 4 in a fourth way (0xAE2A90's
    // third argument 4: (5, 4) always, (4, 4) with bit 8), which the mirror's mode (CE 0xB1E1CB) has not: still no
    // leaves with bit 8 given, in either technique group. Queued after its entities, as the mirror's own.
    static void (__cdecl* drawBucket)(uint32_t list, uint32_t bucket, uint32_t way, uint32_t mode, uint32_t) = nullptr; // CE 0xAE2A90
    static void ExtraBuckets(uintptr_t phase)
    {
        if (!usedThisFrame || !drawBucket || !extraBuckets)
            return;
        const uint32_t list = *reinterpret_cast<const uint32_t*>(phase + 0x938);
        drawBucket(list, 5, 4, 0, 0);
        drawBucket(list, 4, 4, 0, 0);
        ++extraBucketDraws;
    }

    static void PopReflectionTechniques(SafetyHookContext& regs);

    static void AfterMirrorEntities(SafetyHookContext& regs)
    {
        ExtraBuckets(regs.edi);
        PopReflectionTechniques(regs);
    }

    static void PopReflectionTechniques(SafetyHookContext&)
    {
        if (!techniquePushed)
            return;
        techniquePushed = false;
        auto command = static_cast<uint32_t*>(callback.allocate(0x8, 0));
        if (command)
        {
            command[0] = callback.baseVtable;
            command[1] ^= (command[1] ^ *callback.counter) & 0x3FFF;
            ++*callback.counter;
            command[0] = techniqueGroup.popVtable;
        }
        callback.add(command);
    }

    // In the mirror phase's draw list, after its entities and before what is drawn with blending (CE 0xC0FA50): the
    // water, as the main scene queues it after its own (the stencil states it sets round it there left out: the
    // mirror's pass reads no stencil after).
    static void QueueMirrorWater(uintptr_t phase)
    {
        if (!waterFlagPhase || phase != waterFlagPhase)
            return;
        const uint32_t list = *reinterpret_cast<const uint32_t*>(phase + 0x938);
        callback.Queue(reinterpret_cast<const void*>(&MirrorWater), list);
        ++waterQueued;
    }

    static void PhaseWater(SafetyHookContext& regs)
    {
        QueueMirrorWater(regs.edi);
    }

    // c152 for mirror.fx (mirror_car_shape.patch): x 1 cuts the quad to the glass's outline, y the zoom out - 1 widens
    // the view as a convex mirror's, z how dark (1 dark glass, not the reflection), w 1 / Widen - 1 (the reflection made
    // wider than the screen, read nearer its middle); zeros keep an interior mirror as the
    // game draws it. c153-c160: the glass's outline's edges, a corner xy and the way to the next zw, counter-clockwise
    // over the quad's -1..1 (u right, v up as the driver sees it; CarMirrorsReverse turns u round); unused edges zero.
    static void SetShape(const CarMirrorGlass* glass, float darkness)
    {
        auto device = rage::grcDevice::GetD3DDevice();
        if (!device)
            return;
        float shape[9][4]{};
        const int n = glass ? glass->corners : 0;
        shape[0][0] = glass && rounded && n >= 3 ? 1.0f : 0.0f;
        shape[0][1] = glass ? zoomOut - 1.0f : 0.0f;
        shape[0][2] = glass ? std::clamp(darkness, 0.0f, 1.0f) : 0.0f;
        shape[0][3] = glass ? 1.0f / widened.load(std::memory_order_relaxed) - 1.0f : 0.0f;
        if (shape[0][0] != 0.0f)
        {
            float outline[8][2];
            for (int k = 0; k < n; ++k)
            {
                // turned round, the order goes back too, so the corners stay counter-clockwise
                const auto& corner = glass->outline[reverse ? n - 1 - k : k];
                outline[k][0] = reverse ? -corner[0] : corner[0];
                outline[k][1] = corner[1];
            }
            for (int k = 0; k < n; ++k)
            {
                const auto& a = outline[k];
                const auto& b = outline[(k + 1) % n];
                shape[1 + k][0] = a[0];
                shape[1 + k][1] = a[1];
                shape[1 + k][2] = b[0] - a[0];
                shape[1 + k][3] = b[1] - a[1];
            }
        }
        device->SetPixelShaderConstantF(152, &shape[0][0], 9);
        if (auto real = RageDirect3DDevice9::RealDevice(device); real != device)
            real->SetPixelShaderConstantF(152, &shape[0][0], 9);
    }

    static uint32_t Snap(const float* m, const CarMirrorGlass& glass, float darkness)
    {
        const uint32_t number = nextSnapshot++ | 0x80000000u; // never 0, never a small visibility id
        auto& snapshot = snapshots[number & 31];
        snapshot.number.store(0, std::memory_order_relaxed);
        GlassCorners(m, glass, snapshot.corners);
        snapshot.glass = &glass;
        snapshot.darkness = darkness;
        snapshot.number.store(number, std::memory_order_release);
        return number;
    }

    // Main thread, the quad's command just made (edx), its parameter about to be stored from eax: the corners by the car
    // as it is now, and their number in its place; the car's other mirrors queued after it, dark.
    static void QueueQuad(SafetyHookContext& regs)
    {
        const auto vehicle = drawnVehicle.load(std::memory_order_relaxed);
        const float* m = vehicle ? CEntity::GetMatrix(vehicle) : nullptr;
        if (!quadsThisFrame || !m || !mirrorOn || !*mirrorOn || !byDrawnCar || !carGlassCount)
            return;
        const CarMirrorGlass* primary = active ? active : carGlasses[0];
        regs.eax = Snap(m, *primary, active ? activeDarkness : 1.0f);
        for (int g = 0; g < carGlassCount; ++g)
            if (carGlasses[g] != primary)
                callback.Queue(quadFunction, Snap(m, *carGlasses[g], 1.0f));
    }

    // Render thread, a mirror quad about to be drawn (its parameter at esp+4): ours by the snapshot, else the game's.
    static void PlaceQuad(SafetyHookContext& regs)
    {
        const uint32_t number = *reinterpret_cast<const uint32_t*>(regs.esp + 4);
        if (number & 0x80000000u)
        {
            const auto& snapshot = snapshots[number & 31];
            if (snapshot.number.load(std::memory_order_acquire) == number && snapshot.glass)
            {
                Compensate(snapshot.corners);
                SetShape(snapshot.glass, snapshot.darkness);
                ++quadDrawn;
                ++quadBySnapshot;
                if (snapshot.darkness >= 1.0f)
                    ++quadsDark;
                return;
            }
        }
        SetShape(nullptr, 0.0f);
    }
}

class CarMirrorsHooks
{
public:
    CarMirrorsHooks()
    {
        EntityClass::AddDrawHooks(CarMirrors::MirrorDrawBegin, CarMirrors::MirrorDrawEnd);

        FusionFix::onInitEventAsync() += []()
        {
            static auto readIni = [](CIniReader& ini)
            {
                CarMirrors::reverse = ini.ReadInteger("MISC", "CarMirrorsReverse", 0) != 0;
                CarMirrors::rounded = ini.ReadInteger("MISC", "CarMirrorsRounded", 1) != 0;
                CarMirrors::byDrawnCar = ini.ReadInteger("MISC", "CarMirrorsByDrawnCar", 1) != 0;
                CarMirrors::zoomOut = std::clamp(ini.ReadFloat("MISC", "CarMirrorsZoomOut", 1.5f), 1.0f, 2.5f);
                CarMirrors::water = ini.ReadInteger("MISC", "CarMirrorsWater", 1) != 0;
                CarMirrors::farModels = ini.ReadInteger("MISC", "CarMirrorsFarModels", 1) != 0;
                CarMirrors::waterReflection = ini.ReadInteger("MISC", "CarMirrorsWaterReflection", 1) != 0;
                CarMirrors::reflectionShaders = ini.ReadInteger("MISC", "CarMirrorsReflectionShaders", 1) != 0;
                CarMirrors::extraBuckets = ini.ReadInteger("MISC", "CarMirrorsExtraBuckets", 1) != 0;
                CarMirrors::lampCoronas = ini.ReadInteger("MISC", "CarMirrorsLampCoronas", 1) != 0;
                CarMirrors::isolateStates = ini.ReadInteger("MISC", "CarMirrorsEntityStates", 1) != 0;
                CarMirrors::logStats = ini.ReadInteger("MISC", "CarMirrorsLog", 0) != 0;
                CarMirrors::sideAimX = std::clamp(ini.ReadFloat("MISC", "CarMirrorsSideAimX", 0.0f), -30.0f, 30.0f);
                CarMirrors::sideAimY = std::clamp(ini.ReadFloat("MISC", "CarMirrorsSideAimY", -1.5f), -30.0f, 30.0f);
                CarMirrors::interiorAimX = std::clamp(ini.ReadFloat("MISC", "CarMirrorsInteriorAimX", 0.0f), -30.0f, 30.0f);
                CarMirrors::interiorAimY = std::clamp(ini.ReadFloat("MISC", "CarMirrorsInteriorAimY", -3.0f), -30.0f, 30.0f);
                FusionLog::Write("CarMirrors", "Init", "reverse %d, rounded %d, by the drawn car %d, zoom out %.2f, far models %d, water %d, water reflection %d, reflection shaders %d, extra buckets %d, lamp coronas %d, entities' states kept apart %d, log %d; aim x/y: side %.1f/%.1f, interior %.1f/%.1f\n",
                    int(CarMirrors::reverse), int(CarMirrors::rounded), int(CarMirrors::byDrawnCar), CarMirrors::zoomOut, int(CarMirrors::farModels), int(CarMirrors::water),
                    int(CarMirrors::waterReflection), int(CarMirrors::reflectionShaders), int(CarMirrors::extraBuckets),
                    int(CarMirrors::lampCoronas), int(CarMirrors::isolateStates), int(CarMirrors::logStats), CarMirrors::sideAimX, CarMirrors::sideAimY, CarMirrors::interiorAimX, CarMirrors::interiorAimY);
            };
            CIniReader iniReader("");
            readIni(iniReader);
            IniReload::Add(readIni);

            // push ebx / push 166DA22 / push corners / push plane / call 0xA0F820 / xor bl, bl / add esp, C /
            // mov [mirror on], al / mov [camera behind], bl
            auto update = hook::pattern("53 68 ? ? ? ? 68 ? ? ? ? 68 ? ? ? ? E8 ? ? ? ? 32 DB 83 C4 0C A2 ? ? ? ? 88 1D");
            if (update.empty())
            {
                FusionLog::Write("CarMirrors", "Init", "the mirror update was not found, car mirrors off\n");
                return;
            }
            CarMirrors::corners = *update.get_first<float*>(7);
            CarMirrors::plane = *update.get_first<float*>(12);
            CarMirrors::mirrorOn = *update.get_first<uint8_t*>(27);
            CarMirrors::cameraBehind = *update.get_first<uint8_t*>(33);
            CarMirrors::updateHook = safetyhook::create_inline(update.get_first(0), CarMirrors::Update);

            // push ebp / mov ebp, esp / and esp, -10 / sub esp, 18 / cmp byte [mirror on], 0 / push esi / push edi / mov edi, ecx / je
            auto build = hook::pattern("55 8B EC 83 E4 F0 83 EC 18 80 3D ? ? ? ? 00 56 57 8B F9 0F 84");
            // push ecx / cmp byte [mirror on], 0 / push esi / mov esi, ecx / je / cmp byte [camera behind], 0 / jne
            auto visibility = hook::pattern("51 80 3D ? ? ? ? 00 56 8B F1 0F 84 ? ? ? ? 80 3D ? ? ? ? 00 0F 85");
            // only where they test the same flag
            const bool timeBuild = !build.empty() && *build.get_first<uint8_t*>(11) == CarMirrors::mirrorOn;
            const bool timeVisibility = !visibility.empty() && *visibility.get_first<uint8_t*>(3) == CarMirrors::mirrorOn;
            if (timeBuild)
                CarMirrors::phaseBuildHook = safetyhook::create_inline(build.get_first(0), CarMirrors::PhaseBuild);
            if (timeVisibility)
                CarMirrors::phaseVisibilityHook = safetyhook::create_inline(visibility.get_first(0), CarMirrors::PhaseVisibility);
            // push ebp / mov ebp, esp / and esp, -10 / sub esp, 88 / push esi / mov esi, [ebp+8] / push edi / mov edi, ecx /
            // add esi, 10 / lea ecx, [edi+B0] / push esi / call (the view's copy)
            auto view = hook::pattern("55 8B EC 83 E4 F0 81 EC 88 00 00 00 56 8B 75 08 57 8B F9 83 C6 10 8D 8F B0 00 00 00 56 E8");
            if (!view.empty())
                CarMirrors::phaseViewHook = safetyhook::create_inline(view.get_first(0), CarMirrors::PhaseView);
            // grcViewport::Perspective: push ecx / movss xmm0, [esp+8] / movss [ecx+2B8], xmm0 / movss xmm0, [esp+C] / movss [ecx+2BC], xmm0
            auto lens = hook::pattern("51 F3 0F 10 44 24 08 F3 0F 11 81 B8 02 00 00 F3 0F 10 44 24 0C F3 0F 11 81 BC 02 00 00");
            if (!lens.empty())
                CarMirrors::perspective = lens.get_first<void(__fastcall)(uintptr_t, void*, float, float, float, float)>(0);
            // push ebp / mov ebp, esp / and esp, -10 / sub esp, 20 / call / test byte [eax], 40 / je / cmp byte [mirror on], 0 / je
            auto quad = hook::pattern("55 8B EC 83 E4 F0 83 EC 20 E8 ? ? ? ? F6 00 40 0F 84 ? ? ? ? 80 3D ? ? ? ? 00 0F 84");
            if (!quad.empty() && *quad.get_first<uint8_t*>(25) == CarMirrors::mirrorOn)
            {
                CarMirrors::quadFunction = quad.get_first(0);
                CarMirrors::quadHook = safetyhook::create_mid(quad.get_first(0), CarMirrors::PlaceQuad);
                // how they make it: push 0 / push 10 / call allocate / mov edx, eax / add esp, 10 / test edx, edx / je /
                // mov ecx, [edx+4] / mov [edx], base vtable / xor ecx, [counter] / and ecx, 3FFF / xor [edx+4], ecx /
                // inc [counter] / mov [edx], vtable / mov [edx+8], fn / mov eax, [edi+938] / mov [edx+C], eax / jmp +2 /
                // xor edx, edx / push edx / call add
                auto make = hook::pattern("6A 00 6A 10 E8 ? ? ? ? 8B D0 83 C4 10 85 D2 74 ? 8B 4A 04 C7 02 ? ? ? ? 33 0D ? ? ? ? 81 E1 FF 3F 00 00 31 4A 04 FF 05 ? ? ? ? C7 02 ? ? ? ? C7 42 08 ? ? ? ? 8B 87 38 09 00 00 89 42 0C EB 02 33 D2 52 E8");
                for (size_t i = 0; i < make.size() && !CarMirrors::callback.add; ++i)
                {
                    auto site = make.get(i);
                    if (*site.get<void*>(57) != quad.get_first(0))
                        continue; // the same kind of command for another function
                    CarMirrors::callback.allocate = reinterpret_cast<void* (__cdecl*)(uint32_t, uint32_t)>(injector::GetBranchDestination(site.get<void>(4)).as_int());
                    CarMirrors::callback.add = reinterpret_cast<void (__cdecl*)(void*)>(injector::GetBranchDestination(site.get<void>(75)).as_int());
                    CarMirrors::callback.baseVtable = *site.get<uint32_t>(23);
                    CarMirrors::callback.counter = *site.get<uint32_t*>(29);
                    CarMirrors::callback.vtable = *site.get<uint32_t>(50);
                }
                // where the scene's draw lists queue it: mov [edx+8], 0xB59900 / mov eax, [edi+938] / mov [edx+C], eax /
                // jmp +2 (over xor edx, edx, a jump's target, left alone)
                auto queue = hook::pattern("C7 42 08 ? ? ? ? 8B 87 38 09 00 00 89 42 0C EB 02");
                for (size_t i = 0; i < queue.size(); ++i)
                {
                    if (*queue.get(i).get<void*>(3) != quad.get_first(0))
                        continue;
                    CarMirrors::queueHooks.emplace_back(safetyhook::create_mid(queue.get(i).get<void>(13), CarMirrors::QueueQuad));
                    // the water's command the same way, made right before (CE 0xADD4C2 before 0xADD519)
                    const auto at = reinterpret_cast<uintptr_t>(queue.get(i).get<void>(0));
                    for (size_t j = 0; j < queue.size() && !CarMirrors::waterFunction; ++j)
                    {
                        const auto before = reinterpret_cast<uintptr_t>(queue.get(j).get<void>(0));
                        if (before < at && at - before < 0x80)
                            CarMirrors::waterFunction = *queue.get(j).get<void*>(3);
                    }
                }
            }
            // in it: movss xmm2, [the offset] / movaps xmm1, xmm4 / mulss xmm1, xmm3 (the pattern shaders.ixx patches)
            auto offset = hook::pattern("F3 0F 10 15 ? ? ? ? 0F 28 CC F3 0F 59 CB");
            if (!offset.empty())
                CarMirrors::quadOffset = offset.get_first<float*>(4);
            // cmp byte [mirror on], 1 / push esi / mov esi, ecx / je / mov eax, [..] / test eax, eax / je / mov al, [eax+22] / and al, 1 / jne
            auto water = hook::pattern("80 3D ? ? ? ? 01 56 8B F1 74 ? A1 ? ? ? ? 85 C0 74 ? 8A 40 22 24 01 75");
            if (!water.empty() && *water.get_first<uint8_t*>(2) == CarMirrors::mirrorOn)
            {
                CarMirrors::waterVisibility = water.get_first<void(__fastcall)(uintptr_t, void*)>(0);
                CarMirrors::waterReflectionZone = *water.get_first<uintptr_t*>(13);
                CarMirrors::waterGateHooks[0] = safetyhook::create_mid(water.get_first(10), CarMirrors::WaterVisibilityGate);
            }
            // the water reflection's draw list: push ebp / mov ebp, esp / and esp, -10 / sub esp, 1C / cmp byte [mirror on], 1 /
            // push esi / mov esi, ecx / je
            auto waterBuild = hook::pattern("55 8B EC 83 E4 F0 83 EC 1C 80 3D ? ? ? ? 01 56 8B F1 0F 84");
            if (!waterBuild.empty() && *waterBuild.get_first<uint8_t*>(11) == CarMirrors::mirrorOn)
                CarMirrors::waterGateHooks[1] = safetyhook::create_mid(waterBuild.get_first(19), CarMirrors::WaterBuildGate);
            // the water's buffers: mov eax, [index] / imul eax, eax, 10C4 / add eax, buffers / ret (main thread), and
            // mov ecx, [index] / imul ecx, ecx, 10C4 / mov eax, the other / sub eax, ecx / ret (render thread)
            auto buffer = hook::pattern("A1 ? ? ? ? 69 C0 C4 10 00 00 05 ? ? ? ? C3");
            auto renderBuffer = hook::pattern("8B 0D ? ? ? ? 69 C9 C4 10 00 00 B8 ? ? ? ? 2B C1 C3");
            // the gather: mov eax, [scanning phase] / mov eax, [eax+8E8] / test al, 10 / jne / test eax, 400 / je
            auto gather = hook::pattern("A1 ? ? ? ? 8B 80 E8 08 00 00 A8 10 75 ? A9 00 04 00 00 0F 84");
            // the scan's call of it: ... / mov eax, [game state] / test eax, eax / je / mov al, [eax+22] / and al, 1 / jne
            auto scanGather = hook::pattern("A1 ? ? ? ? 56 F7 80 E8 08 00 00 00 04 00 00 8B 74 24 08 74 ? 56 E8 ? ? ? ? 83 C4 04 56 E8 ? ? ? ? A1 ? ? ? ? 83 C4 04 F6 80 E8 08 00 00 10 74 ? F6 40 19 01 74 ? A1");
            // every phase's scan done, the water reflection's last: test byte [main+19], 1 / je / cmp byte [no water], 0 /
            // jne / mov ecx, ebp / call / push 80 (hooked) / push 0 / push
            auto scansDone = hook::pattern("F6 43 19 01 74 10 80 3D ? ? ? ? 00 75 07 8B CD E8 ? ? ? ? 68 80 00 00 00 6A 00 68");
            // the water's totals: mov ecx, [quads] / test ecx, ecx / jle / mov eax, [their flags] / and word [eax], FFFE / ...
            // mov ecx, [triangles]
            auto totals = hook::pattern("8B 0D ? ? ? ? 85 C9 7E ? B8 ? ? ? ? 66 83 20 FE 8D 40 10 49 75 ? 8B 0D");
            if (!totals.empty())
            {
                CarMirrors::waterTotals[0] = *totals.get_first<const uint32_t*>(2);
                CarMirrors::waterTotals[1] = *totals.get_first<const uint32_t*>(27);
            }
            if (!buffer.empty() && !renderBuffer.empty() && !gather.empty() && !scanGather.empty() && !scansDone.empty())
            {
                CarMirrors::waterBuffer = buffer.get_first<uintptr_t(__cdecl)()>(0);
                CarMirrors::waterRenderBuffer = renderBuffer.get_first<uintptr_t(__cdecl)()>(0);
                CarMirrors::gatherWater = gather.get_first<void(__cdecl)(uint32_t)>(0);
                CarMirrors::scanningPhase = *gather.get_first<uintptr_t*>(1);
                CarMirrors::gameState = *scanGather.get_first<const uintptr_t*>(61);
                CarMirrors::noWater = *scansDone.get_first<const uint8_t*>(8);
                CarMirrors::gatherHook = safetyhook::create_mid(scansDone.get_first(22), CarMirrors::GatherMirrorWater);
            }
            // in the water's draw: mov eax, [mode] / cmp eax, 1 (hooked) / je +5 / cmp eax, 5, at its start and end
            if (CarMirrors::waterFunction)
            {
                const auto at = reinterpret_cast<uint8_t*>(CarMirrors::waterFunction);
                static constexpr uint8_t compare[] = { 0x83, 0xF8, 0x01, 0x74, 0x05, 0x83, 0xF8, 0x05 };
                const size_t sites[] = { 0x21, 0x9BA };
                bool found = true;
                for (size_t site : sites)
                    found &= std::memcmp(at + site, compare, sizeof(compare)) == 0;
                if (found)
                    for (size_t i = 0; i < std::size(sites); ++i)
                        CarMirrors::waterTargetHooks[i] = safetyhook::create_mid(at + sites[i], CarMirrors::WaterTargetMode);
                // test byte [eax], 10 / je (near), right after the call for the draw list's flags
                static constexpr uint8_t flagTest[] = { 0xF6, 0x00, 0x10, 0x0F, 0x84 };
                if (std::memcmp(at + 0x13, flagTest, sizeof(flagTest)) == 0)
                    CarMirrors::waterFlagHook = safetyhook::create_mid(at + 0x13, CarMirrors::MirrorWaterFlag);
                // cmp byte [shadows], 1 / mov ecx, [..] / jne +4 (hooked) / push 1 / jmp +2 / push 0 / call (the pass)
                static constexpr uint8_t passPick[] = { 0x75, 0x04, 0x6A, 0x01, 0xEB, 0x02, 0x6A, 0x00, 0xE8 };
                if (std::memcmp(at + 0x686, passPick, sizeof(passPick)) == 0 && at[0x679] == 0x80 && at[0x67A] == 0x3D)
                    CarMirrors::waterPassHook = safetyhook::create_mid(at + 0x686, CarMirrors::MirrorWaterPass);
            }
            // in the mirror phase's draw list: push [edi+938] / call (what is drawn with blending) / add esp, 4 /
            // cmp byte [clip plane on], 0 / je
            auto phaseWater = hook::pattern("FF B7 38 09 00 00 E8 ? ? ? ? 83 C4 04 80 3D ? ? ? ? 00 74");
            if (!phaseWater.empty() && CarMirrors::waterFunction && CarMirrors::callback.add && CarMirrors::gatherHook)
                CarMirrors::phaseWaterHook = safetyhook::create_mid(phaseWater.get_first(0), CarMirrors::PhaseWater);
            // in what is drawn with blending: mov dword [eax + 8], CCoronas::Render (the callback's function)
            if (!phaseWater.empty())
            {
                const auto blended = reinterpret_cast<uint8_t*>(injector::GetBranchDestination(phaseWater.get_first(6)).as_int());
                if (blended && blended[0x4F] == 0xC7 && blended[0x50] == 0x40 && blended[0x51] == 0x08)
                    CarMirrors::coronaSprites = *reinterpret_cast<void**>(blended + 0x52);
            }
            // the water reflection's techniques: push 0 / push C / call allocate / add esp, C / test eax, eax / je /
            // push [group] / mov ecx, eax / call (the push command) / jmp +2 / xor eax, eax / push eax / call add /
            // push 2 / push 90D / push [esi+938] / call (its entities) / push 0 / push 8 / call allocate / add esp, 18 /
            // test eax, eax / je / mov ecx, [eax+4] / mov [eax], base / xor ecx, [counter] / and ecx, 3FFF /
            // xor [eax+4], ecx / inc [counter] / mov [eax], (the pop command)
            auto techniques = hook::pattern("6A 00 6A 0C E8 ? ? ? ? 83 C4 0C 85 C0 74 ? FF 35 ? ? ? ? 8B C8 E8 ? ? ? ? EB 02 33 C0 50 E8 ? ? ? ? 6A 02 68 0D 09 00 00 FF B6 38 09 00 00 E8 ? ? ? ? 6A 00 6A 08 E8 ? ? ? ? 83 C4 18 85 C0 74 ? 8B 48 04 C7 00 ? ? ? ? 33 0D ? ? ? ? 81 E1 FF 3F 00 00 31 48 04 FF 05 ? ? ? ? C7 00");
            // round the mirror phase's entities: cmp byte [..], 0 (push hooked) / mov eax, 2 / jne / cmp byte [..], 0 / je /
            // mov eax, 1 / push eax / push 307 / push [edi+938] / call / add esp, C (pop hooked) / test esi, esi
            auto mirrorEntities = hook::pattern("80 3D ? ? ? ? 00 B8 02 00 00 00 75 09 80 3D ? ? ? ? 00 74 05 B8 01 00 00 00 50 68 07 03 00 00 FF B7 38 09 00 00 E8 ? ? ? ? 83 C4 0C 85 F6");
            // the water reflection's sky callback: call (the draw list's flags) / test byte [eax], 20 (hooked) / je / push esi /
            // call / mov esi, [clip planes] / push 0 / call / add esp, 4 / cmp byte [sky on], 0 / je / call (the sky)
            auto sky = hook::pattern("E8 ? ? ? ? F6 00 20 74 ? 56 E8 ? ? ? ? 8B 35 ? ? ? ? 6A 00 E8 ? ? ? ? 83 C4 04 80 3D ? ? ? ? 00 74 ? E8");
            if (!sky.empty() && CarMirrors::callback.add)
            {
                const auto drawSky = reinterpret_cast<uint8_t*>(injector::GetBranchDestination(sky.get_first(41)).as_int());
                // in it: test dword [eax], 20000 (the timecycle's colours, how it is drawn)
                static constexpr uint8_t colourTest[] = { 0xF7, 0x00, 0x00, 0x00, 0x02, 0x00 };
                if (drawSky && std::memcmp(drawSky + 0x156, colourTest, sizeof(colourTest)) == 0 &&
                    std::memcmp(drawSky + 0x5D3, colourTest, sizeof(colourTest)) == 0)
                {
                    CarMirrors::skyFunction = sky.get_first(0);
                    CarMirrors::skyHooks[0] = safetyhook::create_mid(drawSky + 0x156, CarMirrors::MirrorSkyFlags);
                    CarMirrors::skyHooks[1] = safetyhook::create_mid(drawSky + 0x5D3, CarMirrors::MirrorSkyFlags);
                    CarMirrors::skyHooks[2] = safetyhook::create_mid(sky.get_first(5), CarMirrors::MirrorSkyFlags);
                }
            }
            // the coronas' callback: call (the draw list's flags) / test byte [eax], 8 (hooked) / je / push esi / call /
            // mov esi, [clip planes] / push 0 / call / call (the coronas) / call (the lens flares)
            auto coronas = hook::pattern("E8 ? ? ? ? F6 00 08 74 ? 56 E8 ? ? ? ? 8B 35 ? ? ? ? 6A 00 E8 ? ? ? ? E8 ? ? ? ? E8");
            if (!coronas.empty() && !phaseWater.empty() && CarMirrors::callback.add)
            {
                // the flares: cmp byte [on], 0 / je (hooked)
                const auto flares = reinterpret_cast<uint8_t*>(injector::GetBranchDestination(coronas.get_first(34)).as_int());
                if (flares && flares[0] == 0x80 && flares[1] == 0x3D && flares[7] == 0x0F && flares[8] == 0x84)
                {
                    CarMirrors::coronaFunction = coronas.get_first(0);
                    CarMirrors::coronaHooks[0] = safetyhook::create_mid(flares + 7, CarMirrors::MirrorNoFlares);
                    CarMirrors::coronaHooks[1] = safetyhook::create_mid(phaseWater.get_first(14), CarMirrors::QueueMirrorCoronas);
                    CarMirrors::coronaHooks[2] = safetyhook::create_mid(coronas.get_first(5), CarMirrors::MirrorCoronaFlags);
                }
            }
            // the coronas' drawing list made (CE 0xC1EE40): push ebp / mov ebp, esp / and esp, -16 / sub esp, 44 /
            // mov eax, [buffer] / push ebx / mov ebx, [viewport] / push esi / xor ecx, ecx / push edi / xor edi, edi /
            // mov [esp+18], ecx / mov [esp+1C], eax / cmp [registered], ecx / jle / mov esi, list + 8
            auto coronaList = hook::pattern("55 8B EC 83 E4 F0 83 EC 44 A1 ? ? ? ? 53 8B 1D ? ? ? ? 56 33 C9 57 33 FF 89 4C 24 18 89 44 24 1C 39 0D ? ? ? ? 0F 8E ? ? ? ? BE");
            if (!coronaList.empty() && CarMirrors::plane)
            {
                auto f = coronaList.get_first<uint8_t>(0);
                auto is = [&](uint32_t offset, std::initializer_list<uint8_t> bytes)
                {
                    uint32_t i = 0;
                    for (auto b : bytes)
                        if (f[offset + i++] != b)
                            return false;
                    return true;
                };
                if (is(0x55, { 0xF3, 0x0F, 0x59, 0x05 }) && is(0x71, { 0xF3, 0x0F, 0x58, 0x05 }) && is(0x98, { 0xE8 }) &&
                    is(0x101, { 0x3D }) && is(0x12A, { 0xC1, 0xE1 }) && is(0x13A, { 0xF3, 0x0F, 0x11, 0x89 }) && is(0x1BF, { 0x89, 0x0C, 0x85 }))
                {
                    auto& c = CarMirrors::coronaCopy;
                    c.registered = *reinterpret_cast<int32_t**>(f + 0x25);
                    c.source = *reinterpret_cast<uint8_t**>(f + 0x30) - 8;
                    c.sizeScale = *reinterpret_cast<float**>(f + 0x59);
                    c.ballExtra = *reinterpret_cast<float**>(f + 0x75);
                    c.visible = reinterpret_cast<decltype(c.visible)>(f + 0x98 + 5 + *reinterpret_cast<int32_t*>(f + 0x99));
                    c.cap = *reinterpret_cast<int32_t*>(f + 0x102);
                    c.shift = f + 0x12C;
                    c.list = *reinterpret_cast<uint8_t**>(f + 0x13E); // movss [ecx + list], xmm1: the item's x
                    CarMirrors::coronaCopyHook = safetyhook::create_mid(f + 0x1BF, CarMirrors::MirrorCoronaList);
                    CoronaRender::pOtherRange = &CarMirrors::MirrorCoronaRange;
                }
            }
            // the pass state setter: mov eax, [esp+4] / mov edx, [esp+8] / push esi / mov esi, [types + eax*4] /
            // mov [values + eax*4], edx / cmp esi, -1 / je
            auto passState = hook::pattern("8B 44 24 04 8B 54 24 08 56 8B 34 85 ? ? ? ? 89 14 85 ? ? ? ? 83 FE FF 74");
            if (!passState.empty())
            {
                CarMirrors::setPassState = passState.get_first<void (__cdecl)(uint32_t, uint32_t)>(0);
                CarMirrors::passStateCache = *passState.get_first<const uint32_t*>(19);
            }
            FusionLog::Write("CarMirrors", "Init", "the entities' states kept apart in the mirror's pass %s\n", CarMirrors::setPassState ? "on" : "off (not found)");
            // the technique group's setter: mov eax, [esp+4] / mov [group], eax / ret / int3 x6 / mov eax, [group] / ret
            auto groupSetter = hook::pattern("8B 44 24 04 A3 ? ? ? ? C3 CC CC CC CC CC CC A1 ? ? ? ? C3");
            if (!groupSetter.empty() && *groupSetter.get_first<int32_t*>(5) == *groupSetter.get_first<int32_t*>(17))
                CarMirrors::currentTechniqueGroup = *groupSetter.get_first<int32_t*>(5);
            if (!techniques.empty() && !mirrorEntities.empty() && CarMirrors::callback.add)
            {
                CarMirrors::techniqueGroup.group = *techniques.get_first<const uint32_t*>(18);
                CarMirrors::techniqueGroup.pushCommand = reinterpret_cast<void (__fastcall*)(void*, void*, uint32_t)>(
                    injector::GetBranchDestination(techniques.get_first(24)).as_int());
                CarMirrors::techniqueGroup.popVtable = *techniques.get_first<uint32_t>(105);
                CarMirrors::techniqueHooks[0] = safetyhook::create_mid(mirrorEntities.get_first(0), CarMirrors::PushReflectionTechniques);
            }
            // push 0x307 (the buckets, now on the stack) / push [edi + 0x938] (hooked) / call (CE 0xB1DEE0, in it at +0x77
            // a call of the bucket draw) / add esp, 0xC (hooked: the extra buckets, the techniques popped)
            if (!mirrorEntities.empty())
            {
                CarMirrors::bucketHook = safetyhook::create_mid(mirrorEntities.get_first(34), CarMirrors::MirrorBuckets);
                const auto entities = reinterpret_cast<uint8_t*>(injector::GetBranchDestination(mirrorEntities.get_first(40)).as_int());
                if (entities && entities[0x77] == 0xE8)
                    CarMirrors::drawBucket = reinterpret_cast<decltype(CarMirrors::drawBucket)>(
                        injector::GetBranchDestination(entities + 0x77).as_int());
                CarMirrors::techniqueHooks[1] = safetyhook::create_mid(mirrorEntities.get_first(45), CarMirrors::AfterMirrorEntities);
            }
            FusionLog::Write("CarMirrors", "Init", "%u mirror glasses in the table; phase timing %s; outdoor scan %s; quad by the drawn car %s, %u draw list sites\n",
                uint32_t(std::size(CarMirrorTable)), timeBuild && timeVisibility ? "on" : "partly off",
                CarMirrors::waterVisibility ? "found" : "not found (the mirrors stay empty outdoors)",
                CarMirrors::quadHook ? "on" : "off (not found)", uint32_t(CarMirrors::queueHooks.size()));
            FusionLog::Write("CarMirrors", "Init", "the mirror pass's glass tint %s; the plane before the reflected view %s; wider reflection %s\n",
                CarMirrors::callback.add ? "on" : "off (the game's callback command was not found)",
                CarMirrors::phaseViewHook ? "on" : "off (not found)", CarMirrors::perspective ? "on" : "off (not found)");
            FusionLog::Write("CarMirrors", "Init", "water reflection gates %s/%s; water in the mirror %s, its gather after the scans %s, "
                "without the material ID target %s, its flag given %s, its pass without shadows %s\n",
                CarMirrors::waterGateHooks[0] ? "on" : "off", CarMirrors::waterGateHooks[1] ? "on" : "off",
                CarMirrors::phaseWaterHook ? "on" : "off (not found)", CarMirrors::gatherHook ? "on" : "off (not found)",
                CarMirrors::waterTargetHooks[1] ? "on" : "off (not found)", CarMirrors::waterFlagHook ? "on" : "off (not found)",
                CarMirrors::waterPassHook ? "on" : "off (not found)");
            FusionLog::Write("CarMirrors", "Init", "the mirror's entities in the reflection techniques %s; its sky %s; its coronas %s (the lamps' %s, listed by the mirror's view %s); "
                "the trees' leaves %s, buckets 5 and 4 the main scene's fourth way %s\n", CarMirrors::techniqueHooks[0] ? "on" : "off (not found)", CarMirrors::skyHooks[2] ? "on" : "off (not found)",
                CarMirrors::coronaHooks[2] ? "on" : "off (not found)", CarMirrors::coronaSprites ? "on" : "off (not found)",
                CarMirrors::coronaCopyHook ? "on" : "off (not found)", CarMirrors::bucketHook ? "on" : "off (not found)",
                CarMirrors::drawBucket && CarMirrors::techniqueHooks[1] ? "on" : "off (not found)");
            if (CarMirrors::quadOffset)
                FusionLog::Write("CarMirrors", "Init", "the game's quad offset at start %.4f m (read again each frame)\n", **CarMirrors::quadOffset);
        };
    }
} CarMirrorsHooksInstance;
