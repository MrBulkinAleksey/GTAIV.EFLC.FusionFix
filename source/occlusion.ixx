module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

export module occlusion;

import common;
import comvars;
import framehistory;

// Occlusion culling against the depth already drawn (Hi-Z). The game leaves out of its scene only what is outside the
// view (and interiors behind closed portals; its rmOccluderSystem is never used), so in the city every building, prop
// and car behind the block in front is drawn into the G-buffer, each with its draw calls on the render thread.
//
// - The GPU side (BuildHiZ in postfx.ixx, HiZ.fx) takes the farthest depth of every 4 x 4 pixels of the world at the
//   end of the G-buffer pass, again until the grid is a few hundred texels wide, and reads it back two or three
//   frames later, when the GPU is done with it (an event query; it never waits). Cars and people count as nothing in
//   the way, they move.
// - Publish takes it on the render thread with the camera it was drawn with, and builds its levels of 2 x 2 texels.
// - The game adds each entity to the lists of the phases it is drawn in through one function (CE 0xae43a0: phases,
//   entity, distance, bucket). There, an entity of the scene (the phase of type 0x1f, CE 0x159af24) is tested: its
//   bounding sphere (vtable +0x50 the centre, +0x58 the radius), grown by how far the camera has moved since the grid
//   was drawn, is projected with that frame's camera, and is hidden when its nearest point lies behind the farthest
//   depth of every texel it covers. Off that frame's screen, too near, or the camera moved too far: drawn.
//   Hidden HiZOcclusionFrames frames running, the scene's phase is taken off it (HiZOcclusion = 1); its shadows,
//   reflections, LOD bookkeeping and fade are the game's as before, those are kept in the lists' callers.
// - HiZOcclusion = 2 tests and counts only: the log says what would have been left out.
export namespace Occlusion
{
    // HiZOcclusion: 0 off, 1 hidden entities left out of the scene, 2 tested and counted only
    int nMode = 2;
    bool Enabled() { return nMode != 0; }

    // Render thread, a frame's grid read back: the farthest view depth (metres) of each texel, rows pitchBytes apart;
    // viewProjection the camera it was drawn with (rows, clip = [x y z 1] * M, no jitter), position the camera's, and
    // scaleU / scaleV how many texels of the grid one width / height of the screen takes (a texel covers 4^n pixels
    // where the screen took no whole number of them: the last one is cut short)
    void Publish(const uint8_t* texels, uint32_t pitchBytes, int width, int height, float scaleU, float scaleV,
                 const float viewProjection[16], const float position[3], float nearPlane, uint32_t renderFrame);
}

namespace Occlusion
{
    constexpr int MaxLevels = 12;
    float fMaxMove = 4.0f;          // HiZOcclusionMaxMove: metres the camera may have moved since the grid was drawn
    float fMoveGrowth = 1.0f;       // each metre moved grows the sphere by this much
    int nFrames = 2;                // HiZOcclusionFrames: hidden this many frames running before it's left out
    float fMinRadius = 0.0f;        // HiZOcclusionMinRadius: smaller entities are always drawn

    struct Grid
    {
        int levels = 0;
        int width[MaxLevels]{}, height[MaxLevels]{};
        std::vector<float> data[MaxLevels];
        float scaleU = 0.0f, scaleV = 0.0f;
        float viewProjection[16]{};
        float position[3]{};
        float nearPlane = 0.0f;
        float wLength = 1.0f;       // length of the w column's xyz: w of a point moves by this per metre along the view
        uint32_t renderFrame = 0;
        uint64_t published = 0;     // GetTickCount64

        float At(int level, int x, int y) const { return data[level][size_t(y) * width[level] + x]; }
    };

    std::mutex latestMutex;
    std::shared_ptr<const Grid> latest;
    std::atomic<uint32_t> publishedCount{ 0 };

    uint32_t* pScenePhase = nullptr;    // CE 0x159af24
    float* pCamera = nullptr;           // CE 0x128e340, the game camera's position (main thread)

    // The game's frame (counted in onGameProcessEvent); the rest belongs to the thread that builds the lists: the frame
    // it last saw, the grid taken for it, and what each entity was found
    std::atomic<uint32_t> gameFrame{ 1 };
    DWORD gameThread = 0;
    uint32_t mainFrame = 0;
    std::shared_ptr<const Grid> grid;
    struct EntityState
    {
        uint32_t frame = 0;
        uint16_t hiddenRunning = 0;
        bool hidden = false;    // this frame's test
        bool left = false;      // left out of the scene this frame
    };
    std::unordered_map<uintptr_t, EntityState> entities;

