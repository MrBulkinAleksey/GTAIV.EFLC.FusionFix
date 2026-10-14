#pragma once
#include <array>
#include <cstdint>

namespace fusionfix::shadows {
// Ranking history only: never store/dereference a light pointer or shadow map.
// Survive a brief native selection gap without forgetting the incumbent and
// promoting its replacement to an equal incumbent on the very next frame.
class NativeLampContinuity41 {
    struct Entry { std::uint64_t key{},generation{}; std::uint32_t lastSelected{},lastFrame{}; };
    std::array<Entry,7> entries_{};
    std::uintptr_t session_{};
    std::uint32_t time_{},frame_{};
public:
    static constexpr std::uint32_t GraceMs=120,GraceFrames=8;
    void Begin(std::uintptr_t session,std::uint32_t frame,std::uint32_t time) noexcept {
        if(session_!=session || frame<frame_ || time-time_>2000) entries_={};
        session_=session;frame_=frame;time_=time;
        for(auto& e:entries_) if(e.key && (time-e.lastSelected>GraceMs || frame-e.lastFrame>GraceFrames))e={};
    }
    bool Retained(std::uint64_t key,std::uint64_t generation,bool relevant) noexcept {
        for(auto& e:entries_) if(e.key==key && key) {
            if(e.generation!=generation || !relevant){e={};return false;}
            return true;
        }
        return false;
    }
    void Selected(std::uint64_t key,std::uint64_t generation) noexcept {
        if(!key)return;
        for(auto& e:entries_)if(e.key==key){e={key,generation,time_,frame_};return;}
        // Do not displace a still-live incumbent's short lease with a one-frame
        // challenger. Expired entries were removed by Begin.
        for(auto& e:entries_)if(!e.key){e={key,generation,time_,frame_};return;}
    }
};
}
