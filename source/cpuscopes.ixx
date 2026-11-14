module;

#include <common.hxx>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

export module cpuscopes;

import common;

// CpuScopes: CPU milliseconds a frame of FusionFix's own work, by named scopes timed where they run, on the thread they
// run on (the main thread, the render thread, any other), and the frames' own times. The sampling profiler tells shares
// of samples, and under Wine which function called which only roughly; these are exact, cost two counter reads a scope,
// and nothing at all while no one measures (the debug server's cpu_scopes, a block of frames at a time).
export namespace CpuScopes
{
    // A scope's name, registered once (a static Id where it is timed)
    struct Id
    {
        int index = -1;
        explicit Id(const char* name);
    };

    // True while a measurement runs: a scope reads the counter only then
    inline std::atomic<bool> bOn = false;

    void Add(int index, int64_t ticks);

    // Times what runs from here to the end of the scope into id
    struct Scope
    {
        int index;
        LARGE_INTEGER start;
        explicit Scope(const Id& id) : index(bOn.load(std::memory_order_relaxed) ? id.index : -1)
        {
            if (index >= 0)
                QueryPerformanceCounter(&start);
        }
        ~Scope()
        {
            if (index < 0)
                return;
            LARGE_INTEGER end;
            QueryPerformanceCounter(&end);
            Add(index, end.QuadPart - start.QuadPart);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    struct Row
    {
        std::string name;
        const char* thread;     // "main", "render" or "other"
        double mean = 0.0;      // ms a frame
        double max = 0.0;       // ms in the frame it took most
        double calls = 0.0;     // a frame
    };
    struct Frames
    {
        int count = 0;
        double mean = 0.0, p50 = 0.0, p95 = 0.0, p99 = 0.0, max = 0.0;  // ms between presents
        double mainMean = 0.0, mainMax = 0.0;                          // ms between the main thread's frames
    };
    struct Report
    {
        Frames frames;
        DWORD mainThread = 0, renderThread = 0;
        std::vector<Row> rows;  // the busiest first
    };

    // Measures `frames` presented frames (debug server's thread; blocks, at most timeoutSeconds)
    Report Measure(int frames, int timeoutSeconds);
}

namespace CpuScopes
{
    constexpr int MaxScopes = 128;
    constexpr int Kinds = 3;    // main, render, other
    constexpr const char* KindNames[Kinds] = { "main", "render", "other" };

    std::mutex namesMutex;
    std::string names[MaxScopes];
    std::atomic<int> nScopes = 0;

    std::atomic<DWORD> mainThread = 0, renderThread = 0;

    // This frame's sums, by any thread
    std::atomic<int64_t> frameTicks[MaxScopes][Kinds] = {};
    std::atomic<uint32_t> frameCalls[MaxScopes][Kinds] = {};

    // The measurement's, by the render thread at each present
    struct Sums
    {
        double ticks[MaxScopes][Kinds] = {};
        double maxTicks[MaxScopes][Kinds] = {};
        double calls[MaxScopes][Kinds] = {};
        std::vector<double> frameMs, mainMs;   // mainMs under mainMutex: the main thread adds to it
        void Clear()
        {
            std::fill(&ticks[0][0], &ticks[0][0] + MaxScopes * Kinds, 0.0);
            std::fill(&maxTicks[0][0], &maxTicks[0][0] + MaxScopes * Kinds, 0.0);
            std::fill(&calls[0][0], &calls[0][0] + MaxScopes * Kinds, 0.0);
            frameMs.clear();
            mainMs.clear();
        }
    };
    Sums sums;
    std::mutex mainMutex;
    std::atomic<bool> bResetAsked = false;
    std::atomic<int> nFramesWanted = 0, nFramesDone = 0;
    LARGE_INTEGER lastPresent = {}, lastMainFrame = {};
    double tickMs = 0.0;

    Id::Id(const char* name)
    {
        std::lock_guard lock(namesMutex);
        const int n = nScopes.load();
        for (int i = 0; i < n; ++i)
            if (names[i] == name)
            {
                index = i;
                return;
            }
        if (n >= MaxScopes)
            return;
        names[n] = name;
        index = n;
        nScopes = n + 1;
    }

