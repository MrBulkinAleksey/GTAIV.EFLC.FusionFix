// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ShadowTrace34 {
struct Event {
 std::uint32_t stage,frame,tick,key;
 std::int32_t a,b,c,d;
 float x,y,z,w;
};
static_assert(sizeof(Event)==48);
// Fixed-size, nonblocking producer buffer. Contention/overflow is counted,
// never silently presented as an absence of rendering activity.
class Recorder {
public:
 static constexpr unsigned Capacity=65536;
 bool Push(Event e) noexcept {
  if(lock_.test_and_set(std::memory_order_acquire)){++dropped;return false;}
  const unsigned h=(e.key*2654435761u+e.stage)%last_.size();
  if(valid_[h]&&!std::memcmp(&last_[h],&e,sizeof(e))){lock_.clear(std::memory_order_release);return true;}
  // Clock samples differ within a frame; compare the semantic payload too.
  auto prior=last_[h];prior.tick=e.tick;
  if(valid_[h]&&!std::memcmp(&prior,&e,sizeof(e))){lock_.clear(std::memory_order_release);return true;}
  if(count_==Capacity){++dropped;lock_.clear(std::memory_order_release);return false;}
  last_[h]=e;valid_[h]=true;events_[count_++]=e;
  lock_.clear(std::memory_order_release);return true;
 }
 unsigned Drain(Event* output,unsigned capacity) noexcept {
  if(!output||capacity<Capacity||lock_.test_and_set(std::memory_order_acquire))return 0;
  const auto n=count_;std::memcpy(output,events_.data(),n*sizeof(Event));count_=0;
  lock_.clear(std::memory_order_release);return n;
 }
 std::atomic<std::uint32_t> dropped{0};
private:
 std::atomic_flag lock_=ATOMIC_FLAG_INIT;
 std::array<Event,Capacity> events_{};
 std::array<Event,512> last_{};
 std::array<bool,512> valid_{};
 unsigned count_=0;
};
inline Recorder recorder;
inline std::array<Event,Recorder::Capacity> drainBuffer{};
inline std::atomic<bool> enabled{false};
inline std::array<std::atomic<std::uint32_t>,256> tracked{};
inline std::atomic<unsigned> nextTracked{0};
inline std::filesystem::path outputPath;
inline std::uint64_t written=0;
inline std::atomic_flag flushLock=ATOMIC_FLAG_INIT;
inline constexpr std::uint64_t MaximumBytes=64ull*1024*1024;
inline void Track(std::uint32_t key) noexcept {
 if(!key||!enabled.load(std::memory_order_relaxed))return;
 for(auto& v:tracked)if(v.load(std::memory_order_relaxed)==key)return;
 tracked[nextTracked.fetch_add(1,std::memory_order_relaxed)%tracked.size()].store(key,std::memory_order_relaxed);
}
inline bool Tracked(std::uint32_t key) noexcept {
 if(!key||!enabled.load(std::memory_order_relaxed))return false;
 for(auto& v:tracked)if(v.load(std::memory_order_relaxed)==key)return true;
 return false;
}
inline void Emit(Event e) noexcept {if(enabled.load(std::memory_order_relaxed))recorder.Push(e);}
inline bool Start(const std::filesystem::path& path,unsigned mode,unsigned pid) noexcept {
 try {
  outputPath=path;
  const std::uint32_t header[]{0x34335453,1,32,1,mode,pid,sizeof(Event),0};
  std::ofstream out(path,std::ios::binary|std::ios::trunc);
  out.write(reinterpret_cast<const char*>(header),sizeof(header));out.flush();
  written=sizeof(header);enabled.store(out.good(),std::memory_order_release);return out.good();
 }catch(...){return false;}
}
// Called only from the existing low-frequency diagnostic writer, never from
// a light-selection/lookup/render hook. The file is capped at64MiB per launch.
inline void Flush() noexcept {
 if(!enabled.load(std::memory_order_acquire)||flushLock.test_and_set(std::memory_order_acquire))return;
 const auto count=recorder.Drain(drainBuffer.data(),drainBuffer.size());
 try {
  const auto bytes=std::uint64_t(count)*sizeof(Event);
  if(written+bytes>MaximumBytes)enabled=false;
  else if(count){std::ofstream out(outputPath,std::ios::binary|std::ios::app);out.write(reinterpret_cast<const char*>(drainBuffer.data()),bytes);out.flush();
   if(out.good())written+=bytes;else enabled=false;}
 }catch(...){enabled=false;}
 flushLock.clear(std::memory_order_release);
}
}
