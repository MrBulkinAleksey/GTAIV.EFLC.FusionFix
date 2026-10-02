// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

namespace HeadlightEnhancement
{
    static SafetyHookMid brightnessHook;
    static uintptr_t imageBase = 0;
    static std::atomic<uint32_t> retainedSubmissions{0};
    static bool brightnessInstalled = false;
    // Why the hook is or is not in place: another plugin that patches the same game code turns it
    // off at install.
    static std::string brightnessStatus = "not checked";
    // The radius of the beams is (floor(vehicle+F74) * bonus + base) * range. F74 climbs to 1 while
    // a player sits in the car and falls back to 0 once none does (CE 0xA4EA3B), so the bonus,
    // 20 m on top of a base of 10 m, is the player's alone: it was lost right after getting out,
    // a third of the driving reach with ConsistentBrightness, and for a second after getting in.
    // Both read from the instructions.
    static const float* pRadiusBonus = nullptr;
    static const float* pRadiusBase = nullptr;
    static std::string lightModesStatus = "off in the ini";
    static std::string splitBeamsStatus = "off in the ini";
    static std::string offscreenLightsStatus = "off in the ini";
    static std::atomic<bool> diagnosticsReady{false};
    static std::filesystem::path logPath;

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
        const auto vehicle = static_cast<uintptr_t>(regs.esi);
        const auto driver = *reinterpret_cast<const uintptr_t*>(vehicle + 0xF50);
        if (driver)
        {
            // Same driver classification used by the original multiplier block.
            // Occupied cars have ALREADY received their original scaling.
            // F74 takes about a second to climb back to 1 after getting in, and the beams
            // reached a third as far until it did.
            if (!*reinterpret_cast<const uint8_t*>(driver + 0x218) &&
                 *reinterpret_cast<const uint8_t*>(driver + 0x219) &&
                 *reinterpret_cast<const uint8_t*>(imageBase + 0xC3CC4A))
                KeepRadiusBonus(regs, vehicle);
            return;
        }
        if (!PlayerCar::IsLast(vehicle) ||
            !*reinterpret_cast<const uint8_t*>(imageBase + 0xC3CC4A)) return;
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
        out << "brightnessInstalled=" << brightnessInstalled
            << "\nbrightnessStatus=" << brightnessStatus
            << "\nretainedSubmissions=" << retainedSubmissions.load()
            << "\nlightModesStatus=" << lightModesStatus
            << "\nsplitBeamsStatus=" << splitBeamsStatus
            << "\noffscreenLightsStatus=" << offscreenLightsStatus
            << "\ntrackedVehicle=" << (PlayerCar::Last() != 0) << '\n';
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
        const auto token = PlayerCar::VehicleToken(vehicle);
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
    }

    // Runs at the read of vehicle+10C2 in the light code, esi the vehicle.
    static void ApplyLightMode(SafetyHookContext& regs)
    {
        if (!nextLightMode) return;
        const auto vehicle = static_cast<uintptr_t>(regs.esi);
        const auto entry = FindLightMode(PlayerCar::VehicleToken(vehicle));
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

    // Both lit lamps of a car make one beam from the point between them (CE 0xA3FCA5: the two
    // lamp bones averaged, then 0xA3E070), so a hand at the car's edge shadowed the far half of
    // the beam. For a car within a few metres of the player on foot, the call at 0xA3FE11 now
    // moves that beam: mode 2 slides it from the middle towards the lamp on the player's side,
    // one beam and one shadow as before, as far as the player stands out to that side;
    // mode 1 splits it into a beam from each lamp at half the intensity, keyed by the car and the
    // car + 1 as the game keys a single lamp, two softer shadows apart. Further away, or while
    // the player drives, one beam from between them as before.
    using SubmitBeams = void(__cdecl*)(void*, float*, float*, void*, float, float, float, float, int, int,
                                       uintptr_t, int);
    static SubmitBeams submitBeams = nullptr;
    static constexpr float SplitBeamsDistance = 6.0f;
    static int splitBeamsMode = 0;

    static void __cdecl SubmitSplitBeams(void* matrix, float* position, float* direction, void* colour,
        float intensity, float radius, float a7, float a8, int a9, int a10, uintptr_t vehicle, int player)
    {
        const auto ped = CPlayer::getLocalPlayerPed ? CPlayer::getLocalPlayerPed() : 0;
        const auto carMatrix = CEntity::GetMatrix(vehicle);
        const auto pedMatrix = CEntity::GetMatrix(ped);
        bool split = bHeadlightShadows && carMatrix && pedMatrix && CPlayer::findPlayerCar && !CPlayer::findPlayerCar();
        if (split)
        {
            const float dx = carMatrix[12] - pedMatrix[12], dy = carMatrix[13] - pedMatrix[13], dz = carMatrix[14] - pedMatrix[14];
            split = dx * dx + dy * dy + dz * dz < SplitBeamsDistance * SplitBeamsDistance;
        }
        if (!split)
            return submitBeams(matrix, position, direction, colour, intensity, radius, a7, a8, a9, a10, vehicle, player);

        // The caller's frame: the world position at position[0..2], the lamps' bones in car space
        // at position + 0x30 and + 0x40 (x, -, z), and their shared forward offset at position - 0xC.
        const auto bytes = reinterpret_cast<const uint8_t*>(position);
        const float y = *reinterpret_cast<const float*>(bytes - 0xC);
        const auto& m = carMatrix;
        const auto lampAt = [&](int lamp, float* world)
        {
            const auto bone = reinterpret_cast<const float*>(bytes + (lamp ? 0x30 : 0x40));
            const float x = bone[0], z = bone[2];
            world[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
            world[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
            world[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
            world[3] = position[3];
            return x;
        };
        float world[4];
        if (splitBeamsMode == 1)
        {
            for (int lamp = 0; lamp < 2; ++lamp)
            {
                lampAt(lamp, world);
                submitBeams(matrix, world, direction, colour, intensity * 0.5f, radius, a7, a8, a9, a10,
                            vehicle + lamp, player);
            }
            return;
        }
        // Slid from the middle towards the lamp on the player's side, as far as the player stands
        // across the car (its matrix's x axis) out of the lamp's own offset: the middle as the game
        // has it with the player in front, the lamp itself at the car's edge, back to the middle
        // over the last 2 m before SplitBeamsDistance.
        float other[4];
        const float x0 = lampAt(0, world), x1 = lampAt(1, other);
        const float halfWidth = std::fabs(x0 - x1) * 0.5f;
        if (!(halfWidth > 0.05f))
            return submitBeams(matrix, position, direction, colour, intensity, radius, a7, a8, a9, a10, vehicle, player);
        const float px = pedMatrix[12] - m[12], py = pedMatrix[13] - m[13], pz = pedMatrix[14] - m[14];
        const float across = px * m[0] + py * m[1] + pz * m[2];
        const float distance = std::sqrt(px * px + py * py + pz * pz);
        const float closeness = std::clamp((SplitBeamsDistance - distance) / 2.0f, 0.0f, 1.0f);
        const float t = std::clamp(across / halfWidth, -1.0f, 1.0f) * closeness; // -1 the lamp at -x, +1 at +x
        const float* plus = x0 >= x1 ? world : other;
        const float* minus = x0 >= x1 ? other : world;
        const float* lamp = t >= 0.0f ? plus : minus;
        const float k = std::fabs(t);
        float at[4];
        for (int i = 0; i < 3; ++i)
            at[i] = position[i] + (lamp[i] - position[i]) * k;
        at[3] = position[3];
        submitBeams(matrix, at, direction, colour, intensity, radius, a7, a8, a9, a10, vehicle, player);
    }

    static void InstallSplitBeams(int mode)
    {
        if (mode != 1 && mode != 2) return;
        splitBeamsMode = mode;
        imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto call = imageBase + 0x63FE11;
        // push dword ptr [ebp+24] / call 0xA3E070 / add esp, 30
        constexpr uint8_t before[]{0xFF,0x75,0x24,0xE8}, after[]{0x83,0xC4,0x30};
        if (std::memcmp(reinterpret_cast<const void*>(call - 3), before, sizeof(before)) ||
            call + 5 + *reinterpret_cast<const int32_t*>(call + 1) != imageBase + 0x63E070 ||
            std::memcmp(reinterpret_cast<const void*>(call + 5), after, sizeof(after)))
        {
            splitBeamsStatus = "game code differs: " + DumpBytes(call - 3, 11);
            return;
        }
        submitBeams = reinterpret_cast<SubmitBeams>(imageBase + 0x63E070);
        injector::MakeCALL(call, SubmitSplitBeams, true);
        splitBeamsStatus = mode == 1 ? "installed, two beams" : "installed, beam slid towards the player's side";
    }

    // A car's lights, its headlight beams among them, are only made while the car was seen by one
    // of the frame's render phases (CE 0xA43616 tests vehicle+8 against the phase mask 0x159B75C).
    // Outdoors some phase still takes in a car behind the camera; in a tunnel none does, so the
    // beam of a car behind the camera went out and came back on once the car was in view. Map
    // lights keep theirs within 35 m of the camera whether seen or not (CE 0xC1DBA4); mode 1 does
    // the same for the player's car (PlayerCar::Last), mode 2 for every car.
    static constexpr float OffscreenLightsDistance = 35.0f;
    static int offscreenLightsMode = 0;
    static const uint32_t* pPhaseMask = nullptr;
    static const float* pCameraPosition = nullptr;

    // Called in place of the mask test with the car in ECX; ECX and EDX are dead after it.
    static bool __fastcall MakesLights(uintptr_t vehicle)
    {
        if (*reinterpret_cast<const uint32_t*>(vehicle + 8) & *pPhaseMask) return true;
        if (offscreenLightsMode == 1 && !PlayerCar::IsLast(vehicle)) return false;
        float position[3];
        if (!CEntity::GetPosition(vehicle, position)) return false;
        const float dx = position[0] - pCameraPosition[0], dy = position[1] - pCameraPosition[1],
                    dz = position[2] - pCameraPosition[2];
        return dx * dx + dy * dy + dz * dz <= OffscreenLightsDistance * OffscreenLightsDistance;
    }

    static void InstallOffscreenLights(int mode)
    {
        if (mode != 1 && mode != 2) return;
        offscreenLightsMode = mode;
        imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto at = imageBase + 0x643616;
        // mov eax, [0x159B75C] / test [esi+8], eax / je 0xA44CAA
        constexpr uint8_t test[]{0x85,0x46,0x08,0x0F,0x84};
        if (*reinterpret_cast<const uint8_t*>(at) != 0xA1 ||
            *reinterpret_cast<const uint32_t*>(at + 1) != imageBase + 0x119B75C ||
            std::memcmp(reinterpret_cast<const void*>(at + 5), test, sizeof(test)) ||
            at + 14 + *reinterpret_cast<const int32_t*>(at + 10) != imageBase + 0x644CAA)
        {
            offscreenLightsStatus = "game code differs: " + DumpBytes(at, 14);
            return;
        }
        pPhaseMask = reinterpret_cast<const uint32_t*>(imageBase + 0x119B75C);
        pCameraPosition = reinterpret_cast<const float*>(imageBase + 0xE8E340);
        // call MakesLights / test al, al / je 0xA44CAA / nop
        injector::MakeCALL(at, MakesLights, true);
        constexpr uint8_t branch[]{0x84,0xC0,0x0F,0x84};
        for (size_t i = 0; i < sizeof(branch); ++i)
            injector::WriteMemory<uint8_t>(at + 5 + i, branch[i], true);
        injector::WriteMemory<int32_t>(at + 9, static_cast<int32_t>(imageBase + 0x644CAA - (at + 13)), true);
        injector::WriteMemory<uint8_t>(at + 13, 0x90, true);
        offscreenLightsStatus = mode == 1 ? "installed, the player's car" : "installed, every car";
    }
}
