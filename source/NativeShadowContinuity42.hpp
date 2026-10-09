#pragma once
#include "ShadowCasterPresence.hpp"
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
        // Beams and uncached lamps by whether their light reaches the view,
        // cached lamps by the vehicles and peds in their light.
        SlotGain gain=SlotGain::InView;
        // What it is ranked by, squared; negative when unknown. Only ClaimDistanceRatio reads it.
        float distanceSquared=-1.0f;
        // Its gain comes from a vehicle or ped in a cached lamp's light: only such a newcomer
        // can push out a far claim, so dense uncached lamps (tunnels) do not swap more often.
        bool byCaster=false;
        // SlotMinHold: it took its slot less than that long ago and still holds a claim, so it keeps
        // the slot against anything but own headlights and the game's 0x400 lights.
        bool fresh=false;
    };
    // ClaimDistanceRatio: a claim keeps its slot against a newcomer of the same gain unless
    // the claim is this many times further away than the newcomer; 0 always keeps it.
    static inline float claimDistanceRatio=0.0f;
    // Why held claims were dropped, for the slot trace: 0 when none were held or none dropped.
    enum Reset : unsigned { ResetSession=1, ResetFrameBack=2, ResetPause=4, ResetCommitTime=8, ResetCommitFrames=16 };
    unsigned Begin(std::uintptr_t session,std::uint32_t frame,std::uint32_t now) noexcept {
        bool held=false;
        for(const auto& c:claims_) held|=c.key!=0;
        unsigned reset=(session!=session_?ResetSession:0u)|(frame<frame_?ResetFrameBack:0u)|(now-time_>2000u?ResetPause:0u);
        if(reset) claims_={};
        session_=session;frame_=frame;time_=now;
        // A native update-divisor may skip passes. Keep the last successful set
        // through short gaps; abandon it after a pause/load, never by score.
        const unsigned gap=(now-lastCommit_>250u?ResetCommitTime:0u)|(frame-lastCommitFrame_>16u?ResetCommitFrames:0u);
        if(gap) claims_={};
        return held?reset|gap:0u;
    }
    Candidate Observe(Identity id,std::uint32_t flags,bool relevant,bool own,SlotGain gain=SlotGain::InView,
                      float distanceSquared=-1.0f,bool byCaster=false,bool fresh=false) const noexcept {
        Candidate c{flags,Unclaimed,own,true,gain,distanceSquared,byCaster};
        if(id.key && relevant)
            for(unsigned i=0;i<Slots;++i)
                if(claims_[i].key==id.key && claims_[i].generation==id.generation) {c.claim=i;break;}
        c.fresh=fresh && c.claim!=Unclaimed;
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
    // A newcomer's category/distance cannot evict a still-visible valid claim,
    // unless the newcomer's shadow is in view and the claim's is not, or the
    // claim only redraws what the lamp's cache already shows.
    // Which rule of Compare decided, for the slot trace.
    enum class Rule : std::uint8_t { Native, Special, OwnBeam, Gain, Claim, Nearer, Fresh };
    // Whether the unclaimed one of the two is so much nearer that the claim gives way.
    static bool NearerThanClaim(const Candidate& unclaimed,const Candidate& claimed) noexcept {
        const float r=claimDistanceRatio;
        return r>0 && unclaimed.byCaster && unclaimed.claim==Unclaimed && claimed.claim!=Unclaimed &&
            unclaimed.distanceSquared>=0 && claimed.distanceSquared>=0 &&
            unclaimed.distanceSquared*r*r<claimed.distanceSquared;
    }
    static Rule Decider(int native,const Candidate& challenger,const Candidate& incumbent) noexcept {
        if(native<0 || native>2 || !challenger.observed || !incumbent.observed) return Rule::Native;
        if((challenger.flags|incumbent.flags)&0x400u) return Rule::Special;
        if(challenger.ownBeam!=incumbent.ownBeam) return Rule::OwnBeam;
        if(challenger.fresh!=incumbent.fresh) return Rule::Fresh;
        if(challenger.gain!=incumbent.gain) return Rule::Gain;
        if(challenger.claim!=incumbent.claim)
            return NearerThanClaim(challenger,incumbent) || NearerThanClaim(incumbent,challenger) ? Rule::Nearer : Rule::Claim;
        return Rule::Native;
    }
    static int Compare(int native,const Candidate& challenger,const Candidate& incumbent) noexcept {
        if(native<0 || native>2 || !challenger.observed || !incumbent.observed ||
           ((challenger.flags|incumbent.flags)&0x400u)) return native;
        if(challenger.ownBeam!=incumbent.ownBeam) return challenger.ownBeam?0:2;
        if(challenger.fresh!=incumbent.fresh) return challenger.fresh?0:2;
        if(challenger.gain!=incumbent.gain) return challenger.gain>incumbent.gain?0:2;
        if(challenger.claim!=incumbent.claim) {
            if(NearerThanClaim(challenger,incumbent)) return 0;
            if(NearerThanClaim(incumbent,challenger)) return 2;
            return challenger.claim<incumbent.claim?0:2;
        }
        return native;
    }
private:
    std::array<Identity,Slots> claims_{};
    std::uintptr_t session_{};
    std::uint32_t frame_{},time_{},lastCommit_{},lastCommitFrame_{};
};
}
