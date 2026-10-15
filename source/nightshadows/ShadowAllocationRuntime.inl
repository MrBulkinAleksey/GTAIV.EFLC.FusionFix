// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

// Included after module imports and CShadows declarations. This experimental
// adapter is opt-in and has not been run in GTA IV. No install occurs from the
// offline build; the game integration activates only on a later chosen launch.
namespace PlayerShadowAllocation
{
    namespace allocation = fusionfix::shadows::ce::allocation;
    namespace budget = fusionfix::shadows::budget;
    using fusionfix::shadows::Vec3;
    using fusionfix::shadows::FloatingPointState;

    static SafetyHookInline selectionHook; static thread_local std::array<uint8_t,4096> labRelevant{};
    // CE 1.8 extension points, set by EmergencyTrafficShadowsRuntime.inl when its features are on.
    static int (*labCompareGrace)(int,int,int,int) noexcept=nullptr;
    static bool (*labPointSource)(const rage::CLightSource&) noexcept=nullptr;
    static uint64_t (*labLampGeneration)(const rage::CLightSource&) noexcept=nullptr;
    static void (*observeShadowResult)() noexcept=nullptr;
    static void (*observeSubmittedLights)(const rage::CLightSource*,uint32_t) noexcept = nullptr;
    // Changes the frame's lights before the game picks the slots (Lighting20: stale lamp keys, cached lamps).
    static void (*prepareSources)(rage::CLightSource*,uint32_t) noexcept = nullptr;
    // At the pre-cache boundary (PreCacheRva), before the stable map ordinals: the emergency vacancy.
    static void (*preCacheVacancy)(SafetyHookContext&) noexcept = nullptr;
    static SafetyHookMid preCacheHook;
    static bool stableOrdinals = false;
    static std::atomic<uint32_t> stableOrdinalPasses{0}, stableOrdinalMoves{0}, stableOrdinalRejected{0}, sharedMapsSkipped{0};
    static SafetyHookMid collectHook, finalizeHook, lampDistanceHook, compareResultHook, cacheResultHook, cacheDistanceHook;
    static bool nativeLampPriority=false;
    static bool visibleReceiverRanking=false;
    static std::atomic<uint32_t> receiverRanked{0}, indoorRanked{0};
    static std::atomic<uint32_t> lampDistanceAdjusted{0};
    // LampCacheViewPriority: the 8 static caches go to the nearest lamps without one, measured from 8 m behind the
    // camera, so lamps behind you held them and the lamps down the street you look at had no shadow at all (no slot,
    // no cache) until you came near. For the cache pick only, a lamp's distance is scaled by 3 / its view weight
    // (1 out of view .. 3 in view): one out of view counts three times as far. Lamps within 10 m keep their distance.
    // The slots' sort gets the distance back unchanged (Collect).
    static bool cacheViewPriority=true;
    static std::atomic<uint32_t> cacheDistanceAdjusted{0};
    static thread_local float cacheSavedDistance=0;
    static thread_local bool cacheDistanceSwapped=false;
    static std::atomic<uint32_t> cacheDependencyChecks{0}, cacheDependencyRedirected{0}, cacheDependencyDeferred{0}, cacheDependencyRejected{0};
    static std::atomic<uint32_t> continuityComparisons{0}, continuityOverrides{0}, continuityRejected{0};
    static std::atomic<bool> ready{false}, unsupportedThread{false};
    static std::atomic<DWORD> ownerThread{0};
    static uintptr_t gameBase = 0;
    static bool cameraPriority = false;
    static bool casterPriority = false;
    static uint32_t casterHoldMs = 0;
    static float casterLeadSeconds = 1.0f; // CasterAwarePriorityLead
    static float boxedCarCasterDistance = 25.0f; // BoxedCarCasterDistance
    static float pedCasterDistance = 35.0f; // PedCasterDistance
    // InteriorLightsYieldOutside: while the scene is not an interior (IS_INTERIOR_SCENE, read once a frame in
    // CaptureCasters), a light of an interior (mInteriorIndex set) takes no dynamic slot at all, a cached one keeps
    // its cache: room lights shining out through a door took slots from the street lamps, by a ped in their light,
    // by lighting you (uncached), or as the slots were free, and threw a second shadow of the cars and lamp posts.
    static bool interiorLightsYield = true;
    static std::atomic<bool> interiorScene{false};
    static std::atomic<uint32_t> interiorLightsYielded{0};
    // Copied on the game-process callback, read by selection on the render side.
    static std::atomic_flag castersLock = ATOMIC_FLAG_INIT;
    static fusionfix::shadows::ShadowCasterPresence casters{};
    static std::atomic<uint32_t> casterCaptures{0}, lampsWithoutCasters{0}, lightsOffScreen{0}, carsLeftToBoxes{0}, pedsLeftOut{0};
    // NightShadowsSlotLog: filled by selection, written out from the game's thread.
    static fusionfix::shadows::ShadowSlotTrace slotTrace;
    // LampSpacing: lamps beside a nearer one in view give way to lamps further along
    static fusionfix::shadows::ShadowLampSpacing lampSpacing;
    static std::atomic<uint32_t> lampsSpacedOut{0};
    static void FlushSlotTrace() noexcept
    {
        // A mark in the trace where something was seen: Ctrl+Shift+F7, or F12, the usual screenshot key.
        if (slotTrace.enabled && CTimer::m_snTimeInMilliseconds && CTimer::m_frameCount) {
            static bool markWasDown = false, shotWasDown = false;
            const bool mark = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                (GetAsyncKeyState(VK_F7) & 0x8000);
            const bool shot = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
            try {
                if (mark && !markWasDown) slotTrace.Mark(static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds), *CTimer::m_frameCount, "key");
                if (shot && !shotWasDown) slotTrace.Mark(static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds), *CTimer::m_frameCount, "screenshot");
            } catch (...) {}
            markWasDown = mark; shotWasDown = shot;
        }
        static ULONGLONG last = 0;
        if (!slotTrace.enabled || GetTickCount64() - last < 1000) return;
        last = GetTickCount64();
        try { slotTrace.Flush(); } catch (...) {}
    }
    static std::atomic<uint32_t> cameraPasses{0}, cameraFallbacks{0};
    static std::atomic<uint32_t> auxiliaryViewsRejected{0}, sceneCameraReads{0};
    static bool publicationEnabled = false; // Immutable after ready is published.
    static std::string installStatus = "not_requested"; // Init-only; diagnostics reads after ready publication.
    // Diagnostics are counters only: no per-frame logging/allocations or I/O.
    static std::atomic<uint32_t> appliedPasses{0}, observedPasses{0}, fallbackPasses{0}, staleKeysCleared{0};

    struct State
    {
        budget::ShadowAllocationPass pass;
        Vec3 player{}, drivingFocus{};
        fusionfix::shadows::ShadowDrivingFocus motionFocus;
        std::array<budget::PlayerShadowBudget::Slot,7> previousSelection{};
        uint32_t previousSelectionFrame=0;
        fusionfix::shadows::NativeLampContinuity41 lampContinuity;
        fusionfix::shadows::NativeShadowContinuity42 continuity;
        std::array<fusionfix::shadows::NativeShadowContinuity42::Candidate,4096> nativeCandidates{};
        bool continuityActive=false;
        bool tracedComparison=false;
        unsigned claimsReset=0; // NativeShadowContinuity42::Begin, for the slot trace
        fusionfix::shadows::ShadowView view{};
        fusionfix::shadows::ShadowVolumeFrustum20 frustum{}; // the view's planes, worked out once a pass
        fusionfix::shadows::ShadowCasterPresence casters{};
        fusionfix::shadows::SlotGainHold gainHold;
        uintptr_t ped = 0, occupiedCar = 0;
        uint32_t frame = 0, viewFrame = 0;
        uintptr_t stackAnchor = 0;
        unsigned depth = 0;
        bool committed = false;
        // SharedMaps: lights of this pass that use another's map, marked with the pass's serial.
        uint32_t passSerial = 0;
        std::array<uint32_t,4096> sharedMap{};
    };
    static thread_local State state;
    // A light of this pass sharing another's map: the game does not insert it among the candidates.
    static void MarkSharedMap(uint32_t index) noexcept { if (index < state.sharedMap.size()) state.sharedMap[index] = state.passSerial; }
    static bool IsSharedMap(uint32_t index) noexcept { return index < state.sharedMap.size() && state.passSerial && state.sharedMap[index] == state.passSerial; }

    struct Invocation
    {
        Invocation() noexcept { ++state.depth; if (state.depth == 1) state.committed = false; }
        ~Invocation() noexcept { state.pass.EndInvocation(); --state.depth; }
        Invocation(const Invocation&) = delete;
        Invocation& operator=(const Invocation&) = delete;
    };
    static std::atomic<uint32_t> lampRemoved{0}, lampMissingInput{0}, lampDroppedPresent{0};
    static std::array<std::atomic<uint32_t>,8> rejectedPassReasons{};
    static std::atomic<uint32_t> rejectedAdapterChecks{0};
    static void TraceRejected(unsigned code) noexcept
    {
        if (slotTrace.enabled && CTimer::m_snTimeInMilliseconds && CTimer::m_frameCount)
            try { slotTrace.Rejected(code, static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds), *CTimer::m_frameCount); }
            catch (...) { slotTrace.enabled = false; }
    }

    static void RejectPass() noexcept
    {
        if (state.pass.Active()) { ++fallbackPasses; ++rejectedAdapterChecks; TraceRejected(0); }
        state.pass.Cancel();
    }

    static rage::CLightSource* CurrentLights() noexcept
    {
        return *reinterpret_cast<rage::CLightSource**>(gameBase + allocation::LightArrayPointerRva);
    }
    static uint32_t CurrentCount() noexcept
    {
        return *reinterpret_cast<uint32_t*>(gameBase + allocation::LightCountRva);
    }
    static bool Enabled() noexcept
    {
        return ready.load(std::memory_order_acquire) && !unsupportedThread.load(std::memory_order_relaxed) &&
            bExtraNightShadows && bHeadlightShadows && bVehicleNightShadows;
    }

    // SlotMinHold: each slot remembers when its light took it. Render thread, like selection.
    struct HoldEntry { uint32_t key{}; uint32_t since{}; };
    static std::array<HoldEntry,7> holdEntries{};
    static uint32_t slotMinHoldMs = 0;
    static float behindLampReach = 12.0f; // BehindLampReach
    static float uncachedLampAdvantage = 1.5f; // UncachedLampAdvantage
    static std::atomic<uint32_t> lampsHidden{0}, sightProbes{0}, sightHits{0}; // LampsBehindWalls, for the status line
    static std::atomic<uint32_t> lightsFlickering{0}; // lights ranked off screen as they flicker out of the list, for the status line
    // Rear lamp keys (Liberty's Shadows 2.0 tail light shadows), set by Lighting20, which comes after
    static bool (*isTailLightKey)(uint32_t) = nullptr;
    static std::atomic<uint32_t> tailLightsYielded{0};

    static void NoteSlotAcquires(const budget::PlayerShadowBudget::Selection& selection) noexcept
    {
        if (!slotMinHoldMs) return;
        const auto now = GetTickCount();
        std::array<HoldEntry,7> next{};
        for (unsigned i = 0; i < 7; ++i) {
            if (!(selection.validMask & (1u << i))) continue;
            const auto key = static_cast<uint32_t>(selection.slots[i].key);
            if (!key) continue;
            const HoldEntry* held = nullptr;
            for (const auto& e : holdEntries) if (e.key == key) { held = &e; break; }
            if (held) { next[i] = *held; continue; }
            next[i] = {key, now};
        }
        holdEntries = next;
    }

    // Whether a light took its slot less than SlotMinHold ago.
    static bool SlotIsFresh(uint32_t key) noexcept
    {
        if (!slotMinHoldMs || !key) return false;
        for (const auto& e : holdEntries)
            if (e.key == key) return GetTickCount() - e.since < slotMinHoldMs;
        return false;
    }

    // Game thread, once a second with the slot trace: whether selection runs at all, and how passes end.
    static void SlotTraceStatus() noexcept
    {
        static ULONGLONG last = 0;
        if (!slotTrace.enabled || GetTickCount64() - last < 1000) return;
        last = GetTickCount64();
        static uint32_t applied = 0, observed = 0, fallback = 0, adapter = 0;
        static std::array<uint32_t, 8> reasons{};
        const uint32_t a = appliedPasses.load(), o = observedPasses.load(), f = fallbackPasses.load(), r = rejectedAdapterChecks.load();
        std::string why;
        for (unsigned i = 0; i < 8; ++i) {
            const uint32_t now = rejectedPassReasons[i].load();
            if (now != reasons[i]) why += std::format(" {}:{}", i, now - reasons[i]);
            reasons[i] = now;
        }
        try {
            slotTrace.Status(std::format("ready={} install={} thread_ok={} night_shadows={} headlight_shadows={} vehicle_night_shadows={} "
                "publication={} lamp_priority={} in the last second: applied={} observed={} fallback={} adapter_rejects={} reasons:{} "
                "lamps_behind_walls={} sight_probes={} hits={} flickering={} tail_lights_yielded={}",
                ready.load() ? 1 : 0, ready.load() ? std::string("enabled") : "[" + installStatus + "]", unsupportedThread.load() ? 0 : 1, bExtraNightShadows ? 1 : 0, bHeadlightShadows ? 1 : 0,
                bVehicleNightShadows ? 1 : 0, publicationEnabled ? 1 : 0, nativeLampPriority ? 1 : 0,
                a - applied, o - observed, f - fallback, r - adapter, why.empty() ? " -" : why,
                lampsHidden.exchange(0), sightProbes.exchange(0), sightHits.exchange(0), lightsFlickering.exchange(0), tailLightsYielded.exchange(0)));
        } catch (...) {}
        applied = a; observed = o; fallback = f; adapter = r;
    }
    static std::atomic<uint32_t> captureCalls{0}, captureValid{0}, perspectiveCalls{0};
    static std::atomic<int> captureWidth{0}, captureHeight{0}, activeWidth{0}, activeHeight{0};
    static SafetyHookMid cameraCaptureHook;
    static std::atomic_flag gameplayViewLock = ATOMIC_FLAG_INIT;
    static fusionfix::shadows::ShadowView gameplayView{};
    static uint32_t gameplayViewFrame = 0;
    static const rage::grcViewport* SceneViewport() noexcept
    {
        // The guarded native selection reads this exact table before touching
        // the same scene object. Never dereference a light/reflection viewport
        // simply because its resolution happens to match the back buffer.
        if(!ready.load(std::memory_order_acquire) || !gameBase) return nullptr;
        const auto* table=reinterpret_cast<const uint32_t*>(gameBase+allocation::SceneCameraTableRva);
        const auto index=table[0];
        if(index<1 || index>3) return nullptr;
        const auto camera=static_cast<uintptr_t>(table[index]);
        if(camera<0x10000 || camera>UINTPTR_MAX-allocation::SceneViewportOffset-sizeof(rage::grcViewport)) return nullptr;
        return reinterpret_cast<const rage::grcViewport*>(camera+allocation::SceneViewportOffset);
    }
    static_assert(offsetof(rage::grcViewport,mViewMatrix)==0x180);
    static_assert(offsetof(rage::grcViewport,mProjectionMatrix)==0x1C0);
    static_assert(offsetof(rage::grcViewport,mWidth)==0x2B0);
    static fusionfix::shadows::ShadowView ReadView(const rage::grcViewport* viewport) noexcept
    {
        fusionfix::shadows::ShadowView view{};
        if (!viewport || !rage::grcDevice::ms_nActiveWidth || !rage::grcDevice::ms_nActiveHeight ||
            !fusionfix::shadows::IsGameplayViewport(viewport->mIsPerspective,
                viewport->mWidth,viewport->mHeight,*rage::grcDevice::ms_nActiveWidth,
                *rage::grcDevice::ms_nActiveHeight)) return view;
        std::memcpy(view.view, viewport->mViewMatrix, sizeof(view.view));
        std::memcpy(view.projection, viewport->mProjectionMatrix, sizeof(view.projection));
        view.valid = true;
        for (const auto& row : view.view) for (float f : row) if (!std::isfinite(f)) view.valid = false;
        for (const auto& row : view.projection) for (float f : row) if (!std::isfinite(f)) view.valid = false;
        if (std::abs(view.projection[0][0]) < 0.001f || std::abs(view.projection[1][1]) < 0.001f)
            view.valid = false;
        return view;
    }
    static fusionfix::shadows::ShadowView ReadGameplayView() noexcept
    {
        auto view=ReadView(SceneViewport());
        if(view.valid)++sceneCameraReads;
        return view;
    }
    static bool InstallCameraCapture() noexcept
    {
        const auto base = GameBase();
        // CE setter verified in the running CE executable: mov [viewport],ecx.
        // Guard both opcode and relocated destination before installing.
        const auto site = reinterpret_cast<const uint8_t*>(base + 0x30C6F);
        constexpr uint8_t prefix[]{0x89,0x0D};
        if (!rage::pCurrentViewport || std::memcmp(site,prefix,sizeof(prefix)) ||
            *reinterpret_cast<const uint32_t*>(site+2) != reinterpret_cast<uintptr_t>(rage::pCurrentViewport) ||
            site[6]!=0xC6 || site[7]!=0x05) return false;
        cameraCaptureHook = safetyhook::create_mid(base + 0x30C6F, [](SafetyHookContext& regs) {
            const FloatingPointState fp;
            if (!CTimer::m_frameCount) return;
            ++captureCalls;
            const auto* viewport = reinterpret_cast<const rage::grcViewport*>(regs.ecx);
            if(viewport!=SceneViewport()) {++auxiliaryViewsRejected;return;}
            if (viewport && viewport->mIsPerspective) {
                ++perspectiveCalls; captureWidth = viewport->mWidth; captureHeight = viewport->mHeight;
                activeWidth = rage::grcDevice::ms_nActiveWidth ? *rage::grcDevice::ms_nActiveWidth : 0; activeHeight = rage::grcDevice::ms_nActiveHeight ? *rage::grcDevice::ms_nActiveHeight : 0;
            }
            const auto view = ReadView(viewport);
            if (view.valid) ++captureValid;
            if (view.valid && !gameplayViewLock.test_and_set(std::memory_order_acquire)) {
                gameplayView = view; gameplayViewFrame = *CTimer::m_frameCount;
                gameplayViewLock.clear(std::memory_order_release);
            }
        });
        return static_cast<bool>(cameraCaptureHook);
    }
    // LampsBehindWalls: whether a light reaches the frame is told by its volume against the view, which
    // walls do not stop, so in a tunnel the lamps of the bore beside it, as bright and never seen, took
    // slots from those over the cars in front. An uncached lamp in view within its reach is checked by a
    // segment from it to the camera against the map's collision, the way the game's ground probe tests
    // it (CE 0x738880 on [0x12B9C78]); one that hits something counts as off screen. Selection (render
    // thread) only reads the answers and asks for stale ones; the game thread probes a few a frame.
    struct LampSight { Vec3 position{}; uint32_t checked{}, asked{}; bool hidden{}, pending{}, known{}; };
    static std::mutex sightMutex;
    static std::unordered_map<uint32_t, LampSight> lampSight;
    static bool lampsBehindWalls = false;
    static uint32_t sightFlags = 6; // the ground probe's: the map, not vehicles
    static uintptr_t probeFunction = 0, probeLevel = 0, probeFar = 0;

    // Lights seen with a frame or more missing in between, by key: when they came back, and until when they count as flickering.
    struct LightPresence { uint32_t lastFrame{}, returned[2]{}, flickeringUntil{}; };
    static std::unordered_map<uint32_t, LightPresence> lightPresence;
    static bool FlickeringLight(uint32_t key, uint32_t frame, uint32_t now) noexcept
    {
        if (!key) return false;
        if (lightPresence.size() > 4096) lightPresence.clear();
        auto& p = lightPresence[key];
        if (p.lastFrame && frame > p.lastFrame + 1 && frame - p.lastFrame < 30) {
            p.returned[1] = p.returned[0]; p.returned[0] = now;
            if (p.returned[1] && now - p.returned[1] < 2000) p.flickeringUntil = now + 3000;
        }
        p.lastFrame = frame;
        return p.flickeringUntil && int32_t(p.flickeringUntil - now) > 0;
    }

    static bool LampBehindWall(uint32_t key, Vec3 position) noexcept
    {
        if (!lampsBehindWalls || !key) return false;
        const auto now = GetTickCount();
        std::lock_guard lock(sightMutex);
        auto& s = lampSight[key];
        const float x = s.position.x - position.x, y = s.position.y - position.y, z = s.position.z - position.z;
        if (!s.asked || x * x + y * y + z * z > 0.25f) s = { position }; // new, or the key moved to another lamp
        s.asked = now;
        if (!s.pending && (!s.known || now - s.checked > 300)) s.pending = true;
        return s.known && s.hidden && now - s.checked < 1500;
    }

    struct alignas(16) SightSegment { float a[4]; float b[4]; };
    struct alignas(16) SightResult { uint8_t bytes[0x60]; };
    using SightProbe = int(__thiscall*)(void* level, const SightSegment*, SightResult*, void* exclude,
        uint32_t includeFlags, uint32_t typeFlags, uint32_t stateFlags, uint32_t maxResults, uint32_t unknown);

    // The physics instance of an entity, as the ground probe takes one: its vtable +0xA0, else +0x38.
    static void* EntityInstance(uintptr_t entity) noexcept
    {
        if (!entity) return nullptr;
        const auto vtable = *reinterpret_cast<const uintptr_t*>(entity);
        auto inst = reinterpret_cast<void*(__thiscall*)(uintptr_t)>(*reinterpret_cast<const uintptr_t*>(vtable + 0xA0))(entity);
        return inst ? inst : *reinterpret_cast<void* const*>(entity + 0x38);
    }

    // Game thread, every frame: up to eight lamps asked about, oldest first.
    static void ProbeLampSight(uintptr_t exclude) noexcept
    {
        if (!lampsBehindWalls || !probeFunction) return;
        float camera[3];
        if (!GameCamera::Position(camera)) return;
        std::array<std::pair<uint32_t, Vec3>, 8> work{};
        unsigned n = 0;
        const auto now = GetTickCount();
        {
            std::lock_guard lock(sightMutex);
            std::erase_if(lampSight, [&](const auto& e) { return now - e.second.asked > 5000; });
            for (auto& [key, s] : lampSight)
                if (s.pending && n < work.size()) work[n++] = { key, s.position };
        }
        const auto level = *reinterpret_cast<void* const*>(probeLevel);
        if (!level) return;
        void* skip = EntityInstance(exclude);
        for (unsigned i = 0; i < n; ++i) {
            const auto& p = work[i].second;
            float d[3] = { camera[0] - p.x, camera[1] - p.y, camera[2] - p.z };
            const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            bool hidden = false;
            if (length > 1.0f) {
                // From half a metre off the lamp, so its own fitting in the ceiling is not the wall
                const float k = 0.5f / length;
                SightSegment segment{ { p.x + d[0] * k, p.y + d[1] * k, p.z + d[2] * k, 0.0f }, { camera[0], camera[1], camera[2], 0.0f } };
                SightResult result{};
                const auto farPoint = reinterpret_cast<const float*>(probeFar);
                for (uint32_t at : { 0x10u, 0x20u, 0x30u }) std::memcpy(result.bytes + at, farPoint, 12);
                *reinterpret_cast<uint32_t*>(result.bytes + 0x4C) = 0xFFFF;
                hidden = reinterpret_cast<SightProbe>(probeFunction)(level, &segment, &result, skip, sightFlags, 0xFFFFFFFF, 7, 1, 0) != 0;
                ++sightProbes;
                if (hidden) ++sightHits;
            }
            std::lock_guard lock(sightMutex);
            if (auto it = lampSight.find(work[i].first); it != lampSight.end()) {
                it->second.hidden = hidden; it->second.known = true; it->second.pending = false; it->second.checked = GetTickCount();
            }
        }
    }

    // At install: the ground probe's call (CE 0xA54570 mov ecx, [level] ... 0xA5458A call 0x738880).
    static void InstallLampSight(bool enabled, uint32_t flags) noexcept
    {
        if (!enabled) return;
        const auto base = GameBase();
        const auto at = reinterpret_cast<const uint8_t*>(base + 0x654570);
        const auto call = reinterpret_cast<const uint8_t*>(base + 0x65458A);
        if (at[0] != 0x8B || at[1] != 0x0D || *reinterpret_cast<const uint32_t*>(at + 2) != base + 0xEB9C78 ||
            call[0] != 0xE8 || base + 0x65458A + 5 + *reinterpret_cast<const int32_t*>(call + 1) != base + 0x338880)
            return;
        probeLevel = base + 0xEB9C78;
        probeFunction = base + 0x338880;
        probeFar = base + 0x174B320;
        sightFlags = flags;
        lampsBehindWalls = true;
    }

    static void CaptureCasters() noexcept
    {
        using fusionfix::shadows::ShadowCasterPresence;
        if (!casterPriority || !Enabled() || !CTimer::m_snTimeInMilliseconds) return;
        if (interiorLightsYield) interiorScene.store(Natives::IsInteriorScene(), std::memory_order_relaxed);
        PlayerCar::Focus focus;
        ShadowCasterPresence next{};
        next.timeMs = static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds);
        const bool focused = PlayerCar::ReadFocus(focus);
        if (focused) {
            // With vehicle box shadows, cars that keep a box from a lamp until they leave it need no slot from it;
            // nor do people sitting in cars, inside the car's own shadow.
            if (VehicleBoxShadowsBridge::CopyBoxedPairs) {
                static std::array<VehicleBoxShadowsBridge::BoxedPair, ShadowCasterPresence::BoxedCapacity> pairs{};
                const int n = VehicleBoxShadowsBridge::CopyBoxedPairs(pairs.data(), int(pairs.size()));
                for (int i = 0; i < n; ++i)
                    next.boxed[next.boxedCount++] = {{pairs[i].position[0], pairs[i].position[1], pairs[i].position[2]}, pairs[i].car};
            }
            static std::vector<uintptr_t> occupants;
            occupants.clear();
            const auto add = [&](uintptr_t entity, float extent, uintptr_t car) {
                float p[3];
                if (!CEntity::GetPosition(entity, p)) return;
                const float x = p[0] - focus.position[0], y = p[1] - focus.position[1], z = p[2] - focus.position[2];
                if (x * x + y * y + z * z <= ShadowCasterPresence::Range * ShadowCasterPresence::Range)
                    next.Add({p[0], p[1], p[2]}, extent, car);
            };
            // BoxedCarCasterDistance: with box shadows, a car further from you than this gives no lamp a claim to a
            // slot, its box shadows it there. A slot trace: lamps 30-60 m off took a slot in one grant of three for
            // the cars under them, and the lamp you stood under gave its slot away and back, its props' shadow
            // blinking. Your own car always counts; held under the boxes' fade-out (20 m before their end).
            float boxedFrom = 0.0f;
            if (boxedCarCasterDistance > 0 && VehicleBoxShadowsBridge::Enabled && VehicleBoxShadowsBridge::Enabled()) {
                const float boxesEnd = VehicleBoxShadowsBridge::MaxDistance ? VehicleBoxShadowsBridge::MaxDistance() : 0.0f;
                boxedFrom = boxesEnd > 0 ? (std::min)(boxedCarCasterDistance, (std::max)(boxesEnd - 20.0f, 0.0f)) : boxedCarCasterDistance;
            }
            uint32_t leftToBoxes = 0;
            next.valid = CVehicle::ForEachVehicle([&](uintptr_t v) {
                float at[3];
                const bool boxed = boxedFrom > 0 && v != focus.car && CEntity::GetPosition(v, at) &&
                    (at[0] - focus.position[0]) * (at[0] - focus.position[0]) + (at[1] - focus.position[1]) * (at[1] - focus.position[1]) +
                    (at[2] - focus.position[2]) * (at[2] - focus.position[2]) > boxedFrom * boxedFrom;
                if (boxed) ++leftToBoxes;
                else add(v, ShadowCasterPresence::VehicleExtent, v);
                // CE: the driver followed by eight passengers
                if (VehicleBoxShadowsBridge::CopyBoxedPairs && PlayerCar::driverOffset)
                    for (int i = 0; i < 9; ++i)
                        if (const auto ped = reinterpret_cast<const uintptr_t*>(v + PlayerCar::driverOffset)[i])
                            occupants.push_back(ped);
            });
            carsLeftToBoxes.store(leftToBoxes, std::memory_order_relaxed);
            // Your car also counts where its speed takes it in CasterAwarePriorityLead: driving up to a cached lamp,
            // the lamp's slot went once the ped in its light had left and came back 0.4 s later as the car came in, a
            // blink of its shadow.
            static uintptr_t lastCar = 0;
            static uint32_t lastMs = 0;
            static float lastAt[3]{};
            float carAt[3];
            if (focus.car && casterLeadSeconds > 0 && CEntity::GetPosition(focus.car, carAt)) {
                const uint32_t dt = next.timeMs - lastMs;
                if (focus.car == lastCar && dt > 0 && dt < 500) {
                    const float k = casterLeadSeconds * 1000.0f / float(dt);
                    const fusionfix::shadows::Vec3 ahead{carAt[0] + (carAt[0] - lastAt[0]) * k,
                        carAt[1] + (carAt[1] - lastAt[1]) * k, carAt[2] + (carAt[2] - lastAt[2]) * k};
                    const float x = ahead.x - carAt[0], y = ahead.y - carAt[1], z = ahead.z - carAt[2];
                    if (std::isfinite(x + y + z) && x * x + y * y + z * z > 1.0f && x * x + y * y + z * z < 60.0f * 60.0f)
                        next.Add(ahead, ShadowCasterPresence::VehicleExtent, focus.car);
                }
                if (focus.car != lastCar || dt >= 50) {
                    lastCar = focus.car; lastMs = next.timeMs; std::copy_n(carAt, 3, lastAt);
                }
            }
            // PedCasterDistance: a ped further from you than this gives no lamp a claim to a slot either (they have
            // no box shadow, so there they cast none). With the far cars left to their boxes, a slot trace had lamps
            // 40-65 m off take slots for a ped 4-8 m from them, the lamp you stood under handing its slot round.
            std::sort(occupants.begin(), occupants.end());
            uint32_t pedsOut = 0;
            next.valid = next.valid && CPed::ForEachPed([&](uintptr_t p) {
                if (std::binary_search(occupants.begin(), occupants.end(), p)) return;
                float at[3];
                if (pedCasterDistance > 0 && p != focus.ped && CEntity::GetPosition(p, at) &&
                    (at[0] - focus.position[0]) * (at[0] - focus.position[0]) + (at[1] - focus.position[1]) * (at[1] - focus.position[1]) +
                    (at[2] - focus.position[2]) * (at[2] - focus.position[2]) > pedCasterDistance * pedCasterDistance) {
                    ++pedsOut;
                    return;
                }
                add(p, ShadowCasterPresence::PedExtent, 0);
            });
            pedsLeftOut.store(pedsOut, std::memory_order_relaxed);
        }
        if (!castersLock.test_and_set(std::memory_order_acquire)) {
            casters = next; ++casterCaptures; castersLock.clear(std::memory_order_release);
        }
        if (focused) try { ProbeLampSight(focus.car); } catch (...) {}
    }

    static bool Prepare() noexcept
    {
        if (!Enabled() || !CTimer::m_frameCount || !CTimer::m_snTimeInMilliseconds ||
            !CPlayer::getLocalPlayerPed || !CPlayer::findPlayerCar)
            return false;
        PlayerCar::Focus focus;
        if (!PlayerCar::ReadFocus(focus)) return false;
        const auto ped = focus.ped;
        if (ped != state.ped) {
            state.view = {};
            state.previousSelection = {}; state.previousSelectionFrame = 0;
            state.gainHold.Reset();
        }
        state.gainHold.holdMs = casterHoldMs;
        state.ped = ped;
        state.player = {focus.position[0], focus.position[1], focus.position[2]};
        state.occupiedCar = focus.car;
        state.drivingFocus=state.motionFocus.Update(state.player,state.occupiedCar,static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds));
        state.frame = *CTimer::m_frameCount;
        state.lampContinuity.Begin(ped,state.frame,static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds));
        state.claimsReset=state.continuity.Begin(ped,state.frame,static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds));
        state.nativeCandidates.fill({}); labRelevant.fill(0);
        if (casterPriority && !castersLock.test_and_set(std::memory_order_acquire)) {
            state.casters = casters; castersLock.clear(std::memory_order_release);
        }
        // A contended copy keeps the last one; a stale one, from a pause or a
        // load, makes every lamp count as lighting someone.
        if (static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds) - state.casters.timeMs > 250u)
            state.casters.valid = false;
        state.continuityActive=false;
        state.tracedComparison=false;
        state.stackAnchor = 0;
        state.view = {};
        if (cameraPriority && rage::pCurrentViewport) {
            state.view = ReadGameplayView();
            // Shadow passes replace the current viewport. Keep only a recent,
            // full-resolution perspective view. Selection can consume a view on a different
            // thread; publish a consistent snapshot without waiting in either hook.
            if (!state.view.valid && !gameplayViewLock.test_and_set(std::memory_order_acquire)) {
                if (gameplayView.valid && state.frame - gameplayViewFrame <= 1) state.view = gameplayView;
                gameplayViewLock.clear(std::memory_order_release);
            }
            if (state.view.valid) ++cameraPasses; else ++cameraFallbacks;
        }
        if (casterPriority) state.casters.Look(state.view);
        state.frustum = fusionfix::shadows::PrepareShadowVolumeFrustum20(state.view);
        if (reinterpret_cast<uintptr_t>(CurrentLights()) < 0x10000) return false;
        state.pass.Begin({state.frame, static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds),
                          ped, state.occupiedCar != 0}, CurrentLights(), CurrentCount());
        state.continuityActive=publicationEnabled && nativeLampPriority && state.pass.Active();
        lampSpacing.Update(state.motionFocus.Speed(),state.occupiedCar!=0,static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds));
        if(slotTrace.enabled) slotTrace.spacing=lampSpacing.spacing;
        if(lampSpacing.spacing>0 && state.continuityActive) {
            try {
                const auto* lights=CurrentLights();
                const auto count=CurrentCount();
                lampSpacing.Begin(count);
                for(uint32_t i=0;i<count && i<4096;++i) {
                    const auto& light=lights[i];
                    if(!(light.mFlags&rage::LF_DYNAMIC_SHADOW) || (light.mFlags&rage::LF_VEHICLE)) continue;
                    const Vec3 position{light.mPosition.x,light.mPosition.y,light.mPosition.z};
                    const float x=position.x-state.player.x,y=position.y-state.player.y,z=position.z-state.player.z;
                    const float distanceSquared=x*x+y*y+z*z;
                    if(!std::isfinite(distanceSquared) || distanceSquared>90.0f*90.0f ||
                       !fusionfix::shadows::ShadowVolumeMayReachView(state.frustum,position,light.mRadius)) continue;
                    lampSpacing.Add({i,position,{light.mDirection.x,light.mDirection.y,light.mDirection.z},
                        static_cast<int>(light.mType)==2,distanceSquared,static_cast<uint32_t>(light.mCastShadows)});
                }
                lampSpacing.Direction(state.occupiedCar ? state.motionFocus.Velocity() : fusionfix::shadows::Vec3{});
                lampSpacing.Resolve();
            } catch(...) { lampSpacing.spacing=lampSpacing.base=lampSpacing.seconds=0; }
        }
        if(slotTrace.enabled && state.continuityActive)
            try { slotTrace.Begin(state.frame,static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds),state.player,
                state.occupiedCar!=0,state.view,CurrentCount());
                if(state.claimsReset) slotTrace.ClaimsReset(state.claimsReset,static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds),state.frame); }
            catch(...) { slotTrace.enabled=false; }
        if(state.continuityActive && ShadowTrace34::enabled.load(std::memory_order_relaxed)) {
            const auto tick=GetTickCount();
            for(int row=0;row<4;++row) {
                const auto* v=state.view.view[row];const auto* p=state.view.projection[row];
                ShadowTrace34::Emit({8,state.frame,tick,static_cast<uint32_t>(row+1),row,0,0,0,v[0],v[1],v[2],v[3]});
                ShadowTrace34::Emit({9,state.frame,tick,static_cast<uint32_t>(row+1),row,0,0,0,p[0],p[1],p[2],p[3]});
            }
            ShadowTrace34::Emit({10,state.frame,tick,static_cast<uint32_t>(state.ped),state.occupiedCar?1:0,0,0,0,
                state.player.x,state.player.y,state.player.z,0});
        }
        return state.pass.Active();
    }

    static bool InfluencesPlayer(const rage::CLightSource& light) noexcept
    {
        const Vec3 position{light.mPosition.x,light.mPosition.y,light.mPosition.z};
        const Vec3 direction{light.mDirection.x,light.mDirection.y,light.mDirection.z};
        if ((light.mFlags & rage::LF_VEHICLE) && !(labPointSource && labPointSource(light))) {
            if (fusionfix::shadows::BeamTouchesReceiver(state.player, state.occupiedCar ? 3.0f : 1.5f,
                position, direction, light.mOuterConeAngle, light.mRadius)) return true;
            return !state.occupiedCar && NearbyVehicleLighting36::Relevant(state.ped,state.frame,
                static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds),state.player,position,direction,
                light.mOuterConeAngle,light.mRadius,static_cast<uint32_t>(light.mCastShadows));
        }
        // Nearby lamps retain their protected share; scenery uses the reach slider.
        if (fusionfix::shadows::EvaluateGeometry(state.player,position).distanceSquared > 15.24f*15.24f) return false;
        return fusionfix::shadows::LightVolumeContains(state.player,position,direction,
            light.mType,light.mRadius,light.mOuterConeAngle);
    }

    static uint64_t LampGeometry(const rage::CLightSource& light) noexcept
    {
        if(labLampGeneration){const auto generation=labLampGeneration(light);if(generation)return generation;}
        // A stationary lamp key reused at a new position must not inherit its
        // old slot's hold interval. Vehicle keys lack a verified generation
        // field; the adapter does not claim to detect every pool-pointer reuse.
        uint32_t words[6];
        std::memcpy(words, &light.mPosition, 12);
        words[3] = static_cast<uint32_t>(light.mType);
        words[4] = static_cast<uint32_t>(light.mInteriorIndex);
        words[5] = static_cast<uint32_t>(light.mRoomIndex);
        uint64_t hash = 14695981039346656037ull;
        for (auto word : words) { hash ^= word; hash *= 1099511628211ull; }
        return lab_siren::Generation(labPointSource && labPointSource(light),hash);
    }

    // Audited boundary BEFORE distance is consumed by either cache refresh or
    // native dynamic sorting. Never substitute final lamp indices after cache work.
    static void AdjustLampDistance(SafetyHookContext& regs) noexcept {
        const FloatingPointState fp;
        if(!nativeLampPriority || !publicationEnabled || !Enabled() || state.depth!=1 ||
            !state.pass.Active() || !state.view.valid) return;
        const auto* lights=CurrentLights(); const auto count=CurrentCount();
        const auto address=reinterpret_cast<uintptr_t>(lights);
        const auto index=*reinterpret_cast<const uint32_t*>(regs.esp+0x14);
        if(address<0x10000 || count>4096 || index>=count || regs.edi!=address ||
            regs.esi!=index*sizeof(rage::CLightSource) || address>UINTPTR_MAX-count*sizeof(rage::CLightSource)) return;
        const auto& light=lights[index];
        if(!(light.mFlags&(rage::LF_STATIC_SHADOW|rage::LF_DYNAMIC_SHADOW)) || (light.mFlags&rage::LF_VEHICLE) ||
            *reinterpret_cast<const uint32_t*>(regs.esp+0x1C)!=light.mFlags) return;
        const Vec3 position{light.mPosition.x,light.mPosition.y,light.mPosition.z};
        const Vec3 direction{light.mDirection.x,light.mDirection.y,light.mDirection.z};
        float visibleWeight=fusionfix::shadows::ShadowViewWeight(state.view,position,direction,light.mRadius,false);
        float viewWeight=1.0f+(visibleWeight-1.0f)*(state.occupiedCar?1.0f:0.5f);
        const int feet=fusionfix::shadows::ShadowReachFeet(ShadowReachStep(false));
        const float reach=feet>0?feet*0.3048f:60.0f;
        const float original=regs.xmm0.f32[0];
        const float distanceSquared=fusionfix::shadows::EvaluateGeometry(state.player,position).distanceSquared;
        // VisibleReceiverRanking (Liberty's Shadows 2.0.1): a lamp whose light reaches the view counts as
        // relevant past the reach too, ranks up to 1.15 times nearer, and its view preference leans to the
        // middle of the frame.
        float receiverBenefit=0.0f;
        if(visibleReceiverRanking &&
           fusionfix::shadows::VisibleReceiverEmission133(light.mIntensity,{light.mColor.x,light.mColor.y,light.mColor.z}))
            receiverBenefit=fusionfix::shadows::VisibleReceiverBenefit133(state.view.valid,
                fusionfix::shadows::ShadowVolumeMayReachView(state.frustum,position,light.mRadius),
                distanceSquared,light.mRadius,reach,visibleWeight,static_cast<uint32_t>(light.mType),light.mOuterConeAngle);
        if(receiverBenefit>0) {
            const float forward=fusionfix::shadows::SceneryReceiverFactor163(state.view,position,direction,
                light.mRadius,static_cast<uint32_t>(light.mType));
            visibleWeight=1.0f+(visibleWeight-1.0f)*forward;
            viewWeight=1.0f+(visibleWeight-1.0f)*(state.occupiedCar?1.0f:0.5f);
        }
        const bool retained=state.lampContinuity.Retained(static_cast<uint32_t>(light.mCastShadows),
            LampGeometry(light),visibleWeight>1.1f && std::isfinite(distanceSquared) &&
            (distanceSquared<reach*reach || receiverBenefit>0));
        // Keep the softer on-foot acquisition preference, but do not halve an
        // incumbent's visibility merely because the player is walking.
        const float effectiveWeight=retained ? visibleWeight : viewWeight;
        float adjusted=fusionfix::shadows::NativeLampPriorityDistance(original,
            distanceSquared,effectiveWeight,reach,retained);
        if(receiverBenefit>0) { adjusted=fusionfix::shadows::VisibleReceiverNativeDistance133(adjusted,receiverBenefit); ++receiverRanked; }
        // IndoorShadowBubble (Liberty's Shadows 2.0.4): on foot, a lamp of the room around you within
        // 35 ft ranks by how much it shows: its light in view, its size on screen, its brightness.
        if(LampShadowSizeGate::indoorBubble && !state.occupiedCar && light.mInteriorIndex>0 && light.mRoomIndex>0 &&
           (light.mType==rage::LT_POINT || light.mType==rage::LT_SPOT)) {
            const float bubble=fusionfix::shadows::IndoorBubbleStrength212(state.player,position);
            if(bubble>0) {
                const float consequence=fusionfix::shadows::IndoorBubbleConsequence212(state.view,position,direction,
                    light.mRadius,light.mIntensity,{light.mColor.x,light.mColor.y,light.mColor.z},light.mType==rage::LT_SPOT);
                if(consequence>=0) { adjusted=fusionfix::shadows::IndoorBubbleNativeDistance212(adjusted,consequence,bubble); ++indoorRanked; }
            }
        }
        // Driving, rank a visible lamp by how close the car is about to be, so
        // the lamps over the traffic ahead take their slots before it is near.
        if(casterPriority && state.occupiedCar && std::isfinite(distanceSquared) && distanceSquared>1.0f) {
            const float anticipated=fusionfix::shadows::DrivingLampPriorityDistance(state.player,state.drivingFocus,
                position,distanceSquared,visibleWeight);
            if(std::isfinite(anticipated) && anticipated>=0 && anticipated<distanceSquared)
                adjusted*=std::sqrt(anticipated/distanceSquared);
        }
        if(adjusted!=original && std::isfinite(adjusted) && adjusted>=0) {
            regs.xmm0.f32[0]=adjusted; ++lampDistanceAdjusted;
        }
    }

    static void AdjustCacheDistance(SafetyHookContext& regs) noexcept {
        cacheDistanceSwapped=false;
        const FloatingPointState fp;
        if(!cacheViewPriority || !publicationEnabled || !Enabled() || state.depth!=1 || !state.pass.Active() || !state.view.valid) return;
        const auto* lights=CurrentLights(); const auto count=CurrentCount();
        const auto address=reinterpret_cast<uintptr_t>(lights);
        const auto index=*reinterpret_cast<const uint32_t*>(regs.esp+0x14);
        if(address<0x10000 || count>4096 || index>=count || regs.edi!=address ||
            regs.esi!=index*sizeof(rage::CLightSource)) return;
        const auto& light=lights[index];
        if(!(light.mFlags&rage::LF_STATIC_SHADOW) || (light.mFlags&rage::LF_VEHICLE)) return;
        const Vec3 position{light.mPosition.x,light.mPosition.y,light.mPosition.z};
        const float distanceSquared=fusionfix::shadows::EvaluateGeometry(state.player,position).distanceSquared;
        if(!std::isfinite(distanceSquared) || distanceSquared<10.0f*10.0f) return;
        const float weight=fusionfix::shadows::ShadowViewWeight(state.view,position,
            {light.mDirection.x,light.mDirection.y,light.mDirection.z},light.mRadius,false);
        const float original=regs.xmm0.f32[0];
        if(!std::isfinite(weight) || weight<1 || weight>=3 || !std::isfinite(original) || original<=0) return;
        cacheSavedDistance=original;
        cacheDistanceSwapped=true;
        regs.xmm0.f32[0]=original*3.0f/weight;
        ++cacheDistanceAdjusted;
    }

    // CasterMinLightHeight: whether a light of the list other than skip, with a shadow, at least that high above the
    // caster at p, reaches it (see ShadowCasterPresence::MinLightAbove).
    static bool CoveredByHigherLight(const rage::CLightSource* lights, uint32_t count, uint32_t skip, Vec3 p, float extent) noexcept
    {
        for (uint32_t j = 0; j < count; ++j)
        {
            const auto& o = lights[j];
            if (j == skip || !(o.mFlags & (rage::LF_STATIC_SHADOW | rage::LF_DYNAMIC_SHADOW)) || (o.mFlags & rage::LF_VEHICLE) ||
                o.mPosition.z - p.z < fusionfix::shadows::ShadowCasterPresence::MinLightAbove)
                continue;
            const Vec3 source{o.mPosition.x, o.mPosition.y, o.mPosition.z};
            if (o.mType == rage::LT_SPOT
                ? fusionfix::shadows::BeamTouchesReceiver(p, extent, source, {o.mDirection.x, o.mDirection.y, o.mDirection.z}, o.mOuterConeAngle, o.mRadius)
                : (p.x - source.x) * (p.x - source.x) + (p.y - source.y) * (p.y - source.y) + (p.z - source.z) * (p.z - source.z) <=
                    (o.mRadius + extent) * (o.mRadius + extent))
                return true;
        }
        return false;
    }

    static void Collect(SafetyHookContext& regs) noexcept
    {
        if (cacheDistanceSwapped)
        {
            regs.xmm0.f32[0] = cacheSavedDistance;
            cacheDistanceSwapped = false;
        }
        const FloatingPointState fp;
        // A light drawn with another's map (the same projection, its key changed to that light's) is not
        // inserted: it would take a slot to draw the same map again.
        if (state.depth == 1 && regs.edx < state.sharedMap.size() && state.sharedMap[regs.edx] == state.passSerial &&
            state.passSerial && regs.edi == reinterpret_cast<uintptr_t>(CurrentLights()) && regs.esi == regs.edx * sizeof(rage::CLightSource))
        {
            regs.eip = gameBase + allocation::CollectSkipRva;
            ++sharedMapsSkipped;
            return;
        }
        if (!Enabled() || state.depth != 1 || !state.pass.Active()) return;
        if (!state.stackAnchor) state.stackAnchor = regs.esp;
        if (state.stackAnchor != regs.esp) { RejectPass(); return; }
        const auto index = static_cast<uint32_t>(regs.edx);
        auto* lights = CurrentLights();
        const auto count = CurrentCount();
        const auto address = reinterpret_cast<uintptr_t>(lights);
        if (address < 0x10000 || count > 4096 ||
            address > UINTPTR_MAX - static_cast<uintptr_t>(count) * sizeof(rage::CLightSource) ||
            index >= count || regs.edi != address ||
            regs.esi != index * sizeof(rage::CLightSource) ||
            *reinterpret_cast<const uint32_t*>(regs.esp + 0x14) != index ||
            *reinterpret_cast<const uint32_t*>(regs.esp + 0x18) != regs.esi)
        {
            RejectPass();
            return;
        }
        const auto& light = lights[index];
        const auto flags = *reinterpret_cast<const uint32_t*>(regs.esp + 0x1C);
        if (flags != light.mFlags || !(flags & rage::LF_DYNAMIC_SHADOW) ||
            ((flags & rage::LF_STATIC_SHADOW) && light.mShadowCacheIndex < 0))
        {
            RejectPass();
            return;
        }
        if (interiorLightsYield && light.mInteriorIndex >= 0 && !(flags & rage::LF_VEHICLE) &&
            !interiorScene.load(std::memory_order_relaxed))
        {
            regs.eip = gameBase + allocation::CollectSkipRva;
            ++interiorLightsYielded;
            return;
        }
        const auto key = static_cast<uint32_t>(light.mCastShadows); // Audited opaque +60 key.
        auto geometry = fusionfix::shadows::EvaluateGeometry(state.player,
            {light.mPosition.x, light.mPosition.y, light.mPosition.z});
        auto kind = budget::Kind::Lamp;
        const bool vehicleBeam = (flags & rage::LF_VEHICLE) && !(labPointSource && labPointSource(light));
        if (vehicleBeam)
            kind = fusionfix::shadows::ce::IsVehicleBeam(key, state.occupiedCar ? state.occupiedCar : PlayerCar::Last())
                ? budget::Kind::PlayerBeam : budget::Kind::OtherBeam;
        if (kind == budget::Kind::PlayerBeam) BeamTrace::Mark(BeamTrace::Candidate);
        if (vehicleBeam)
            geometry.distanceSquared = fusionfix::shadows::ReceiverDistanceSquared(state.player,
                {light.mPosition.x,light.mPosition.y,light.mPosition.z},state.occupiedCar ? 3.0f : 1.5f);
        const int feet = fusionfix::shadows::ShadowReachFeet(ShadowReachStep(kind != budget::Kind::Lamp));
        const float configuredReach = feet > 0 ? feet * 0.3048f : (kind == budget::Kind::Lamp ? 60.0f : 35.0f);
        const float reach = kind == budget::Kind::OtherBeam && std::isfinite(light.mRadius) && light.mRadius > 0
            ? (std::max)(configuredReach,light.mRadius) : configuredReach;
        const auto viewWeight=fusionfix::shadows::ShadowViewWeight(state.view,
            {light.mPosition.x,light.mPosition.y,light.mPosition.z},
            {light.mDirection.x,light.mDirection.y,light.mDirection.z},light.mRadius,vehicleBeam);
        const float priorityDistance=state.occupiedCar && kind==budget::Kind::Lamp && viewWeight>1.0f
            ? fusionfix::shadows::DrivingLampPriorityDistance(state.player,state.drivingFocus,
                {light.mPosition.x,light.mPosition.y,light.mPosition.z},geometry.distanceSquared,viewWeight)
            : -1.0f;
        state.pass.Observe({key, index, kind, geometry.distanceSquared,
                           InfluencesPlayer(light), true,
                           kind == budget::Kind::Lamp ? LampGeometry(light) : 0,
                           viewWeight, reach * reach, priorityDistance}, &light);
        if(state.continuityActive && state.pass.Active()) {
            // Eligibility/cache checks above are native and current-frame.
            // Protect the whole visible influence volume, not screen center.
            const bool volumeVisible=fusionfix::shadows::ShadowVolumeMayReachView(state.frustum,
                {light.mPosition.x,light.mPosition.y,light.mPosition.z},light.mRadius);
            // A light holding a slot keeps its claim a quarter further out than a newcomer may take one,
            // so lights right at the reach do not drop it and win it back by turns.
            bool holder=false;
            for(const auto& old:state.previousSelection) if(old.key==key) { holder=true; break; }
            const float claimReach=holder ? reach*1.25f : reach;
            // Beyond the reach a light used to hold no claim at all, so the far lamps that got slots when nothing
            // nearer wanted them handed them back and forth by distance; now its claim stands against others beyond
            // it, and any light within the reach goes ahead of it (Candidate::inReach).
            const bool inReach=std::isfinite(geometry.distanceSquared) && geometry.distanceSquared<=claimReach*claimReach;
            const bool relevant=std::isfinite(geometry.distanceSquared) && (volumeVisible || kind==budget::Kind::PlayerBeam);
            // A cached lamp with nobody in its light looks the same from its cache;
            // beams and uncached lamps shadow the world wherever they shine.
            using fusionfix::shadows::SlotGain;
            const bool cached=(flags&rage::LF_STATIC_SHADOW) && light.mShadowCacheIndex>=0;
            auto gain=SlotGain::InView;
            int caster=-1;
            if(casterPriority && kind!=budget::Kind::PlayerBeam)
                gain=kind==budget::Kind::Lamp && cached
                    ? state.casters.Lights({light.mPosition.x,light.mPosition.y,light.mPosition.z},
                        {light.mDirection.x,light.mDirection.y,light.mDirection.z},
                        static_cast<int>(light.mType),light.mRadius,light.mOuterConeAngle,&caster,
                        [&](unsigned c) { return CoveredByHigherLight(lights,count,index,state.casters.points[c],state.casters.extents[c]); })
                    : volumeVisible ? SlotGain::InView : SlotGain::OffScreen;
            // An uncached lamp whose light reaches you or your car throws its shadow round you, in view whichever
            // way the camera looks. Ranked by where it stood from the camera, the lamp next to the car in a tunnel
            // took and lost its slot as the camera turned or the car went back and forth, and the car's shadow
            // jumped between the lamp's map and its box.
            const bool lightsPlayer=casterPriority && kind==budget::Kind::Lamp && !cached &&
                std::isfinite(geometry.distanceSquared) && geometry.distanceSquared<=light.mRadius*light.mRadius;
            if(lightsPlayer) gain=SlotGain::InView;
            // A cached lamp you stand under, in view or not and up to 2 m outside its cone: a cobra head's light 28
            // degrees off its axis took you in and lost you as you moved, and its slot came and went every second or so
            // while you stood still, the post's and your shadow with it. Not a lamp lower above you than
            // CasterMinLightHeight (a door's), which a street lamp above you covers.
            if(casterPriority && kind==budget::Kind::Lamp && cached && gain!=SlotGain::InView &&
               std::isfinite(geometry.distanceSquared) && geometry.distanceSquared<=light.mRadius*light.mRadius &&
               light.mPosition.z-state.player.z>=fusionfix::shadows::ShadowCasterPresence::MinLightAbove) {
                const Vec3 source{light.mPosition.x,light.mPosition.y,light.mPosition.z};
                if(light.mType!=rage::LT_SPOT || fusionfix::shadows::BeamTouchesReceiver(state.player,2.0f,source,
                    {light.mDirection.x,light.mDirection.y,light.mDirection.z},light.mOuterConeAngle,light.mRadius))
                    gain=SlotGain::InView;
            }
            // An uncached lamp out of the frame (behind or beside the camera, view weight 1) whose light still
            // reaches it counted as a shadow in view, and in tunnels lamps 15-20 m behind took slots from those
            // ahead. Close behind it throws your car's shadow forward into view, so it keeps that within
            // BehindLampReach; further away it ranks as off screen.
            if(casterPriority && kind==budget::Kind::Lamp && !cached && !lightsPlayer && gain==SlotGain::InView && behindLampReach>0 &&
               state.view.valid && viewWeight<=1.01f && geometry.distanceSquared>behindLampReach*behindLampReach)
                gain=SlotGain::OffScreen;
            if(casterPriority && kind==budget::Kind::Lamp && !cached && gain==SlotGain::InView &&
               geometry.distanceSquared<=reach*reach &&
               LampBehindWall(key,{light.mPosition.x,light.mPosition.y,light.mPosition.z})) {
                gain=SlotGain::OffScreen; ++lampsHidden;
            }
            const auto rawGain=gain;
            if(casterPriority) gain=state.gainHold.Apply(key,kind==budget::Kind::Lamp?LampGeometry(light):0,gain,
                static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds));
            const bool spacedOut=lampSpacing.spacing>0 && kind==budget::Kind::Lamp && lampSpacing.SpacedOut(index);
            // Spacing thins rows of alike lamps (tunnels); a cached lamp whose worth comes from a car or ped in its
            // light is not one of them: spaced out, it lost its slot to a lamp 60 m off for a frame (a slot trace).
            if(spacedOut && gain==SlotGain::InView && !(cached && caster>=0)) { gain=SlotGain::OffScreen; ++lampsSpacedOut; }
            // A light the game leaves out of its list every other frame (flickering fittings, some in tunnels) gave
            // its slot to another light each time it went and took it back as it came, both shadows blinking. Back
            // twice within 2 s, it ranks off screen for 3 s: it takes no slot from lights that are there.
            if(kind!=budget::Kind::PlayerBeam && FlickeringLight(key,state.frame,static_cast<uint32_t>(*CTimer::m_snTimeInMilliseconds)) &&
               gain==SlotGain::InView) { gain=SlotGain::OffScreen; ++lightsFlickering; }
            // A car's rear lamp ranks as off screen: it takes a slot only no lamp or headlight in view wants. A slot trace
            // while driving: rear lamps within 25 m took 123 slots to the street lamps' 137, the lamps 1.4 of 7 on average
            // with 14 lights waiting
            if(isTailLightKey && gain==SlotGain::InView && isTailLightKey(key)) { gain=SlotGain::OffScreen; ++tailLightsYielded; }
            if(slotTrace.enabled)
                try {
                    fusionfix::shadows::ShadowSlotTrace::Light traced{key,static_cast<uint8_t>(kind),rawGain,gain,cached,volumeVisible,
                        {light.mPosition.x,light.mPosition.y,light.mPosition.z},geometry.distanceSquared,viewWeight,priorityDistance,spacedOut};
                    traced.reachSquared=reach*reach;
                    if(caster>=0 && static_cast<unsigned>(caster)<state.casters.count) {
                        const auto& p=state.casters.points[caster];
                        traced.caster=state.casters.extents[caster]>=fusionfix::shadows::ShadowCasterPresence::VehicleExtent?1:2;
                        traced.casterInView=state.casters.inView[caster];
                        traced.casterDistance=std::sqrt((p.x-light.mPosition.x)*(p.x-light.mPosition.x)+
                            (p.y-light.mPosition.y)*(p.y-light.mPosition.y)+(p.z-light.mPosition.z)*(p.z-light.mPosition.z));
                    }
                    slotTrace.Observe(index,traced); }
                catch(...) { slotTrace.enabled=false; }
            // UncachedLampAdvantage: an uncached lamp in view casts no shadow at all without a slot, a cached one
            // still its cache's of the static world, so the one's whole shadow went behind the other's of a ped. A
            // newcomer without a car or ped in its light could take no held slot short of the reach (ClaimClass), and
            // the lamp you drove up to, uncached, waited 7 s behind cached lamps 30-50 m off and lit with no shadow
            // until 17 m. It now claims as a caster's lamp does, ranked as if this many times nearer.
            const bool wholeShadow=casterPriority && uncachedLampAdvantage>0 && kind==budget::Kind::Lamp && !cached &&
                rawGain==SlotGain::InView;
            float rankedSquared=priorityDistance>=0?priorityDistance:geometry.distanceSquared;
            if(wholeShadow && rankedSquared>=0) rankedSquared/=uncachedLampAdvantage*uncachedLampAdvantage;
            if(gain==SlotGain::None) ++lampsWithoutCasters;
            else if(gain==SlotGain::OffScreen) ++lightsOffScreen;
            labRelevant[index]=relevant?1:0; state.nativeCandidates[index]=state.continuity.Observe(
                {key,kind==budget::Kind::Lamp?LampGeometry(light):0},flags,relevant,kind==budget::Kind::PlayerBeam,gain,
                rankedSquared,(caster>=0 && rawGain!=SlotGain::None) || wholeShadow,
                kind!=budget::Kind::PlayerBeam && SlotIsFresh(key),inReach);
            // Record why a prior choice loses its claim, independently of
            // whether native sorting eventually drops it. Bounded to 7/pass.
            for(const auto& old:state.previousSelection) if(old.key==key) {
                const bool sameGeneration=old.generation==(kind==budget::Kind::Lamp?LampGeometry(light):0);
                if(slotTrace.enabled)
                    try { slotTrace.Claim(key,sameGeneration,geometry.distanceSquared,3.4e38f,volumeVisible,kind==budget::Kind::PlayerBeam); }
                    catch(...) { slotTrace.enabled=false; }
                ShadowTrace34::Emit({7,state.frame,GetTickCount(),key,relevant?1:0,sameGeneration?1:0,
                    static_cast<int>(flags | (volumeVisible?0x10000u:0)),static_cast<int>(state.nativeCandidates[index].claim),
                    viewWeight,geometry.distanceSquared,reach,light.mRadius});
                break;
            }
        }
        if (!state.pass.Active()) { ++fallbackPasses; ++rejectedPassReasons[state.pass.FailureCode() & 7]; TraceRejected(state.pass.FailureCode()); }
    }

    static void CompareNativeCandidates(SafetyHookContext& regs) noexcept
    {
        const FloatingPointState fp;
        if(!Enabled() || state.depth!=1 || !state.pass.Active() || !state.continuityActive) return;
        // The eighth insertion cell is native scratch; only the first seven
        // feed the dynamic pass. Do not read its uninitialized flags/index.
        if(regs.esi>=7*4 || (regs.esi&3u)) return;
        if(!state.stackAnchor || regs.esp!=state.stackAnchor) {++continuityRejected;return;}
        const auto challenger=*reinterpret_cast<const int32_t*>(regs.esp+0x24);
        const auto incumbent=*reinterpret_cast<const int32_t*>(regs.esp+0x7C+regs.esi);
        const auto count=CurrentCount();
        if(challenger<0 || incumbent<0) return; // Native inserts into empty cells.
        if(count>state.nativeCandidates.size() || static_cast<uint32_t>(challenger)>=count ||
           static_cast<uint32_t>(incumbent)>=count) {++continuityRejected;return;}
        const auto& a=state.nativeCandidates[challenger];
        const auto& b=state.nativeCandidates[incumbent];
        if(!a.observed || !b.observed || a.flags!=regs.edi ||
           b.flags!=*reinterpret_cast<const uint32_t*>(regs.esp+0x9C+regs.esi)) {++continuityRejected;return;}
        ++continuityComparisons;
        const int original=static_cast<int>(regs.eax);
        int result=fusionfix::shadows::NativeShadowContinuity42::Compare(original,a,b);
        const int ours=result;
        if(labCompareGrace)result=labCompareGrace(original,result,challenger,incumbent);
        if(slotTrace.enabled) {
            using Trace=fusionfix::shadows::ShadowSlotTrace;
            using Rule=fusionfix::shadows::NativeShadowContinuity42::Rule;
            const auto decider=fusionfix::shadows::NativeShadowContinuity42::Decider(original,a,b);
            auto rule=result!=ours ? Trace::Rule::Grace
                : decider==Rule::Special ? Trace::Rule::Special : decider==Rule::OwnBeam ? Trace::Rule::OwnBeam
                : decider==Rule::Gain ? Trace::Rule::Gain : decider==Rule::Claim ? Trace::Rule::Claim
                : decider==Rule::Nearer ? Trace::Rule::Nearer : decider==Rule::Fresh ? Trace::Rule::Fresh
                : decider==Rule::Reach ? Trace::Rule::Reach
                : result==1 ? Trace::Rule::Distance : Trace::Rule::Native;
            const auto* lights=CurrentLights();
            try { slotTrace.Compared(static_cast<uint32_t>(lights[challenger].mCastShadows),
                static_cast<uint32_t>(lights[incumbent].mCastShadows),rule,result); }
            catch(...) { slotTrace.enabled=false; }
        }
        if(result!=original) {
            regs.eax=static_cast<uint32_t>(result); ++continuityOverrides;
            // Decisions only, not render completion. No allocations/file I/O.
            if(!state.tracedComparison) {
                const auto* lights=CurrentLights();
                ShadowTrace34::Emit({6,state.frame,GetTickCount(),static_cast<uint32_t>(lights[challenger].mCastShadows),
                    original,result,static_cast<int>(lights[incumbent].mCastShadows),static_cast<int>(regs.esi/4),
                    static_cast<float>(a.claim),static_cast<float>(b.claim),a.ownBeam?1.0f:0.0f,b.ownBeam?1.0f:0.0f});
                state.tracedComparison=true;
            }
        }
    }

