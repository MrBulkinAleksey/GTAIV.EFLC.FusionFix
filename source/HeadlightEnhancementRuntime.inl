// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

namespace HeadlightEnhancement
{
    static SafetyHookMid brightnessHook;
    static const uintptr_t imageBase = GameBase();
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
    static fusionfix::DiagnosticsLog log;

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

    // The game code a hook relies on, checked before it is changed. Offsets are from the image
    // base; a failed check names the bytes found, so the log shows what another plugin or another
    // game version put there.
    class CodeCheck
    {
    public:
        template <size_t N>
        CodeCheck& Bytes(uintptr_t offset, const uint8_t (&bytes)[N])
        {
            return Expect(offset, N, !std::memcmp(reinterpret_cast<const void*>(imageBase + offset), bytes, N));
        }

        // A 4-byte absolute address operand.
        CodeCheck& Address(uintptr_t offset, uintptr_t target)
        {
            return Expect(offset, 4, *reinterpret_cast<const uint32_t*>(imageBase + offset) == imageBase + target);
        }

        // A 4-byte relative branch operand, counted from its end.
        CodeCheck& Branch(uintptr_t offset, uintptr_t target)
        {
            return Expect(offset, 4, offset + 4 + *reinterpret_cast<const int32_t*>(imageBase + offset) == target);
        }

        explicit operator bool() const { return differs.empty(); }
        std::string Status() const { return "game code differs: " + differs; }

    private:
        CodeCheck& Expect(uintptr_t offset, size_t count, bool matches)
        {
            if (!matches)
                differs += (differs.empty() ? "" : " | ") + DumpBytes(imageBase + offset, count);
            return *this;
        }

