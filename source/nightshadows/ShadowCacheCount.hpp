#pragma once
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
}
