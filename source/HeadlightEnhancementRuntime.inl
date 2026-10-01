// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

namespace HeadlightEnhancement
{
    static SafetyHookMid brightnessHook;
    static uintptr_t imageBase = 0;
    static std::atomic<uint64_t> lastDrivenToken{0};
    static std::atomic<uint32_t> retainedSubmissions{0};
    static std::atomic<uint32_t> hookCalls{0};
    static bool brightnessInstalled = false;
    // Why the hook is or is not in place, and the jump it wrote: another plugin that patches
    // the same game code turns the hook off at install, or replaces the jump later.
    static std::string brightnessStatus = "not checked";
    // The radius of the beams is (floor(vehicle+F74) * bonus + base) * range. F74 climbs to 1 while
    // a player sits in the car and falls back to 0 once none does (CE 0xA4EA3B), so the bonus,
    // 20 m on top of a base of 10 m, is the player's alone: it was lost right after getting out,
    // a third of the driving reach with ConsistentBrightness, and for a second after getting in.
    // Both read from the instructions.
    static const float* pRadiusBonus = nullptr;
    static const float* pRadiusBase = nullptr;
    static uint8_t siteBytes[5]{};
    static std::string lightModesStatus = "off in the ini";
    static std::atomic<uint32_t> lightModeSteps{0};
    static std::atomic<uint8_t> lastLightModeFrom{0}, lastLightModeTo{0};
    static std::atomic<bool> diagnosticsReady{false};
    static std::filesystem::path logPath;

    // Diagnostics: changes of the light state of the car the player drives or last drove, as
    // this function sees them, to find why its headlights stop lighting when the player gets out.
    struct LightEvent
    {
        ULONGLONG time;
        bool driver;
        uint8_t f15, f19, f21, highBeam, highBeamArg, left, right;
        float intensity, range, radius;
        uint32_t frame, gap;
    };
    static LightEvent lightEvents[24]{};
    static uint32_t lightEventCount = 0;
    static LightEvent lastLightState{};
    static uint64_t lastLightToken = 0;
    static std::atomic_flag lightEventsLock = ATOMIC_FLAG_INIT;

    // Diagnostics: the shadow passes of the beam of the car the player drives or last drove, as
    // the night shadow code sees them, on each change: to find why its headlight shadow blinks
    // while the player gets in.
    struct ShadowPassEvent
    {
        ULONGLONG time;
        uint32_t frame, slot, kind;
        bool active, own, traffic, carExcluded, occupantsExcluded;
    };
    static ShadowPassEvent shadowPassEvents[32]{};
    static uint32_t shadowPassEventCount = 0;
    static ShadowPassEvent lastShadowPass{};
    static std::atomic_flag shadowPassLock = ATOMIC_FLAG_INIT;

    static void TraceShadowPass(const ShadowPassEvent& pass)
    {
        const auto& l = lastShadowPass;
        if (shadowPassEventCount && pass.slot == l.slot && pass.kind == l.kind && pass.active == l.active &&
            pass.own == l.own && pass.traffic == l.traffic && pass.carExcluded == l.carExcluded &&
            pass.occupantsExcluded == l.occupantsExcluded)
            return;
        while (shadowPassLock.test_and_set(std::memory_order_acquire)) {}
        lastShadowPass = pass;
        shadowPassEvents[shadowPassEventCount++ % std::size(shadowPassEvents)] = pass;
        shadowPassLock.clear(std::memory_order_release);
    }

    // Diagnostics: what becomes of the beam of the car the player drives or last drove once
    // submitted, on each change. The night shadow wrappers on the submission (CE 0xABCC50 from
    // 0xA3DE90 and 0xA3E070) mark it, and with shadow diagnostics mid hooks in the light add
    // function (0xABCCD0) record how far it gets: 1 past the intensity test, 2 past the camera
    // flag test, 3 in front of the camera's near plane, 4 inside the view, 5 past the interior
    // and occlusion tests, 6 handed to the light list. 0 means it never reached the function.
    struct SubmitEvent
    {
        ULONGLONG time;
        uint32_t frame, gap, flags;
        float radius;
        bool shadow;
        uint8_t stage;
    };
    static SubmitEvent submitEvents[32]{};
    static uint32_t submitEventCount = 0;
    static SubmitEvent lastSubmit{};
    static std::atomic_flag submitLock = ATOMIC_FLAG_INIT;
    static thread_local bool submitTraced = false;
    static thread_local uint8_t submitStage = 0;
    static SafetyHookMid submitStageHooks[6];
    static std::string submitStagesStatus = "off";

