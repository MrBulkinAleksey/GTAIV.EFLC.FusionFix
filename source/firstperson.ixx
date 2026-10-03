module;

#include <common.hxx>

export module firstperson;

import common;
import comvars;
import natives;

// First person on foot, as a fourth step of the on-foot camera views. INPUT_NEXT_CAMERA steps the
// zoom of CCamFollowPed (+0x360) through 1, 2 and 3 (CE 0xA21A1C); after 3 the view now goes to
// first person, with the zoom held at 1 underneath, and the next press back to 1. The follow
// camera keeps working as it does (look, aim, its collision), only its position is moved to the
// player's head once it is done, and the player is left out of the camera's scene, his shadows and
// reflections kept.
namespace FirstPerson
{
    static bool active = false;
    static float forwardOffset = 0.1f;
    static float upOffset = 0.05f;
    // Where the follow camera's update put the view this frame, and the frame it did: the final
    // camera takes it and the player is hidden only while it is current, so he shows again once
    // another camera takes over (aiming, cars, cutscenes).
    static float eye[3]{};
    static uint32_t eyeFrame = 0;
    static constexpr uint32_t HeadBone = 0x4B5; // the bone the game itself asks for most, the head
    static constexpr float HeadAbovePed = 0.6f;   // fallback without the bone, above the ped's origin
    static constexpr uintptr_t ZoomOffset = 0x360, TargetOffset = 0x384;

    static SafetyHookMid zoomHook;
    static SafetyHookInline updateHook;
    static SafetyHookMid finalHook;

    static bool EyeCurrent() noexcept
    {
        return active && CTimer::m_frameCount && *CTimer::m_frameCount - eyeFrame <= 1;
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
    // the view and up. The camera's frame at +0x10 holds right, front and up, then the position at
    // +0x40, which is moved too.
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
        Vector3 bone{};
        if (handle)
            Natives::GetPedBonePosition(handle, HeadBone, 0.0f, 0.0f, 0.0f, &bone);
        if (std::isfinite(bone.fX) && std::isfinite(bone.fY) && std::isfinite(bone.fZ) &&
            (bone.fX != 0.0f || bone.fY != 0.0f || bone.fZ != 0.0f))
        {
            head[0] = bone.fX; head[1] = bone.fY; head[2] = bone.fZ;
        }
        else
        {
            if (!CEntity::GetPosition(ped, head)) return result;
            head[2] += HeadAbovePed;
        }

        auto frame = reinterpret_cast<float*>(camera + 0x10);
        const float* front = frame + 4;
        const float* up = frame + 8;
        for (int i = 0; i < 3; ++i)
            eye[i] = head[i] + front[i] * forwardOffset + up[i] * upOffset;
        std::copy(std::begin(eye), std::end(eye), frame + 12);
        eyeFrame = *CTimer::m_frameCount;
        return result;
    }

    // CCamFinal hands its matrix to the viewport (CE 0xC462FE, the matrix pushed for 0x4319B0, its
    // position at +0x30, built from the final camera's frame in esi): the eye goes in there, the
    // view as rendered, whichever cameras the follow camera's position went through on the way.
    static void PlaceFinal(SafetyHookContext& regs)
    {
        const auto matrix = *reinterpret_cast<float**>(regs.esp);
        if (!matrix) return;
        if (!EyeCurrent()) return;
        std::copy(std::begin(eye), std::end(eye), matrix + 12);
        std::copy(std::begin(eye), std::end(eye), reinterpret_cast<float*>(regs.esi) + 12);
    }

    static bool SkipInScene(void* entity)
    {
        return EyeCurrent() && CPlayer::getLocalPlayerPed &&
            reinterpret_cast<uintptr_t>(entity) == CPlayer::getLocalPlayerPed();
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
            if (iniReader.ReadInteger("MISC", "FirstPersonOnFoot", 1) == 0)
                return;
            FirstPerson::forwardOffset = std::clamp(iniReader.ReadFloat("MISC", "FirstPersonForward", 0.1f), -0.5f, 0.5f);
            FirstPerson::upOffset = std::clamp(iniReader.ReadFloat("MISC", "FirstPersonUp", 0.05f), -0.5f, 0.5f);

            // inc [ecx+360] / pop ebx / mov eax, [ecx+360] / test eax, eax / jg
            auto zoom = hook::pattern("FF 81 60 03 00 00 5B 8B 81 60 03 00 00 85 C0 7F");
            auto update = hook::pattern("55 8B EC 83 E4 F0 81 EC F4 00 00 00 53 56 8B F1 57 C7 46 74 00 00 00 00");
            if (zoom.empty() || update.empty())
                return;
            FirstPerson::zoomHook = safetyhook::create_mid(zoom.get_first(7), FirstPerson::StepZoom);
            FirstPerson::updateHook = safetyhook::create_inline(update.get_first(0), FirstPerson::Update);
            // lea eax, [esp+30] / push eax / mov ecx, ebx / call 0x4319B0 / movss xmm0, [esi+54]
            auto finalSite = hook::pattern("8D 44 24 30 50 8B CB E8 ? ? ? ? F3 0F 10 46 54");
            if (!finalSite.empty())
                FirstPerson::finalHook = safetyhook::create_mid(finalSite.get_first(7), FirstPerson::PlaceFinal);
            CRenderPhaseDeferredLighting_SceneToGBuffer::SkipEntity = FirstPerson::SkipInScene;
        };
    }
} FirstPersonCamera;
