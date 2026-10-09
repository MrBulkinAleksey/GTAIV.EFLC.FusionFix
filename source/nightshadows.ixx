module;

#include <common.hxx>
#include "StableHeadlightSelector.hpp"
#include "ShadowAdapterCE.hpp"
#include "ShadowAllocationCE.hpp"
#include "ShadowAllocationPass.hpp"
#include "ShadowFloatingPointState.hpp"
#include "ShadowLightGeometry.hpp"
#include "HeadlightCasterPolicy.hpp"
#include "ShadowCasterCE.hpp"
#include "ShadowGuardDiagnostics.hpp"
#include "CloseHeadlightRelevance.hpp"
#include "ShadowReach.hpp"
#include "ShadowReceiver.hpp"
#include "NearbyVehicleReceivers36.hpp"
#include "NativeLampDistance38.hpp"
#include "NativeLampContinuity41.hpp"
#include "NativeShadowContinuity42.hpp"
#include "ShadowVolumeVisibility43.hpp"
#include "ShadowCasterPresence.hpp"
#include "ShadowSlotTrace.hpp"
#include "ShadowLampSpacing.hpp"
#include "NativeCacheDependencies44.hpp"
#include "ShadowInactiveSlots.hpp"
#include "ShadowViewPriority.hpp"
#include "ShadowDrivingFocus.hpp"
#include "ShadowLookupValidation.hpp"
#include "ShadowTrace34.hpp"
#include "SubmittedHeadlightHistory.hpp"
#include "ShadowLookupLayout.hpp"
#include "ShadowCrashTrace30.hpp"
#include "DiagnosticsLog.hpp"
#include "LabSirenLifetime.hpp"
#include "LabSirenVacancy.hpp"
#include "LabPoliceClaim.hpp"
#include "NearbyPoliceShadows.hpp"
#include "TrafficSignalRegistry.hpp"
#include "TrafficSignalDistanceGate.hpp"
#include "TrafficSignalNativeQueue.hpp"
#include "FreshPageReadable.hpp"
#include "PoolDescriptorSnapshots.hpp"
#include <fstream>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <intrin.h>

export module nightshadows;

import common;
import comvars;
import natives;
import settings;

bool bHighResolutionNightShadows = false;
static bool bCloseHeadlightRelevance = false;
static bool bTrafficSelfShadowFix = false;
static float fTrafficSignalDrawScale = 1.0f;  // TrafficSignalDrawDistance
// TrafficSignalDrawDistanceLog: what the scale reached, to GTAIV.EFLC.FusionFix.NightShadows.TrafficSignals.log.
// A signal lights up in three steps, each logged with the farthest signal that got through it:
// its pre-render runs its logic (CE 0xA32853 -> 0xD208F0, phase and colour), the pre-render goes
// on to its 2dfx lights only with its drawable loaded (0xA32820 -> 0xC1DB60, faded by the entity's
// alpha +0x63 / 255), and the light made from those lands in the frame's light list (flag 0x200).
namespace TrafficSignalLog
{
    static bool enabled = false;
    static std::atomic<uint32_t> models{0}, entities{0}, placed{0};
    static std::atomic<float> farthestDistance{0.0f};
    static std::mutex firstMutex;
    static std::string first; // the first models scaled, from and to

    struct Step
    {
        std::atomic<uint32_t> count{0};
        std::atomic<float> farthest{0.0f}, farthestValue{0.0f}; // value: draw distance, fade or intensity
        // The same for signals within 20 degrees of where the camera looks, the ones you watch light up
        std::atomic<uint32_t> aheadCount{0};
        std::atomic<float> aheadFarthest{0.0f}, aheadValue{0.0f};
        void Add(float d, float value, bool ahead) noexcept
        {
            ++count;
            if (d > farthest.load(std::memory_order_relaxed)) { farthest = d; farthestValue = value; }
            if (!ahead) return;
            ++aheadCount;
            if (d > aheadFarthest.load(std::memory_order_relaxed)) { aheadFarthest = d; aheadValue = value; }
        }
        std::string Take(const char* value)
        {
            const auto n = count.exchange(0), an = aheadCount.exchange(0);
            const float d = farthest.exchange(0.0f), v = farthestValue.exchange(0.0f);
            const float ad = aheadFarthest.exchange(0.0f), av = aheadValue.exchange(0.0f);
            if (!n) return "0";
            return std::format("{} (farthest {:.1f} m, {} {:.2f}; ahead {}{})", n, d, value, v, an,
                an ? std::format(", farthest {:.1f} m, {} {:.2f}", ad, value, av) : std::string());
        }
    };
    static Step logic, lights, listed, framed, inFront, unoccluded, roadCalls, roadTaken;
    static std::atomic<uint32_t> listedFlags{0}; // of the farthest signal light ahead in the list
    static std::atomic<float> listedColour{0.0f}; // its colour's brightest channel, which the 2dfx fade scales
    // The 2dfx fade distances of signals as the effect has them (+0x60 times [0x1048230], +0x64), and how often
    static std::atomic<float> fadeFirst{0.0f}, fadeSecond{0.0f};
    static std::atomic<uint32_t> fadeSeen{0};
    static float* roadLimit = nullptr;           // [0x103AB80], the road light's distance
    // TrafficSignalLightTest: 1 draws the signals' lights without the interior bit 0x20 (they carry
    // 0x260, street lamps do not), 2 draws them dark, to tell what the light on the road is (it is
    // theirs: 2 took it away, 1 changed nothing), 3 draws those beyond 100 m three times as wide and
    // bright, to tell whether far away the road takes no light at all or only this one.
    static int lightTest = 0;
    static bool CameraFar(uintptr_t lightPlus28, float metres) noexcept;
    // The camera as the game thread last saw it, for the steps on the render thread (no natives there).
    static std::atomic<float> cameraPosition[3]{}, cameraForward[3]{};
    static std::atomic<bool> cameraKnown{false};

    // Where the camera looks, once a frame (GET_CAM_ROT: x pitch, z heading, degrees).
    static bool CameraForward(float (&forward)[3]) noexcept
    {
        static uint32_t frame = 0;
        static bool valid = false;
        static float cached[3]{};
        const uint32_t now = CTimer::m_frameCount ? *CTimer::m_frameCount : 0;
        if (!valid || !now || now != frame)
        {
            Cam camera = 0;
            Natives::GetRootCam(&camera);
            float x = 0, y = 0, z = 0;
            valid = camera != 0;
            if (valid) Natives::GetCamRot(camera, &x, &y, &z);
            constexpr float toRad = 3.14159265f / 180.0f;
            cached[0] = -std::sin(z * toRad) * std::cos(x * toRad);
            cached[1] = std::cos(z * toRad) * std::cos(x * toRad);
            cached[2] = std::sin(x * toRad);
            valid = valid && std::isfinite(cached[0]) && std::isfinite(cached[1]) && std::isfinite(cached[2]);
            frame = now;
        }
        for (int i = 0; i < 3; ++i) forward[i] = cached[i];
        return valid;
    }

    // Distance from the camera, and whether it lies within 20 degrees of where it looks.
    static bool CameraDistance(const float (&position)[3], float& d, bool& ahead) noexcept
    {
        float camera[3], forward[3];
        if (!GameCamera::Position(camera)) return false;
        const float x = position[0] - camera[0], y = position[1] - camera[1], z = position[2] - camera[2];
        d = std::sqrt(x * x + y * y + z * z);
        ahead = d > 0.01f && CameraForward(forward) && (x * forward[0] + y * forward[1] + z * forward[2]) / d > 0.94f;
        return std::isfinite(d);
    }

