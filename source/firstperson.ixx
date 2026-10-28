module;

#include <common.hxx>
#include <chrono>
#include "FusionLog.hpp"

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
// ped's matrix as it is when the final camera is built. The hood view's look around is clamped to
// +-74.5 degrees to the sides and -19..57 up and down (CE 0xA17131); in first person it goes to 150 to
// the sides, over the shoulder, and 60 up and down.
//
// Aiming on foot in first person keeps it: the aim camera (CCamAimWeapon) still turns the view, as the
// bullets go along its ray, but its pull over the right shoulder (a target offset it eases towards each
// frame, x 0.475 m, CE 0xA27B71) is held at 0, so the ray runs past the head, and the eye goes on that
// ray where it passes the head: the crosshair stays the bullet line.
//
// In both the head, hair and face are left out of the camera's scene, their shadows and reflections
// kept, and the near clip is raised, which cuts away the neck and shoulders around the eye. The game
// keeps whether a player component is drawn in a byte each (SET_DRAW_PLAYER_COMPONENT, CE 0x150E0DC):
// IV reads them as the player is drawn (CE 0xAB7EBF), so commands around his draws in the camera's
// scene clear and restore them there; the episodes copy them into each player draw command as it is
// made (CE 0xBF16F4) and back when it runs, so that copy is set too, hidden in the camera's scene and
// drawn everywhere else.
namespace FirstPerson
{
    static bool active = false;
    static bool vehicleActive = false;
    static uintptr_t vehicleCamera = 0; // the vehicle camera first person was switched on in
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
    static constexpr float NearClip = 0.12f;        // in vehicles; the game's own is 0.05; hands, guns and the wheel are further
    // On foot the eye keeps within eyeReach of the ped's position across (the head bone leans out with the idle and
    // running animations, 0.18 m against a wall), and the near clip is nearClipOnFoot: the ped's collision keeps a
    // wall a capsule's radius (0.2 to 0.3 m) from its position, so the near plane, its corners some 1.35 times the
    // near clip from the eye, stays on this side. With the eye on the head and a near clip of 0.12 the view went
    // through walls ahead and to the side. The reach grows with the ped's speed, from reachStill below reachSlowSpeed
    // to reachRunning above reachFastSpeed (0.2 and 1.5 m/s: from 1 m/s the collar showed as the run began): running in the open the head leans out over 0.2 m and the eye held back
    // looked down onto his back, while running into a wall the run plays with the ped all but still. Hidden, read
    // again live: FirstPersonEyeReach, FirstPersonEyeReachRunning, FirstPersonReachSlowSpeed, FirstPersonReachFastSpeed,
    // FirstPersonNearClip.
    static float reachStill = 0.1f, reachRunning = 0.35f;
    static float reachSlowSpeed = 0.2f, reachFastSpeed = 1.5f; // m/s
    static constexpr float ReachEase = 0.2f;    // of the way to the speed's reach each frame
    static float eyeReach = 0.1f;               // this frame's
    static bool reachByMotion = true;
    static float motionSpeed = 0.0f;            // across, from the ped's position frame to frame (m/s)
    static float nearClipOnFoot = 0.05f;
    static constexpr float LookDownForward = 0.2f;  // m the eye moves out over the chest looking straight down
    static constexpr float LookDownUp = 0.03f;
    static float lookDownWeight = 0.0f;              // eases to 0 while the phone is in the hand
    static constexpr float LearnBelowSpeed = 1.5f;  // m/s: below it the head bone's lag is a few cm
    static constexpr uintptr_t LookPitchOffset = 0x1B0, LookYawOffset = 0x1B4;
    static constexpr float LookYawLimit = 2.618f;   // radians, 150 degrees
    static constexpr float LookPitchLimit = 1.047f; // 60 degrees
    static float look[2]{};                         // pitch and yaw before the hood view's clamp

