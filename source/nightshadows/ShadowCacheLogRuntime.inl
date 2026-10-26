// NightShadowsCacheLog: what the eight static lamp caches do while you move, to tell the two causes of lamp shadows
// coming and going apart. Either the entries are too few (a lamp still in view loses its entry to a nearer one and
// gets it back later), or they are made too slowly (one new entry a selection run, and a lamp with flag 2 gets no
// slot until its entry is baked). Each selection run compares the entries with the last run's; a key that changed
// is a new entry, the one it replaced an eviction. Lines go to GTAIV.EFLC.FusionFix.NightShadows.CacheLog.log:
//   new     a lamp got an entry: its distance as the game measures it, how long it waited without one, and what it
//           replaced (key, the game's last distance of it, still_listed = that lamp is still among this run's
//           lights, its distance from you, regained = it had an entry until that many ms ago)
//   summary every 5 s: runs, new entries, evictions of lamps still listed (and of those within 30 m), lamps that
//           got their entry back within 10 s, the most lamps waiting for an entry in one run (all, within 30 m,
//           with flag 4 too = without any shadow while waiting), the nearest waiting one, the wait times
namespace PlayerShadowAllocation::CacheLog
{
    static bool enabled = false;

    struct Entry { uint32_t key{}; int32_t state{-1}; float distance{}; };
    struct Waiting { uint64_t since{}; uint64_t seen{}; };
    struct Evicted { uint64_t at{}; };

    // Selection (render thread) only
    static std::array<Entry, 8> last{};
    static bool haveLast = false;
    static std::unordered_map<uint32_t, Waiting> waiting;
    static std::unordered_map<uint32_t, Evicted> evicted;

    // Shared with the writer
    static std::atomic_flag lock = ATOMIC_FLAG_INIT;
    static std::string lines;
    static struct Window
    {
        uint32_t runs{}, created{}, evictions{}, evictedListed{}, evictedNear{}, regained{};
        uint32_t waitingMost{}, waitingNearMost{}, waitingBlockedMost{};
        float nearestWaiting = -1.0f;
        uint64_t waitSum{}, waitMost{}; uint32_t waits{};
    } window{};

    struct Guard
    {
        Guard() noexcept { while (lock.test_and_set(std::memory_order_acquire)) {} }
        ~Guard() { lock.clear(std::memory_order_release); }
    };

    static std::array<Entry, 8> Read() noexcept
    {
        std::array<Entry, 8> entries{};
        for (unsigned i = 0; i < entries.size(); ++i)
        {
            const auto entry = gameBase + allocation::CacheAge0Rva + i * 0x100;
            entries[i] = {*reinterpret_cast<const uint32_t*>(entry + 4), *reinterpret_cast<const int32_t*>(entry + 8),
                          *reinterpret_cast<const float*>(entry - 0xC)};
        }
        return entries;
    }

    static float FromPlayer(const rage::CLightSource& l) noexcept
    {
        const float x = l.mPosition.x - state.player.x, y = l.mPosition.y - state.player.y, z = l.mPosition.z - state.player.z;
        return std::sqrt(x * x + y * y + z * z);
    }

    static const rage::CLightSource* Find(const rage::CLightSource* lights, uint32_t count, uint32_t key) noexcept
    {
        for (uint32_t i = 0; i < count; ++i)
            if (static_cast<uint32_t>(lights[i].mCastShadows) == key && (lights[i].mFlags & rage::LF_STATIC_SHADOW))
                return &lights[i];
        return nullptr;
    }

