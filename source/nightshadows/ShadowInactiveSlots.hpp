// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace fusionfix::shadows {
// Native lookup searches buffered identities without testing the active byte.
// Keep slot zero (static refresh) and every active dynamic record untouched.
inline unsigned ClearInactiveShadowKeys(void* records, std::size_t bytes) noexcept {
    if(!records || bytes < 0x880) return 0;
    auto* p=static_cast<std::uint8_t*>(records); unsigned cleared=0;
    for(unsigned slot=1;slot<=7;++slot) {
        auto* entry=p+slot*0x110;
        std::uint32_t key; std::memcpy(&key,entry+0xF8,4);
        if(entry[0xED]==0 && key) {
            key=0; std::memcpy(entry+0xF8,&key,4); ++cleared;
        }
    }
    return cleared;
}
}
