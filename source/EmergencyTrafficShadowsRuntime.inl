// OnyxOak modification project: Liberty's Shadows - Extra Night Shadows Fix and Enhancements.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-10-04 (CE 1.8). See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

// CE 1.8's traffic signal and emergency light shadows, from the standalone adapter's
// traffic-signal-observer.inc and police-nearby.inc. Both give an existing native light the
// shadow flag and a reserved key as it is submitted; no light is added, and the native seven
// shadow slots stay as they are. Audited for CE 1.2.0.59 only, behind the CE adapter.
namespace EmergencyTrafficShadows
{
    using namespace PlayerShadowAllocation;

    inline constexpr uint32_t SubmitRva = fusionfix::shadows::ce::SubmitRva;
    inline constexpr uint32_t TrafficSiteRva = 0x921029;      // the traffic signal's call to submit
    inline constexpr uint32_t VacancyRva = 0x527DFC;          // the selection, before the cache pick
    inline constexpr uint32_t PoolFreeRva = 0x70BA00;         // atPool free, ecx the pool, arg the object
    inline constexpr uint32_t TimerRestoreRva = 0x4D0B97;     // the frame counter restored from its backup
    inline constexpr uint32_t TimerBackupActiveRva = 0xD73610, TimerBackupFrameRva = 0xD73684;
    inline constexpr uint32_t BuildingPoolRva = 0xEBD0E8, ObjectPoolRva = 0x1232C60;
    inline constexpr uint32_t ModelInfoTableRva = 0xE95CD8, ModelInfoTableUseRva = 0x63EB37;
    inline constexpr uint32_t VacancyFlagRva = 0xD9D011;

    static uintptr_t game = 0;
    static bool trafficRequested = false, emergencyRequested = false;
    static SafetyHookMid submitHook, vacancyHook, freeHook, timerRestoreHook;
    static fusionfix::DiagnosticsLog log;
    static std::string installStatus = "not_requested";

    static uint32_t Frame() noexcept { return *CTimer::m_frameCount; }

    static bool Readable(uintptr_t address, size_t size) noexcept
    {
        if (address < 0x10000 || !size || address > UINTPTR_MAX - size) return false;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        return address >= base && address + size <= base + info.RegionSize;
    }

    // The 64-byte cell a reserved key points at, or tokens when the key is none of them.
    static constexpr unsigned TokenIndex(uint32_t key, uint32_t base, unsigned tokens) noexcept
    {
        if (key < base || ((key - base) & 63u)) return tokens;
        const auto index = (key - base) / 64;
        return index < tokens ? index : tokens;
    }

    // Keys are addresses of storage reserved here, so they never match a game object's key.
    static bool ReservedKeyFree(uint32_t key) noexcept
    {
        const auto* lights = CurrentLights(); const auto count = CurrentCount();
        if (count > 4096 || (count && reinterpret_cast<uintptr_t>(lights) < 0x10000)) return false;
        for (uint32_t i = 0; i < count; ++i) if (static_cast<uint32_t>(lights[i].mCastShadows) == key) return false;
        for (unsigned i = 0; i < 8; ++i)
            if (*reinterpret_cast<const uint32_t*>(game + allocation::CacheAge0Rva + i * 0x100 + 4) == key) return false;
        for (unsigned i = 0; i < 7; ++i)
            if (*reinterpret_cast<const uint32_t*>(game + allocation::DynamicPassFirstPositionRva + i * 0x110 + 0x30) == key) return false;
        return true;
    }

    // Traffic signals: one reserved key per building or object pool lifetime of the signal.
    namespace TrafficSignal
    {
        constexpr uint32_t KeyCapacity = 16384;
        std::atomic<bool> enabled{false}, fatal{false};
        std::atomic<uint32_t> observerThread{0}, issuedKeys{0}, fatalReason{0};
        alignas(64) uint8_t reservedIdentities[KeyCapacity][64]{};
        traffic_signal::Registry<KeyCapacity> registry;
        traffic_signal::PlayerPositionSnapshot playerPositionSnapshot;
        // 0 alive, 1 admitted, 2 retired; pool and owner for frees seen off the observer thread.
        struct Snapshot { std::atomic<uint32_t> state{0}, pool{0}, owner{0}; };
        std::array<Snapshot, KeyCapacity> snapshots;
        std::atomic<uint32_t> admitted{0}, collisions{0};
        uint32_t validationCursor{};

