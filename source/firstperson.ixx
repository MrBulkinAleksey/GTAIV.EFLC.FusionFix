module;

#include <common.hxx>

export module firstperson;

import common;
import comvars;
import natives;
import inireload;

// First person on foot, as a fourth step of the on-foot camera views. INPUT_NEXT_CAMERA steps the
// zoom of CCamFollowPed (+0x360) through 1, 2 and 3 (CE 0xA21A1C); after 3 the view now goes to
// first person, with the zoom held at 1 underneath, and the next press back to 1. The follow
// camera keeps working as it does (look, aim, its collision), only its position is moved to the
// player's head once it is done: a little ahead of it along the level view, further out over the chest
// as the view goes down (not with the phone in the hand, which stays in view), so the body, the
// arms and the weapon in them show, and the feet looking down.
//
// First person in vehicles, as one more step after the hood view. CCamFollowVehicle keeps its view
// in +0x22C (CE 0xA12DF0 steps it on a press: 0..3 behind the car, 4 the hood, then 5 or back to
// 0/1); after the hood the view now goes to first person, with the hood view held underneath, so its
// look around and its rotation with the car stay and only the eye is moved to the player's head. The
// head bone lags the car by a frame or two at speed (the skeleton is posed before the car moves), so
// the head is kept in the ped's own frame, learned while the car nearly stands, and placed with the
// ped's matrix as it is when the final camera is built.
//
// In both the head, hair and face are not drawn and the near clip is raised, which cuts away the
// neck and shoulders around the eye.
namespace FirstPerson
{
    static bool active = false;
    static bool vehicleActive = false;
    static float forwardOffset = 0.1f;
    static float upOffset = 0.05f;
    // Where the follow camera's update put the view this frame, and the frame it did: the final
    // camera takes it and the head is hidden only while it is current, so it shows again once
    // another camera takes over (aiming, cars, cutscenes).
    static float eye[3]{};
    static uint32_t eyeFrame = 0;
    // The same for the vehicle camera: the head in the ped's frame (x right, y forward, z up), put in
    // the world only by the final camera.
    static float seat[3]{};
    static uint32_t seatFrame = 0;
    static constexpr uint32_t HeadBone = 0x4B5; // the bone the game itself asks for most, the head
    static constexpr float HeadAbovePed = 0.6f;   // fallback without the bone, above the ped's origin
    static constexpr uintptr_t ZoomOffset = 0x360, TargetOffset = 0x384;
    static constexpr uintptr_t VehicleViewOffset = 0x22C, VehicleOffset = 0x170;
    static constexpr int8_t HoodView = 4;
    static constexpr float NearClip = 0.12f;        // the game's own is 0.05; hands, guns and the wheel are further
    static constexpr float LookDownForward = 0.2f;  // m the eye moves out over the chest looking straight down
    static constexpr float LookDownUp = 0.03f;
    static float lookDownWeight = 0.0f;              // eases to 0 while the phone is in the hand
    static constexpr float LearnBelowSpeed = 1.5f;  // m/s: below it the head bone's lag is a few cm

    static SafetyHookMid zoomHook;
    static SafetyHookInline updateHook;
    static SafetyHookMid finalHook;
    static SafetyHookMid vehicleViewHook;
    static SafetyHookInline vehicleUpdateHook;

    static bool EyeCurrent() noexcept
    {
        return active && CTimer::m_frameCount && *CTimer::m_frameCount - eyeFrame <= 1;
    }

    static bool SeatCurrent() noexcept
    {
        return vehicleActive && CTimer::m_frameCount && *CTimer::m_frameCount - seatFrame <= 1;
    }

    static bool HeadBonePosition(Ped handle, float (&head)[3])
    {
        Vector3 bone{};
        Natives::GetPedBonePosition(handle, HeadBone, 0.0f, 0.0f, 0.0f, &bone);
        if (!std::isfinite(bone.fX) || !std::isfinite(bone.fY) || !std::isfinite(bone.fZ) ||
            (bone.fX == 0.0f && bone.fY == 0.0f && bone.fZ == 0.0f))
            return false;
        head[0] = bone.fX; head[1] = bone.fY; head[2] = bone.fZ;
        return true;
    }

