module;

#include <common.hxx>
#include "FusionLog.hpp"
#include "carmirrorstable.hxx"

export module carmirrors;

import common;
import comvars;
import natives;
import settings;
import inireload;
import firstperson;

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

    static bool reverse = false;            // CarMirrorsReverse (hidden): the corners the other way round, if the quad is culled
    static bool rounded = true;             // CarMirrorsRounded (hidden): 0 draws the plain quad, not cut to the glass's outline
    static bool byDrawnCar = true;          // CarMirrorsByDrawnCar (hidden): 0 keeps the update's corners, no dark glass
    static float zoomOut = 1.5f;            // CarMirrorsZoomOut (hidden): how much wider than a flat mirror they show, 1 flat
    static bool water = true;               // CarMirrorsWater (hidden): 0 leaves the water out of the reflection
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

    // The mirror phase's CPU time while a car mirror is on, for the log every 300 frames.
    static double buildMs = 0.0, visibilityMs = 0.0;
    static uint32_t timedFrames = 0;
    static uint32_t outdoorScans = 0;      // frames the outdoor scan ran for the mirror
    static uint32_t waterQueued = 0;       // the water's commands put in the mirror phase's draw list
    static uint32_t waterQuads = 0;        // the water's quads the scan gathered in the mirror phase's list
    static uintptr_t (__cdecl* waterLists)() = nullptr; // CE 0xAD61A0: the main thread's water lists, quads of list i at +0xFEC + 4i
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
    static void __cdecl MirrorPassTint(uint32_t on)
    {
        auto device = rage::grcDevice::GetD3DDevice();
        if (!device)
            return;
        const float tint[4] = { on ? 0.4f : 0.0f, on ? 4.0f * 4.0f : 0.0f, 0.0f, 0.0f };
        device->SetPixelShaderConstantF(204, tint, 1);
        if (auto real = RageDirect3DDevice9::RealDevice(device); real != device)
            real->SetPixelShaderConstantF(204, tint, 1);
        if (on)
            for (DWORD stage : { 9u, 11u, 13u })
                RageDirect3DDevice9::SetTextureBoth(device, stage, nullptr);
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

    // Main thread, once a frame, after the game looked for an interior mirror.
    static void Update()
    {
        updateHook.unsafe_call<void>();
        usedThisFrame = false;
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

    static void LogTiming()
    {
        if (++timedFrames < 300)
            return;
        FusionLog::Write("CarMirrors", "Cost", "mirror phase CPU per frame: draw list %.3f ms, visibility %.3f ms; outdoor scans %u of %u frames; "
            "quads drawn %u, by the draw list's car %u, dark %u; water queued %u, its quads gathered %u\n",
            buildMs / timedFrames, visibilityMs / timedFrames, outdoorScans, timedFrames, quadDrawn, quadBySnapshot, quadsDark, waterQueued, waterQuads);
        outdoorScans = quadDrawn = quadBySnapshot = quadsDark = waterQueued = waterQuads = 0;
        buildMs = visibilityMs = 0.0;
        timedFrames = 0;
    }

    // The phase's flags (+0x8E8, the draw list takes a copy as it is made): 0x10 has the scan gather the water seen
    // in the phase's list (CE 0xB1E810 -> 0xAD7D80) and the water's draw (CE 0xAD7EF0) draw it; the mirror phase has
    // it not. Set from the car mirror's scan to its draw list's end, with the water queued in that list (PhaseWater).
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

    // CRenderPhaseMirrorReflection: its draw list (vtable slot 8, CE 0xD76950) and its visibility (CE 0xD77430).
    // The plane again as the phase's draw list is made, with the car where its draws of it are: the update's plane is
    // from before the car moved this frame, and the reflected view then lagged the car's own side in the mirror.
    static void __fastcall PhaseBuild(uintptr_t phase, void*)
    {
        const double start = usedThisFrame ? NowMs() : 0.0;
        if (usedThisFrame)
        {
            const auto vehicle = drawnVehicle.load(std::memory_order_relaxed);
            if (const float* m = active && vehicle ? CEntity::GetMatrix(vehicle) : nullptr)
                PlacePlane(m, *active);
            callback.Queue(reinterpret_cast<const void*>(&MirrorPassTint), 1);
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

    // The water reflection's visibility (CE 0xD77550) and draw list (CE 0xD76C10) are skipped while a mirror is on
    // (0x166DA20 == 1): a bathroom mirror has no water to see. A car's mirror is on all the time in first person, and
    // the water by the road lost its reflection (and the mirrors the water). Each gate's je, right after the compare,
    // is not taken while the mirror on is the car's: a flag in the compare's result, not the mirror flag itself,
    // which the render thread reads as it draws the mirror's quad.
    static void WaterGate(SafetyHookContext& regs)
    {
        if (quadsThisFrame)
            regs.eflags &= ~0x40u; // ZF
    }

    // The mirror's scan runs only from an interior's room: it skips unless the phase's scene start (+0x940) has an
    // interior (+0x30) and a room above 0 (+0x38), as a bathroom mirror's has. Outdoors the car mirror's phase then
    // had nothing to draw (0.000 ms), so there it takes the water's scan, which starts from the phase's viewport like
    // the main camera (its mirror gate let through by WaterGate), gathering the water too.
    static void __fastcall PhaseVisibility(uintptr_t phase, void*)
    {
        ClearWaterFlag();
        const double start = usedThisFrame ? NowMs() : 0.0;
        const auto scene = *reinterpret_cast<const uintptr_t*>(phase + 0x940);
        const bool inRoom = scene && *reinterpret_cast<const uintptr_t*>(scene + 0x30) &&
                            *reinterpret_cast<const int32_t*>(scene + 0x38) > 0;
        if (usedThisFrame && !inRoom && waterVisibility)
        {
            if (water && waterFunction && phaseWaterHook)
                SetWaterFlag(phase);
            const bool gated = !waterGateHooks[0];
            if (gated) *mirrorOn = 0;
            waterVisibility(phase, nullptr);
            if (gated) *mirrorOn = 1;
            ++outdoorScans;
        }
        else
            phaseVisibilityHook.unsafe_thiscall<void>(phase);
        if (usedThisFrame)
            visibilityMs += NowMs() - start;
    }

    // In the mirror phase's draw list, after its entities and before what is drawn with blending (CE 0xC0FA50): the
    // water, as the main scene queues it after its own (the stencil states it sets round it there left out: the
    // mirror's pass reads no stencil after).
    static void PhaseWater(SafetyHookContext& regs)
    {
        const uintptr_t phase = regs.edi;
        if (!waterFlagPhase || phase != waterFlagPhase)
            return;
        const uint32_t list = *reinterpret_cast<const uint32_t*>(phase + 0x938);
        callback.Queue(waterFunction, list);
        ++waterQueued;
        if (waterLists && list < 24)
            waterQuads += *reinterpret_cast<const uint32_t*>(waterLists() + 0xFEC + 4 * list);
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
        FusionFix::onInitEventAsync() += []()
        {
            static auto readIni = [](CIniReader& ini)
            {
                CarMirrors::reverse = ini.ReadInteger("MISC", "CarMirrorsReverse", 0) != 0;
                CarMirrors::rounded = ini.ReadInteger("MISC", "CarMirrorsRounded", 1) != 0;
                CarMirrors::byDrawnCar = ini.ReadInteger("MISC", "CarMirrorsByDrawnCar", 1) != 0;
                CarMirrors::zoomOut = std::clamp(ini.ReadFloat("MISC", "CarMirrorsZoomOut", 1.5f), 1.0f, 2.5f);
                CarMirrors::water = ini.ReadInteger("MISC", "CarMirrorsWater", 1) != 0;
                CarMirrors::sideAimX = std::clamp(ini.ReadFloat("MISC", "CarMirrorsSideAimX", 0.0f), -30.0f, 30.0f);
                CarMirrors::sideAimY = std::clamp(ini.ReadFloat("MISC", "CarMirrorsSideAimY", -1.5f), -30.0f, 30.0f);
                CarMirrors::interiorAimX = std::clamp(ini.ReadFloat("MISC", "CarMirrorsInteriorAimX", 0.0f), -30.0f, 30.0f);
                CarMirrors::interiorAimY = std::clamp(ini.ReadFloat("MISC", "CarMirrorsInteriorAimY", -3.0f), -30.0f, 30.0f);
                FusionLog::Write("CarMirrors", "Init", "reverse %d, rounded %d, by the drawn car %d, zoom out %.2f, water %d; aim x/y: side %.1f/%.1f, interior %.1f/%.1f\n",
                    int(CarMirrors::reverse), int(CarMirrors::rounded), int(CarMirrors::byDrawnCar), CarMirrors::zoomOut, int(CarMirrors::water),
                    CarMirrors::sideAimX, CarMirrors::sideAimY, CarMirrors::interiorAimX, CarMirrors::interiorAimY);
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
                CarMirrors::waterGateHooks[0] = safetyhook::create_mid(water.get_first(10), CarMirrors::WaterGate);
            }
            // the water reflection's draw list: push ebp / mov ebp, esp / and esp, -10 / sub esp, 1C / cmp byte [mirror on], 1 /
            // push esi / mov esi, ecx / je
            auto waterBuild = hook::pattern("55 8B EC 83 E4 F0 83 EC 1C 80 3D ? ? ? ? 01 56 8B F1 0F 84");
            if (!waterBuild.empty() && *waterBuild.get_first<uint8_t*>(11) == CarMirrors::mirrorOn)
                CarMirrors::waterGateHooks[1] = safetyhook::create_mid(waterBuild.get_first(19), CarMirrors::WaterGate);
            // in the mirror phase's draw list: push [edi+938] / call (what is drawn with blending) / add esp, 4 /
            // cmp byte [clip plane on], 0 / je
            // mov eax, [index] / imul eax, eax, 10C4 / add eax, lists / ret
            auto lists = hook::pattern("A1 ? ? ? ? 69 C0 C4 10 00 00 05 ? ? ? ? C3");
            if (!lists.empty())
                CarMirrors::waterLists = lists.get_first<uintptr_t(__cdecl)()>(0);
            auto phaseWater = hook::pattern("FF B7 38 09 00 00 E8 ? ? ? ? 83 C4 04 80 3D ? ? ? ? 00 74");
            if (!phaseWater.empty() && CarMirrors::waterFunction && CarMirrors::callback.add)
                CarMirrors::phaseWaterHook = safetyhook::create_mid(phaseWater.get_first(0), CarMirrors::PhaseWater);
            FusionLog::Write("CarMirrors", "Init", "%u mirror glasses in the table; phase timing %s; outdoor scan %s; quad by the drawn car %s, %u draw list sites\n",
                uint32_t(std::size(CarMirrorTable)), timeBuild && timeVisibility ? "on" : "partly off",
                CarMirrors::waterVisibility ? "found" : "not found (the mirrors stay empty outdoors)",
                CarMirrors::quadHook ? "on" : "off (not found)", uint32_t(CarMirrors::queueHooks.size()));
            FusionLog::Write("CarMirrors", "Init", "the mirror pass's glass tint %s; the plane before the reflected view %s; wider reflection %s\n",
                CarMirrors::callback.add ? "on" : "off (the game's callback command was not found)",
                CarMirrors::phaseViewHook ? "on" : "off (not found)", CarMirrors::perspective ? "on" : "off (not found)");
            FusionLog::Write("CarMirrors", "Init", "water reflection gates %s/%s; water in the mirror %s\n",
                CarMirrors::waterGateHooks[0] ? "on" : "off", CarMirrors::waterGateHooks[1] ? "on" : "off",
                CarMirrors::phaseWaterHook ? "on" : "off (not found)");
            if (CarMirrors::quadOffset)
                FusionLog::Write("CarMirrors", "Init", "the game's quad offset at start %.4f m (read again each frame)\n", **CarMirrors::quadOffset);
        };
    }
} CarMirrorsHooksInstance;