    static bool IsSignal(uintptr_t entity) noexcept
    {
        const auto index = *reinterpret_cast<const int16_t*>(entity + 0x2E);
        if (index < 0) return false;
        const auto model = *reinterpret_cast<const uintptr_t*>(GameBase() + 0xE95CD8 + index * 4);
        return model && (*reinterpret_cast<const uint32_t*>(model + 0x40) & 0xFF000000) == 0x3000000;
    }

    // Step 1, CE 0xA32853, esi the entity.
    static void Logic(uintptr_t entity) noexcept
    {
        float position[3], d;
        bool ahead;
        if (CEntity::GetPosition(entity, position) && CameraDistance(position, d, ahead))
            logic.Add(d, *reinterpret_cast<const float*>(entity + 0x50), ahead);
    }

    // Step 2, CE 0xA32818, esi the entity, xmm1 the fade its lights get.
    static void Lights(uintptr_t entity, float fade) noexcept
    {
        float position[3], d;
        bool ahead;
        if (IsSignal(entity) && CEntity::GetPosition(entity, position) && CameraDistance(position, d, ahead))
            lights.Add(d, fade, ahead);
    }

    // Steps 4 to 6, render thread, in the loop that draws the frame's lights (CE 0xAC1030, edi the
    // light + 0x28): its sphere in the frame (0x4B1A70), not behind the camera, not occluded (0x431E40).
    static void Drawn(Step& step, uintptr_t lightPlus28) noexcept
    {
        if (!cameraKnown.load(std::memory_order_relaxed)) return;
        const auto& light = *reinterpret_cast<const rage::CLightSource*>(lightPlus28 - 0x28);
        if ((light.mFlags & 0x201) != 0x200) return;
        const float x = light.mPosition.x - cameraPosition[0], y = light.mPosition.y - cameraPosition[1],
            z = light.mPosition.z - cameraPosition[2];
        const float d = std::sqrt(x * x + y * y + z * z);
        if (!std::isfinite(d)) return;
        const bool ahead = d > 0.01f && (x * cameraForward[0] + y * cameraForward[1] + z * cameraForward[2]) / d > 0.94f;
        step.Add(d, light.mRadius, ahead);
    }

    // The road light (CE 0x9BBC70: [esp+4] its position on entry, esi once within the distance at 0x9BBD0A).
    static void Road(Step& step, const float* position) noexcept
    {
        float camera[3], forward[3];
        if (!position || !cameraKnown.load(std::memory_order_relaxed)) return;
        for (int i = 0; i < 3; ++i) { camera[i] = cameraPosition[i]; forward[i] = cameraForward[i]; }
        const float x = position[0] - camera[0], y = position[1] - camera[1], z = position[2] - camera[2];
        const float d = std::sqrt(x * x + y * y + z * z);
        if (!std::isfinite(d)) return;
        step.Add(d, *reinterpret_cast<const float*>(GameBase() + 0xC3F6BC),
            d > 0.01f && (x * forward[0] + y * forward[1] + z * forward[2]) / d > 0.94f);
    }

    static void InstallRoadSteps()
    {
        auto entry = hook::pattern("55 8B EC 83 E4 F0 83 EC 28 80 7D 18 00 56 57 0F 84");
        if (entry.empty()) { FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance", "the road light was not found\n"); return; }
        const auto at = reinterpret_cast<uintptr_t>(entry.get_first(0));
        // 0x9BBD0A: shl eax, 6 / add eax, ecx, the entry's slot once within the distance
        if (std::memcmp(reinterpret_cast<const void*>(at + 0x9A), "\xC1\xE0\x06\x03\xC1", 5))
        { FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance", "the road light differs\n"); return; }
        static auto callHook = safetyhook::create_mid(at, [](SafetyHookContext& regs)
        {
            Road(roadCalls, *reinterpret_cast<const float* const*>(regs.esp + 4));
        });
        static auto takenHook = safetyhook::create_mid(at + 0x9A, [](SafetyHookContext& regs)
        {
            Road(roadTaken, reinterpret_cast<const float*>(regs.esi));
        });
    }

    static void InstallDrawSteps()
    {
        // call 0x4B1A70 / test al, al / je / movss xmm0, [esp+14h] / xorps xmm0, [...]
        auto pattern = hook::pattern("E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? F3 0F 10 44 24 14 0F 57 05");
        if (pattern.empty())
        {
            FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance", "the light draw loop was not found\n");
            return;
        }
        const auto at = reinterpret_cast<uintptr_t>(pattern.get_first(0));
        // +0x25 movss xmm0, [edi+2Ch] once in front; +0x64 cmp byte ptr [ebp+0Ch], 0 once not occluded
        if (std::memcmp(reinterpret_cast<const void*>(at + 0x25), "\xF3\x0F\x10\x47\x2C", 5) ||
            std::memcmp(reinterpret_cast<const void*>(at + 0x64), "\x80\x7D\x0C\x00", 4))
        {
            FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance", "the light draw loop differs\n");
            return;
        }
        static auto framedHook = safetyhook::create_mid(at + 5, [](SafetyHookContext& regs)
        {
            if (!(regs.eax & 0xFF)) return;
            // Before the loop reads the flags (+0x20) for the technique and the intensity (+0x18).
            if (lightTest && (*reinterpret_cast<const uint32_t*>(regs.edi + 0x20) & 0x201) == 0x200)
            {
                if (lightTest == 1) *reinterpret_cast<uint32_t*>(regs.edi + 0x20) &= ~0x20u;
                else if (lightTest == 2) *reinterpret_cast<float*>(regs.edi + 0x18) = 0.0f;
                else if (lightTest == 3 && CameraFar(regs.edi, 100.0f))
                {
                    *reinterpret_cast<float*>(regs.edi + 0x2C) *= 3.0f; // radius, light + 0x54
                    *reinterpret_cast<float*>(regs.edi + 0x18) *= 3.0f; // intensity, light + 0x40
                }
            }
            if (enabled) Drawn(framed, regs.edi);
        });
        if (!enabled) return;
        static auto inFrontHook = safetyhook::create_mid(at + 0x25, [](SafetyHookContext& regs) { Drawn(inFront, regs.edi); });
        static auto unoccludedHook = safetyhook::create_mid(at + 0x64, [](SafetyHookContext& regs) { Drawn(unoccluded, regs.edi); });
    }

    static bool CameraFar(uintptr_t lightPlus28, float metres) noexcept
    {
        if (!cameraKnown.load(std::memory_order_relaxed)) return false;
        const auto& light = *reinterpret_cast<const rage::CLightSource*>(lightPlus28 - 0x28);
        const float x = light.mPosition.x - cameraPosition[0], y = light.mPosition.y - cameraPosition[1],
            z = light.mPosition.z - cameraPosition[2];
        return x * x + y * y + z * z > metres * metres;
    }

    // Step 3, once a game frame: the signal lights in the list the renderer draws ([0x103EED0], count [0x154DFD0]).
    static void Tick() noexcept
    {
        if (!enabled && !lightTest) return;
        {
            float position[3], forward[3];
            const bool known = GameCamera::Position(position) && CameraForward(forward);
            for (int i = 0; i < 3; ++i) { cameraPosition[i] = position[i]; cameraForward[i] = forward[i]; }
            cameraKnown = known;
        }
        if (!enabled) return;
        const auto list = *reinterpret_cast<const rage::CLightSource* const*>(GameBase() + 0xC3EED0);
        const auto count = *reinterpret_cast<const uint32_t*>(GameBase() + 0x114DFD0);
        if (!list || count > 0x280) return;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto& light = list[i];
            if ((light.mFlags & 0x201) != 0x200) continue;
            const float position[3] = { light.mPosition.x, light.mPosition.y, light.mPosition.z };
            float d;
            bool ahead;
            if (!CameraDistance(position, d, ahead)) continue;
            if (ahead && d > listed.aheadFarthest.load(std::memory_order_relaxed)) {
                listedFlags = light.mFlags;
                listedColour = (std::max)({ light.mColor.x, light.mColor.y, light.mColor.z });
            }
            listed.Add(d, light.mIntensity, ahead);
        }
    }

    // Game thread, every two seconds.
    static void Write() noexcept
    {
        static ULONGLONG last = 0;
        if (!enabled || GetTickCount64() - last < 2000) return;
        last = GetTickCount64();
        try
        {
            std::string firstModels;
            { std::lock_guard lock(firstMutex); firstModels.swap(first); }
            FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance",
                "scale %.2f; scaled %u models, %u signals made with the model's distance, %u with their own (farthest %.1f m); "
                "last 2 s: logic %s, 2dfx lights %s, in the light list %s (a light per frame), drawn: in the frame %s, "
                "in front %s, not occluded %s; ahead's flags %08x colour %.3f; road light: called %s, within its distance %s, "
                "its distance %.1f m; 2dfx fades seen %u, distances %.1f / %.1f m before scaling%s%s\n",
                fTrafficSignalDrawScale, models.load(), entities.load(), placed.load(), farthestDistance.load(),
                logic.Take("draw distance").c_str(), lights.Take("fade").c_str(), listed.Take("intensity").c_str(),
                framed.Take("radius").c_str(), inFront.Take("radius").c_str(), unoccluded.Take("radius").c_str(),
                listedFlags.exchange(0), listedColour.exchange(0.0f), roadCalls.Take("game scale").c_str(), roadTaken.Take("game scale").c_str(),
                roadLimit ? *roadLimit : -1.0f, fadeSeen.exchange(0), fadeFirst.load(), fadeSecond.load(),
                firstModels.empty() ? "" : "; models: ", firstModels.c_str());
        }
        catch (...) {}
    }
}
#include "PlayerCarRuntime.inl"
#include "BeamTraceRuntime.inl"
#include "HeadlightEnhancementRuntime.inl"