    // At the wrap of the zoom after a press may have stepped it (mov eax, [ecx+360]), ecx the camera.
    static void StepZoom(SafetyHookContext& regs)
    {
        auto& zoom = *reinterpret_cast<int32_t*>(regs.ecx + ZoomOffset);
        if (active)
        {
            if (zoom != 1)
            {
                active = false;
                zoom = 1;
            }
        }
        else if (zoom >= 4)
        {
            active = true;
            zoom = 1;
        }
    }

    // CCamFollowPed::Update (vtable slot 4), then where the eye goes: the head, a little ahead along
    // the level view and up, and out over the chest as the view goes down. The camera's frame at +0x10
    // holds right, front and up, then the position at +0x40, which is moved too.
    static bool __fastcall Update(uintptr_t camera, void*)
    {
        const bool result = updateHook.unsafe_thiscall<bool>(camera);
        if (!active || !CPlayer::getLocalPlayerPed || !CTimer::m_frameCount)
            return result;
        const auto ped = CPlayer::getLocalPlayerPed();
        if (!ped || *reinterpret_cast<const uintptr_t*>(camera + TargetOffset) != ped)
            return result;

        float head[3]{};
        Ped handle = 0;
        Natives::GetPlayerChar(Natives::GetPlayerId(), &handle);
        if (!handle || !HeadBonePosition(handle, head))
        {
            if (!CEntity::GetPosition(ped, head)) return result;
            head[2] += HeadAbovePed;
        }

        int32_t phoneTask = 0;
        const bool phone = handle && (Natives::GetMobilePhoneTaskSubTask(handle, &phoneTask) || Natives::IsMobilePhoneCallOngoing());
        lookDownWeight += ((phone ? 0.0f : 1.0f) - lookDownWeight) * 0.1f;

        auto frame = reinterpret_cast<float*>(camera + 0x10);
        const float* front = frame + 4;
        const float level = std::sqrt(front[0] * front[0] + front[1] * front[1]);
        const float down = std::max(-front[2], 0.0f) * lookDownWeight;
        const float push = forwardOffset + down * LookDownForward;
        if (level > 1e-3f)
        {
            eye[0] = head[0] + front[0] / level * push;
            eye[1] = head[1] + front[1] / level * push;
        }
        else
        {
            eye[0] = head[0];
            eye[1] = head[1];
        }
        eye[2] = head[2] + upOffset + down * LookDownUp;
        std::copy(std::begin(eye), std::end(eye), frame + 12);
        eyeFrame = *CTimer::m_frameCount;
        return result;
    }

    // Before the vehicle camera steps its view on a press (movsx eax, byte [esi+22C]), esi the camera.
    // From the hood the press goes to first person: the view is set back by one, so the step lands on
    // the hood again (and centres the look, as it does there); the next press goes on from the hood.
    static void StepVehicleView(SafetyHookContext& regs)
    {
        auto& view = *reinterpret_cast<int8_t*>(regs.esi + VehicleViewOffset);
        if (vehicleActive)
            vehicleActive = false;
        else if (view == HoodView)
        {
            vehicleActive = true;
            view = HoodView - 1;
        }
    }

    // Where the head sits in the ped's frame. The bone is from the last posing, a frame or two behind
    // the ped's matrix while the car moves; matched against the last few matrices, the one that puts it
    // nearest to where the head was learned (or the usual seated head) is the one it was posed with.
    struct SeatTracker
    {
        static constexpr int History = 4;
        float matrices[History][16]{};
        int count = 0;
        uint32_t lastFrame = 0;
        std::map<uint32_t, std::array<float, 3>> learned; // by vehicle model
        std::array<float, 3> lastLearned[2]{ { 0.0f, -0.2f, 0.56f }, { 0.0f, 0.0f, 0.6f } }; // car, bike

        static void ToLocal(const float* m, const float (&world)[3], float (&local)[3])
        {
            const float d[3] = { world[0] - m[12], world[1] - m[13], world[2] - m[14] };
            for (int r = 0; r < 3; ++r)
                local[r] = d[0] * m[r * 4] + d[1] * m[r * 4 + 1] + d[2] * m[r * 4 + 2];
        }

        static bool Plausible(const float (&p)[3])
        {
            return p[0] > -0.4f && p[0] < 0.4f && p[1] > -0.7f && p[1] < 0.7f && p[2] > 0.2f && p[2] < 1.0f;
        }