#include "ShadowCacheDependenciesRuntime44.inl"

    // StableMapSlots (Liberty's Shadows 2.0): a light that keeps its slot keeps its map's place in the atlas;
    // newcomers take the places left. The game's sort renumbers the seven every frame, so a lamp kept from one
    // frame to the next moved between tiles and slot records. Lights with the player flag 0x400 stay first, as the
    // game puts them. Run before the cache pick; the cache dependency check and Finalize see the same order.
    static void StabilizeMapOrdinals(SafetyHookContext& regs) noexcept {
        const FloatingPointState fp;
        if(!stableOrdinals || !Enabled() || state.depth!=1 || !state.pass.Active() || !state.continuityActive) return;
        const auto* lights=CurrentLights(); const auto count=CurrentCount();
        if(!state.stackAnchor || regs.esp!=state.stackAnchor || !CTimer::m_frameCount || *CTimer::m_frameCount!=state.frame ||
           !state.pass.InputsUnchanged(lights,count)) { ++stableOrdinalRejected; return; }
        auto* indices=reinterpret_cast<int32_t*>(regs.esp+allocation::SelectedIndicesStackOffset);
        std::array<fusionfix::shadows::ShadowMapOrdinal20,7> chosen{};
        for(unsigned i=0;i<7;++i) {
            const auto index=indices[i]; chosen[i].index=index;
            if(index==-1) continue;
            if(index<0 || static_cast<uint32_t>(index)>=count || !state.nativeCandidates[index].observed ||
               state.nativeCandidates[index].flags!=lights[index].mFlags) { ++stableOrdinalRejected; return; }
            chosen[i].claim=state.nativeCandidates[index].claim;
            chosen[i].flags=lights[index].mFlags;
        }
        std::array<int32_t,7> output{};
        if(!fusionfix::shadows::RestoreShadowMapOrdinals20(chosen,output)) { ++stableOrdinalRejected; return; }
        ++stableOrdinalPasses;
        for(unsigned i=0;i<7;++i) if(output[i]!=indices[i]) ++stableOrdinalMoves;
        std::memcpy(indices,output.data(),sizeof(output));
    }

    static void PreCache(SafetyHookContext& regs) noexcept {
        if(preCacheVacancy) preCacheVacancy(regs);
        StabilizeMapOrdinals(regs);
    }

    // One hook at the pre-cache boundary for the vacancy and the ordinals, after both were configured.
    static bool InstallPreCache() noexcept {
        if(preCacheHook || (!preCacheVacancy && !stableOrdinals) || !ready.load(std::memory_order_acquire)) return bool(preCacheHook);
        const auto at=reinterpret_cast<const uint8_t*>(gameBase+allocation::PreCacheRva);
        const auto skip=reinterpret_cast<const uint8_t*>(gameBase+allocation::CollectSkipRva);
        if(at[0]!=0x8A || at[1]!=0x15 || skip[0]!=0x8B || skip[1]!=0x54 || skip[2]!=0x24 || skip[3]!=0x14) return false;
        preCacheHook=safetyhook::create_mid(gameBase+allocation::PreCacheRva,PreCache);
        return bool(preCacheHook);
    }

    static void Finalize(SafetyHookContext& regs) noexcept
    {
        const FloatingPointState fp;
        if (!Enabled() || state.depth != 1 || !state.pass.Active()) return;
        if (!state.stackAnchor || state.stackAnchor != regs.esp ||
            !CTimer::m_frameCount || *CTimer::m_frameCount != state.frame ||
            CPlayer::getLocalPlayerPed() != state.ped || CPlayer::findPlayerCar() != state.occupiedCar)
        {
            RejectPass();
            return;
        }
        auto* indices = reinterpret_cast<int32_t*>(regs.esp + allocation::SelectedIndicesStackOffset);
        const auto traceTick=GetTickCount();
        auto traceChoice=[&](unsigned stage,unsigned slot,int index) noexcept {
            if(index<0 || static_cast<unsigned>(index)>=CurrentCount())return;
            const auto& light=CurrentLights()[index];
            const auto key=static_cast<uint32_t>(light.mCastShadows);
            ShadowTrace34::Track(key);
            ShadowTrace34::Emit({stage,state.frame,traceTick,key,static_cast<int>(slot),index,
                static_cast<int>(light.mFlags),static_cast<int>(light.mShadowCacheIndex),
                light.mPosition.x,light.mPosition.y,light.mPosition.z,light.mRadius});
        };
        for(unsigned slot=0;slot<7;++slot) traceChoice(1,slot,indices[slot]);
        // Candidate35: preserve native lamp slots and rank only the remaining beam space.
        // Observe mode tests the whole transaction against a private copy.
        // Switching to publication requires a new explicitly selected launch.
        std::array<int32_t, 7> observation{};
        if (!publicationEnabled) std::memcpy(observation.data(), indices, sizeof(observation));
        if (state.pass.Commit(CurrentLights(), CurrentCount(), publicationEnabled ? indices : observation.data(), true,
                              state.continuityActive))
        {
            const auto& traced=state.pass.LastSelection();
            for(unsigned slot=0;slot<7;++slot)
                if(traced.validMask&(1u<<slot)) traceChoice(2,slot,static_cast<int>(traced.slots[slot].index));
            if (publicationEnabled) {
                ++appliedPasses; state.committed = true;
                const auto& selection=state.pass.LastSelection();
                for(const auto& old:state.previousSelection) {
                    if(!old.key || old.kind!=budget::Kind::Lamp) continue;
                    bool retained=false;
                    for(const auto& now:selection.slots)
                        if(now.key==old.key && now.generation==old.generation) retained=true;
                    if(!retained) {
                        ++lampRemoved;
                        ShadowTrace34::Emit({3,state.frame,traceTick,static_cast<uint32_t>(old.key),
                            state.pass.WasObserved(old.key,old.generation)?1:0,0,0,0,0,0,0,0});
                        if(state.pass.WasObserved(old.key,old.generation)) ++lampDroppedPresent;
                        else ++lampMissingInput;
                    }
                }
                for(const auto& slot:selection.slots)
                    if(slot.key && slot.kind==budget::Kind::PlayerBeam) BeamTrace::Mark(BeamTrace::Selected);
                if(slotTrace.enabled && state.continuityActive) {
                    std::array<fusionfix::shadows::ShadowSlotTrace::Selected,7> traced{};
                    for(unsigned i=0;i<7;++i) if(selection.validMask&(1u<<i))
                        traced[i]={static_cast<uint32_t>(selection.slots[i].key),static_cast<int>(selection.slots[i].index)};
                    try { slotTrace.Commit(traced); } catch(...) { slotTrace.enabled=false; }
                }
                NoteSlotAcquires(selection);
                state.previousSelection=selection.slots;
                state.previousSelectionFrame=state.frame;
                std::array<fusionfix::shadows::NativeShadowContinuity42::Identity,7> identities{};
                for(unsigned i=0;i<7;++i) if(selection.validMask&(1u<<i))
                    identities[i]={static_cast<uint32_t>(selection.slots[i].key),selection.slots[i].generation};
                state.continuity.Commit(identities);
                for(const auto& slot:selection.slots) if(slot.key && slot.kind==budget::Kind::Lamp)
                    state.lampContinuity.Selected(slot.key,slot.generation);
            }
            else ++observedPasses;
        }
        else { ++fallbackPasses; ++rejectedPassReasons[state.pass.FailureCode() & 7]; TraceRejected(state.pass.FailureCode()); }
    }

    static void __cdecl Select()
    {
        const Invocation invocation;
        {
            const FloatingPointState fp;
            DWORD expected = 0;
            const DWORD current = GetCurrentThreadId();
            ownerThread.compare_exchange_strong(expected, current, std::memory_order_relaxed);
            if (ownerThread.load(std::memory_order_relaxed) != current)
                unsupportedThread.store(true, std::memory_order_relaxed);
            if (state.depth == 1) ++state.passSerial;
            if (state.depth == 1 && Enabled() && prepareSources && !unsupportedThread.load(std::memory_order_relaxed) &&
                reinterpret_cast<uintptr_t>(CurrentLights()) >= 0x10000)
                prepareSources(CurrentLights(), CurrentCount());
            if (state.depth != 1 || !Prepare()) RejectPass();
            else if(observeSubmittedLights && !unsupportedThread.load(std::memory_order_relaxed)) observeSubmittedLights(CurrentLights(),CurrentCount());
        }
        // Hooks are immutable once published. The unsafe call avoids the
        // hook-object mutex around native execution (including recursion).
        selectionHook.unsafe_ccall<void>();
        if(observeShadowResult && state.depth==1)observeShadowResult();
        // Native copying marks holes inactive but leaves their previous keys.
        // Clear only those dead identities before native frame-buffer copying.
        // This prevents lighting lookup from finding a stale, unrendered map.
        if (state.depth == 1 && state.committed && publicationEnabled) {
            auto* records = reinterpret_cast<void*>(gameBase + fusionfix::shadows::ce::casterguard::SlotKeyRva - 0xF8);
            staleKeysCleared.fetch_add(fusionfix::shadows::ClearInactiveShadowKeys(records,0x880),std::memory_order_relaxed);
        }
    }

    // The guard as the image was when the ASI loaded, before any module hooked anything: install
    // runs from the async initializers, where framelimit.ixx hooks the frame counter increment
    // the guard checks, and whichever ran first decided whether night shadow selection worked.
    static bool guardAtLoad = false;
    static std::string guardDetailsAtLoad;

    static bool Install(bool publish)
    {
        gameBase = GameBase();
        publicationEnabled = publish;
        if (!guardAtLoad)
        {
            installStatus = "guard_failed " + guardDetailsAtLoad;
            return false;
        }
        // Prepare all trampolines before any write. Mid hooks are inert until
        // ready is published AND execution is inside our selection wrapper.
        auto begin = safetyhook::InlineHook::create(gameBase + allocation::SelectionRva,
            Select, safetyhook::InlineHook::StartDisabled);
        auto collect = safetyhook::MidHook::create(gameBase + allocation::CollectRva,
            Collect, safetyhook::MidHook::StartDisabled);
        auto finish = safetyhook::MidHook::create(gameBase + allocation::FinalizeRva,
            Finalize, safetyhook::MidHook::StartDisabled);
        auto lampDistance = safetyhook::MidHook::create(gameBase + allocation::LampDistanceRva,
            AdjustLampDistance, safetyhook::MidHook::StartDisabled);
        auto compare = safetyhook::MidHook::create(gameBase + allocation::CompareResultRva,
            CompareNativeCandidates, safetyhook::MidHook::StartDisabled);
        auto cacheResult = safetyhook::MidHook::create(gameBase + allocation::CacheResultRva,
            ProtectCacheDependencies, safetyhook::MidHook::StartDisabled);
        auto cacheDistance = safetyhook::MidHook::create(gameBase + allocation::CacheDistanceRva,
            AdjustCacheDistance, safetyhook::MidHook::StartDisabled);
        if (!begin || !collect || !finish || !lampDistance || !compare || !cacheResult)
        {
            installStatus = std::string("hook_creation_failed begin=") + (begin ? "1" : "0") +
                " collect=" + (collect ? "1" : "0") + " finish=" + (finish ? "1" : "0") + " compare=" + (compare ? "1" : "0") + " cache=" + (cacheResult ? "1" : "0");
            return false;
        }
        selectionHook = std::move(*begin);
        collectHook = std::move(*collect);
        finalizeHook = std::move(*finish);
        lampDistanceHook = std::move(*lampDistance);
        compareResultHook = std::move(*compare);
        cacheResultHook = std::move(*cacheResult);
        // Optional: without it the caches go as the game picks them
        if (cacheDistance && cacheDistance->enable())
            cacheDistanceHook = std::move(*cacheDistance);
        if (!cacheResultHook.enable() || !compareResultHook.enable() || !lampDistanceHook.enable() || !collectHook.enable() || !finalizeHook.enable() || !selectionHook.enable())
        {
            installStatus = "hook_enable_failed";
            // Leave any enabled trampolines owned and inert; freeing them while
            // another thread is executing a stub would be unsafe.
            return false;
        }
        ready.store(true, std::memory_order_release);
        installStatus = "enabled";
        OutputDebugStringW(publish ? L"FusionFix experimental shadow allocator: publication enabled.\n" :
            L"FusionFix experimental shadow allocator: observing only; native output unchanged.\n");
        return true;
    }
}