    static SafetyHookMid zoomHook;
    static SafetyHookInline updateHook;
    static SafetyHookMid finalHook;
    static SafetyHookMid vehicleViewHook;
    static SafetyHookInline vehicleUpdateHook;
    static SafetyHookMid componentCopyHook;
    static SafetyHookMid lookTakeHook;
    static SafetyHookInline aimUpdateHook;
    static SafetyHookMid aimShoulderHook;
    static SafetyHookMid swayHook;
    static SafetyHookInline idleHook;
    static uintptr_t aimCamera = 0;   // the aim camera being updated in first person
    static uint32_t aimFrame = 0;     // the frame it put the eye
    static SafetyHookMid lookClampHook;
    static uint8_t* drawComponents = nullptr;          // the game's 16 player component draw bytes
    static constexpr int32_t HeadComponents[] = { 0, 7, 9, 10 }; // head, hair, teeth, face
    static bool buildingHiddenHead = false;            // the player's draws in the camera's scene are being made

    static bool EyeCurrent() noexcept
    {
        return active && CTimer::m_frameCount && *CTimer::m_frameCount - eyeFrame <= 1;
    }

    static bool SeatCurrent() noexcept
    {
        return vehicleActive && CTimer::m_frameCount && *CTimer::m_frameCount - seatFrame <= 1;
    }

    static bool HeadBonePosition(uintptr_t ped, float (&head)[3])
    {
        if (!CPed::GetBoneMatrix || !ped) return false;
        float m[16]{};
        CPed::GetBoneMatrix(ped, nullptr, m, HeadBone);
        if (!std::isfinite(m[12]) || !std::isfinite(m[13]) || !std::isfinite(m[14]) ||
            (m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f))
            return false;
        head[0] = m[12]; head[1] = m[13]; head[2] = m[14];
        return true;
    }

    // A line now and then while first person is on, to tell from a log what it follows.
    static bool LogDue(uint32_t& last)
    {
        const uint32_t frame = CTimer::m_frameCount ? *CTimer::m_frameCount : 0;
        if (last && frame - last < 120) return false;
        last = frame;
        return true;
    }

    // Standing still, the follow camera turns around the player while he keeps facing where he did: in first
    // person the view then went over his shoulder, and looking down (the eye moved out along the view) behind his
    // head into his collar. Once the view is TurnFrom off his heading he turns after it at TurnRate until it is
    // within TurnUntil, only while nothing else turns him (walking, cover, a ragdoll, swimming, no control; the
    // aim camera turns him itself).
    static constexpr float TurnFrom = 95.0f;    // degrees
    static constexpr float TurnUntil = 15.0f;
    static constexpr float TurnRate = 220.0f;   // degrees a second
    static constexpr float TurnBelowSpeed = 0.5f;
    static bool turning = false;
    static int32_t turnTime = 0;

