#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>

#include "FusionLog.hpp"

namespace fusionfix
{
    // A diagnostics writer for one component of a feature's log (FusionLog: GTAIV.EFLC.FusionFix.<Feature>.log
    // next to the plugin), called from the game-process callback and written at most every few seconds.
    // A status (std::ios::trunc) is written only when it changed; lines (std::ios::app) every time.
    // A failed write never lets an exception into game code.
    class DiagnosticsLog
    {
    public:
        std::atomic<bool> ready{false};

        void Name(const char* featureName, const char* componentName) noexcept
        {
            feature = featureName;
            component = componentName;
        }

        // Calls write(out, tick) into a buffer once the interval since the last write passed.
        template <typename Writer>
        void Write(std::ios::openmode mode, Writer&& write) noexcept
        {
            if (!ready.load(std::memory_order_acquire) || !feature) return;
            const uint64_t now = GetTickCount64();
            if (now - last < IntervalMs) return;
            last = now;
            try
            {
                std::ostringstream out;
                write(out, now);
                std::string text = out.str();
                if (mode & std::ios::trunc)
                {
                    if (text == lastStatus) return;
                    lastStatus = text;
                }
                FusionLog::WriteText(feature, component, text);
            }
            catch (...) {}
        }

    private:
        static constexpr uint64_t IntervalMs = 5000;
        uint64_t last = 0;
        const char* feature = nullptr;
        const char* component = "";
        std::string lastStatus;
    };
}