// Queried for every light on every frame; a lookup by name scans the whole preference table.
static int32_t ShadowReachStep(bool headlight)
{
    static auto HeadlightReach = FusionFixSettings.GetRef("PREF_HEADLIGHT_REACH");
    static auto LampReach = FusionFixSettings.GetRef("PREF_LAMP_REACH");
    const auto& reach = headlight ? HeadlightReach : LampReach;
    return reach ? reach->get() : 0;
}

namespace NearbyVehicleLighting36 {
    static void Update() noexcept;
    static bool Relevant(uintptr_t player,uint32_t frame,uint32_t now,
        fusionfix::shadows::Vec3 position,fusionfix::shadows::Vec3 source,
        fusionfix::shadows::Vec3 direction,float outerCos,float radius,uintptr_t key) noexcept;
    static float Score(uintptr_t player,uint32_t frame,uint32_t now,
        fusionfix::shadows::Vec3 position,fusionfix::shadows::Vec3 source,
        fusionfix::shadows::Vec3 direction,float outerCos,float radius,uintptr_t key) noexcept;
}

namespace CShadows
{
    // CE's submission adapter takes 16 stack words. Its final word is an opaque
    // stable key (including pointer+1 values), not a dereferenceable light object.
    // Official FusionFix preserved it through a tail jump. Our nontrivial
    // wrappers must explicitly forward it; no upstream flicker cause is claimed.
    using SubmitHeadlight = void(__cdecl*)(int, int, uint32_t, int, int, int, int, int,
                                          int, int, int, int, int, int, int, int);
    injector::hook_back<SubmitHeadlight> hbStoreStaticShadow;
    static std::atomic<uint32_t> drivingBeamAccepted{0}, drivingBeamRejected{0};

    struct StableHeadlightShadow
    {
        std::mutex stateMutex;
        fusionfix::shadows::StableHeadlightSelector selector;
        fusionfix::shadows::SubmittedHeadlightHistory submitted;
        fusionfix::shadows::Vec3 playerPosition{};
        uintptr_t occupiedVehicle = 0, playerSession = 0;
        uint32_t frame = 0;
        bool hasFrame = false;
        bool playerValid = false;

        bool PrepareFrame()
        {
            if (!bHeadlightShadows || !CTimer::m_frameCount || !CTimer::m_snTimeInMilliseconds ||
                !CPlayer::getLocalPlayerPed || !CPlayer::findPlayerCar)
            {
                selector.Reset();
                submitted.Reset();
                hasFrame = false;
                return false;
            }

            // Use the game's verified frame counter. A timer tick does not
            // identify a frame when time is paused, slowed or reset.
            const uint32_t nextFrame = *CTimer::m_frameCount;
            if (hasFrame && nextFrame == frame)
                return playerValid;
            frame = nextFrame;
            hasFrame = true;
            playerValid = false;

            PlayerCar::Focus focus;
            if (!PlayerCar::ReadFocus(focus))
            {
                selector.Reset();
                submitted.Reset();
                return false;
            }
            if (focus.ped != playerSession) submitted.Reset();
            playerSession = focus.ped;
            occupiedVehicle = focus.car;
            playerPosition = {focus.position[0], focus.position[1], focus.position[2]};
            selector.BeginFrame({nextFrame, static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds),
                                 focus.ped, focus.car != 0, focus.car});
            playerValid = true;
            return true;
        }

        bool IsSubmittedBeam(uintptr_t key)
        {
            const std::lock_guard<std::mutex> lock(stateMutex);
            if (!key || !hasFrame || !playerValid) return false;
            return CTimer::m_frameCount && submitted.Contains(key, *CTimer::m_frameCount);
        }