        struct Match { traffic_signal::Identity identity; uint32_t kind; };
        static bool PoolMatch(uintptr_t owner, uint32_t pointerRva, uint32_t stride, uint32_t kind, Match& result) noexcept
        {
            const auto pool = *reinterpret_cast<rage::fwBasePool**>(game + pointerRva);
            if (!Readable(reinterpret_cast<uintptr_t>(pool), 0x1c)) return false;
            if (pool->m_nSize <= 0 || pool->m_nSize > 65536 || pool->m_nStorageSize != static_cast<int32_t>(stride)) return false;
            const auto storage = reinterpret_cast<uintptr_t>(pool->m_aStorage), flags = reinterpret_cast<uintptr_t>(pool->m_aFlags);
            const uint64_t bytes = uint64_t(pool->m_nSize) * stride;
            if (owner < storage || uint64_t(owner - storage) >= bytes || (owner - storage) % stride || !Readable(owner, 0x30)) return false;
            const auto slot = uint32_t((owner - storage) / stride);
            if (!Readable(flags + slot, 1)) return false;
            const auto reference = pool->m_aFlags[slot]; if (reference & 0x80) return false; // free
            const auto index = *reinterpret_cast<const int16_t*>(owner + 0x2e); if (index < 0 || index >= 31000) return false;
            const auto info = *reinterpret_cast<const uintptr_t*>(game + ModelInfoTableRva + index * 4);
            if (!Readable(info, 0x40)) return false;
            const auto model = *reinterpret_cast<const uint32_t*>(info + 0x3c); if (!model) return false;
            const auto matrix = *reinterpret_cast<const uintptr_t*>(owner + 0x20); if (!Readable(matrix, 64)) return false;
            for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(reinterpret_cast<const float*>(matrix)[12 + i])) return false;
            if (pool->m_aFlags[slot] != reference) return false;
            result.identity = {uint32_t(reinterpret_cast<uintptr_t>(pool)), uint32_t(storage), uint32_t(flags), uint32_t(owner), slot, reference, model};
            result.kind = kind; return true;
        }
        static bool MatchOwner(uintptr_t owner, Match& value) noexcept
        {
            Match b{}, o{};
            const bool building = PoolMatch(owner, BuildingPoolRva, 0x70, 0, b), object = PoolMatch(owner, ObjectPoolRva, 0x320, 1, o);
            if (building == object) return false;
            value = building ? b : o; return true;
        }
        static uint32_t PrivateKey() noexcept { return uint32_t(reinterpret_cast<uintptr_t>(reservedIdentities)); }
        static uint32_t KeyIndex(uint32_t key) noexcept
        {
            const auto base = PrivateKey(); if (key < base || (key - base) % 64) return UINT32_MAX;
            const auto index = (key - base) / 64; return index < issuedKeys.load(std::memory_order_acquire) ? index : UINT32_MAX;
        }
        static uint64_t LampGeneration(const rage::CLightSource& light) noexcept
        {
            const auto key = uint32_t(light.mCastShadows), index = KeyIndex(key);
            return index != UINT32_MAX && traffic_signal::StoredPrivateTraffic(light.mType, light.mFlags, key, key) ? uint64_t(index) + 1 : 0;
        }
        static void Retire(uint32_t index, traffic_signal::End reason) noexcept
        {
            if (index < registry.Used() && registry.Retire(index, reason)) snapshots[index].state.store(2, std::memory_order_release);
        }
        static void FatalThread() noexcept { if (!fatal.exchange(true)) fatalReason = 7; }
        static uint32_t KeyCollision(uint32_t key) noexcept
        {
            const auto count = *reinterpret_cast<const uint32_t*>(game + traffic_signal::SubmissionCountRva);
            const auto* lights = *reinterpret_cast<rage::CLightSource* const*>(game + traffic_signal::SubmissionPointerRva);
            const auto address = reinterpret_cast<uintptr_t>(lights), completed = reinterpret_cast<uintptr_t>(CurrentLights());
            const bool readable = count <= traffic_signal::SubmissionCapacity && (!count || Readable(address, size_t(count) * sizeof(rage::CLightSource)));
            return traffic_signal::SubmissionCollision(count, address, completed, readable, key, [lights](uint32_t i) { return uint32_t(lights[i].mCastShadows); });
        }
        static bool PlayerPosition(uint32_t frame, float (&position)[3]) noexcept
        {
            auto& cached = playerPositionSnapshot;
            if (frame != cached.frame)
            {
                cached.frame = frame; cached.valid = false;
                const auto ped = CPlayer::getLocalPlayerPed ? CPlayer::getLocalPlayerPed() : 0;
                if (Readable(ped + 0x20, 4))
                {
                    const auto matrix = *reinterpret_cast<const uintptr_t*>(ped + 0x20);
                    if (Readable(matrix, 64))
                    {
                        cached.valid = true;
                        for (unsigned i = 0; i < 3; ++i)
                        {
                            cached.position[i] = reinterpret_cast<const float*>(matrix)[12 + i];
                            cached.valid = cached.valid && std::isfinite(cached.position[i]);
                        }
                    }
                }
            }
            if (cached.valid) std::memcpy(position, cached.position, 12);
            return cached.valid;
        }

