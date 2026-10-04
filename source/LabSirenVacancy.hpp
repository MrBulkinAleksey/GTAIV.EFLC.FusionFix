#pragma once
#include <array>
#include <cmath>
#include <cstdint>
namespace lab_siren {
struct VacancyEntry {std::uint32_t key{};std::uint64_t generation{};bool protectedPriority{};};
class Vacancy {
 std::array<VacancyEntry,6> retained_{};bool armed_=false;std::uint32_t frame_=0,tick_=0;
public:
 int Filter(std::uint32_t frame,std::uint32_t tick,bool valid,bool enabled,bool on,bool known,float input,
            std::uint32_t privateKey,const std::array<VacancyEntry,7>& slots) noexcept {
  if(!valid||!enabled||!on){armed_=false;return -1;}
  unsigned count=0;bool hasPrivate=false;for(const auto& e:slots){if(e.key)++count;if(e.key==privateKey)hasPrivate=true;}
  if(hasPrivate){armed_=false;if(count==7){unsigned j=0;for(const auto& e:slots)if(e.key!=privateKey)retained_[j++]=e;armed_=j==6;frame_=frame;tick_=tick;}return -1;}
  if(!armed_||frame-frame_>12||tick-tick_>120){armed_=false;return -1;}
  if(count!=7||!known||!std::isfinite(input)||input<0||input>=0.001f)return -1;
  // All six actual incumbents must still be selected. Only the one new borrower yields.
  for(const auto& old:retained_){unsigned matches=0;for(const auto& e:slots)if(e.key==old.key&&e.generation==old.generation)++matches;if(matches!=1)return -1;}
  int newcomer=-1;for(unsigned i=0;i<7;++i){bool old=false;for(const auto& e:retained_)if(e.key==slots[i].key&&e.generation==slots[i].generation)old=true;if(!old){if(newcomer!=-1||slots[i].protectedPriority)return -1;newcomer=i;}}
  return newcomer;
 }
};
}
