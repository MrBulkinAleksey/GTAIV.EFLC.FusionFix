// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include <array>
#include <cstdint>

namespace fusionfix::shadows {
struct ShadowLookupSnapshot {
    std::array<std::uint32_t, 8> dynamicKeys{};
    std::array<std::uint8_t, 8> dynamicActive{};
    std::array<int, 8> dynamicCache = [] { std::array<int,8> a{};a.fill(-1);return a; }();
    std::array<std::uint32_t, 8> staticKeys{};
};
// A cache index is a location, not an identity. A recycled cache entry must
// never shadow a different light. Returning -1 selects native unshadowed light.
inline int ValidateShadowLookup(int nativeResult, std::uint32_t key, int cache,
                                const ShadowLookupSnapshot& slots) noexcept {
    if (nativeResult <= 0 || key == 0 || key == UINT32_MAX) return nativeResult;
    if (nativeResult < 8) {
        const int dependency=slots.dynamicCache[nativeResult];
        const bool validDependency=dependency==-1 ||
            (dependency>=0 && dependency<8 && slots.staticKeys[dependency]==key);
        if (slots.dynamicActive[nativeResult] == 1 && slots.dynamicKeys[nativeResult] == key && validDependency)
            return nativeResult;
        if (cache >= 0 && cache < 8 && slots.staticKeys[cache] == key) return cache + 8;
        return -1;
    }
    if (nativeResult < 16 && slots.staticKeys[nativeResult - 8] == key) return nativeResult;
    return -1;
}
}
