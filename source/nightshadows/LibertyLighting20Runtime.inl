// Light families ported from Liberty's Shadows 2.0 (OnyxOak, https://www.nexusmods.com/gta4/mods/1459), GPL-3.0,
// rewritten for this plugin: the same hook sites in CE 1.2.0.59 and the same rules, without its standalone checks
// of official FusionFix builds.
//
// - Dual headlights: a car's two working headlights, one beam from the point between them, become two beams of half
//   the intensity from the lamps themselves, each with its own shadow.
// - Tail light shadows: the rear lamps' light (the game's own colour, switching and damage) casts shadows.
// - World light shadows: lamps the game keeps cached take dynamic slots too, so cars and peds shadow from them;
//   model lights the map authored without shadows get dynamic ones; lamps that are only a corona get a point light
//   with a shadow.
// - Building shadows: lights with no cached map (headlights, sirens, the lights above) draw the map's buildings into
//   their dynamic maps, so their light no longer goes through walls. Needs the wd_local techniques the world shaders
//   get from shaders/patches/world_local_shadow_depth.patch.
//
// Each family has its graphics menu toggle (Graphics > Lighting) and works while the night shadow allocation adapter
// does (SlotAllocation=2 and the top Extra Night Shadows choice).
namespace Lighting20
{
    static fusionfix::DiagnosticsLog log;
    static std::string installStatus = "not installed";
    static uintptr_t game = 0;

    static bool Pref(std::optional<std::reference_wrapper<int32_t>>& ref) noexcept
    {
        return ref && ref->get() != 0;
    }

    static uint32_t Frame() noexcept { return CTimer::m_frameCount ? *CTimer::m_frameCount : 0; }

    // A call instruction at site whose target is target, both image offsets.
    static bool CallsTo(uint32_t site, uint32_t target) noexcept
    {
        const auto at = reinterpret_cast<const uint8_t*>(game + site);
        return at[0] == 0xE8 && site + 5 + *reinterpret_cast<const int32_t*>(at + 1) == target;
    }

    static bool BytesAt(uint32_t site, std::initializer_list<uint8_t> bytes) noexcept
    {
        auto at = reinterpret_cast<const uint8_t*>(game + site);
        for (auto b : bytes) if (*at++ != b) return false;
        return true;
    }

    // Keys for lamps the game gives no key of their own: addresses in this block, so never a game object's pointer
    // nor one of its owner + ordinal keys. A key stands for a vehicle pool slot and a lamp of the car in it; a new car
    // in the slot takes the same keys, which only matters for the frame it is seen first.
    namespace LampKeys
    {
        enum Channel : uint32_t { LeftHeadlight, RightHeadlight, TailLight0, TailLight1, Channels };
        static constexpr uint32_t MaxVehicles = 4096;
        alignas(16) static uint32_t block[MaxVehicles * Channels];
        static std::atomic<uintptr_t> owners[MaxVehicles * Channels];
        static std::atomic<uint32_t> frames[MaxVehicles * Channels];