    enum Outcome : uint8_t { Visible, Hidden, NoGrid, Stale, Moved, NearCamera, OffScreen, Small, Outcomes };
    constexpr const char* OutcomeNames[Outcomes] = { "visible", "hidden", "no grid", "grid too old", "camera moved too far",
                                                     "too near", "off the grid's screen", "too small" };
    struct Stats
    {
        uint32_t frames = 0;
        uint32_t entities = 0;          // tested, once a frame each
        uint32_t outcomes[Outcomes]{};
        uint32_t props = 0, propsHidden = 0;
        uint32_t left = 0;              // left out (or would be, mode 2)
        uint32_t adds = 0, addsLeft = 0; // calls into the lists with the scene's phase, and of those left out
        uint32_t grids = 0;             // grids taken
        uint64_t ageFrames = 0;         // render frames from the grid's to the render thread's when taken, summed
        double testMs = 0.0;
        double maxFrameMs = 0.0;
    } stats;
    DWORD hookThread = 0;
    std::atomic<uint32_t> otherThreads{ 0 };
    double frameMs = 0.0;
    LARGE_INTEGER qpcFrequency{};

    void Publish(const uint8_t* texels, uint32_t pitchBytes, int width, int height, float scaleU, float scaleV,
                 const float viewProjection[16], const float position[3], float nearPlane, uint32_t renderFrame)
    {
        if (!texels || width <= 0 || height <= 0)
            return;
        auto g = std::make_shared<Grid>();
        g->width[0] = width;
        g->height[0] = height;
        g->data[0].resize(size_t(width) * height);
        for (int y = 0; y < height; ++y)
            memcpy(&g->data[0][size_t(y) * width], texels + size_t(y) * pitchBytes, size_t(width) * sizeof(float));
        // Each level the farthest of 2 x 2 of the one before; an odd last row or column is cut short
        int level = 0;
        while (level + 1 < MaxLevels && (g->width[level] > 1 || g->height[level] > 1))
        {
            const int w = g->width[level], h = g->height[level];
            const int nw = (w + 1) / 2, nh = (h + 1) / 2;
            auto& next = g->data[level + 1];
            next.resize(size_t(nw) * nh);
            const auto& src = g->data[level];
            for (int y = 0; y < nh; ++y)
            {
                const float* row0 = &src[size_t(2 * y) * w];
                const float* row1 = &src[size_t(std::min(2 * y + 1, h - 1)) * w];
                for (int x = 0; x < nw; ++x)
                {
                    const int x1 = std::min(2 * x + 1, w - 1);
                    next[size_t(y) * nw + x] = std::max(std::max(row0[2 * x], row0[x1]), std::max(row1[2 * x], row1[x1]));
                }
            }
            ++level;
            g->width[level] = nw;
            g->height[level] = nh;
        }
        g->levels = level + 1;
        g->scaleU = scaleU;
        g->scaleV = scaleV;
        memcpy(g->viewProjection, viewProjection, sizeof(g->viewProjection));
        memcpy(g->position, position, sizeof(g->position));
        g->nearPlane = nearPlane;
        const float* m = viewProjection;
        g->wLength = std::sqrt(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
        g->renderFrame = renderFrame;
        g->published = GetTickCount64();
        {
            std::lock_guard lock(latestMutex);
            latest = std::move(g);
        }
        publishedCount.fetch_add(1, std::memory_order_release);
    }

    // The sphere against the grid, as above
    Outcome Test(const Grid& g, const float centre[3], float radius)
    {
        if (radius < fMinRadius)
            return Small;
        float move = 0.0f;
        if (pCamera)
        {
            const float dx = pCamera[0] - g.position[0], dy = pCamera[1] - g.position[1], dz = pCamera[2] - g.position[2];
            move = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (move > fMaxMove)
                return Moved;
        }
        const float r = radius + move * fMoveGrowth;
        const float* m = g.viewProjection;
        float clip[4];
        for (int c = 0; c < 4; ++c)
            clip[c] = centre[0] * m[c] + centre[1] * m[4 + c] + centre[2] * m[8 + c] + m[12 + c];
        const float nearest = clip[3] - r * g.wLength;
        if (nearest <= g.nearPlane * 2.0f)
            return NearCamera;

        // The screen rectangle of the sphere's bounding box, all eight corners in front of the camera
        float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
        for (int corner = 0; corner < 8; ++corner)
        {
            const float sx = (corner & 1) ? r : -r, sy = (corner & 2) ? r : -r, sz = (corner & 4) ? r : -r;
            const float x = clip[0] + sx * m[0] + sy * m[4] + sz * m[8];
            const float y = clip[1] + sx * m[1] + sy * m[5] + sz * m[9];
            const float w = clip[3] + sx * m[3] + sy * m[7] + sz * m[11];
            if (w <= g.nearPlane)
                return NearCamera;
            const float u = x / w * 0.5f + 0.5f, v = 0.5f - y / w * 0.5f;
            minU = std::min(minU, u);
            maxU = std::max(maxU, u);
            minV = std::min(minV, v);
            maxV = std::max(maxV, v);
        }
        if (minU < 0.0f || minV < 0.0f || maxU > 1.0f || maxV > 1.0f)
            return OffScreen;

        // The level where it covers no more than 4 x 4 texels
        const float x0 = minU * g.scaleU, x1 = maxU * g.scaleU, y0 = minV * g.scaleV, y1 = maxV * g.scaleV;
        int level = 0, tx0 = 0, tx1 = 0, ty0 = 0, ty1 = 0;
        for (;; ++level)
        {
            const float scale = 1.0f / float(1 << level);
            tx0 = std::clamp(int(x0 * scale), 0, g.width[level] - 1);
            tx1 = std::clamp(int(x1 * scale), 0, g.width[level] - 1);
            ty0 = std::clamp(int(y0 * scale), 0, g.height[level] - 1);
            ty1 = std::clamp(int(y1 * scale), 0, g.height[level] - 1);
            if ((tx1 - tx0 < 4 && ty1 - ty0 < 4) || level + 1 >= g.levels)
                break;
        }
        for (int y = ty0; y <= ty1; ++y)
            for (int x = tx0; x <= tx1; ++x)
                if (g.At(level, x, y) >= nearest)
                    return Visible;
        return Hidden;
    }

    bool IsProp(uintptr_t entity)
    {
        constexpr uint32_t propFlags = 0x10000 | 0x40000000;
        return (*(uint32_t*)(entity + 0x24) & propFlags) == propFlags;
    }

    void EndFrame();

    // The lists' entry: the phases to add the entity to
    uint32_t Phases(uintptr_t entity, uint32_t phases)
    {
        if (!pScenePhase || !entity)
            return phases;
        const uint32_t scene = *pScenePhase;
        if (!(phases & scene))
            return phases;

        // All of this belongs to one thread: were the lists built on others too, those are counted and drawn
        const DWORD thread = GetCurrentThreadId();
        if (!hookThread)
            hookThread = thread;
        if (thread != hookThread)
        {
            otherThreads.fetch_add(1, std::memory_order_relaxed);
            return phases;
        }
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        ++stats.adds;

        // A new frame: the last one counted, and a new grid if there's one
        if (const uint32_t frame = gameFrame.load(std::memory_order_relaxed); frame != mainFrame)
        {
            if (mainFrame)
                EndFrame();
            mainFrame = frame;
            {
                std::lock_guard lock(latestMutex);
                grid = latest;
            }
            if (grid)
            {
                ++stats.grids;
                stats.ageFrames += FrameHistory::Frame() - grid->renderFrame;
            }
        }

        auto& state = entities[entity];
        if (state.frame != mainFrame)
        {
            // Once a frame: the entity may be added to several buckets
            const bool repeat = state.frame + 1 == mainFrame;
            state.frame = mainFrame;
            Outcome outcome = NoGrid;
            if (grid && GetTickCount64() - grid->published > 500)
                outcome = Stale;
            else if (grid)
            {
                alignas(16) float centre[4]{};
                auto vtable = *(void***)entity;
                reinterpret_cast<void(__thiscall*)(uintptr_t, float*)>(vtable[0x50 / 4])(entity, centre);
                const float radius = reinterpret_cast<float(__thiscall*)(uintptr_t)>(vtable[0x58 / 4])(entity);
                outcome = std::isfinite(radius) && std::isfinite(centre[0] + centre[1] + centre[2]) ? Test(*grid, centre, radius) : Visible;
            }
            state.hidden = outcome == Hidden;
            state.hiddenRunning = state.hidden ? uint16_t(std::min<int>(repeat ? state.hiddenRunning + 1 : 1, 0xFFFF)) : 0;
            state.left = state.hidden && state.hiddenRunning >= nFrames;
            ++stats.entities;
            ++stats.outcomes[outcome];
            if (IsProp(entity))
            {
                ++stats.props;
                stats.propsHidden += state.hidden;
            }
            stats.left += state.left;
        }
        if (state.left)
        {
            ++stats.addsLeft;
            if (nMode == 1)
                phases &= ~scene;
        }

        QueryPerformanceCounter(&end);
        frameMs += double(end.QuadPart - start.QuadPart) * 1000.0 / double(qpcFrequency.QuadPart);
        return phases;
    }

    // The lists' thread, at the first entity of the next frame
    void EndFrame()
    {
        ++stats.frames;
        stats.testMs += frameMs;
        stats.maxFrameMs = std::max(stats.maxFrameMs, frameMs);
        frameMs = 0.0;

        if (stats.frames < 600)
            return;
        // Entities not seen for a while go, so a freed one's address starts afresh
        for (auto it = entities.begin(); it != entities.end();)
            it = mainFrame - it->second.frame > 60 ? entities.erase(it) : std::next(it);
        const auto& s = stats;
        const double perFrame = 1.0 / double(s.frames);
        std::string outcomes;
        for (int i = 0; i < Outcomes; ++i)
            if (s.outcomes[i])
                outcomes += std::format("{}{} {:.0f}", outcomes.empty() ? "" : ", ", OutcomeNames[i], s.outcomes[i] * perFrame);
        FusionLog::Write("Occlusion", "Stats",
            "%s, per frame over %u frames: %.0f entities of the scene tested (%s); %.0f props, %.0f of them hidden; "
            "%.0f %s (%.1f%%), %.0f of %.0f list adds; test %.3f ms a frame, worst %.3f; grids published %u, "
            "%.1f render frames old when taken; "
            "threads: lists %lu, game %lu, adds from other threads %u; %zu entities kept\n",
            nMode == 1 ? "left out" : "counted only", s.frames, s.entities * perFrame, outcomes.c_str(), s.props * perFrame,
            s.propsHidden * perFrame, s.left * perFrame, nMode == 1 ? "left out" : "would be left out",
            s.entities ? 100.0 * s.left / s.entities : 0.0, s.addsLeft * perFrame, s.adds * perFrame, s.testMs * perFrame,
            s.maxFrameMs, publishedCount.load(), s.grids ? double(s.ageFrames) / s.grids : 0.0, hookThread, gameThread, otherThreads.exchange(0), entities.size());
        stats = {};
    }
}

class OcclusionFeature
{
public:
    OcclusionFeature()
    {
        using namespace Occlusion;

        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            nMode = std::clamp(iniReader.ReadInteger("MISC", "HiZOcclusion", 2), 0, 2);
            fMaxMove = std::clamp(iniReader.ReadFloat("MISC", "HiZOcclusionMaxMove", 4.0f), 0.0f, 50.0f);
            nFrames = std::clamp(iniReader.ReadInteger("MISC", "HiZOcclusionFrames", 2), 1, 60);
            fMinRadius = std::clamp(iniReader.ReadFloat("MISC", "HiZOcclusionMinRadius", 0.0f), 0.0f, 100.0f);
            QueryPerformanceFrequency(&qpcFrequency);
            if (!nMode)
                return;

            // The scene's phase: test [0x159af24], edx in the render list after the shadow phases are dropped (CE 0xae7076)
            auto scene = hook::pattern("89 4C 24 28 A9 00 00 00 08");
            // The lists' entry (CE 0xae43a0): sub esp, 0x10; push ebx; mov ebx, [esp+0x18] the phases
            auto add = hook::pattern("83 EC 10 53 8B 5C 24 18 85 DB 0F 84 ? ? ? ? 33 C9 8B C3 85 DB 78");
            auto camera = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ? 56 8B 75 0C");
            if (scene.empty() || add.empty())
            {
                nMode = 0;
                FusionLog::Write("Occlusion", "Settings", "the render lists weren't found, nothing is tested\n");
                return;
            }
            pScenePhase = *scene.get_first<uint32_t*>(0xD);
            if (!camera.empty())
                pCamera = *camera.get_first<float*>(12);
            // At the entry, before anything is pushed: [esp+4] the phases, [esp+8] the entity
            static auto AddHook = safetyhook::create_mid(add.get_first(0), [](SafetyHookContext& regs)
            {
                auto args = (uint32_t*)(regs.esp + 4);
                args[0] = Phases(uintptr_t(args[1]), args[0]);
            });
            FusionLog::Write("Occlusion", "Settings", "%s: hidden %d frames running, camera moved at most %.1f m, radius from %.1f m\n",
                nMode == 1 ? "hidden entities are left out of the scene" : "hidden entities are counted only", nFrames, fMaxMove, fMinRadius);
        };

        FusionFix::onGameProcessEvent() += []()
        {
            if (!gameThread)
                gameThread = GetCurrentThreadId();
            gameFrame.fetch_add(1, std::memory_order_relaxed);
        };
    }
} OcclusionFeature;