    template <uint8_t stage>
    static void SubmitStage(SafetyHookContext&)
    {
        if (submitTraced && submitStage < stage)
            submitStage = stage;
    }

    static bool BeginSubmitTrace(int stableKey)
    {
        const auto vehicle = static_cast<uintptr_t>(lastDrivenToken.load() & 0xFFFFFFFF);
        submitTraced = diagnosticsReady.load(std::memory_order_relaxed) && vehicle &&
            fusionfix::shadows::ce::IsVehicleBeam(static_cast<uintptr_t>(static_cast<uint32_t>(stableKey)), vehicle);
        submitStage = 0;
        return submitTraced;
    }

    static void EndSubmitTrace(uint32_t flags, int radiusBits)
    {
        submitTraced = false;
        float radius;
        std::memcpy(&radius, &radiusBits, sizeof(radius));
        const uint32_t frame = CTimer::m_frameCount ? *CTimer::m_frameCount : 0;
        const SubmitEvent e{ GetTickCount64(), frame, submitEventCount ? frame - lastSubmit.frame : 0,
            flags, radius, (flags & 4) != 0, submitStage };
        const bool changed = !submitEventCount || e.gap > 1 || e.flags != lastSubmit.flags ||
            e.stage != lastSubmit.stage || std::fabs(e.radius - lastSubmit.radius) > 0.5f;
        while (submitLock.test_and_set(std::memory_order_acquire)) {}
        lastSubmit = e;
        if (changed)
            submitEvents[submitEventCount++ % std::size(submitEvents)] = e;
        submitLock.clear(std::memory_order_release);
    }

    static void InstallSubmitStages()
    {
        const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        struct Site { uint32_t rva; uint8_t bytes[6]; size_t count; };
        constexpr Site sites[]{
            { 0x6BCD00, {0x8B,0x45,0x14,0x8B,0x75,0x0C}, 6 },   // mov eax,[ebp+14] / mov esi,[ebp+C]
            { 0x6BD05F, {0x0F,0x28,0xC1,0x0F,0x57,0xC3}, 6 },   // movaps xmm0,xmm1 / xorps xmm0,xmm3
            { 0x6BD06E, {0x6A,0x00,0x83,0xEC,0x10}, 5 },        // push 0 / sub esp,10
            { 0x6BD099, {0x8B,0x75,0x10,0x8B,0x45,0x48}, 6 },   // mov esi,[ebp+10] / mov eax,[ebp+48]
            { 0x6BD104, {0x8B,0x0D}, 2 },                       // mov ecx,[...]
            { 0x6BD294, {0x51,0x8D,0x44,0x24,0x64}, 5 },        // push ecx / lea eax,[esp+64]
        };
        for (const auto& site : sites)
            if (std::memcmp(reinterpret_cast<const void*>(image + site.rva), site.bytes, site.count))
            {
                submitStagesStatus = "game code differs: " + DumpBytes(image + site.rva, 6);
                return;
            }
        submitStageHooks[0] = safetyhook::create_mid(image + sites[0].rva, SubmitStage<1>);
        submitStageHooks[1] = safetyhook::create_mid(image + sites[1].rva, SubmitStage<2>);
        submitStageHooks[2] = safetyhook::create_mid(image + sites[2].rva, SubmitStage<3>);
        submitStageHooks[3] = safetyhook::create_mid(image + sites[3].rva, SubmitStage<4>);
        submitStageHooks[4] = safetyhook::create_mid(image + sites[4].rva, SubmitStage<5>);
        submitStageHooks[5] = safetyhook::create_mid(image + sites[5].rva, SubmitStage<6>);
        submitStagesStatus = "installed";
        for (const auto& hook : submitStageHooks)
            if (!hook)
                submitStagesStatus = "hook failed";
    }

