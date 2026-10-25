// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

// Candidate 14: narrowly scoped immediate-render experiment. AE3310 may queue
// AE0690 instead: that path does not visit the entity hook and is NOT fixed here.
namespace OwnHeadlightCaster
{
    namespace policy = fusionfix::shadows::caster;
    namespace guard = fusionfix::shadows::ce::casterguard;
    static thread_local policy::Context context;
    static std::atomic<bool> enabled{false};
    static uintptr_t base = 0;
    static std::atomic<uint32_t> passes{0}, deferredPasses{0}, ownPasses{0};
    static std::atomic<uint32_t> casterVisits{0}, carExcluded{0}, occupantsExcluded{0};
    static std::atomic<uint32_t> trafficCarExcluded{0};
    static std::atomic<uint32_t> boxedPasses{0}, boxedCarExcluded{0};
    // Lighting20: the car whose lamp a key of ours is (dual headlights, tail lights), 0 for other keys.
    static uintptr_t (*lampOwner)(uint32_t key) noexcept = nullptr;

    static policy::Context Capture(void* renderPass) noexcept
    {
        policy::Context result{};
        if (!enabled.load(std::memory_order_acquire) || !renderPass ||
            !bHeadlightShadows || !bVehicleNightShadows || !CPlayer::findPlayerCar)
            return result;
        ++passes;
        // TLS identity cannot be carried into the deferred renderer. Observe it
        // explicitly and leave that path alone rather than applying stale state.
        if (*reinterpret_cast<const uint8_t*>(base + guard::DeferredFlagRva))
        {
            ++deferredPasses;
            return result;
        }
        const auto pass = reinterpret_cast<const uint8_t*>(renderPass);
        if (*reinterpret_cast<const int32_t*>(pass + 0x938) == -1) return result;
        const auto slot = *reinterpret_cast<const uint32_t*>(pass + 0x940);
        if (slot < 1 || slot > 7) return result;
        const auto offset = slot * 0x110;
        const auto key = *reinterpret_cast<const uint32_t*>(base + guard::SlotKeyRva + offset);
        const auto kind = *reinterpret_cast<const uint32_t*>(base + guard::SlotKindRva + offset);
        const bool active = *reinterpret_cast<const uint8_t*>(base + guard::SlotActiveRva + offset) == 1;
        ShadowTrace34::Emit({5,CTimer::m_frameCount?*CTimer::m_frameCount:0,GetTickCount(),key,
            static_cast<int>(slot),static_cast<int>(kind),active?1:0,0,0,0,0,0});
        // The cars that came under this lamp while it had no slot keep their box shadows from it until they leave;
        // its map leaves them and the people in them out, so they have the one shadow. The slot record holds the
        // light's position at +0xC0.
        if (VehicleBoxShadowsBridge::BoxedCars)
        {
            // The lamp's own position, where the slot was moved off it (a lamp pair's map from between them)
            float position[3];
            HeadlightEnhancement::SlotLightPosition(slot, reinterpret_cast<const float*>(base + guard::SlotKeyRva - 0xF8 + 0xC0 + offset), position);
            result.boxedCount = VehicleBoxShadowsBridge::BoxedCars(position, result.boxed.data(), int(result.boxed.size()));
            if (result.boxedCount) ++boxedPasses;
            // CE: the driver followed by eight passengers
            if (PlayerCar::driverOffset)
                for (int i = 0; i < result.boxedCount; ++i)
                    for (int j = 0; j < 9 && result.boxedOccupantCount < int(result.boxedOccupants.size()); ++j)
                        if (const auto ped = reinterpret_cast<const uintptr_t*>(result.boxed[i] + PlayerCar::driverOffset)[j])
                            result.boxedOccupants[result.boxedOccupantCount++] = ped;
        }
        const auto car = CPlayer::findPlayerCar();
        // Exclude only the source vehicle from its own immediate
        // headlight pass. Confirm the opaque key was recently accepted by our submission
        // adapter; do not infer an owner pointer or dereference that key.
        if (bTrafficSelfShadowFix && kind == 4 && active &&
            CShadows::gStableHeadlightShadow.IsSubmittedBeam(key))
            result.trafficBeamKey = key;
        if (lampOwner && kind == 4 && active)
            result.lampOwner = lampOwner(key);
        if (!policy::OwnBeam(slot, kind, active, key, car)) return result;
        result.car = car;
        result.ownBeam = true;
        // Audited CE/official FusionFix: driver followed by eight passengers.
        std::memcpy(result.occupants.data(), reinterpret_cast<const void*>(car + 0xF50),
                    sizeof(result.occupants));
        ++ownPasses;
        return result;
    }

