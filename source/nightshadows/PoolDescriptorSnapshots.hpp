#pragma once
// PRIVATE OFFLINE CANDIDATE. Immediate fresh field snapshots, never saved
// mapping authorization. Fast path requires building membership rejection
// before any owner-body read; otherwise the unchanged validator runs.
namespace traffic_signal::pool_descriptors {
constexpr size_t HeaderExtent=0x1c,MaximumUnion=256;
enum class Attempt { Fallback, Ready, Fault };
struct Header {uintptr_t pool{},storage{},flags{};int32_t size{};bool valid{};};
using Check=bool(*)(uintptr_t,size_t) noexcept;
__declspec(noinline) inline uint32_t NativeWord(uintptr_t address,unsigned offset) noexcept {
 return *reinterpret_cast<const volatile uint32_t*>(address+offset);
}
#ifndef CE_POOL_HEADER_WORD
#define CE_POOL_HEADER_WORD(address,offset) traffic_signal::pool_descriptors::NativeWord(address,offset)
#endif
__declspec(noinline) inline void Snapshot(uintptr_t pool,int32_t stride,Header& out) noexcept {
 out.pool=pool;out.valid=false;
 // Preserve header zero/size/stride short circuits. No copying unused words,
 // descriptor padding or the gap between descriptors.
 const auto size=static_cast<int32_t>(CE_POOL_HEADER_WORD(pool,8));
 if(size<=0||size>65536)return;
 if(static_cast<int32_t>(CE_POOL_HEADER_WORD(pool,12))!=stride)return;
 out.size=size;out.storage=CE_POOL_HEADER_WORD(pool,0);out.flags=CE_POOL_HEADER_WORD(pool,4);out.valid=true;
}
__declspec(noinline) inline bool BuildingRejects(uintptr_t pool,uintptr_t owner) noexcept {
 const auto size=static_cast<int32_t>(CE_POOL_HEADER_WORD(pool,8));
 if(size<=0||size>65536)return true;
 if(static_cast<int32_t>(CE_POOL_HEADER_WORD(pool,12))!=0x70)return true;
 const auto storage=uintptr_t(CE_POOL_HEADER_WORD(pool,0));
 const uint64_t bytes=uint64_t(size)*0x70;
 return owner<storage||uint64_t(owner-storage)>=bytes||(owner-storage)%0x70;
}
__declspec(noinline) inline Attempt RejectedBuildingThenObject(uintptr_t building,uintptr_t object,uintptr_t owner,
 uintptr_t objectGlobal,Header& o) noexcept {
 __try {
  if(!BuildingRejects(building,owner))return Attempt::Fallback;
  if(*reinterpret_cast<const volatile uintptr_t*>(objectGlobal)!=object)return Attempt::Fallback;
  Snapshot(object,0x320,o);return Attempt::Ready;
 }
 __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION||
          GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR||
          GetExceptionCode()==EXCEPTION_GUARD_PAGE?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){
  // Never consume a partial snapshot. A raced protection fault changes original
  // unhandled-fault behavior to validation failure; this requires review.
  o.valid=false;return Attempt::Fault;
 }
}
__declspec(noinline) inline Attempt FreshRejectedBuilding(uintptr_t building,uintptr_t object,uintptr_t owner,
 uintptr_t objectGlobal,Check readable,Header& o) noexcept {
 if(building<0x10000||object<0x10000||building>UINTPTR_MAX-HeaderExtent||object>UINTPTR_MAX-HeaderExtent)return Attempt::Fallback;
 const auto lower=building<object?building:object;
 const auto upper=(building>object?building:object)+HeaderExtent;
 if(upper-lower>MaximumUnion)return Attempt::Fallback;
 // One CURRENT query authorizes a containing contiguous union, followed
 // immediately by minimal scalar reads of both descriptors.
 if(!readable(lower,upper-lower))return Attempt::Fallback;
 return RejectedBuildingThenObject(building,object,owner,objectGlobal,o);
}
}