    static void TurnToView(Ped handle, const float* front)
    {
        const int32_t now = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds : 0;
        const float seconds = std::clamp((now - turnTime) / 1000.0f, 0.0f, 0.1f);
        turnTime = now;
        float speed = 0.0f;
        if (handle)
            Natives::GetCharSpeed(handle, &speed);
        const bool free = handle && speed < TurnBelowSpeed &&
            Natives::IsPlayerControlOn(Natives::ConvertIntToPlayerIndex(Natives::GetPlayerId())) &&
            !Natives::IsPedInCover(handle) && !Natives::IsPedRagdoll(handle) && !Natives::IsCharSwimming(handle);
        const float level = std::sqrt(front[0] * front[0] + front[1] * front[1]);
        if (!free || level < 1e-3f)
        {
            turning = false;
            return;
        }
        // (the game's heading: 0 faces +y, growing counterclockwise)
        const float view = std::atan2(-front[0], front[1]) * 57.29578f;
        float heading = 0.0f;
        Natives::GetCharHeading(handle, &heading);
        const float off = std::remainder(view - heading, 360.0f);
        if (!turning && std::abs(off) > TurnFrom)
        {
            turning = true;
            FusionLog::Write("FirstPerson", "OnFoot", "turning to the view, %.0f degrees off\n", off);
        }
        if (!turning)
            return;
        const float step = std::min(TurnRate * seconds, std::abs(off));
        Natives::SetCharHeading(handle, heading + std::copysign(step, off));
        if (std::abs(off) - step < TurnUntil)
            turning = false;
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
                FusionLog::Write("FirstPerson", "OnFoot", "off\n");
            }
        }
        else if (zoom >= 4)
        {
            active = true;
            zoom = 1;
            FusionLog::Write("FirstPerson", "OnFoot", "on\n");
        }
    }

    // The eye kept within eyeReach of the ped's position across: moved back along `along` (the aim ray, so the eye
    // stays on it) as far as that takes it, else straight in towards the position.
    static void KeepWithinReach(uintptr_t ped, float (&p)[3], const float* along)
    {
        float root[3]{};
        if (!CEntity::GetPosition(ped, root))
            return;
        const float d[2] = { p[0] - root[0], p[1] - root[1] };
        const float out = d[0] * d[0] + d[1] * d[1];
        if (out <= eyeReach * eyeReach)
            return;
        if (along)
        {
            // |d - t a| = eyeReach across, the nearest t >= 0
            const float aa = along[0] * along[0] + along[1] * along[1];
            const float da = d[0] * along[0] + d[1] * along[1];
            const float disc = da * da - aa * (out - eyeReach * eyeReach);
            if (aa > 1e-4f && disc >= 0.0f)
            {
                const float t = (da - std::sqrt(disc)) / aa;
                if (t >= 0.0f)
                {
                    for (int i = 0; i < 3; ++i)
                        p[i] -= along[i] * t;
                    return;
                }
            }
        }
        const float scale = eyeReach / std::sqrt(out);
        p[0] = root[0] + d[0] * scale;
        p[1] = root[1] + d[1] * scale;
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
        // aiming: the aim camera puts the eye this frame
        if (aimFrame == *CTimer::m_frameCount)
            return result;

        float head[3]{};
        const bool bone = HeadBonePosition(ped, head);
        if (!bone)
        {
            if (!CEntity::GetPosition(ped, head)) return result;
            head[2] += HeadAbovePed;
        }
        Ped handle = 0;
        Natives::GetPlayerChar(Natives::GetPlayerId(), &handle);
        float speed = 0.0f;
        if (handle)
            Natives::GetCharSpeed(handle, &speed);
        // How fast the ped really goes across, from its position frame to frame: running into a wall the game's speed
        // stayed up for half a second while the ped stood, and the eye leaned through the wall with the head
        // (FirstPersonReachByMotion, hidden, 1; 0 takes the game's speed)
        {
            static float lastPosition[2]{};
            static auto lastTime = std::chrono::steady_clock::now();
            static float moved = 0.0f;
            float position[3]{};
            const auto now = std::chrono::steady_clock::now();
            const float seconds = std::chrono::duration<float>(now - lastTime).count();
            if (CEntity::GetPosition(ped, position) && seconds > 0.0f)
            {
                const float dx = position[0] - lastPosition[0], dy = position[1] - lastPosition[1];
                const float step = std::sqrt(dx * dx + dy * dy) / seconds;
                moved = seconds > 0.25f || step > 50.0f ? 0.0f : moved + (step - moved) * std::min(seconds * 15.0f, 1.0f);
                lastPosition[0] = position[0];
                lastPosition[1] = position[1];
                lastTime = now;
            }
            motionSpeed = moved;
            if (reachByMotion)
                speed = std::min(speed, moved);
        }
        const float running = std::clamp((speed - reachSlowSpeed) / std::max(reachFastSpeed - reachSlowSpeed, 0.1f), 0.0f, 1.0f);
        eyeReach += (reachStill + (reachRunning - reachStill) * running - eyeReach) * ReachEase;

        int32_t phoneTask = 0;
        const bool phone = handle && (Natives::GetMobilePhoneTaskSubTask(handle, &phoneTask) || Natives::IsMobilePhoneCallOngoing());
        lookDownWeight += ((phone ? 0.0f : 1.0f) - lookDownWeight) * 0.1f;

        auto frame = reinterpret_cast<float*>(camera + 0x10);
        const float* front = frame + 4;
        TurnToView(handle, front);
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
        KeepWithinReach(ped, eye, nullptr);
        // the follow camera lowers its pivot and target by its duck offset (+0x2C8, eased to -0.55 m, CE 0xA1D990)
        static uint32_t logFrame = 0;
        static bool wasDucking = false;
        const bool ducking = handle && Natives::IsCharDucking(handle);
        if (LogDue(logFrame) || ducking != wasDucking)
        {
            wasDucking = ducking;
            float position[3]{};
            CEntity::GetPosition(ped, position);
            const float ahead = std::sqrt((head[0] - position[0]) * (head[0] - position[0]) + (head[1] - position[1]) * (head[1] - position[1]));
            const float eyeAhead = std::sqrt((eye[0] - position[0]) * (eye[0] - position[0]) + (eye[1] - position[1]) * (eye[1] - position[1]));
            FusionLog::Write("FirstPerson", "OnFoot", "%s: head %s z %.2f above the ped %.2f, %.2f across; eye z %.2f, %.2f across (reach %.2f at %.1f m/s, moving %.1f m/s), view pitch %.1f, duck offset %.2f\n",
                ducking ? "ducking" : "standing", bone ? "bone" : "fallback", head[2], head[2] - position[2], ahead, eye[2], eyeAhead,
                eyeReach, speed, motionSpeed, std::asin(std::clamp(front[2], -1.0f, 1.0f)) * 57.29578f, *reinterpret_cast<const float*>(camera + 0x2C8));
        }
        std::copy(std::begin(eye), std::end(eye), frame + 12);
        eyeFrame = *CTimer::m_frameCount;
        return result;
    }

    // CCamAimWeapon::Update (vtable slot 4), on foot in first person: the eye goes on the aim ray where
    // it passes the head, a little ahead along it.
    static uint32_t __fastcall AimUpdate(uintptr_t camera, void*)
    {
        aimCamera = 0;
        const auto ped = active && CPlayer::getLocalPlayerPed && CTimer::m_frameCount ? CPlayer::getLocalPlayerPed() : 0;
        Ped handle = 0;
        if (ped)
            Natives::GetPlayerChar(Natives::GetPlayerId(), &handle);
        if (handle && !Natives::IsCharInAnyCar(handle))
            aimCamera = camera;
        const auto result = aimUpdateHook.unsafe_thiscall<uint32_t>(camera);
        if (aimCamera != camera)
            return result;
        aimCamera = 0;

        float head[3]{};
        if (!HeadBonePosition(ped, head))
            return result;
        auto frame = reinterpret_cast<float*>(camera + 0x10);
        const float* front = frame + 4;
        float* position = frame + 12;
        float along = forwardOffset;
        for (int i = 0; i < 3; ++i)
            along += (head[i] - position[i]) * front[i];
        for (int i = 0; i < 3; ++i)
            eye[i] = position[i] + front[i] * along;
        KeepWithinReach(ped, eye, front);
        std::copy(std::begin(eye), std::end(eye), position);
        eyeFrame = aimFrame = *CTimer::m_frameCount;
        static uint32_t logFrame = 0;
        if (LogDue(logFrame))
            FusionLog::Write("FirstPerson", "Aim", "eye %.2f m along the aim ray, %.2f m from the head\n", along,
                std::sqrt((eye[0] - head[0]) * (eye[0] - head[0]) + (eye[1] - head[1]) * (eye[1] - head[1]) + (eye[2] - head[2]) * (eye[2] - head[2])));
        return result;
    }

    // Where the aim camera stores the shoulder offset it eases towards (CE 0xA27B69, x in xmm6), edi the
    // camera: over the head instead of the right shoulder.
    static void HoldShoulder(SafetyHookContext& regs)
    {
        if (aimCamera && regs.edi == aimCamera)
            regs.xmm6.f32[0] = 0.0f;
    }

    // Where the follow camera hands its hand-held shake strength to its CHandShaker (CE 0xA20E7B, the
    // strength in xmm0, eased per the ped's motion from the table at 0xE9B270), edi the camera. The shake
    // turns the game camera's view, the sway seen standing still; in first person it is held at 0 (the
    // shaker runs on, so the shake eases back in behind the player).
    static void HoldSway(SafetyHookContext& regs)
    {
        if (EyeCurrent() && CPlayer::getLocalPlayerPed &&
            *reinterpret_cast<const uintptr_t*>(regs.edi + TargetOffset) == CPlayer::getLocalPlayerPed())
            regs.xmm0.f32[0] = 0.0f;
    }

    // Whether the game camera may go to its idle camera (CCamIdle, type 12), asked each frame by the
    // camera that makes it once the player is idle long enough (CE 0xB08AF7): not in first person. On
    // no that camera also drops an idle camera already made, so the view comes back at once.
    static bool __fastcall MayGoIdle(uintptr_t camera, void*)
    {
        if (active || vehicleActive)
            return false;
        return idleHook.unsafe_thiscall<bool>(camera);
    }

    // Before the vehicle camera steps its view on a press (movsx eax, byte [esi+22C]), esi the camera.
    // From the hood the press goes to first person: the view is set back by one, so the step lands on
    // the hood again (and centres the look, as it does there); the next press goes on from the hood.
    static void StepVehicleView(SafetyHookContext& regs)
    {
        auto& view = *reinterpret_cast<int8_t*>(regs.esi + VehicleViewOffset);
        if (vehicleActive && regs.esi == vehicleCamera)
        {
            vehicleActive = false;
            FusionLog::Write("FirstPerson", "Vehicle", "off (camera %08X)\n", uint32_t(regs.esi));
        }
        else if (view == HoodView)
        {
            vehicleActive = true;
            vehicleCamera = regs.esi;
            view = HoodView - 1;
            FusionLog::Write("FirstPerson", "Vehicle", "on (camera %08X)\n", uint32_t(regs.esi));
        }
    }

    // The hood view's look angles once they took this frame's mouse and stick, before its clamp stores
    // them (CE 0xA1715B, the clamped pitch in xmm1, both still as they were in the camera), esi the
    // camera. Not earlier: RawInput looks for the bytes from 0xA17131 to 0xA17150.
    static void TakeLook(SafetyHookContext& regs)
    {
        look[0] = *reinterpret_cast<const float*>(regs.esi + LookPitchOffset);
        look[1] = *reinterpret_cast<const float*>(regs.esi + LookYawOffset);
    }

    // After the clamp (CE 0xA1718D): the pitch is stored, the yaw in xmm1 is stored right after. In first
    // person both are clamped again, wider.
    static void ClampLook(SafetyHookContext& regs)
    {
        if (!vehicleActive || *reinterpret_cast<const int8_t*>(regs.esi + VehicleViewOffset) != HoodView)
            return;
        *reinterpret_cast<float*>(regs.esi + LookPitchOffset) = std::clamp(look[0], -LookPitchLimit, LookPitchLimit);
        regs.xmm1.f32[0] = std::clamp(look[1], -LookYawLimit, LookYawLimit);
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
    static constexpr float SeatEase = 0.25f;     // of the way to the tracker's head each frame
    static constexpr float SeatJumpLog = 0.02f;  // m: a frame's move of the tracker's head that is logged

    // CCamFollowVehicle::Update (vtable slot 4), then, in first person, where the head sits in the
    // player's frame: only while he sits in a car the camera follows, not while getting in or out.
    static uint32_t __fastcall VehicleUpdate(uintptr_t camera, void*)
    {
        const auto result = vehicleUpdateHook.unsafe_thiscall<uint32_t>(camera);
        if (!vehicleActive || camera != vehicleCamera || !CPlayer::getLocalPlayerPed || !CTimer::m_frameCount)
            return result;
        static uint32_t logFrame = 0;
        const bool logDue = LogDue(logFrame);
        auto skip = [&](const char* why)
        {
            if (logDue) FusionLog::Write("FirstPerson", "Vehicle", "not placed: %s\n", why);
            return result;
        };
        // another view put in by the game (a new car, a script): first person is left with the hood
        const auto view = *reinterpret_cast<const int8_t*>(camera + VehicleViewOffset);
        if (view != HoodView)
        {
            vehicleActive = false;
            FusionLog::Write("FirstPerson", "Vehicle", "off: the camera went to view %d\n", view);
            return result;
        }
        if (!*reinterpret_cast<const uintptr_t*>(camera + VehicleOffset))
            return skip("the camera has no vehicle");
        const auto ped = CPlayer::getLocalPlayerPed();
        const auto pedMatrix = CEntity::GetMatrix(ped);
        if (!pedMatrix)
            return skip("no player matrix");

        Ped handle = 0;
        Natives::GetPlayerChar(Natives::GetPlayerId(), &handle);
        if (!handle)
            return skip("no player handle");
        if (!Natives::IsCharSittingInAnyCar(handle))
            return skip("the player is not sitting in a car");
        if (Natives::IsCharGettingInToACar(handle))
            return skip("the player is getting in");
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
        if (!HeadBonePosition(ped, head))
            return skip("no head bone");
        // The head in the ped's frame as the tracker has it this frame, then eased into the seat the eye is put
        // at: the car's own motion is in the ped's matrix, so this holds back only what the head itself does
        // (the driving animation, a pick of another of the tracker's frames before the model is learned).
        float raw[3]{};
        const bool learned = seatTracker.learned.contains(model);
        seatTracker.Update(head, model, Natives::IsCharOnAnyBike(handle), speed, raw);
        static float lastRaw[3]{};
        if (frame - seatFrame > 4)
        {
            std::copy(std::begin(raw), std::end(raw), seat);
            std::copy(std::begin(raw), std::end(raw), lastRaw);
        }
        float jump = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            jump += (raw[i] - lastRaw[i]) * (raw[i] - lastRaw[i]);
            seat[i] += (raw[i] - seat[i]) * SeatEase;
        }
        jump = std::sqrt(jump);
        static uint32_t jumpLogFrame = 0;
        if (jump > SeatJumpLog && frame - jumpLogFrame >= 10)
        {
            jumpLogFrame = frame;
            FusionLog::Write("FirstPerson", "Vehicle", "head jumped %.3f m in the ped's frame (%s, speed %.1f): %.3f %.3f %.3f -> %.3f %.3f %.3f\n",
                jump, learned ? "learned" : "not learned yet", speed, lastRaw[0], lastRaw[1], lastRaw[2], raw[0], raw[1], raw[2]);
        }
        std::copy(std::begin(raw), std::end(raw), lastRaw);
        seatFrame = frame;
        if (logDue)
            FusionLog::Write("FirstPerson", "Vehicle", "model %08X speed %.1f, head in the ped's frame %.2f %.2f %.2f%s\n",
                model, speed, seat[0], seat[1], seat[2], seatTracker.learned.contains(model) ? " (learned)" : "");
        return result;
    }

    // CCamFinal hands its matrix to the viewport (CE 0xC462FE, the matrix pushed for 0x4319B0, its
    // position at +0x30, built from the final camera's frame in esi): the eye goes in there, the
    // view as rendered, whichever cameras the follow camera's position went through on the way.
    // In a vehicle the head is placed with the ped's matrix as it is now. The near clip (esi+54, read
    // right after) is set when under NearClip: NearClip in a vehicle, nearClipOnFoot on foot.
    // The radar's map is drawn from a camera of its own (CAM_RADAR, CE 0xD58550) held above the player
    // at the radar's range (50 to 960 m) and looking down, and it is handed on here too: only a camera
    // already near the head is moved, or the map is seen from the head, so near the ground it is blank.
    static constexpr float PlaceWithin = 15.0f;
    static void PlaceFinal(SafetyHookContext& regs)
    {
        const auto matrix = *reinterpret_cast<float**>(regs.esp);
        if (!matrix) return;
        const auto cam = reinterpret_cast<float*>(regs.esi);
        float position[3];
        const bool seated = SeatCurrent();
        if (seated)
        {
            const auto m = CPlayer::getLocalPlayerPed ? CEntity::GetMatrix(CPlayer::getLocalPlayerPed()) : nullptr;
            if (!m) return;
            const float* front = cam + 4;
            const float* up = cam + 8;
            for (int i = 0; i < 3; ++i)
                position[i] = m[12 + i] + m[i] * seat[0] + m[4 + i] * seat[1] + m[8 + i] * seat[2] +
                    front[i] * forwardOffset + up[i] * upOffset;
        }
        else if (EyeCurrent())
            std::copy(std::begin(eye), std::end(eye), position);
        else
            return;
        float distance = 0.0f;
        for (int i = 0; i < 3; ++i)
            distance += (cam[12 + i] - position[i]) * (cam[12 + i] - position[i]);
        if (distance > PlaceWithin * PlaceWithin)
            return;
        std::copy(std::begin(position), std::end(position), matrix + 12);
        std::copy(std::begin(position), std::end(position), cam + 12);
        auto& nearClip = cam[21];
        if (nearClip > 0.0f && nearClip < NearClip)
            nearClip = seated ? NearClip : nearClipOnFoot;
    }

    static bool HeadHidden() noexcept
    {
        return EyeCurrent() || SeatCurrent();
    }

    static bool IsHeadComponent(int32_t component) noexcept
    {
        return std::find(std::begin(HeadComponents), std::end(HeadComponents), component) != std::end(HeadComponents);
    }

    // Runs right before (hide) and after the player's draws in the camera's scene.
    class CHeadDC : public CBaseDC
    {
    public:
        bool hide;
        static inline uint8_t saved[std::size(HeadComponents)]{};

        CHeadDC(bool h) : CBaseDC(), hide(h) {}

        void DrawCommand() override
        {
            for (size_t i = 0; i < std::size(HeadComponents); ++i)
            {
                auto& draw = drawComponents[HeadComponents[i]];
                if (hide)
                {
                    saved[i] = draw;
                    draw = 0;
                }
                else
                    draw = saved[i];
            }
        }

        int32_t GetSize() override
        {
            return sizeof(CHeadDC);
        }
    };

    static void AroundEntity(void* entity, bool before)
    {
        if (!CPlayer::getLocalPlayerPed || reinterpret_cast<uintptr_t>(entity) != CPlayer::getLocalPlayerPed())
            return;
        if (before && !HeadHidden())
            return;
        if (!before && !buildingHiddenHead)
            return;
        buildingHiddenHead = before;
        if (auto dc = new CHeadDC(before))
            dc->Append();
    }

    // The episodes' copy into a player draw command as it is made (dl the byte, ecx the component).
    static void CopyComponent(SafetyHookContext& regs)
    {
        if (HeadHidden() && IsHeadComponent(static_cast<int32_t>(regs.ecx)))
            regs.edx = (regs.edx & ~0xFFu) | (buildingHiddenHead ? 0u : 1u);
    }
}

