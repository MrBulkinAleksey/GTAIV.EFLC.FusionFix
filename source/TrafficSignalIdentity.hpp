#pragma once
#include <cstdint>
namespace traffic_signal {
struct Identity {
 uint32_t pool{},storage{},flags{},owner{},slot{},reference{},model{};
 friend bool operator==(const Identity&,const Identity&)=default;
};
class Lifetime {
public:
 Identity identity{}; bool bound{},retired{}; uint32_t lastFrame{},phaseMask{};
 bool Bind(Identity value,uint32_t frame) noexcept {
  if(bound||retired||!value.pool||!value.storage||!value.flags||!value.owner||!value.model)return false;
  identity=value;bound=true;lastFrame=frame;return true;
 }
 bool Observe(Identity value,uint32_t frame,uint32_t phase) noexcept {
  if(!bound||retired)return false;
  if(!(identity==value)){retired=true;return false;}
  // Check before refreshing; a late producer cannot hide a missed frame span.
  if(uint32_t(frame-lastFrame)>2){retired=true;return false;}
  lastFrame=frame;if(phase<3)phaseMask|=1u<<phase;return true;
 }
 bool CheckGap(uint32_t frame) noexcept {
  if(bound&&!retired&&uint32_t(frame-lastFrame)>2)retired=true;
  return retired;
 }
 // Only the separately authenticated native timer-restore callback may rebase.
 // Ordinary backward values still fail the original gap checks. Pool identity
 // must be freshly validated and a retired lifetime can never resume.
 bool RebaseNativeTimer(Identity current,uint32_t frame,uint32_t savedFrame,bool backupActive) noexcept {
  if(!bound||retired||!(current==identity)||!backupActive||frame!=savedFrame||
     uint32_t(frame-lastFrame)<=0x80000000u)return false;
  lastFrame=frame;return true;
 }
 void Retire() noexcept {retired=true;}
};
}