        bool ShouldCast(int directionAddress, int positionAddress, int stableKey, int radiusBits)
        {
            // Keep policy bookkeeping coherent if render submissions are made
            // on more than one thread. The engine call occurs after unlock.
            const std::lock_guard<std::mutex> lock(stateMutex);
            if (!PrepareFrame() || !positionAddress || !stableKey)
                return false;
            // Audited ABCC50/ABCCD0: argument 4 is direction, argument 6 position.
            // Copy values now; these vectors may live on the caller's stack.
            const auto position = reinterpret_cast<const float*>(positionAddress);
            const fusionfix::shadows::Vec3 lightPosition{position[0], position[1], position[2]};
            fusionfix::shadows::Vec3 forward{};
            if (directionAddress)
            {
                const auto direction = reinterpret_cast<const float*>(directionAddress);
                forward = {direction[0], direction[1], direction[2]};
            }
            auto geometry = fusionfix::shadows::EvaluateGeometry(
                playerPosition, lightPosition, directionAddress ? &forward : nullptr);
            float sourceRadius;
            std::memcpy(&sourceRadius, &radiusBits, sizeof(sourceRadius));
            const float receiverExtent = occupiedVehicle ? 3.0f : 1.5f;
            geometry.distanceSquared = fusionfix::shadows::ReceiverDistanceSquared(playerPosition, lightPosition, receiverExtent);
            // Broad submission gate; the allocator checks the native cone.
            geometry.aimedAtPlayer = directionAddress && fusionfix::shadows::BeamTouchesReceiver(
                playerPosition, receiverExtent, lightPosition, forward, 0.70710678f, sourceRadius);
            float receiverWeight=1.0f;
            if (!occupiedVehicle) {
                if(geometry.aimedAtPlayer) receiverWeight=1.5f;
                else if(directionAddress) {
                    const float benefit=NearbyVehicleLighting36::Score(playerSession,frame,
                        static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds),playerPosition,lightPosition,
                        forward,0.70710678f,sourceRadius,static_cast<uintptr_t>(static_cast<uint32_t>(stableKey)));
                    geometry.aimedAtPlayer=benefit>0;
                    receiverWeight=1.0f+0.5f*benefit;
                }
            }
            const auto identity = static_cast<uintptr_t>(static_cast<uint32_t>(stableKey));
            const bool playerHeadlight = fusionfix::shadows::ce::IsVehicleBeam(identity,
                occupiedVehicle ? occupiedVehicle : PlayerCar::Last());
            // NPC beams must reach the receiver; source-to-ped distance is not a ten-foot gate.
            // The current/recent car may light scenery beyond Niko instead.
            const int feet = fusionfix::shadows::ShadowReachFeet(ShadowReachStep(true));
            const float configuredReach = feet > 0 ? feet * 0.3048f : 35.0f;
            const float reach = !playerHeadlight && std::isfinite(sourceRadius) && sourceRadius > 0
                ? (std::max)(configuredReach, sourceRadius) : configuredReach;
            const bool accepted = selector.Consider({identity, geometry, playerHeadlight,
                reach * reach, !playerHeadlight, receiverWeight});
            if (accepted) submitted.Record(identity, frame);
            if (playerHeadlight)
            {
                BeamTrace::Mark(BeamTrace::BeamOffered);
                if (accepted) BeamTrace::Mark(BeamTrace::ShadowKept);
                if (accepted) ++drivingBeamAccepted;
                else ++drivingBeamRejected;
            }
            return accepted;
        }
    };
    static StableHeadlightShadow gStableHeadlightShadow;

    void __cdecl StoreStaticShadowPlayerDriving(int a1, int a2, uint32_t flags,
        int direction, int tangent, int position, int a7, int a8, int a9, int a10,
        int a11, int a12, int a13, int a14, int a15, int stableKey)
    {
        if (!gStableHeadlightShadow.ShouldCast(direction, position, stableKey, a11))
            flags &= ~rage::LF_DYNAMIC_SHADOW;
        hbStoreStaticShadow.fun(a1, a2, flags, direction, tangent, position,
                                a7, a8, a9, a10, a11, a12, a13, a14, a15, stableKey);
    }

    void __cdecl StoreStaticShadowNPC(int a1, int a2, uint32_t flags,
        int direction, int tangent, int position, int a7, int a8, int a9, int a10,
        int a11, int a12, int a13, int a14, int a15, int stableKey)
    {
        if (!gStableHeadlightShadow.ShouldCast(direction, position, stableKey, a11))
            flags &= ~rage::LF_DYNAMIC_SHADOW;
        hbStoreStaticShadow.fun(a1, a2, flags, direction, tangent, position,
                                a7, a8, a9, a10, a11, a12, a13, a14, a15, stableKey);
    }

    static bool ValidateAdapter()
    {
        const auto image = reinterpret_cast<const uint8_t*>(GameBase());
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (!image || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            dos->e_lfanew > 0x100000)
            return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
        if (!fusionfix::shadows::ce::ValidateMappedImage(
                image, nt->OptionalHeader.SizeOfImage, reinterpret_cast<uintptr_t>(image)))
            return false;
        hbStoreStaticShadow.fun = reinterpret_cast<SubmitHeadlight>(
            reinterpret_cast<uintptr_t>(image) + fusionfix::shadows::ce::SubmitRva);
        return true;
    }

    // Game layouts the CE adapter was not audited for keep the official FusionFix behaviour.
    injector::hook_back<void(__cdecl*)(int, int, uint32_t, int, int, int, int, int, int, int, int, int, int, int, int)> hbStoreStaticShadowLegacy;

    void __cdecl StoreStaticShadowPlayerDrivingLegacy(int a1, int a2, uint32_t Flags, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11, int a12, int a13, int a14, int a15)
    {
        // Disable the headlight shadows of the player's vehicle, if headlight shadows are off
        if (!bHeadlightShadows)
        {
            Flags &= ~4; // Subtract dynamic shadows
        }

        return hbStoreStaticShadowLegacy.fun(a1, a2, Flags, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15);
    }

    void __cdecl StoreStaticShadowNPCLegacy(int a1, int a2, uint32_t Flags, int a4, int a5, int a6, int a7, int a8, int a9, int a10, int a11, int a12, int a13, int a14, int a15)
    {
        // Disable the headlight shadows of NPC vehicles regardless of any condition, to avoid reaching patch 1.0.6.0 night shadow limits
        Flags &= ~4; // Subtract dynamic shadows

        return hbStoreStaticShadowLegacy.fun(a1, a2, Flags, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15);
    }

    // m_pDriver followed by m_pPassengers[8], lights and headlight damage offsets differ between layouts.
    struct VehicleLayout { uint32_t lightsOn, vehicleType, headlightDamage, occupants; };

    static bool IsPlayerCarOrOccupant(const VehicleLayout& layout, uintptr_t car, uintptr_t checkAgainst)
    {
        if (!car || !checkAgainst)
            return false;

        if (!*(uint8_t*)(car + layout.lightsOn)) // Lights off
            return false;

        auto m_nVehicleType = *(uint32_t*)(car + layout.vehicleType);
        auto damaged = (uint8_t*)(car + layout.headlightDamage);
        if (m_nVehicleType == VEHICLETYPE_AUTOMOBILE)
        {
            if (damaged[0] != 0 && damaged[1] != 0) // Headlights damaged
                return false;
        }
        else if (m_nVehicleType == VEHICLETYPE_BIKE)
        {
            if (damaged[0] != 0 || damaged[1] != 0) // Headlight damaged
                return false;
        }

        auto passengers = (uintptr_t*)(car + layout.occupants);
        for (size_t i = 0; i < 9; i++)
        {
            if (checkAgainst == passengers[i])
                return true;
        }

        return checkAgainst == car;
    }
}

#include "ShadowAllocationRuntime.inl"
#include "NearbyVehicleRuntime36.inl"
#include "ShadowLookupRuntime.inl"
#include "ShadowCasterRuntime.inl"
#include "NightShadowAdmissionRuntime.inl"
#include "EmergencyTrafficShadowsRuntime.inl"

static inline SafetyHookInline shsub_925DB0{};
static inline SafetyHookInline shsub_D77A00{};

// Lamppost shadows workaround 1
static int __cdecl sub_925DB0(int a1, int a2, int flags)
{
    if (!bExtraNightShadows)
    {
        if (!Natives::IsInteriorScene())
        {
            return -1;
        }
    }

    const int buffer = ShadowLookupGuard::ReadBuffer();
    const int result = shsub_925DB0.ccall<int>(a1, a2, flags);
    return ShadowLookupGuard::Filter(result, static_cast<uint32_t>(a1), a2, buffer);
}

// Lamppost shadows workaround 2
static void __fastcall sub_D77A00(void* _this, void* edx)
{
    if (!bExtraNightShadows)
    {
        if (!Natives::IsInteriorScene())
        {
            return;
        }
    }

    const fusionfix::shadows::caster::Scope scope(OwnHeadlightCaster::context,
                                                OwnHeadlightCaster::Capture(_this));
    return shsub_D77A00.unsafe_fastcall(_this, edx);
}

int GetNightShadowQuality()
{
    static auto NightShadows = FusionFixSettings.GetRef("PREF_SHADOW_DENSITY");
    switch (NightShadows->get())
    {
        // MO_OFF / MO_MED / MO_HIGH / MO_VHIGH
        case 0: return 0;
        case 1: return 256  * (bHighResolutionNightShadows ? 2 : 1);
        case 2: return 512  * (bHighResolutionNightShadows ? 2 : 1);
        case 3: return 1024 * (bHighResolutionNightShadows ? 2 : 1);
        default: return 0;
    }
}

