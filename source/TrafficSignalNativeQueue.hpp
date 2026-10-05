#pragma once
#include <cstdint>
#include <cstddef>
namespace traffic_signal {
// CE 1.2.0.59: 0x6BD2C0 writes the producer buffer. 0x6C2DD0
// publishes it to the renderer, rotates three buffers and resets this count.
inline constexpr uint32_t SubmissionPointerRva=0xC3EEDC;
inline constexpr uint32_t SubmissionCountRva=0x114CBBC;
inline constexpr uint32_t SubmissionCapacity=0x280;
inline constexpr uint32_t StoredTrafficFlags=0x264;
inline bool StoredPrivateTraffic(uint32_t type,uint32_t flags,uint32_t key,uint32_t reserved) noexcept {
 return type==2&&flags==StoredTrafficFlags&&key==reserved;
}
template<class KeyAt>
uint32_t SubmissionCollision(uint32_t count,uintptr_t producer,uintptr_t completed,
 bool readable,uint32_t reserved,KeyAt keyAt) noexcept {
 if(count>SubmissionCapacity)return 2;
 if(producer<0x10000)return 3;
 // Rotation must leave the producer and completed renderer buffers distinct.
 if(producer==completed)return 5;
 if(!count)return 0;
 if(!readable)return 3;
 for(uint32_t i=0;i<count;++i)if(keyAt(i)==reserved)return 1;
 return 0;
}
}