        // Submit from the traffic signal: esi the signal, edi its phase, arguments as submitted.
        static void Observe(SafetyHookContext& regs) noexcept
        {
            if (!enabled.load() || fatal.load() || !Enabled()) return;
            if (GetCurrentThreadId() != observerThread.load()) { FatalThread(); return; }
            if (!Readable(regs.esp, 17 * 4)) return;
            auto* args = reinterpret_cast<uint32_t*>(regs.esp);
            if (args[2] != 2 || args[3] != 0x200 || args[16] || regs.edi > 2) return;
            if (!Readable(args[6], 12) || !Readable(args[7], 12)) return;
            const auto* pos = reinterpret_cast<const float*>(args[6]); const auto* rgb = reinterpret_cast<const float*>(args[7]);
            for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(pos[i]) || !std::isfinite(rgb[i])) return;
            const auto frame = Frame();
            // CE 1.8 measures the signals' reach in metres of the lamp reach step.
            const auto reach = float(std::clamp(ShadowReachStep(false), 0, 100));
            if (traffic_signal::RejectUntrackedFar(regs.esi, frame, pos, reach, playerPositionSnapshot,
                [] { return Enabled(); }, [](uint32_t owner) { return registry.TracksOwner(owner); })) return;
            Match value{}; if (!MatchOwner(regs.esi, value)) return;
            float player[3]{};
            const bool relevant = PlayerPosition(frame, player) && traffic_signal::Nearby(pos, player, reach);
            const auto result = registry.Observe(value.identity, frame, regs.edi, relevant);
            if (result.replaced != UINT32_MAX) snapshots[result.replaced].state = 2;
            if (result.visit == traffic_signal::Visit::Capacity || result.index == UINT32_MAX) return;
            const auto index = result.index; auto& entry = registry.entries[index];
            if (result.visit == traffic_signal::Visit::New)
            {
                snapshots[index].state = 0; snapshots[index].pool = value.identity.pool; snapshots[index].owner = value.identity.owner;
                issuedKeys.store(registry.Used(), std::memory_order_release);
            }
            else if (result.visit == traffic_signal::Visit::Retired) { snapshots[index].state = 2; return; }
            if (!entry.leased) { snapshots[index].state = 0; return; }
            if (entry.submitted && entry.lastAdmission == frame) return;
            const auto collision = KeyCollision(entry.key);
            if (registry.Apply(index, value.identity, frame, TrafficSiteRva, args[2], regs.edi, collision != 0, args[3], args[16]))
            {
                snapshots[index].state.store(1, std::memory_order_release); ++admitted;
            }
            else if (collision)
            {
                ++collisions; snapshots[index].state = 2;
                if (collision != 1) { fatalReason = collision; fatal = true; }
            }
        }

        static void Free(SafetyHookContext& regs) noexcept
        {
            const fusionfix::shadows::FloatingPointState fp;
            if (!enabled.load() || fatal.load() || !Readable(regs.esp + 4, 4)) return;
            const auto owner = *reinterpret_cast<const uint32_t*>(regs.esp + 4);
            if (GetCurrentThreadId() != observerThread.load())
            {
                for (uint32_t i = 0; i < issuedKeys.load(std::memory_order_acquire); ++i)
                    if (snapshots[i].state.load() != 2 && snapshots[i].pool.load() == regs.ecx && snapshots[i].owner.load() == owner) { FatalThread(); break; }
                return;
            }
            const auto index = registry.Free(regs.ecx, owner); if (index == UINT32_MAX) return;
            snapshots[index].state = 2;
        }

        // The game puts its frame counter back from a backup (load, replay); rebase the leases on it.
        static void NativeTimerRestore(SafetyHookContext& regs) noexcept
        {
            const fusionfix::shadows::FloatingPointState fp;
            if (!enabled.load() || fatal.load()) return;
            if (GetCurrentThreadId() != observerThread.load()) { FatalThread(); return; }
            const auto frame = Frame(), saved = *reinterpret_cast<const uint32_t*>(game + TimerBackupFrameRva);
            if (*reinterpret_cast<const uint8_t*>(game + TimerBackupActiveRva) != 1 || frame != saved) return;
            for (uint32_t i = 0; i < registry.Used(); ++i)
            {
                auto& entry = registry.entries[i]; if (entry.retired || !entry.leased) continue;
                Match current{};
                if (!MatchOwner(entry.id.owner, current) || !(current.identity == entry.id)) { Retire(i, traffic_signal::End::Identity); continue; }
                if (registry.Rebase(i, current.identity, frame, saved, true)) snapshots[i].state = 0;
            }
        }

        // After the selection: a reserved key in two slots retires its signal.
        static void Result() noexcept
        {
            if (!issuedKeys.load() || fatal.load()) return;
            if (GetCurrentThreadId() != observerThread.load()) { FatalThread(); return; }
            std::array<uint32_t, 7> keys{}; unsigned count = 0;
            for (unsigned slot = 0; slot < 7; ++slot)
            {
                const auto record = game + allocation::DynamicPassFirstPositionRva + slot * 0x110;
                if (!*reinterpret_cast<const uint8_t*>(record + 0x25)) continue;
                const auto key = *reinterpret_cast<const uint32_t*>(record + 0x30), index = KeyIndex(key); if (index == UINT32_MAX) continue;
                bool duplicate = false; for (unsigned j = 0; j < count; ++j) duplicate = duplicate || keys[j] == key;
                keys[count++] = key;
                if (duplicate) { ++collisions; fatalReason = 4; Retire(index, traffic_signal::End::Collision); }
            }
        }

        // Game-process callback, on the thread that submits lights; it becomes the observer thread.
        static void Poll() noexcept
        {
            if (!trafficRequested || registry.Disabled()) return;
            if (!observerThread.load())
            {
                observerThread.store(GetCurrentThreadId(), std::memory_order_release);
                enabled.store(true, std::memory_order_release);
            }
            if (GetCurrentThreadId() != observerThread.load()) { FatalThread(); return; }
            if (fatal.load())
            {
                registry.Disable(traffic_signal::End::Thread);
                for (uint32_t i = 0; i < registry.Used(); ++i) snapshots[i].state = 2;
                return;
            }
            // Revalidate up to 32 signals a frame; leases lapse after two frames unseen.
            const auto frame = Frame();
            for (unsigned n = 0, visits = (std::min)(registry.Used(), 32u); n < visits; ++n)
            {
                if (validationCursor >= registry.Used()) validationCursor = 0;
                const auto index = validationCursor++; auto& entry = registry.entries[index]; if (entry.retired) continue;
                Match current{};
                if (!MatchOwner(entry.id.owner, current) || !(current.identity == entry.id)) { Retire(index, traffic_signal::End::Identity); continue; }
                const auto age = uint32_t(frame - entry.lastFrame);
                if (age >= 0x80000000u) { Retire(index, traffic_signal::End::Clock); continue; }
                if (age > 2 && registry.Suspend(index)) snapshots[index].state = 0;
            }
        }
    }

    // Emergency vehicle lights: up to two vehicles with their sirens on within 30 m (the player's
    // own always first), each emitter on one reserved key for the vehicle's lifetime.
    namespace Emergency
    {
        lab_siren::NearbyPolice nearbyPolice;
        alignas(64) std::array<std::array<uint8_t, 64>, lab_siren::NearbyPolice::Tokens> nearbyKeys{};
        std::array<std::atomic<bool>, lab_siren::NearbyPolice::Tokens> nearbyKeyUsed{};
        std::array<lab_siren::Vacancy, lab_siren::NearbyPolice::Candidates> nearbyVacancies{};
        std::array<uint32_t, lab_siren::NearbyPolice::Candidates> nearbyVacancyGeneration{};
        std::atomic_flag nearbyLock = ATOMIC_FLAG_INIT;
        std::atomic<uint32_t> bound{0}, promoted{0}, vacancySuppressed{0};
        struct NearbyLock
        {
            bool acquired;
            NearbyLock() noexcept : acquired(!nearbyLock.test_and_set(std::memory_order_acquire)) {}
            ~NearbyLock() { if (acquired) nearbyLock.clear(std::memory_order_release); }
        };
        static uint32_t NearbyKey(unsigned token) noexcept { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(nearbyKeys[token].data())); }
        static bool NearbyPoint(const rage::CLightSource& l) noexcept
        {
            if (l.mType != rage::LT_POINT) return false;
            const auto token = TokenIndex(static_cast<uint32_t>(l.mCastShadows), NearbyKey(0), nearbyKeys.size());
            if (token == nearbyKeys.size()) return false;
            return nearbyKeyUsed[token].load(std::memory_order_acquire);
        }
        // Reads only; an unfamiliar model hash is left alone.
        static bool ReadNearbyOwner(uintptr_t owner, lab_siren::Identity& id, uint32_t& model, bool& on) noexcept
        {
            __try
            {
                const auto pool = CVehicle::GetVehiclePool();
                if (!owner || !pool || !pool->m_aStorage || !pool->m_aFlags || pool->m_nSize <= 0 || pool->m_nSize > 4096 ||
                    pool->m_nStorageSize < 0xF1F || pool->m_nStorageSize > 0x10000) return false;
                const auto base = reinterpret_cast<uintptr_t>(pool->m_aStorage);
                if (owner < base || uint64_t(owner - base) >= uint64_t(pool->m_nSize) * pool->m_nStorageSize || (owner - base) % pool->m_nStorageSize) return false;
                const auto slot = static_cast<uint32_t>((owner - base) / pool->m_nStorageSize); if (pool->GetIsFree(slot)) return false;
                const int index = *reinterpret_cast<const int16_t*>(owner + 0x2E); if (index < 0 || index >= 31000) return false;
                const auto info = *reinterpret_cast<const uintptr_t*>(game + ModelInfoTableRva + index * 4); if (info < 0x10000 || (info & 3)) return false;
                model = *reinterpret_cast<const uint32_t*>(info + 0x3C); if (!lab_siren::PoliceModel(model)) return false;
                id = {owner, reinterpret_cast<uintptr_t>(pool), slot, pool->GetReference(slot)};
                on = lab_siren::NativeSirenOn(*reinterpret_cast<const uint8_t*>(owner + 0xF19), *reinterpret_cast<const uint8_t*>(owner + 0xF1E));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        static bool NearbyOrigin(fusionfix::shadows::Vec3& origin) noexcept
        {
            __try
            {
                const auto ped = CPlayer::getLocalPlayerPed ? CPlayer::getLocalPlayerPed() : 0; if (!ped) return false;
                const auto matrix = *reinterpret_cast<const float* const*>(ped + 0x20); if (!matrix) return false;
                origin = {matrix[12], matrix[13], matrix[14]};
                return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        static bool NearbyValid(lab_siren::NearbyPolice::Owner& o) noexcept
        {
            lab_siren::Identity id{}; uint32_t model{}; bool on = false;
            if (!ReadNearbyOwner(o.identity.owner, id, model, on) || !(id == o.identity) || model != o.model) return false;
            o.eligible = o.eligible && on; o.player = CPlayer::findPlayerCar && CPlayer::findPlayerCar() == o.identity.owner; return true;
        }
        static void RefreshNearby() noexcept { nearbyPolice.Begin(Frame(), GetTickCount(), NearbyValid); }

        static void Update() noexcept
        {
            if (!emergencyRequested) return;
            NearbyLock lock; if (!lock.acquired) return;
            RefreshNearby();
            unsigned live = 0; for (const auto& o : nearbyPolice.owners) if (o.live) ++live;
            bound = live;
        }

        // Submit from one of the six siren emitters; the owner register depends on the emitter.
        static void Promote(SafetyHookContext& regs, uint32_t caller) noexcept
        {
            if (!Enabled()) return;
            auto* args = reinterpret_cast<uint32_t*>(regs.esp);
            if (args[2] != 0 || args[3] != 0x110 || args[16] != 0) return;
            const auto owner = lab_siren::PoliceOwnerAtSite(caller, regs.edi, regs.esi, regs.ecx);
            lab_siren::Identity id{}; uint32_t model{}; bool on = false;
            if (!ReadNearbyOwner(owner, id, model, on) || !on) return;
            float input{}, radius{}; memcpy(&input, args + 8, 4); memcpy(&radius, args + 11, 4);
            if (!std::isfinite(input) || input < 0 || !std::isfinite(radius) || radius <= 0 || radius > 100) return;
            const auto* xyz = reinterpret_cast<const float*>(args[6]); if (reinterpret_cast<uintptr_t>(xyz) < 0x10000) return;
            const fusionfix::shadows::Vec3 position{xyz[0], xyz[1], xyz[2]};
            fusionfix::shadows::Vec3 origin{}; if (!NearbyOrigin(origin)) return;
            const float x = position.x - origin.x, y = position.y - origin.y, z = position.z - origin.z, d = x * x + y * y + z * z;
            const bool player = CPlayer::findPlayerCar && CPlayer::findPlayerCar() == owner;
            const bool eligible = std::isfinite(d) && (player || d <= 30.f * 30.f) &&
                fusionfix::shadows::ShadowVolumeMayReachView(ReadGameplayView(), position, radius);
            NearbyLock lock; if (!lock.acquired) return;
            RefreshNearby();
            auto* o = nearbyPolice.Touch(id, model, caller, d, player, eligible, [](unsigned token) { return ReservedKeyFree(NearbyKey(token)); });
            if (!o) return;
            nearbyKeyUsed[o->token].store(true, std::memory_order_release);
            if (!nearbyPolice.Promote(*o, args[8])) return;
            args[3] |= rage::LF_DYNAMIC_SHADOW; args[16] = NearbyKey(o->token); ++promoted;
        }

        static lab_siren::NearbyPolice::Owner* NearbyOwnerForKey(uint32_t key) noexcept
        {
            const auto token = TokenIndex(key, NearbyKey(0), nearbyKeys.size()); if (token == nearbyKeys.size()) return nullptr;
            for (auto& o : nearbyPolice.owners) if (o.live && o.token == token) return &o;
            return nullptr;
        }
        static void BeginSelection(const rage::CLightSource*, uint32_t) noexcept { NearbyLock lock; if (lock.acquired) RefreshNearby(); }

        // The selection's comparison: the primary siren takes a slot from any lamp, the secondary
        // yields to a relevant visible lamp; headlights and special lights are left alone.
        static int Compare(int, int unchanged, int challenger, int incumbent) noexcept
        {
            const auto* lights = CurrentLights();
            if (!lights || challenger < 0 || incumbent < 0 || static_cast<uint32_t>(challenger) >= CurrentCount() ||
                static_cast<uint32_t>(incumbent) >= CurrentCount()) return unchanged;
            NearbyLock lock; if (!lock.acquired) return unchanged;
            auto* a = NearbyOwnerForKey(static_cast<uint32_t>(lights[challenger].mCastShadows));
            auto* b = NearbyOwnerForKey(static_cast<uint32_t>(lights[incumbent].mCastShadows));
            const bool ap = a && a->rank < 2 && a->eligible && NearbyValid(*a), bp = b && b->rank < 2 && b->eligible && NearbyValid(*b);
            const auto& ca = state.nativeCandidates[challenger]; const auto& cb = state.nativeCandidates[incumbent];
            const auto visibleLamp = [&](int i)
            {
                const auto& l = lights[i];
                return !NearbyPoint(l) && !(l.mFlags & rage::LF_VEHICLE) && labRelevant[i] &&
                    fusionfix::shadows::ShadowVolumeMayReachView(state.view, {l.mPosition.x, l.mPosition.y, l.mPosition.z}, l.mRadius);
            };
            const bool needVisible = ap != bp;
            return lab_siren::NearbyPolice::Compare(unchanged, ap, bp, ap ? a->rank : 2, bp ? b->rank : 2, ca.observed, cb.observed,
                labRelevant[challenger] != 0, labRelevant[incumbent] != 0, ca.ownBeam, cb.ownBeam,
                (ca.flags & 0x400u) != 0, (cb.flags & 0x400u) != 0,
                needVisible && visibleLamp(challenger), needVisible && visibleLamp(incumbent));
        }

        // Before the cache pick: when a siren blinks off, the lamp that took its slot gives it
        // back the next frame instead of the six incumbents shuffling.
        static void ApplyVacancy(SafetyHookContext& regs) noexcept
        {
            const fusionfix::shadows::FloatingPointState fp;
            if (!Enabled() || state.depth != 1 || !state.pass.Active() || !state.continuityActive || !state.stackAnchor ||
                regs.esp != state.stackAnchor || Frame() != state.frame) return;
            const auto* lights = CurrentLights(); const auto count = CurrentCount(); if (!lights || count > 4096) return;
            NearbyLock lock; if (!lock.acquired) return;
            auto* indices = reinterpret_cast<int32_t*>(regs.esp + allocation::SelectedIndicesStackOffset);
            for (unsigned oi = 0; oi < nearbyPolice.owners.size(); ++oi)
            {
                auto& o = nearbyPolice.owners[oi]; if (!o.live) continue;
                if (nearbyVacancyGeneration[oi] != o.generation) { nearbyVacancies[oi] = lab_siren::Vacancy{}; nearbyVacancyGeneration[oi] = o.generation; }
                std::array<lab_siren::VacancyEntry, 7> entries{};
                for (unsigned j = 0; j < 7; ++j)
                {
                    const auto i = indices[j]; if (i == -1) continue;
                    if (i < 0 || static_cast<uint32_t>(i) >= count || !state.nativeCandidates[i].observed) return;
                    const auto& l = lights[i]; const auto* privateOwner = NearbyOwnerForKey(static_cast<uint32_t>(l.mCastShadows));
                    entries[j] = {static_cast<uint32_t>(l.mCastShadows),
                        privateOwner ? privateOwner->generation : ((l.mFlags & rage::LF_VEHICLE) ? 0 : LampGeometry(l)),
                        state.nativeCandidates[i].ownBeam || (l.mFlags & 0x400u) != 0 ||
                            (o.rank == 1 && !privateOwner && !(l.mFlags & rage::LF_VEHICLE) && labRelevant[i])};
                }
                float intensity{}; memcpy(&intensity, &o.inputBits, 4); const bool valid = NearbyValid(o);
                const int drop = nearbyVacancies[oi].Filter(state.frame, GetTickCount(), valid, o.rank < 2 && o.eligible, valid && o.eligible,
                    o.inputFrame + 1 == state.frame, intensity, NearbyKey(o.token), entries);
                if (drop < 0) continue;
                const auto removed = indices[drop]; indices[drop] = -1;
                auto* pending = reinterpret_cast<int32_t*>(regs.esp + 0x28);
                if (*pending == removed) { *pending = -1; regs.esi = UINT32_MAX; }
                ++vacancySuppressed;
            }
        }
    }

    static void OnSubmit(SafetyHookContext& regs) noexcept
    {
        const fusionfix::shadows::FloatingPointState fp;
        const uint32_t caller = *reinterpret_cast<const uint32_t*>(regs.esp) - static_cast<uint32_t>(game) - 5;
        if (caller == TrafficSiteRva) { if (trafficRequested) TrafficSignal::Observe(regs); }
        else if (emergencyRequested && lab_siren::PoliceSite(caller)) Emergency::Promote(regs, caller);
    }
    static void OnSelectionResult() noexcept { if (trafficRequested) TrafficSignal::Result(); }

    static bool Bytes(uint32_t rva, std::initializer_list<uint8_t> bytes) noexcept
    {
        return !std::memcmp(reinterpret_cast<const void*>(game + rva), bytes.begin(), bytes.size());
    }
    static uint32_t Word(uint32_t rva) noexcept { return *reinterpret_cast<const uint32_t*>(game + rva); }
    static bool CallsSubmit(uint32_t rva) noexcept
    {
        return Bytes(rva, {0xE8}) && rva + 5 + Word(rva + 1) == SubmitRva;
    }

    // After the allocation adapter and only with its publication on; every site is checked first.
    static void Install(bool traffic, bool emergency) noexcept
    {
        game = GameBase();
        if (!traffic && !emergency) return;
        if (!ready.load(std::memory_order_acquire) || !publicationEnabled) { installStatus = "allocation_unavailable"; return; }
        const char* failed = nullptr;
        if (!Bytes(SubmitRva, {0xFF,0x74,0x24,0x40,0xF3,0x0F,0x10,0x44,0x24,0x38})) failed = "submit";
        else if (!Bytes(VacancyRva, {0x8A,0x15}) || Word(VacancyRva + 2) != game + VacancyFlagRva || !Bytes(VacancyRva + 6, {0x84,0xD2})) failed = "vacancy";
        else if (Word(ModelInfoTableUseRva) != game + ModelInfoTableRva) failed = "model_table";
        if (failed) { installStatus = std::string("guard_failed=") + failed; return; }
        if (traffic && !(Bytes(TrafficSiteRva - 9, {0x68,0x00,0x02,0x00,0x00,0x6A,0x02,0x6A,0x00}) && CallsSubmit(TrafficSiteRva) &&
            Bytes(PoolFreeRva, {0x56,0xFF,0x74,0x24,0x08,0x8B,0xF1,0xE8}) &&
            Bytes(TimerRestoreRva, {0xC6,0x05}) && Word(TimerRestoreRva + 2) == game + TimerBackupActiveRva && Bytes(TimerRestoreRva + 6, {0x00,0xC3}) &&
            TrafficSignal::registry.Configure(TrafficSignal::PrivateKey(), 0)))
        {
            traffic = false; installStatus = "traffic_guard_failed ";
        }
        if (emergency)
            for (const auto site : lab_siren::PoliceSites)
                if (!CallsSubmit(site)) { emergency = false; installStatus += "emergency_guard_failed "; break; }
        if (!traffic && !emergency) return;

        if (emergency)
        {
            vacancyHook = safetyhook::create_mid(game + VacancyRva, Emergency::ApplyVacancy);
            labPointSource = Emergency::NearbyPoint;
            labCompareGrace = Emergency::Compare;
            observeSubmittedLights = Emergency::BeginSelection;
        }
        if (traffic)
        {
            freeHook = safetyhook::create_mid(game + PoolFreeRva, TrafficSignal::Free);
            timerRestoreHook = safetyhook::create_mid(game + TimerRestoreRva, TrafficSignal::NativeTimerRestore);
            labLampGeneration = TrafficSignal::LampGeneration;
            observeShadowResult = OnSelectionResult;
        }
        trafficRequested = traffic; emergencyRequested = emergency;
        submitHook = safetyhook::create_mid(game + SubmitRva, OnSubmit);
        installStatus += std::string("traffic=") + (traffic ? "1" : "0") + " emergency=" + (emergency ? "1" : "0");
    }

    static void Update() noexcept
    {
        TrafficSignal::Poll();
        Emergency::Update();
        log.Write(std::ios::app, [](std::ofstream& out, uint64_t now)
        {
            uint32_t alive = 0, leased = 0;
            for (uint32_t i = 0; i < TrafficSignal::registry.Used(); ++i)
            {
                const auto& e = TrafficSignal::registry.entries[i];
                if (!e.retired) { ++alive; if (e.leased) ++leased; }
            }
            out << "tick=" << now << ' ' << installStatus
                << " traffic_registered=" << TrafficSignal::registry.Used() << " traffic_alive=" << alive << " traffic_leased=" << leased
                << " traffic_admitted=" << TrafficSignal::admitted.load() << " traffic_collisions=" << TrafficSignal::collisions.load()
                << " traffic_stopped=" << TrafficSignal::fatal.load() << " traffic_stop_reason=" << TrafficSignal::fatalReason.load()
                << " emergency_bound=" << Emergency::bound.load() << " emergency_promoted=" << Emergency::promoted.load()
                << " emergency_vacancies=" << Emergency::vacancySuppressed.load() << '\n';
        });
    }
}