        static uint32_t Base() noexcept { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block)); }

        static bool Contains(uint32_t key) noexcept
        {
            return key >= Base() && key - Base() < sizeof(block) && !((key - Base()) & 3);
        }

        static uint32_t ChannelOf(uint32_t key) noexcept { return ((key - Base()) / 4) % Channels; }

        // The key of a car's lamp this frame, 0 where the car is not in the vehicle pool.
        static uint32_t Key(uintptr_t vehicle, uint32_t channel) noexcept
        {
            const auto pool = CVehicle::GetCheckedVehiclePool();
            if (!pool || channel >= Channels) return 0;
            const auto storage = reinterpret_cast<uintptr_t>(pool->m_aStorage);
            if (vehicle < storage) return 0;
            const auto offset = vehicle - storage, stride = static_cast<uintptr_t>(pool->m_nStorageSize);
            const auto index = offset / stride;
            if (offset % stride || index >= static_cast<uintptr_t>(pool->m_nSize) || index >= MaxVehicles ||
                (pool->m_aFlags[index] & 0x80))
                return 0;
            const auto cell = index * Channels + channel;
            owners[cell].store(vehicle, std::memory_order_relaxed);
            frames[cell].store(Frame(), std::memory_order_release);
            return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&block[cell]));
        }

        // The car whose lamp has this key, where it was submitted within the last two frames; 0 otherwise.
        static uintptr_t Owner(uint32_t key) noexcept
        {
            if (!Contains(key)) return 0;
            const auto cell = (key - Base()) / 4;
            if (Frame() - frames[cell].load(std::memory_order_acquire) > 2) return 0;
            return owners[cell].load(std::memory_order_relaxed);
        }

        static uintptr_t HeadlightOwner(uintptr_t key) noexcept
        {
            return key <= UINT32_MAX && Contains(static_cast<uint32_t>(key)) &&
                ChannelOf(static_cast<uint32_t>(key)) <= RightHeadlight ? Owner(static_cast<uint32_t>(key)) : 0;
        }
    }

    // ---------------------------------------------------------------- dual headlights
    namespace Dual
    {
        static bool installed = false;
        static std::optional<std::reference_wrapper<int32_t>> pref;
        static SafetyHookMid pre, post;
        static std::atomic<uint32_t> pairs{0}, fallbacks{0};

        // The combined beam call 0xA3FE11 -> 0xA3E070 in progress: the car and its two lamps in the world.
        struct Invocation { bool valid = false; uintptr_t car = 0; uint32_t frame = 0; float lamps[2][3]{}; };
        static thread_local Invocation invocation;

        static bool On() noexcept { return installed && Pref(pref) && bHeadlightShadows && bVehicleNightShadows; }

        // At the call: [esp] the car's matrix, and the lamps' bone positions in the car's space at esp+0xA0 (the
        // first lamp, bone edi) and esp+0x90 (the second), from 0xC65EA0 at 0xA3FCC1 / 0xA3FCD2.
        static void Pre(SafetyHookContext& regs) noexcept
        {
            invocation = {};
            if (!On()) return;
            const auto matrix = *reinterpret_cast<const float* const*>(regs.esp);
            const auto car = static_cast<uintptr_t>(regs.esi);
            if (!matrix || !car || *reinterpret_cast<const uintptr_t*>(regs.esp + 0x28) != car) { ++fallbacks; return; }
            const float* local[2] = { reinterpret_cast<const float*>(regs.esp + 0xA0), reinterpret_cast<const float*>(regs.esp + 0x90) };
            for (int side = 0; side < 2; ++side)
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float* p = local[side];
                    invocation.lamps[side][axis] = p[0] * matrix[axis] + p[1] * matrix[4 + axis] + p[2] * matrix[8 + axis] + matrix[12 + axis];
                    if (!std::isfinite(invocation.lamps[side][axis])) { ++fallbacks; return; }
                }
            float apart = 0;
            for (int axis = 0; axis < 3; ++axis)
                apart += (invocation.lamps[0][axis] - invocation.lamps[1][axis]) * (invocation.lamps[0][axis] - invocation.lamps[1][axis]);
            // Lamps at one point or metres apart are not a pair of headlights.
            if (!(apart > 0.0001f && apart < 400.0f)) { ++fallbacks; return; }
            invocation.valid = true;
            invocation.car = car;
            invocation.frame = Frame();
        }

        static void Post(SafetyHookContext&) noexcept { invocation = {}; }

        // In the submission wrappers: the combined beam of the car in the call above becomes its two lamps' beams,
        // each at half the intensity, with the cone, colour, range, room and flags of the combined one. False leaves
        // the combined beam to the caller.
        static bool Split(int a1, int a2, uint32_t flags, int direction, int tangent, int position, int a7, int a8,
            int a9, int a10, int a11, int a12, int a13, int a14, int a15, int stableKey, bool shadow) noexcept
        {
            if (!invocation.valid || invocation.frame != Frame() || static_cast<uintptr_t>(static_cast<uint32_t>(stableKey)) != invocation.car ||
                a2 != 2 || (flags != 0x504 && flags != 0x104))
                return false;
            uint32_t keys[2];
            for (uint32_t side = 0; side < 2; ++side)
                if (!(keys[side] = LampKeys::Key(invocation.car, LampKeys::LeftHeadlight + side))) { ++fallbacks; return false; }
            float intensity;
            std::memcpy(&intensity, &a8, 4);
            if (!std::isfinite(intensity) || intensity < 0) { ++fallbacks; return false; }
            const float half = intensity * 0.5f;
            int halfBits;
            std::memcpy(&halfBits, &half, 4);
            if (!shadow) flags &= ~rage::LF_DYNAMIC_SHADOW;
            for (int side = 0; side < 2; ++side)
            {
                CShadows::hbStoreStaticShadow.fun(a1, a2, flags, direction, tangent,
                    static_cast<int>(reinterpret_cast<uintptr_t>(invocation.lamps[side])), a7, halfBits,
                    a9, a10, a11, a12, a13, a14, a15, static_cast<int>(keys[side]));
                const std::lock_guard<std::mutex> lock(CShadows::gStableHeadlightShadow.stateMutex);
                CShadows::gStableHeadlightShadow.submitted.Record(keys[side], invocation.frame);
            }
            ++pairs;
            return true;
        }

        static void Install() noexcept
        {
            pref = FusionFixSettings.GetRef("PREF_DUAL_HEADLIGHTS");
            if (!CallsTo(0x63FE11, 0x63E070) || !BytesAt(0x63FE16, { 0x83, 0xC4, 0x30 }))
            {
                installStatus += " dual=code_differs";
                return;
            }
            pre = safetyhook::create_mid(game + 0x63FE11, Pre);
            post = safetyhook::create_mid(game + 0x63FE16, Post);
            installed = pre && post;
            fusionfix::shadows::ce::AdditionalVehicleBeamOwner = LampKeys::HeadlightOwner;
            installStatus += installed ? " dual=1" : " dual=hook_failed";
        }
    }

    // ---------------------------------------------------------------- tail lights
    namespace Tail
    {
        static bool installed = false;
        static std::optional<std::reference_wrapper<int32_t>> pref;
        static SafetyHookMid combined, single;
        static std::atomic<uint32_t> promoted{0}, refused{0};
        static constexpr uint32_t NativeColorRva = 0xEE2340; // the rear lamps' colour the game passes

        static bool On() noexcept
        {
            return installed && Pref(pref) && bHeadlightShadows && bVehicleNightShadows && PlayerShadowAllocation::Enabled();
        }

        // At the calls 0xA43376 / 0xA4342D -> 0xA3DE90 (the beam of one or both rear lamps), ESI the car. The words:
        // [0] matrix, [1] position, [2] direction (0, -1, 0), [3] colour, [4..7] intensity and range, [10] key (the car,
        // or the car + 1 for the second lamp alone), [11] cast a shadow (0 from the game).
        static void Submit(SafetyHookContext& regs) noexcept
        {
            if (!On()) return;
            auto words = reinterpret_cast<uint32_t*>(regs.esp);
            const auto car = static_cast<uint32_t>(regs.esi);
            const auto direction = reinterpret_cast<const float*>(static_cast<uintptr_t>(words[2]));
            if (!car || !words[0] || !words[1] || !direction || words[3] != game + NativeColorRva ||
                (words[10] != car && words[10] != car + 1) || words[11] || words[12] ||
                direction[0] != 0.0f || direction[1] != -1.0f || direction[2] != 0.0f)
            {
                ++refused;
                return;
            }
            float intensity, range;
            std::memcpy(&intensity, &words[4], 4);
            std::memcpy(&range, &words[5], 4);
            if (!std::isfinite(intensity) || !std::isfinite(range) || intensity <= 0 || range <= 0) { ++refused; return; }
            const auto key = LampKeys::Key(car, words[10] == car ? LampKeys::TailLight0 : LampKeys::TailLight1);
            if (!key) { ++refused; return; }
            words[10] = key;
            words[11] = 1;
            ++promoted;
        }

        // Rear lamp beams take no part in the headlight selection; the player's own lose the player flag 0x400,
        // which would rank them above every other light.
        static bool IsKey(uint32_t key) noexcept
        {
            return LampKeys::Contains(key) && LampKeys::ChannelOf(key) >= LampKeys::TailLight0;
        }

        static void Install() noexcept
        {
            pref = FusionFixSettings.GetRef("PREF_TAIL_LIGHT_SHADOWS");
            if (!CallsTo(0x643376, 0x63DE90) || !CallsTo(0x64342D, 0x63DE90))
            {
                installStatus += " tail=code_differs";
                return;
            }
            combined = safetyhook::create_mid(game + 0x643376, Submit);
            single = safetyhook::create_mid(game + 0x64342D, Submit);
            installed = combined && single;
            installStatus += installed ? " tail=1" : " tail=hook_failed";
        }
    }

    // The keys of the lights this plugin made or gave a shadow this frame (corona lights, model lights), so the
    // selection can tell them from the game's own: a fixed table, written by the 2dfx light code and read before the
    // slots are picked a frame or two later.
    namespace Ours
    {
        enum Kind : uint8_t { None, CoronaLight, ModelLight };
        struct Mark { std::atomic<uint32_t> key{0}, frame{0}; std::atomic<uint8_t> kind{None}; };
        static constexpr uint32_t Cells = 1024;
        static Mark marks[Cells];

        static uint32_t Cell(uint32_t key) noexcept { return (key * 0x9E3779B1u) >> 22; }

        static void Add(uint32_t key, Kind kind) noexcept
        {
            const auto now = Frame();
            for (uint32_t i = 0, c = Cell(key); i < 8; ++i, c = (c + 1) & (Cells - 1))
            {
                auto& m = marks[c];
                const auto k = m.key.load(std::memory_order_relaxed);
                if (k == key || !k || now - m.frame.load(std::memory_order_relaxed) > 8)
                {
                    m.key.store(key, std::memory_order_relaxed);
                    m.kind.store(kind, std::memory_order_relaxed);
                    m.frame.store(now, std::memory_order_release);
                    return;
                }
            }
        }

        static Kind Of(uint32_t key) noexcept
        {
            if (!key) return None;
            const auto now = Frame();
            for (uint32_t i = 0, c = Cell(key); i < 8; ++i, c = (c + 1) & (Cells - 1))
            {
                const auto& m = marks[c];
                if (m.key.load(std::memory_order_relaxed) == key)
                    return now - m.frame.load(std::memory_order_acquire) <= 4 ? Kind(m.kind.load(std::memory_order_relaxed)) : None;
            }
            return None;
        }
    }

    // ---------------------------------------------------------------- world lights
    namespace World
    {
        static bool installed = false;
        static std::optional<std::reference_wrapper<int32_t>> pref;
        static bool cachedLamps = true, modelLights = true, coronas = true;
        static SafetyHookMid coronaHook;
        static std::atomic<uint32_t> lampsPromoted{0}, modelLightsPromoted{0}, coronaLights{0}, coronasRefused{0}, coronasCapped{0};
        static std::atomic<uint32_t> coronasMerged{0}, modelLightsMerged{0}, modelLightsIdle{0};
        static bool modelLightsNeedCasters = true; // ModelLightShadowsNeedCasters
        // WorldLightsWithoutSlots: the model lights keep the game's no shadow and the coronas' lights take none, so
        // they never race the lamps for the seven slots; the cars, people and props under them still shadow through the
        // light shaders' shapes (VehicleBoxShadows), which every light without a map of its own takes
        static bool withoutSlots = true;
        static float mergeDistance = 1.5f; // ExtraLightMergeDistance

        struct CoronaSettings
        {
            bool approximate = true;
            float radiusPerSize = 12.0f, fallbackIntensity = 1.0f, intensityScale = 1.0f;
            float minRadius = 0.1f, maxRadius = 128.0f, maxIntensity = 64.0f;
            float distance = 100.0f;
            uint32_t perFrame = 48;
        };
        static CoronaSettings corona;
        static uint32_t coronaFrame = 0, coronaCount = 0;

        static bool On() noexcept { return installed && Pref(pref) && bExtraNightShadows && !LampShadowsOffBySun(); }

        // Lamps the game shadows from their cache alone (flags 2, cache entry with their key) race for the dynamic
        // slots as well: the cache holds the static world only, so a car or ped under them cast no shadow.
        static void PromoteCachedLamps(rage::CLightSource* lights, uint32_t count) noexcept
        {
            if (!On() || !cachedLamps || !lights || count > 4096) return;
            for (uint32_t i = 0; i < count; ++i)
            {
                auto& light = lights[i];
                if ((light.mType != rage::LT_POINT && light.mType != rage::LT_SPOT) || (light.mFlags & 6) != rage::LF_STATIC_SHADOW ||
                    (light.mFlags & rage::LF_VEHICLE) || !light.mCastShadows || light.mCastShadows == -1 ||
                    light.mShadowCacheIndex < 0 || light.mShadowCacheIndex >= 8 || !std::isfinite(light.mRadius) || light.mRadius <= 0)
                    continue;
                const auto cached = *reinterpret_cast<const uint32_t*>(game + fusionfix::shadows::ce::allocation::CacheAge0Rva +
                    static_cast<uint32_t>(light.mShadowCacheIndex) * 0x100 + 4);
                if (cached != static_cast<uint32_t>(light.mCastShadows)) continue;
                light.mFlags |= rage::LF_DYNAMIC_SHADOW;
                ++lampsPromoted;
            }
        }

        // From the 2dfx light flags hook (CE 0xC1E615): a point or spot light the map authored without shadows
        // (flags & 6 == 0) gets a dynamic one. Its key stays the game's (the entity + the effect's ordinal), which
        // the game clears right after for lights without shadow flags.
        static bool PromoteModelLight(uint32_t flags, uintptr_t descriptor, uint32_t key) noexcept
        {
            if (!On() || !modelLights || withoutSlots || (flags & 6) || !descriptor) return false;
            const auto type = *reinterpret_cast<const uint8_t*>(descriptor + 0x5B);
            const float radius = *reinterpret_cast<const float*>(descriptor + 0x38);
            if ((type != 1 && type != 2) || !std::isfinite(radius) || radius <= 0 || !key || key == UINT32_MAX) return false;
            Ours::Add(key, Ours::ModelLight);
            ++modelLightsPromoted;
            return true;
        }

        // At 0xC1EB27, the call registering a 2dfx effect's corona (0xC1F020): a corona-only effect (light type 0)
        // gets a point light with a dynamic shadow at the corona, in its colour (with its fade), its authored range
        // and intensity, or ones from its size where the map left them 0. The corona itself is untouched.
        // Words: [0] key, [1..3] RGB, [4] size, [5] position, [13] 0, [14] 1. EDI the effect's light data, EBP the frame
        // of 0xC1E200: +8 effect, +0x10 brightness, +0x14 fade, +0x20 key, +0x24 interior, +0x28 room, +0x2C texture.
        static void Corona(SafetyHookContext& regs) noexcept
        {
            if (!On() || !coronas) return;
            const auto descriptor = static_cast<uintptr_t>(regs.edi);
            const auto frame = reinterpret_cast<const uint32_t*>(regs.ebp);
            const auto words = reinterpret_cast<const uint32_t*>(regs.esp);
            if (!descriptor || *reinterpret_cast<const uint8_t*>(descriptor + 0x5B) != 0) return;
            const uint32_t key = frame[8];
            const auto position = reinterpret_cast<const float*>(static_cast<uintptr_t>(words[5]));
            float brightness, fade, size, authoredRadius, authoredIntensity;
            std::memcpy(&brightness, &frame[4], 4);
            std::memcpy(&fade, &frame[5], 4);
            std::memcpy(&size, reinterpret_cast<const void*>(descriptor + 0x5C), 4);
            std::memcpy(&authoredRadius, reinterpret_cast<const void*>(descriptor + 0x38), 4);
            std::memcpy(&authoredIntensity, reinterpret_cast<const void*>(descriptor + 0x3C), 4);
            if (!key || key == UINT32_MAX || words[0] != key || words[13] || words[14] != 1 || !position ||
                words[1] > 255 || words[2] > 255 || words[3] > 255 || !(words[1] | words[2] | words[3]) ||
                !std::isfinite(brightness) || brightness <= 0 || !std::isfinite(size) || size <= 0 ||
                !std::isfinite(authoredRadius) || authoredRadius < 0 || !std::isfinite(authoredIntensity) || authoredIntensity < 0 ||
                // The game's own gate for the light of type 1 and 2 effects (0xC1E2B3)
                !*reinterpret_cast<const uint8_t*>(game + 0xC48244) || !(brightness >= 0.01f || (std::isfinite(fade) && fade >= 0.01f)) ||
                !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
            {
                ++coronasRefused;
                return;
            }
            const bool radiusMissing = authoredRadius == 0, intensityMissing = authoredIntensity == 0;
            if (!corona.approximate && (radiusMissing || intensityMissing)) { ++coronasRefused; return; }
            float radius = radiusMissing ? std::clamp(size * corona.radiusPerSize, corona.minRadius, corona.maxRadius) : authoredRadius;
            float intensity = (intensityMissing ? corona.fallbackIntensity : authoredIntensity) * brightness * corona.intensityScale;
            if (intensityMissing) intensity = (std::min)(intensity, corona.maxIntensity);
            if (!std::isfinite(radius) || !std::isfinite(intensity) || radius <= 0 || intensity <= 0) { ++coronasRefused; return; }
            // Far coronas would fill the frame's light list (640) with lights nobody sees.
            float player[3];
            if (!CPlayer::getLocalPlayerPed || !CEntity::GetPosition(CPlayer::getLocalPlayerPed(), player)) return;
            const float dx = position[0] - player[0], dy = position[1] - player[1], dz = position[2] - player[2];
            const float reach = corona.distance + radius;
            if (dx * dx + dy * dy + dz * dz > reach * reach) return;
            const auto now = Frame();
            if (now != coronaFrame) { coronaFrame = now; coronaCount = 0; }
            if (coronaCount >= corona.perFrame) { ++coronasCapped; return; }
            ++coronaCount;
            alignas(16) const float direction[4] = { 0, 0, -1, 0 }, up[4] = { 0, 1, 0, 0 };
            alignas(16) const float at[4] = { position[0], position[1], position[2], 1 };
            alignas(16) const float colour[4] = { words[1] / 255.0f, words[2] / 255.0f, words[3] / 255.0f, 1 };
            auto bits = [](float f) { int i; std::memcpy(&i, &f, 4); return i; };
            // The game's wrapper copies the vectors at once; a point light (type 0), dynamic shadow only (4), or none
            // with WorldLightsWithoutSlots.
            CShadows::hbStoreStaticShadow.fun(static_cast<int>(frame[2]), 0, withoutSlots ? 0 : rage::LF_DYNAMIC_SHADOW,
                static_cast<int>(reinterpret_cast<uintptr_t>(direction)), static_cast<int>(reinterpret_cast<uintptr_t>(up)),
                static_cast<int>(reinterpret_cast<uintptr_t>(at)), static_cast<int>(reinterpret_cast<uintptr_t>(colour)),
                bits(intensity), 0, static_cast<int>(frame[11]), bits(radius), 0, 0,
                static_cast<int>(frame[9]), static_cast<int>(frame[10]), static_cast<int>(key));
            if (!withoutSlots)
                Ours::Add(key, Ours::CoronaLight);
            ++coronaLights;
        }

        static void Install(CIniReader& ini) noexcept
        {
            pref = FusionFixSettings.GetRef("PREF_WORLD_LIGHT_SHADOWS");
            cachedLamps = ini.ReadInteger("NIGHTSHADOWS", "CachedLampShadows", 1) != 0;
            modelLights = ini.ReadInteger("NIGHTSHADOWS", "ModelLightShadows", 1) != 0;
            coronas = ini.ReadInteger("NIGHTSHADOWS", "CoronaLightShadows", 1) != 0;
            corona.approximate = ini.ReadInteger("NIGHTSHADOWS", "CoronaLightApproximation", 1) != 0;
            corona.radiusPerSize = std::clamp(ini.ReadFloat("NIGHTSHADOWS", "CoronaLightRadiusPerSize", 12.0f), 0.1f, 1024.0f);
            corona.fallbackIntensity = std::clamp(ini.ReadFloat("NIGHTSHADOWS", "CoronaLightFallbackIntensity", 1.0f), 0.01f, 64.0f);
            corona.intensityScale = std::clamp(ini.ReadFloat("NIGHTSHADOWS", "CoronaLightIntensityScale", 1.0f), 0.01f, 64.0f);
            corona.minRadius = std::clamp(ini.ReadFloat("NIGHTSHADOWS", "CoronaLightMinRadius", 0.1f), 0.01f, 4096.0f);
            corona.maxRadius = std::clamp(ini.ReadFloat("NIGHTSHADOWS", "CoronaLightMaxRadius", 128.0f), corona.minRadius, 4096.0f);
            corona.maxIntensity = std::clamp(ini.ReadFloat("NIGHTSHADOWS", "CoronaLightMaxIntensity", 64.0f), 0.01f, 1024.0f);
            corona.distance = std::clamp(ini.ReadFloat("NIGHTSHADOWS", "CoronaLightDistance", 100.0f), 0.0f, 1000.0f);
            corona.perFrame = static_cast<uint32_t>(std::clamp(ini.ReadInteger("NIGHTSHADOWS", "CoronaLightMaxCount", 48), 0, 256));
            mergeDistance = std::clamp(ini.ReadFloat("NIGHTSHADOWS", "ExtraLightMergeDistance", 1.5f), 0.0f, 10.0f);
            modelLightsNeedCasters = ini.ReadInteger("NIGHTSHADOWS", "ModelLightShadowsNeedCasters", 1) != 0;
            withoutSlots = ini.ReadInteger("NIGHTSHADOWS", "WorldLightsWithoutSlots", 1) != 0;
            installed = true;
            if (coronas)
            {
                if (CallsTo(0x81EB27, 0x81F020) && BytesAt(0x81EB24, { 0xFF, 0x75, 0x20 }))
                    coronaHook = safetyhook::create_mid(game + 0x81EB27, Corona);
                installStatus += coronaHook ? " coronas=1" : " coronas=code_differs";
            }
            installStatus += std::string(" world=1 cached_lamps=") + (cachedLamps ? "1" : "0") + " model_lights=" + (modelLights ? "1" : "0") +
                " without_slots=" + (withoutSlots ? "1" : "0");
        }
    }

    // ---------------------------------------------------------------- buildings in dynamic maps
    namespace Buildings
    {
        static bool installed = false;
        static std::optional<std::reference_wrapper<int32_t>> pref;
        static SafetyHookMid collect, mask, draw;
        static thread_local bool scope = false;
        static std::atomic<uint32_t> passes{0}, masks{0}, draws{0};

        static bool On() noexcept { return installed && Pref(pref) && bExtraNightShadows; }

        // A render phase (CE 0xD77990 object) drawing slot 1-7 of a light with no cached map: render mode 4 (dynamic
        // only) and cache index -1 in the slot record 0x119F100 + slot * 0x110. Mode 4 with a cache index copies the
        // cache's static world in, so drawing it again would only cost.
        static bool UncachedDynamicPass(uintptr_t phase) noexcept
        {
            if (!phase) return false;
            const auto slot = *reinterpret_cast<const uint32_t*>(phase + 0x940);
            if (slot < 1 || slot > 7) return false;
            const auto record = game + 0xD9F100 + slot * 0x110;
            const auto key = *reinterpret_cast<const uint32_t*>(record + 0xF8);
            return *reinterpret_cast<const uint8_t*>(record + 0xED) == 1 && *reinterpret_cast<const uint32_t*>(record + 0xFC) == 4 &&
                *reinterpret_cast<const int32_t*>(record + 0xF0) < 0 && key && key != UINT32_MAX &&
                !*reinterpret_cast<const uint8_t*>(game + fusionfix::shadows::ce::casterguard::DeferredFlagRva);
        }

        // BuildRenderList 0xD782E0 just set phase +0x15/+0x16 (static, dynamic) to 0/1 for mode 4: collect the static
        // entities too, as mode 5 does.
        static void Collect(SafetyHookContext& regs) noexcept
        {
            if (!On() || !UncachedDynamicPass(regs.esi)) return;
            auto flags = reinterpret_cast<uint16_t*>(regs.esi + 0x15);
            if (*flags != 0x100) return;
            *flags = 0x101;
            ++passes;
        }

        // 0xD784AE stores the phase's entity mask (+0x8D0): mode 4 leaves out 0x80, which regular buildings need
        // (0xAE7914 drops a building from a view whose mask lacks it); mode 5 has it.
        static void Mask(SafetyHookContext& regs) noexcept
        {
            if (!On() || regs.eax != 0x1800007 || !UncachedDynamicPass(regs.edi)) return;
            regs.eax |= 0x80;
            ++masks;
        }

        // 0xAE4265: a phase drawing with the light's own technique (EBX -1) skips the static geometry bucket 7;
        // in the passes above it is drawn, as for cached maps.
        static void Draw(SafetyHookContext& regs) noexcept
        {
            if (!scope || regs.ebx != UINT32_MAX) return;
            regs.eip = game + 0x6E426A;
            ++draws;
        }

        // Around the phase's render (0xD77A00, the lamppost shadows workaround 2 hook in nightshadows).
        class Scope
        {
            bool previous;
        public:
            explicit Scope(void* phase) noexcept : previous(scope)
            {
                scope = On() && UncachedDynamicPass(reinterpret_cast<uintptr_t>(phase));
            }
            ~Scope() noexcept { scope = previous; }
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;
        };

        static void Install() noexcept
        {
            pref = FusionFixSettings.GetRef("PREF_BUILDING_SHADOWS");
            if (!BytesAt(0x97833E, { 0x0F, 0xB6, 0x05 }) || !BytesAt(0x978338, { 0x66, 0xC7, 0x46, 0x15, 0x00, 0x01 }) ||
                !BytesAt(0x9784AE, { 0x89, 0x87, 0xD0, 0x08, 0x00, 0x00 }) || !BytesAt(0x6E4265, { 0x83, 0xFB, 0xFF, 0x74, 0x10 }) ||
                !CallsTo(0x6E4272, 0x6E3310))
            {
                installStatus += " buildings=code_differs";
                return;
            }
            collect = safetyhook::create_mid(game + 0x97833E, Collect);
            mask = safetyhook::create_mid(game + 0x9784AE, Mask);
            draw = safetyhook::create_mid(game + 0x6E4265, Draw);
            installed = collect && mask && draw;
            installStatus += installed ? " buildings=1" : " buildings=hook_failed";
        }
    }

    // A light of ours next to one the game already has would light and shadow the same spot twice: a lamp whose
    // glow is an effect of its own beside its light got a second light in the glow's colour, and a model's second
    // light half a metre from its shadowed first one a second shadow; under the lamp a car had two shadows of two
    // colours (one a box, VehicleBoxShadows, the other the map's). Within ExtraLightMergeDistance of a light of the
    // game's (or of a corona light of ours before it in the list), a corona light goes dark and loses its shadow, and a
    // model light near a shadowed one keeps the game's no shadow.
    static void MergeWithNative(rage::CLightSource* lights, uint32_t count) noexcept
    {
        const float merge = World::mergeDistance;
        if (merge <= 0) return;
        std::array<uint16_t, 256> ours;
        std::array<uint8_t, 256> kinds;
        unsigned n = 0;
        for (uint32_t i = 0; i < count && n < ours.size(); ++i)
        {
            const auto& l = lights[i];
            if (!(l.mFlags & rage::LF_DYNAMIC_SHADOW) || (l.mFlags & rage::LF_VEHICLE)) continue;
            if (const auto kind = Ours::Of(static_cast<uint32_t>(l.mCastShadows)))
            {
                ours[n] = static_cast<uint16_t>(i);
                kinds[n++] = kind;
            }
        }
        if (!n) return;
        std::array<bool, 4096> isOurs{};
        for (unsigned k = 0; k < n; ++k) isOurs[ours[k]] = true;
        for (unsigned k = 0; k < n; ++k)
        {
            auto& o = lights[ours[k]];
            const bool corona = kinds[k] == Ours::CoronaLight;
            for (uint32_t j = 0; j < count; ++j)
            {
                if (j == ours[k]) continue;
                const auto& other = lights[j];
                if ((other.mFlags & rage::LF_VEHICLE) || (other.mType != rage::LT_POINT && other.mType != rage::LT_SPOT) ||
                    !(other.mIntensity > 0) || !(other.mRadius > 0))
                    continue;
                if (isOurs[j] && !(corona && j < ours[k] && (other.mFlags & 6))) continue;
                if (!corona && !(other.mFlags & 6)) continue;
                const float dx = o.mPosition.x - other.mPosition.x, dy = o.mPosition.y - other.mPosition.y, dz = o.mPosition.z - other.mPosition.z;
                if (dx * dx + dy * dy + dz * dz > merge * merge) continue;
                if (corona)
                {
                    o.mIntensity = 0;
                    o.mFlags &= ~6u;
                    ++World::coronasMerged;
                }
                else
                {
                    o.mFlags &= ~rage::LF_DYNAMIC_SHADOW;
                    ++World::modelLightsMerged;
                }
                break;
            }
        }
    }

    // ModelLightShadowsNeedCasters: a log had some 127 model lights a pass given a shadow, all competing for the seven
    // slots (some 900 comparisons a frame) with their shadows mostly of walls nobody looks at. A model light keeps its
    // dynamic shadow only within the lamp shadow reach of you and with a car or ped in its light (the casters of the
    // last pass); the others light as the map has them, and cars under them get box shadows.
    static void IdleModelLights(rage::CLightSource* lights, uint32_t count) noexcept
    {
        if (!World::modelLightsNeedCasters) return;
        const auto& state = PlayerShadowAllocation::state;
        const int feet = fusionfix::shadows::ShadowReachFeet(ShadowReachStep(false));
        const float reach = feet > 0 ? feet * 0.3048f : 60.0f;
        for (uint32_t i = 0; i < count; ++i)
        {
            auto& l = lights[i];
            if (!(l.mFlags & rage::LF_DYNAMIC_SHADOW) || (l.mFlags & rage::LF_VEHICLE) ||
                Ours::Of(static_cast<uint32_t>(l.mCastShadows)) != Ours::ModelLight)
                continue;
            const float dx = l.mPosition.x - state.player.x, dy = l.mPosition.y - state.player.y, dz = l.mPosition.z - state.player.z;
            const bool inReach = dx * dx + dy * dy + dz * dz <= reach * reach;
            const bool lit = inReach && state.casters.Lights({ l.mPosition.x, l.mPosition.y, l.mPosition.z },
                { l.mDirection.x, l.mDirection.y, l.mDirection.z }, static_cast<int>(l.mType), l.mRadius, l.mOuterConeAngle) != fusionfix::shadows::SlotGain::None;
            if (!lit)
            {
                l.mFlags &= ~rage::LF_DYNAMIC_SHADOW;
                ++World::modelLightsIdle;
            }
        }
    }

    // ---------------------------------------------------------------- lamp pairs
    // PairedLampDistance: many posts carry two lamps a metre or three apart, each its own light, and each took a slot to
    // draw nearly the same map; under them a car had two shadows. The second of a pair takes no part in the selection;
    // when the first has a slot, its map is drawn from between them (the slot moved there, its radius grown by half
    // the spacing, HeadlightEnhancement::slotOrigin, drawn without its cache, which was from the first lamp) and the
    // second finds that map by the first's key (the lookup hook sub_925DB0, and HasShadowMap for the box shadows).
    // Without a slot each lamp keeps its cached map.
    namespace Pairs
    {
        static float distance = 2.5f;
        static std::atomic<uint32_t> found{0}, mostInFrame{0}, lookedUp{0}, mostCandidates{0};
        static constexpr uint32_t Cells = 1024;
        struct Link { std::atomic<uint32_t> second{0}, first{0}, frame{0}; };
        static Link links[Cells];     // by the second lamp's key: the first's
        struct Origin { uint32_t first = 0, frame = 0; float between[3]{}; float grow = 0; };
        static Origin origins[Cells]; // by the first lamp's key: where the map is drawn from; selection thread only

        static uint32_t Cell(uint32_t key) noexcept { return (key * 0x9E3779B1u) >> 22; }

        static void Link2(uint32_t second, uint32_t first, uint32_t frame) noexcept
        {
            for (uint32_t i = 0, c = Cell(second); i < 8; ++i, c = (c + 1) & (Cells - 1))
            {
                auto& l = links[c];
                const auto k = l.second.load(std::memory_order_relaxed);
                if (k == second || !k || frame - l.frame.load(std::memory_order_relaxed) > 8)
                {
                    l.second.store(second, std::memory_order_relaxed);
                    l.first.store(first, std::memory_order_relaxed);
                    l.frame.store(frame, std::memory_order_release);
                    return;
                }
            }
        }

        // The first lamp's key for the second's, 0 for none: the light being lit may be of a list a few frames old.
        static uint32_t FirstOf(uint32_t second) noexcept
        {
            if (distance <= 0 || !second) return 0;
            const auto now = Frame();
            for (uint32_t i = 0, c = Cell(second); i < 8; ++i, c = (c + 1) & (Cells - 1))
            {
                const auto& l = links[c];
                if (l.second.load(std::memory_order_relaxed) == second)
                    return now - l.frame.load(std::memory_order_acquire) <= 6 ? l.first.load(std::memory_order_relaxed) : 0u;
            }
            return 0;
        }

        // HeadlightEnhancement::slotOrigin: a first lamp's slot, drawn from between the two.
        static bool OriginOf(uint32_t first, const float base[4], float out[4]) noexcept
        {
            if (distance <= 0) return false;
            const auto now = Frame();
            for (uint32_t i = 0, c = Cell(first); i < 8; ++i, c = (c + 1) & (Cells - 1))
            {
                const auto& o = origins[c];
                if (o.first != first) continue;
                if (now - o.frame > 6) return false;
                std::copy_n(o.between, 3, out);
                out[3] = base[3] + o.grow;
                return true;
            }
            return false;
        }

        // VehicleBoxShadowsBridge::LampPair: either lamp of a pair, the point between them and their spacing.
        static bool PairOf(uint32_t key, float between[3], float& spacing) noexcept
        {
            if (distance <= 0 || !key) return false;
            const uint32_t first = FirstOf(key);
            const uint32_t want = first ? first : key;
            const auto now = Frame();
            for (uint32_t i = 0, c = Cell(want); i < 8; ++i, c = (c + 1) & (Cells - 1))
            {
                const auto& o = origins[c];
                if (o.first != want) continue;
                if (now - o.frame > 6) return false;
                std::copy_n(o.between, 3, between);
                spacing = o.grow * 2.0f;
                return true;
            }
            return false;
        }

        // NightShadowsSlotLog: Q lines, each light within TraceReach of you near another shadow light, paired or the
        // first rule of Find that kept it from its nearest neighbour, when that changes.
        static constexpr float TraceReach = 40.0f;
        static std::unordered_map<uint32_t, std::string> traced;

        static void Trace(const rage::CLightSource* lights, uint32_t count, const std::vector<int>& partner,
            const std::vector<bool>& candidate, const std::vector<bool>& first) noexcept
        {
            auto& trace = PlayerShadowAllocation::slotTrace;
            if (!trace.enabled || !CTimer::m_snTimeInMilliseconds) return;
            try
            {
                const auto player = PlayerShadowAllocation::state.player;
                const auto shadowed = [](const rage::CLightSource& l)
                {
                    return (l.mFlags & 6) && !(l.mFlags & (rage::LF_VEHICLE | rage::LF_TRAFFIC)) && l.mCastShadows && l.mCastShadows != -1;
                };
                const auto d2 = [](const rage::CLightSource& a, const rage::CLightSource& b)
                {
                    const float dx = a.mPosition.x - b.mPosition.x, dy = a.mPosition.y - b.mPosition.y, dz = a.mPosition.z - b.mPosition.z;
                    return dx * dx + dy * dy + dz * dz;
                };
                if (traced.size() > 4096) traced.clear();
                for (uint32_t i = 0; i < count; ++i)
                {
                    const auto& l = lights[i];
                    if (!shadowed(l)) continue;
                    const float px = l.mPosition.x - player.x, py = l.mPosition.y - player.y, pz = l.mPosition.z - player.z;
                    if (!(px * px + py * py + pz * pz <= TraceReach * TraceReach)) continue;
                    int nearest = partner[i];
                    if (nearest < 0)
                    {
                        float best = (distance * 1.5f) * (distance * 1.5f);
                        for (uint32_t j = 0; j < count; ++j)
                            if (j != i && shadowed(lights[j]) && d2(l, lights[j]) < best) { best = d2(l, lights[j]); nearest = int(j); }
                    }
                    const auto key = static_cast<uint32_t>(l.mCastShadows);
                    if (nearest < 0)
                    {
                        if (auto it = traced.find(key); it != traced.end() && it->second != "alone") { it->second = "alone"; trace.Pair(std::format("t={} f={} key={:08x} alone", *CTimer::m_snTimeInMilliseconds, Frame(), key)); }
                        continue;
                    }
                    const auto& o = lights[nearest];
                    const char* status;
                    if (partner[i] >= 0) status = first[i] ? "first" : "second";
                    else if (!(l.mFlags & rage::LF_DYNAMIC_SHADOW)) status = "not_dynamic";
                    else if (!(o.mFlags & rage::LF_DYNAMIC_SHADOW)) status = "other_not_dynamic";
                    else if (!candidate[i]) status = PlayerShadowAllocation::IsSharedMap(i) ? "shared_map" : "not_candidate";
                    else if (!candidate[nearest]) status = PlayerShadowAllocation::IsSharedMap(nearest) ? "other_shared_map" : "other_not_candidate";
                    else if (o.mType != l.mType) status = "type";
                    else if (o.mInteriorIndex != l.mInteriorIndex || o.mRoomIndex != l.mRoomIndex) status = "room";
                    else if (o.mCastShadows == l.mCastShadows) status = "same_key";
                    else if (std::abs(o.mRadius - l.mRadius) > 0.25f * (std::max)(o.mRadius, l.mRadius)) status = "radius";
                    else if (l.mType == rage::LT_SPOT && l.mDirection.x * o.mDirection.x + l.mDirection.y * o.mDirection.y + l.mDirection.z * o.mDirection.z < 0.5f) status = "direction";
                    else if (d2(l, o) >= distance * distance) status = "distance";
                    else if (partner[nearest] >= 0) status = "other_taken";
                    else status = "unknown";
                    const auto otherKey = static_cast<uint32_t>(o.mCastShadows);
                    auto now = std::format("{} {:08x}", status, otherKey);
                    auto& last = traced[key];
                    if (last == now) continue;
                    last = std::move(now);
                    const float dot = l.mDirection.x * o.mDirection.x + l.mDirection.y * o.mDirection.y + l.mDirection.z * o.mDirection.z;
                    trace.Pair(std::format("t={} f={} key={:08x} {} other={:08x} apart={:.2f} pos={:.1f},{:.1f},{:.1f} d={:.1f} "
                        "flags={:x}/{:x} type={}/{} cache={}/{} r={:.1f}/{:.1f} room={}.{}/{}.{} dot={:.2f} other_partner={:08x}",
                        *CTimer::m_snTimeInMilliseconds, Frame(), key, status, otherKey, std::sqrt(d2(l, o)),
                        l.mPosition.x, l.mPosition.y, l.mPosition.z, std::sqrt(px * px + py * py + pz * pz),
                        l.mFlags, o.mFlags, int(l.mType), int(o.mType), int(l.mShadowCacheIndex), int(o.mShadowCacheIndex),
                        l.mRadius, o.mRadius, int(l.mInteriorIndex), int(l.mRoomIndex), int(o.mInteriorIndex), int(o.mRoomIndex), dot,
                        partner[nearest] >= 0 && partner[nearest] != int(i) ? static_cast<uint32_t>(lights[partner[nearest]].mCastShadows) : 0u));
                }
            }
            catch (...) { trace.enabled = false; }
        }

        static void Find(rage::CLightSource* lights, uint32_t count) noexcept
        {
            if (distance <= 0) return;
            const auto frame = Frame();
            const bool tracing = PlayerShadowAllocation::slotTrace.enabled;
            static std::vector<int> partner;
            static std::vector<bool> candidate, firstOf;
            if (tracing)
            {
                partner.assign(count, -1);
                candidate.assign(count, false);
                firstOf.assign(count, false);
            }
            // All of them, by x: the pairs were found only among the first 256 of the list, whose order and length
            // change as the camera turns, so a post near you paired and came apart by turns (its second lamp taking a
            // slot of its own for a frame or two, two shadows under a car); sorted, they depend on where lamps stand.
            static std::vector<uint16_t> candidates;
            static std::vector<bool> taken;
            candidates.clear();
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto& l = lights[i];
                if ((l.mFlags & rage::LF_DYNAMIC_SHADOW) && !(l.mFlags & (rage::LF_VEHICLE | rage::LF_TRAFFIC)) &&
                    l.mCastShadows && l.mCastShadows != -1 && (l.mType == rage::LT_POINT || l.mType == rage::LT_SPOT) &&
                    std::isfinite(l.mRadius) && l.mRadius > 0 && std::isfinite(l.mPosition.x) && !PlayerShadowAllocation::IsSharedMap(i))
                {
                    if (tracing) candidate[i] = true;
                    candidates.push_back(static_cast<uint16_t>(i));
                }
            }
            const auto n = static_cast<unsigned>(candidates.size());
            if (n > mostCandidates.load(std::memory_order_relaxed)) mostCandidates.store(n, std::memory_order_relaxed);
            std::sort(candidates.begin(), candidates.end(), [lights](uint16_t a, uint16_t b)
            {
                return lights[a].mPosition.x < lights[b].mPosition.x ||
                    (lights[a].mPosition.x == lights[b].mPosition.x && static_cast<uint32_t>(lights[a].mCastShadows) < static_cast<uint32_t>(lights[b].mCastShadows));
            });
            taken.assign(n, false);
            uint32_t inFrame = 0;
            for (unsigned a = 0; a < n; ++a)
            {
                if (taken[a]) continue;
                const auto& one = lights[candidates[a]];
                int best = -1;
                float bestD2 = distance * distance;
                for (unsigned b = a + 1; b < n && lights[candidates[b]].mPosition.x - one.mPosition.x < distance; ++b)
                {
                    if (taken[b]) continue;
                    const auto& other = lights[candidates[b]];
                    if (other.mType != one.mType || other.mInteriorIndex != one.mInteriorIndex || other.mRoomIndex != one.mRoomIndex ||
                        other.mCastShadows == one.mCastShadows ||
                        std::abs(other.mRadius - one.mRadius) > 0.25f * (std::max)(other.mRadius, one.mRadius))
                        continue;
                    if (one.mType == rage::LT_SPOT &&
                        one.mDirection.x * other.mDirection.x + one.mDirection.y * other.mDirection.y + one.mDirection.z * other.mDirection.z < 0.5f)
                        continue;
                    const float dx = other.mPosition.x - one.mPosition.x, dy = other.mPosition.y - one.mPosition.y, dz = other.mPosition.z - one.mPosition.z;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < bestD2)
                    {
                        bestD2 = d2;
                        best = int(b);
                    }
                }
                if (best < 0) continue;
                taken[a] = taken[best] = true;
                // The lamp with the lower key draws the map, whichever comes first in the list, so the slot keeps its key.
                uint16_t firstIndex = candidates[a], secondIndex = candidates[best];
                if (static_cast<uint32_t>(lights[secondIndex].mCastShadows) < static_cast<uint32_t>(lights[firstIndex].mCastShadows))
                    std::swap(firstIndex, secondIndex);
                const auto& first = lights[firstIndex];
                const auto& second = lights[secondIndex];
                const auto firstKey = static_cast<uint32_t>(first.mCastShadows), secondKey = static_cast<uint32_t>(second.mCastShadows);
                PlayerShadowAllocation::MarkSharedMap(secondIndex);
                if (tracing)
                {
                    partner[firstIndex] = secondIndex;
                    partner[secondIndex] = firstIndex;
                    firstOf[firstIndex] = true;
                }
                Link2(secondKey, firstKey, frame);
                for (uint32_t i = 0, c = Cell(firstKey); i < 8; ++i, c = (c + 1) & (Cells - 1))
                {
                    auto& o = origins[c];
                    if (o.first == firstKey || !o.first || frame - o.frame > 8)
                    {
                        o.first = firstKey;
                        o.frame = frame;
                        o.between[0] = (first.mPosition.x + second.mPosition.x) * 0.5f;
                        o.between[1] = (first.mPosition.y + second.mPosition.y) * 0.5f;
                        o.between[2] = (first.mPosition.z + second.mPosition.z) * 0.5f;
                        o.grow = std::sqrt(bestD2) * 0.5f;
                        break;
                    }
                }
                ++inFrame;
            }
            found += inFrame;
            if (inFrame > mostInFrame.load(std::memory_order_relaxed)) mostInFrame.store(inFrame, std::memory_order_relaxed);
            if (tracing) Trace(lights, count, partner, candidate, firstOf);
        }
    }

    // ---------------------------------------------------------------- shared maps
    // SharedMaps (Liberty's Shadows 2.0 aliases projections the same way): lights with a dynamic shadow only
    // that stand at one point with the same direction, type, range, cone, interior and room would each take a slot
    // to draw the same map, as models with a 2dfx light written twice do. The later ones take the first one's key,
    // so the lighting finds its map, and the game does not insert them among the candidates (the Collect hook).
    namespace Sharing
    {
        static bool enabled = true;
        static std::atomic<uint32_t> shared{0}, mostInFrame{0}, duplicates{0}, duplicatesUnshadowed{0}, duplicatesVehicle{0}, duplicatesOurs{0};

        static bool SameProjection(const rage::CLightSource& a, const rage::CLightSource& b) noexcept
        {
            return !std::memcmp(&a.mPosition, &b.mPosition, sizeof(a.mPosition)) &&
                !std::memcmp(&a.mDirection, &b.mDirection, sizeof(a.mDirection)) &&
                a.mType == b.mType && a.mRadius == b.mRadius && a.mInnerConeAngle == b.mInnerConeAngle &&
                a.mOuterConeAngle == b.mOuterConeAngle && a.mInteriorIndex == b.mInteriorIndex && a.mRoomIndex == b.mRoomIndex;
        }

        // The same key twice in one list (a light the game hands over twice in a frame, as some model lights and
        // headlights are) made the allocation pass reject the whole pass (rejected_duplicate, a quarter of the passes
        // in a log) and leave the slots to the game. A light the same as the first with its key uses its map; one
        // elsewhere with the same key would find the first's map, so it casts none.
        static void Duplicates(rage::CLightSource* lights, uint32_t count) noexcept
        {
            constexpr uint32_t Cells = 2048;
            std::array<uint16_t, Cells> table{}; // light index + 1
            for (uint32_t i = 0; i < count; ++i)
            {
                auto& light = lights[i];
                const auto key = static_cast<uint32_t>(light.mCastShadows);
                if (!(light.mFlags & rage::LF_DYNAMIC_SHADOW) || !key || key == UINT32_MAX || PlayerShadowAllocation::IsSharedMap(i)) continue;
                uint32_t cell = (key * 0x9E3779B1u) >> 21;
                for (; table[cell]; cell = (cell + 1) & (Cells - 1))
                {
                    const auto& first = lights[table[cell] - 1];
                    if (static_cast<uint32_t>(first.mCastShadows) != key) continue;
                    if (SameProjection(light, first))
                        PlayerShadowAllocation::MarkSharedMap(i);
                    else
                    {
                        light.mFlags &= ~rage::LF_DYNAMIC_SHADOW;
                        ++duplicatesUnshadowed;
                    }
                    ++duplicates;
                    if (light.mFlags & rage::LF_VEHICLE) ++duplicatesVehicle;
                    if (Ours::Of(key)) ++duplicatesOurs;
                    break;
                }
                if (!table[cell]) table[cell] = static_cast<uint16_t>(i + 1);
            }
        }

        static void Apply(rage::CLightSource* lights, uint32_t count) noexcept
        {
            if (!enabled) return;
            std::array<uint16_t, 256> candidates;
            std::array<bool, 256> isShared{};
            unsigned n = 0;
            for (uint32_t i = 0; i < count && n < candidates.size(); ++i)
            {
                const auto& l = lights[i];
                if ((l.mFlags & 6) == rage::LF_DYNAMIC_SHADOW && !(l.mFlags & (rage::LF_VEHICLE | rage::LF_TRAFFIC)) &&
                    l.mCastShadows && l.mCastShadows != -1 && l.mShadowCacheIndex < 0 &&
                    (l.mType == rage::LT_POINT || l.mType == rage::LT_SPOT) && std::isfinite(l.mRadius) && l.mRadius > 0)
                    candidates[n++] = static_cast<uint16_t>(i);
            }
            uint32_t inFrame = 0;
            for (unsigned a = 1; a < n; ++a)
                for (unsigned b = 0; b < a; ++b)
                {
                    if (isShared[b]) continue;
                    auto& light = lights[candidates[a]];
                    const auto& first = lights[candidates[b]];
                    if (light.mCastShadows == first.mCastShadows || !SameProjection(light, first)) continue; // the same key: Duplicates
                    light.mCastShadows = first.mCastShadows;
                    PlayerShadowAllocation::MarkSharedMap(candidates[a]);
                    isShared[a] = true;
                    ++inFrame;
                    break;
                }
            shared += inFrame;
            if (inFrame > mostInFrame.load(std::memory_order_relaxed)) mostInFrame.store(inFrame, std::memory_order_relaxed);
        }
    }

    // Liberty's Shadows 2.0.4 (OnyxOak, GPL-3.0): ArcadePlacement197. An arcade in Algonquin (model 0x2E73D323 at
    // -19.29, -350.38, 16.79) has two lamp effects authored 1.4 to 1.7 m below their fittings (model 0x90507341, the
    // same place and heading), inside the building: with ModelLightShadows their shadow maps were drawn from in there.
    // A shadowed light of exactly theirs (position bits, colour, intensity 20, range 10, flags 0x41) moves to the
    // fitting, 30 mm out from its nearest triangle. Its map is dynamic only, drawn from where it is each frame, so the
    // game's key does: no cached map from the old place.
    namespace Arcade
    {
        static std::atomic<uint32_t> moved{0};
        constexpr float Origin[3] = { -19.28564453125f, -350.38043212890625f, 16.78708267211914f };
        constexpr float Authored[2][3] = { { -34.497642517089844f, -326.367645263671875f, 15.356832504272461f },
                                           { -26.226375579833984f, -320.151123046875f, 15.356832504272461f } };
        constexpr float Fitting[2][3] = { { -15.074825134375509f, 24.0128f, 0.23388017289465424f },
                                          { -6.94073f, 30.2293f, 0.2287890625f } };

        static void Place(rage::CLightSource* lights, uint32_t count) noexcept
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                auto& l = lights[i];
                if (l.mType != rage::LT_POINT || (l.mFlags & ~rage::LF_DYNAMIC_SHADOW) != 0x41 || !(l.mFlags & rage::LF_DYNAMIC_SHADOW) ||
                    l.mIntensity != 20.0f || l.mRadius != 10.0f || l.mColor.x != 1.0f || l.mColor.y != 0.9647059440612793f ||
                    l.mColor.z != 0.7372549176216125f)
                    continue;
                for (unsigned n = 0; n < 2; ++n)
                {
                    if (l.mPosition.x != Authored[n][0] || l.mPosition.y != Authored[n][1] || l.mPosition.z != Authored[n][2]) continue;
                    l.mPosition.x = Origin[0] + Fitting[n][0];
                    l.mPosition.y = Origin[1] + Fitting[n][1];
                    l.mPosition.z = Origin[2] + Fitting[n][2];
                    ++moved;
                    break;
                }
            }
        }
    }

    // Before the game picks the night shadow slots (PlayerShadowAllocation::prepareSources): rear lamp and headlight
    // keys whose car went lose their shadow, the cached lamps join the dynamic slots, and lights at one point share
    // a map.
    static void PrepareSources(rage::CLightSource* lights, uint32_t count) noexcept
    {
        if (!lights || count > 4096) return;
        for (uint32_t i = 0; i < count; ++i)
        {
            auto& light = lights[i];
            const auto key = static_cast<uint32_t>(light.mCastShadows);
            if ((light.mFlags & rage::LF_DYNAMIC_SHADOW) && LampKeys::Contains(key) && !LampKeys::Owner(key))
                light.mFlags &= ~rage::LF_DYNAMIC_SHADOW;
        }
        World::PromoteCachedLamps(lights, count);
        Arcade::Place(lights, count);
        MergeWithNative(lights, count);
        IdleModelLights(lights, count);
        Sharing::Apply(lights, count);
        Sharing::Duplicates(lights, count);
        Pairs::Find(lights, count);
    }

    // The car whose lamp (headlight of a pair or rear lamp) a slot's key is, so its own map leaves it out.
    static uintptr_t LampOwner(uint32_t key) noexcept { return LampKeys::Owner(key); }

    static void Install(CIniReader& ini) noexcept
    {
        game = GameBase();
        installStatus.clear();
        Dual::Install();
        Tail::Install();
        PlayerShadowAllocation::isTailLightKey = Tail::IsKey;
        PlayerShadowAllocation::lampKeyOwner = LampOwner;
        PlayerShadowAllocation::lightOrigin = [](uint32_t key) noexcept -> uint8_t
        {
            using Trace = fusionfix::shadows::ShadowSlotTrace;
            if (LampKeys::Contains(key))
                return LampKeys::ChannelOf(key) >= LampKeys::TailLight0 ? Trace::OriginTailLight : Trace::OriginHeadlight;
            const auto kind = Ours::Of(key);
            return kind == Ours::ModelLight ? Trace::OriginModelLight : kind == Ours::CoronaLight ? Trace::OriginCorona : Trace::OriginGame;
        };
        World::Install(ini);
        Buildings::Install();
        Sharing::enabled = ini.ReadInteger("NIGHTSHADOWS", "SharedMaps", 1) != 0;
        Pairs::distance = std::clamp(ini.ReadFloat("NIGHTSHADOWS", "PairedLampDistance", 2.5f), 0.0f, 6.0f);
        if (Pairs::distance > 0)
        {
            HeadlightEnhancement::slotOrigin = Pairs::OriginOf;
            VehicleBoxShadowsBridge::SharedMapKey = Pairs::FirstOf;
            VehicleBoxShadowsBridge::LampPair = Pairs::PairOf;
        }
        PlayerShadowAllocation::prepareSources = PrepareSources;
        OwnHeadlightCaster::lampOwner = LampOwner;
        log.Name("NightShadows", "Lighting20");
    }

    static void Update() noexcept
    {
        log.Write(std::ios::app, [](std::ostream& out, uint64_t now)
        {
            out << "tick=" << now << installStatus
                << " | dual " << (Dual::On() ? "on" : "off") << " pairs=" << Dual::pairs.load() << " fallbacks=" << Dual::fallbacks.load()
                << " | tail " << (Tail::On() ? "on" : "off") << " promoted=" << Tail::promoted.load() << " refused=" << Tail::refused.load()
                << " | world " << (World::On() ? "on" : "off") << " cached_lamps=" << World::lampsPromoted.load()
                << " model_lights=" << World::modelLightsPromoted.load() << " corona_lights=" << World::coronaLights.load()
                << " corona_refused=" << World::coronasRefused.load() << " corona_capped=" << World::coronasCapped.load()
                << " arcade_moved=" << Arcade::moved.load() << " merged: coronas=" << World::coronasMerged.load() << " model_lights=" << World::modelLightsMerged.load()
                << " model_lights_without_casters=" << World::modelLightsIdle.load()
                << " | buildings " << (Buildings::On() ? "on" : "off") << " passes=" << Buildings::passes.load()
                << " masks=" << Buildings::masks.load() << " draws=" << Buildings::draws.load()
                << " | shared maps " << (Sharing::enabled ? "on" : "off") << " lights=" << Sharing::shared.load()
                << " duplicates=" << Sharing::duplicates.load() << " duplicates_unshadowed=" << Sharing::duplicatesUnshadowed.load()
                << " (vehicle " << Sharing::duplicatesVehicle.load() << ", model lights " << Sharing::duplicatesOurs.load() << ')'
                << " most_in_frame=" << Sharing::mostInFrame.load() << " not_inserted=" << PlayerShadowAllocation::sharedMapsSkipped.load()
                << " | lamp pairs " << (Pairs::distance > 0 ? "on" : "off") << " found=" << Pairs::found.load()
                << " most_in_frame=" << Pairs::mostInFrame.load() << " most_candidates=" << Pairs::mostCandidates.load()
                << " second_lamp_lookups=" << Pairs::lookedUp.load()
                << " | stable map slots " << (PlayerShadowAllocation::preCacheHook ? (PlayerShadowAllocation::stableOrdinals ? "on" : "vacancy only") : "off")
                << " passes=" << PlayerShadowAllocation::stableOrdinalPasses.load() << " moves=" << PlayerShadowAllocation::stableOrdinalMoves.load()
                << " rejected=" << PlayerShadowAllocation::stableOrdinalRejected.load() << '\n';
        });
    }
}
