#pragma once
#include <atomic>
#include <cstdint>

namespace fusionfix::shadows {
// The static lamp shadow caches in use: the game's 8, 16 once LampShadowCaches has patched the game for them, or 32
// with their records moved into the plugin (ShadowCache16Runtime.inl). The game holds 16 records at 0x11A0B10 and
// their per-buffer copies at 0x119D0F0, 0x1000 a buffer.
inline constexpr unsigned MaxCacheEntries = 32;
inline unsigned cacheEntries = 8;
inline uintptr_t cacheRecordsAt = 0;   // record 0 when moved, 0: the game's
inline uintptr_t cacheCopiesAt = 0;    // buffer 0's copy of record 0 when moved
inline uintptr_t cacheCopyStride = 0x1000;

// Record i (0x100 bytes: +0xC0 its tile, +0xDC age, +0xE0 key, +0xE4 state, +0xE8 light, +0xEC count, -0xC from
// +0xDC its distance), gameBase the image's base
inline uintptr_t CacheRecord(uintptr_t gameBase, unsigned i) noexcept
{
    return (cacheRecordsAt ? cacheRecordsAt : gameBase + 0xDA0B10) + i * 0x100;
}
// The copy of record i for a buffer (0 or 1), as lighting reads it
inline uintptr_t CacheCopy(uintptr_t gameBase, int buffer, unsigned i) noexcept
{
    return (cacheCopiesAt ? cacheCopiesAt : gameBase + 0xD9D0F0) + buffer * cacheCopyStride + i * 0x100;
}

// What each cache's tile holds: the key of the lamp it was last baked for and the rectangle it was drawn into (a
// BakeRect), set where the bake's rewarp draws the tile (ShadowCache16's RewarpQuad, LampShadowCaches 32). An entry
// takes its new key and, moved from a quarter tile to a whole one, its new rectangle at once, but is baked there only
// a run or more later: until then its tile holds another lamp's map, or another entry's, and a lamp lit from it threw
// shadows where none could be, coming and going as you drove.
inline bool bakeTracked = false;
inline std::atomic<uint32_t> bakedKey[MaxCacheEntries]{};
inline std::atomic<uint32_t> bakedRect[MaxCacheEntries]{};

// A tile rectangle (x, y, 0, size as shares of the atlas) in twentieths: tiles and quarter tiles fall on them
inline uint32_t BakeRect(const float* rect) noexcept
{
    const auto twentieths = [](float v) { return static_cast<uint32_t>(v * 20.0f + 0.5f) & 0xFF; };
    return twentieths(rect[0]) | twentieths(rect[1]) << 8 | twentieths(rect[3]) << 16 | 1u << 24;
}

// Whether cache i's tile holds the map baked for key at rect; always, while the bakes are not tracked
inline bool BakedFor(unsigned i, uint32_t key, const float* rect) noexcept
{
    if (!bakeTracked) return true;
    if (i >= MaxCacheEntries) return false;
    return bakedKey[i].load(std::memory_order_acquire) == key && bakedRect[i].load(std::memory_order_relaxed) == BakeRect(rect);
}
}