        // The ped's matrix of this frame; a gap of a few frames is a new stint in a vehicle.
        void Push(const float* m, uint32_t frame)
        {
            if (count && frame - lastFrame > 4) count = 0;
            lastFrame = frame;
            for (int k = History - 1; k > 0; --k)
                std::copy_n(matrices[k - 1], 16, matrices[k]);
            std::copy_n(m, 16, matrices[0]);
            count = std::min(count + 1, History);
        }

        void Update(const float (&head)[3], uint32_t model, bool bike, float speed, float (&out)[3])
        {
            // a ped just sat down may still be moving into the seat: learn once he has sat a moment
            if (speed < LearnBelowSpeed && count == History)
            {
                float local[3];
                ToLocal(matrices[0], head, local);
                if (Plausible(local))
                {
                    auto [it, fresh] = learned.try_emplace(model, std::array<float, 3>{ local[0], local[1], local[2] });
                    if (!fresh)
                        for (int i = 0; i < 3; ++i) it->second[i] += (local[i] - it->second[i]) * 0.1f;
                    lastLearned[bike] = it->second;
                }
            }
            if (auto it = learned.find(model); it != learned.end())
            {
                std::copy(it->second.begin(), it->second.end(), out);
                return;
            }
            const auto& guess = lastLearned[bike];
            std::copy(guess.begin(), guess.end(), out);
            float best = std::numeric_limits<float>::max();
            for (int k = 0; k < count; ++k)
            {
                float local[3];
                ToLocal(matrices[k], head, local);
                float d = 0.0f;
                for (int i = 0; i < 3; ++i) d += (local[i] - guess[i]) * (local[i] - guess[i]);
                if (Plausible(local) && d < best)
                {
                    best = d;
                    std::copy(std::begin(local), std::end(local), out);
                }
            }
        }
    };
    static SeatTracker seatTracker;

    // CCamFollowVehicle::Update (vtable slot 4), then, in first person, where the head sits in the
    // player's frame: only while he sits in a car the camera follows, not while getting in or out.
    static uint32_t __fastcall VehicleUpdate(uintptr_t camera, void*)
    {
        const auto result = vehicleUpdateHook.unsafe_thiscall<uint32_t>(camera);
        if (!vehicleActive || !CPlayer::getLocalPlayerPed || !CTimer::m_frameCount)
            return result;
        // another view put in by the game (a new car, a script): first person is left with the hood
        if (*reinterpret_cast<const int8_t*>(camera + VehicleViewOffset) != HoodView)
        {
            vehicleActive = false;
            return result;
        }
        if (!*reinterpret_cast<const uintptr_t*>(camera + VehicleOffset))
            return result;
        const auto pedMatrix = CEntity::GetMatrix(CPlayer::getLocalPlayerPed());
        if (!pedMatrix)
            return result;

        Ped handle = 0;
        Natives::GetPlayerChar(Natives::GetPlayerId(), &handle);
        if (!handle || !Natives::IsCharSittingInAnyCar(handle) || Natives::IsCharGettingInToACar(handle))
            return result;
        Vehicle vehicle = 0;
        Natives::GetCarCharIsUsing(handle, &vehicle);
        float speed = 0.0f;
        uint32_t model = 0;
        if (vehicle)
        {
            Natives::GetCarSpeed(vehicle, &speed);
            Natives::GetCarModel(vehicle, &model);
        }

        const uint32_t frame = *CTimer::m_frameCount;
        seatTracker.Push(pedMatrix, frame);
        float head[3]{};
        if (!HeadBonePosition(handle, head))
            return result;
        seatTracker.Update(head, model, Natives::IsCharOnAnyBike(handle), speed, seat);
        seatFrame = frame;
        return result;
    }

