#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace fusionfix
{
    // A diagnostics file next to the ini, written from the game-process callback at most every
    // few seconds. A failed write never lets an exception into game code.
    class DiagnosticsLog
    {
    public:
        std::filesystem::path path;
        std::atomic<bool> ready{false};

        // Calls write(out, tick) with the file open once the interval since the last write passed.
        template <typename Writer>
        void Write(std::ios::openmode mode, Writer&& write) noexcept
        {
            if (!ready.load(std::memory_order_acquire) || path.empty()) return;
            const uint64_t now = GetTickCount64();
            if (now - last < IntervalMs) return;
            last = now;
            try
            {
                std::ofstream out(path, mode);
                write(out, now);
            }
            catch (...) {}
        }

    private:
        static constexpr uint64_t IntervalMs = 5000;
        uint64_t last = 0;
    };
}
