#pragma once
#include "TrafficSignalIdentity.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <cmath>
namespace traffic_signal {
enum class End : uint32_t { None, Free, Identity, Off, Clock, Collision, Thread };
enum class Visit : uint32_t { Rejected, New, Continuous, LeaseRestart, Retired, Capacity };
inline bool Nearby(const float* light,const float* player,float reach) noexcept {
 if(!light||!player||!std::isfinite(reach)||reach<=0)return false;
 float distance=0;for(unsigned i=0;i<3;++i){if(!std::isfinite(light[i])||!std::isfinite(player[i]))return false;
  const auto delta=light[i]-player[i];distance+=delta*delta;}
 return std::isfinite(distance)&&distance<=reach*reach;
}
// Keys belong to physical pool lifetimes, never phases, positions or render slots.
// The fixed CPU bookkeeping capacity does not enlarge native rendering pools.
template<size_t Capacity=16384> class Registry {
 static_assert(Capacity&&Capacity<0x100000&&!(Capacity&(Capacity-1)));
 static constexpr uint32_t Empty=0,Tombstone=UINT32_MAX;
 std::array<uint32_t,Capacity*2> owners_{};
 uint32_t base_{},used_{};bool disabled_{};
 static uint32_t Hash(uint32_t owner) noexcept {
  return (owner^0x811c9dc5u)*16777619u;
 }
 uint32_t FindCell(uint32_t pool,uint32_t owner,bool insertion) const noexcept {
  const uint32_t start=Hash(owner)&(owners_.size()-1);uint32_t tomb=UINT32_MAX;
  for(uint32_t i=0;i<owners_.size();++i){const auto cell=(start+i)&(owners_.size()-1),value=owners_[cell];
   if(value==Empty)return insertion&&tomb!=UINT32_MAX?tomb:cell;
   if(value==Tombstone){if(tomb==UINT32_MAX)tomb=cell;continue;}
   const auto& e=entries[value-1];if(e.id.owner==owner)return cell;
  }return insertion?tomb:UINT32_MAX;
 }
public:
 struct Entry {
  Identity id{};uint32_t key{},generation{},lastFrame{},lastAdmission{},phase{},phaseMask{};
  bool retired{},leased{},submitted{};End reason=End::None;
 };
 std::array<Entry,Capacity> entries{};
 struct Result {Visit visit=Visit::Rejected;uint32_t index=UINT32_MAX,replaced=UINT32_MAX;};
 bool Configure(uint32_t base,uint32_t otherKey) noexcept {
  const uint64_t limit=uint64_t(base)+Capacity*64;
  if(base_||disabled_||base<0x10000||(base&63)||limit>UINT32_MAX||
     (otherKey>=base&&uint64_t(otherKey)<limit)){disabled_=true;return false;}
  base_=base;return true;
 }
 uint32_t Used() const noexcept{return used_;}
 bool Disabled() const noexcept{return disabled_;}
 bool TracksOwner(uint32_t owner) const noexcept {
  const auto cell=FindCell(0,owner,false);if(cell==UINT32_MAX)return false;
  const auto value=owners_[cell];return value&&value!=Tombstone&&entries[value-1].id.owner==owner;
 }
 uint32_t IndexOfKey(uint32_t key) const noexcept {
  if(key<base_||(key-base_)&63)return UINT32_MAX;
  const auto index=(key-base_)/64;return base_&&index<used_?index:UINT32_MAX;
 }
 uint32_t FindOwner(uint32_t pool,uint32_t owner) const noexcept {
  const auto cell=FindCell(pool,owner,false);if(cell==UINT32_MAX)return UINT32_MAX;
  const auto value=owners_[cell];return value&&value!=Tombstone&&entries[value-1].id.pool==pool?value-1:UINT32_MAX;
 }
 bool Retire(uint32_t index,End reason) noexcept {
  if(index>=used_)return false;auto& e=entries[index];
  if(e.retired)return false;e.retired=true;e.leased=false;e.reason=reason;return true;
 }
 uint32_t Free(uint32_t pool,uint32_t owner) noexcept {
  const auto cell=FindCell(pool,owner,false);if(cell==UINT32_MAX)return UINT32_MAX;
  const auto value=owners_[cell];if(!value||value==Tombstone)return UINT32_MAX;
  const auto index=value-1;if(entries[index].id.pool!=pool)return UINT32_MAX;
  Retire(index,End::Free);owners_[cell]=Tombstone;return index;
 }
 void Disable(End reason=End::Off) noexcept {
  disabled_=true;for(uint32_t i=0;i<used_;++i)Retire(i,reason);
 }
 bool Suspend(uint32_t index) noexcept {
  if(index>=used_||entries[index].retired||!entries[index].leased)return false;
  entries[index].leased=false;return true;
 }
 Result Observe(Identity id,uint32_t frame,uint32_t phase,bool relevant) noexcept {
  if(disabled_||!base_||phase>2||!id.pool||!id.storage||!id.flags||!id.owner||!id.model)return {};
  uint32_t replaced=UINT32_MAX,index=UINT32_MAX;
  const auto existing=FindCell(id.pool,id.owner,false);
  if(existing!=UINT32_MAX&&owners_[existing]&&owners_[existing]!=Tombstone)index=owners_[existing]-1;
  if(index!=UINT32_MAX&&!(entries[index].id==id)){
   replaced=index;Retire(index,End::Identity);
   const auto cell=FindCell(id.pool,id.owner,false);owners_[cell]=Tombstone;index=UINT32_MAX;
  }
  if(index==UINT32_MAX){
   if(!relevant)return {Visit::Rejected,UINT32_MAX,replaced};
   if(used_==Capacity)return {Visit::Capacity,UINT32_MAX,replaced};
   const auto cell=FindCell(id.pool,id.owner,true);if(cell==UINT32_MAX)return {Visit::Capacity,UINT32_MAX,replaced};
   index=used_++;auto& e=entries[index];e.id=id;e.key=base_+index*64;e.generation=index+1;
   e.lastFrame=frame;e.phase=phase;e.phaseMask=1u<<phase;e.leased=true;owners_[cell]=index+1;
   return {Visit::New,index,replaced};
  }
  auto& e=entries[index];if(e.retired)return {Visit::Retired,index,replaced};
  const auto age=uint32_t(frame-e.lastFrame);
  if(age>=0x80000000u){Retire(index,End::Clock);return {Visit::Retired,index,replaced};}
  const bool restart=!e.leased||age>2;e.lastFrame=frame;e.phase=phase;e.phaseMask|=1u<<phase;
  e.leased=relevant;
  return {restart&&relevant?Visit::LeaseRestart:Visit::Continuous,index,replaced};
 }
 bool Apply(uint32_t index,Identity current,uint32_t frame,uint32_t caller,uint32_t type,
  uint32_t phase,bool collision,uint32_t& flags,uint32_t& nativeKey) noexcept {
  if(disabled_||index>=used_)return false;auto& e=entries[index];
  if(!(e.id==current)){Retire(index,End::Identity);return false;}
  if(e.retired||!e.leased||e.lastFrame!=frame||caller!=0x921029||
     type!=2||phase>2||flags!=0x200||nativeKey)return false;
  if(e.submitted&&e.lastAdmission==frame)return false;
  if(collision){Retire(index,End::Collision);return false;}
  flags|=4;nativeKey=e.key;e.submitted=true;e.lastAdmission=frame;return true;
 }
 bool Rebase(uint32_t index,Identity current,uint32_t restored,uint32_t saved,bool active) noexcept {
  if(disabled_||index>=used_)return false;auto& e=entries[index];
  if(e.retired||!(e.id==current)||!active||restored!=saved||uint32_t(restored-e.lastFrame)<=0x80000000u)return false;
  e.lastFrame=restored;e.leased=false;return true;
 }
};
}
