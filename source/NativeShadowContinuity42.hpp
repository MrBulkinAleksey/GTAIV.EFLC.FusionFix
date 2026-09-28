#pragma once
#include <array>
#include <cstdint>

namespace fusionfix::shadows {
// Continuity participates in the native insertion sort, BEFORE static-cache
// scheduling. It never copies a cached map or manufactures an eligible light.
// Call Observe only for this frame's native-eligible inputs. Missing, invalid,
// offscreen or out-of-range lights cannot be pinned by old identities.
class NativeShadowContinuity42 {
public:
    static constexpr unsigned Slots=7, Unclaimed=Slots;
    struct Identity {
        std::uint32_t key{};
        std::uint64_t generation{};
    };
    struct Candidate {
        std::uint32_t flags{};
        unsigned claim=Unclaimed;
        bool ownBeam=false;
        bool observed=false;
    };
    void Begin(std::uintptr_t session,std::uint32_t frame,std::uint32_t now) noexcept {
        if(session!=session_ || frame<frame_ || now-time_>2000u) claims_={};
        session_=session;frame_=frame;time_=now;
        // A native update-divisor may skip passes. Keep the last successful set
        // through short gaps; abandon it after a pause/load, never by score.
        if(now-lastCommit_>250u || frame-lastCommitFrame_>16u) claims_={};
    }
    Candidate Observe(Identity id,std::uint32_t flags,bool relevant,bool own) const noexcept {
        Candidate c{flags,Unclaimed,own,true};
        if(id.key && relevant)
            for(unsigned i=0;i<Slots;++i)
                if(claims_[i].key==id.key && claims_[i].generation==id.generation) {c.claim=i;break;}
        return c;
    }
    bool Commit(const std::array<Identity,Slots>& chosen) noexcept {
        for(unsigned i=0;i<Slots;++i) if(chosen[i].key)
            for(unsigned j=0;j<i;++j) if(chosen[j].key==chosen[i].key) return false;
        claims_=chosen;lastCommit_=time_;lastCommitFrame_=frame_;return true;
    }
    // Native results: 0 inserts, 1 compares distance, 2 keeps the incumbent.
    // Preserve special native 0x400 priority. Own headlights stay usable on
    // entry/exit; otherwise hold the already drawn set across *all* light types.
    // A newcomer's category/distance cannot evict a still-visible valid claim.
    static int Compare(int native,const Candidate& challenger,const Candidate& incumbent) noexcept {
        if(native<0 || native>2 || !challenger.observed || !incumbent.observed ||
           ((challenger.flags|incumbent.flags)&0x400u)) return native;
        if(challenger.ownBeam!=incumbent.ownBeam) return challenger.ownBeam?0:2;
        if(challenger.claim!=incumbent.claim) return challenger.claim<incumbent.claim?0:2;
        return native;
    }
private:
    std::array<Identity,Slots> claims_{};
    std::uintptr_t session_{};
    std::uint32_t frame_{},time_{},lastCommit_{},lastCommitFrame_{};
};
}