    // CCamFinal hands its matrix to the viewport (CE 0xC462FE, the matrix pushed for 0x4319B0, its
    // position at +0x30, built from the final camera's frame in esi): the eye goes in there, the
    // view as rendered, whichever cameras the follow camera's position went through on the way.
    // In a vehicle the head is placed with the ped's matrix as it is now. The near clip (esi+54, read
    // right after) is raised.
    static void PlaceFinal(SafetyHookContext& regs)
    {
        const auto matrix = *reinterpret_cast<float**>(regs.esp);
        if (!matrix) return;
        const auto cam = reinterpret_cast<float*>(regs.esi);
        if (SeatCurrent())
        {
            const auto m = CPlayer::getLocalPlayerPed ? CEntity::GetMatrix(CPlayer::getLocalPlayerPed()) : nullptr;
            if (!m) return;
            const float* front = cam + 4;
            const float* up = cam + 8;
            float position[3];
            for (int i = 0; i < 3; ++i)
                position[i] = m[12 + i] + m[i] * seat[0] + m[4 + i] * seat[1] + m[8 + i] * seat[2] +
                    front[i] * forwardOffset + up[i] * upOffset;
            std::copy(std::begin(position), std::end(position), matrix + 12);
            std::copy(std::begin(position), std::end(position), cam + 12);
        }
        else if (EyeCurrent())
        {
            std::copy(std::begin(eye), std::end(eye), matrix + 12);
            std::copy(std::begin(eye), std::end(eye), cam + 12);
        }
        else
            return;
        auto& nearClip = cam[21];
        if (nearClip > 0.0f && nearClip < NearClip)
            nearClip = NearClip;
    }

    // The player's head, hair, teeth and face are not drawn while the eye is in them, and drawn again
    // as soon as it is not.
    static void HideHead()
    {
        static bool hidden = false;
        const bool hide = EyeCurrent() || SeatCurrent();
        if (hide == hidden) return;
        hidden = hide;
        for (const int32_t component : { 0, 7, 9, 10 })
            Natives::SetDrawPlayerComponent(component, !hide);
    }
}

class FirstPersonCamera
{
public:
    FirstPersonCamera()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            const bool onFoot = iniReader.ReadInteger("MISC", "FirstPersonOnFoot", 1) != 0;
            const bool inVehicle = iniReader.ReadInteger("MISC", "FirstPersonInVehicle", 1) != 0;
            if (!onFoot && !inVehicle)
                return;
            // At startup and on Ctrl+Shift+F10 (IniReload)
            static auto readOffsets = [](CIniReader& ini)
            {
                FirstPerson::forwardOffset = std::clamp(ini.ReadFloat("MISC", "FirstPersonForward", 0.1f), -0.5f, 0.5f);
                FirstPerson::upOffset = std::clamp(ini.ReadFloat("MISC", "FirstPersonUp", 0.05f), -0.5f, 0.5f);
            };
            readOffsets(iniReader);
            IniReload::Add(readOffsets);

            // lea eax, [esp+30] / push eax / mov ecx, ebx / call 0x4319B0 / movss xmm0, [esi+54]
            auto finalSite = hook::pattern("8D 44 24 30 50 8B CB E8 ? ? ? ? F3 0F 10 46 54");
            if (finalSite.empty())
                return;
            FirstPerson::finalHook = safetyhook::create_mid(finalSite.get_first(7), FirstPerson::PlaceFinal);

            if (onFoot)
            {
                // inc [ecx+360] / pop ebx / mov eax, [ecx+360] / test eax, eax / jg
                auto zoom = hook::pattern("FF 81 60 03 00 00 5B 8B 81 60 03 00 00 85 C0 7F");
                auto update = hook::pattern("55 8B EC 83 E4 F0 81 EC F4 00 00 00 53 56 8B F1 57 C7 46 74 00 00 00 00");
                if (!zoom.empty() && !update.empty())
                {
                    FirstPerson::zoomHook = safetyhook::create_mid(zoom.get_first(7), FirstPerson::StepZoom);
                    FirstPerson::updateHook = safetyhook::create_inline(update.get_first(0), FirstPerson::Update);
                }
            }

            if (inVehicle)
            {
                // movsx eax, byte [esi+22C] / cmp eax, 5 / ja
                auto view = hook::pattern("0F BE 86 2C 02 00 00 83 F8 05 0F 87");
                // CCamFollowVehicle::Update: push ebp / mov ebp, esp / and esp, -10 / sub esp, 104 / ...
                auto update = hook::pattern("55 8B EC 83 E4 F0 81 EC 04 01 00 00 53 56 57 8B F9 8B 87 2C 01 00 00 80 8F 3D 01 00 00 02");
                if (!view.empty() && !update.empty())
                {
                    FirstPerson::vehicleViewHook = safetyhook::create_mid(view.get_first(0), FirstPerson::StepVehicleView);
                    FirstPerson::vehicleUpdateHook = safetyhook::create_inline(update.get_first(0), FirstPerson::VehicleUpdate);
                }
            }
            FusionFix::onGameProcessEvent() += FirstPerson::HideHead;
        };
    }
} FirstPersonCamera;