    static void TraceLightState(SafetyHookContext& regs, uintptr_t vehicle, uint64_t token, bool driver)
    {
        const auto byteAt = [](uintptr_t address) { return *reinterpret_cast<const uint8_t*>(address); };
        // The radius the beams will have, as the function goes on to compute it.
        const float ramp = std::floor(*reinterpret_cast<const float*>(vehicle + 0xF74));
        const float radius = pRadiusBonus && pRadiusBase ? (ramp * *pRadiusBonus + *pRadiusBase) * regs.xmm4.f32[0] : 0.0f;
        const uint32_t frame = CTimer::m_frameCount ? *CTimer::m_frameCount : 0;
        const uint32_t gap = token == lastLightToken ? frame - lastLightState.frame : 0;
        LightEvent state{ GetTickCount64(), driver, byteAt(vehicle + 0xF15), byteAt(vehicle + 0xF19),
            byteAt(vehicle + 0xF21), static_cast<uint8_t>(regs.eax & 0xFF), byteAt(regs.ebp + 0x28),
            byteAt(regs.ebp + 0x10), byteAt(regs.ebp + 0x14), regs.xmm1.f32[0], regs.xmm4.f32[0],
            radius, frame, gap };
        const bool changed = token != lastLightToken || state.driver != lastLightState.driver || gap > 1 ||
            std::fabs(state.radius - lastLightState.radius) > 0.5f ||
            state.f15 != lastLightState.f15 || state.f19 != lastLightState.f19 ||
            state.highBeam != lastLightState.highBeam || state.highBeamArg != lastLightState.highBeamArg ||
            state.left != lastLightState.left || state.right != lastLightState.right;
        lastLightToken = token;
        lastLightState = state;
        if (!changed)
            return;
        while (lightEventsLock.test_and_set(std::memory_order_acquire)) {}
        lightEvents[lightEventCount++ % std::size(lightEvents)] = state;
        lightEventsLock.clear(std::memory_order_release);
    }

    // Identify the live pool slot and generation, not only a reusable pointer.
    static uint64_t VehicleToken(uintptr_t vehicle)
    {
        const auto pool = CVehicle::GetVehiclePool();
        if (!pool || !pool->m_aStorage || !pool->m_aFlags ||
            pool->m_nStorageSize <= 0 || pool->m_nSize <= 0) return 0;
        const auto start = reinterpret_cast<uintptr_t>(pool->m_aStorage);
        if (vehicle < start) return 0;
        const auto offset = vehicle - start;
        const auto stride = static_cast<uint32_t>(pool->m_nStorageSize);
        const auto index = offset / stride;
        if (offset % stride || index >= static_cast<uint32_t>(pool->m_nSize) ||
            index > 0x7FFFFF || pool->GetIsFree(index)) return 0;
        const auto handle = (index << 8) | pool->GetReference(index);
        return (static_cast<uint64_t>(handle) << 32) | vehicle;
    }

    // Scales the range by the bonus as it would be with the ramp full, over what F74 leaves of it.
    static void KeepRadiusBonus(SafetyHookContext& regs, uintptr_t vehicle)
    {
        if (!pRadiusBonus || !pRadiusBase) return;
        const float bonus = *pRadiusBonus, base = *pRadiusBase;
        const float ramp = std::floor(*reinterpret_cast<const float*>(vehicle + 0xF74));
        const float kept = ramp * bonus + base;
        if (std::isfinite(bonus) && std::isfinite(kept) && bonus > 0 && kept > 0 && ramp < 1)
            regs.xmm4.f32[0] *= (bonus + base) / kept;
    }

