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
import temporal;

// Real reflections in the player car's mirrors, from the driver's seat in first person (Car Mirrors in the menu).
// The game has planar mirrors for interiors (bathrooms): each frame CE 0xB59DD0 asks 0xA0F820 for the nearest
// visible one an interior registered, which writes its plane (n, d) and the four corners of its glass, then whether a
// mirror is on (0x166DA20) and the camera behind it (0x166DA21). The mirror render phase (CRenderPhaseMirrorReflection)
// draws the scene from the camera reflected in that plane, clipped by it, into MIRROR_RT, and the main scene draws
// the glass as a quad on those corners (0xB59900, 3.5 cm in front of the plane along n) with mirror.fx, which reads
// MIRROR_RT at the screen position. With no interior mirror on, the mirror of the player's car the camera looks at
// most goes in instead: its glass from carmirrorstable.hxx (tools/vehicle_mirrors, from vehicles.img), in the car's
// space, placed with the car's matrix, rounded by mirror_car_shape.patch. One mirror at a time, as the game has one
// target.
namespace CarMirrors
{
    static float* plane = nullptr;    // n.xyz, d
    static float* corners = nullptr;  // 4 x xyzw in order round the glass (the game draws them as a strip 0, 1, 3, 2)
    static uint8_t* mirrorOn = nullptr;
    static uint8_t* cameraBehind = nullptr;
    static SafetyHookInline updateHook;
    static SafetyHookInline phaseBuildHook;
    static SafetyHookInline phaseVisibilityHook;

    static bool reverse = false;            // CarMirrorsReverse (hidden): the corners the other way round, if the quad is culled
    static constexpr float AheadOfGlass = 0.03f; // the game puts the quad 0.035 m along n: 5 mm in front of the glass
    static constexpr float MinLook = 0.3f;  // cosine: a mirror further off the view than ~73 degrees is not taken
    static constexpr float Keep = 0.1f;     // the mirror in use stays until another is this much nearer the view

    static uint32_t usedModel = 0;
    static int usedKind = -1;               // 0 left, 1 right, 2 interior, -1 none
    static bool usedThisFrame = false;

    // The mirror phase's CPU time while a car mirror is on, for the log every 300 frames.
    static double buildMs = 0.0, visibilityMs = 0.0;
    static uint32_t timedFrames = 0;
    static uint32_t outdoorScans = 0;      // frames the outdoor scan ran for the mirror
    // The car and glass in use, for the render thread, which puts the quad where the car was drawn: the game reads the
    // corners as it draws it (CE 0xB59900, after the G-buffer), and the car the main thread placed them by had moved
    // on by then, the quad off the mirror and sliding as the car drove.
    static std::atomic<uintptr_t> drawnVehicle{ 0 };
    static std::atomic<const CarMirrorGlass*> drawnGlass{ nullptr };
    static SafetyHookMid quadHook;
    static uint32_t quadDrawn = 0, quadByDrawnCar = 0;
    static float quadShift = 0.0f;          // the most the drawn car stood off the main thread's, since the last log

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