    static bool Exclude(uintptr_t entity, uint32_t type, bool artificial) noexcept
    {
        if (!enabled.load(std::memory_order_acquire)) return false;
        ++casterVisits;
        if (!bHeadlightShadows || !bVehicleNightShadows ||
            !policy::Exclude(context, entity, type, artificial)) return false;
        if ((type == 2 && std::find(context.boxed.begin(), context.boxed.begin() + context.boxedCount, entity) != context.boxed.begin() + context.boxedCount) ||
            (type == 3 && std::find(context.boxedOccupants.begin(), context.boxedOccupants.begin() + context.boxedOccupantCount, entity) !=
                context.boxedOccupants.begin() + context.boxedOccupantCount))
            ++boxedCarExcluded;
        else if (type == 2) { ++carExcluded; if (context.trafficBeamKey) ++trafficCarExcluded; }
        else ++occupantsExcluded;
        return true;
    }
}

// PostShadowsFromCarLights ([NIGHTSHADOWS]): street and park lamp posts in the shadow maps of car lights (Liberty's Shadows 2.0.6 found
// the same two gates). The caster loop (0xAE3713) draws a type-4 object in mode 0, the wd_local pass, only with bit
// 0x800 of +0x210, which posts lack (no map dummy behind them): in a slot of a car's lamp a post goes in as if it had
// it (resume at 0xAE3767 with EAX 1). Its fragment's children then reach the drawable call in 0xAEC7B0 with the
// material filter of the pass (0xAEC855: EDX = [context + 8]) 0, while the posts' shaders are all in bucket 4, so
// nothing was drawn: under wd_local, for a post whose shader group has bucket 4 and no bucket 0, the filter is 4.
// Lamps' own maps are left as they are: a post there shadowed its own light.
namespace PostCarLightShadows
{
    namespace guard = fusionfix::shadows::ce::casterguard;
    static bool enabled = false;
    static uintptr_t base = 0;
    static int32_t localGroup = -1;     // wd_local's technique group
    static thread_local bool carLightPass = false;
    static SafetyHookMid filterHook;
    static std::atomic<uint32_t> passes{0}, admitted{0}, routed{0};

    // The lamp post models (their names' hashes, model info +0x3c)
    static bool IsPost(uint32_t hash) noexcept
    {
        switch (hash)
        {
        case 0x67105BD6: case 0x46C33C49: case 0xD7B45B49: case 0x12272A5F: case 0xBA49A074: case 0xBAA79CDA:
        case 0x3A8020FF: case 0x70AB8D51: case 0x9EED69D4: case 0x97DFDA59: case 0x4AF44083: case 0x5B38E534:
        case 0x6CB90840: case 0x0B9145F2: case 0x1CD7687E: case 0x307B9012: case 0x2D7E0A07: case 0x2E9B0C05:
        case 0x70A9D8E5: case 0xBEF47579: case 0x1485E82F: case 0xD4E8589D: case 0x9A978A05: case 0x3D4E4F74:
        case 0x0F1C59B2: case 0x517FD33A:
            return true;
        default:
            return false;
        }
    }

    static bool PostModel(int index) noexcept
    {
        if (index < 0 || index >= 31000) return false;
        const auto info = *reinterpret_cast<const uintptr_t*>(base + 0xE95CD8 + index * 4); // CE 0x1295cd8
        return info >= 0x10000 && IsPost(*reinterpret_cast<const uint32_t*>(info + 0x3C));
    }

    // A car's lamp: ours (dual headlights, rear lamps) or the game's, keyed by the car's address or that + 1
    static bool CarKey(uint32_t key) noexcept
    {
        if (OwnHeadlightCaster::lampOwner && OwnHeadlightCaster::lampOwner(key)) return true;
        const auto pool = CVehicle::GetCheckedVehiclePool();
        if (!pool || !key) return false;
        const auto storage = reinterpret_cast<uintptr_t>(pool->m_aStorage), stride = static_cast<uintptr_t>(pool->m_nStorageSize);
        if (!stride) return false;
        for (const uintptr_t k : { uintptr_t(key), uintptr_t(key) - 1 })
        {
            if (k < storage) continue;
            const auto offset = k - storage, index = offset / stride;
            if (!(offset % stride) && index < static_cast<uintptr_t>(pool->m_nSize) && !(pool->m_aFlags[index] & 0x80))
                return true;
        }
        return false;
    }