    static void RetainAfterExit(SafetyHookContext& regs)
    {
        hookCalls.fetch_add(1, std::memory_order_relaxed);
        const auto vehicle = static_cast<uintptr_t>(regs.esi);
        const auto token = VehicleToken(vehicle);
        if (!token) return;
        const auto driver = *reinterpret_cast<const uintptr_t*>(vehicle + 0xF50);
        if (driver)
        {
            // Same driver classification used by the original multiplier block.
            // Occupied cars have ALREADY received their original scaling.
            if (!*reinterpret_cast<const uint8_t*>(driver + 0x218) &&
                 *reinterpret_cast<const uint8_t*>(driver + 0x219))
            {
                lastDrivenToken.store(token);
                // F74 takes about a second to climb back to 1 after getting in, and the beams
                // reached a third as far until it did.
                if (*reinterpret_cast<const uint8_t*>(imageBase + 0xC3CC4A))
                    KeepRadiusBonus(regs, vehicle);
                if (diagnosticsReady.load(std::memory_order_relaxed))
                    TraceLightState(regs, vehicle, token, true);
            }
            else
            {
                auto expected = token;
                lastDrivenToken.compare_exchange_strong(expected, 0);
            }
            return;
        }
        if (lastDrivenToken.load() != token) return;
        const float intensity = *reinterpret_cast<const float*>(imageBase + 0xC3CC74);
        const float range = *reinterpret_cast<const float*>(imageBase + 0xC3CC78);
        if (*reinterpret_cast<const uint8_t*>(imageBase + 0xC3CC4A) && std::isfinite(intensity) &&
            std::isfinite(range) && intensity > 0 && range > 0 && intensity <= 10 && range <= 10)
        {
            regs.xmm1.f32[0] *= intensity;
            regs.xmm4.f32[0] *= range;
            KeepRadiusBonus(regs, vehicle);
            ++retainedSubmissions;
        }
        if (diagnosticsReady.load(std::memory_order_relaxed))
            TraceLightState(regs, vehicle, token, false);
    }

    static void WriteDiagnostics()
    {
        if (!diagnosticsReady.load(std::memory_order_acquire)) return;
        static ULONGLONG last = 0;
        const auto now = GetTickCount64();
        if (logPath.empty() || now - last < 5000) return;
        last = now;
        std::ofstream out(logPath, std::ios::trunc);
        const bool siteIntact = brightnessInstalled &&
            !std::memcmp(reinterpret_cast<const void*>(imageBase + 0x63FB77), siteBytes, sizeof(siteBytes));
        out << "brightnessInstalled=" << brightnessInstalled
            << "\nbrightnessStatus=" << brightnessStatus
            << "\nsiteIntact=" << siteIntact
            << "\nhookCalls=" << hookCalls.load(std::memory_order_relaxed)
            << "\nretainedSubmissions=" << retainedSubmissions.load()
            << "\nlightModesStatus=" << lightModesStatus
            << "\nlightModeSteps=" << lightModeSteps.load() << " (last " << unsigned(lastLightModeFrom.load())
            << " -> " << unsigned(lastLightModeTo.load()) << ", 0 off 1 on 2 high)"
            << "\nradiusBonus=" << (pRadiusBonus ? *pRadiusBonus : -1.0f)
            << "\ntrackedVehicle=" << (lastDrivenToken.load() != 0) << '\n';

        // Oldest first. highBeam is the flag the function uses (vehicle+F19 & 2, or highBeamArg);
        // left and right its lamp arguments, the beams are only submitted for 1; intensity, range
        // and radius are as they leave the hook, before high beam scaling (x1.1, x1.3); frame is
        // the game's frame, gap the frames since the function last ran for that car.
        while (lightEventsLock.test_and_set(std::memory_order_acquire)) {}
        const auto count = lightEventCount;
        const auto first = count > std::size(lightEvents) ? count - std::size(lightEvents) : 0;
        for (auto i = first; i < count; ++i)
        {
            const auto& e = lightEvents[i % std::size(lightEvents)];
            out << "t=" << e.time << " driver=" << e.driver << std::hex
                << " f15=" << unsigned(e.f15) << " f19=" << unsigned(e.f19) << " f21=" << unsigned(e.f21)
                << std::dec << " highBeam=" << unsigned(e.highBeam) << " highBeamArg=" << unsigned(e.highBeamArg)
                << " left=" << unsigned(e.left) << " right=" << unsigned(e.right) << " intensity=" << e.intensity
                << " range=" << e.range << " radius=" << e.radius << " frame=" << e.frame << " gap=" << e.gap << '\n';
        }
        lightEventsLock.clear(std::memory_order_release);

        // Its beam once submitted, oldest first (see SubmitEvent).
        out << "submitStages=" << submitStagesStatus << '\n';
        while (submitLock.test_and_set(std::memory_order_acquire)) {}
        const auto submits = submitEventCount;
        const auto firstSubmit = submits > std::size(submitEvents) ? submits - std::size(submitEvents) : 0;
        for (auto i = firstSubmit; i < submits; ++i)
        {
            const auto& e = submitEvents[i % std::size(submitEvents)];
            out << "submit t=" << e.time << " frame=" << e.frame << " gap=" << e.gap << std::hex
                << " flags=0x" << e.flags << std::dec << " radius=" << e.radius << " shadow=" << e.shadow
                << " stage=" << unsigned(e.stage) << '\n';
        }
        submitLock.clear(std::memory_order_release);

        // The shadow passes of that car's beam, oldest first: own and traffic tell which of the
        // night shadow fixes took the car out of its own headlight shadow, and whether the car and
        // its occupants were then left out of that pass.
        while (shadowPassLock.test_and_set(std::memory_order_acquire)) {}
        const auto passes = shadowPassEventCount;
        const auto firstPass = passes > std::size(shadowPassEvents) ? passes - std::size(shadowPassEvents) : 0;
        for (auto i = firstPass; i < passes; ++i)
        {
            const auto& e = shadowPassEvents[i % std::size(shadowPassEvents)];
            out << "pass t=" << e.time << " frame=" << e.frame << " slot=" << e.slot << " kind=" << e.kind
                << " active=" << e.active << " own=" << e.own << " traffic=" << e.traffic
                << " carExcluded=" << e.carExcluded << " occupantsExcluded=" << e.occupantsExcluded << '\n';
        }
        shadowPassLock.clear(std::memory_order_release);
    }

