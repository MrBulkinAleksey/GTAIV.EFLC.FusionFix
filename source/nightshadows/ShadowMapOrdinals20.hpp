// From Liberty's Shadows 2.0 by OnyxOak (https://www.nexusmods.com/gta4/mods/1459), GPL-3.0.
#pragma once
#include <array>
#include <cstdint>

namespace fusionfix::shadows {
// Reorder only the seven lights already selected by native admission/ranking.
// Run after sorting, BEFORE cache scheduling and native map-record copying.
// A survivor keeps its ordinal; a replacement occupies a vacancy. No previous
// source, stale map, missing candidate or extra rendering slot is introduced.
struct ShadowMapOrdinal20 {
    std::int32_t index=-1;
    unsigned claim=7;
    std::uint32_t flags{};
};
inline bool RestoreShadowMapOrdinals20(const std::array<ShadowMapOrdinal20,7>& chosen,
                                      std::array<std::int32_t,7>& output) noexcept {
    std::array<std::int32_t,7> result;result.fill(-1);
    std::array<bool,7> placed{};
    unsigned special=0;
    // Native special priority must acquire immediately on player entry and
    // preserve its native order. Old scenery claims cannot occupy its prefix.
    for(unsigned i=0;i<7;++i) {
        if(chosen[i].index < -1 || chosen[i].claim>7)return false;
        if(chosen[i].index<0)continue;
        for(unsigned j=0;j<i;++j)
            if(chosen[j].index==chosen[i].index)return false;
        if(chosen[i].flags&0x400u) {
            result[special++]=chosen[i].index;placed[i]=true;
        }
    }
    for(unsigned i=0;i<7;++i) {
        const auto& c=chosen[i];
        if(c.index<0 || placed[i] || c.claim==7 || c.claim<special)continue;
        // Two current identities cannot inherit the same prior ordinal.
        if(result[c.claim]!=-1)return false;
        result[c.claim]=c.index;placed[i]=true;
    }
    for(unsigned i=0;i<7;++i) {
        if(chosen[i].index<0 || placed[i])continue;
        unsigned slot=0;while(slot<7 && result[slot]!=-1)++slot;
        if(slot==7)return false;
        result[slot]=chosen[i].index;
    }
    output=result;return true;
}
}