    // The slot render 0xD77A00 of a car's lamp: slot 1-7, dynamic (kind 4) and active
    static bool CarLightPass(void* renderPass) noexcept
    {
        if (!enabled || !renderPass || !bHeadlightShadows || *reinterpret_cast<const uint8_t*>(base + guard::DeferredFlagRva))
            return false;
        const auto pass = reinterpret_cast<const uint8_t*>(renderPass);
        if (*reinterpret_cast<const int32_t*>(pass + 0x938) == -1) return false;
        const auto slot = *reinterpret_cast<const uint32_t*>(pass + 0x940);
        if (slot < 1 || slot > 7) return false;
        const auto offset = slot * 0x110;
        if (*reinterpret_cast<const uint32_t*>(base + guard::SlotKindRva + offset) != 4 ||
            *reinterpret_cast<const uint8_t*>(base + guard::SlotActiveRva + offset) != 1)
            return false;
        if (!CarKey(*reinterpret_cast<const uint32_t*>(base + guard::SlotKeyRva + offset))) return false;
        ++passes;
        return true;
    }

    class Scope
    {
        bool saved;
    public:
        explicit Scope(void* renderPass) noexcept : saved(carLightPass) { carLightPass = CarLightPass(renderPass); }
        ~Scope() noexcept { carLightPass = saved; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    // The caster loop's entity (type EAX, mode EDX) in the pass: let a post into mode 0
    static bool Admit(uintptr_t entity, uint32_t type, uint32_t mode, bool artificial) noexcept
    {
        if (!carLightPass || !artificial || type != 4 || mode != 0 || !entity ||
            (*reinterpret_cast<const uint32_t*>(entity + 0x210) & 0x800) ||
            !PostModel(*reinterpret_cast<const int16_t*>(entity + 0x2E)))
            return false;
        ++admitted;
        return true;
    }

    // At 0xAEC855: ESI the drawable, EDX the material filter
    static void Filter(SafetyHookContext& regs) noexcept
    {
        if (regs.edx || localGroup < 0 || *reinterpret_cast<const int32_t*>(base + 0xC6B308) != localGroup) return;
        uint16_t model;
        const float* place;
        if (!EntityClass::CurrentModel(model, place) || !PostModel(model)) return;
        const auto drawable = static_cast<uintptr_t>(regs.esi);
        const auto group = drawable ? *reinterpret_cast<const uintptr_t*>(drawable + 8) : 0;
        if (!group) return;
        const auto shaders = *reinterpret_cast<const uintptr_t* const*>(group + 8);
        const auto count = *reinterpret_cast<const uint16_t*>(group + 0xC);
        if (!shaders || !count || count > 128) return;
        uint32_t buckets = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (const auto shader = shaders[i]; shader)
                if (const auto bucket = *reinterpret_cast<const uint8_t*>(shader + 9); bucket < 32)
                    buckets |= 1u << bucket;
        if ((buckets & (1u << 4)) && !(buckets & 1u))
        {
            regs.edx = 4;
            ++routed;
        }
    }

    static void Install(bool on, int32_t wdLocal) noexcept
    {
        base = GameBase();
        localGroup = wdLocal;
        if (!on || wdLocal < 0) return;
        // mov edx, [esp+10h]; mov ecx, [esp+28h]; mov edx, [edx+8]; test ecx, ecx; jz
        auto pattern = hook::pattern("8B 54 24 10 8B 4C 24 28 8B 52 08 85 C9 0F 84");
        if (pattern.empty()) return;
        filterHook = safetyhook::create_mid(pattern.get_first(11), Filter);
        enabled = static_cast<bool>(filterHook);
    }
}

namespace ShadowDiagnostics
{
    static fusionfix::DiagnosticsLog log;
    static bool guardPassed = false;
    static int casterMode = 0, allocationMode = 0;
    static std::string startupGuardDetails;
    static bool startupWritten = false;
    static bool admissionInstalled = false;
    static std::string adapterStatus;

