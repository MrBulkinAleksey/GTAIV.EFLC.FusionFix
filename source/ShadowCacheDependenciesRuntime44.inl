    // At CE527E29, native selection is complete but static-cache replacement
    // has not happened. Protect the cache dependencies of that exact output.
    // Never substitute dynamic indices after native cache scheduling (CE35).
    static void ProtectCacheDependencies(SafetyHookContext& regs) noexcept {
        const FloatingPointState fp;
        if(!Enabled() || state.depth!=1 || !state.pass.Active() || !state.continuityActive) return;
        if(!state.stackAnchor || regs.esp!=state.stackAnchor ||
           !CShadows::pFrameCounter || *CShadows::pFrameCounter!=state.frame) {++cacheDependencyRejected;return;}
        const int native=static_cast<int>(regs.edx);
        if(native<0 || native>=8)return;
        const auto* lights=CurrentLights();const auto count=CurrentCount();
        const auto address=reinterpret_cast<uintptr_t>(lights);
        if(address<0x10000 || count>4096 || address>UINTPTR_MAX-count*sizeof(rage::CLightSource)) {++cacheDependencyRejected;return;}
        const auto candidate=*reinterpret_cast<const int32_t*>(regs.esp+0x28);
        if(candidate<0 || static_cast<uint32_t>(candidate)>=count || regs.esi!=static_cast<uint32_t>(candidate)) {++cacheDependencyRejected;return;}
        const float distance=*reinterpret_cast<const float*>(regs.esp+0x2C);
        if(!std::isfinite(distance)||distance<0){++cacheDependencyRejected;return;}
        std::array<fusionfix::shadows::NativeCacheEntry44,8> cache{};
        for(unsigned i=0;i<cache.size();++i){
            const auto entry=gameBase+allocation::CacheAge0Rva+i*0x100;
            cache[i]={*reinterpret_cast<const float*>(entry-0xC),
                      *reinterpret_cast<const int32_t*>(entry),
                      *reinterpret_cast<const uint32_t*>(entry+4),
                      *reinterpret_cast<const int32_t*>(entry+8)};
        }
        // Also verify the copied decision agrees with the actual native call.
        // A mismatching layout or changing snapshot leaves native behavior alone.
        if(fusionfix::shadows::NativeCacheChoice44(distance,cache)!=native){++cacheDependencyRejected;return;}
        const auto* indices=reinterpret_cast<const int32_t*>(regs.esp+allocation::SelectedIndicesStackOffset);
        uint8_t dependencies=0;
        for(unsigned i=0;i<7;++i){
            const int index=indices[i];if(index==-1)continue;
            if(index<0 || static_cast<uint32_t>(index)>=count){++cacheDependencyRejected;return;}
            const auto& light=lights[index];
            if(!state.nativeCandidates[index].observed || state.nativeCandidates[index].flags!=light.mFlags){++cacheDependencyRejected;return;}
            const int slot=light.mShadowCacheIndex;
            if(slot==-1)continue;
            if(slot<0 || slot>=8 || cache[slot].key!=static_cast<uint32_t>(light.mCastShadows)) {++cacheDependencyRejected;return;}
            dependencies|=static_cast<uint8_t>(1u<<slot);
        }
        ++cacheDependencyChecks;
        const int result=fusionfix::shadows::ProtectNativeCacheChoice44(native,distance,cache,dependencies);
        if(result==native)return;
        regs.edx=static_cast<uint32_t>(result);
        if(result<0)++cacheDependencyDeferred;else ++cacheDependencyRedirected;
        ShadowTrace34::Emit({11,state.frame,GetTickCount(),cache[native].key,
            native,result,dependencies,static_cast<int>(lights[candidate].mCastShadows),
            distance,cache[native].distance,static_cast<float>(cache[native].age),static_cast<float>(cache[native].state)});
    }
