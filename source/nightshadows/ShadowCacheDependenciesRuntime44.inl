    // At CE527E29, native selection is complete but static-cache replacement
    // has not happened. Protect the cache dependencies of that exact output.
    // Never substitute dynamic indices after native cache scheduling (CE35).
    static void ProtectCacheDependenciesNative(SafetyHookContext& regs) noexcept {
        if(!Enabled() || state.depth!=1 || !state.pass.Active() || !state.continuityActive) return;
        if(!state.stackAnchor || regs.esp!=state.stackAnchor ||
           !CTimer::m_frameCount || *CTimer::m_frameCount!=state.frame) {++cacheDependencyRejected;return;}
        const int native=static_cast<int>(regs.edx);
        if(native<0 || native>=static_cast<int>(fusionfix::shadows::cacheEntries))return;
        const auto* lights=CurrentLights();const auto count=CurrentCount();
        const auto address=reinterpret_cast<uintptr_t>(lights);
        if(address<0x10000 || count>4096 || address>UINTPTR_MAX-count*sizeof(rage::CLightSource)) {++cacheDependencyRejected;return;}
        const auto candidate=*reinterpret_cast<const int32_t*>(regs.esp+0x28);
        if(candidate<0 || static_cast<uint32_t>(candidate)>=count || regs.esi!=static_cast<uint32_t>(candidate)) {++cacheDependencyRejected;return;}
        const float distance=*reinterpret_cast<const float*>(regs.esp+0x2C);
        if(!std::isfinite(distance)||distance<0){++cacheDependencyRejected;return;}
        std::array<fusionfix::shadows::NativeCacheEntry44,fusionfix::shadows::MaxCacheEntries> cache{};
        for(unsigned i=0;i<fusionfix::shadows::cacheEntries;++i){
            const auto entry=fusionfix::shadows::CacheRecord(gameBase,i)+0xDC;
            cache[i]={*reinterpret_cast<const float*>(entry-0xC),
                      *reinterpret_cast<const int32_t*>(entry),
                      *reinterpret_cast<const uint32_t*>(entry+4),
                      *reinterpret_cast<const int32_t*>(entry+8)};
        }
        // Also verify the copied decision agrees with the actual native call.
        // A mismatching layout or changing snapshot leaves native behavior alone.
        if(fusionfix::shadows::NativeCacheChoice44(distance,cache)!=native){++cacheDependencyRejected;return;}
        const auto* indices=reinterpret_cast<const int32_t*>(regs.esp+allocation::SelectedIndicesStackOffset);
        uint32_t dependencies=0;
        for(unsigned i=0;i<7;++i){
            const int index=indices[i];if(index==-1)continue;
            if(index<0 || static_cast<uint32_t>(index)>=count){++cacheDependencyRejected;return;}
            const auto& light=lights[index];
            if(!state.nativeCandidates[index].observed || state.nativeCandidates[index].flags!=light.mFlags){++cacheDependencyRejected;return;}
            const int slot=light.mShadowCacheIndex;
            if(slot==-1)continue;
            if(slot<0 || slot>=static_cast<int>(fusionfix::shadows::cacheEntries) || cache[slot].key!=static_cast<uint32_t>(light.mCastShadows)) {++cacheDependencyRejected;return;}
            dependencies|=1u<<slot;
        }
        ++cacheDependencyChecks;
        const int result=fusionfix::shadows::ProtectNativeCacheChoice44(native,distance,cache,dependencies);
        if(result==native)return;
        regs.edx=static_cast<uint32_t>(result);
        if(result<0)++cacheDependencyDeferred;else ++cacheDependencyRedirected;
        ShadowTrace34::Emit({11,state.frame,GetTickCount(),cache[native].key,
            native,result,static_cast<int>(dependencies),static_cast<int>(lights[candidate].mCastShadows),
            distance,cache[native].distance,static_cast<float>(cache[native].age),static_cast<float>(cache[native].state)});
    }

    // LampShadowCaches 32: a near lamp's new cache in a whole tile, a far one's in a quarter (ShadowCache16::CacheChoice),
    // when the protection above left the game's pick as it was. [esp+0x2C] the lamp's distance.
    // It must leave alone the entries a slot copies its static world from this frame: the cache of each light the
    // selection picked for a slot (the 7 light indices on the stack) and of each slot record still active. Given to
    // another lamp and baked over, that slot drew the other lamp's static shadow (shadows where none should be).
    static int (*cacheChoice)(int native, float distance, uint32_t inUse) noexcept = nullptr;
    static uint32_t CachesInUse(const SafetyHookContext& regs) noexcept {
        uint32_t used=0;
        const auto* lights=CurrentLights();const auto count=CurrentCount();
        if(reinterpret_cast<uintptr_t>(lights)>=0x10000 && count<=4096) {
            const auto* indices=reinterpret_cast<const int32_t*>(regs.esp+allocation::SelectedIndicesStackOffset);
            for(unsigned i=0;i<7;++i) {
                const int index=indices[i];
                if(index<0 || static_cast<uint32_t>(index)>=count) continue;
                const int cache=lights[index].mShadowCacheIndex;
                if(cache>=0 && cache<32) used|=1u<<cache;
            }
        }
        for(unsigned slot=1;slot<8;++slot) {
            const auto record=gameBase+0xD9F100+slot*0x110;
            const int cache=*reinterpret_cast<const int32_t*>(record+0xF0);
            if(*reinterpret_cast<const uint8_t*>(record+0xED)==1 && cache>=0 && cache<32) used|=1u<<cache;
        }
        return used;
    }
    static void ProtectCacheDependencies(SafetyHookContext& regs) noexcept {
        const FloatingPointState fp;
        const auto native=regs.edx;
        ProtectCacheDependenciesNative(regs);
        if(cacheChoice && regs.edx==native && !unsupportedThread.load(std::memory_order_relaxed))
            regs.edx=static_cast<uint32_t>(cacheChoice(static_cast<int>(native),*reinterpret_cast<const float*>(regs.esp+0x2C),CachesInUse(regs)));
    }