    static void Write() noexcept
    {
        // Low-frequency game-event I/O, never inside submission/caster hooks.
        log.Write(std::ios::app, [](std::ostream& log, uint64_t now)
        {
            ShadowTrace34::Flush();
            if (!startupWritten)
            {
                log << "adapter " << adapterStatus << '\n';
                log << "startup_guard " << startupGuardDetails << '\n';
                log << "allocator_startup " << PlayerShadowAllocation::installStatus << '\n';
                if (log.good()) startupWritten = true;
            }
            log << "tick=" << now << " admission_installed=" << admissionInstalled
                << " lamp_size_gate=" << LampShadowSizeGate::status << " lamp_size_gate_kept=" << LampShadowSizeGate::kept.load()
                << " lamp_size_gate_retained=" << LampShadowSizeGate::retained.load() << " lamp_size_gate_indoor=" << LampShadowSizeGate::indoor.load()
                << " lamp_size_gate_fade=" << LampShadowSizeGate::fade << " lamp_size_gate_faded=" << LampShadowSizeGate::faded.load()
                << " model_ref_guard=" << ModelReferenceQueue::status << " model_ref_duplicates=" << ModelReferenceQueue::duplicates.load()
                << " model_ref_spilled=" << ModelReferenceQueue::spilled.load() << " model_ref_invalid=" << ModelReferenceQueue::invalid.load()
                << " traffic_self_shadow_fix=" << bTrafficSelfShadowFix
                << " traffic_car_excluded=" << OwnHeadlightCaster::trafficCarExcluded.load()
                << " post_car_light_shadows=" << PostCarLightShadows::enabled << " post_car_light_passes=" << PostCarLightShadows::passes.load()
                << " posts_admitted=" << PostCarLightShadows::admitted.load() << " post_filters_routed=" << PostCarLightShadows::routed.load()
                << " close_headlight_relevance=" << bCloseHeadlightRelevance << " caster_guard=" << guardPassed
                << " caster_requested=" << casterMode << " caster_enabled=" << OwnHeadlightCaster::enabled.load()
                << " lamp_policy=scene_camera_cache_continuity allocation_mode=" << allocationMode << " allocation_ready=" << PlayerShadowAllocation::ready.load()
                << " allocation_thread_block=" << PlayerShadowAllocation::unsupportedThread.load()
                << " capture_calls=" << PlayerShadowAllocation::captureCalls.load()
                << " capture_valid=" << PlayerShadowAllocation::captureValid.load()
                << " view_w=" << PlayerShadowAllocation::captureWidth.load()
                << " view_h=" << PlayerShadowAllocation::captureHeight.load()
                << " device_w=" << PlayerShadowAllocation::activeWidth.load()
                << " device_h=" << PlayerShadowAllocation::activeHeight.load()
                << " crash_trace=" << (shadow_crash_trace::handler != nullptr)
                << " camera_priority=" << PlayerShadowAllocation::cameraPriority
                << " scene_camera_reads=" << PlayerShadowAllocation::sceneCameraReads.load()
                << " auxiliary_views_rejected=" << PlayerShadowAllocation::auxiliaryViewsRejected.load()
                << " camera_passes=" << PlayerShadowAllocation::cameraPasses.load()
                << " camera_fallbacks=" << PlayerShadowAllocation::cameraFallbacks.load()
                << " trace_enabled=" << ShadowTrace34::enabled.load()
                << " trace_dropped=" << ShadowTrace34::recorder.dropped.load()
                << " native_lamp_priority=" << PlayerShadowAllocation::nativeLampPriority
                << " cache_dependency_checks=" << PlayerShadowAllocation::cacheDependencyChecks.load()
                << " cache_dependency_redirected=" << PlayerShadowAllocation::cacheDependencyRedirected.load()
                << " cache_dependency_deferred=" << PlayerShadowAllocation::cacheDependencyDeferred.load()
                << " cache_dependency_rejected=" << PlayerShadowAllocation::cacheDependencyRejected.load()
                << " continuity_compares=" << PlayerShadowAllocation::continuityComparisons.load()
                << " continuity_overrides=" << PlayerShadowAllocation::continuityOverrides.load()
                << " continuity_rejected=" << PlayerShadowAllocation::continuityRejected.load()
                << " lamp_distance_adjusted=" << PlayerShadowAllocation::lampDistanceAdjusted.load()
                << " receiver_ranked=" << PlayerShadowAllocation::receiverRanked.load() << " indoor_ranked=" << PlayerShadowAllocation::indoorRanked.load()
                << " cache_view_priority=" << PlayerShadowAllocation::cacheViewPriority << " cache_distance_adjusted=" << PlayerShadowAllocation::cacheDistanceAdjusted.load()
                << " caster_priority=" << PlayerShadowAllocation::casterPriority
                << " caster_captures=" << PlayerShadowAllocation::casterCaptures.load()
                << " cars_left_to_boxes=" << PlayerShadowAllocation::carsLeftToBoxes.load()
                << " peds_left_out=" << PlayerShadowAllocation::pedsLeftOut.load()
                << " interior_lights_yielded=" << PlayerShadowAllocation::interiorLightsYielded.load()
                << " interior_scene=" << PlayerShadowAllocation::interiorScene.load()
                << " interior_lights_first=" << PlayerShadowAllocation::interiorLightsFirst
                << " player_interior=" << PlayerShadowAllocation::playerInterior.load()
                << " room_lights_first=" << PlayerShadowAllocation::roomLightsFirst.load()
                << " lamps_without_casters=" << PlayerShadowAllocation::lampsWithoutCasters.load()
                << " lights_off_screen=" << PlayerShadowAllocation::lightsOffScreen.load()
                << " cached_lamps_in_view=" << PlayerShadowAllocation::cachedLampsInView.load()
                << " uncached_lamps_trimmed=" << PlayerShadowAllocation::uncachedLampsTrimmed.load()
                << " lamps_behind_camera=" << PlayerShadowAllocation::lampsBehindCamera.load()
                << " added_lights_yielded=" << PlayerShadowAllocation::addedLightsYielded.load()
                << " added_lights_last=" << PlayerShadowAllocation::addedLightsLast << " added_lights_last_resort=" << PlayerShadowAllocation::addedLightsLastResort.load()
                << " building_lights_lowered=" << PlayerShadowAllocation::buildingLightsLowered.load()
                << " nearby_receivers=" << NearbyVehicleLighting36::enabled.load()
                << " receiver_captures=" << NearbyVehicleLighting36::captures.load()
                << " receiver_matches=" << NearbyVehicleLighting36::matches.load()
                << " receiver_invalid_pool=" << NearbyVehicleLighting36::invalidPool.load()
                << " lookup_guard=" << ShadowLookupGuard::ready
                << " lamp_removed=" << PlayerShadowAllocation::lampRemoved.load()
                << " lamp_missing_input=" << PlayerShadowAllocation::lampMissingInput.load()
                << " lamp_dropped_present=" << PlayerShadowAllocation::lampDroppedPresent.load()
                << " rejected_dynamic_map=" << ShadowLookupGuard::rejectedDynamic.load()
                << " rejected_static_map=" << ShadowLookupGuard::rejectedStatic.load()
                << " lookup_buffer_changed=" << ShadowLookupGuard::changedBuffer.load()
                << " stale_keys_cleared=" << PlayerShadowAllocation::staleKeysCleared.load()
                << " rejected_adapter=" << PlayerShadowAllocation::rejectedAdapterChecks.load()
                << " rejected_input=" << PlayerShadowAllocation::rejectedPassReasons[2].load()
                << " rejected_duplicate=" << PlayerShadowAllocation::rejectedPassReasons[3].load()
                << " rejected_capacity=" << PlayerShadowAllocation::rejectedPassReasons[4].load()
                << " rejected_commit=" << PlayerShadowAllocation::rejectedPassReasons[5].load()
                << " rejected_snapshot=" << PlayerShadowAllocation::rejectedPassReasons[6].load()
                << " applied=" << PlayerShadowAllocation::appliedPasses.load()
                << " observed=" << PlayerShadowAllocation::observedPasses.load()
                << " fallback=" << PlayerShadowAllocation::fallbackPasses.load()
                << " passes=" << OwnHeadlightCaster::passes.load()
                << " deferred=" << OwnHeadlightCaster::deferredPasses.load()
                << " own_beam_passes=" << OwnHeadlightCaster::ownPasses.load()
                << " caster_visits=" << OwnHeadlightCaster::casterVisits.load()
                << " car_excluded=" << OwnHeadlightCaster::carExcluded.load()
                << " occupants_excluded=" << OwnHeadlightCaster::occupantsExcluded.load()
                << " boxed_lamp_passes=" << OwnHeadlightCaster::boxedPasses.load()
                << " boxed_cars_excluded=" << OwnHeadlightCaster::boxedCarExcluded.load()
                << " driving_beam_yes=" << CShadows::drivingBeamAccepted.load()
                << " driving_beam_no=" << CShadows::drivingBeamRejected.load() << '\n';
        });
    }
}
