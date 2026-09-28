// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include <array>
#include <cstdint>
namespace fusionfix::shadows {
// Rendering may consume the previous submitted frame. Selection membership at
// the moment of drawing is not proof of which source produced that light.
// Keep only opaque, recently accepted headlight identities; never dereference.
class SubmittedHeadlightHistory {
    struct Entry { std::uintptr_t key{}; std::uint32_t frame{}; };
    std::array<Entry,64> entries{};
public:
    void Reset() noexcept { entries={}; }
    void Record(std::uintptr_t key,std::uint32_t frame) noexcept {
        if(!key) return;
        std::size_t target=0;
        for(std::size_t i=0;i<entries.size();++i) {
            if(entries[i].key==key || !entries[i].key) { target=i;break; }
            if(frame-entries[i].frame > frame-entries[target].frame) target=i;
        }
        entries[target]={key,frame};
    }
    bool Contains(std::uintptr_t key,std::uint32_t frame) const noexcept {
        if(!key) return false;
        for(const auto& entry:entries)
            if(entry.key==key && frame-entry.frame<=2) return true;
        return false;
    }
};
}