    // After the game's selection, depth 1, on its thread
    static void Observe() noexcept
    {
        if (!enabled || unsupportedThread.load(std::memory_order_relaxed)) return;
        const auto* lights = CurrentLights();
        const uint32_t count = CurrentCount();
        if (reinterpret_cast<uintptr_t>(lights) < 0x10000 || count > 4096) return;
        try
        {
            const uint64_t now = GetTickCount64();
            const auto entries = Read();
            const float nearReach = 30.0f;

            // Lamps that want an entry and have none
            uint32_t waitingAll = 0, waitingNear = 0, waitingBlocked = 0;
            float nearest = -1.0f;
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto& l = lights[i];
                if (!(l.mFlags & rage::LF_STATIC_SHADOW) || (l.mFlags & rage::LF_VEHICLE) || l.mShadowCacheIndex != -1) continue;
                const uint32_t key = static_cast<uint32_t>(l.mCastShadows);
                auto& w = waiting[key];
                if (!w.since || now - w.seen > 2000) w.since = now;
                w.seen = now;
                const float d = FromPlayer(l);
                ++waitingAll;
                if (d <= nearReach) ++waitingNear;
                if ((l.mFlags & rage::LF_DYNAMIC_SHADOW) && d <= nearReach) ++waitingBlocked;
                if (nearest < 0 || d < nearest) nearest = d;
            }

            std::string text;
            uint32_t created = 0, evictions = 0, listed = 0, nearEvicted = 0, regained = 0, waits = 0;
            uint64_t waitSum = 0, waitMost = 0;
            for (unsigned i = 0; haveLast && i < entries.size(); ++i)
            {
                const auto& entry = entries[i];
                const auto& was = last[i];
                if (entry.key == was.key || !entry.key) continue;
                ++created;
                uint64_t waited = 0;
                if (auto it = waiting.find(entry.key); it != waiting.end())
                {
                    waited = now - it->second.since;
                    waiting.erase(it);
                    ++waits; waitSum += waited; waitMost = (std::max)(waitMost, waited);
                }
                int64_t back = -1;
                if (auto it = evicted.find(entry.key); it != evicted.end())
                {
                    back = static_cast<int64_t>(now - it->second.at);
                    if (back <= 10000) ++regained;
                    evicted.erase(it);
                }
                text += std::format("new entry={} key={:08X} dist={:.1f} waited={}", i, entry.key, entry.distance, waited);
                if (back >= 0) text += std::format(" regained={}", back);
                if (was.key)
                {
                    ++evictions;
                    evicted[was.key] = {now};
                    text += std::format(" | replaced key={:08X} state={} dist={:.1f}", was.key, was.state, was.distance);
                    if (const auto* old = Find(lights, count, was.key))
                    {
                        const float d = FromPlayer(*old);
                        ++listed;
                        if (d <= nearReach) ++nearEvicted;
                        text += std::format(" still_listed from_you={:.1f} flags={:X}", d, old->mFlags);
                    }
                }
                text += '\n';
            }
            last = entries;
            haveLast = true;

            // Forget lamps not seen for a while
            if (waiting.size() > 512 || evicted.size() > 512)
            {
                std::erase_if(waiting, [now](const auto& w) { return now - w.second.seen > 5000; });
                std::erase_if(evicted, [now](const auto& e) { return now - e.second.at > 10000; });
            }

            const Guard guard;
            auto& s = window;
            ++s.runs;
            s.created += created; s.evictions += evictions; s.evictedListed += listed; s.evictedNear += nearEvicted;
            s.regained += regained;
            s.waitingMost = (std::max)(s.waitingMost, waitingAll);
            s.waitingNearMost = (std::max)(s.waitingNearMost, waitingNear);
            s.waitingBlockedMost = (std::max)(s.waitingBlockedMost, waitingBlocked);
            if (nearest >= 0 && (s.nearestWaiting < 0 || nearest < s.nearestWaiting)) s.nearestWaiting = nearest;
            s.waits += waits; s.waitSum += waitSum; s.waitMost = (std::max)(s.waitMost, waitMost);
            if (lines.size() < (1u << 20)) lines += text;
        }
        catch (...) { enabled = false; }
    }

    // The game's thread
    static void Flush() noexcept
    {
        if (!enabled) return;
        static uint64_t lastLines = 0, lastSummary = 0;
        const uint64_t now = GetTickCount64();
        if (now - lastLines < 1000) return;
        lastLines = now;
        try
        {
            std::string text;
            Window s{};
            const bool summary = now - lastSummary >= 5000;
            {
                const Guard guard;
                text.swap(lines);
                if (summary) { s = window; window = {}; }
            }
            if (summary)
            {
                lastSummary = now;
                text += std::format("summary runs={} new={} evicted={} evicted_listed={} evicted_listed_within_30m={} regained_within_10s={} "
                    "waiting_most={} waiting_within_30m_most={} waiting_within_30m_with_flag4_most={} nearest_waiting={:.1f} "
                    "wait_avg_ms={} wait_most_ms={}\n",
                    s.runs, s.created, s.evictions, s.evictedListed, s.evictedNear, s.regained,
                    s.waitingMost, s.waitingNearMost, s.waitingBlockedMost, s.nearestWaiting,
                    s.waits ? s.waitSum / s.waits : 0, s.waitMost);
            }
            if (!text.empty()) FusionLog::WriteText("NightShadows.CacheLog", "Cache", text);
        }
        catch (...) {}
    }
}
