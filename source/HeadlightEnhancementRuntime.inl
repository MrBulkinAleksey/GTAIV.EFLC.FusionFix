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
    static uint8_t siteBytes[5]{};
    static std::atomic<bool> diagnosticsReady{false};
    static std::filesystem::path logPath;

    // Diagnostics: changes of the light state of the car the player drives or last drove, as
    // this function sees them, to find what turns its high beams off when the player gets out.
    struct LightEvent
    {
        ULONGLONG time;
        bool driver;
        uint8_t f15, f19, f21, highBeam, highBeamArg;
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
            regs.xmm1.f32[0], regs.xmm4.f32[0] };
        const bool changed = token != lastLightToken || state.driver != lastLightState.driver ||
            state.f15 != lastLightState.f15 || state.f19 != lastLightState.f19 ||
            state.highBeam != lastLightState.highBeam || state.highBeamArg != lastLightState.highBeamArg;
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
        ++retainedSubmissions;
    }

    // At the end of the same function the beams are submitted with 0x504 instead of 0x104 only
    // while the local player drives (CE 0xA4A9E0 compares the player with vehicle+F50). The car
    // the player just left kept the high beam flag and the brightness above, yet its high beams
    // went out, and that bit is all its submission still differs in. Runs after each of the
    // three calls: for that car, with no one at the wheel, the answer stays "the player drives".
    static SafetyHookMid ownBeamHooks[3];
    static std::atomic<uint32_t> keptOwnBeams{0};
    static std::string ownBeamStatus = "not checked";

    static void KeepOwnBeamAfterExit(SafetyHookContext& regs)
    {
        if (regs.eax & 0xFF) return;
        const auto vehicle = static_cast<uintptr_t>(regs.esi);
        const auto token = lastDrivenToken.load();
        if (!token || *reinterpret_cast<const uintptr_t*>(vehicle + 0xF50) ||
            VehicleToken(vehicle) != token) return;
        regs.eax |= 1;
        ++keptOwnBeams;
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
            << "\nownBeamStatus=" << ownBeamStatus
            << "\nkeptOwnBeams=" << keptOwnBeams.load()
            << "\ntrackedVehicle=" << (lastDrivenToken.load() != 0) << '\n';

        // Oldest first. highBeam is the flag the function uses (vehicle+F19 & 2, or highBeamArg);
        // intensity and range are the multipliers before high beam scaling.
        while (lightEventsLock.test_and_set(std::memory_order_acquire)) {}
        const auto count = lightEventCount;
        const auto first = count > std::size(lightEvents) ? count - std::size(lightEvents) : 0;
        for (auto i = first; i < count; ++i)
        {
            const auto& e = lightEvents[i % std::size(lightEvents)];
            out << "t=" << e.time << " driver=" << e.driver << std::hex
                << " f15=" << unsigned(e.f15) << " f19=" << unsigned(e.f19) << " f21=" << unsigned(e.f21)
                << std::dec << " highBeam=" << unsigned(e.highBeam) << " highBeamArg=" << unsigned(e.highBeamArg)
                << " intensity=" << e.intensity << " range=" << e.range << '\n';
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

    // Without it the low beams still keep the player's brightness.
    static void InstallOwnBeam()
    {
        // call 0xA4A9E0 followed by movss xmm0, [esp+disp8], the instruction hooked.
        constexpr uint32_t calls[]{ 0x63FDBB, 0x63FE40, 0x63FEB1 };
        constexpr uint8_t movss[]{ 0xF3,0x0F,0x10,0x44,0x24 };
        for (const auto call : calls)
        {
            const auto site = imageBase + call;
            if (*reinterpret_cast<const uint8_t*>(site) != 0xE8 ||
                site + 5 + *reinterpret_cast<const int32_t*>(site + 1) != imageBase + 0x64A9E0 ||
                std::memcmp(reinterpret_cast<const void*>(site + 5), movss, sizeof(movss)))
            {
                ownBeamStatus = "game code differs: " + DumpBytes(site, 11);
                return;
            }
        }
        for (size_t i = 0; i < std::size(calls); ++i)
        {
            ownBeamHooks[i] = safetyhook::create_mid(imageBase + calls[i] + 5, KeepOwnBeamAfterExit);
            if (!ownBeamHooks[i])
            {
                ownBeamStatus = "hook failed";
                return;
            }
        }
        ownBeamStatus = "installed";
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
        brightnessStatus = "installed";
        InstallOwnBeam();
        return true;
    }
}