    static std::string DumpBytes(uintptr_t address, size_t count)
    {
        std::ostringstream out;
        out << std::hex;
        for (size_t i = 0; i < count; ++i)
        {
            const auto byte = reinterpret_cast<const uint8_t*>(address)[i];
            out << (i ? " " : "") << (byte < 0x10 ? "0" : "") << unsigned(byte);
        }
        return out.str();
    }

    static bool InstallBrightness(bool enabled)
    {
        if (!enabled)
        {
            brightnessStatus = "off in the ini";
            return false;
        }
        imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        constexpr uint8_t driverBlock[]{
            0x74,0x33,0x8B,0x8E,0x50,0x0F,0,0,0x85,0xC9,0x74,0x29,
            0x80,0xB9,0x18,0x02,0,0,0,0x75,0x20,
            0x80,0xB9,0x19,0x02,0,0,0,0x74,0x17
        };
        constexpr uint8_t instruction[]{0x80,0x3D};
        const auto site = reinterpret_cast<const uint8_t*>(imageBase + 0x63FB77);
        if (std::memcmp(reinterpret_cast<void*>(imageBase + 0x63FB42), driverBlock, sizeof(driverBlock)) ||
            std::memcmp(site, instruction, sizeof(instruction)) || site[6] != 0 ||
            *reinterpret_cast<const uint32_t*>(site + 2) != imageBase + 0xC3CC49)
        {
            brightnessStatus = "game code differs: " + DumpBytes(imageBase + 0x63FB42, sizeof(driverBlock)) +
                " | " + DumpBytes(imageBase + 0x63FB77, 7);
            return false;
        }
        // Runs after original player-only scaling, before high-beam scaling.
        // Do not bypass or alter the original driver eligibility instructions.
        brightnessHook = safetyhook::create_mid(imageBase + 0x63FB77, RetainAfterExit);
        if (!brightnessHook)
        {
            brightnessStatus = "hook failed";
            return false;
        }
        std::memcpy(siteBytes, site, sizeof(siteBytes));
        // mulss xmm5, [bonus] / addss xmm5, [base] in the radius computation.
        constexpr uint8_t mulss[]{0xF3,0x0F,0x59,0x2D}, addss[]{0xF3,0x0F,0x58,0x2D};
        const auto bonusAt = reinterpret_cast<const uint8_t*>(imageBase + 0x63FC45);
        const auto baseAt = reinterpret_cast<const uint8_t*>(imageBase + 0x63FC51);
        if (!std::memcmp(bonusAt, mulss, sizeof(mulss)) && !std::memcmp(baseAt, addss, sizeof(addss)) &&
            !std::memcmp(reinterpret_cast<const void*>(imageBase + 0x63FBAB), "\xF3\x0F\x10\x96\x74\x0F\0\0", 8))
        {
            pRadiusBonus = *reinterpret_cast<const float* const*>(bonusAt + 4);
            pRadiusBase = *reinterpret_cast<const float* const*>(baseAt + 4);
        }
        brightnessStatus = "installed";
        return true;
    }

