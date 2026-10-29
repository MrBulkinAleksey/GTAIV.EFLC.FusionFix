#pragma once
#include "TrafficSignalRegistry.hpp"
namespace traffic_signal {
// Observe must never populate this cache before successful owner validation.
// A malformed callback would otherwise change the first player read of a frame.
struct PlayerPositionSnapshot {
 uint32_t frame=UINT32_MAX;bool valid=false;float position[3]{};
};
template<class AllocatorEnabled,class TracksOwner>
bool RejectUntrackedFar(uint32_t owner,uint32_t frame,const float* light,float reach,
 const PlayerPositionSnapshot& cached,AllocatorEnabled&& enabled,TracksOwner&& tracked) noexcept {
 if(!cached.valid||cached.frame!=frame||!light||!std::isfinite(reach)||reach<=0)return false;
 for(unsigned i=0;i<3;++i)if(!std::isfinite(light[i])||!std::isfinite(cached.position[i]))return false;
 if(Nearby(light,cached.position,reach)||!enabled())return false;
 // A mapped retired owner also needs the original identity/reuse handling.
 // This CPU lookup reads no native pool or owner memory.
 return !tracked(owner);
}
}