    // The glass in the world by the car's matrix m: the corners round it, a little in front of it, and with plane, its
    // plane; centre, where given, takes its middle.
    static void PlaceGlass(const float* m, const CarMirrorGlass& glass, bool withPlane, float* centreOut)
    {
        float centre[3], normal[3], right[3], up[3];
        Transform(m, glass.centre, centre, true);
        Transform(m, glass.normal, normal, false);
        Transform(m, glass.right, right, false);
        Transform(m, glass.up, up, false);
        const float length = std::sqrt(Dot(normal, normal));
        for (auto& n : normal) n /= length;
        if (withPlane)
        {
            for (int i = 0; i < 3; ++i) plane[i] = normal[i];
            plane[3] = Dot(normal, centre);
        }
        // round the glass: bottom left, bottom right, top right, top left as the driver sees it
        static constexpr float signs[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
        for (int k = 0; k < 4; ++k)
        {
            const auto& s = signs[reverse ? (5 - k) % 4 : k];
            for (int i = 0; i < 3; ++i)
                corners[k * 4 + i] = centre[i] + right[i] * s[0] + up[i] * s[1] - normal[i] * AheadOfGlass;
            corners[k * 4 + 3] = 1.0f;
        }
        if (centreOut)
            std::copy_n(centre, 3, centreOut);
    }

    static void Off(const char* why)
    {
        if (usedKind >= 0)
            FusionLog::Write("CarMirrors", "Select", "off: %s\n", why);
        usedKind = -1;
        usedModel = 0;
        drawnGlass.store(nullptr, std::memory_order_release);
    }

    // Main thread, once a frame, after the game looked for an interior mirror.
    static void Update()
    {
        updateHook.unsafe_call<void>();
        usedThisFrame = false;
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

        const CarMirrorGlass* best = nullptr;
        float bestScore = -2.0f;
        bool known = false;
        for (const auto& glass : CarMirrorTable)
        {
            if (glass.model != model)
                continue;
            known = true;
            float centre[3], normal[3], toEye[3];
            Transform(m, glass.centre, centre, true);
            Transform(m, glass.normal, normal, false);
            for (int i = 0; i < 3; ++i) toEye[i] = eye[i] - centre[i];
            const float distance = std::sqrt(Dot(toEye, toEye));
            if (Dot(normal, toEye) <= 0.0f || distance < 0.05f)
                continue; // seen from behind
            const float toGlass[3] = { -toEye[0] / distance, -toEye[1] / distance, -toEye[2] / distance };
            float score = Dot(forward, toGlass);
            if (model == usedModel && glass.kind == usedKind)
                score += Keep;
            if (score > bestScore)
            {
                bestScore = score;
                best = &glass;
            }
        }
        if (!known)
        {
            if (usedModel != model || usedKind != -2)
                FusionLog::Write("CarMirrors", "Select", "model %08X has no mirrors in the table\n", model);
            usedModel = model;
            usedKind = -2;
            drawnGlass.store(nullptr, std::memory_order_release);
            return;
        }
        if (!best || bestScore < MinLook)
            return Off("no mirror in view");

        float centre[3];
        PlaceGlass(m, *best, true, centre);
        drawnVehicle.store(vehicle, std::memory_order_relaxed);
        drawnGlass.store(best, std::memory_order_release);
        *mirrorOn = 1;
        *cameraBehind = 0;
        usedThisFrame = true;

        if (model != usedModel || best->kind != usedKind)
        {
            static constexpr const char* kinds[] = { "left", "right", "interior" };
            FusionLog::Write("CarMirrors", "Select", "model %08X: the %s mirror, %.2f off the view, %.2f m away, plane %.3f %.3f %.3f %.3f\n",
                model, kinds[best->kind], bestScore - (model == usedModel && best->kind == usedKind ? Keep : 0.0f),
                std::sqrt((eye[0] - centre[0]) * (eye[0] - centre[0]) + (eye[1] - centre[1]) * (eye[1] - centre[1]) +
                          (eye[2] - centre[2]) * (eye[2] - centre[2])),
                plane[0], plane[1], plane[2], plane[3]);
            usedModel = model;
            usedKind = best->kind;
        }
    }

    static void LogTiming()
    {
        if (++timedFrames < 300)
            return;
        FusionLog::Write("CarMirrors", "Cost", "mirror phase CPU per frame: draw list %.3f ms, visibility %.3f ms; outdoor scans %u of %u frames; "
            "quad placed by the drawn car %u of %u, it stood up to %.2f m off\n",
            buildMs / timedFrames, visibilityMs / timedFrames, outdoorScans, timedFrames, quadByDrawnCar, quadDrawn, quadShift);
        outdoorScans = quadDrawn = quadByDrawnCar = 0;
        quadShift = 0.0f;
        buildMs = visibilityMs = 0.0;
        timedFrames = 0;
    }

    // CRenderPhaseMirrorReflection: its draw list (vtable slot 8, CE 0xD76950) and its visibility (CE 0xD77430).
    static void __fastcall PhaseBuild(uintptr_t phase, void*)
    {
        const double start = usedThisFrame ? NowMs() : 0.0;
        phaseBuildHook.unsafe_thiscall<void>(phase);
        if (usedThisFrame)
        {
            buildMs += NowMs() - start;
            LogTiming();
        }
    }

    // CRenderPhaseWaterReflection's visibility (vtable slot 3, CE 0xD77550): the same scan from the phase's own
    // viewport, with no interior gate; it does nothing while a mirror is on.
    static void (__fastcall* waterVisibility)(uintptr_t phase, void*) = nullptr;
    // The mirror's scan runs only from an interior's room: it skips unless the phase's scene start (+0x940) has an
    // interior (+0x30) and a room above 0 (+0x38), as a bathroom mirror's has. Outdoors the car mirror's phase then
    // had nothing to draw (0.000 ms), so there it takes the water's scan, which starts from the phase's viewport like
    // the main camera, with the mirror flag down for the call. The water reflection is left out while a mirror is on.
    static void __fastcall PhaseVisibility(uintptr_t phase, void*)
    {
        const double start = usedThisFrame ? NowMs() : 0.0;
        const auto scene = *reinterpret_cast<const uintptr_t*>(phase + 0x940);
        const bool inRoom = scene && *reinterpret_cast<const uintptr_t*>(scene + 0x30) &&
                            *reinterpret_cast<const int32_t*>(scene + 0x38) > 0;
        if (usedThisFrame && !inRoom && waterVisibility)
        {
            *mirrorOn = 0;
            waterVisibility(phase, nullptr);
            *mirrorOn = 1;
            ++outdoorScans;
        }
        else
            phaseVisibilityHook.unsafe_thiscall<void>(phase);
        if (usedThisFrame)
            visibilityMs += NowMs() - start;
    }

    // Render thread, the game's mirror quad about to be drawn: its corners by the matrix the G-buffer drew the player's
    // car with this frame (TemporalAA, with object motion vectors), else as the main thread put them.
    // c152.x for mirror.fx (mirror_car_shape.patch): 1 rounds the quad as car mirrors are, 0 keeps an interior
    // mirror's corners.
    static void SetRounded(bool rounded)
    {
        auto device = rage::grcDevice::GetD3DDevice();
        if (!device)
            return;
        const float shape[4] = { rounded ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
        device->SetPixelShaderConstantF(152, shape, 1);
        if (auto real = RageDirect3DDevice9::RealDevice(device); real != device)
            real->SetPixelShaderConstantF(152, shape, 1);
    }

    static void PlaceQuad(SafetyHookContext&)
    {
        const auto glass = drawnGlass.load(std::memory_order_acquire);
        const auto vehicle = drawnVehicle.load(std::memory_order_relaxed);
        const bool ours = glass && vehicle && mirrorOn && *mirrorOn;
        SetRounded(ours);
        if (!ours)
            return;
        ++quadDrawn;
        float around[3];
        if (!CEntity::GetPosition(vehicle, around))
            return;
        float m[16];
        bool skinned = false;
        if (!TemporalAA::DrawnWorldMatrix(reinterpret_cast<void*>(vehicle), around, 3.0f, m, nullptr, nullptr, nullptr, &skinned, 25.0f) || !skinned)
            return;
        const float dx = m[12] - around[0], dy = m[13] - around[1], dz = m[14] - around[2];
        quadShift = (std::max)(quadShift, std::sqrt(dx * dx + dy * dy + dz * dz));
        PlaceGlass(m, *glass, false, nullptr);
        ++quadByDrawnCar;
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
            // push ebp / mov ebp, esp / and esp, -10 / sub esp, 20 / call / test byte [eax], 40 / je / cmp byte [mirror on], 0 / je
            auto quad = hook::pattern("55 8B EC 83 E4 F0 83 EC 20 E8 ? ? ? ? F6 00 40 0F 84 ? ? ? ? 80 3D ? ? ? ? 00 0F 84");
            if (!quad.empty() && *quad.get_first<uint8_t*>(25) == CarMirrors::mirrorOn)
                CarMirrors::quadHook = safetyhook::create_mid(quad.get_first(0), CarMirrors::PlaceQuad);
            // cmp byte [mirror on], 1 / push esi / mov esi, ecx / je / mov eax, [..] / test eax, eax / je / mov al, [eax+22] / and al, 1 / jne
            auto water = hook::pattern("80 3D ? ? ? ? 01 56 8B F1 74 ? A1 ? ? ? ? 85 C0 74 ? 8A 40 22 24 01 75");
            if (!water.empty() && *water.get_first<uint8_t*>(2) == CarMirrors::mirrorOn)
                CarMirrors::waterVisibility = water.get_first<void(__fastcall)(uintptr_t, void*)>(0);
            FusionLog::Write("CarMirrors", "Init", "%u mirror glasses in the table; phase timing %s; outdoor scan %s; quad by the drawn car %s\n",
                uint32_t(std::size(CarMirrorTable)), timeBuild && timeVisibility ? "on" : "partly off",
                CarMirrors::waterVisibility ? "found" : "not found (the mirrors stay empty outdoors)",
                CarMirrors::quadHook ? "on" : "off (not found)");
        };
    }
} CarMirrorsHooksInstance;