    void Add(int index, int64_t ticks)
    {
        const DWORD thread = GetCurrentThreadId();
        const int kind = thread == mainThread.load(std::memory_order_relaxed) ? 0
                       : thread == renderThread.load(std::memory_order_relaxed) ? 1 : 2;
        frameTicks[index][kind].fetch_add(ticks, std::memory_order_relaxed);
        frameCalls[index][kind].fetch_add(1, std::memory_order_relaxed);
    }

    // At each present, on the render thread: the frame's sums into the measurement's
    void EndFrame()
    {
        renderThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const bool reset = bResetAsked.exchange(false);
        if (reset)
        {
            std::lock_guard lock(mainMutex);
            sums.Clear();
        }
        const int n = nScopes.load();
        const bool counting = bOn && !reset && nFramesDone < nFramesWanted;
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < Kinds; ++k)
            {
                const double t = double(frameTicks[i][k].exchange(0, std::memory_order_relaxed));
                const double c = double(frameCalls[i][k].exchange(0, std::memory_order_relaxed));
                if (!counting)
                    continue;
                sums.ticks[i][k] += t;
                sums.maxTicks[i][k] = (std::max)(sums.maxTicks[i][k], t);
                sums.calls[i][k] += c;
            }
        if (counting && lastPresent.QuadPart)
        {
            sums.frameMs.push_back(double(now.QuadPart - lastPresent.QuadPart) * tickMs);
            nFramesDone.fetch_add(1);
        }
        lastPresent = now;
    }

    // At each frame of the main thread (CGame::Process)
    void MainFrame()
    {
        mainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        if (!bOn)
        {
            lastMainFrame = {};
            return;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (lastMainFrame.QuadPart && !bResetAsked && nFramesDone < nFramesWanted)
        {
            std::lock_guard lock(mainMutex);
            sums.mainMs.push_back(double(now.QuadPart - lastMainFrame.QuadPart) * tickMs);
        }
        lastMainFrame = now;
    }

    std::mutex measureMutex;

    Report Measure(int frames, int timeoutSeconds)
    {
        std::unique_lock busy(measureMutex, std::try_to_lock);
        if (!busy)
            throw std::runtime_error("a CPU scope measurement is being taken already");
        nFramesWanted = 0;
        nFramesDone = 0;
        bResetAsked = true;
        nFramesWanted = (std::max)(frames, 1);
        bOn = true;
        struct Off { ~Off() { bOn = false; } } off;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
        while (bResetAsked || nFramesDone < nFramesWanted)
        {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("the frames did not come (minimized, paused, loading?)");
            Sleep(10);
        }
        // The render thread adds nothing more once the frames are done
        Report report;
        report.mainThread = mainThread;
        report.renderThread = renderThread;
        const double count = double(nFramesDone.load());
        auto ms = sums.frameMs;
        std::sort(ms.begin(), ms.end());
        auto at = [&](double share) { return ms.empty() ? 0.0 : ms[(std::min)(size_t(share * double(ms.size())), ms.size() - 1)]; };
        report.frames.count = int(ms.size());
        for (double v : ms)
            report.frames.mean += v / double(ms.size());
        report.frames.p50 = at(0.5);
        report.frames.p95 = at(0.95);
        report.frames.p99 = at(0.99);
        report.frames.max = ms.empty() ? 0.0 : ms.back();
        {
            std::lock_guard lock(mainMutex);
            const auto& main = sums.mainMs;
            for (double v : main)
            {
                report.frames.mainMean += v / double(main.size());
                report.frames.mainMax = (std::max)(report.frames.mainMax, v);
            }
        }
        const int n = nScopes.load();
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < Kinds; ++k)
                if (sums.calls[i][k] > 0.0)
                {
                    std::lock_guard lock(namesMutex);
                    report.rows.push_back({ names[i], KindNames[k], sums.ticks[i][k] * tickMs / count, sums.maxTicks[i][k] * tickMs,
                                            sums.calls[i][k] / count });
                }
        std::sort(report.rows.begin(), report.rows.end(), [](const Row& a, const Row& b) { return a.mean > b.mean; });
        return report;
    }
}

class CpuScopesFeature
{
public:
    CpuScopesFeature()
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        CpuScopes::tickMs = 1000.0 / double(frequency.QuadPart);
        FusionFix::onBeforePresent() += []() { CpuScopes::EndFrame(); };
        FusionFix::onGameProcessEvent() += []() { CpuScopes::MainFrame(); };
    }
} CpuScopesFeature;
