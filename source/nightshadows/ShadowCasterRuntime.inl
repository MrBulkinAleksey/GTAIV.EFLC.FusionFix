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
                << " lamps_without_casters=" << PlayerShadowAllocation::lampsWithoutCasters.load()
                << " lights_off_screen=" << PlayerShadowAllocation::lightsOffScreen.load()
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
