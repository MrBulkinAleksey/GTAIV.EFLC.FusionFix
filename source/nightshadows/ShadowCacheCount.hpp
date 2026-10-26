#pragma once

namespace fusionfix::shadows {
// The static lamp shadow caches in use: the game's 8, or 16 once LampShadowCaches has patched the game for them
// (ShadowCache16Runtime.inl). Its records, their per-buffer copies and the lookups hold 16 in any case.
inline constexpr unsigned MaxCacheEntries = 16;
inline unsigned cacheEntries = 8;
}
