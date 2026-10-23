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
        // SlotMinHold: it took its slot less than that long ago, still holds a claim and its shadow shows
        // in view (gain InView), so it keeps the slot against anything but own headlights and the game's
        // 0x400 lights.
        bool fresh=false;
        // Within the shadow reach (a holder's a quarter further). One beyond it may still hold a slot when
        // nothing nearer wants it, and keeps a claim against others beyond it, but gives way to any within.
        bool inReach=true;
        // AddedLightsLast: a light the map gave no shadow (a facade's or a glow's, a traffic signal) with one of ours
        // gives way to any light whose shadow shows as much (gain), also holding a slot; meanwhile the shapes in the
        // light shaders shadow the cars, people and props under it.
        bool lastResort=false;
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
                      float distanceSquared=-1.0f,bool byCaster=false,bool fresh=false,bool inReach=true,bool lastResort=false) const noexcept {
        Candidate c{flags,Unclaimed,own,true,gain,distanceSquared,byCaster};
        c.inReach=inReach;
        c.lastResort=lastResort;
        if(id.key && relevant)
            for(unsigned i=0;i<Slots;++i)
                if(claims_[i].key==id.key && claims_[i].generation==id.generation) {c.claim=i;break;}
        // Only a slot whose shadow shows something in view is worth keeping: a lamp that took a free slot
        // with nobody in its light would otherwise keep a lamp with a car under it waiting.
        c.fresh=fresh && c.claim!=Unclaimed && gain==SlotGain::InView;
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
    enum class Rule : std::uint8_t { Native, Special, OwnBeam, Gain, Claim, Nearer, Fresh, Reach, LastResort };
    // Claims as one score per light rather than rules on pairs. Pairwise, a claim gave way to a newcomer with
    // someone in its light only when that one was ClaimDistanceRatio times nearer, while it beat any other
    // newcomer, and two newcomers went by distance: claim A at 30 m, caster newcomer B at 10 m, other newcomer
    // C at 5 m gave B over A, A over C and C over B. The game's insertion sort then made the seven out of the
    // order the lights came in (Liberty's Shadows 2.0 found the same with its pairwise bands).
    // Class 1: holders of a slot and newcomers with someone in their light; class 0: other newcomers, below.
    static unsigned ClaimClass(const Candidate& c) noexcept {
        return c.claim!=Unclaimed || (claimDistanceRatio>0 && c.byCaster) ? 1u : 0u;
    }
    // Within class 1, by distance, a holder's divided by the ratio: a newcomer takes the slot only from one
    // that many times further away; holders among themselves go by distance, as the game ranked them.
    static float ClaimScore(const Candidate& c) noexcept {
        const float r=claimDistanceRatio;
        return c.claim!=Unclaimed && r>0 ? c.distanceSquared/(r*r) : c.distanceSquared;
    }
    static bool ClaimScored(const Candidate& a,const Candidate& b) noexcept {
        return ClaimClass(a)==1 && ClaimClass(b)==1 && a.distanceSquared>=0 && b.distanceSquared>=0 &&
            ClaimScore(a)!=ClaimScore(b);
    }
    static Rule Decider(int native,const Candidate& challenger,const Candidate& incumbent) noexcept {
        if(native<0 || native>2 || !challenger.observed || !incumbent.observed) return Rule::Native;
        if((challenger.flags|incumbent.flags)&0x400u) return Rule::Special;
        if(challenger.ownBeam!=incumbent.ownBeam) return Rule::OwnBeam;
        if(challenger.gain!=incumbent.gain) return Rule::Gain;
        if(challenger.lastResort!=incumbent.lastResort) return Rule::LastResort;
        if(challenger.fresh!=incumbent.fresh) return Rule::Fresh;
        if(challenger.inReach!=incumbent.inReach) return Rule::Reach;
        if(ClaimClass(challenger)!=ClaimClass(incumbent)) return Rule::Claim;
        if(ClaimScored(challenger,incumbent)) {
            // A newcomer against a holder: by the scaled distance; otherwise plain distance
            if((challenger.claim==Unclaimed)!=(incumbent.claim==Unclaimed)) return Rule::Nearer;
            return Rule::Native;
        }
        return Rule::Native;
    }
    static int Compare(int native,const Candidate& challenger,const Candidate& incumbent) noexcept {
        if(native<0 || native>2 || !challenger.observed || !incumbent.observed ||
           ((challenger.flags|incumbent.flags)&0x400u)) return native;
        if(challenger.ownBeam!=incumbent.ownBeam) return challenger.ownBeam?0:2;
        // By what its shadow shows first, and only among those that show as much a last resort gives way, also when
        // it holds a slot. Before the gain, a facade light over you whose shadow showed lost its slot to lamps 60-100 m
        // off whose shadows showed nothing, and took it back each time one of them dropped out of the game's light list
        // for a frame: ten turns in ten seconds, the people's shadows under it swapping between its map and the
        // shapes. The hold is unchanged: a fresh holder shows in view, and nothing shows more.
        if(challenger.gain!=incumbent.gain) return challenger.gain>incumbent.gain?0:2;
        if(challenger.lastResort!=incumbent.lastResort) return challenger.lastResort?2:0;
        if(challenger.fresh!=incumbent.fresh) return challenger.fresh?0:2;
        if(challenger.inReach!=incumbent.inReach) return challenger.inReach?0:2;
        // Both held a slot: neither has the better right, and the slot each had says nothing. Ranked by that,
        // which the commit renumbers every frame, the one left out for a newcomer changed frame by frame, and
        // the held set turned round, each lamp's shadow gone for a frame (a trace: claim losses back in 11 ms).
        // By distance, the same one gives way each time.
        const unsigned a=ClaimClass(challenger),b=ClaimClass(incumbent);
        if(a!=b) return a>b?0:2;
        if(ClaimScored(challenger,incumbent)) return ClaimScore(challenger)<ClaimScore(incumbent)?0:2;
        return native;
    }
private:
    std::array<Identity,Slots> claims_{};
    std::uintptr_t session_{};
    std::uint32_t frame_{},time_{},lastCommit_{},lastCommitFrame_{};
};
}
