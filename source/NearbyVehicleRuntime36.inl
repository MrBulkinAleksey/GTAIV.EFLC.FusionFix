namespace NearbyVehicleLighting36 {
    static std::atomic<bool> enabled{false};
    static std::atomic_flag snapshotLock=ATOMIC_FLAG_INIT;
    static fusionfix::shadows::NearbyVehicleReceivers36 snapshot{};
    static std::atomic<uint32_t> captures{0},matches{0},invalidPool{0};

    static void Update() noexcept {
        using namespace fusionfix::shadows;
        if(!enabled.load(std::memory_order_acquire) || !CTimer::m_frameCount ||
            !CTimer::m_snTimeInMilliseconds || !CPlayer::getLocalPlayerPed || !CPlayer::findPlayerCar) return;
        NearbyVehicleReceivers36 next{};
        next.frame=*CTimer::m_frameCount;next.timeMs=*CTimer::m_snTimeInMilliseconds;
        // Keep the accepted driving behavior unchanged. Collect only on the
        // game-process callback; render hooks consume copied positions.
        next.session=CPlayer::getLocalPlayerPed();
        if(next.session && !CPlayer::findPlayerCar()) {
            const auto matrix=*reinterpret_cast<const float* const*>(next.session+0x20);
            if(matrix) {
                next.origin={matrix[12],matrix[13],matrix[14]};
                next.view=PlayerShadowAllocation::ReadGameplayView();
                if(!next.view.valid && !PlayerShadowAllocation::gameplayViewLock.test_and_set(std::memory_order_acquire)) {
                    if(next.frame-PlayerShadowAllocation::gameplayViewFrame<=2)
                        next.view=PlayerShadowAllocation::gameplayView;
                    PlayerShadowAllocation::gameplayViewLock.clear(std::memory_order_release);
                }
                const auto pool=CVehicle::GetVehiclePool();
                if(pool && pool->m_aStorage && pool->m_aFlags && pool->m_nSize>0 && pool->m_nSize<=4096 &&
                    pool->m_nStorageSize>=0x24 && pool->m_nStorageSize<=0x10000 &&
                    reinterpret_cast<uintptr_t>(pool->m_aStorage)<=UINTPTR_MAX-
                        static_cast<uintptr_t>(pool->m_nSize)*pool->m_nStorageSize) {
                    for(int i=0;i<pool->m_nSize;++i) {
                        const auto vehicle=reinterpret_cast<uintptr_t>(pool->GetSlot(i));
                        if(!vehicle) continue;
                        const auto transform=*reinterpret_cast<const float* const*>(vehicle+0x20);
                        if(transform) next.Add({transform[12],transform[13],transform[14]},vehicle);
                    }
                } else ++invalidPool;
            }
        }
        if(!snapshotLock.test_and_set(std::memory_order_acquire)) {
            snapshot=next;++captures;snapshotLock.clear(std::memory_order_release);
        }
    }

    static float Score(uintptr_t player,uint32_t frame,uint32_t now,
        fusionfix::shadows::Vec3 position,fusionfix::shadows::Vec3 source,
        fusionfix::shadows::Vec3 direction,float outerCos,float radius,uintptr_t key) noexcept {
        if(!enabled.load(std::memory_order_acquire)) return 0.0f;
        // A contended publisher cannot make an otherwise current receiver set
        // vanish for a frame. Never retain it beyond the explicit age limits.
        static thread_local fusionfix::shadows::NearbyVehicleReceivers36 local;
        if(!snapshotLock.test_and_set(std::memory_order_acquire)) {
            local=snapshot;snapshotLock.clear(std::memory_order_release);
        }
        const float result=local.Score(player,frame,now,position,source,direction,outerCos,radius,key);
        if(result)++matches;
        return result;
    }
    static bool Relevant(uintptr_t player,uint32_t frame,uint32_t now,
        fusionfix::shadows::Vec3 position,fusionfix::shadows::Vec3 source,
        fusionfix::shadows::Vec3 direction,float outerCos,float radius,uintptr_t key) noexcept {
        return Score(player,frame,now,position,source,direction,outerCos,radius,key)>0;
    }
}
