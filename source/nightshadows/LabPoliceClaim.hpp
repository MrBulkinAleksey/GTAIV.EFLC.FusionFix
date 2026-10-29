#pragma once
#include "LabSirenLifetime.hpp"
#include "NativeShadowContinuity42.hpp"
namespace lab_siren {
constexpr bool NativeSirenOn(std::uint8_t f19,std::uint8_t f1e) noexcept {
 return (f1e&0x20u)&&(f19&0x10u)&&!(f19&0x20u);
}
class PoliceClaim {
 Identity owner_{}; bool bound_=false,retired_=false,enabled_=false,on_=false,active_=false;
 std::uint32_t frame_=0,tick_=0,lastRelevantFrame_=0,lastRelevantTick_=0;
public:
 void Begin(bool valid,Identity owner,bool enabled,bool siren,std::uint32_t frame,std::uint32_t tick) noexcept {
  if(bound_&&(!valid||!(owner==owner_)))retired_=true;
  if(valid&&!bound_&&!retired_){owner_=owner;bound_=true;}
  if(frame-frame_>16||tick-tick_>250)active_=false;
  frame_=frame;tick_=tick;enabled_=enabled&&valid&&!retired_;on_=siren;
  if(!enabled_||!on_)active_=false;
 }
 bool Observe(bool present,bool eligibleRelevant) noexcept {
  if(enabled_&&on_&&present&&eligibleRelevant){active_=true;lastRelevantFrame_=frame_;lastRelevantTick_=tick_;return true;}
  if(frame_-lastRelevantFrame_>12||tick_-lastRelevantTick_>120)active_=false;
  return false; // No missing/irrelevant input is ever admitted or rendered.
 }
 bool Active()const noexcept{return active_;}
 bool Retired()const noexcept{return retired_;}
 static int Compare(int unchanged,bool ap,bool bp,bool protectedInput,
   const fusionfix::shadows::NativeShadowContinuity42::Candidate& a,
   const fusionfix::shadows::NativeShadowContinuity42::Candidate& b) noexcept {
  if(!protectedInput||ap==bp||!a.observed||!b.observed||((a.flags|b.flags)&0x400u)||a.ownBeam||b.ownBeam)return unchanged;
  return ap?0:2;
 }
};
}