// Menu-only status query. Keep the original warning when the guarded fix is
// unavailable, disabled, or running in observation mode.
export bool IsPlayerNightShadowFixActive() noexcept
{
    return PlayerShadowAllocation::ready.load(std::memory_order_acquire) &&
        PlayerShadowAllocation::publicationEnabled &&
        !PlayerShadowAllocation::unsupportedThread.load(std::memory_order_relaxed) &&
        OwnHeadlightCaster::enabled.load(std::memory_order_acquire) &&
        bExtraNightShadows && bHeadlightShadows && bVehicleNightShadows;
}

// ShadowFadeIn: in the loop that draws the frame's lights (CE 0xAC1030), once a light's sphere is in the
// frame (0xAC11CC, edi the light + 0x28), c143.x gets how much of its shadow is still to come in.
static void InstallShadowFadeIn()
{
    // movss xmm0, [esp+14h] / xorps xmm0, [...] / comiss xmm0, [esp+10h] / jbe; the test before it may
    // already be hooked by the traffic signal log
    auto pattern = hook::pattern("F3 0F 10 44 24 14 0F 57 05 ? ? ? ? 0F 2F 44 24 10 0F 86");
    if (pattern.empty())
    {
        PlayerShadowAllocation::shadowFadeMs = 0;
        return;
    }
    static auto hook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
    {
        const auto key = *reinterpret_cast<const uint32_t*>(regs.edi + 0x38); // light + 0x60
        const float fade[4] = { PlayerShadowAllocation::ShadowFadeLeft(key), 0.0f, 0.0f, 0.0f };
        if (auto device = rage::grcDevice::GetD3DDevice())
            device->SetPixelShaderConstantF(143, fade, 1);
    });
}

