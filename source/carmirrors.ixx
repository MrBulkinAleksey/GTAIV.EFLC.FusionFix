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
// the glass as a quad on those corners (0xB59900, in front of the plane along n by a float in its code) with mirror.fx, which reads
// MIRROR_RT at the screen position. With no interior mirror on, the mirror of the player's car the camera looks at
// most goes in instead: its glass from carmirrorstable.hxx (tools/vehicle_mirrors, from vehicles.img), in the car's
// space, placed with the car's matrix, cut to the glass's outline and widened as a convex mirror by
// mirror_car_shape.patch. One mirror at a time, as the game has one
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
    static bool rounded = true;             // CarMirrorsRounded (hidden): 0 draws the plain quad, not cut to the glass's outline
    static bool byDrawnCar = true;          // CarMirrorsByDrawnCar (hidden): 0 keeps the main thread's update's corners
    static float zoomOut = 1.3f;            // CarMirrorsZoomOut (hidden): how much wider than a flat mirror they show, 1 flat
    // The game puts the quad along n by a float its quad code reads (0.035 m on PC, 0.0125 with shaders.ixx's Xbox
    // value): read where the code points as the corners are placed, which lands the quad 5 mm in front of the glass.
    // Taken as 0.035, the quad sat 1.75 cm behind the glass, inside the mirror's housing.
    static float* const* quadOffset = nullptr;
    static constexpr float InFrontOfGlass = 0.005f;
    static constexpr float MinLook = 0.3f;  // cosine: a mirror further off the view than ~73 degrees is not taken
    static constexpr float Keep = 0.1f;     // the mirror in use stays until another is this much nearer the view
    // ... and while it is on the screen at all (within ~60 degrees of the view): the game draws one mirror, and the
    // one in use went off while still in view as soon as another came nearer the view's middle.
    static constexpr float OnScreen = 0.5f;

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
    static std::vector<SafetyHookMid> queueHooks;
    static uint32_t quadDrawn = 0, quadByDrawnCar = 0, quadBySnapshot = 0;
    // The corners as the quad is put in the scene's draw list (main thread, CE 0xADD526 / 0xADD930), with the car where
    // its own draws of that list are made: the quad's command hands its parameter to 0xB59900, which never reads it, so
    // it carries the snapshot's number to the render thread. No object motion vectors needed.
    struct Snapshot
    {
        std::atomic<uint32_t> number{ 0 };
        float corners[16]{};
    };
    static Snapshot snapshots[16];
    static uint32_t nextSnapshot = 1;
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
    static void PlaceGlass(const float* m, const CarMirrorGlass& glass, bool withPlane, float* centreOut, float* out = nullptr)
    {
        if (!out)
            out = corners;
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
        const float pushed = (quadOffset && *quadOffset ? **quadOffset : 0.035f) - InFrontOfGlass;
        // round the glass: bottom left, bottom right, top right, top left as the driver sees it
        static constexpr float signs[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
        for (int k = 0; k < 4; ++k)
        {
            const auto& s = signs[reverse ? (5 - k) % 4 : k];
            for (int i = 0; i < 3; ++i)
                out[k * 4 + i] = centre[i] + right[i] * s[0] + up[i] * s[1] - normal[i] * pushed;
            out[k * 4 + 3] = 1.0f;
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
        float bestScore = -2.0f, bestLook = -2.0f;
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
            const float look = Dot(forward, toGlass);
            float score = look;
            if (model == usedModel && glass.kind == usedKind)
                score += look >= OnScreen ? 10.0f : Keep;
            if (score > bestScore)
            {
                bestScore = score;
                bestLook = look;
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
        if (!best || bestLook < MinLook)
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
                model, kinds[best->kind], bestLook,
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
            "quads %u: by the draw list's car %u, by the drawn car %u (it stood up to %.2f m off)\n",
            buildMs / timedFrames, visibilityMs / timedFrames, outdoorScans, timedFrames, quadDrawn, quadBySnapshot, quadByDrawnCar, quadShift);
        outdoorScans = quadDrawn = quadByDrawnCar = quadBySnapshot = 0;
        quadShift = 0.0f;
        buildMs = visibilityMs = 0.0;
        timedFrames = 0;
    }

    // CRenderPhaseMirrorReflection: its draw list (vtable slot 8, CE 0xD76950) and its visibility (CE 0xD77430).
    // The plane again as the phase's draw list is made, with the car where its draws of it are: the update's plane is
    // from before the car moved this frame, and the reflected view then lagged the car's own side in the mirror.
    static void __fastcall PhaseBuild(uintptr_t phase, void*)
    {
        const double start = usedThisFrame ? NowMs() : 0.0;
        if (usedThisFrame)
        {
            const auto glass = drawnGlass.load(std::memory_order_acquire);
            const auto vehicle = drawnVehicle.load(std::memory_order_relaxed);
            if (const float* m = glass && vehicle ? CEntity::GetMatrix(vehicle) : nullptr)
                PlaceGlass(m, *glass, true, nullptr);
        }
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

    // c152 for mirror.fx (mirror_car_shape.patch): x 1 cuts the quad to the glass's outline, y the zoom out - 1 widens
    // the view as a convex mirror's; zeros keep an interior mirror as the game draws it.
    // c153-c160: the glass's outline's edges, a corner xy and the way to the next zw, counter-clockwise over the quad's
    // -1..1 (u right, v up as the driver sees it; CarMirrorsReverse turns u round); unused edges all zero.
    static void SetShape(const CarMirrorGlass* glass)
    {
        auto device = rage::grcDevice::GetD3DDevice();
        if (!device)
            return;
        float shape[9][4]{};
        const int n = glass ? glass->corners : 0;
        shape[0][0] = glass && rounded && n >= 3 ? 1.0f : 0.0f;
        shape[0][1] = glass ? zoomOut - 1.0f : 0.0f;
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

    // Main thread, the quad's command just made (edx), its parameter about to be stored from eax: the corners by the car
    // as it is now, and their number in its place.
    static void QueueQuad(SafetyHookContext& regs)
    {
        const auto glass = drawnGlass.load(std::memory_order_acquire);
        const auto vehicle = drawnVehicle.load(std::memory_order_relaxed);
        const float* m = vehicle ? CEntity::GetMatrix(vehicle) : nullptr;
        if (!glass || !m || !mirrorOn || !*mirrorOn || !byDrawnCar)
            return;
        const uint32_t number = nextSnapshot++ | 0x80000000u; // never 0, never a small visibility id
        auto& snapshot = snapshots[number & 15];
        snapshot.number.store(0, std::memory_order_relaxed);
        PlaceGlass(m, *glass, false, nullptr, snapshot.corners);
        snapshot.number.store(number, std::memory_order_release);
        regs.eax = number;
    }

    // Render thread, the game's mirror quad about to be drawn (its parameter at esp+4): its corners as the draw list put
    // them, else by the matrix the G-buffer drew the player's car with (TemporalAA, with object motion vectors), else as
    // the main thread's update put them.
    static void PlaceQuad(SafetyHookContext& regs)
    {
        const auto glass = drawnGlass.load(std::memory_order_acquire);
        const auto vehicle = drawnVehicle.load(std::memory_order_relaxed);
        const bool ours = glass && vehicle && mirrorOn && *mirrorOn;
        SetShape(ours ? glass : nullptr);
        if (!ours || !byDrawnCar)
            return;
        ++quadDrawn;
        const uint32_t number = *reinterpret_cast<const uint32_t*>(regs.esp + 4);
        if (number & 0x80000000u)
        {
            const auto& snapshot = snapshots[number & 15];
            if (snapshot.number.load(std::memory_order_acquire) == number)
            {
                std::copy_n(snapshot.corners, 16, corners);
                ++quadBySnapshot;
                return;
            }
        }
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
                CarMirrors::rounded = ini.ReadInteger("MISC", "CarMirrorsRounded", 1) != 0;
                CarMirrors::byDrawnCar = ini.ReadInteger("MISC", "CarMirrorsByDrawnCar", 1) != 0;
                CarMirrors::zoomOut = std::clamp(ini.ReadFloat("MISC", "CarMirrorsZoomOut", 1.3f), 1.0f, 2.5f);
                FusionLog::Write("CarMirrors", "Init", "reverse %d, rounded %d, by the drawn car %d, zoom out %.2f\n",
                    int(CarMirrors::reverse), int(CarMirrors::rounded), int(CarMirrors::byDrawnCar), CarMirrors::zoomOut);
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
            {
                CarMirrors::quadHook = safetyhook::create_mid(quad.get_first(0), CarMirrors::PlaceQuad);
                // where the scene's draw lists queue it: mov [edx+8], 0xB59900 / mov eax, [edi+938] / mov [edx+C], eax /
                // jmp +2 (over xor edx, edx, a jump's target, left alone)
                auto queue = hook::pattern("C7 42 08 ? ? ? ? 8B 87 38 09 00 00 89 42 0C EB 02");
                for (size_t i = 0; i < queue.size(); ++i)
                    if (*queue.get(i).get<void*>(3) == quad.get_first(0))
                        CarMirrors::queueHooks.emplace_back(safetyhook::create_mid(queue.get(i).get<void>(13), CarMirrors::QueueQuad));
            }
            // in it: movss xmm2, [the offset] / movaps xmm1, xmm4 / mulss xmm1, xmm3 (the pattern shaders.ixx patches)
            auto offset = hook::pattern("F3 0F 10 15 ? ? ? ? 0F 28 CC F3 0F 59 CB");
            if (!offset.empty())
                CarMirrors::quadOffset = offset.get_first<float*>(4);
            // cmp byte [mirror on], 1 / push esi / mov esi, ecx / je / mov eax, [..] / test eax, eax / je / mov al, [eax+22] / and al, 1 / jne
            auto water = hook::pattern("80 3D ? ? ? ? 01 56 8B F1 74 ? A1 ? ? ? ? 85 C0 74 ? 8A 40 22 24 01 75");
            if (!water.empty() && *water.get_first<uint8_t*>(2) == CarMirrors::mirrorOn)
                CarMirrors::waterVisibility = water.get_first<void(__fastcall)(uintptr_t, void*)>(0);
            FusionLog::Write("CarMirrors", "Init", "%u mirror glasses in the table; phase timing %s; outdoor scan %s; quad by the drawn car %s, %u draw list sites\n",
                uint32_t(std::size(CarMirrorTable)), timeBuild && timeVisibility ? "on" : "partly off",
                CarMirrors::waterVisibility ? "found" : "not found (the mirrors stay empty outdoors)",
                CarMirrors::quadHook ? "on" : "off (not found)", uint32_t(CarMirrors::queueHooks.size()));
            if (CarMirrors::quadOffset)
                FusionLog::Write("CarMirrors", "Init", "the game's quad offset at start %.4f m (read again each frame)\n", **CarMirrors::quadOffset);
        };
    }
} CarMirrorsHooksInstance;