    // Three light modes for the car the player drives: off, on and high beams. The game has the
    // headlights follow the time of day, and the player's tap on the headlight control only
    // flips the high beams (CE 0xA3F82F toggles vehicle+F19 & 2 and stores it at 0xA3F844).
    // The light code reads vehicle+10C2 & 3 as 0 time of day, 1 off, 2 on (CE 0xA43867), the
    // mode FORCE_CAR_LIGHTS sets, but some vehicles rewrite it every frame before that by the
    // time of day (CE 0xC3B880), so the mode is kept here per car and written into the field
    // right where it is read. At the store, the tap steps on -> high beams -> off -> on; until a
    // car has a mode of its own, whether its lights are on is what the game made of its lamps.
    struct LightMode
    {
        uint64_t token;
        uint8_t mode;
    };
    static LightMode lightModes[16]{};
    static uint32_t nextLightMode = 0;
    static SafetyHookMid lightModeHook, lightModeApplyHook, highBeamTimeoutHook;
    static const uint32_t* pGameTime = nullptr;

    static LightMode* FindLightMode(uint64_t token)
    {
        for (auto& entry : lightModes)
            if (entry.token == token)
                return &entry;
        return nullptr;
    }

    // Runs at the store of the toggled flags, inside the headlight function (ebp its frame,
    // ebp+10 and ebp+14 its left and right lamp, 1 where lit).
    static void StepLightMode(SafetyHookContext& regs)
    {
        const auto vehicle = static_cast<uintptr_t>(regs.esi);
        const auto token = VehicleToken(vehicle);
        if (!token) return;
        auto entry = FindLightMode(token);
        if (!entry)
        {
            entry = &lightModes[nextLightMode++ % std::size(lightModes)];
            *entry = { token, 0 };
        }
        const auto old = *reinterpret_cast<const uint8_t*>(vehicle + 0xF19);
        const bool lit = *reinterpret_cast<const uint8_t*>(regs.ebp + 0x10) == 1 ||
                         *reinterpret_cast<const uint8_t*>(regs.ebp + 0x14) == 1;
        const bool on = entry->mode == 2 || (entry->mode == 0 && lit);
        const auto from = static_cast<uint8_t>(!on ? 0 : (old & 2) ? 2 : 1);
        auto lights = static_cast<uint8_t>(regs.ecx & 0xFF);
        if (!on)
        {
            entry->mode = 2;
            lights &= ~2;
        }
        else if (old & 2)
        {
            entry->mode = 1;
            lights &= ~2;
        }
        else
        {
            entry->mode = 2;
            lights |= 2;
        }
        regs.ecx = (regs.ecx & ~0xFFu) | lights;
        ++lightModeSteps;
        lastLightModeFrom = from;
        lastLightModeTo = static_cast<uint8_t>(entry->mode == 1 ? 0 : (lights & 2) ? 2 : 1);
    }