        std::string differs;
    };

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
        log.Write(std::ios::trunc, [](std::ofstream& out, uint64_t) {
            out << "brightnessInstalled=" << brightnessInstalled
                << "\nbrightnessStatus=" << brightnessStatus
                << "\nretainedSubmissions=" << retainedSubmissions.load()
                << "\nlightModesStatus=" << lightModesStatus
                << "\nsplitBeamsStatus=" << splitBeamsStatus
                << "\noffscreenLightsStatus=" << offscreenLightsStatus
                << "\ntrackedVehicle=" << (PlayerCar::Last() != 0) << '\n';
        });
    }

    static bool InstallBrightness(bool enabled)
    {
        if (!enabled)
        {
            brightnessStatus = "off in the ini";
            return false;
        }
        constexpr uint8_t driverBlock[]{
            0x74,0x33,0x8B,0x8E,0x50,0x0F,0,0,0x85,0xC9,0x74,0x29,
            0x80,0xB9,0x18,0x02,0,0,0,0x75,0x20,
            0x80,0xB9,0x19,0x02,0,0,0,0x74,0x17
        };
        // cmp byte ptr [0xC3CC49], 0
        const auto check = CodeCheck().Bytes(0x63FB42, driverBlock).Bytes(0x63FB77, {0x80,0x3D})
            .Address(0x63FB79, 0xC3CC49).Bytes(0x63FB7D, {0x00});
        if (!check)
        {
            brightnessStatus = check.Status();
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
        // The ramp read: movss xmm2, [esi+F74].
        if (CodeCheck().Bytes(0x63FC45, {0xF3,0x0F,0x59,0x2D}).Bytes(0x63FC51, {0xF3,0x0F,0x58,0x2D})
                .Bytes(0x63FBAB, {0xF3,0x0F,0x10,0x96,0x74,0x0F,0x00,0x00}))
        {
            pRadiusBonus = *reinterpret_cast<const float* const*>(imageBase + 0x63FC49);
            pRadiusBase = *reinterpret_cast<const float* const*>(imageBase + 0x63FC55);
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
        constexpr uintptr_t timeoutAt = 0x63F868, gameTimeAt = timeoutAt + sizeof(timeout);
        const auto check = CodeCheck().Bytes(0x63F82F, toggle).Bytes(timeoutAt, timeout)
            .Address(gameTimeAt, 0xD735B4).Bytes(gameTimeAt + 4, {0x76}).Bytes(0x643867, read);
        if (!check)
        {
            lightModesStatus = check.Status();
            return;
        }
        pGameTime = reinterpret_cast<const uint32_t*>(imageBase + 0xD735B4);
        lightModeHook = safetyhook::create_mid(imageBase + 0x63F844, StepLightMode);
        lightModeApplyHook = safetyhook::create_mid(imageBase + 0x643867, ApplyLightMode);
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

    // How far the beams have moved out of the middle, easing towards where the player stands
    // instead of following him at once: getting in or out switched it between the lamp at the
    // door and the middle in one frame. Mode 2: -1 the lamp at -x to +1 the lamp at +x; mode 1:
    // 0 one beam to 1 a beam from each lamp. A car with none is in the middle.
    struct BeamSlide
    {
        uint64_t token = 0;
        float t = 0.0f;
        int32_t timeMs = 0;
    };
    static std::array<BeamSlide, 4> beamSlides{};
    static constexpr float BeamSlidePerSecond = 2.5f; // lamp to middle in 0.4 s
    static constexpr int32_t BeamSlideStaleMs = 1000; // lights off or out of range for longer start anew

    static float EaseBeamSlide(uintptr_t vehicle, float target)
    {
        if (!CTimer::m_snTimeInMilliseconds) return target;
        const int32_t now = *CTimer::m_snTimeInMilliseconds;
        BeamSlide* slide = nullptr;
        for (auto& entry : beamSlides)
            if (entry.token && static_cast<uintptr_t>(static_cast<uint32_t>(entry.token)) == vehicle)
                slide = &entry;
        const auto token = (slide || target != 0.0f) ? PlayerCar::VehicleToken(vehicle) : 0;
        if (slide && (slide->token != token || now - slide->timeMs > BeamSlideStaleMs))
        {
            *slide = {};
            slide = nullptr;
        }
        if (!slide)
        {
            if (target == 0.0f || !token) return 0.0f;
            slide = &beamSlides[0];
            for (auto& entry : beamSlides)
                if (!entry.token || entry.timeMs - slide->timeMs < 0)
                    slide = &entry;
            *slide = { token, 0.0f, now };
        }
        const float step = BeamSlidePerSecond * std::clamp(now - slide->timeMs, 0, 100) / 1000.0f;
        slide->t += std::clamp(target - slide->t, -step, step);
        slide->timeMs = now;
        const float t = slide->t;
        if (t == 0.0f && target == 0.0f)
            *slide = {};
        return t;
    }

    static void __cdecl SubmitSplitBeams(void* matrix, float* position, float* direction, void* colour,
        float intensity, float radius, float a7, float a8, int a9, int a10, uintptr_t vehicle, int player)
    {
        const auto m = CEntity::GetMatrix(vehicle);
        if (!bHeadlightShadows || !m)
            return submitBeams(matrix, position, direction, colour, intensity, radius, a7, a8, a9, a10, vehicle, player);

        // The caller's frame: the world position at position[0..2], the lamps' bones in car space
        // at position + 0x30 and + 0x40 (x, -, z), and their shared forward offset at position - 0xC.
        const auto bytes = reinterpret_cast<const uint8_t*>(position);
        const float y = *reinterpret_cast<const float*>(bytes - 0xC);
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
        float lamps[2][4];
        const float x0 = lampAt(0, lamps[0]), x1 = lampAt(1, lamps[1]);
        const float halfWidth = std::fabs(x0 - x1) * 0.5f;

        // Where the beams go for where the player stands: on foot within SplitBeamsDistance, as far
        // as he stands across the car (its matrix's x axis) out of the lamp's own offset for mode 2,
        // back to the middle over the last 2 m before SplitBeamsDistance.
        float target = 0.0f;
        const auto ped = CPlayer::getLocalPlayerPed ? CPlayer::getLocalPlayerPed() : 0;
        const auto pedMatrix = CEntity::GetMatrix(ped);
        if (halfWidth > 0.05f && pedMatrix && CPlayer::findPlayerCar && !CPlayer::findPlayerCar())
        {
            const float px = pedMatrix[12] - m[12], py = pedMatrix[13] - m[13], pz = pedMatrix[14] - m[14];
            const float distance = std::sqrt(px * px + py * py + pz * pz);
            const float closeness = std::clamp((SplitBeamsDistance - distance) / 2.0f, 0.0f, 1.0f);
            const float across = px * m[0] + py * m[1] + pz * m[2];
            target = splitBeamsMode == 1 ? closeness : std::clamp(across / halfWidth, -1.0f, 1.0f) * closeness;
        }
        const float t = EaseBeamSlide(vehicle, target);
        if (t == 0.0f)
            return submitBeams(matrix, position, direction, colour, intensity, radius, a7, a8, a9, a10, vehicle, player);

        const auto toward = [&](const float* lamp, float k, float* at)
        {
            for (int i = 0; i < 3; ++i)
                at[i] = position[i] + (lamp[i] - position[i]) * k;
            at[3] = position[3];
        };
        float at[4];
        if (splitBeamsMode == 1)
        {
            for (int lamp = 0; lamp < 2; ++lamp)
            {
                toward(lamps[lamp], t, at);
                submitBeams(matrix, at, direction, colour, intensity * 0.5f, radius, a7, a8, a9, a10,
                            vehicle + lamp, player);
            }
            return;
        }
        const float* plus = x0 >= x1 ? lamps[0] : lamps[1];
        const float* minus = x0 >= x1 ? lamps[1] : lamps[0];
        toward(t >= 0.0f ? plus : minus, std::fabs(t), at);
        submitBeams(matrix, at, direction, colour, intensity, radius, a7, a8, a9, a10, vehicle, player);
    }

    static void InstallSplitBeams(int mode)
    {
        if (mode != 1 && mode != 2) return;
        splitBeamsMode = mode;
        const auto call = imageBase + 0x63FE11;
        // push dword ptr [ebp+24] / call 0xA3E070 / add esp, 30
        const auto check = CodeCheck().Bytes(0x63FE0E, {0xFF,0x75,0x24,0xE8}).Branch(0x63FE12, 0x63E070)
            .Bytes(0x63FE16, {0x83,0xC4,0x30});
        if (!check)
        {
            splitBeamsStatus = check.Status();
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
    // the same for the player's car (PlayerCar::Last), mode 2 for every car. Cars take a distance of
    // their own, by default as far as high beams reach (75 to 98 m): a car 35 m behind the camera
    // still lit the road ahead of it.
    static float offscreenLightsDistance = 100.0f;
    static int offscreenLightsMode = 0;
    static const uint32_t* pPhaseMask = nullptr;

    // Called in place of the mask test with the car in ECX; ECX and EDX are dead after it.
    static bool OffscreenLightsReach(uintptr_t vehicle)
    {
        if (offscreenLightsMode == 1 && !PlayerCar::IsLast(vehicle)) return false;
        float position[3], camera[3];
        if (!CEntity::GetPosition(vehicle, position) || !GameCamera::Position(camera)) return false;
        const float dx = position[0] - camera[0], dy = position[1] - camera[1], dz = position[2] - camera[2];
        return dx * dx + dy * dy + dz * dz <= offscreenLightsDistance * offscreenLightsDistance;
    }

    static bool __fastcall MakesLights(uintptr_t vehicle)
    {
        const bool seen = (*reinterpret_cast<const uint32_t*>(vehicle + 8) & *pPhaseMask) != 0;
        const bool made = seen || OffscreenLightsReach(vehicle);
        if (!BeamTrace::path.empty() && PlayerCar::IsLast(vehicle))
        {
            if (seen) BeamTrace::Mark(BeamTrace::CarSeen);
            if (made) BeamTrace::Mark(BeamTrace::LightsMade);
        }
        return made;
    }

    static void InstallOffscreenLights(int mode, float distance)
    {
        if (mode != 1 && mode != 2) return;
        offscreenLightsMode = mode;
        offscreenLightsDistance = distance;
        const auto at = imageBase + 0x643616;
        // mov eax, [0x159B75C] / test [esi+8], eax / je 0xA44CAA
        const auto check = CodeCheck().Bytes(0x643616, {0xA1}).Address(0x643617, 0x119B75C)
            .Bytes(0x64361B, {0x85,0x46,0x08,0x0F,0x84}).Branch(0x643620, 0x644CAA);
        if (!check)
        {
            offscreenLightsStatus = check.Status();
            return;
        }
        pPhaseMask = reinterpret_cast<const uint32_t*>(imageBase + 0x119B75C);
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
