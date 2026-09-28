#pragma once
#include "PlayerShadowBudget.hpp"

namespace fusionfix::shadows::budget {
// The native pass has already scheduled static-cache work by the time our
// final-output hook runs. Do not substitute, remove or relocate its lamps.
// Rank beams only within native beam slots and empty dynamic slots.
inline PlayerShadowBudget::Selection PreserveNativeLampSlots(
    const PlayerShadowBudget::Selection& native,
    const PlayerShadowBudget::Selection& desired) noexcept
{
    using Selection = PlayerShadowBudget::Selection;
    using Slot = PlayerShadowBudget::Slot;
    const auto valid=[](const Selection& s) noexcept {
        if (!s.safeToApply || s.inputOverflow || s.ambiguousRecords || s.invalidInput ||
            s.unsupportedIdentity || (s.validMask & 0x80)) return false;
        unsigned count=0;
        for (unsigned i=0;i<7;++i) if(s.validMask&(1u<<i)) {
            const auto& a=s.slots[i]; ++count;
            if(!a.key || a.index==PlayerShadowBudget::InvalidIndex ||
                (a.kind!=Kind::Lamp && a.kind!=Kind::PlayerBeam && a.kind!=Kind::OtherBeam)) return false;
            for(unsigned j=0;j<i;++j) if(s.validMask&(1u<<j))
                if(s.slots[j].key==a.key || s.slots[j].index==a.index) return false;
        }
        return count==s.count;
    };
    if(!valid(native)||!valid(desired)) return {};
    for(unsigned i=0;i<7;++i) if(native.validMask&(1u<<i))
        for(unsigned j=0;j<7;++j) if(desired.validMask&(1u<<j)) {
            const auto& a=native.slots[i]; const auto& b=desired.slots[j];
            if((a.key==b.key || a.index==b.index) &&
                (a.key!=b.key || a.index!=b.index || a.generation!=b.generation || a.kind!=b.kind)) return {};
        }
    Selection out{}; out.safeToApply=true;
    const auto put=[&](unsigned slot,Slot light) noexcept {
        out.slots[slot]=light; out.validMask|=static_cast<std::uint8_t>(1u<<slot); ++out.count;
    };
    for(unsigned i=0;i<7;++i) if((native.validMask&(1u<<i)) && native.slots[i].kind==Kind::Lamp)
        put(i,native.slots[i]);
    const unsigned capacity=7-out.count;
    std::array<Slot,7> beams{};unsigned count=0;
    const auto add=[&](Slot s) noexcept {
        if(count==capacity) return;
        for(unsigned i=0;i<count;++i) if(beams[i].key==s.key) return;
        beams[count++]=s;
    };
    // Own beam wins available beam space; it cannot steal a cache-backed lamp.
    // Do not create a hole just because the narrower custom policy rejected a
    // beam the engine can already render. This preserves the native fallback.
    for(auto kind : {Kind::PlayerBeam,Kind::OtherBeam}) {
        for(unsigned i=0;i<7;++i) if((desired.validMask&(1u<<i)) && desired.slots[i].kind==kind) add(desired.slots[i]);
        for(unsigned i=0;i<7;++i) if((native.validMask&(1u<<i)) && native.slots[i].kind==kind) add(native.slots[i]);
    }
    std::array<bool,7> placed{};
    // Keep surviving beams in the engine's existing slots before filling gaps.
    for(unsigned i=0;i<7;++i) if((native.validMask&(1u<<i)) && native.slots[i].kind!=Kind::Lamp)
        for(unsigned j=0;j<count;++j) if(beams[j].key==native.slots[i].key) {put(i,beams[j]);placed[j]=true;break;}
    for(unsigned j=0;j<count;++j) if(!placed[j])
        for(unsigned i=0;i<7;++i) if(!(out.validMask&(1u<<i))) {put(i,beams[j]);break;}
    return out;
}
}