class NightShadows
{
    // Taken while the ASI loads, before any FusionFix module installs hooks. The async
    // initializers run in parallel, and framelimit.ixx hooks the frame counter increment
    // the adapter checks, so checking from onInitEventAsync races with it.
    static inline bool ceAdapter = false;
    static inline bool casterGuard = false;

public:
    NightShadows()
    {
        {
            ceAdapter = CShadows::ValidateAdapter();
            const auto image = reinterpret_cast<const uint8_t*>(GameBase());
            // Uses the real image size, so an unexpected executable is never read past its end.
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
            ShadowDiagnostics::adapterStatus = std::string(ceAdapter ? "ce_adapter=1 " : "ce_adapter=0 ") +
                fusionfix::shadows::ce::DescribeMappedImage(image, nt->OptionalHeader.SizeOfImage,
                    reinterpret_cast<uintptr_t>(image));
            if (ceAdapter)
            {
                // Validate BEFORE allocator and caster installation alter guarded bytes.
                casterGuard = fusionfix::shadows::ce::casterguard::Validate(
                    image, fusionfix::shadows::ce::ImageSize, reinterpret_cast<uintptr_t>(image));
                // Preserve the exact startup mismatch before any hook modifies a guarded
                // range. This remains diagnostic only: no guard bypass.
                ShadowDiagnostics::startupGuardDetails = fusionfix::shadows::ce::diagnostics::Describe(
                    image, fusionfix::shadows::ce::ImageSize, reinterpret_cast<uintptr_t>(image));
                // The allocation adapter installs from the async initializers; its guard is taken here.
                PlayerShadowAllocation::guardAtLoad = fusionfix::shadows::ce::allocation::ValidateMappedImage(
                    image, fusionfix::shadows::ce::ImageSize, reinterpret_cast<uintptr_t>(image));
                PlayerShadowAllocation::guardDetailsAtLoad = ShadowDiagnostics::startupGuardDetails;
            }
        }

        // Registered before game callbacks start, independent of async init.
        FusionFix::onGameProcessEvent() += []() { PlayerCar::Update(); BeamTrace::Update(); NearbyVehicleLighting36::Update(); PlayerShadowAllocation::CaptureCasters(); PlayerShadowAllocation::SlotTraceStatus(); PlayerShadowAllocation::FlushSlotTrace(); EmergencyTrafficShadows::Update(); ShadowDiagnostics::Write(); HeadlightEnhancement::WriteDiagnostics(); TrafficSignalLog::Tick(); TrafficSignalLog::Write(); };
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");

            // Traffic signals light up only while their own model is drawn in full: the signal's light comes from
            // its entity's pre-render (CE 0xa32854 -> 0xd208f0, models with 0x3000000 in +0x40), which runs only for
            // entities drawn this frame. Their draw distance (model +0x2c, copied to entity +0x50 at CE 0x9d7a44, or
            // the placement's own) is short, so a signal ahead stayed dark until close. Scaled here: the model once,
            // the first time an entity of it is made, and each entity made with the old value or its own
            TrafficSignalLog::enabled = iniReader.ReadInteger("SHADOWS", "TrafficSignalDrawDistanceLog", 0) != 0;
            TrafficSignalLog::lightTest = std::clamp(iniReader.ReadInteger("SHADOWS", "TrafficSignalLightTest", 0), 0, 3);
            if (TrafficSignalLog::enabled || TrafficSignalLog::lightTest)
                TrafficSignalLog::InstallDrawSteps();
            if (TrafficSignalLog::enabled)
            {
                // push esi / call 0xD208F0, the signal's light from its pre-render
                if (auto pattern = hook::pattern("56 E8 ? ? ? ? 83 C4 04 5E 5D 33 C0 5B 59 C3"); !pattern.empty())
                {
                    static auto SignalLitHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                    {
                        TrafficSignalLog::Logic(regs.esi);
                    });
                }
                else
                    FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance", "the signal logic call was not found\n");
                // push ecx / mov ecx, esi / movss [esp], xmm1 / call 0xC1DB60, the entity's 2dfx lights
                if (auto pattern = hook::pattern("51 8B CE F3 0F 11 0C 24 E8 ? ? ? ? F3 0F 10 44 24 10 51 8B CE F3 0F 11 04 24 E8 ? ? ? ? 0F BF 46 2E 5F"); !pattern.empty())
                {
                    static auto SignalLightsHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                    {
                        TrafficSignalLog::Lights(regs.esi, regs.xmm1.f32[0]);
                    });
                }
                else
                    FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance", "the 2dfx lights call was not found\n");
                TrafficSignalLog::InstallRoadSteps();
            }
            if (auto distance = std::clamp(iniReader.ReadFloat("SHADOWS", "TrafficSignalDrawDistance", 2.5f), 1.0f, 5.0f); distance != 1.0f)
            {
                fTrafficSignalDrawScale = distance;
                if (auto pattern = hook::pattern("F3 0F 11 47 50 EB 0A 8B 44 24 18 8B 40 2C 89 47 50 8B 44 24 18"); !pattern.empty())
                {
                    static std::mutex scaledMutex;
                    static std::unordered_set<uintptr_t> scaledModels;
                    static auto SignalDistanceHook = safetyhook::create_mid(pattern.get_first(17), [](SafetyHookContext& regs)
                    {
                        auto model = *(uint8_t**)(regs.esp + 0x18);
                        if (!model || regs.edi < 0x10000 || (*(uint32_t*)(model + 0x40) & 0xFF000000) != 0x3000000)
                            return;
                        auto& modelDistance = *(float*)(model + 0x2C);
                        auto& entityDistance = *(float*)(regs.edi + 0x50);
                        std::lock_guard lock(scaledMutex);
                        if (scaledModels.insert(uintptr_t(model)).second && std::isfinite(modelDistance) && modelDistance > 0.0f)
                        {
                            const float from = modelDistance;
                            modelDistance *= fTrafficSignalDrawScale;
                            if (TrafficSignalLog::enabled)
                            {
                                ++TrafficSignalLog::models;
                                std::lock_guard firstLock(TrafficSignalLog::firstMutex);
                                if (TrafficSignalLog::first.size() < 400)
                                    TrafficSignalLog::first += std::format("{}{:08x} {:.1f}->{:.1f}", TrafficSignalLog::first.empty() ? "" : ", ",
                                        uintptr_t(model), from, modelDistance);
                            }
                        }
                        if (std::isfinite(entityDistance) && entityDistance > 0.0f && entityDistance != modelDistance)
                        {
                            entityDistance *= fTrafficSignalDrawScale;
                            ++TrafficSignalLog::placed;
                        }
                        else
                            ++TrafficSignalLog::entities;
                        if (entityDistance > TrafficSignalLog::farthestDistance.load(std::memory_order_relaxed))
                            TrafficSignalLog::farthestDistance = entityDistance;
                    });
                }
                else
                    FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance", "the signal draw distance store was not found\n");

                // The coloured light a signal throws on the road is not its 2dfx light: its logic (0xD208F0 ->
                // 0x9BBC70) adds it to a list of its own, 64 a frame, only within [0x103AB80] = 60 m of the camera
                // over the game's distance scale [0x103F6BC], fading out towards there (0x9BBDFB). Those two reads
                // are the only ones of that value, so it is scaled with the signals. Past 48 entries in a frame,
                // signals further than the game's 60 m are left out, so the near ones keep their places.
                auto limitRead = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 51 C9 F3 0F 5E 0D ? ? ? ? 0F 2F C1 F3 0F 11 4C 24 14");
                auto fadeRead = hook::pattern("F3 0F 10 1D ? ? ? ? F3 0F 10 4C 24 14 F3 0F 10 15");
                if (!limitRead.empty() && !fadeRead.empty() &&
                    *limitRead.get_first<float*>(4) == *fadeRead.get_first<float*>(4) && **limitRead.get_first<float*>(4) == 60.0f)
                {
                    static float* limit = *limitRead.get_first<float*>(4);
                    TrafficSignalLog::roadLimit = limit;
                    static const float gameLimit = *limit;
                    injector::WriteMemory<float>(limit, gameLimit * fTrafficSignalDrawScale, true);
                    // comiss xmm0, xmm1 with ecx the entries so far, xmm1 the distance, xmm0 the limit
                    static auto SignalLightReserveHook = safetyhook::create_mid(limitRead.get_first(0x14), [](SafetyHookContext& regs)
                    {
                        if (regs.ecx >= 48 && regs.xmm1.f32[0] > gameLimit)
                            regs.xmm0.f32[0] = gameLimit;
                    });
                }
                else
                    FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance", "the signal road light distance was not found\n");

                // The signal's light on the road is its 2dfx light (TrafficSignalLightTest 2 took it away), which
                // fades by distances of its own: the effect's +0x60 times [0x1048230] and +0x64 (CE 0xC1DFF8 to
                // 0xC1E128, the camera at 0x128E340), each over a range at +0x38 / +0x34. Past them the light is
                // still made, at full intensity but with its colour faded to nothing, so it showed only from
                // about 120 m and then at once. Both are scaled for signals; -1 means no fade and is kept.
                // At 0xC1E07A esi is the entity, xmm1 the first distance (scaled), xmm6 the second.
                if (auto fade = hook::pattern("F3 0F 59 0D ? ? ? ? F3 0F 58 D0 0F 2E CF 0F 51 D2"); !fade.empty())
                {
                    static auto SignalLightFadeHook = safetyhook::create_mid(fade.get_first(8), [](SafetyHookContext& regs)
                    {
                        if (!regs.esi || !TrafficSignalLog::IsSignal(regs.esi)) return;
                        ++TrafficSignalLog::fadeSeen;
                        TrafficSignalLog::fadeFirst = regs.xmm1.f32[0];
                        TrafficSignalLog::fadeSecond = regs.xmm6.f32[0];
                        if (regs.xmm1.f32[0] > 0.0f) regs.xmm1.f32[0] *= fTrafficSignalDrawScale;
                        if (regs.xmm6.f32[0] > 0.0f) regs.xmm6.f32[0] *= fTrafficSignalDrawScale;
                    });
                }
                else
                    FusionLog::Write("NightShadows.TrafficSignals", "DrawDistance", "the signal light fade was not found\n");
            }
            if (iniReader.ReadInteger("SHADOWS", "ExperimentalCrashDiagnostics", 0) != 0)
            {
                const auto crashPath = iniReader.GetIniPath().parent_path() /
                    (L"GTAIV-shadow-crash-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
                const auto contextPath = iniReader.GetIniPath().parent_path() /
                    (L"GTAIV-path-context-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
                crash_path_context::Install(contextPath.c_str());
                shadow_crash_trace::Install(crashPath.c_str());
            }

            // [NIGHTSHADOWS]
            bHighResolutionNightShadows = iniReader.ReadInteger("SHADOWS", "HighResolutionNightShadows", 0) != 0;
            const bool shadowDiagnostics = iniReader.ReadInteger("SHADOWS", "ExperimentalShadowDiagnostics", 0) != 0;

            // The experimental adapter has only been audited for CE 1.2.0.59. Any other
            // layout skips it and keeps the official FusionFix night shadow behaviour.
            ShadowDiagnostics::log.Name("NightShadows", "Diagnostics");
            int casterMode = 0;
            if (ceAdapter)
            {
                PlayerCar::driverOffset = 0xF50;
                BeamTrace::enabled = true;
                HeadlightEnhancement::log.Name("Headlights", "Status");
                HeadlightEnhancement::brightnessInstalled = HeadlightEnhancement::InstallBrightness(
                    iniReader.ReadInteger("HEADLIGHTS", "ConsistentBrightness", 0) != 0);
                HeadlightEnhancement::InstallLightModes(iniReader.ReadInteger("HEADLIGHTS", "LightModes", 0) != 0);
                HeadlightEnhancement::InstallOffscreenLights(iniReader.ReadInteger("HEADLIGHTS", "OffscreenLights", 1),
                    std::clamp(iniReader.ReadFloat("HEADLIGHTS", "OffscreenLightsDistance", 100.0f), 0.0f, 500.0f));
                bCloseHeadlightRelevance = iniReader.ReadInteger("SHADOWS", "ExperimentalCloseHeadlightRelevance", 0) != 0;
                bTrafficSelfShadowFix = iniReader.ReadInteger("SHADOWS", "ExperimentalTrafficSelfShadowFix", 0) != 0;
                NearbyVehicleLighting36::enabled.store(iniReader.ReadInteger("SHADOWS", "NearbyVehicleHeadlightReceivers", 0) != 0, std::memory_order_release);

                casterMode = iniReader.ReadInteger("SHADOWS", "ExperimentalOwnHeadlightCasterFix", 0);
                OwnHeadlightCaster::base = GameBase();
                // Publication happens after all hooks are installed below.
                ShadowDiagnostics::guardPassed = casterGuard;
                ShadowDiagnostics::casterMode = casterMode;

                // Offline-reviewed prototype; never turn this on silently for an
                // existing installation. A later controlled launch must opt in.
                PlayerShadowAllocation::cameraPriority = iniReader.ReadInteger("SHADOWS", "CameraAwareShadowPriority", 0) != 0;
                if (PlayerShadowAllocation::cameraPriority)
                    PlayerShadowAllocation::cameraPriority = PlayerShadowAllocation::InstallCameraCapture();
                PlayerShadowAllocation::nativeLampPriority = iniReader.ReadInteger("SHADOWS", "NativeLampViewPriority", 0) != 0;
                PlayerShadowAllocation::casterPriority = iniReader.ReadInteger("SHADOWS", "CasterAwareLampPriority", 0) != 0;
                PlayerShadowAllocation::casterHoldMs = static_cast<uint32_t>(std::clamp(iniReader.ReadInteger("SHADOWS", "CasterAwareLampPriorityHold", 750), 0, 2000));
                PlayerShadowAllocation::slotTrace.enabled = iniReader.ReadInteger("SHADOWS", "CasterAwareLampPriorityLog", 0) != 0;
                PlayerShadowAllocation::lampSpacing.base = std::clamp(iniReader.ReadFloat("SHADOWS", "LampSpacing", 0.0f), 0.0f, 40.0f);
                PlayerShadowAllocation::lampSpacing.spacing = PlayerShadowAllocation::lampSpacing.base;
                PlayerShadowAllocation::lampSpacing.seconds = std::clamp(iniReader.ReadFloat("SHADOWS", "LampSpacingSeconds", 0.5f), 0.0f, 5.0f);
                PlayerShadowAllocation::lampSpacing.alongRoad = iniReader.ReadInteger("SHADOWS", "LampSpacingAlongRoad", 0) != 0;
                PlayerShadowAllocation::behindLampReach = std::clamp(iniReader.ReadFloat("SHADOWS", "BehindLampReach", 12.0f), 0.0f, 100.0f);
                PlayerShadowAllocation::slotMinHoldMs = static_cast<uint32_t>(std::clamp(iniReader.ReadInteger("SHADOWS", "SlotMinHold", 1000), 0, 5000));
                fusionfix::shadows::NativeShadowContinuity42::claimDistanceRatio =
                    std::clamp(iniReader.ReadFloat("SHADOWS", "ClaimDistanceRatio", 1.5f), 0.0f, 10.0f);
                const int allocationMode = iniReader.ReadInteger("SHADOWS", "ExperimentalPlayerShadowAllocation", 0);
                ShadowDiagnostics::allocationMode = allocationMode;
                if (shadowDiagnostics)
                    ShadowTrace34::Start(FusionLog::PathFor("NightShadows.LightTrace-mode" + std::to_string(allocationMode) + "-" +
                        std::to_string(GetCurrentProcessId()), L".bin"),
                        allocationMode, GetCurrentProcessId());
                // 0=off, 1=observe private output only, 2=experimental publication.
                if (allocationMode == 1 || allocationMode == 2)
                {
                    if (!PlayerShadowAllocation::Install(allocationMode == 2))
                        OutputDebugStringW(L"FusionFix experimental shadows: allocation adapter unavailable; original engine selection retained.\n");
                    else if ((PlayerShadowAllocation::shadowFadeMs = static_cast<uint32_t>(
                                 std::clamp(iniReader.ReadInteger("SHADOWS", "ShadowFadeIn", 400), 0, 2000))) != 0)
                        InstallShadowFadeIn();
                }
                // After the allocation adapter, which checks the selection's bytes this hooks.
                HeadlightEnhancement::InstallShadowOrigin(
                    std::clamp(iniReader.ReadFloat("HEADLIGHTS", "ShadowBehindLamps", 0.8f), 0.0f, 2.0f));
                // CE 1.8: shadows from traffic signals and emergency vehicle lights.
                EmergencyTrafficShadows::log.Name("NightShadows", "EmergencyTraffic");
                EmergencyTrafficShadows::Install(static_cast<unsigned>(std::clamp(iniReader.ReadInteger("SHADOWS", "TrafficSignalShadows", 2), 0, 7)),
                    iniReader.ReadInteger("SHADOWS", "EmergencyLightShadows", 1) != 0);
            }
            else
            {
                OutputDebugStringW(L"FusionFix experimental shadows: CE adapter validation failed; official night shadow behaviour kept.\n");
            }

            // This official workaround edits a guarded instruction, so it is installed
            // after the adapter checks above rather than from fixes.ixx.
            ShadowDiagnostics::admissionInstalled = NightShadowAdmission::Install();

            // Make the night shadow options adjust the night shadow resolution
            {
                auto pattern = find_pattern("8B 0D ? ? ? ? 85 C9 7E 1B", "8B 0D ? ? ? ? 33 C0 85 C9 7E 1B");
                static auto shsub_925E70 = safetyhook::create_inline(pattern.get_first(0), GetNightShadowQuality);
            }

            // Vehicle night shadows
            // Allows rendering dynamic shadows of vehicles from artificial light sources
            {
                static int32_t nRenderListIndex;
                auto pattern = hook::pattern("E8 ? ? ? ? 83 C4 08 57 56");
                static auto ListIndexHook = safetyhook::create_mid(injector::GetBranchDestination(pattern.get_first(0)).get<void>(), [](SafetyHookContext& regs)
                {
                    nRenderListIndex = *(int32_t*)(regs.esp + 0x04);
                });

                pattern = find_pattern("A1 ? ? ? ? 83 EC 40 56", "A1 ? ? ? ? 83 EC 40 85 C0");
                auto registerTechniqueGroup = (int32_t(__cdecl*)(const char*))pattern.get_first(0);
                static int32_t nLocalShadowTechId = registerTechniqueGroup("wd_local");

                pattern = find_pattern("E8 ? ? ? ? 6A 00 6A 14 E8 ? ? ? ? 8B F8 83 C4 44", "E8 ? ? ? ? 53 6A 14 E8 ? ? ? ? 83 C4 44");
                static void (__cdecl* AddToDrawListEntityListShadow)(int32_t, int32_t, int32_t, int32_t, int32_t) = injector::GetBranchDestination(pattern.get_first(0)).get();

                pattern = find_pattern("74 ? 53 6A ? 6A ? 6A ? 55 E8 ? ? ? ? 53", "74 ? 56 53 53 53");
                static auto VehicleShadowsHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                {
                    if(bVehicleNightShadows && regs.eflags & 0x0040)
                    {
                        auto pushTech = new CShaderFx_PushForcedTechnique(nLocalShadowTechId);
                        if(pushTech)
                            pushTech->Append();

                        AddToDrawListEntityListShadow(nRenderListIndex, 0, 0, 0, -1);
                        AddToDrawListEntityListShadow(nRenderListIndex, 1, 0, 0, -1);

                        auto popTech = new CShaderFx_PopForcedTechnique();
                        if(popTech)
                            popTech->Append();
                    }
                });
            }

            // Headlight shadows
            if (ceAdapter)
            {
                const uintptr_t image = GameBase();
                for (size_t i = 0; i < fusionfix::shadows::ce::CallRvas.size(); ++i)
                {
                    const auto wrapper = i < 2 ? CShadows::StoreStaticShadowPlayerDriving :
                                                  CShadows::StoreStaticShadowNPC;
                    injector::MakeCALL(image + fusionfix::shadows::ce::CallRvas[i], wrapper);
                }
            }
            else
            {
                auto pattern = hook::pattern("68 04 05 00 00 6A 02 6A 00");
                if (!pattern.count(2).empty())
                {
                    CShadows::hbStoreStaticShadowLegacy.fun = injector::MakeCALL(pattern.count(2).get(0).get<void*>(9), CShadows::StoreStaticShadowPlayerDrivingLegacy).get();
                    CShadows::hbStoreStaticShadowLegacy.fun = injector::MakeCALL(pattern.count(2).get(1).get<void*>(9), CShadows::StoreStaticShadowPlayerDrivingLegacy).get();
                }

                pattern = hook::pattern("68 04 01 00 00 6A 02 6A 00");
                if (!pattern.count(2).empty())
                {
                    CShadows::hbStoreStaticShadowLegacy.fun = injector::MakeCALL(pattern.count(2).get(0).get<void*>(9), CShadows::StoreStaticShadowNPCLegacy).get();
                    CShadows::hbStoreStaticShadowLegacy.fun = injector::MakeCALL(pattern.count(2).get(1).get<void*>(9), CShadows::StoreStaticShadowNPCLegacy).get();
                }
            }

            {
                static bool bCEAdapter = ceAdapter;
                auto pattern = hook::pattern("83 F8 03 75 14 F6 86");
                if (!pattern.empty())
                {
                    static auto loc_AE3867 = resolve_next_displacement(pattern.get_first(14)).value();
                    static auto loc_AE374F = resolve_next_displacement(pattern.get_first(3)).value();
                    struct ShadowsHook
                    {
                        void operator()(injector::reg_pack& regs)
                        {
                            const bool artificial = !*reinterpret_cast<const uint8_t*>(regs.esp + 0x0B);
                            if (bCEAdapter)
                            {
                                // Exclude the occupied car/occupants only in their own
                                // immediate headlight pass; retain their lamp shadows.
                                if (OwnHeadlightCaster::Exclude(regs.esi, regs.eax, artificial))
                                    return_to(loc_AE3867);
                            }
                            else if (bHeadlightShadows && bVehicleNightShadows)
                            {
                                // Disable the shadow of the player's vehicle, along with the shadows of the player/peds in that vehicle, if headlight shadows and vehicle night shadows are on (to avoid both interfering witch each other)
                                if (CShadows::IsPlayerCarOrOccupant({ 0xF15, 0x1304, 0x1190, 0xF50 }, CPlayer::findPlayerCar(), regs.esi) && artificial)
                                    return_to(loc_AE3867);
                            }

                            // Enable shadows of the player/peds while in vehicles, if vehicle night shadows are on and if headlight shadows are off
                            if (!bHeadlightShadows && bVehicleNightShadows)
                            {
                                return_to(loc_AE374F);
                            }

                            if (!bVehicleNightShadows)
                            {
                                if (regs.eax == 3 && (*(uint8_t*)(regs.esi + 620) & 4) != 0 && artificial)
                                {
                                    force_return_address(loc_AE3867);
                                }
                            }
                        }
                    }; injector::MakeInline<ShadowsHook>(pattern.get_first(0), pattern.get_first(25));
                }
                else
                {
                    pattern = hook::pattern("83 F8 03 75 17 F6 86");
                    static auto loc_AE3867 = resolve_next_displacement(pattern.get_first(14)).value();
                    static auto loc_AE374F = resolve_next_displacement(pattern.get_first(3)).value();
                    struct ShadowsHook
                    {
                        void operator()(injector::reg_pack& regs)
                        {
                            const bool artificial = !*reinterpret_cast<const uint8_t*>(regs.esp + 0x0F);
                            if (bHeadlightShadows && bVehicleNightShadows)
                            {
                                // Disable the shadow of the player's vehicle, along with the shadows of the player/peds in that vehicle, if headlight shadows and vehicle night shadows are on (to avoid both interfering witch each other)
                                if (CShadows::IsPlayerCarOrOccupant({ 0xF65, 0x1350, 0x11E0, 0xFA0 }, CPlayer::findPlayerCar(), regs.esi) && artificial)
                                    return_to(loc_AE3867);
                            }

                            // Enable shadows of the player/peds while in vehicles, if vehicle night shadows are on and if headlight shadows are off
                            if (!bHeadlightShadows && bVehicleNightShadows)
                            {
                                return_to(loc_AE374F);
                            }

                            if (!bVehicleNightShadows)
                            {
                                if (regs.eax == 3 && (*(uint8_t*)(regs.esi + 620) & 4) != 0 && artificial)
                                {
                                    force_return_address(loc_AE3867);
                                }
                            }
                        }
                    }; injector::MakeInline<ShadowsHook>(pattern.get_first(0), pattern.get_first(25));
                }
            }

            // Lamppost shadows
            {
                // Lamppost shadows workaround 1
                auto pattern = hook::pattern("80 3D ? ? ? ? ? 75 04 83 C8 FF");
                if (ceAdapter)
                    ShadowLookupGuard::Initialize();
                shsub_925DB0 = safetyhook::create_inline(pattern.get_first(), sub_925DB0);

                // Lamppost shadows workaround 2
                pattern = find_pattern("83 EC 3C 80 3D ? ? ? ? ? 56 8B F1", "83 EC 3C 53 33 DB");
                shsub_D77A00 = safetyhook::create_inline(pattern.get_first(0), sub_D77A00);

                // This code tests every light source for a custom flag.
                // When Extra Night Shadows is disabled, any light that holds that custom flag has its static and dynamic shadow bits removed if present.
                // This is mainly of use for us to allow disabling the shadows of light models from lamppost.img, for parity with consoles.
                // NOTE:
                // Currently, this code's effects are overridden by a workaround for an ugly bug,
                // which forces us to disable all exterior/outdoors night shadows. Because of that, this hook does not currently produce visible changes.
                pattern = hook::pattern("8B 55 20 F6 C1 06");
                if (!pattern.empty())
                {
                    static auto FlagsHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                    {
                        if (!bExtraNightShadows)
                        {
                            if ((*(uint32_t*)(regs.edi + 0x4C) & 0x8000000) != 0) // Check if flag 134217728 exists
                            {
                                regs.ecx &= ~2; // Subtract static shadows
                                regs.ecx &= ~4; // Subtract dynamic shadows
                                *(uint32_t*)(regs.esp + 0x18) = regs.ecx;
                            }
                        }
                    });
                }
                else
                {
                    pattern = hook::pattern("E8 ? ? ? ? 0F B6 46 ? F3 0F 10 44 24");
                    static auto FlagsHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                    {
                        if (!bExtraNightShadows)
                        {
                            if ((*(uint32_t*)(regs.esi + 0x4C) & 0x8000000) != 0) // Check if flag 134217728 exists
                            {
                                regs.ebx &= ~2; // Subtract static shadows
                                regs.ebx &= ~4; // Subtract dynamic shadows
                            }
                        }
                    });
                }
            }

            // Keep the original contact-shadow compensation only when actual vehicle night shadows are disabled.
            {
                auto pattern = hook::pattern("C7 44 24 ? ? ? ? ? F3 0F 11 14 24 50");
                if (!pattern.empty())
                {
                    static auto CarStaticShadowIntensityHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                    {
                        if (bHeadlightShadows && !bVehicleNightShadows)
                        {
                            regs.xmm2.f32[0] *= 3.0f;
                        }
                    });
                }
                else
                {
                    pattern = hook::pattern("D9 1C 24 8D 4C 24 34 51 8B 4D 0C 52 51 50 6A 00 6A 03 6A 00 E8 ? ? ? ? 83 C4 40 8B E5 5D C3 CC");
                    static auto CarStaticShadowIntensityHook = safetyhook::create_mid(pattern.count(2).get(0).get<void*>(7), [](SafetyHookContext& regs)
                    {
                        if (bHeadlightShadows && !bVehicleNightShadows)
                        {
                            *(float*)regs.esp *= 3.0f;
                        }
                    });
                }
            }
            OwnHeadlightCaster::enabled.store(ceAdapter && casterGuard && casterMode == 1 &&
                static_cast<bool>(shsub_D77A00), std::memory_order_release);
            HeadlightEnhancement::log.ready.store(ceAdapter && shadowDiagnostics, std::memory_order_release);
            EmergencyTrafficShadows::log.ready.store(ceAdapter && shadowDiagnostics, std::memory_order_release);
            // Written on every executable, so a missing adapter shows up with its reason.
            ShadowDiagnostics::log.ready.store(shadowDiagnostics, std::memory_order_release);
        };
    }
} NightShadows;