// Whether the eye is in the player's seat this frame (first person in a vehicle), for the car glass's tint.
export bool FirstPersonInVehicle() noexcept
{
    return FirstPerson::SeatCurrent();
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
                FirstPerson::reachStill = std::clamp(ini.ReadFloat("MISC", "FirstPersonEyeReach", 0.1f), 0.0f, 0.5f);
                FirstPerson::reachRunning = std::clamp(ini.ReadFloat("MISC", "FirstPersonEyeReachRunning", 0.35f), 0.0f, 0.6f);
                FirstPerson::reachSlowSpeed = std::clamp(ini.ReadFloat("MISC", "FirstPersonReachSlowSpeed", 0.2f), 0.0f, 10.0f);
                FirstPerson::reachFastSpeed = std::clamp(ini.ReadFloat("MISC", "FirstPersonReachFastSpeed", 1.5f), 0.0f, 10.0f);
                FirstPerson::reachByMotion = ini.ReadInteger("MISC", "FirstPersonReachByMotion", 1) != 0;
                FirstPerson::nearClipOnFoot = std::clamp(ini.ReadFloat("MISC", "FirstPersonNearClip", 0.05f), 0.01f, 0.3f);
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

                    // CCamAimWeapon::Update: push ebp / mov ebp, esp / and esp, -10 / sub esp, 1A4 / ... / mov byte [edi+2C4], 0
                    auto aim = hook::pattern("55 8B EC 83 E4 F0 81 EC A4 01 00 00 53 56 57 8B F9 C6 87 C4 02 00 00 00");
                    // movss xmm0, [esp+CC] / movss [edi+1DC], xmm0 / movss [edi+1D0], xmm6 / movss [edi+1D4], xmm5
                    auto shoulder = hook::pattern("F3 0F 10 84 24 ? ? ? ? F3 0F 11 87 DC 01 00 00 F3 0F 11 B7 D0 01 00 00 F3 0F 11 AF D4 01 00 00");
                    if (!aim.empty() && !shoulder.empty())
                    {
                        FirstPerson::aimUpdateHook = safetyhook::create_inline(aim.get_first(0), FirstPerson::AimUpdate);
                        FirstPerson::aimShoulderHook = safetyhook::create_mid(shoulder.get_first(9), FirstPerson::HoldShoulder);
                    }

                    // movss xmm0, [edi+2D8] / push ecx / mov ecx, esi / movss [esp], xmm0 / call CHandShaker::Process
                    auto sway = hook::pattern("F3 0F 10 87 D8 02 00 00 51 8B CE F3 0F 11 04 24 E8");
                    if (!sway.empty())
                        FirstPerson::swayHook = safetyhook::create_mid(sway.get_first(11), FirstPerson::HoldSway);
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
                // movss xmm0, [esi+1B4] / movss [esi+1B0], xmm1 / movss xmm1, [the yaw's upper limit] / comiss / jbe
                auto take = hook::pattern("F3 0F 10 86 B4 01 00 00 F3 0F 11 8E B0 01 00 00 F3 0F 10 0D ? ? ? ? 0F 2F C8 76 0F");
                // push ecx / lea eax, [esp+B4] / mov [esp], 1.0 / push eax / lea eax, [esp+A8] / push eax / push edi / mov ecx, esi / movss [esi+1B4], xmm1
                auto clamp = hook::pattern("51 8D 84 24 B4 00 00 00 C7 04 24 00 00 80 3F 50 8D 84 24 A8 00 00 00 50 57 8B CE F3 0F 11 8E B4 01 00 00");
                if (!take.empty() && !clamp.empty())
                {
                    FirstPerson::lookTakeHook = safetyhook::create_mid(take.get_first(0), FirstPerson::TakeLook);
                    FirstPerson::lookClampHook = safetyhook::create_mid(clamp.get_first(0), FirstPerson::ClampLook);
                }
            }

            // push ebp / mov ebp, esp / and esp, -10 / sub esp, 18 / cmp dword [..], 1 / push esi / push edi / mov esi, ecx / je
            auto idle = hook::pattern("55 8B EC 83 E4 F0 83 EC 18 83 3D ? ? ? ? 01 56 57 8B F1 0F 84");
            if (!idle.empty())
                FirstPerson::idleHook = safetyhook::create_inline(idle.get_first(0), FirstPerson::MayGoIdle);

            // cmp ecx, 10 / jae / mov dl, [ecx+150E0DC] / jmp / mov dl, 1 / mov al, [edi+ecx+30] / and al, 7F / shl dl, 7
            auto copy = hook::pattern("83 F9 10 73 08 8A 91 ? ? ? ? EB 02 B2 01 8A 44 0F 30 24 7F C0 E2 07");
            if (copy.empty())
                return;
            FirstPerson::drawComponents = *copy.get_first<uint8_t*>(7);
            FirstPerson::componentCopyHook = safetyhook::create_mid(copy.get_first(15), FirstPerson::CopyComponent);
            CRenderPhaseDeferredLighting_SceneToGBuffer::AroundEntity = FirstPerson::AroundEntity;
        };
    }
} FirstPersonCamera;