    // Runs at the read of vehicle+10C2 in the light code, esi the vehicle.
    static void ApplyLightMode(SafetyHookContext& regs)
    {
        if (!nextLightMode) return;
        const auto vehicle = static_cast<uintptr_t>(regs.esi);
        const auto entry = FindLightMode(VehicleToken(vehicle));
        if (!entry || !entry->mode) return;
        auto& mode = *reinterpret_cast<uint8_t*>(vehicle + 0x10C2);
        mode = static_cast<uint8_t>((mode & ~3) | entry->mode);
    }

    // The game switches off high beams 40 s after they went on where the time of day keeps the
    // lights off (CE 0xA3F868, only with the player at the wheel); lights forced on keep them.
    // Runs at "add eax, 40000" with eax the switch-on time.
    static void KeepForcedHighBeams(SafetyHookContext& regs)
    {
        if ((*reinterpret_cast<const uint8_t*>(regs.esi + 0x10C2) & 3) == 2)
            regs.eax = *pGameTime;
    }

    static void InstallLightModes(bool enabled)
    {
        if (!enabled) return;
        imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        // mov al,[esi+F19] / mov cl,al / shr cl,1 / not cl / add cl,cl / xor cl,al / and cl,2 /
        // xor cl,al / mov [esi+F19],cl
        constexpr uint8_t toggle[]{
            0x8A,0x86,0x19,0x0F,0,0,0x8A,0xC8,0xD0,0xE9,0xF6,0xD1,0x02,0xC9,
            0x32,0xC8,0x80,0xE1,0x02,0x32,0xC8,0x88,0x8E,0x19,0x0F,0,0
        };
        // test [esi+F15],1 / jnz / mov cl,[esi+F19] / test cl,2 / jz / mov eax,[esi+F30] /
        // mov edx,[esp+1C] / add eax,9C40 / cmp [game time],eax / jbe
        constexpr uint8_t timeout[]{
            0xF6,0x86,0x15,0x0F,0,0,0x01,0x75,0x2D,
            0x8A,0x8E,0x19,0x0F,0,0,0xF6,0xC1,0x02,0x74,0x22,
            0x8B,0x86,0x30,0x0F,0,0,0x8B,0x54,0x24,0x1C,
            0x05,0x40,0x9C,0,0,0x39,0x05
        };
        // mov al,[esi+10C2] / xor cl,cl / mov [esp+6C],ecx / and al,3
        constexpr uint8_t read[]{0x8A,0x86,0xC2,0x10,0,0,0x32,0xC9,0x89,0x4C,0x24,0x6C,0x24,0x03};
        const auto toggleAt = imageBase + 0x63F82F, timeoutAt = imageBase + 0x63F868, readAt = imageBase + 0x643867;
        if (std::memcmp(reinterpret_cast<const void*>(toggleAt), toggle, sizeof(toggle)) ||
            std::memcmp(reinterpret_cast<const void*>(readAt), read, sizeof(read)) ||
            std::memcmp(reinterpret_cast<const void*>(timeoutAt), timeout, sizeof(timeout)) ||
            *reinterpret_cast<const uint32_t*>(timeoutAt + sizeof(timeout)) != imageBase + 0xD735B4 ||
            *reinterpret_cast<const uint8_t*>(timeoutAt + sizeof(timeout) + 4) != 0x76)
        {
            lightModesStatus = "game code differs: " + DumpBytes(toggleAt, sizeof(toggle)) +
                " | " + DumpBytes(timeoutAt, sizeof(timeout) + 5) + " | " + DumpBytes(readAt, sizeof(read));
            return;
        }
        pGameTime = reinterpret_cast<const uint32_t*>(imageBase + 0xD735B4);
        lightModeHook = safetyhook::create_mid(imageBase + 0x63F844, StepLightMode);
        lightModeApplyHook = safetyhook::create_mid(readAt, ApplyLightMode);
        highBeamTimeoutHook = safetyhook::create_mid(imageBase + 0x63F886, KeepForcedHighBeams);
        lightModesStatus = lightModeHook && lightModeApplyHook && highBeamTimeoutHook ? "installed" : "hook failed";
    }
}
