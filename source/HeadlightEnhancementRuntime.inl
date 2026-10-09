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
    static std::string shadowOriginStatus = "off in the ini";
    static std::atomic<uint32_t> shadowOriginsMoved{0}, shadowOriginCachesDropped{0};
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
        log.Write(std::ios::trunc, [](std::ostream& out, uint64_t) {
            out << "brightnessInstalled=" << brightnessInstalled
                << "\nbrightnessStatus=" << brightnessStatus
                << "\nretainedSubmissions=" << retainedSubmissions.load()
                << "\nlightModesStatus=" << lightModesStatus
                << "\nshadowOriginStatus=" << shadowOriginStatus
                << "\nshadowOriginsMoved=" << shadowOriginsMoved.load()
                << "\nshadowOriginCachesDropped=" << shadowOriginCachesDropped.load()
                << "\nnearConeStatus=" << nearConeStatus
                << "\nnearConeBeams=" << nearConeBeams.load()
                << "\nnearConeAdjusted=" << nearConeAdjusted.load()
                << "\nnearConeConesInDegrees=" << nearConeConesInDegrees.load()
                << "\nnearConeLastCones=" << nearConeLastInner.load() << ' ' << nearConeLastOuter.load()
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
    // lamp bones averaged, then 0xA3E070), and its shadow is drawn from there too, so a hand by
    // one lamp, a few centimetres from that point, took up half of what it sees and cut away
    // half of the beam with a hard edge down the middle. Sliding the beam (191e631, dropped) or
    // only its shadow (c49f045) towards the lamp on the player's side moved the cut to the other
    // side of the hand: anything right by the point a shadow is drawn from cuts it in half.
    // The shadow of every car's headlights is now drawn from further back along the beam, inside
    // the car, so a hand at a lamp is that far from it and casts a shadow of its own size, while
    // things further out are hardly shadowed differently. The light stays where it is: the shadow
    // selection copies the light's position into its slot (0x92810D, slots at 0x119F100 +
    // n * 0x110, position at +0xC0, direction at +0xD0, radius at +0xE0), and both the shadow map
    // (0xD784C6 -> 0x925070 / 0x924E50, the paraboloid matrices at +0x40) and the lighting's
    // lookup of it are built from the slot, while the light is lit from its own position.
    static float shadowOriginBack = 0.0f;
    static SafetyHookMid shadowOriginHook;

    // A slot moved last time and not filled anew since (the selection skips frames, leaving its
    // slots as they were) still holds the moved position and radius, so it moves from those it
    // had before.
    struct MovedSlot
    {
        uint32_t key = 0;
        float base[4]{}; // position, radius
        float moved[4]{};
    };
    static std::array<MovedSlot, 8> movedSlots{};

    static bool IsHeadlightBeam(uint32_t key)
    {
        const auto lights = *reinterpret_cast<const rage::CLightSource* const*>(imageBase + 0xC3EED8);
        const auto count = *reinterpret_cast<const uint32_t*>(imageBase + 0x110E240);
        if (!lights) return false;
        constexpr uint32_t beam = rage::LF_VEHICLE | rage::LF_DYNAMIC_SHADOW;
        for (uint32_t i = 0; i < count && i < 0x280; ++i)
            if (static_cast<uint32_t>(lights[i].mCastShadows) == key)
                return lights[i].mType == rage::LT_SPOT && (lights[i].mFlags & beam) == beam;
        return false;
    }

    // At the end of the shadow selection (CE 0x9280C1, also where it leaves early on the frames
    // it skips). The cached map of the static scene a slot may start from was drawn from the
    // light's own position and would not match, so a moved slot draws without it (0x925BD0
    // clears the map instead) and draws the static scene itself.
    static void MoveShadowOrigins(SafetyHookContext&)
    {
        for (uint32_t n = 1; n < 8; ++n)
        {
            const auto slot = imageBase + 0xD9F100 + n * 0x110;
            auto position = reinterpret_cast<float*>(slot + 0xC0);
            auto& radius = *reinterpret_cast<float*>(slot + 0xE0);
            const auto direction = reinterpret_cast<const float*>(slot + 0xD0);
            const auto key = *reinterpret_cast<const uint32_t*>(slot + 0xF8);
            const bool active = *reinterpret_cast<const uint8_t*>(slot + 0xED) != 0;
            auto& moved = movedSlots[n];
            float base[4] = { position[0], position[1], position[2], radius };
            const float now[4] = { position[0], position[1], position[2], radius };
            if (moved.key && moved.key == key && !std::memcmp(now, moved.moved, sizeof(now)))
                std::copy(std::begin(moved.base), std::end(moved.base), base);
            moved = {};
            if (!active || !key || !IsHeadlightBeam(key))
            {
                std::copy(base, base + 3, position);
                radius = base[3];
                continue;
            }
            for (int i = 0; i < 3; ++i)
                position[i] = base[i] - direction[i] * shadowOriginBack;
            radius = base[3] + shadowOriginBack;
            moved.key = key;
            std::copy(std::begin(base), std::end(base), moved.base);
            std::copy(position, position + 3, moved.moved);
            moved.moved[3] = radius;
            auto& cache = *reinterpret_cast<int32_t*>(slot + 0xF0);
            if (cache != -1)
            {
                cache = -1;
                ++shadowOriginCachesDropped;
            }
            ++shadowOriginsMoved;
        }
    }

    static void InstallShadowOrigin(float back)
    {
        if (!(back > 0.0f)) return;
        shadowOriginBack = back;
        // mov [esi-8], eax (the slot's position from the light's) at 0x92810D, the slots' bounds
        // at 0x928065 and 0x9280B9, the light list at 0x92807C, and mov ecx, [esp+BC] where the
        // selection ends
        const auto check = CodeCheck()
            .Bytes(0x52810D, {0x89,0x46,0xF8}).Bytes(0x528065, {0xBE}).Address(0x528066, 0xD9F2D8)
            .Bytes(0x5280B9, {0x81,0xFE}).Address(0x5280BB, 0xD9FA48)
            .Bytes(0x52807C, {0x8B,0x0D}).Address(0x52807E, 0xC3EED8)
            .Bytes(0x5280C1, {0x8B,0x8C,0x24,0xBC,0x00,0x00,0x00});
        if (!check)
        {
            shadowOriginStatus = check.Status();
            return;
        }
        auto hook = safetyhook::MidHook::create(imageBase + 0x5280C1, MoveShadowOrigins);
        if (!hook)
        {
            shadowOriginStatus = "hook failed";
            return;
        }
        shadowOriginHook = std::move(*hook);
        shadowOriginStatus = "installed";
    }

    // Experiment: a ped right by the lamps still cuts the beam's shadow, so the beam itself gives way
    // to them. Within NearConeReach of the lamps a ped on one side moves that side's edge of the
    // beam in by up to NearConeCut, while the edge on the other side stays where it was: the cone
    // narrows by half of that and turns away by the other half (sliding the whole beam, 191e631,
    // moved the side nothing covered too). Peds on both sides move both edges in.
    // The game adds the beam of both lamps at one call (CE 0xA3FE11 -> 0xA3E070 -> 0xABCC50, which
    // appends it to the frame's light list [0x103EEDC], its count at 0x154CBBC); right after it,
    // with the car in esi, the light just added is turned and narrowed. Each car eases towards
    // what it sees at NearConeSpeed per second, so the beam does not jump with a hand.
    struct NearConeSettings
    {
        float cut = 20.0f;     // degrees an edge moves in with someone right at the lamps on its side
        float reach = 1.5f;    // metres from the lamps where it starts
        float full = 0.6f;     // metres from the lamps where it is all there
        float spread = 0.7f;   // half the spacing of the lamps, metres
        float speed = 4.0f;    // easing per second
    };
    static NearConeSettings nearCone{};
    static std::string nearConeStatus = "off in the ini";
    static SafetyHookMid nearConeBeforeHook, nearConeAfterHook;
    static uint32_t nearConeCount = 0;
    static std::atomic<uint32_t> nearConeBeams{0}, nearConeAdjusted{0}, nearConeConesInDegrees{0};
    static std::atomic<float> nearConeLastOuter{0.0f}, nearConeLastInner{0.0f};

    struct NearConeCar
    {
        uintptr_t vehicle = 0;
        float left = 0.0f, right = 0.0f;
        std::chrono::steady_clock::time_point seen{};
    };
    static std::array<NearConeCar, 32> nearConeCars{};

    static NearConeCar& NearConeState(uintptr_t vehicle, std::chrono::steady_clock::time_point now)
    {
        NearConeCar* oldest = &nearConeCars[0];
        for (auto& car : nearConeCars)
        {
            if (car.vehicle == vehicle) return car;
            if (car.seen < oldest->seen) oldest = &car;
        }
        *oldest = {};
        oldest->vehicle = vehicle;
        oldest->seen = now;
        return *oldest;
    }

    // How much of each side of the beam the peds by its lamps take, 0 to 1.
    static void NearConeSides(const rage::CLightSource& light, float& left, float& right)
    {
        left = right = 0.0f;
        const auto pool = CPed::GetPedPool();
        if (!pool || !pool->m_aStorage || !pool->m_aFlags || pool->m_nSize <= 0 || pool->m_nSize > 4096 ||
            pool->m_nStorageSize < 0x24 || pool->m_nStorageSize > 0x10000)
            return;
        const float fx = light.mDirection.x, fy = light.mDirection.y;
        const float flen = std::sqrt(fx * fx + fy * fy);
        if (!(flen > 1e-3f)) return;
        const float forward[2] = { fx / flen, fy / flen };
        const float rightAxis[2] = { forward[1], -forward[0] };
        const float fade = (std::max)(nearCone.reach - nearCone.full, 0.01f);
        for (int32_t i = 0; i < pool->m_nSize; ++i)
        {
            const auto ped = reinterpret_cast<uintptr_t>(pool->GetSlot(i));
            float position[3];
            if (!ped || !CEntity::GetPosition(ped, position)) continue;
            const float d[3] = { position[0] - light.mPosition.x, position[1] - light.mPosition.y,
                                 position[2] - light.mPosition.z };
            // A ped's position is about a metre above its feet, the lamps lower.
            if (d[2] < -1.5f || d[2] > 2.0f) continue;
            const float f = d[0] * forward[0] + d[1] * forward[1];
            const float s = d[0] * rightAxis[0] + d[1] * rightAxis[1];
            // Those inside the car or behind its front are out of the beam.
            if (f < -0.6f) continue;
            const float distance = std::hypot((std::max)(f, 0.0f), (std::max)(std::abs(s) - nearCone.spread, 0.0f));
            const float w = std::clamp((nearCone.reach - distance) / fade, 0.0f, 1.0f);
            if (w <= 0.0f) continue;
            // Right in the middle counts for both sides.
            right = (std::max)(right, w * std::clamp(0.5f + s / 0.6f, 0.0f, 1.0f));
            left = (std::max)(left, w * std::clamp(0.5f - s / 0.6f, 0.0f, 1.0f));
        }
    }

    static void RotateAboutUp(rage::Vector3& v, float c, float s)
    {
        const float x = v.x * c - v.y * s, y = v.x * s + v.y * c;
        v.x = x;
        v.y = y;
    }

    static void NearConeBefore(SafetyHookContext&)
    {
        nearConeCount = *reinterpret_cast<const uint32_t*>(imageBase + 0x114CBBC);
    }

    static void NearConeAfter(SafetyHookContext& regs)
    {
        const auto count = *reinterpret_cast<const uint32_t*>(imageBase + 0x114CBBC);
        const auto lights = *reinterpret_cast<rage::CLightSource* const*>(imageBase + 0xC3EEDC);
        // A full list replaces some other light instead; that one is left alone.
        if (!lights || count != nearConeCount + 1 || count > 0x280) return;
        auto& light = lights[count - 1];
        if (light.mType != rage::LT_SPOT || !(light.mFlags & rage::LF_VEHICLE)) return;
        ++nearConeBeams;

        const auto now = std::chrono::steady_clock::now();
        auto& car = NearConeState(static_cast<uintptr_t>(regs.esi), now);
        const float dt = std::clamp(std::chrono::duration<float>(now - car.seen).count(), 0.0f, 0.25f);
        car.seen = now;
        float left, right;
        NearConeSides(light, left, right);
        const float ease = 1.0f - std::exp(-nearCone.speed * dt);
        car.left += (left - car.left) * ease;
        car.right += (right - car.right) * ease;
        if (car.left < 1e-3f && car.right < 1e-3f) return;

        // Spot cones are cosines of the half-angles; anything above 1 would be degrees.
        const bool degrees = light.mOuterConeAngle > 1.0f;
        nearConeLastOuter = light.mOuterConeAngle;
        nearConeLastInner = light.mInnerConeAngle;
        if (degrees) ++nearConeConesInDegrees;
        constexpr float toRad = 3.14159265f / 180.0f;
        const float outer = degrees ? light.mOuterConeAngle * toRad : std::acos(std::clamp(light.mOuterConeAngle, -1.0f, 1.0f));
        const float inner = degrees ? light.mInnerConeAngle * toRad : std::acos(std::clamp(light.mInnerConeAngle, -1.0f, 1.0f));
        if (!(outer > 1e-3f)) return;
        // Neither edge past the axis: at least 5 degrees of the cone stay.
        const float room = (std::max)(2.0f * (outer - 5.0f * toRad), 0.0f);
        float cutLeft = car.left * nearCone.cut * toRad, cutRight = car.right * nearCone.cut * toRad;
        if (cutLeft + cutRight > room)
        {
            const float k = room / (cutLeft + cutRight);
            cutLeft *= k;
            cutRight *= k;
        }
        const float narrowed = outer - 0.5f * (cutLeft + cutRight);
        const float newInner = inner * narrowed / outer;
        light.mOuterConeAngle = degrees ? narrowed / toRad : std::cos(narrowed);
        light.mInnerConeAngle = degrees ? newInner / toRad : std::cos(newInner);
        // Positive turns left about up: a cut left edge turns the axis right by half of it, so the
        // right edge stays.
        const float yaw = 0.5f * (cutRight - cutLeft);
        const float c = std::cos(yaw), s = std::sin(yaw);
        RotateAboutUp(light.mDirection, c, s);
        RotateAboutUp(light.mTangent, c, s);
        ++nearConeAdjusted;
    }

    static void InstallNearCone(bool enabled, const NearConeSettings& settings)
    {
        if (!enabled) return;
        nearCone = settings;
        // call 0xA3E070 / add esp, 30h / pop edi / pop esi
        const auto check = CodeCheck()
            .Bytes(0x63FE11, {0xE8}).Branch(0x63FE12, 0x63E070)
            .Bytes(0x63FE16, {0x83,0xC4,0x30,0x5F,0x5E})
            .Bytes(0x6BD2C1, {0x8B,0x35}).Address(0x6BD2C3, 0x114CBBC)
            .Bytes(0x6BD2DA, {0x03,0x0D}).Address(0x6BD2DC, 0xC3EEDC);
        if (!check)
        {
            nearConeStatus = check.Status();
            return;
        }
        auto before = safetyhook::MidHook::create(imageBase + 0x63FE11, NearConeBefore);
        auto after = safetyhook::MidHook::create(imageBase + 0x63FE16, NearConeAfter);
        if (!before || !after)
        {
            nearConeStatus = "hook failed";
            return;
        }
        nearConeBeforeHook = std::move(*before);
        nearConeAfterHook = std::move(*after);
        nearConeStatus = "installed";
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
        if (BeamTrace::enabled && PlayerCar::IsLast(vehicle))
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
