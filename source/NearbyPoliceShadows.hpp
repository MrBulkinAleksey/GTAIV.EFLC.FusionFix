#pragma once
#include "LabSirenLifetime.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
namespace lab_siren {
constexpr std::uint32_t ModelHash(const char* text) noexcept {
 std::uint32_t h=0;while(*text){h+=static_cast<unsigned char>(*text++);h+=h<<10;h^=h>>6;}h+=h<<3;h^=h>>11;h+=h<<15;return h;
}
constexpr std::array<const char*,13> PoliceModels{"police","police2","police3","police4","policew","policeb","fbi","noose","polpatriot","nstockade","pstockade","firetruk","ambulance"};
constexpr bool PoliceModel(std::uint32_t hash) noexcept {for(auto name:PoliceModels)if(ModelHash(name)==hash)return true;return false;}
constexpr std::array<std::uint32_t,6> PoliceSites{0x640d0a,0x64126e,0x64161d,0x6418f0,0x642053,0x6425ab};
constexpr bool PoliceSite(std::uint32_t site) noexcept {for(auto s:PoliceSites)if(site==s)return true;return false;}
// Exact guarded native callsite ABI. EDI is not an owner at every producer:
// the NOOSE/truck group uses ESI, and the POLICEB path uses ECX. Never guess
// another register after pool validation fails, or mutate the native registers.
constexpr std::uintptr_t PoliceOwnerAtSite(std::uint32_t site,std::uintptr_t edi,std::uintptr_t esi,std::uintptr_t ecx) noexcept {
 switch(site){case 0x640d0a:case 0x64126e:case 0x6418f0:case 0x6425ab:return edi;
 case 0x64161d:return esi;case 0x642053:return ecx;default:return 0;}
}
// Fixed storage only. Native cache ownership is consulted by the adapter before
// any virgin or quarantined token is leased. No native cache is cleared here.
class NearbyPolice {
public:
 static constexpr unsigned Candidates=8,Promoted=2,Tokens=64;
 struct Owner {
  Identity identity{};std::uint32_t model{},emitter{},generation{},seenFrame{},seenTick{},inputFrame{},inputBits{};
  std::uint32_t promotedFrame=UINT32_MAX;unsigned token=Tokens,rank=Promoted;
  float distanceSquared{};bool live=false,eligible=false,player=false;
 };
 struct Token {unsigned state=0;std::uint32_t retiredFrame{},retiredTick{};}; // 0 virgin, 1 leased, 2 quarantine
 std::array<Owner,Candidates> owners{};std::array<Token,Tokens> tokens{};
 std::uint32_t generation=0,frame=0,tick=0;bool started=false;
 void Retire(Owner& o) noexcept {if(!o.live)return;tokens[o.token]={2,frame,tick};o=Owner{};}
 template<class Valid> void Begin(std::uint32_t f,std::uint32_t t,Valid valid) noexcept {
  if(started&&frame==f)return;started=true;frame=f;tick=t;std::array<bool,Candidates> retained{};
  for(unsigned i=0;i<Candidates;++i){auto& o=owners[i];retained[i]=o.rank<Promoted;o.rank=Promoted;
   if(!o.live)continue;if(!valid(o)||frame-o.seenFrame>60||tick-o.seenTick>1000){Retire(o);continue;}
   if(frame-o.seenFrame>2)o.eligible=false;
  }
  for(unsigned rank=0;rank<Promoted;++rank){unsigned best=Candidates;float score=std::numeric_limits<float>::infinity();
   for(unsigned i=0;i<Candidates;++i){const auto& o=owners[i];if(!o.live||!o.eligible||o.rank<Promoted||!std::isfinite(o.distanceSquared))continue;
    const float s=o.player?-1.f:o.distanceSquared*(retained[i]?0.85f:1.f);
    if(s<score){score=s;best=i;}}
   if(best!=Candidates)owners[best].rank=rank;
  }
 }
 template<class NativeClear> Owner* Touch(Identity id,std::uint32_t model,std::uint32_t emitter,float distance,bool player,bool eligible,NativeClear clear) noexcept {
  if(!PoliceModel(model)||!PoliceSite(emitter)||!std::isfinite(distance)||distance<0)return nullptr;
  Owner* found=nullptr;
  for(auto& o:owners)if(o.live){if(o.identity==id&&o.model==model){found=&o;break;}
   if(o.identity.owner==id.owner||(o.identity.pool==id.pool&&o.identity.slot==id.slot))Retire(o);}
  if(!found&&!eligible)return nullptr;
  if(!found){for(auto& o:owners)if(!o.live){found=&o;break;}
   if(!found){for(auto& o:owners)if(!o.eligible&&frame-o.seenFrame>=12&&tick-o.seenTick>=120){Retire(o);found=&o;break;}}
   if(!found)return nullptr;
   unsigned token=Tokens;for(unsigned i=0;i<Tokens;++i){const auto& k=tokens[i];if(k.state==1)continue;
    if(k.state==2&&(frame-k.retiredFrame<24||tick-k.retiredTick<250))continue;
    if(clear(i)){token=i;break;}}
   if(token==Tokens)return nullptr;
   tokens[token].state=1;*found=Owner{};found->identity=id;found->model=model;found->emitter=emitter;found->token=token;found->generation=++generation;found->live=true;
  }
  if(found->emitter!=emitter)return nullptr; // Exactly one native emitter per vehicle lifetime.
  found->distanceSquared=distance;found->player=player;found->eligible=eligible;
  if(eligible){found->seenFrame=frame;found->seenTick=tick;}
  return found;
 }
 bool Promote(Owner& o,std::uint32_t inputBits) noexcept {
  if(!o.live||!o.eligible||o.rank>=Promoted||o.promotedFrame==frame)return false;
  o.promotedFrame=frame;o.inputFrame=frame;o.inputBits=inputBits;return true;
 }
 static int Compare(int unchanged,bool ap,bool bp,unsigned ar,unsigned br,bool aObserved,bool bObserved,
    bool aRelevant,bool bRelevant,bool aOwn,bool bOwn,bool aSpecial,bool bSpecial,bool aVisibleLamp,bool bVisibleLamp) noexcept {
  if(ap==bp){if(ap&&ar!=br&&aObserved&&bObserved&&aRelevant&&bRelevant)return ar<br?0:2;return unchanged;}
  if(!aObserved||!bObserved)return unchanged;
  if((ap?ar:br)==1&&(aOwn||bOwn))return aOwn?0:2;
  if(aOwn||bOwn||aSpecial||bSpecial)return unchanged;
  const unsigned rank=ap?ar:br;if(rank>=Promoted||!(ap?aRelevant:bRelevant))return unchanged;
  // The secondary may yield to any relevant visible lamp; the primary retains
  // the accepted single-siren comparison behavior.
  if(rank==1&&(ap?bVisibleLamp:aVisibleLamp))return ap?2:0;
  return ap?0:2;
 }
};
}
