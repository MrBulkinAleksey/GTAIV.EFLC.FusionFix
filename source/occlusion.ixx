module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

export module occlusion;

import common;
import comvars;
import framehistory;
import inireload;
import coronas;

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
    int nMode = 1;
    // LampShadowsOcclusion: lights whose whole reach is hidden draw no shadow map (nightshadows)
    bool bLampShadows = true;
    // HiZOccluderCategories: a bit per material category (the material ID less its 128 and 8 bits, as WetGround takes
    // it) whose pixels hide what is behind them; the rest count as nothing in the way
    int nOccluderCategories = 1;
    // CoronasOcclusion: 0 off, 1 the game camera's coronas hidden behind the world are given no quad, 2 counted only
    int nCoronas = 1;
    // The grid is built while anything tests against it
    bool Enabled() { return nMode != 0 || bLampShadows || nCoronas != 0; }

    // The latest grid with what this moment needs to test against it: how far the camera has moved and turned since.
    // Taken once for a batch of tests, on any thread.
    struct Grid;
    struct Probe
    {
        std::shared_ptr<const Grid> grid;
        float bandU = 1.0f, bandV = 1.0f;   // how far the screen slid past the grid's edges with the camera's turn
        float move = 0.0f;                  // metres the camera moved
        bool usable = false;                // a grid, recent, the camera not moved too far
    };
    Probe Take();
    // The sphere lies wholly behind the world drawn into the grid
    bool SphereHidden(const Probe& probe, const float centre[3], float radius);

    // Render thread, a frame's grid read back: the farthest view depth (metres) of each texel, rows pitchBytes apart;
    // viewProjection the camera it was drawn with (rows, clip = [x y z 1] * M, no jitter), position the camera's, and
    // scaleU / scaleV how many texels of the grid one width / height of the screen takes (a texel covers 4^n pixels
    // where the screen took no whole number of them: the last one is cut short)
    void Publish(const uint8_t* texels, uint32_t pitchBytes, int width, int height, float scaleU, float scaleV,
                 const float viewProjection[16], const float position[3], float nearPlane, float farPlane, uint32_t renderFrame);
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
        float forward[3]{};         // where the camera looked: the w column, normalised
        float turnU = 0.0f, turnV = 0.0f; // how far the screen slides, in widths / heights, per radian the camera turns
        uint32_t renderFrame = 0;
        uint64_t published = 0;     // GetTickCount64

        float At(int level, int x, int y) const { return data[level][size_t(y) * width[level] + x]; }
    };

    std::mutex latestMutex;
    std::shared_ptr<const Grid> latest;
    std::atomic<uint32_t> publishedCount{ 0 };
    // What the grids held, summed over the texels of their first level: nothing in the way (other materials than the
    // world's), sky (at the far plane), the world within 50 m
    std::atomic<uint64_t> texelsAll{ 0 }, texelsNothing{ 0 }, texelsSky{ 0 }, texelsNear{ 0 };

    uint32_t* pScenePhase = nullptr;    // CE 0x159af24
    float* pCamera = nullptr;           // CE 0x128e340, the game camera's position (main thread)

    // The game's frame (counted in onGameProcessEvent); the rest belongs to the thread that builds the lists: the frame
    // it last saw, the grid taken for it, and what each entity was found
    std::atomic<uint32_t> gameFrame{ 1 };
    DWORD gameThread = 0;
    uint32_t mainFrame = 0;
    Probe frameProbe;           // taken at the first entity of each frame
    uint8_t frameOutcome = 0;   // its Precheck
    struct EntityState
    {
        uint32_t frame = 0;
        uint16_t hiddenRunning = 0;
        bool hidden = false;    // this frame's test
        bool left = false;      // left out of the scene this frame
    };
    // Each entity's state by its address, open addressing: the lists add some 2000 a frame, each looked up once a call
    struct Slot
    {
        uintptr_t entity = 0;
        EntityState state;
    };
    std::vector<Slot> table(1 << 14);
    size_t tableUsed = 0;
    void Rebuild(size_t capacity)
    {
        std::vector<Slot> old(capacity);
        old.swap(table);
        tableUsed = 0;
        const size_t mask = table.size() - 1;
        for (const auto& slot : old)
        {
            // Entities not seen for a while go, so a freed one's address starts afresh
            if (!slot.entity || mainFrame - slot.state.frame > 60)
                continue;
            size_t i = (slot.entity >> 3) * 2654435761u & mask;
            while (table[i].entity)
                i = (i + 1) & mask;
            table[i] = slot;
            ++tableUsed;
        }
    }
    EntityState& StateOf(uintptr_t entity)
    {
        size_t mask = table.size() - 1;
        size_t i = (entity >> 3) * 2654435761u & mask;
        while (table[i].entity)
        {
            if (table[i].entity == entity)
                return table[i].state;
            i = (i + 1) & mask;
        }
        if (tableUsed + 1 > table.size() * 3 / 4)
        {
            Rebuild(table.size());
            if (tableUsed + 1 > table.size() / 2)
                Rebuild(table.size() * 2);
            return StateOf(entity);
        }
        table[i].entity = entity;
        table[i].state = {};
        ++tableUsed;
        return table[i].state;
    }

    // This frame: how far the screen slid past the edges of the grid's screen with the camera's turn since, in widths
    // and heights. Up to EdgeBand, a sphere reaching out of the grid's screen is tested on its part inside, the edge
    // standing for the band (what lies beyond is off the screen now); turned farther, it is drawn
    constexpr float EdgeBand = 0.03f;

    enum Outcome : uint8_t { Visible, Hidden, NoGrid, Stale, Moved, NearCamera, OffScreen, Small, Outcomes };
    constexpr const char* OutcomeNames[Outcomes] = { "visible", "hidden", "no grid", "grid too old", "camera moved too far",
                                                     "too near", "off the grid's screen", "too small" };
    struct Stats
    {
        uint32_t frames = 0;
        uint32_t entities = 0;          // tested, once a frame each
        uint32_t outcomes[Outcomes]{};
        uint32_t props = 0, propsHidden = 0;
        uint32_t hiddenAtEdge = 0;      // hidden, though reaching out of the grid's screen
        uint32_t boxes = 0, boxHidden = 0, boxSphereHidden = 0; // tested by the model's box, hidden so, and by the sphere
        uint32_t boxByMatrix = 0, boxByHeading = 0, boxByHeadingSwapped = 0, boxMismatch = 0; // how the box was placed
        uint32_t left = 0;              // left out (or would be, mode 2)
        uint32_t adds = 0, addsLeft = 0; // calls into the lists with the scene's phase, and of those left out
        uint32_t grids = 0;             // grids taken
        uint64_t ageFrames = 0;         // render frames from the grid's to the render thread's when taken, summed
    } stats;
    DWORD hookThread = 0;
    std::atomic<uint32_t> otherThreads{ 0 };
    uint64_t frameTicks = 0, maxFrameTicks = 0, ticks = 0; // __rdtsc: the test's time this frame, worst, this block
    LARGE_INTEGER windowQpc{};
    uint64_t windowTsc = 0;
    LARGE_INTEGER qpcFrequency{};

    void Publish(const uint8_t* texels, uint32_t pitchBytes, int width, int height, float scaleU, float scaleV,
                 const float viewProjection[16], const float position[3], float nearPlane, float farPlane, uint32_t renderFrame)
    {
        if (!texels || width <= 0 || height <= 0)
            return;
        auto g = std::make_shared<Grid>();
        g->width[0] = width;
        g->height[0] = height;
        g->data[0].resize(size_t(width) * height);
        for (int y = 0; y < height; ++y)
            memcpy(&g->data[0][size_t(y) * width], texels + size_t(y) * pitchBytes, size_t(width) * sizeof(float));
        uint64_t nothing = 0, sky = 0, nearby = 0;
        for (float depth : g->data[0])
        {
            nothing += depth >= 1.0e8f;
            sky += depth < 1.0e8f && depth >= farPlane * 0.9f;
            nearby += depth < 50.0f;
        }
        texelsAll.fetch_add(g->data[0].size(), std::memory_order_relaxed);
        texelsNothing.fetch_add(nothing, std::memory_order_relaxed);
        texelsSky.fetch_add(sky, std::memory_order_relaxed);
        texelsNear.fetch_add(nearby, std::memory_order_relaxed);
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
        for (int i = 0; i < 3; ++i)
            g->forward[i] = m[3 + 4 * i] / g->wLength;
        g->turnU = 0.5f * std::sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]) / g->wLength;
        g->turnV = 0.5f * std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]) / g->wLength;
        g->renderFrame = renderFrame;
        g->published = GetTickCount64();
        {
            std::lock_guard lock(latestMutex);
            latest = std::move(g);
        }
        publishedCount.fetch_add(1, std::memory_order_release);
    }

    // A grid published longer ago than this: the game stopped drawing, or the GPU fell behind
    constexpr uint64_t StaleMs = 500;

    Probe MakeProbe(std::shared_ptr<const Grid> grid)
    {
        Probe p;
        p.grid = std::move(grid);
        if (!p.grid || GetTickCount64() - p.grid->published > StaleMs)
            return p;
        const auto& g = *p.grid;
        if (pCamera)
        {
            const float dx = pCamera[0] - g.position[0], dy = pCamera[1] - g.position[1], dz = pCamera[2] - g.position[2];
            p.move = std::sqrt(dx * dx + dy * dy + dz * dz);
            // The camera's turn since: where it looks now, the second row of its matrix (CE 0x128e320)
            const float* f = pCamera - 8;
            const float length = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (length > 0.5f)
            {
                const float cosTurn = (f[0] * g.forward[0] + f[1] * g.forward[1] + f[2] * g.forward[2]) / length;
                const float turn = std::acos(std::clamp(cosTurn, -1.0f, 1.0f));
                p.bandU = turn * g.turnU + 0.002f;
                p.bandV = turn * g.turnV + 0.002f;
            }
        }
        p.usable = pCamera && p.move <= fMaxMove;
        return p;
    }

    // Whether the probe can tell anything at all: Visible when it can
    Outcome Precheck(const Probe& p)
    {
        if (!p.grid)
            return NoGrid;
        if (GetTickCount64() - p.grid->published > StaleMs)
            return Stale;
        if (p.move > fMaxMove)
            return Moved;
        return Visible;
    }

    // A screen rectangle (shares of the grid's screen) against the grid: hidden when nearest (w) lies behind the
    // farthest depth of every texel it covers
    Outcome TestRect(const Probe& p, float minU, float maxU, float minV, float maxV, float nearest, bool* atEdge)
    {
        const Grid& g = *p.grid;
        // Beyond the band it's off the screen now; within, the grid's edge stands for it
        if (p.bandU > EdgeBand || p.bandV > EdgeBand)
        {
            if (minU < 0.0f || minV < 0.0f || maxU > 1.0f || maxV > 1.0f)
                return OffScreen;
        }
        if (atEdge)
            *atEdge = minU < 0.0f || minV < 0.0f || maxU > 1.0f || maxV > 1.0f;
        minU = std::max(minU, 0.0f);
        minV = std::max(minV, 0.0f);
        maxU = std::min(maxU, 1.0f);
        maxV = std::min(maxV, 1.0f);

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

    // A projected box (its eight corners' clip x, y, w) against the grid: hidden when its nearest point lies behind
    // the farthest depth of every texel its screen rectangle covers
    Outcome TestProjected(const Probe& p, const float (&clips)[8][3], float nearest, bool* atEdge)
    {
        const Grid& g = *p.grid;
        if (nearest <= g.nearPlane * 2.0f)
            return NearCamera;
        float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
        for (const auto& c : clips)
        {
            if (c[2] <= g.nearPlane)
                return NearCamera;
            const float u = c[0] / c[2] * 0.5f + 0.5f, v = 0.5f - c[1] / c[2] * 0.5f;
            minU = std::min(minU, u);
            maxU = std::max(maxU, u);
            minV = std::min(minV, v);
            maxV = std::max(maxV, v);
        }
        return TestRect(p, minU, maxU, minV, maxV, nearest, atEdge);
    }

    // A sphere, by its bounding box along the world's axes, grown by how far the camera moved; after Precheck
    Outcome Test(const Probe& p, const float centre[3], float radius, bool* atEdge = nullptr)
    {
        if (radius < fMinRadius)
            return Small;
        const Grid& g = *p.grid;
        const float r = radius + p.move * fMoveGrowth;
        const float* m = g.viewProjection;
        float clip[4];
        for (int c = 0; c < 4; ++c)
            clip[c] = centre[0] * m[c] + centre[1] * m[4 + c] + centre[2] * m[8 + c] + m[12 + c];
        float clips[8][3];
        for (int corner = 0; corner < 8; ++corner)
        {
            const float sx = (corner & 1) ? r : -r, sy = (corner & 2) ? r : -r, sz = (corner & 4) ? r : -r;
            clips[corner][0] = clip[0] + sx * m[0] + sy * m[4] + sz * m[8];
            clips[corner][1] = clip[1] + sx * m[1] + sy * m[5] + sz * m[9];
            clips[corner][2] = clip[3] + sx * m[3] + sy * m[7] + sz * m[11];
        }
        // The sphere's nearest point, nearer the truth than its box's nearest corner
        return TestProjected(p, clips, clip[3] - r * g.wLength, atEdge);
    }

    // A box of the model (its corners boxMin, boxMax) placed by axes (right, forward, up, unit length) and origin,
    // grown on each side by how far the camera moved; after Precheck
    Outcome TestBox(const Probe& p, const float axes[3][3], const float origin[3], const float boxMin[3], const float boxMax[3],
                    float radius, bool* atEdge = nullptr)
    {
        if (radius < fMinRadius)
            return Small;
        const Grid& g = *p.grid;
        const float grow = p.move * fMoveGrowth;
        const float* m = g.viewProjection;
        float clips[8][3];
        float nearest = 1e30f;
        for (int corner = 0; corner < 8; ++corner)
        {
            const float local[3] = { (corner & 1) ? boxMax[0] + grow : boxMin[0] - grow, (corner & 2) ? boxMax[1] + grow : boxMin[1] - grow,
                                     (corner & 4) ? boxMax[2] + grow : boxMin[2] - grow };
            float world[3];
            for (int i = 0; i < 3; ++i)
                world[i] = origin[i] + local[0] * axes[0][i] + local[1] * axes[1][i] + local[2] * axes[2][i];
            for (int c = 0; c < 3; ++c)
            {
                const int col = c == 2 ? 3 : c;
                clips[corner][c] = world[0] * m[col] + world[1] * m[4 + col] + world[2] * m[8 + col] + m[12 + col];
            }
            nearest = std::min(nearest, clips[corner][2]);
        }
        return TestProjected(p, clips, nearest, atEdge);
    }

    Probe Take()
    {
        std::shared_ptr<const Grid> g;
        {
            std::lock_guard lock(latestMutex);
            g = latest;
        }
        return MakeProbe(std::move(g));
    }

    bool SphereHidden(const Probe& probe, const float centre[3], float radius)
    {
        return probe.usable && Precheck(probe) == Visible && std::isfinite(radius) &&
               std::isfinite(centre[0] + centre[1] + centre[2]) && Test(probe, centre, radius) == Hidden;
    }

    // The coronas the game camera's pass would draw (coronas.ixx, render thread), by their index into the list
    // (0x20 bytes each: position, +0xc size, +0x14 how far the vertex shader moves it toward the camera): those whose
    // quad lies wholly behind the world drawn are taken out. A corona's own vertex shader hides it behind the depth
    // anyway, so only the CPU's work on its quad goes: from a river bank most of the city's distant lights are behind
    // its first row of buildings. One projected point a corona, its quad's ball (1.5 times its size and half a metre,
    // as the view cone takes it, and the move toward the camera, grown by the camera's move) a square of texels
    uint64_t coronaPasses = 0, coronasTested = 0, coronasHidden = 0, coronaTicks = 0;
    void CullCoronas(std::vector<uint32_t>& drawn, const uint8_t* list)
    {
        if (!nCoronas)
            return;
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        const Probe p = Take();
        size_t kept = drawn.size();
        if (p.usable && Precheck(p) == Visible)
        {
            const Grid& g = *p.grid;
            const float* m = g.viewProjection;
            // How far u and v move per metre across the view at w = 1
            const float perMetreU = g.turnU, perMetreV = g.turnV;
            const float grow = p.move * fMoveGrowth;
            // Most are a texel or two of the first level, well inside the screen: read there straight away, as
            // TestRect would; the rest go through it (the profile had the test cost more than half the quads it saved)
            const float* level0 = g.data[0].data();
            const int width0 = g.width[0], height0 = g.height[0];
            const float scaleU = g.scaleU, scaleV = g.scaleV;
            kept = 0;
            for (size_t k = 0; k < drawn.size(); ++k)
            {
                const uint32_t index = drawn[k];
                const uint8_t* item = list + size_t(index) * 0x20;
                const float* pos = (const float*)item;
                const float r = 1.5f * std::fabs(*(const float*)(item + 0xC)) + 0.5f + grow;
                const float toward = std::fabs(*(const float*)(item + 0x14));
                const float x = pos[0] * m[0] + pos[1] * m[4] + pos[2] * m[8] + m[12];
                const float y = pos[0] * m[1] + pos[1] * m[5] + pos[2] * m[9] + m[13];
                const float w = pos[0] * m[3] + pos[1] * m[7] + pos[2] * m[11] + m[15];
                const float nearest = w - (r + toward) * g.wLength;
                bool hidden = false;
                if (nearest > g.nearPlane * 2.0f && std::isfinite(x + y + w))
                {
                    const float inverse = 1.0f / w;
                    const float u = x * inverse * 0.5f + 0.5f, v = 0.5f - y * inverse * 0.5f;
                    const float du = r * perMetreU * g.wLength * inverse, dv = r * perMetreV * g.wLength * inverse;
                    const float cx = u * scaleU, cy = v * scaleV, ex = du * scaleU, ey = dv * scaleV;
                    if (ex < 1.5f && ey < 1.5f && cx - ex >= 0.0f && cy - ey >= 0.0f && cx + ex < scaleU && cy + ey < scaleV)
                    {
                        const int tx0 = int(cx - ex), tx1 = std::min(int(cx + ex), width0 - 1);
                        const int ty0 = int(cy - ey), ty1 = std::min(int(cy + ey), height0 - 1);
                        hidden = true;
                        for (int ty = ty0; ty <= ty1 && hidden; ++ty)
                        {
                            const float* row = level0 + size_t(ty) * width0;
                            for (int tx = tx0; tx <= tx1; ++tx)
                                if (row[tx] >= nearest)
                                {
                                    hidden = false;
                                    break;
                                }
                        }
                    }
                    else
                        hidden = TestRect(p, u - du, u + du, v - dv, v + dv, nearest, nullptr) == Hidden;
                }
                coronasHidden += hidden;
                if (!hidden || nCoronas == 2)
                    drawn[kept++] = index;
            }
            coronasTested += drawn.size();
        }
        drawn.resize(kept);
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        coronaTicks += uint64_t(end.QuadPart - start.QuadPart);
        if (++coronaPasses >= 600)
        {
            const double perPass = 1.0 / double(coronaPasses);
            FusionLog::Write("Occlusion", "Coronas", "%s, per pass of the game camera over %llu: %.0f tested, %.0f hidden (%.1f%%), "
                "test %.3f ms\n", nCoronas == 1 ? "hidden ones get no quad" : "counted only", coronaPasses, coronasTested * perPass,
                coronasHidden * perPass, coronasTested ? 100.0 * double(coronasHidden) / double(coronasTested) : 0.0,
                double(coronaTicks) * 1000.0 / double(qpcFrequency.QuadPart) * perPass);
            coronaPasses = coronasTested = coronasHidden = coronaTicks = 0;
        }
    }

    // The model's box for entities whose box is their model's (vtable +0x60 the base entity's, CE 0xa319c0: model info
    // +0x20, its min at +0x20 and max at +0x30, the bounding sphere's local centre at +0x10): buildings and the like;
    // others (objects, cars, peds, whose box comes from their fragments) go by the sphere. Placed by the entity's
    // matrix (+0x20) or, without one, its position and heading (+0x10, +0x1c), as the game places the sphere (CE
    // 0xa34340): which of its sine and cosine goes where is taken from whichever puts the model's sphere centre where
    // the game's own (vtable +0x50) is. Returns the model info as floats (min at [8], max at [12]), or null.
    void* pBaseBoundBox = nullptr;
    uint32_t votesPlain = 0, votesSwapped = 0; // which way the heading's sine and cosine went where it could be told
    uintptr_t* pModelInfos = nullptr;
    bool bBoxes = true;
    const float* PlaceModelBox(uintptr_t entity, void** vtable, const float centre[3], float axes[3][3], float origin[3])
    {
        if (!pBaseBoundBox || !pModelInfos || vtable[0x60 / 4] != pBaseBoundBox)
            return nullptr;
        const int16_t index = *(int16_t*)(entity + 0x2e);
        if (index < 0)
            return nullptr;
        const float* model = (const float*)pModelInfos[index];
        if (!model)
            return nullptr;
        const float* local = model + 4; // the sphere's local centre (+0x10)
        auto matches = [&]()
        {
            float x = origin[0], y = origin[1], z = origin[2];
            for (int i = 0; i < 3; ++i)
            {
                x += local[i] * axes[i][0];
                y += local[i] * axes[i][1];
                z += local[i] * axes[i][2];
            }
            const float dx = x - centre[0], dy = y - centre[1], dz = z - centre[2];
            return dx * dx + dy * dy + dz * dz < 0.05f * 0.05f;
        };
        if (const float* matrix = *(const float**)(entity + 0x20))
        {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    axes[i][j] = matrix[i * 4 + j];
            for (int j = 0; j < 3; ++j)
                origin[j] = matrix[12 + j];
            if (matches())
            {
                ++stats.boxByMatrix;
                return model;
            }
            ++stats.boxMismatch;
            return nullptr;
        }
        const float* placed = (const float*)(entity + 0x10);
        const float s = std::sin(placed[3]), c = std::cos(placed[3]);
        origin[0] = placed[0];
        origin[1] = placed[1];
        origin[2] = placed[2];
        axes[2][0] = axes[2][1] = 0.0f;
        axes[2][2] = 1.0f;
        axes[0][2] = axes[1][2] = 0.0f;
        // x' = x cos - y sin, y' = y cos + x sin; or sine and cosine the other way round
        auto place = [&](bool swapped)
        {
            const float a = swapped ? s : c, b = swapped ? c : s;
            axes[0][0] = a, axes[0][1] = b, axes[1][0] = -b, axes[1][1] = a;
        };
        // A sphere centred on the model's vertical axis sits the same either way: such a model goes by the way that
        // matched the others, once they agree
        const bool telling = local[0] * local[0] + local[1] * local[1] > 0.5f * 0.5f;
        for (bool swapped : { false, true })
        {
            place(swapped);
            if (!matches())
                continue;
            if (telling)
            {
                ++(swapped ? votesSwapped : votesPlain);
                ++(swapped ? stats.boxByHeadingSwapped : stats.boxByHeading);
                return model;
            }
            const uint32_t plain = votesPlain, other = votesSwapped;
            if (plain + other < 20 || (swapped ? other * 10 < (plain + other) * 9 : plain * 10 < (plain + other) * 9))
                continue;
            ++(swapped ? stats.boxByHeadingSwapped : stats.boxByHeading);
            return model;
        }
        ++stats.boxMismatch;
        return nullptr;
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
        if (!pScenePhase || !entity || !nMode)
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
        const uint64_t start = __rdtsc();
        ++stats.adds;

        // A new frame: the last one counted, and a new grid if there's one
        if (const uint32_t frame = gameFrame.load(std::memory_order_relaxed); frame != mainFrame)
        {
            if (mainFrame)
                EndFrame();
            mainFrame = frame;
            frameProbe = Take();
            frameOutcome = Precheck(frameProbe);
            if (frameProbe.grid)
            {
                ++stats.grids;
                stats.ageFrames += FrameHistory::Frame() - frameProbe.grid->renderFrame;
            }
        }

        auto& state = StateOf(entity);
        if (state.frame != mainFrame)
        {
            // Once a frame: the entity may be added to several buckets
            const bool repeat = state.frame + 1 == mainFrame;
            state.frame = mainFrame;
            Outcome outcome = Outcome(frameOutcome);
            bool atEdge = false;
            if (outcome == Visible)
            {
                alignas(16) float centre[4]{};
                auto vtable = *(void***)entity;
                reinterpret_cast<void(__thiscall*)(uintptr_t, float*)>(vtable[0x50 / 4])(entity, centre);
                const float radius = reinterpret_cast<float(__thiscall*)(uintptr_t)>(vtable[0x58 / 4])(entity);
                if (!std::isfinite(radius) || !std::isfinite(centre[0] + centre[1] + centre[2]))
                    outcome = Visible;
                else
                {
                    float axes[3][3], origin[3];
                    const float* model = bBoxes ? PlaceModelBox(entity, vtable, centre, axes, origin) : nullptr;
                    if (model)
                    {
                        outcome = TestBox(frameProbe, axes, origin, model + 8, model + 12, radius, &atEdge);
                        ++stats.boxes;
                        stats.boxHidden += outcome == Hidden;
                        // What the sphere alone would have said, for the log: the box lies inside it, so only a hidden box
                        // can have a hidden sphere
                        if (outcome == Hidden)
                            stats.boxSphereHidden += Test(frameProbe, centre, radius) == Hidden;
                    }
                    else
                        outcome = Test(frameProbe, centre, radius, &atEdge);
                }
            }
            state.hidden = outcome == Hidden;
            stats.hiddenAtEdge += state.hidden && atEdge;
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

        frameTicks += __rdtsc() - start;
        return phases;
    }

    // The lists' thread, at the first entity of the next frame
    void EndFrame()
    {
        ++stats.frames;
        ticks += frameTicks;
        maxFrameTicks = std::max(maxFrameTicks, frameTicks);
        frameTicks = 0;
        if (!windowTsc)
        {
            windowTsc = __rdtsc();
            QueryPerformanceCounter(&windowQpc);
        }

        if (stats.frames < 600)
            return;
        Rebuild(table.size());
        // The time stamp counter's rate over the block, from the performance counter
        LARGE_INTEGER nowQpc;
        QueryPerformanceCounter(&nowQpc);
        const uint64_t nowTsc = __rdtsc();
        const double seconds = double(nowQpc.QuadPart - windowQpc.QuadPart) / double(qpcFrequency.QuadPart);
        const double msPerTick = seconds > 0.0 && nowTsc > windowTsc ? seconds * 1000.0 / double(nowTsc - windowTsc) : 0.0;
        windowTsc = nowTsc;
        windowQpc = nowQpc;
        const auto& s = stats;
        const double perFrame = 1.0 / double(s.frames);
        const uint64_t all = texelsAll.exchange(0);
        auto share = [all](std::atomic<uint64_t>& part) { return all ? 100.0 * double(part.exchange(0)) / double(all) : 0.0; };
        std::string outcomes;
        for (int i = 0; i < Outcomes; ++i)
            if (s.outcomes[i])
                outcomes += std::format("{}{} {:.0f}", outcomes.empty() ? "" : ", ", OutcomeNames[i], s.outcomes[i] * perFrame);
        FusionLog::Write("Occlusion", "Stats",
            "%s, per frame over %u frames: %.0f entities of the scene tested (%s; hidden at the screen's edge %.0f); %.0f props, %.0f of them hidden; "
            "%.0f %s (%.1f%%), %.0f of %.0f list adds; test %.3f ms a frame, worst %.3f; grids published %u, "
            "%.1f render frames old when taken; grid texels: %.0f%% nothing in the way (cars, people, other materials), %.0f%% sky, "
            "%.0f%% world within 50 m; "
            "threads: lists %lu, game %lu, adds from other threads %u; %zu entities kept\n"
            "by the model's box: %.0f tested, %.0f hidden (by their sphere %.0f); placed by matrix %.0f, by heading %.0f, by heading "
            "with sine and cosine swapped %.0f, not placed (the sphere instead) %.0f\n",
            nMode == 1 ? "left out" : "counted only", s.frames, s.entities * perFrame, outcomes.c_str(), s.hiddenAtEdge * perFrame, s.props * perFrame,
            s.propsHidden * perFrame, s.left * perFrame, nMode == 1 ? "left out" : "would be left out",
            s.entities ? 100.0 * s.left / s.entities : 0.0, s.addsLeft * perFrame, s.adds * perFrame, double(ticks) * msPerTick * perFrame,
            double(maxFrameTicks) * msPerTick, publishedCount.load(), s.grids ? double(s.ageFrames) / s.grids : 0.0,
            share(texelsNothing), share(texelsSky), share(texelsNear), hookThread, gameThread, otherThreads.exchange(0), tableUsed,
            s.boxes * perFrame, s.boxHidden * perFrame, s.boxSphereHidden * perFrame, s.boxByMatrix * perFrame, s.boxByHeading * perFrame,
            s.boxByHeadingSwapped * perFrame, s.boxMismatch * perFrame);
        stats = {};
        ticks = maxFrameTicks = 0;
    }
}

class OcclusionFeature
{
public:
    OcclusionFeature()
    {
        using namespace Occlusion;

        // All but whether the lists' hook is installed is taken on the fly (Ctrl+Shift+F10): with the hook in, HiZOcclusion
        // moves between 0, 1 and 2 for comparing the same view
        static bool hooked = false, started = false;
        static auto readIni = [](CIniReader& iniReader)
        {
            const int mode = std::clamp(iniReader.ReadInteger("PERFORMANCE", "HiZOcclusion", 1), 0, 2);
            fMaxMove = std::clamp(iniReader.ReadFloat("PERFORMANCE", "HiZOcclusionMaxMove", 4.0f), 0.0f, 50.0f);
            nFrames = std::clamp(iniReader.ReadInteger("PERFORMANCE", "HiZOcclusionFrames", 2), 1, 60);
            fMinRadius = std::clamp(iniReader.ReadFloat("PERFORMANCE", "HiZOcclusionMinRadius", 0.0f), 0.0f, 100.0f);
            bLampShadows = iniReader.ReadInteger("PERFORMANCE", "LampShadowsOcclusion", 1) != 0;
            nCoronas = std::clamp(iniReader.ReadInteger("PERFORMANCE", "CoronasOcclusion", 1), 0, 2);
            nOccluderCategories = iniReader.ReadInteger("PERFORMANCE", "HiZOccluderCategories", 1) & 0xFF;
            bBoxes = iniReader.ReadInteger("PERFORMANCE", "HiZOcclusionBoxes", 1) != 0;
            if (hooked)
            {
                nMode = mode;
                FusionLog::Write("Occlusion", "Settings", "%s: hidden %d frames running, camera moved at most %.1f m, radius from %.1f m, %s\n",
                    nMode == 1 ? "hidden entities are left out of the scene" : nMode == 2 ? "hidden entities are counted only" : "nothing tested",
                    nFrames, fMaxMove, fMinRadius, bBoxes ? "buildings by box" : "all by sphere");
            }
            else if (!started)
                nMode = mode;
        };

        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            readIni(iniReader);
            started = true;
            IniReload::Add(readIni);
            QueryPerformanceFrequency(&qpcFrequency);
            CoronaRender::pCullHidden = &CullCoronas;
            // The game camera, where the procedural generator measures from too (movss xmm2, [0x128e340] in CE 0xc09440)
            if (auto camera = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ? 56 8B 75 0C"); !camera.empty())
                pCamera = *camera.get_first<float*>(12);
            if (!nMode)
                return;

            // The scene's phase: test [0x159af24], edx in the render list after the shadow phases are dropped (CE 0xae7076)
            auto scene = hook::pattern("89 4C 24 28 A9 00 00 00 08");
            // The lists' entry (CE 0xae43a0): sub esp, 0x10; push ebx; mov ebx, [esp+0x18] the phases
            auto add = hook::pattern("83 EC 10 53 8B 5C 24 18 85 DB 0F 84 ? ? ? ? 33 C9 8B C3 85 DB 78");
            if (scene.empty() || add.empty())
            {
                nMode = 0;
                FusionLog::Write("Occlusion", "Settings", "the render lists weren't found, nothing is tested\n");
                return;
            }
            pScenePhase = *scene.get_first<uint32_t*>(0xD);
            // The base entity's box (CE 0xa319c0): movsx eax, word [ecx+0x2e]; mov eax, [eax*4+models]; add eax, 0x20; ret
            if (auto box = hook::pattern("0F BF 41 2E 8B 04 85 ? ? ? ? 83 C0 20 C3"); !box.empty())
            {
                pBaseBoundBox = box.get_first(0);
                pModelInfos = *box.get_first<uintptr_t*>(7);
            }
            // At the entry, before anything is pushed: [esp+4] the phases, [esp+8] the entity
            static auto AddHook = safetyhook::create_mid(add.get_first(0), [](SafetyHookContext& regs)
            {
                auto args = (uint32_t*)(regs.esp + 4);
                args[0] = Phases(uintptr_t(args[1]), args[0]);
            });
            hooked = true;
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
