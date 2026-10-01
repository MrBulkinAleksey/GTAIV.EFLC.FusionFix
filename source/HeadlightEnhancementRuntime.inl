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
    static std::atomic<bool> diagnosticsReady{false};
    static std::filesystem::path logPath;

    // Diagnostics: changes of the light state of the car the player drives or last drove, as
    // this function sees them, to find why its headlights stop lighting when the player gets out.
    struct LightEvent
    {
        ULONGLONG time;
        bool driver;
        uint8_t f15, f19, f21, highBeam, highBeamArg, left, right;
        float intensity, range;
    };
    static LightEvent lightEvents[24]{};
    static uint32_t lightEventCount = 0;
    static LightEvent lastLightState{};
    static uint64_t lastLightToken = 0;
    static std::atomic_flag lightEventsLock = ATOMIC_FLAG_INIT;

    static void TraceLightState(SafetyHookContext& regs, uintptr_t vehicle, uint64_t token, bool driver)
    {
        const auto byteAt = [](uintptr_t address) { return *reinterpret_cast<const uint8_t*>(address); };
        LightEvent state{ GetTickCount64(), driver, byteAt(vehicle + 0xF15), byteAt(vehicle + 0xF19),
            byteAt(vehicle + 0xF21), static_cast<uint8_t>(regs.eax & 0xFF), byteAt(regs.ebp + 0x28),
            byteAt(regs.ebp + 0x10), byteAt(regs.ebp + 0x14), regs.xmm1.f32[0], regs.xmm4.f32[0] };
        const bool changed = token != lastLightToken || state.driver != lastLightState.driver ||
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
                if (diagnosticsReady.load(std::memory_order_relaxed))
                    TraceLightState(regs, vehicle, token, true);
                // F74 takes about a second to climb back to 1 after getting in, and the beams
                // reached a third as far until it did.
                if (*reinterpret_cast<const uint8_t*>(imageBase + 0xC3CC4A))
                    KeepRadiusBonus(regs, vehicle);
            }
            else
            {
                auto expected = token;
                lastDrivenToken.compare_exchange_strong(expected, 0);
            }
            return;
        }
        if (lastDrivenToken.load() != token) return;
        if (diagnosticsReady.load(std::memory_order_relaxed))
            TraceLightState(regs, vehicle, token, false);
        if (!*reinterpret_cast<const uint8_t*>(imageBase + 0xC3CC4A)) return;
        const float intensity = *reinterpret_cast<const float*>(imageBase + 0xC3CC74);
        const float range = *reinterpret_cast<const float*>(imageBase + 0xC3CC78);
        if (!std::isfinite(intensity) || !std::isfinite(range) ||
            intensity <= 0 || range <= 0 || intensity > 10 || range > 10) return;
        regs.xmm1.f32[0] *= intensity;
        regs.xmm4.f32[0] *= range;
        KeepRadiusBonus(regs, vehicle);
        ++retainedSubmissions;
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
            << "\nradiusBonus=" << (pRadiusBonus ? *pRadiusBonus : -1.0f)
            << "\ntrackedVehicle=" << (lastDrivenToken.load() != 0) << '\n';

        // Oldest first. highBeam is the flag the function uses (vehicle+F19 & 2, or highBeamArg);
        // left and right its lamp arguments, the beams are only submitted for 1; intensity and
        // range are the multipliers before high beam scaling.
        while (lightEventsLock.test_and_set(std::memory_order_acquire)) {}
        const auto count = lightEventCount;
        const auto first = count > std::size(lightEvents) ? count - std::size(lightEvents) : 0;
        for (auto i = first; i < count; ++i)
        {
            const auto& e = lightEvents[i % std::size(lightEvents)];
            out << "t=" << e.time << " driver=" << e.driver << std::hex
                << " f15=" << unsigned(e.f15) << " f19=" << unsigned(e.f19) << " f21=" << unsigned(e.f21)
                << std::dec << " highBeam=" << unsigned(e.highBeam) << " highBeamArg=" << unsigned(e.highBeamArg)
                << " left=" << unsigned(e.left) << " right=" << unsigned(e.right) << " intensity=" << e.intensity << " range=" << e.range << '\n';
        }
        lightEventsLock.clear(std::memory_order_release);
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
    // vehicle+10C2 & 3 is the mode FORCE_CAR_LIGHTS sets, which the light code reads: 0 follows
    // the time of day, 1 keeps the lights off, 2 keeps them on. At the store, the tap now steps
    // on -> high beams -> off -> on, the lights forced on or off as it goes; where they still
    // follow the time of day, it counts as on or off by what that gives (vehicle+F15 & 1).
    static SafetyHookMid lightModeHook, highBeamTimeoutHook;
    static const uint32_t* pGameTime = nullptr;

    static void StepLightMode(SafetyHookContext& regs)
    {
        const auto vehicle = static_cast<uintptr_t>(regs.esi);
        auto& mode = *reinterpret_cast<uint8_t*>(vehicle + 0x10C2);
        const auto old = *reinterpret_cast<const uint8_t*>(vehicle + 0xF19);
        const bool on = (mode & 3) == 2 || ((mode & 3) == 0 && (*reinterpret_cast<const uint8_t*>(vehicle + 0xF15) & 1));
        auto lights = static_cast<uint8_t>(regs.ecx & 0xFF);
        if (!on)
        {
            mode = static_cast<uint8_t>((mode & ~3) | 2);
            lights &= ~2;
        }
        else if (old & 2)
        {
            mode = static_cast<uint8_t>((mode & ~3) | 1);
            lights &= ~2;
        }
        else
        {
            mode = static_cast<uint8_t>((mode & ~3) | 2);
            lights |= 2;
        }
        regs.ecx = (regs.ecx & ~0xFFu) | lights;
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
        const auto toggleAt = imageBase + 0x63F82F, timeoutAt = imageBase + 0x63F868;
        if (std::memcmp(reinterpret_cast<const void*>(toggleAt), toggle, sizeof(toggle)) ||
            std::memcmp(reinterpret_cast<const void*>(timeoutAt), timeout, sizeof(timeout)) ||
            *reinterpret_cast<const uint32_t*>(timeoutAt + sizeof(timeout)) != imageBase + 0xD735B4 ||
            *reinterpret_cast<const uint8_t*>(timeoutAt + sizeof(timeout) + 4) != 0x76)
        {
            lightModesStatus = "game code differs: " + DumpBytes(toggleAt, sizeof(toggle)) +
                " | " + DumpBytes(timeoutAt, sizeof(timeout) + 5);
            return;
        }
        pGameTime = reinterpret_cast<const uint32_t*>(imageBase + 0xD735B4);
        lightModeHook = safetyhook::create_mid(imageBase + 0x63F844, StepLightMode);
        highBeamTimeoutHook = safetyhook::create_mid(imageBase + 0x63F886, KeepForcedHighBeams);
        lightModesStatus = lightModeHook && highBeamTimeoutHook ? "installed" : "hook failed";
    }
}
