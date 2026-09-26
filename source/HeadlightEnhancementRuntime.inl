namespace HeadlightEnhancement
{
    static SafetyHookMid brightnessHook;
    static uintptr_t imageBase = 0;
    static std::atomic<uint64_t> lastDrivenToken{0};
    static std::atomic<uint32_t> retainedSubmissions{0};
    static bool brightnessInstalled = false, cutoffInstalled = false;
    static std::atomic<bool> diagnosticsReady{false};
    static std::filesystem::path logPath;

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
                lastDrivenToken.store(token);
            else
            {
                auto expected = token;
                lastDrivenToken.compare_exchange_strong(expected, 0);
            }
            return;
        }
        if (lastDrivenToken.load() != token ||
            !*reinterpret_cast<const uint8_t*>(imageBase + 0xC3CC4A)) return;
        const float intensity = *reinterpret_cast<const float*>(imageBase + 0xC3CC74);
        const float range = *reinterpret_cast<const float*>(imageBase + 0xC3CC78);
        if (!std::isfinite(intensity) || !std::isfinite(range) ||
            intensity <= 0 || range <= 0 || intensity > 10 || range > 10) return;
        regs.xmm1.f32[0] *= intensity;
        regs.xmm4.f32[0] *= range;
        ++retainedSubmissions;
    }

    // No projection-mask replacement in this candidate.
    static int SelectMask(int original, int) { return original; }
    static bool InstallCutoff(bool) { return false; }

    static void WriteDiagnostics()
    {
        if (!diagnosticsReady.load(std::memory_order_acquire)) return;
        static ULONGLONG last = 0;
        const auto now = GetTickCount64();
        if (logPath.empty() || now - last < 5000) return;
        last = now;
        std::ofstream out(logPath, std::ios::trunc);
        out << "Headlight candidate 24\nbrightnessInstalled=" << brightnessInstalled
            << "\ncutoffInstalled=0\nretainedSubmissions=" << retainedSubmissions.load()
            << "\ntrackedVehicle=" << (lastDrivenToken.load() != 0) << '\n';
    }

    static bool InstallBrightness(bool enabled)
    {
        if (!enabled) return false;
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
            return false;
        // Runs after original player-only scaling, before high-beam scaling.
        // Do not bypass or alter the original driver eligibility instructions.
        brightnessHook = safetyhook::create_mid(imageBase + 0x63FB77, RetainAfterExit);
        return static_cast<bool>(brightnessHook);
    }
}
