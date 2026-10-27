module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>
#include <malloc.h>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module procedural;

import common;
import comvars;
import settings;
import inireload;
import entityclass;

// Grass and procedural props ([PROCEDURAL] in the ini, Grass & Props Distance in Display > Advanced).
//
// The game places two kinds of things on its own: grass (plants, drawn instanced by gta_grass, no entities)
// and props (procedural objects: bushes, rocks, litter), which are buildings from the building pool, each
// held by a 0x18-byte record of the procedural object generator (CE 0x1683290):
//  - +0x0000  how many of them have a matrix of their own (scaled or aligned to a slope), at most 512
//  - +0x0004  how many are dynamic objects (at most 200, the game's own cap, left alone)
//  - +0x0008  512 records, put in a free list at +0x3008 by the init (CE 0xc09d20), so 512 props at most
//  - +0x3020  64 candidate positions for the props of the 2dfx effects of one entity, no bounds check
//  - +0x5220  the cap of the per-update queue of positions (+0x5224, 0x20 bytes each, 512)
// Props come from procedural.dat by surface (spacing, how near the camera may be when they are made) and from 2dfx effects of map
// entities found in a 30 m box around the camera (CE 0xc0aa90, at most 128 entities). Grass and the
// surface props go as far as the procedural manager (CE 0x16fb6a0) says: +0x10 near and +0x14 far of the
// grass fade (20 and 20 + Detail Quality / 2, so 25..75 m), +0x1c how far cells are gathered (far + 40),
// +0x20 far squared. The fade reaches the grass shader (fadeAlphaDist) only at the manager's init, so
// moving the Detail Quality slider left it stale; here it's set again with each change.
//
// What this does:
//  - ProceduralGroundTriangles: the manager keeps no more than 512 triangles of the ground with grass or
//    props (its +0x18, set in its constructor at CE 0xc86c2e, sizes its three arrays of 0x60-byte records,
//    linked by 16-bit indices). Those fill up as soon as a place loads (a trace showed 511 at once), and
//    triangles further on are only taken once some nearer ones are let go, so grass and props came in
//    20-30 m ahead of the camera instead of at the gathering distance.
//  - ProceduralPool: more records than 512, so props don't vanish or pop where many are around.
//  - Grass & Props Distance: grass, surface props and 2dfx props go further, and props' own draw distance.
//  - ProceduralDensity: more or fewer props (not grass).
//  - ProceduralFarThinning: past the game's own reach the props thin out instead of all going the whole stretched
//    reach: each one's reach is drawn, by the hash of its position, between the two (the square of a uniform share
//    of the way), so some 75% are left at 130 m, 45% at 200 m and 20% at 300 m with the slider and Detail Quality at
//    their top. Thousands of weeds and plants 100-300 m out took 2-2.5 ms of CPU a frame in the G-buffer.
//  - ProceduralSmallFarCull: a prop whose radius is under 0.5% of its distance (a 0.9 m weed past 180 m, a 0.4 m
//    plant past 80 m) is left out of the scene's render list. The thinning alone left 3000+ weed draws, 2.5 ms of a
//    6 ms G-buffer at night: the game's own reach is own + Detail Quality's 110 m, and it fades them over those
//    110 m (alpha (reach - distance) / extra, CE 0xae7cac), so most far ones were a faint stipple anyway.
//    And past 60 m small props (radius under 2 m) thin out as they are drawn: of those at distance d, a share
//    60 / d is kept (half at 120 m, 30% at 200 m; (40 / d)^2 showed them coming in), picked by the hash of their place, so the same ones stay and nearer ones come back as you
//    get close (skipping them when made would lose them for good: a position is offered once). With the pool at
//    16384 instead of 512 and the slider at its top, 4000+ weed draws 50-200 m out still took 3.5 ms. Models that Far
//    Prop Instancing draws in batches (ProceduralGround::DrawnInstanced) are left out of the thinning.
//  - The 64 candidates of one entity's 2dfx props are capped, which the game itself never checked.
//  - HeavyPropCull ([PERFORMANCE]): not the generator's, any small entity (radius under 3 m, no LOD parent) whose model
//    took kHeavyDraws or more device draws an entity in the G-buffer (EntityClass::DrawsPerEntity, learned as they are
//    drawn): benches, posts, stands made of a dozen to forty geometries cost 20-180 us each where a lamp post costs 5,
//    and 1-2 ms of a frame 40-140 m out (CJ_MH_CP_POST1, CJ_GB_bench_3/4, BM_knobelisk, GB_hotdogstand01). Past 40 m
//    they leave the sun's cascades (the phases of type 0x15, CE 0x159b760, the same the game drops for props: past
//    the second cascade their shadows are a few texels), past 80 m they thin out as the small props do, 80 / distance
//    kept by the hash of their place, and under 1% of their distance (a 1.6 m bench past 160 m) none are drawn.
//  - FarFadeCull ([PERFORMANCE]): any building or prop under kFadeCullRadius (not vehicles or people, no LOD parent)
//    past kFadeCullFrom that the game is fading out by distance (its alpha, entity +0x63, set by the visibility pass
//    CE 0xae6ca0 -> 0xae9400/0xae9260 before the lists are made) leaves the scene's list once that alpha is under its
//    own share of kFadeCullAlpha (the hash of its place), so each goes somewhere in the faint half of its fade instead
//    of at its end. Detail Quality's 110 m are added to every entity's reach and faded over: lamp posts (150 m of their
//    own), traffic lights (100 m) and plant props (12-20 m) were drawn 130-290 m out as a faint stipple at the full
//    price of a draw, 5-9 us each for the posts, some 1.5 ms of a 6 ms G-buffer looking over a park.
//    Taken where the entity goes into the phases' lists (CE 0xae43a0): the render lists' hooks above miss most
//    objects (a log: CJ_MH_CP_POST1 kept 30 cascade draws a frame 75 m out).
//  - ProceduralGroundCovers: grass that runs on under a road or a pavement grows no props there, nor GPU grass
//    (Procedural::UnderCover).
//
// And what keeps the cost of more props down (the game already keeps them out of shadows: it sets
// entity +0x24 0x10000 on each, and the render lists then drop the shadow phases, CE 0xae85a5/0xae7058):
//  - ProceduralInReflections = 0: the render lists drop the water reflection phases for props, as for
//    shadows: the mask CE 0x159af28 has the phases of type 0x11 (water reflection) and of type 0x1f, which
//    is the scene itself (its bit alone in CE 0x159af24), so that one is kept.
//  - ProceduralSpawnsPerUpdate: the queue of positions makes no more props than this an update, nearest
//    and those ahead of the camera first, all within 50 m at once, the rest wait for the next ones, so a jump in distance or a new
//    area doesn't stall a frame. Waiting positions
//    of a provider the game removed meanwhile (its requests at +0x522c, processed first) are dropped.
//  - ProceduralLog: the log has the time of the generator's update (CE 0xc0aa90) and how many props are out.
// The ground triangles the procedural manager holds, for effects that grow things on them (GPU grass): told on the main
// thread as the manager takes and lets go of them; a record's address is its key until it is let go.
export namespace ProceduralGround
{
    struct Triangle
    {
        float corner[3][3];
        uint8_t tag;        // the surface's procedural tag (material +0x4c, bits 8..15): PROCOBJ records by tag
        uint8_t surface;    // its material (bits 0..7): 1 GRASS, 2 SHORT_GRASS, 3 GRASS_PATCHY, 4 GRASS_LONG, 0 any other (GrassSurface)
    };
    // Set before the world loads (gpugrass.ixx): the manager also gathers ground of the grassy materials (GrassSurface)
    // whose procedural tag has neither plants nor props, as ground with props of a tag that has none (so the generator
    // makes nothing there)
    inline bool GatherGrassSurfaces = false;
    inline void (*OnAdded)(const void* key, const Triangle& triangle) = nullptr;
    // Far Prop Instancing (farpropinstancing.ixx), any thread: the model's far props are drawn in one instanced draw,
    // so ProceduralSmallFarCull's thinning past 60 m leaves them in (the size cull still takes the tiniest); not those
    // with a matrix of their own (scaled), which it doesn't take
    inline bool (*DrawnInstanced)(uint16_t model) = nullptr;
    inline void (*OnRemoved)(const void* key) = nullptr;
    // A prop of `model` at `distance` from the camera left out of the scene, where other grass grows in its place
    // (gpugrass.ixx); `keep` is the prop's own random value, 0..1, the same each time it is made there
    inline bool (*HideGrass)(uint16_t model, float distance, float keep) = nullptr;
    // How far ahead of the camera ground should be gathered (GPU grass's range), 0 for the manager's own circle around
    // it: its centre then moves ahead along the view by what its radius lacks of that, at most half the radius, so less
    // is held behind (Procedural::ShiftGatherCentre)
    inline float GatherAhead = 0.0f;

    // procedural.dat's PROCOBJ records (CE 0x16c8fb0): +4 count, records of 0x44 from +8: +0 tag, +4 model index,
    // +8 spacing; nullptr until loaded
    uint8_t* Definitions();

    // ProceduralGroundCovers: surfaces without plants or props (roads, pavements, roofs) near the camera, which grass
    // under them gives way to (Procedural::UnderCover): grass where one lies level with it (CoverBelow under it at
    // most) or up to CoverHeight above. Any thread: their version, changing as they come and go, and those within
    // `reach` (flat) of x, y
    struct Cover { float corner[3][3]; };
    constexpr float CoverHeight = 3.0f;
    constexpr float CoverBelow = 0.03f;
    uint32_t CoversVersion();
    void CopyCovers(std::vector<Cover>& out, float x, float y, float reach);
}

namespace Procedural
{
    constexpr uint32_t VanillaRecords = 512;
    constexpr uint32_t RecordSize = 0x18;
    constexpr uint32_t QueueEntrySize = 0x20;
    constexpr int32_t EntityCandidates = 64;
    constexpr float VanillaEntityRadius = 30.0f;
    constexpr float QueryLead = 40.0f;
    constexpr float NearAlways = 50.0f;     // queued props closer than this are made at once

    // Settings
    int32_t nPool = 4096;
    int32_t nMatrixLimit = 4096;
    int32_t nMatrixPool = 28000;
    int32_t nDrawableRefs = 40000;
    int32_t nRenderCache = 32768;
    float fDensity = 1.0f;
    float fDistance = 1.0f;
    bool bFarThinning = true;
    bool bSmallFarCull = true;
    bool bHeavyPropCull = true;
    bool bFarFadeCull = true;
    constexpr float kFadeCullRadius = 8.0f;        // entities under this radius
    constexpr float kFadeCullFrom = 50.0f;         // further than this
    constexpr uint32_t kFadeCullAlpha = 128;       // leave once their fade's alpha is under their share of this
    constexpr uint32_t kHeavyDraws = 8;            // device draws an entity in the G-buffer from which a model is heavy
    constexpr float kHeavyRadius = 3.0f;           // and its radius under this
    constexpr float kHeavyCascadesFrom = 40.0f;
    constexpr float kHeavyThinFrom = 80.0f;
    constexpr float kHeavyScreenSize = 0.01f;
    constexpr float SmallFarScreenSize = 0.005f;   // radius over distance under which a prop is left out
    constexpr float SmallPropRadius = 2.0f;        // smaller ones thin out past SmallFarThinFrom
    constexpr float SmallFarThinFrom = 60.0f;
    int32_t nGroundTriangles = 2048;
    volatile LONG nTriangles = 0;       // with the trace on, the triangles the manager holds
    bool bInReflections = false;
    int32_t nSpawnsPerUpdate = 128;
    bool bLog = false;                  // ProceduralLog >= 1: the statistics every 30 s
    bool bTrace = false;                // ProceduralLog >= 2: every event, to ProceduralTrace.log

    // The game's
    uint8_t* pGenerator = nullptr;      // CE 0x1683290
    uint8_t* pManager = nullptr;        // CE 0x16fb6a0, taken from the first call that has it
    uint8_t* pDefinitions = nullptr;    // CE 0x16c8fb0, procedural.dat's PROCOBJ records
    void(__fastcall* ListPush)(void* list, void* edx, void* node) = nullptr;
    float* pDetailExtra = nullptr;      // CE 0x103f714: Detail Quality's 10..110 m, added to every entity's draw distance
    uint8_t** pBuildingPool = nullptr;  // CE 0x12bd0e8: props are buildings; the generator wants 500 of it free
    float* pCamera = nullptr;           // CE 0x128e340, where the generator measures distances from: the
                                        // last row of the camera's matrix at CE 0x128e310, whose second row
                                        // (0x128e320) is where it looks (the game's heading comes from it, CE 0xa88308)
    void(__cdecl* SetGrassFade)(float nearDistance, float farDistance) = nullptr;
    float* pRadius[3] = {};             // the 30 m half extents of the 2dfx box, in code
    uint32_t* pReflectionPhases = nullptr; // CE 0x159af28: the phases of type 0x11 (water reflection) and 0x1f
    uint32_t* pScenePhase = nullptr;       // CE 0x159af24: the one of type 0x1f, the scene itself, kept
    uint32_t* pCascadePhases = nullptr;    // CE 0x159b760: those of type 0x15, the sun's cascades

    // Map entities with 2dfx props the generator found this update. The game marks them with the world scan
    // stamp (CE 0x11a8908) in entity +0x3c, the field and stamp the render lists' sector scan swaps to skip
    // what it already took (CE 0xae92ee); a render pass reading the stamp after the generator moved it on took
    // the generator's entities for done and left them out of that frame. With the 2dfx box at x3 (90 m) that
    // was every tree, fence or ladder around the player flickering. The generator keeps its own set here
    std::unordered_set<uintptr_t> scanned;

    std::vector<uint8_t> extraRecords;
    // The larger queue is ours, not from the game's heap: the game's own buffer of 512 is kept aside and
    // put back before the game frees it (reset CE 0xc0aa2e, destructor CE 0xc08dc5)
    std::vector<uint8_t> queueStorage;
    void* pGameQueue = nullptr;
    float fBaseNear = 0.0f;             // the manager's own near (20), read before it's first changed

    // procedural.dat's PROCOBJ fields here (loaded at CE 0xc29310): +0x08 spacing (at least 2), +0x0c 1 / spacing^2,
    // +0x10 the minimum distance squared (at most 80 m): the generator makes a triangle's props only while the camera
    // is further than that (CE 0xc09440), and tries once, when the triangle comes in, so props don't pop up in view
    struct Definition { float spacing, inverseSquare, minDistanceSquared; };
    std::vector<Definition> definitions;  // as procedural.dat set them

    // Trace (ProceduralLog = 2): lines gathered here and written once a second to
    // GTAIV.EFLC.FusionFix.ProceduralTrace.log, at most TraceLimit bytes a run
    constexpr size_t TraceLimit = 64u << 20;
    std::mutex traceMutex;
    std::string traceText;
    size_t traceWritten = 0;
    bool traceFull = false;
    const uint64_t traceStart = GetTickCount64();

    void Trace(std::string&& line)
    {
        if (!bTrace || traceFull)
            return;
        std::lock_guard lock(traceMutex);
        traceText += line;
        traceText += '\n';
    }

    void FlushTrace()
    {
        std::string text;
        {
            std::lock_guard lock(traceMutex);
            text.swap(traceText);
        }
        if (text.empty() || traceFull)
            return;
        traceWritten += text.size();
        if (traceWritten > TraceLimit)
        {
            traceFull = true;
            text += std::format("trace stopped at {} MB\n", TraceLimit >> 20);
        }
        FusionLog::WriteText("ProceduralTrace", "", text);
    }

    uint32_t Frame() { return CTimer::m_frameCount ? *CTimer::m_frameCount : 0; }
    uint64_t Now() { return GetTickCount64() - traceStart; }

    // " t=ms f=frame": when, in milliseconds since the plugin started and in frames
    std::string When() { return std::format("t={} f={}", Now(), Frame()); }

    // " pos=x,y,z d=metres ang=degrees": where, and how far and how far off the camera's view direction
    std::string Where(const float* position)
    {
        if (!pCamera)
            return std::format(" pos={:.1f},{:.1f},{:.1f}", position[0], position[1], position[2]);
        const float* forward = pCamera - 8;
        float dx = position[0] - pCamera[0], dy = position[1] - pCamera[1], dz = position[2] - pCamera[2];
        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        float facing = distance > 0.001f ? (dx * forward[0] + dy * forward[1] + dz * forward[2]) / distance : 1.0f;
        float angle = std::acos(std::clamp(facing, -1.0f, 1.0f)) * 57.29578f;
        return std::format(" pos={:.1f},{:.1f},{:.1f} d={:.1f} ang={:.0f}", position[0], position[1], position[2], distance, angle);
    }

    // Queued positions by when they came in, to tell how long each waited
    std::unordered_map<uint64_t, uint64_t> queuedAt;
    uint64_t PositionKey(const float* position)
    {
        uint64_t key = 1469598103934665603ull;
        for (int i = 0; i < 3; i++)
            key = (key ^ std::bit_cast<uint32_t>(position[i])) * 1099511628211ull;
        return key;
    }

    // What the last pass over the queue did, for the update's line
    uint32_t lastQueue = 0, lastNew = 0, lastMade = 0, lastWaiting = 0, lastDropped = 0, lastFailed = 0;
    uint32_t updateIndex = 0;
    // With the trace on: the most entities a sector scan found in the frame's buffer (ProceduralRenderCache), and
    // how often the game ran out of drawable references and took models from other entities to free some
    volatile LONG nRenderCachePeak = 0, nDrawableReclaims = 0;
    float lastCamera[3] = {};
    uint64_t lastCameraTime = 0;

    // The props that are out, for the render lists, with their model's radius and the share of the way past
    // SmallFarThinFrom they keep being drawn at (from the hash of their place)
    struct PropInfo { float radius; float keep; };
    std::unordered_map<uintptr_t, PropInfo> props;
    SRWLOCK propsLock = SRWLOCK_INIT;
    uintptr_t modelInfos = 0;           // CE 0x1295cd8, model infos by index

    // Its radius and share when it is a prop, else a negative radius
    PropInfo FindProp(uintptr_t entity)
    {
        AcquireSRWLockShared(&propsLock);
        auto it = props.find(entity);
        PropInfo info = it != props.end() ? it->second : PropInfo{ -1.0f, 0.0f };
        ReleaseSRWLockShared(&propsLock);
        return info;
    }

    // Positions held over to the next update, first in the queue then
    std::vector<uint8_t> waiting;
    uint32_t nCarried = 0;

    // ProceduralConeBack: positions outside a cone around where the camera looks are kept here, not made, and go
    // back to the queue once the camera turns to them. Its radius is the gathering radius within ProceduralConeFullAngle
    // of where the camera looks and this behind, easing down between them. A turning camera is followed
    // ProceduralConeTurnLead ahead (45 degrees at most): a position is inside if it is inside the cone now or the one the
    // camera turns to. Made props stay until their ground triangle goes, or with ProceduralConeRelease until they are
    // well outside the cone
    float fConeBack = 70.0f;
    float fConeFullCos = 0.2588f;           // cos of ProceduralConeFullAngle, 75 degrees
    float fConeTurnLead = 0.3f;             // seconds
    constexpr size_t ParkedLimit = 65536;   // positions kept at most, past it new ones are made at once
    std::vector<uint8_t> parked;

    // The cone of one pass over the queue
    struct Cone
    {
        bool active = false;
        bool turning = false;
        float position[3] = {}, forward[3] = {}, turnedTo[3] = {};
        float ahead = 0.0f;
    } cone;
    float coneLastForward[3] = {};
    int64_t coneLastTime = 0;

    void UpdateCone()
    {
        cone.active = cone.turning = false;
        if (fConeBack <= 0.0f || !pCamera)
            return;
        const float* forward = pCamera - 8;
        float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
        // A camera not set up yet: everything is inside
        if (!(length > 0.5f && length < 2.0f) || !std::isfinite(pCamera[0] + pCamera[1] + pCamera[2]))
        {
            coneLastTime = 0;
            return;
        }
        for (int i = 0; i < 3; i++)
        {
            cone.position[i] = pCamera[i];
            cone.forward[i] = forward[i] / length;
        }
        cone.ahead = pManager ? std::max(*(float*)(pManager + 0x1C), fConeBack) : 1.0e9f;

        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        if (coneLastTime && fConeTurnLead > 0.0f)
        {
            float seconds = float(double(now.QuadPart - coneLastTime) / double(frequency.QuadPart));
            float turn[3], turnSquared = 0.0f;
            for (int i = 0; i < 3; i++)
            {
                turn[i] = cone.forward[i] - coneLastForward[i];
                turnSquared += turn[i] * turn[i];
            }
            // A cut, or more than about 50 degrees since the last update, is no turn to follow
            if (seconds > 0.0005f && seconds < 0.25f && turnSquared > 1.0e-8f && turnSquared < 0.7f)
            {
                float k = std::min(fConeTurnLead / seconds, 1.0f / std::sqrt(turnSquared));
                float to[3], toSquared = 0.0f;
                for (int i = 0; i < 3; i++)
                {
                    to[i] = cone.forward[i] + turn[i] * k;
                    toSquared += to[i] * to[i];
                }
                if (toSquared > 1.0e-6f)
                {
                    for (int i = 0; i < 3; i++)
                        cone.turnedTo[i] = to[i] / std::sqrt(toSquared);
                    cone.turning = true;
                }
            }
        }
        coneLastTime = now.QuadPart;
        std::copy_n(cone.forward, 3, coneLastForward);
        cone.active = true;
    }

    // How much a position faces the camera's view, the one it turns to as well
    float ConeFacing(float dx, float dy, float dz, float distance)
    {
        float facing = (dx * cone.forward[0] + dy * cone.forward[1] + dz * cone.forward[2]) / distance;
        if (cone.turning)
            facing = std::max(facing, (dx * cone.turnedTo[0] + dy * cone.turnedTo[1] + dz * cone.turnedTo[2]) / distance);
        return facing;
    }

    // slack: metres the cone is taken wider all round, so a made prop goes only once well outside
    bool InCone(const float* position, float slack = 0.0f)
    {
        if (!cone.active)
            return true;
        float back = fConeBack + slack, ahead = cone.ahead + slack;
        float dx = position[0] - cone.position[0], dy = position[1] - cone.position[1], dz = position[2] - cone.position[2];
        float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (!(distanceSquared > back * back))
            return true;
        if (distanceSquared > ahead * ahead)
            return false;
        float distance = std::sqrt(distanceSquared);
        float facing = ConeFacing(dx, dy, dz, distance);
        if (facing >= fConeFullCos)
            return true;
        float t = std::clamp((facing + 1.0f) / (fConeFullCos + 1.0f), 0.0f, 1.0f);
        return distance <= back + (ahead - back) * t * t * (3.0f - 2.0f * t);
    }

    // ProceduralConeRelease: the queue entry each surface prop was made from (by its record), so a prop the cone has left
    // behind goes and its position waits in the parked store like the others. A dump at x3 had 55% of the props behind
    // the camera, holding their models, records and places in the game's list of buildings with models
    bool bConeRelease = true;
    constexpr float ReleaseSlack = 25.0f;
    constexpr uint32_t ReleasesPerUpdate = 256;
    struct MadeFrom { uint8_t entry[QueueEntrySize]; };
    std::unordered_map<uint8_t*, MadeFrom> madeFrom;
    uint32_t nConeReleased = 0;

    // ProceduralGroundCovers (below): on, and the covers' version; the props made from the queue by the cell of their
    // entry, so new covers find those under them without a walk of them all
    bool bCovers = true;
    volatile uint32_t nCoversVersion = 0;
    constexpr float kCoverCell = 4.0f;
    uint64_t CoverCellKey(int x, int y) { return uint64_t(uint32_t(x)) << 32 | uint32_t(y); }
    int CoverCellOf(float v) { return int(std::floor(v / kCoverCell)); }
    uint64_t CoverCellAt(const float* place) { return CoverCellKey(CoverCellOf(place[0]), CoverCellOf(place[1])); }
    std::unordered_map<uint64_t, std::vector<uint8_t*>> madeCells;

    void ForgetMade(uint8_t* record)
    {
        auto it = madeFrom.find(record);
        if (it == madeFrom.end())
            return;
        if (auto cell = madeCells.find(CoverCellAt((const float*)it->second.entry)); cell != madeCells.end())
        {
            auto& records = cell->second;
            if (auto at = std::find(records.begin(), records.end(), record); at != records.end())
            {
                *at = records.back();
                records.pop_back();
            }
            if (records.empty())
                madeCells.erase(cell);
        }
        madeFrom.erase(it);
    }

    void ForgetAllMade()
    {
        madeFrom.clear();
        madeCells.clear();
    }

    // Statistics for the log
    uint32_t nUsedMax = 0;
    uint32_t nCandidatesCapped = 0, nEntitiesCapped = 0;
    uint32_t nQueueMax = 0, nWaitingMax = 0, nDropped = 0;
    uint32_t nGrassHidden = 0;
    uint32_t nReflectionsSkipped = 0, nSmallFarCulled = 0, nHeavyCascades = 0, nHeavyScene = 0, nFadeCulled = 0;
    uint32_t nConeFull = 0, nParkedNow = 0;
    uint32_t nUpdates = 0;
    double fUpdateMs = 0.0, fUpdateMaxMs = 0.0;

    int32_t BuildingsFree()
    {
        auto pool = pBuildingPool ? *pBuildingPool : nullptr;
        return pool ? int32_t(*(uint32_t*)(pool + 8) - *(uint32_t*)(pool + 0x14)) : -1;
    }

    void Log(const char* component, const std::string& text);

    // The generator makes a prop only while 500 places of the building pool are free (CE 0xc08ebf). With
    // thousands of props that left the map too few to stream a new area in: without ExtendedLimits (32000) it
    // ran out and the game crashed making a map entity (CE 0x9d77e0). An eighth of the pool is kept for it
    uint32_t* pBuildingReserve = nullptr;
    uint32_t nBuildingReserve = 0;

    void ApplyBuildingReserve()
    {
        auto pool = pBuildingPool ? *pBuildingPool : nullptr;
        if (!pool || !pBuildingReserve || nBuildingReserve)
            return;
        nBuildingReserve = std::max<uint32_t>(2000, *(uint32_t*)(pool + 8) / 8);
        injector::WriteMemory(pBuildingReserve, nBuildingReserve, true);
        Log("Settings", std::format("building pool {}, {} of it kept free for the map (game 500)", *(uint32_t*)(pool + 8), nBuildingReserve));
    }

    uint32_t Records() { return std::max<uint32_t>(VanillaRecords, uint32_t(nPool)); }

    uint32_t UsedRecords()
    {
        if (!pGenerator)
            return 0;
        auto freeCount = *(uint32_t*)(pGenerator + 0x3008);
        auto total = Records();
        return freeCount < total ? total - freeCount : 0;
    }

    // After the generator's init (game start and every rebuild of the manager): the extra records go
    // into the free list after the game's 512, and the per-update queue grows to as many entries.
    void ExtendGenerator(uint8_t* generator)
    {
        pGenerator = generator;
        ApplyBuildingReserve();
        nCarried = 0;
        waiting.clear();
        parked.clear();
        ForgetAllMade();
        coneLastTime = 0;
        queuedAt.clear();
        Trace(std::format("I {} generator init, records {}", When(), Records()));
        AcquireSRWLockExclusive(&propsLock);
        props.clear();
        ReleaseSRWLockExclusive(&propsLock);
        auto extra = Records() - VanillaRecords;
        if (extra && ListPush)
        {
            extraRecords.assign(size_t(extra) * RecordSize, 0);
            for (uint32_t i = 0; i < extra; i++)
                ListPush(generator + 0x3008, nullptr, extraRecords.data() + size_t(i) * RecordSize);
        }

        auto queueSize = std::min<uint32_t>(Records(), 0xFFFF);
        if (queueSize > VanillaRecords)
        {
            queueStorage.assign(size_t(queueSize) * QueueEntrySize, 0);
            pGameQueue = *(void**)(generator + 0x5224);
            *(void**)(generator + 0x5224) = queueStorage.data();
            *(uint16_t*)(generator + 0x5228) = 0;
            *(uint16_t*)(generator + 0x522A) = uint16_t(queueSize);
            *(uint32_t*)(generator + 0x5220) = queueSize;
        }
    }

    void RestoreGameQueue(uint8_t* generator)
    {
        if (pGameQueue && *(void**)(generator + 0x5224) == queueStorage.data())
        {
            *(void**)(generator + 0x5224) = pGameQueue;
            *(uint16_t*)(generator + 0x5228) = 0;
            *(uint16_t*)(generator + 0x522A) = uint16_t(VanillaRecords);
            *(uint32_t*)(generator + 0x5220) = VanillaRecords;
            pGameQueue = nullptr;
        }
        ForgetAllMade();
    }

    // ProceduralGatherPriority: the manager takes ground triangles in the order of the collision it walks (CE 0xc87b00),
    // while one of its points is within +0x20 (far squared, CE 0xc87f40) and a record is free ([+4], the head of the
    // free list, 16-bit indices from 1, next at +0x56 of a record at [+0xf30] + (index - 1) * 0x60), and lets one go once
    // all of it is beyond. Full, it took nearby triangles only as far ones went, often within procedural.dat's minimum
    // distance, which then never got props. So while it is nearly full that radius narrows, the game lets the furthest
    // go its own way and takes the near ones in their place; with room again it widens back
    bool bGatherPriority = true;
    float fGatherFull = 0.0f;               // the radius set for Detail Quality and the slider
    float fGatherNow = 0.0f;                // the radius this frame
    constexpr float GatherMin = 60.0f;

    void UpdateGatherRadius()
    {
        if (!bGatherPriority || !pManager || fGatherFull <= 0.0f)
            return;
        auto records = *(uint8_t**)(pManager + 0xF30);
        uint32_t capacity = *(uint32_t*)(pManager + 0x18);
        if (!records || !capacity)
            return;
        // Only whether fewer than 1/32 or more than 1/8 are free matters, so the walk stops past 1/8: a whole walk of
        // the free list each update, cache misses at 0x60 apart, was about 3% of the main thread in a profile
        uint32_t free = 0;
        for (uint32_t index = *(uint16_t*)(pManager + 4); index && index <= capacity && free <= capacity / 8; free++)
            index = *(uint16_t*)(records + size_t(index - 1) * 0x60 + 0x56);
        float low = std::min(GatherMin, fGatherFull);
        if (fGatherNow <= 0.0f)
            fGatherNow = fGatherFull;
        if (free < capacity / 32)
            fGatherNow = std::max(low, fGatherNow - 8.0f);
        else if (free > capacity / 8)
            fGatherNow = std::min(fGatherFull, fGatherNow + 2.0f);
        fGatherNow = std::clamp(fGatherNow, low, fGatherFull);
        *(float*)(pManager + 0x20) = fGatherNow * fGatherNow;
    }

    // GatherAhead: the manager's centre (+0xf40), copied from the camera in its update (CE 0xc869e0) and what its
    // collision query (box +0x1c, CE 0xc87920), gathering and letting go (CE 0xc874f0) all measure from, moved ahead
    // along the view, flat
    void ShiftGatherCentre(uint8_t* manager)
    {
        const float ahead = ProceduralGround::GatherAhead;
        if (!(ahead > 0.0f) || !pCamera)
            return;
        const float radius = std::sqrt((std::max)(*(float*)(manager + 0x20), 0.0f));
        const float shift = std::clamp(ahead - radius, 0.0f, radius * 0.5f);
        const float* forward = pCamera - 8;
        const float length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
        if (!(shift > 0.0f) || !(length > 0.1f))
            return;
        auto centre = (float*)(manager + 0xF40);
        centre[0] += forward[0] / length * shift;
        centre[1] += forward[1] / length * shift;
    }

    // The manager's grass and gathering distances, from what the game just set for Detail Quality
    void ScaleManagerDistances(uint8_t* manager)
    {
        pManager = manager;
        auto& nearDistance = *(float*)(manager + 0x10);
        auto& farDistance = *(float*)(manager + 0x14);
        if (fBaseNear <= 0.0f)
            fBaseNear = nearDistance;
        nearDistance = fBaseNear * fDistance;
        farDistance *= fDistance;
        *(float*)(manager + 0x1C) = farDistance + QueryLead;
        *(float*)(manager + 0x20) = farDistance * farDistance;
        fGatherFull = farDistance;
        fGatherNow = std::min(fGatherNow > 0.0f ? fGatherNow : farDistance, farDistance);
    }

    // At startup and on Ctrl+Shift+F10 (IniReload): what the generator looks at as it runs. The pools, the density
    // and the ground triangles are sized at startup.
    void ReadLiveSettings(CIniReader& iniReader)
    {
        bInReflections = iniReader.ReadInteger("PROCEDURAL", "ProceduralInReflections", 0) != 0;
        bGatherPriority = iniReader.ReadInteger("PROCEDURAL", "ProceduralGatherPriority", 1) != 0;
        fConeBack = std::clamp(iniReader.ReadFloat("PROCEDURAL", "ProceduralConeBack", 70.0f), 0.0f, 1000.0f);
        // For the props made from now on
        bFarThinning = iniReader.ReadInteger("PROCEDURAL", "ProceduralFarThinning", 1) != 0;
        bSmallFarCull = iniReader.ReadInteger("PROCEDURAL", "ProceduralSmallFarCull", 1) != 0;
        bHeavyPropCull = iniReader.ReadInteger("PERFORMANCE", "HeavyPropCull", 1) != 0;
        bFarFadeCull = iniReader.ReadInteger("PERFORMANCE", "FarFadeCull", 1) != 0;
        fConeFullCos = std::cos(std::clamp(iniReader.ReadFloat("PROCEDURAL", "ProceduralConeFullAngle", 75.0f), 30.0f, 120.0f) * 0.01745329f);
        fConeTurnLead = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralConeTurnLead", 300), 0, 1000) / 1000.0f;
        bConeRelease = iniReader.ReadInteger("PROCEDURAL", "ProceduralConeRelease", 1) != 0;
        if (const bool covers = iniReader.ReadInteger("PROCEDURAL", "ProceduralGroundCovers", 1) != 0; covers != bCovers)
        {
            bCovers = covers;
            nCoversVersion = nCoversVersion + 1;
        }
        nSpawnsPerUpdate = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralSpawnsPerUpdate", 128), 0, 0xFFFF);
    }

    void ApplyDefinitions()
    {
        if (!pDefinitions)
            return;
        auto count = std::min<size_t>(*(uint32_t*)(pDefinitions + 4), definitions.size());
        auto rootDensity = std::sqrt(fDensity);
        for (size_t i = 0; i < count; i++)
        {
            auto record = pDefinitions + 8 + i * 0x44;
            *(float*)(record + 0x08) = definitions[i].spacing / rootDensity;
            *(float*)(record + 0x0C) = definitions[i].inverseSquare * fDensity;
            // The minimum distance stays as the game has it: scaled with the slider it kept props off every triangle
            // that came in nearer than three times as far, after a load or when the triangle cap freed a place nearby
            *(float*)(record + 0x10) = definitions[i].minDistanceSquared;
        }
    }

    void ApplyEntityRadius()
    {
        for (auto radius : pRadius)
            if (radius)
                injector::WriteMemory(radius, VanillaEntityRadius * fDistance, true);
    }

    float DistanceFromPref(int32_t step) { return 1.0f + 0.25f * float(std::clamp(step, 0, 8)); }

    SafetyHookInline shGeneratorInit;
    bool __fastcall GeneratorInit(uint8_t* generator, void* edx)
    {
        auto result = shGeneratorInit.unsafe_fastcall<bool>(generator, edx);
        ExtendGenerator(generator);
        return result;
    }

    SafetyHookInline shUpdateDistances;
    void __fastcall UpdateDistances(uint8_t* manager, void* edx)
    {
        shUpdateDistances.unsafe_fastcall(manager, edx);
        ScaleManagerDistances(manager);
        if (SetGrassFade)
            SetGrassFade(*(float*)(manager + 0x10), *(float*)(manager + 0x14));
    }

    SafetyHookInline shLoadDefinitions;
    bool __fastcall LoadDefinitions(uint8_t* defs, void* edx)
    {
        auto result = shLoadDefinitions.unsafe_fastcall<bool>(defs, edx);
        pDefinitions = defs;
        auto count = *(uint32_t*)(defs + 4);
        definitions.resize(count);
        for (uint32_t i = 0; i < count; i++)
        {
            auto record = defs + 8 + i * 0x44;
            definitions[i] = { *(float*)(record + 0x08), *(float*)(record + 0x0C), *(float*)(record + 0x10) };
        }
        ApplyDefinitions();
        return result;
    }

    // Props made: kept in a set for the render lists, traced, and their own draw distance (entity +0x50, copied
    // from the model's at 0x9d7a52; the render lists draw an entity within it times the phase's multiplier,
    // CE 0xaebfa5/0xaec03a) goes with the distance they now spawn at
    SafetyHookInline shCreate;
    uint8_t* __fastcall Create(uint8_t* generator, void* edx, void* a1, void* a2, void* a3, void* a4, void* a5)
    {
        auto record = shCreate.unsafe_fastcall<uint8_t*>(generator, edx, a1, a2, a3, a4, a5);
        if (bTrace)
        {
            // From the queue (a1 is its entry) or from a 2dfx effect
            auto queue = pGenerator ? *(uint8_t**)(pGenerator + 0x5224) : nullptr;
            auto entry = (uint8_t*)a1;
            bool queued = queue && entry >= queue && entry < queue + size_t(*(uint32_t*)(pGenerator + 0x5220)) * QueueEntrySize;
            std::string waited;
            if (queued)
            {
                if (auto it = queuedAt.find(PositionKey((float*)entry)); it != queuedAt.end())
                {
                    waited = std::format(" waited={}", Now() - it->second);
                    queuedAt.erase(it);
                }
            }
            auto entity = record ? *(uint8_t**)(record + 8) : nullptr;
            float position[3] = {};
            if (entity && CEntity::GetPosition(uintptr_t(entity), position))
                Trace(std::format("P {} src={} model={} draw={:.0f}{}{}", When(), queued ? "surface" : "2dfx",
                    *(int16_t*)(entity + 0x2E), *(float*)(entity + 0x50), Where(position), waited));
            else
            {
                lastFailed++;
                Trace(std::format("P {} src={} failed{}{}", When(), queued ? "surface" : "2dfx",
                    queued ? Where((float*)entry) : std::string(), waited));
            }
        }
        if (record && (bConeRelease || ProceduralGround::HideGrass || bCovers) && pGenerator)
        {
            // Made from the queue (a1 its entry; the caller sets the record's provider, +0xc, right after)
            auto queue = *(uint8_t**)(pGenerator + 0x5224);
            auto entry = (uint8_t*)a1;
            if (queue && entry >= queue && entry < queue + size_t(*(uint32_t*)(pGenerator + 0x5220)) * QueueEntrySize)
            {
                ForgetMade(record);
                memcpy(madeFrom[record].entry, entry, QueueEntrySize);
                madeCells[CoverCellAt((const float*)entry)].push_back(record);
            }
        }
        if (record)
        {
            if (auto entity = *(uint8_t**)(record + 8))
            {
                const auto model = *(uint16_t*)(entity + 0x2E);
                const auto info = modelInfos ? *(uintptr_t*)(modelInfos + model * 4) : 0;
                const float radius = info ? *(float*)(info + 0x1C) : 0.0f;
                // Other bits of the hash than the reach's below, so the two don't go together
                float place[3];
                const float keep = CEntity::GetPosition(uintptr_t(entity), place) ?
                    float((PositionKey(place) >> 12) & 0xFFFFFF) / float(0x1000000) : 0.0f;
                AcquireSRWLockExclusive(&propsLock);
                props[uintptr_t(entity)] = { radius, keep };
                ReleaseSRWLockExclusive(&propsLock);

                // Drawn while its own distance times the phase's multiplier (about 1) plus Detail Quality's extra
                // is more than the camera's distance (CE 0xaec13a), so for the slider to take that whole reach
                // k times further the prop's own distance becomes k * own + (k - 1) * extra
                auto& drawDistance = *(float*)(entity + 0x50);
                if (fDistance != 1.0f && std::isfinite(drawDistance) && drawDistance > 0.0f && drawDistance < 2000.0f)
                {
                    float extra = std::max(pDetailExtra ? *pDetailExtra : 0.0f, 0.0f);
                    float stretched = drawDistance * fDistance + (fDistance - 1.0f) * extra;
                    float position[3];
                    if (bFarThinning && fDistance > 1.0f && CEntity::GetPosition(uintptr_t(entity), position))
                    {
                        // Its reach (own + extra) between the game's and the stretched one, the same for a prop at the
                        // same place each time it is made
                        const float own = drawDistance + extra, full = stretched + extra;
                        const float share = float((PositionKey(position) >> 40) & 0xFFFFFF) / float(0x1000000);
                        drawDistance = own + (full - own) * share * share - extra;
                    }
                    else
                        drawDistance = stretched;
                }
            }
        }
        if (auto used = UsedRecords(); used > nUsedMax)
            nUsedMax = used;
        return record;
    }

    SafetyHookInline shRelease;
    void __fastcall Release(uint8_t* generator, void* edx, uint8_t* record, void* list)
    {
        ForgetMade(record);
        if (auto entity = *(uintptr_t*)(record + 8))
        {
            float position[3] = {};
            if (bTrace && CEntity::GetPosition(entity, position))
                Trace(std::format("R {} model={}{}", When(), *(int16_t*)(entity + 0x2E), Where(position)));
            AcquireSRWLockExclusive(&propsLock);
            props.erase(entity);
            ReleaseSRWLockExclusive(&propsLock);
        }
        shRelease.unsafe_fastcall(generator, edx, record, list);
    }

    // GPU grass grows at a queue entry's place instead of its prop (ProceduralGround::HideGrass, by the model of the
    // procedural.dat record its +0xc points to), taken `slack` metres further off than it is
    bool GrassGivesWay(const uint8_t* entry, float slack = 0.0f)
    {
        const auto hide = ProceduralGround::HideGrass;
        const auto definition = *(const uint8_t* const*)(entry + 0xC);
        if (!hide || !pCamera || !pDefinitions || definition < pDefinitions + 8 ||
            definition >= pDefinitions + 8 + size_t(*(uint32_t*)(pDefinitions + 4)) * 0x44)
            return false;
        const int32_t model = *(const int32_t*)(definition + 4);
        if (model < 0 || model > 0xFFFF)
            return false;
        const float* position = (const float*)entry;
        const float dx = position[0] - pCamera[0], dy = position[1] - pCamera[1], dz = position[2] - pCamera[2];
        const float keep = float((PositionKey(position) >> 12) & 0xFFFFFF) / float(0x1000000);
        return hide(uint16_t(model), std::sqrt(dx * dx + dy * dy + dz * dz) + slack, keep);
    }

    // Wanted now: inside the cone, and not where GPU grass grows instead
    bool Wanted(const uint8_t* entry)
    {
        return InCone((const float*)entry) && !GrassGivesWay(entry);
    }

    // ProceduralGroundCovers: grass that runs on under a road or a pavement (terrain kept a little below the street) grew
    // its plants, and GPU grass its blades, up through the asphalt. The manager walks each of its bounds' scan records
    // every frame (CE 0xc87b00: manager, record, centre, an eighth of the polygons); at its call the record's polygons
    // with neither the plants nor the props bit (record +0x48, bits 2i and 2i + 1 of +0x54 slots; its bound through
    // +0x44, +0x38, +4, +0xc as the walk takes it) are looked at here, kCoverBudget a frame, again once the manager's
    // centre moved kCoverRescan or the bound changed. Those facing up, of other materials than the grassy ones
    // (GrassSurface; the bound's +0xd4 and +0x5c calls, as the bound's take makes them) within kCoverReach past the
    // gathering radius are kept in world space as the triangle build makes its corners (CE 0xc87f40: the int16
    // set; a quad's second triangle 0, 2, 3), in kCoverCell cells, until kCoverForget past that radius; walls and grass
    // are marked by record and not looked at again while its bound stays. A
    // place on a grassy triangle is under one when it lies level with it or up to CoverHeight above, there or
    // kCoverAround away (a plant's leaves): the generator's candidates there (CE 0xc093a0: generator, position, normal,
    // definition, triangle) are not queued, props made there before the cover came are let go (ReleaseCovered, by the
    // cells new covers came into), and GPU grass leaves those cells (ProceduralGround::CopyCovers, on its builder's
    // thread). Covers are written on the main thread, which reads them without the lock and never waits for it: a scan
    // the builder's copy holds off goes on the next frame.
    constexpr float kCoverReach = 40.0f;
    constexpr float kCoverForget = 80.0f;
    constexpr float kCoverAround = 0.25f;
    constexpr float kCoverRescan = 16.0f;
    constexpr uint32_t kCoverBudget = 4000;     // polygons a frame
    constexpr size_t kMaxCovers = 200000;
    struct CoverPolygon { ProceduralGround::Cover triangles[2]; int count; uintptr_t vertices; int x0, y0, x1, y1; };
    SRWLOCK coversLock = SRWLOCK_INIT;
    std::unordered_map<uint64_t, CoverPolygon> covers;              // by bound << 32 | polygon
    std::unordered_map<uint64_t, std::vector<uint64_t>> coverCells;
    // By scan record: its bound's vertices and the centre when it was last looked at, the next polygon while it is,
    // its polygons that are no cover (walls, slivers, grass)
    struct CoverScan { uintptr_t vertices = 0; float centre[2] = {}; uint32_t next = 0; bool active = false; std::vector<bool> settled; };
    std::unordered_map<const uint8_t*, CoverScan> coverScans;
    uint32_t nCoverFrame = ~0u, nCoverLeft = 0;
    std::unordered_set<uint64_t> dirtyCells;    // those new covers came into, and the ring round them
    uint32_t nCoverSkipped = 0, nCoverReleased = 0;

    uint8_t GrassSurface(uint32_t material);
    bool GrassSurfacesKnown();

    // The height of `t` at x, y seen from above, false off it
    bool CoverHeightAt(const ProceduralGround::Cover& t, float x, float y, float& z)
    {
        const float* a = t.corner[0];
        const float* b = t.corner[1];
        const float* c = t.corner[2];
        const float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (std::abs(d) < 1e-6f)
            return false;
        const float l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d;
        const float l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d;
        const float l3 = 1.0f - l1 - l2;
        if (l1 < 0.0f || l2 < 0.0f || l3 < 0.0f)
            return false;
        z = l1 * a[2] + l2 * b[2] + l3 * c[2];
        return true;
    }

    // Main thread
    bool UnderCover(const float* place, bool around)
    {
        static constexpr float kOffsets[5][2] = { { 0.0f, 0.0f }, { kCoverAround, 0.0f }, { -kCoverAround, 0.0f }, { 0.0f, kCoverAround }, { 0.0f, -kCoverAround } };
        if (covers.empty())
            return false;
        // The points' cells, each once (one mostly, at most four)
        const int points = around ? 5 : 1;
        uint64_t cells[5];
        int cellCount = 0;
        for (int i = 0; i < points; ++i)
        {
            const uint64_t cell = CoverCellKey(CoverCellOf(place[0] + kOffsets[i][0]), CoverCellOf(place[1] + kOffsets[i][1]));
            if (std::find(cells, cells + cellCount, cell) == cells + cellCount)
                cells[cellCount++] = cell;
        }
        for (int j = 0; j < cellCount; ++j)
        {
            auto cell = coverCells.find(cells[j]);
            if (cell == coverCells.end())
                continue;
            for (const auto key : cell->second)
            {
                auto it = covers.find(key);
                if (it == covers.end())
                    continue;
                for (int t = 0; t < it->second.count; ++t)
                    for (int i = 0; i < points; ++i)
                    {
                        float z;
                        if (CoverHeightAt(it->second.triangles[t], place[0] + kOffsets[i][0], place[1] + kOffsets[i][1], z) &&
                            z - place[2] > -ProceduralGround::CoverBelow && z - place[2] < ProceduralGround::CoverHeight)
                            return true;
                    }
            }
        }
        return false;
    }

    // One of the manager's triangles of a grassy material
    bool OnGrass(const uint8_t* triangle)
    {
        if (!pManager || !triangle)
            return false;
        const auto records = *(const uint8_t* const*)(pManager + 0xF30);
        const uint32_t capacity = *(const uint32_t*)(pManager + 0x18);
        if (!records || triangle < records || triangle >= records + size_t(capacity) * 0x60 || (triangle - records) % 0x60)
            return false;
        return GrassSurface(*(const uint32_t*)(triangle + 0x4c)) != 0;
    }

    void RemoveCover(std::unordered_map<uint64_t, CoverPolygon>::iterator it)    // the lock held
    {
        const auto& c = it->second;
        for (int x = c.x0; x <= c.x1; ++x)
            for (int y = c.y0; y <= c.y1; ++y)
            {
                auto cell = coverCells.find(CoverCellKey(x, y));
                if (cell == coverCells.end())
                    continue;
                auto& keys = cell->second;
                for (size_t i = 0; i < keys.size(); ++i)
                    if (keys[i] == it->first)
                    {
                        keys[i] = keys.back();
                        keys.pop_back();
                        break;
                    }
                if (keys.empty())
                    coverCells.erase(cell);
            }
        covers.erase(it);
    }

    // A polygon without plants or props (the lock held): false when it is no cover while its bound stays
    bool OfferCover(const uint8_t* bound, const uint8_t* record, uint32_t index)
    {
        const uint64_t key = uint64_t(uintptr_t(bound)) << 32 | index;
        const auto vertices = *(const uintptr_t*)(bound + 0xB0);
        auto known = covers.find(key);
        if (known != covers.end() && known->second.vertices == vertices)
            return true;
        const auto polygons = *(const uint8_t* const*)(bound + 0x8C);
        if (!polygons || !vertices || index >= *(const uint32_t*)(bound + 0xCC))
            return false;
        const auto corners = (const uint16_t*)(polygons + size_t(index) * 32 + 0x10);
        const int n = corners[3] ? 4 : 3;
        const float* scale = (const float*)(bound + 0x90);
        const float* offset = (const float*)(bound + 0xA0);
        const float* m = (const float*)record;
        const auto instance = *(const uint8_t* const*)(record + 0x44);
        const bool transform = !record[0x40] && instance && *(const uintptr_t*)(instance + 0x38);
        float w[4][3];
        for (int k = 0; k < n; ++k)
        {
            const auto v = (const int16_t*)vertices + size_t(corners[k]) * 3;
            const float l[3] = { v[0] * scale[0] + offset[0], v[1] * scale[1] + offset[1], v[2] * scale[2] + offset[2] };
            for (int r = 0; r < 3; ++r)
                w[k][r] = transform ? m[r] * l[0] + m[4 + r] * l[1] + m[8 + r] * l[2] + m[12 + r] : l[r];
        }
        // Near enough: kept while it is, looked at again when it comes nearer
        const float* centre = (const float*)(pManager + 0xF40);
        const float reach = std::sqrt((std::max)(*(const float*)(pManager + 0x20), 0.0f)) + kCoverReach;
        bool inReach = false;
        for (int k = 0; k < n && !inReach; ++k)
            inReach = std::abs(w[k][0] - centre[0]) < reach && std::abs(w[k][1] - centre[1]) < reach;
        if (!inReach)
            return true;

        CoverPolygon c{};
        c.vertices = vertices;
        static constexpr int kTriangles[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
        for (int t = 0; t < (n == 4 ? 2 : 1); ++t)
        {
            const float* a = w[kTriangles[t][0]];
            const float* b = w[kTriangles[t][1]];
            const float* d = w[kTriangles[t][2]];
            const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
            const float nx = u[1] * v[2] - u[2] * v[1], ny = u[2] * v[0] - u[0] * v[2], nz = u[0] * v[1] - u[1] * v[0];
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            // Walls and slivers cover no ground
            if (length < 1e-4f || std::abs(nz) < 0.3f * length)
                continue;
            auto& out = c.triangles[c.count++];
            for (int k = 0; k < 3; ++k)
                out.corner[0][k] = a[k], out.corner[1][k] = b[k], out.corner[2][k] = d[k];
        }
        // Its material last: two of the bound's virtual calls, as the walk makes them for the triangle build (+0xd4 the
        // polygon's material index, +0x5c its material)
        if (!c.count)
            return false;
        using Call = uint32_t(__thiscall*)(const uint8_t*, uint32_t);
        const auto vtable = *(const uintptr_t* const*)bound;
        if (GrassSurface(((Call)vtable[0x5c / 4])(bound, ((Call)vtable[0xd4 / 4])(bound, index))) != 0)
            return false;
        if (known != covers.end())
            RemoveCover(known);
        if (covers.size() < kMaxCovers)
        {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (int t = 0; t < c.count; ++t)
                for (int k = 0; k < 3; ++k)
                {
                    x0 = (std::min)(x0, c.triangles[t].corner[k][0]), x1 = (std::max)(x1, c.triangles[t].corner[k][0]);
                    y0 = (std::min)(y0, c.triangles[t].corner[k][1]), y1 = (std::max)(y1, c.triangles[t].corner[k][1]);
                }
            c.x0 = CoverCellOf(x0), c.x1 = CoverCellOf(x1), c.y0 = CoverCellOf(y0), c.y1 = CoverCellOf(y1);
            // Larger than any road piece: left out
            if (c.x1 - c.x0 <= 32 && c.y1 - c.y0 <= 32)
            {
                for (int x = c.x0; x <= c.x1; ++x)
                    for (int y = c.y0; y <= c.y1; ++y)
                        coverCells[CoverCellKey(x, y)].push_back(key);
                for (int x = c.x0 - 1; x <= c.x1 + 1; ++x)
                    for (int y = c.y0 - 1; y <= c.y1 + 1; ++y)
                        dirtyCells.insert(CoverCellKey(x, y));
                covers[key] = c;
            }
        }
        nCoversVersion = nCoversVersion + 1;
        return true;
    }

    // The walk's call for `record`: its polygons without plants or props looked at, kCoverBudget a frame in all
    void ScanForCovers(const uint8_t* record)
    {
        // Until the grassy materials are known any lawn would be a cover of its own grass
        if (!bCovers || !record || !pManager || !GrassSurfacesKnown())
            return;
        const auto instance = *(const uint8_t* const*)(record + 0x44);
        const auto archetype = instance ? *(const uint8_t* const*)(instance + 0x38) : nullptr;
        const auto holder = archetype ? *(const uint8_t* const*)(archetype + 4) : nullptr;
        const auto bound = holder ? *(const uint8_t* const*)(holder + 0xC) : nullptr;
        const auto bits = *(const uint32_t* const*)(record + 0x48);
        if (!bound || !bits)
            return;
        const uint32_t count = *(const uint16_t*)(record + 0x54) / 2;
        if (count != *(const uint32_t*)(bound + 0xCC))
            return;
        if (nCoverFrame != Frame())
        {
            nCoverFrame = Frame();
            nCoverLeft = kCoverBudget;
        }
        const float* centre = (const float*)(pManager + 0xF40);
        const auto vertices = *(const uintptr_t*)(bound + 0xB0);
        auto& scan = coverScans[record];
        if (scan.vertices != vertices || scan.settled.size() != count)
        {
            scan.vertices = vertices;
            scan.settled.assign(count, false);
            scan.active = false;
            scan.centre[0] = scan.centre[1] = 1e30f;
        }
        if (!scan.active)
        {
            const float dx = centre[0] - scan.centre[0], dy = centre[1] - scan.centre[1];
            if (dx * dx + dy * dy < kCoverRescan * kCoverRescan)
                return;
            scan.centre[0] = centre[0], scan.centre[1] = centre[1];
            scan.next = 0;
            scan.active = true;
        }
        if (!nCoverLeft || !TryAcquireSRWLockExclusive(&coversLock))
            return;
        for (; scan.next < count && nCoverLeft; ++scan.next)
        {
            const uint32_t bit = 2 * scan.next;    // and the next, in the same word
            if (scan.settled[scan.next] || ((bits[bit >> 5] >> (bit & 31)) & 3))
                continue;
            nCoverLeft--;
            if (!OfferCover(bound, record, scan.next))
                scan.settled[scan.next] = true;
        }
        ReleaseSRWLockExclusive(&coversLock);
        scan.active = scan.next < count;
    }

    // Covers far behind the gathering radius go
    void ForgetFarCovers()
    {
        if (!pManager || covers.empty())
            return;
        const float* centre = (const float*)(pManager + 0xF40);
        const float reach = std::sqrt((std::max)(*(const float*)(pManager + 0x20), 0.0f)) + kCoverForget;
        if (!TryAcquireSRWLockExclusive(&coversLock))
            return;
        bool removed = false;
        for (auto it = covers.begin(); it != covers.end();)
        {
            const auto& c = it->second;
            const float cx = (c.x0 + c.x1 + 1) * 0.5f * kCoverCell, cy = (c.y0 + c.y1 + 1) * 0.5f * kCoverCell;
            if (std::abs(cx - centre[0]) > reach || std::abs(cy - centre[1]) > reach)
            {
                auto next = std::next(it);
                RemoveCover(it);
                it = next;
                removed = true;
            }
            else
                ++it;
        }
        if (removed)
            nCoversVersion = nCoversVersion + 1;
        ReleaseSRWLockExclusive(&coversLock);
    }

    SafetyHookInline shWalk;
    bool __fastcall Walk(uint8_t* manager, void* edx, const uint8_t* record, const float* centre, int32_t part)
    {
        ScanForCovers(record);
        return shWalk.unsafe_fastcall<bool>(manager, edx, record, centre, part);
    }

    // The generator's candidate, not queued where its grass is under a cover
    SafetyHookInline shCandidate;
    void __fastcall Candidate(uint8_t* generator, void* edx, const float* position, const float* normal, const uint8_t* definition, const uint8_t* triangle)
    {
        if (bCovers && position && OnGrass(triangle) && UnderCover(position, true))
        {
            nCoverSkipped++;
            return;
        }
        shCandidate.unsafe_fastcall(generator, edx, position, normal, definition, triangle);
    }

    // Props of grass made before a cover over them came (a teleport, the scan reaching the road later) go as the game
    // lets them go, their entries not kept: those made in the cells new covers came into
    void ReleaseCovered(uint8_t* generator)
    {
        if (dirtyCells.empty())
            return;
        std::vector<std::pair<uint8_t*, uint8_t*>> covered;
        if (bCovers)
            for (const auto cell : dirtyCells)
            {
                auto made = madeCells.find(cell);
                if (made == madeCells.end())
                    continue;
                for (auto record : made->second)
                {
                    auto from = madeFrom.find(record);
                    if (from == madeFrom.end())
                        continue;
                    const auto entry = from->second.entry;
                    const auto provider = *(uint8_t* const*)(entry + 0x1C);
                    if (!*(uintptr_t*)(record + 8) || *(uint8_t**)(record + 0xC) != provider)
                        continue;
                    if (OnGrass(provider) && UnderCover((const float*)entry, true))
                        covered.emplace_back(record, *(uint8_t**)(entry + 0xC) + 0x38);
                }
            }
        dirtyCells.clear();
        for (auto& [record, list] : covered)
        {
            if (bTrace)
                Trace(std::format("U {}", When()));
            Release(generator, nullptr, record, list);
            nCoverReleased++;
        }
    }

    // Props made from the queue that are now well outside the cone (ProceduralConeRelease), or grass props the camera
    // came near enough for GPU grass to grow in their place (by GrassReleaseSlack more), go as the game lets them go
    // (CE 0xc0a920, from the list of their procedural.dat record, +0x38 of the record the entry's +0xc points to), and
    // their entries are parked
    constexpr float GrassReleaseSlack = 5.0f;
    void ReleaseOutsideCone(uint8_t* generator)
    {
        std::vector<std::pair<uint8_t*, MadeFrom>> outside;
        for (auto& [record, from] : madeFrom)
        {
            if (outside.size() >= ReleasesPerUpdate || parked.size() + (outside.size() + 1) * QueueEntrySize > ParkedLimit * QueueEntrySize)
                break;
            // The game's own removal clears the provider before it lets a record go
            if (!*(uintptr_t*)(record + 8) || *(uintptr_t*)(record + 0xC) != *(uintptr_t*)(from.entry + 0x1C))
                continue;
            if ((bConeRelease && !InCone((float*)from.entry, ReleaseSlack)) || GrassGivesWay(from.entry, GrassReleaseSlack))
                outside.emplace_back(record, from);
        }
        for (auto& [record, from] : outside)
        {
            if (bTrace)
                Trace(std::format("B {}{}", When(), Where((float*)from.entry)));
            Release(generator, nullptr, record, *(uint8_t**)(from.entry + 0xC) + 0x38);
            parked.insert(parked.end(), from.entry, from.entry + QueueEntrySize);
            nConeReleased++;
        }
    }

    // Before the queue's props are made (the game's lock held, the providers it removed already done)
    void BeforeQueue(uint8_t* generator)
    {
        auto queue = *(uint8_t**)(generator + 0x5224);
        auto& count = *(uint16_t*)(generator + 0x5228);
        if (!queue)
            return;

        // Held-over positions of a provider removed since
        auto removals = *(uint8_t**)(generator + 0x522C);
        uint32_t removalCount = *(uint16_t*)(generator + 0x5230);
        uint32_t carriedLeft = std::min<uint32_t>(nCarried, count);
        uint32_t droppedBefore = nDropped;
        if (nCarried && removals && removalCount)
        {
            carriedLeft = 0;
            uint32_t write = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                auto entry = queue + size_t(i) * QueueEntrySize;
                bool removed = false;
                if (i < nCarried)
                {
                    auto provider = *(uintptr_t*)(entry + 0x1C);
                    for (uint32_t j = 0; j < removalCount && !removed; j++)
                        removed = *(uintptr_t*)(removals + size_t(j) * 8) == provider;
                }
                if (removed)
                {
                    nDropped++;
                    if (bTrace)
                    {
                        queuedAt.erase(PositionKey((float*)entry));
                        Trace(std::format("D {}{}", When(), Where((float*)entry)));
                    }
                    continue;
                }
                carriedLeft += i < nCarried;
                if (write != i)
                    memcpy(queue + size_t(write) * QueueEntrySize, entry, QueueEntrySize);
                write++;
            }
            count = uint16_t(write);
        }
        nCarried = 0;

        ReleaseCovered(generator);
        static uint64_t lastForget = 0;
        if (const uint64_t now = GetTickCount64(); now - lastForget >= 2000)
        {
            lastForget = now;
            ForgetFarCovers();
        }

        // The cone: kept positions of a removed provider go, those now inside come back (nearest first is the sort
        // below), and those outside are kept, while there is room; past it they are made
        UpdateCone();
        if (cone.active || ProceduralGround::HideGrass)
        {
            if (bConeRelease || ProceduralGround::HideGrass)
                ReleaseOutsideCone(generator);
            uint32_t capacity = *(uint32_t*)(generator + 0x5220);
            std::vector<uint8_t> keep;
            keep.reserve(parked.size());
            for (size_t offset = 0; offset < parked.size(); offset += QueueEntrySize)
            {
                auto entry = parked.data() + offset;
                bool removed = false;
                if (removals)
                {
                    auto provider = *(uintptr_t*)(entry + 0x1C);
                    for (uint32_t j = 0; j < removalCount && !removed; j++)
                        removed = *(uintptr_t*)(removals + size_t(j) * 8) == provider;
                }
                if (removed)
                {
                    nDropped++;
                    continue;
                }
                if (count < capacity && Wanted(entry))
                {
                    memcpy(queue + size_t(count) * QueueEntrySize, entry, QueueEntrySize);
                    count++;
                    continue;
                }
                keep.insert(keep.end(), entry, entry + QueueEntrySize);
            }
            uint32_t write = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                auto entry = queue + size_t(i) * QueueEntrySize;
                if (!Wanted(entry))
                {
                    if (keep.size() < ParkedLimit * QueueEntrySize)
                    {
                        keep.insert(keep.end(), entry, entry + QueueEntrySize);
                        continue;
                    }
                    nConeFull++;
                }
                if (write != i)
                    memcpy(queue + size_t(write) * QueueEntrySize, entry, QueueEntrySize);
                write++;
            }
            carriedLeft = std::min(carriedLeft, write);
            count = uint16_t(write);
            parked.swap(keep);
            nParkedNow = uint32_t(parked.size() / QueueEntrySize);
        }
        nQueueMax = std::max<uint32_t>(nQueueMax, count);
        lastQueue = count;
        lastNew = count - carriedLeft;
        lastDropped = nDropped - droppedBefore;
        lastMade = count;
        lastWaiting = 0;
        if (bTrace)
        {
            for (uint32_t i = carriedLeft; i < count; i++)
            {
                auto entry = (float*)(queue + size_t(i) * QueueEntrySize);
                queuedAt[PositionKey(entry)] = Now();
                Trace(std::format("Q {}{}", When(), Where(entry)));
            }
        }

        waiting.clear();
        if (nSpawnsPerUpdate <= 0 || count <= uint32_t(nSpawnsPerUpdate))
            return;

        // Nearest first, those ahead before those behind (a prop ahead counts as at its distance, one to the
        // side as twice as far, one behind as three times; ahead is also where a turning camera turns to), and all
        // close to the camera now, so nothing appears late where one already is or is looking
        uint32_t made = uint32_t(nSpawnsPerUpdate);
        if (pCamera)
        {
            const float* forward = pCamera - 8;
            struct Order { float priority, distanceSquared; uint32_t index; };
            std::vector<Order> order(count);
            for (uint32_t i = 0; i < count; i++)
            {
                auto position = (float*)(queue + size_t(i) * QueueEntrySize);
                float dx = position[0] - pCamera[0], dy = position[1] - pCamera[1], dz = position[2] - pCamera[2];
                float distanceSquared = dx * dx + dy * dy + dz * dz;
                float distance = std::sqrt(distanceSquared);
                float facing = distance > 0.001f ? (dx * forward[0] + dy * forward[1] + dz * forward[2]) / distance : 1.0f;
                if (cone.turning && distance > 0.001f)
                    facing = std::max(facing, (dx * cone.turnedTo[0] + dy * cone.turnedTo[1] + dz * cone.turnedTo[2]) / distance);
                float weight = 2.0f - std::clamp(facing, -1.0f, 1.0f);
                order[i] = { distanceSquared * weight * weight, distanceSquared, i };
            }
            std::stable_sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.priority < b.priority; });
            std::vector<uint8_t> sorted(size_t(count) * QueueEntrySize);
            for (uint32_t i = 0; i < count; i++)
                memcpy(sorted.data() + size_t(i) * QueueEntrySize, queue + size_t(order[i].index) * QueueEntrySize, QueueEntrySize);
            memcpy(queue, sorted.data(), sorted.size());
            // Every one within NearAlways is made now, wherever it is in that order
            uint32_t nearCount = 0;
            for (auto& o : order)
                nearCount += o.distanceSquared < NearAlways * NearAlways;
            if (nearCount > made)
            {
                // Bring the near ones that sorted after the budget forward, keeping the order otherwise
                std::vector<uint8_t> first, rest;
                first.reserve(size_t(count) * QueueEntrySize);
                for (uint32_t i = 0; i < count; i++)
                {
                    auto entry = sorted.data() + size_t(i) * QueueEntrySize;
                    bool take = i < made || order[i].distanceSquared < NearAlways * NearAlways;
                    (take ? first : rest).insert((take ? first : rest).end(), entry, entry + QueueEntrySize);
                }
                made = uint32_t(first.size() / QueueEntrySize);
                memcpy(queue, first.data(), first.size());
                memcpy(queue + first.size(), rest.data(), rest.size());
            }
        }
        if (made >= count)
            return;
        waiting.assign(queue + size_t(made) * QueueEntrySize, queue + size_t(count) * QueueEntrySize);
        count = uint16_t(made);
        lastMade = made;
        lastWaiting = uint32_t(waiting.size() / QueueEntrySize);
    }

    // After they're made and the queue emptied: the held-over ones go back to its front
    void AfterQueue(uint8_t* generator)
    {
        auto queue = *(uint8_t**)(generator + 0x5224);
        if (!queue || waiting.empty())
            return;
        auto n = uint32_t(waiting.size() / QueueEntrySize);
        memcpy(queue, waiting.data(), waiting.size());
        *(uint16_t*)(generator + 0x5228) = uint16_t(n);
        nCarried = n;
        nWaitingMax = std::max(nWaitingMax, n);
        waiting.clear();
    }

    // HeavyPropCull: a small entity whose model takes many draws, out of the cascades and thinned far off
    uint32_t DropHeavy(uintptr_t entity, uint32_t phases, float distance)
    {
        if (!(distance >= kHeavyCascadesFrom) || !modelInfos)
            return phases;
        const auto cascades = pCascadePhases ? *pCascadePhases & ~*pScenePhase : 0;
        const auto scene = *pScenePhase;
        if (!(phases & (cascades | scene)))
            return phases;
        // A LOD parent takes its child's phases before this (CE 0xae8587), so leaving the child out would leave both out
        if (*(uintptr_t*)(entity + 0x4c))
            return phases;
        const auto model = *(uint16_t*)(entity + 0x2e);
        if (EntityClass::DrawsPerEntity(model) < kHeavyDraws)
            return phases;
        const auto info = *(uintptr_t*)(modelInfos + model * 4);
        const float radius = info ? *(float*)(info + 0x1c) : 0.0f;
        if (!(radius > 0.0f && radius < kHeavyRadius))
            return phases;
        if (phases & cascades)
        {
            nHeavyCascades++;
            phases &= ~cascades;
        }
        if ((phases & scene) && distance > kHeavyThinFrom)
        {
            float place[3];
            const float keep = CEntity::GetPosition(entity, place) ? float((PositionKey(place) >> 12) & 0xFFFFFF) / float(0x1000000) : 0.0f;
            if (radius < distance * kHeavyScreenSize || keep * distance > kHeavyThinFrom)
            {
                nHeavyScene++;
                phases &= ~scene;
            }
        }
        return phases;
    }

    // FarFadeCull: a small building or prop far off out of the scene in the faint half of the game's fade
    uint32_t DropFaded(uintptr_t entity, uint32_t phases, float distance)
    {
        if (!(distance > kFadeCullFrom) || !modelInfos || !pScenePhase)
            return phases;
        const auto scene = *pScenePhase;
        const uint32_t alpha = *(uint8_t*)(entity + 0x63);
        if (!(phases & scene) || alpha >= kFadeCullAlpha || *(uintptr_t*)(entity + 0x4c))
            return phases;
        const auto info = *(uintptr_t*)(modelInfos + *(uint16_t*)(entity + 0x2e) * 4);
        const float radius = info ? *(float*)(info + 0x1c) : 0.0f;
        if (!(radius > 0.0f && radius < kFadeCullRadius))
            return phases;
        const auto kind = EntityClass::OfEntity((void*)entity);
        if (kind != EntityClass::Kind::Building && kind != EntityClass::Kind::Prop)
            return phases;
        float place[3];
        const float keep = CEntity::GetPosition(entity, place) ? float((PositionKey(place) >> 12) & 0xFFFFFF) / float(0x1000000) : 0.0f;
        if (float(alpha) >= keep * float(kFadeCullAlpha))
            return phases;
        nFadeCulled++;
        return phases & ~scene;
    }

    // Far Prop Instancing takes the entity's commands: its model is drawn in batches and the entity has no matrix of its
    // own (+0x24 bit 0x4000000, a scaled prop, whose command keeps that matrix: CEntity's add, CE 0xa2fe70)
    bool DrawnInstanced(uintptr_t entity)
    {
        return ProceduralGround::DrawnInstanced && !(*(uint32_t*)(entity + 0x24) & 0x4000000) &&
               ProceduralGround::DrawnInstanced(*(uint16_t*)(entity + 0x2e));
    }

    // The render lists: reflection phases off for props, and the scene for small ones far off
    uint32_t DropPhases(uintptr_t entity, uint32_t phases, float distance)
    {
        if (!pReflectionPhases || !pScenePhase)
            return phases;
        // Props have 0x10000 (no shadows) and 0x40000000 from the generator; few other entities have both
        constexpr uint32_t propFlags = 0x10000 | 0x40000000;
        if ((*(uint32_t*)(entity + 0x24) & propFlags) != propFlags)
            return phases;
        const auto reflections = bInReflections ? 0 : *pReflectionPhases & ~*pScenePhase;
        const auto hideGrass = ProceduralGround::HideGrass;
        const auto scene = bSmallFarCull || hideGrass ? *pScenePhase : 0;
        if (!(phases & (reflections | scene)))
            return phases;
        const auto [radius, keep] = FindProp(entity);
        if (radius < 0.0f)
            return phases;
        if (phases & reflections)
        {
            nReflectionsSkipped++;
            phases &= ~reflections;
        }
        if ((phases & scene) && hideGrass && hideGrass(*(uint16_t*)(entity + 0x2e), distance, keep))
        {
            nGrassHidden++;
            phases &= ~scene;
        }
        if ((phases & scene) && bSmallFarCull && radius > 0.0f && (radius < distance * SmallFarScreenSize ||
            (radius < SmallPropRadius && distance > SmallFarThinFrom && keep * distance > SmallFarThinFrom && !DrawnInstanced(entity))))
        {
            nSmallFarCulled++;
            phases &= ~scene;
        }
        return phases;
    }

    SafetyHookInline shUpdate;
    void __fastcall Update(uint8_t* generator, void* edx)
    {
        LARGE_INTEGER start, end, frequency;
        QueryPerformanceCounter(&start);
        ApplyBuildingReserve();
        shUpdate.unsafe_fastcall(generator, edx);
        UpdateGatherRadius();
        QueryPerformanceCounter(&end);
        QueryPerformanceFrequency(&frequency);
        double ms = double(end.QuadPart - start.QuadPart) * 1000.0 / double(frequency.QuadPart);
        fUpdateMs += ms;
        fUpdateMaxMs = std::max(fUpdateMaxMs, ms);
        nUpdates++;

        if (bTrace && pCamera)
        {
            // The camera: where, its heading (as the game takes it, 0 north) and pitch, and how fast it moves
            const float* forward = pCamera - 8;
            float heading = std::atan2(-forward[0], forward[1]) * 57.29578f;
            float pitch = std::asin(std::clamp(forward[2], -1.0f, 1.0f)) * 57.29578f;
            auto now = Now();
            float speed = 0.0f;
            if (lastCameraTime && now > lastCameraTime)
            {
                float dx = pCamera[0] - lastCamera[0], dy = pCamera[1] - lastCamera[1], dz = pCamera[2] - lastCamera[2];
                speed = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f / float(now - lastCameraTime);
            }
            std::copy_n(pCamera, 3, lastCamera);
            lastCameraTime = now;
            float grassFar = pManager ? *(float*)(pManager + 0x14) : 0.0f;
            float gather = pManager ? *(float*)(pManager + 0x1C) : 0.0f;
            Trace(std::format("U {} n={} cam={:.1f},{:.1f},{:.1f} heading={:.0f} pitch={:.0f} speed={:.1f} grassfar={:.0f} gather={:.0f} "
                "gather_now={:.0f} triangles={} buildings_free={} queue={} new={} made={} waiting={} parked={} dropped={} failed={} "
                "records={} prop_matrices={} render_cache={}/{} drawable_reclaims={} ms={:.3f}",
                When(), updateIndex++, pCamera[0], pCamera[1], pCamera[2], heading, pitch, speed, grassFar, gather, fGatherNow,
                LONG(nTriangles), BuildingsFree(), lastQueue, lastNew, lastMade, lastWaiting, parked.size() / QueueEntrySize,
                lastDropped, lastFailed, UsedRecords(), *(uint32_t*)generator, InterlockedExchange(&nRenderCachePeak, 0),
                nRenderCache, InterlockedExchange(&nDrawableReclaims, 0), ms));
            lastQueue = lastNew = lastMade = lastWaiting = lastDropped = lastFailed = 0;
        }
    }

    // The grassy materials' indices, from the material manager by their names (as the game takes them, CE 0xcf2d59:
    // [0x18b8968] vtable +0x18 by name): GRASS and SHORT_GRASS by the game's own strings, GRASS_PATCHY and GRASS_LONG
    // (materials.dat only, no tag on PC either) by ours; -1 until found, or when the manager gives a missing name's index
    uintptr_t* pMaterialManager = nullptr;
    const char* szGrassNames[4] = { nullptr, nullptr, "GRASS_PATCHY", "GRASS_LONG" };
    int32_t nGrassMaterials[4] = { -1, -1, -1, -1 };
    bool bGrassMaterialsLooked = false;
    uint8_t GrassSurface(uint32_t material)
    {
        if (!bGrassMaterialsLooked && pMaterialManager && *pMaterialManager)
        {
            bGrassMaterialsLooked = true;
            auto manager = *pMaterialManager;
            using Find = uint32_t(__thiscall*)(uintptr_t, const char*);
            const auto find = (*(Find**)manager)[6];
            const int32_t missing = int32_t(find(manager, "FUSIONFIX_NO_SUCH_MATERIAL") & 0xFF);
            for (int i = 0; i < 4; i++)
            {
                nGrassMaterials[i] = int32_t(find(manager, szGrassNames[i]) & 0xFF);
                if (i >= 2 && nGrassMaterials[i] == missing)
                    nGrassMaterials[i] = -1;
            }
            Log("GrassSurfaces", std::format("GRASS {}, SHORT_GRASS {}, GRASS_PATCHY {}, GRASS_LONG {} (a missing name gives {})",
                nGrassMaterials[0], nGrassMaterials[1], nGrassMaterials[2], nGrassMaterials[3], missing));
        }
        const int32_t surface = int32_t(material & 0xFF);
        for (int i = 0; i < 4; i++)
            if (surface == nGrassMaterials[i])
                return uint8_t(i + 1);
        return 0;
    }

    bool GrassSurfacesKnown()
    {
        GrassSurface(0);
        return nGrassMaterials[0] >= 0 || nGrassMaterials[1] >= 0;
    }

    // GatherGrassSurfaces: where the manager marks each polygon of a bound for plants (bit 2i, the tag has a PLANT
    // record) and props (bit 2i + 1, a PROCOBJ record) as it takes the bound (CE 0xc868f0), a grassy material with
    // neither gets the props bit; the generator makes no props for a tag without PROCOBJ records (GeneratorProps)
    bool bForceProps = false;

    SafetyHookInline shGeneratorProps;
    int32_t __fastcall GeneratorProps(uint8_t* generator, void* edx, uint8_t* triangle)
    {
        // The tag's first PROCOBJ record (CE 0x16c8fb0 + 0x5c10, -1 for none): the game reads the record before the
        // table and compares its tag
        if (ProceduralGround::GatherGrassSurfaces && pDefinitions && *(int32_t*)(pDefinitions + 0x5c10 + size_t(triangle[0x4d]) * 8) < 0)
            return 0;
        return shGeneratorProps.unsafe_fastcall<int32_t>(generator, edx, triangle);
    }

    // And lets none go when the manager lets such a triangle go (CE 0xc0a880, the generator's requests at +0x522c:
    // provider, tag): the game takes record -1 there too, and with its tag matching (lawns are tag 0) walked the
    // bytes before the table as a list of props (AV in 0xc0a8c6)
    SafetyHookInline shGeneratorRemoveProps;
    void __fastcall GeneratorRemoveProps(uint8_t* generator, void* edx, void* provider, uint32_t tag)
    {
        if (pDefinitions && tag <= 0xFF && *(int32_t*)(pDefinitions + 0x5c10 + size_t(tag) * 8) < 0)
            return;
        shGeneratorRemoveProps.unsafe_fastcall(generator, edx, provider, tag);
    }

    // Triangles of the ground the manager gathers (CE 0xc86540 fills one: centre +0x30, +0x55 bit 0 grass,
    // bit 1 props; null when it has neither) and lets go (CE 0xc87010)
    SafetyHookInline shAddTriangle;
    uint8_t* __fastcall AddTriangle(uint8_t* triangle, void* edx, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8)
    {
        auto result = shAddTriangle.unsafe_fastcall<uint8_t*>(triangle, edx, a1, a2, a3, a4, a5, a6, a7, a8);
        if (result && ProceduralGround::OnAdded)
        {
            // Corners at +0, +0x10, +0x20 (CE 0xc86540)
            ProceduralGround::Triangle t{};
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k)
                    t.corner[c][k] = *(float*)(result + c * 0x10 + k * 4);
            t.tag = uint8_t(*(uint32_t*)(result + 0x4c) >> 8);
            t.surface = GrassSurface(*(uint32_t*)(result + 0x4c));
            ProceduralGround::OnAdded(result, t);
        }
        if (bTrace && result)
        {
            InterlockedIncrement(&nTriangles);
            auto flags = *(uint8_t*)(result + 0x55);
            Trace(std::format("T {} grass={} props={} area={:.1f}{}", When(), flags & 1, (flags >> 1) & 1,
                *(float*)(result + 0x48), Where((float*)(result + 0x30))));
        }
        return result;
    }

    SafetyHookInline shRemoveTriangle;
    void __fastcall RemoveTriangle(uint8_t* triangle, void* edx)
    {
        if (ProceduralGround::OnRemoved)
            ProceduralGround::OnRemoved(triangle);
        if (bTrace)
        {
            InterlockedDecrement(&nTriangles);
            auto flags = *(uint8_t*)(triangle + 0x55);
            Trace(std::format("X {} grass={} props={}{}", When(), flags & 1, (flags >> 1) & 1, Where((float*)(triangle + 0x30))));
        }
        shRemoveTriangle.unsafe_fastcall(triangle, edx);
    }

    void Log(const char* component, const std::string& text)
    {
        FusionLog::WriteText("Procedural", component, text);
    }
}

class ProceduralProps
{
public:
    ProceduralProps()
    {
        using namespace Procedural;

        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");

            // [PROCEDURAL]
            auto logLevel = iniReader.ReadInteger("DEBUG", "ProceduralLog", 0);
            bLog = logLevel >= 1;
            bTrace = logLevel >= 2;
            nPool = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralPool", 16384), int32_t(VanillaRecords), 65535);
            nMatrixLimit = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralMatrixLimit", 4096), int32_t(VanillaRecords), nPool);
            fDensity = std::clamp(iniReader.ReadFloat("PROCEDURAL", "ProceduralDensity", 1.0f), 0.25f, 2.0f);
            fDistance = DistanceFromPref(FusionFixSettings.Get("PREF_PROCEDURAL_DISTANCE"));
            nGroundTriangles = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralGroundTriangles", 8192), 512, 32768);

            // The manager's capacity of ground triangles (mov [ecx+0x18], 0x200 in its constructor). Its three arrays
            // (0x60 bytes a triangle, a 0x10 header) are made in its init (CE 0xc87150), here from our memory and not
            // the game's heap. They are let go there and in its shutdown (CE 0xc87450, also run on loading a save or
            // switching episodes, CE 0x5c10e1); a free missed there handed our memory to the game's heap: MMA10
            if (auto capacity = hook::pattern("C7 41 18 00 02 00 00 C7 41 1C 00 00 A0 42"); !capacity.empty())
            {
                auto allocs = hook::pattern("50 E8 ? ? ? ? 83 C4 04 85 C0 74 07 89 38 83 C0 10");
                auto free1 = hook::pattern("50 E8 ? ? ? ? 83 C4 04 C7 86 30 0F 00 00 00 00 00 00");
                auto free2 = hook::pattern("50 E8 ? ? ? ? 8B 4C 24 10 83 C4 04 C7 07 00 00 00 00");
                auto free3 = hook::pattern("A1 ? ? ? ? 85 C0 74 ? 83 C0 F0 50 E8 ? ? ? ? 83 C4 04 C7 05");
                auto free4 = hook::pattern("8B 86 ? ? ? ? 85 C0 74 ? 83 C0 F0 50 E8 ? ? ? ? 83 C4 04 C7 86");
                if (allocs.size() == 2 && !free1.empty() && !free2.empty() && !free3.empty() && !free4.empty())
                {
                    struct TriangleArrays
                    {
                        static void* __cdecl Alloc(size_t size) { return _aligned_malloc(size, 16); }
                        static void __cdecl Free(void* memory) { _aligned_free(memory); }
                    };
                    for (size_t i = 0; i < 2; i++)
                        injector::MakeCALL(allocs.get(i).get<void>(1), TriangleArrays::Alloc, true);
                    injector::MakeCALL(free1.get_first(1), TriangleArrays::Free, true);
                    injector::MakeCALL(free2.get_first(1), TriangleArrays::Free, true);
                    injector::MakeCALL(free3.get_first(13), TriangleArrays::Free, true);
                    injector::MakeCALL(free4.get_first(14), TriangleArrays::Free, true);
                    injector::WriteMemory(capacity.get_first(3), uint32_t(nGroundTriangles), true);
                }
                else
                    injector::WriteMemory(capacity.get_first(3), uint32_t(std::min(nGroundTriangles, 2048)), true);
            }

            // The pool of entity matrices (7000 in CE 0xa32210) is a cache: any entity that keeps only a position and a
            // heading gets one from it when something needs its whole matrix (CE 0xa30750), and when none is free the
            // least recently used is taken from its owner (CE 0xa33cb0). Props drawn further out made that working set
            // outgrow 7000 (a trace: full half the time, 36000 taken in two minutes, props holding 4000), so trees and
            // cars away from the player flickered. Its nodes are linked by pointers only; the array is ours
            // (operator new at CE 0xa3218c, delete at CE 0xa341c8) so the game's heap doesn't pay for it
            nMatrixPool = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralMatrixPool", 28000), 7000, 65536);
            if (auto size = hook::pattern("68 58 1B 00 00 B9 ? ? ? ? C7 05"); !size.empty())
            {
                auto alloc = hook::pattern("51 E8 ? ? ? ? 89 83 E0 01 00 00");
                auto release = hook::pattern("FF B6 E0 01 00 00 E8 ? ? ? ? 83 C4 04");
                if (nMatrixPool != 7000 && !alloc.empty() && !release.empty())
                {
                    struct MatrixNodes
                    {
                        static void* __cdecl Alloc(size_t size) { return _aligned_malloc(size, 16); }
                        static void __cdecl Free(void* memory) { _aligned_free(memory); }
                    };
                    injector::MakeCALL(alloc.get_first(1), MatrixNodes::Alloc, true);
                    injector::MakeCALL(release.get_first(6), MatrixNodes::Free, true);
                    injector::WriteMemory(size.get_first(1), uint32_t(nMatrixPool), true);
                }
                else
                    nMatrixPool = 7000;
            }

            // Every entity with its model in gets a drawable reference (CE 0xa30983 -> 0xaf4710, kept in entity +0x34),
            // 0x14-byte nodes made once at start (CE 0xaf48f0: 13000, ExtendedLimits makes it 20000). When none is free
            // the game takes models from other entities (CE 0xa8a990 on the list at 0x1173750) and tries again; with
            // thousands of props that happened all the time, and trees, fences, cars, the player's too, lost their
            // models for a frame or more. More of them, from our memory. The game frees the array in the session's
            // shutdown (CE 0xaf4b80 from 0xb1e970, on exit too); left to the game's free, our block stopped the game
            // there or broke its heap, so that free is ours as well
            nDrawableRefs = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralDrawableRefs", 40000), 13000, 200000);
            if (auto size = hook::pattern("68 ? ? ? ? E8 ? FF FF FF C3"); !size.empty())
            {
                auto init = injector::GetBranchDestination(size.get_first(5)).as_int();
                auto call = (uint8_t*)init + 0x1D;
                auto release = hook::pattern("56 8B F1 FF 76 50 E8 ? ? ? ? 83 C4 04 C7 46 50 00 00 00 00 5E C3");
                if (*(uint8_t*)(init + 0x1C) == 0x51 && *call == 0xE8 && !release.empty() && *size.get_first<uint32_t>(1) < uint32_t(nDrawableRefs))
                {
                    struct DrawableRefs
                    {
                        static void* __cdecl Alloc(size_t size) { return _aligned_malloc(size, 16); }
                        static void __cdecl Free(void* memory) { _aligned_free(memory); }
                    };
                    injector::MakeCALL(call, DrawableRefs::Alloc, true);
                    injector::MakeCALL(release.get_first(6), DrawableRefs::Free, true);
                    injector::WriteMemory(size.get_first(1), uint32_t(nDrawableRefs), true);
                }
            }

            // A building (props too) or type 5 entity given a drawable reference also goes on the list at 0x1173750
            // (CE 0xaf4778 -> 0xa8a420), 12-byte nodes, 10000 of them (CE 0xa8abd0). With more references than that the
            // list ran full at x3, and the game dropped the model of the first entity on it (CE 0xa8a4a9): that frees a
            // drawable reference, which takes the spinlock at 0x15f8b40 that CE 0xaf4710 still held, and the game hung
            // for good. As many nodes as references, from the game's heap
            if (auto list = hook::pattern("68 10 27 00 00 83 C1 08 E8 ? ? ? ? C3"); !list.empty())
                injector::WriteMemory(list.get_first(1), uint32_t(nDrawableRefs), true);

            // The entities a frame draws go to one buffer of 8-byte entries (CE 0xae5ca0 returns its start, 0x159b768,
            // room for 9692; CE 0xae2520 adds one without a check). The sector scans stop taking entities once it holds
            // 8192 (cmp ecx, 0x10000 at CE 0xae927b and 0xae941b), and whatever the scans meet after that is not drawn
            // that frame: looking at a whole district at x3 that was cars, trees and the player, a different set each
            // frame. Ours holds ProceduralRenderCache entries, and the scans stop at three quarters of it
            nRenderCache = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralRenderCache", 32768), 9692, 1 << 20);
            if (auto scans = hook::pattern("E8 ? ? ? ? 8B 0D ? ? ? ? 2B C8 83 E1 F8 81 F9 00 00 01 00"); scans.size() == 2)
            {
                auto start = (uint8_t*)injector::GetBranchDestination(scans.get_first(0)).as_int();
                if (start == (uint8_t*)injector::GetBranchDestination(scans.get(1).get<void>(0)).as_int() && start[0] == 0xB8 && start[5] == 0xC3)
                {
                    static std::vector<uint8_t> renderCache;
                    renderCache.assign(size_t(nRenderCache) * 8 + 64, 0);
                    injector::WriteMemory(start + 1, uint32_t(uintptr_t(renderCache.data())), true);
                    uint32_t limit = uint32_t(nRenderCache) / 4 * 3 * 8;
                    for (size_t i = 0; i < scans.size(); i++)
                        injector::WriteMemory(scans.get(i).get<void>(18), limit, true);
                    // With the trace on, how full the scans found it (ecx its bytes in use, at cmp ecx, limit)
                    static std::vector<SafetyHookMid> fillHooks;
                    if (bTrace)
                        for (size_t i = 0; i < scans.size(); i++)
                            fillHooks.push_back(safetyhook::create_mid(scans.get(i).get<void>(16), [](SafetyHookContext& regs)
                            {
                                LONG used = LONG(regs.ecx / 8);
                                for (LONG peak = nRenderCachePeak; used > peak; peak = nRenderCachePeak)
                                    if (InterlockedCompareExchange(&nRenderCachePeak, used, peak) == peak)
                                        break;
                            }));
                }
                else
                    nRenderCache = 9692;
            }
            else
                nRenderCache = 9692;

            // Props are buildings: the free places of that pool, for the trace
            if (auto pool = hook::pattern("8B 0D ? ? ? ? E8 ? ? ? ? 3D F4 01 00 00"); !pool.empty())
            {
                pBuildingPool = *pool.get_first<uint8_t**>(2);
                pBuildingReserve = pool.get_first<uint32_t>(12);
            }
            ReadLiveSettings(iniReader);
            IniReload::Add(ReadLiveSettings);

            // With the trace on, the game out of drawable references: mov ecx, 0x1173750; call CE 0xa8a990 in CE 0xaf4710
            if (bTrace)
            {
                if (auto reclaim = hook::pattern("B9 ? ? ? ? E8 ? ? ? ? 8B CF E8 ? ? ? ? 8B F0 85 F6 75 0A"); !reclaim.empty())
                {
                    static auto ReclaimHook = safetyhook::create_mid(reclaim.get_first(0), [](SafetyHookContext&)
                    {
                        InterlockedIncrement(&nDrawableReclaims);
                    });
                }
            }

            // Generator init: the extra records and the larger queue
            auto pattern = hook::pattern("53 55 56 8B D9 57 8D 73 14 BF 00 02 00 00");
            auto resetFree = hook::pattern("FF B5 24 52 00 00 E8");
            auto destructorFree = hook::pattern("FF B6 24 52 00 00 E8");
            if (pattern.empty() || resetFree.empty() || destructorFree.empty())
            {
                Log("Pool", "the procedural object generator was not found (not the Complete Edition?), nothing changed");
                return;
            }
            ListPush = (decltype(ListPush))injector::GetBranchDestination(pattern.get_first(0x32)).as_int();
            shGeneratorInit = safetyhook::create_inline(pattern.get_first(0), GeneratorInit);
            static auto ResetFreeHook = safetyhook::create_mid(resetFree.get_first(0), [](SafetyHookContext& regs)
            {
                RestoreGameQueue((uint8_t*)regs.ebp);
            });
            static auto DestructorFreeHook = safetyhook::create_mid(destructorFree.get_first(0), [](SafetyHookContext& regs)
            {
                RestoreGameQueue((uint8_t*)regs.esi);
            });

            // The generator's own record of the entities its 2dfx scan found, instead of the shared stamp: its
            // increment is jumped over, the mark (movzx eax, stamp; ...; mov [edi+0x3c], eax) becomes an insert,
            // and the check whether a provider was found this update (cmp [ecx+0x3c], eax; je) a lookup
            {
                auto increment = hook::pattern("66 A1 ? ? ? ? 55 57 BF FF FF 00 00 BD 01 00 00 00 66 3B C7 73 0A");
                auto check = hook::pattern("0F B7 05 ? ? ? ? 8B 4A 10 8B 32 39 41 3C 74");
                auto mark = hook::pattern("0F B7 05 ? ? ? ? 32 C9 89 47 3C");
                if (!increment.empty() && !check.empty() && !mark.empty())
                {
                    static auto ClearHook = safetyhook::create_mid(increment.get_first(0), [](SafetyHookContext&)
                    {
                        scanned.clear();
                    });
                    injector::MakeJMP(increment.get_first(18), increment.get_first(45), true);

                    injector::MakeNOP(mark.get_first(0), 7, true);
                    injector::MakeNOP(mark.get_first(9), 3, true);
                    static auto MarkHook = safetyhook::create_mid(mark.get_first(0), [](SafetyHookContext& regs)
                    {
                        scanned.insert(regs.edi);
                    });

                    injector::MakeNOP(check.get_first(0), 7, true);
                    injector::MakeNOP(check.get_first(12), 3, true);
                    static auto CheckHook = safetyhook::create_mid(check.get_first(12), [](SafetyHookContext& regs)
                    {
                        constexpr uint32_t ZF = 0x40;
                        if (scanned.contains(regs.ecx))
                            regs.eflags |= ZF;
                        else
                            regs.eflags &= ~ZF;
                    });
                }
            }

            // The camera position the generator measures from (movss xmm2, [0x128e340] in CE 0xc09440)
            pattern = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ? 56 8B 75 0C");
            if (!pattern.empty())
                pCamera = *pattern.get_first<float*>(12);

            // Caps on props with a matrix of their own: the matrices come from a pool that evicts the
            // least used when full, so they're raised less than the records. Both or neither: another mod that
            // raised them first (grass fix with RenderedObjectCapacity) leaves its own value, and the log says so
            auto limit1 = hook::pattern("81 3D ? ? ? ? 00 02 00 00 7C ? 57 B9");
            auto limit2 = hook::pattern("81 F9 00 02 00 00 0F 8D ? ? ? ? 8B 4F 08 E8");
            if (!limit1.empty() && !limit2.empty())
            {
                injector::WriteMemory(limit1.get_first(6), uint32_t(nMatrixLimit), true);
                injector::WriteMemory(limit2.get_first(2), uint32_t(nMatrixLimit), true);
            }
            else
                nMatrixLimit = 0;

            // 2dfx props of one entity: count by density, and no more than the 64 candidates there are room for.
            // At cvttss2si edi, xmm1: xmm1 is the count, [esp+0x5c] the candidates of this entity so far
            pattern = hook::pattern("F3 0F 59 C8 F3 0F 2C F9 89 BC B4 F8 00 00 00");
            if (!pattern.empty())
            {
                static auto CountHook = safetyhook::create_mid(pattern.get_first(4), [](SafetyHookContext& regs)
                {
                    auto count = regs.xmm1.f32[0] * fDensity;
                    auto room = float(std::max(0, EntityCandidates - *(int32_t*)(regs.esp + 0x5C)));
                    if (count > room)
                    {
                        count = room;
                        nCandidatesCapped++;
                    }
                    regs.xmm1.f32[0] = count;
                });
            }

            // The 30 m box around the camera that finds entities with 2dfx props
            pattern = hook::pattern("C7 84 24 38 02 00 00 00 00 F0 41 C7 84 24 34 02 00 00 00 00 F0 41 C7 84 24 30 02 00 00 00 00 F0 41");
            if (!pattern.empty())
            {
                for (int i = 0; i < 3; i++)
                    pRadius[i] = pattern.get_first<float>(7 + i * 11);
                ApplyEntityRadius();
            }

            // The box keeps 128 entities at most: counted when the loop leaves full. Hooked at xor esi, esi, not
            // the mov before it: an empty box jumps straight to the xor (je CE 0xc0ac90), which landed in the last
            // byte of the hook's jmp and ran the rest as call [eax+...] (crash at the start of a new game)
            pattern = hook::pattern("8B 5C 24 18 33 F6 85 ED 7E");
            if (!pattern.empty())
            {
                static auto EntitiesHook = safetyhook::create_mid(pattern.get_first(4), [](SafetyHookContext& regs)
                {
                    if (regs.ebp >= 0x80)
                        nEntitiesCapped++;
                });
            }

            // Manager distances: at its init, before its first build and the grass fade are set from them,
            // and on each change of Detail Quality
            pattern = hook::pattern("E8 ? ? ? ? F3 0F 10 46 14 83 EC 08 F3 0F 11 44 24 04 F3 0F 10 46 10 F3 0F 11 04 24 E8");
            if (!pattern.empty())
            {
                SetGrassFade = (decltype(SetGrassFade))injector::GetBranchDestination(pattern.get_first(0x1D)).as_int();
                static auto InitDistancesHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                {
                    ScaleManagerDistances((uint8_t*)regs.esi);
                });
            }
            pattern = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 59 05 ? ? ? ? F3 0F 58 05 ? ? ? ? F3 0F 11 41 14");
            if (!pattern.empty())
            {
                pDetailExtra = *pattern.get_first<float*>(4);
                shUpdateDistances = safetyhook::create_inline(pattern.get_first(0), UpdateDistances);
            }
            // The manager's update, right after it copied the camera into its centre (edi the manager)
            pattern = hook::pattern("89 43 0C E8 ? ? ? ? 83 C4 08 8B CF E8");
            if (!pattern.empty())
            {
                static auto CentreHook = safetyhook::create_mid(pattern.get_first(8), [](SafetyHookContext& regs)
                {
                    ShiftGatherCentre((uint8_t*)regs.edi);
                });
            }

            // procedural.dat's PROCOBJ records, kept as loaded so density and distance can change later
            pattern = hook::pattern("B8 F8 42 00 00 E8 ? ? ? ? A1 ? ? ? ? 33 C4 89 84 24 F4 42 00 00");
            if (!pattern.empty())
                shLoadDefinitions = safetyhook::create_inline(pattern.get_first(0), LoadDefinitions);

            pattern = hook::pattern("55 8B EC 83 E4 F0 81 EC 98 00 00 00 56 57 89 4C 24 14 E8");
            if (!pattern.empty())
                shCreate = safetyhook::create_inline(pattern.get_first(0), Create);

            pattern = hook::pattern("56 8B 74 24 08 57 8B 46 10 8B F9");
            if (!pattern.empty())
                shRelease = safetyhook::create_inline(pattern.get_first(0), Release);

            // The generator's update: the gathering radius each frame, and its time for the log
            pattern = hook::pattern("81 EC B8 02 00 00 A1 ? ? ? ? 33 C4 89 84 24 B4 02 00 00 53 8B D9");
            if (!pattern.empty())
                shUpdate = safetyhook::create_inline(pattern.get_first(0), Update);

            // The trace's T/X lines and ProceduralGround (GPU grass)
            {
                pattern = hook::pattern("55 8B EC 83 E4 F0 83 EC 18 8B 55 10 56 8B 75 0C 57 8B 7D 08");
                if (!pattern.empty())
                    shAddTriangle = safetyhook::create_inline(pattern.get_first(0), AddTriangle);
                pattern = hook::pattern("56 8B F1 F6 46 55 04 C7 46 48 00 00 00 00");
                if (!pattern.empty())
                    shRemoveTriangle = safetyhook::create_inline(pattern.get_first(0), RemoveTriangle);
            }

            // ProceduralGroundCovers: the manager's walk of a scan record (its first instructions, CE 0xc87b00), and the
            // generator's candidates
            {
                auto walk = hook::pattern("55 8B EC 83 E4 F0 83 EC 58 56 57 8B 7D 08 8B F1 8B 47 44");
                auto candidate = hook::pattern("55 8B EC 83 E4 F0 8B D1 83 EC 20 0F B7 82 28 52 00 00");
                if (!walk.empty() && !candidate.empty())
                {
                    shWalk = safetyhook::create_inline(walk.get_first(0), Walk);
                    shCandidate = safetyhook::create_inline(candidate.get_first(0), Candidate);
                }
                else
                    Log("GroundCovers", "not found: grass under roads grows through them");
            }

            // GatherGrassSurfaces: the material manager and the names the game finds GRASS and SHORT_GRASS by (CE 0xcf2d59),
            // the bound's polygon marks (CE 0xc86903 the material in eax, 0xc86926 the props bit in al) and the generator's
            // props of a triangle (CE 0xc0a800)
            {
                auto names = hook::pattern("8B 0D ? ? ? ? 68 ? ? ? ? 8B 01 FF 50 18 89 47 44 8B 0D ? ? ? ? 68 ? ? ? ? 8B 01 FF 50 18 89 47 48");
                auto material = hook::pattern("8B F0 C1 EE 08 81 E6 FF 00 00 00 56 B9");
                auto marks = hook::pattern("8A D8 E8 ? ? ? ? 8B 7F 48 8B 4C 24 10 88 44 24 28");
                auto props = hook::pattern("51 89 0C 24 E8 ? ? ? ? 84 C0 74 06 33 C0 59 C2 04 00 53 55 8B 6C 24 10 56 0F B6 5D 4D");
                auto removeProps = hook::pattern("83 EC 08 8B 44 24 10 53 8B 1C C5 ? ? ? ? 56 8B F3 C1 E6 04 03 F3 C1 E6 02");
                if (!names.empty() && !material.empty() && !marks.empty() && !props.empty() && !removeProps.empty())
                {
                    pMaterialManager = *names.get_first<uintptr_t*>(2);
                    szGrassNames[0] = *names.get_first<const char*>(7);
                    szGrassNames[1] = *names.get_first<const char*>(0x1A);
                    static auto MaterialHook = safetyhook::create_mid(material.get_first(0), [](SafetyHookContext& regs)
                    {
                        bForceProps = ProceduralGround::GatherGrassSurfaces && GrassSurface(regs.eax) != 0;
                    });
                    static auto MarksHook = safetyhook::create_mid(marks.get_first(7), [](SafetyHookContext& regs)
                    {
                        // bl the plants bit, al the props bit
                        if (bForceProps && !(regs.ebx & 0xFF) && !(regs.eax & 0xFF))
                            regs.eax = (regs.eax & ~0xFFu) | 1;
                        bForceProps = false;
                    });
                    shGeneratorProps = safetyhook::create_inline(props.get_first(0), GeneratorProps);
                    shGeneratorRemoveProps = safetyhook::create_inline(removeProps.get_first(0), GeneratorRemoveProps);
                }
                else
                    Log("GrassSurfaces", "not found: no GPU grass on ground without props");
            }

            // The queue: before its loop of creations (esi the generator) and after it, the queue emptied
            pattern = hook::pattern("33 C0 33 FF 66 3B 86 28 52 00 00");
            auto queueEnd = hook::pattern("66 89 86 30 52 00 00 8B CE 5E E9");
            if (!pattern.empty() && !queueEnd.empty())
            {
                static auto BeforeQueueHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                {
                    BeforeQueue((uint8_t*)regs.esi);
                });
                static auto AfterQueueHook = safetyhook::create_mid(queueEnd.get_first(0), [](SafetyHookContext& regs)
                {
                    AfterQueue((uint8_t*)regs.esi);
                });
            }

            // Render lists, where the shadow phases are dropped for entities with 0x10000: esi the entity,
            // ecx its phases (the second also keeps them at [esp+0x28]), edi its entry, the distance at +4
            pattern = hook::pattern("85 0D ? ? ? ? 75 ? 0F B6 46 63 3D EF 00 00 00");
            auto listA = hook::pattern("F7 43 40 00 01 00 00 F3 0F 10 47 04");
            auto listB = hook::pattern("89 4C 24 28 A9 00 00 00 08");
            if (!pattern.empty() && !listA.empty() && !listB.empty())
            {
                pReflectionPhases = *pattern.get_first<uint32_t*>(2);
                // CDrawEntityDC's execute: movzx eax, word [esi+0x30]; mov eax, [eax*4 + model infos]
                if (auto infos = hook::pattern("0F B7 46 30 8B 04 85 ? ? ? ? 85 C0"); !infos.empty())
                    modelInfos = *infos.get_first<uintptr_t>(7);
                pScenePhase = *listB.get_first<uint32_t*>(0xD); // test [0x159af24], edx after the je
                // mov eax, [0x159b760]; not eax; and ecx, eax just before the first: the cascades the game drops for props
                if (*listA.get_first<uint8_t>(-9) == 0xA1)
                    pCascadePhases = *listA.get_first<uint32_t*>(-8);
                static auto ListAHook = safetyhook::create_mid(listA.get_first(0), [](SafetyHookContext& regs)
                {
                    regs.ecx = DropPhases(regs.esi, uint32_t(regs.ecx), *(float*)(regs.edi + 4));
                });
                static auto ListBHook = safetyhook::create_mid(listB.get_first(4), [](SafetyHookContext& regs)
                {
                    regs.ecx = DropPhases(regs.esi, uint32_t(regs.ecx), *(float*)(regs.edi + 4));
                    *(uint32_t*)(regs.esp + 0x28) = uint32_t(regs.ecx);
                });
                // HeavyPropCull: where every entity goes into the phases' lists (CE 0xae43a0(phases, entity, distance,
                // bucket), called from twelve places: the two above see only some of them, objects made from the map's
                // dummies come through others), at its entry: [esp+4] phases, [esp+8] entity, [esp+0xc] distance
                if (auto add = hook::pattern("83 EC 10 53 8B 5C 24 18 85 DB 0F 84 ? ? ? ? 33 C9 8B C3 85 DB 78"); !add.empty())
                {
                    static auto AddHook = safetyhook::create_mid(add.get_first(0), [](SafetyHookContext& regs)
                    {
                        auto& phases = *(uint32_t*)(regs.esp + 4);
                        const auto entity = *(uintptr_t*)(regs.esp + 8);
                        if (!phases || !entity)
                            return;
                        if (bHeavyPropCull)
                            phases = DropHeavy(entity, phases, *(float*)(regs.esp + 0xc));
                        if (bFarFadeCull && phases)
                            phases = DropFaded(entity, phases, *(float*)(regs.esp + 0xc));
                    });
                }
            }

            FusionFixSettings.SetCallback("PREF_PROCEDURAL_DISTANCE", [](int32_t value)
            {
                fDistance = DistanceFromPref(value);
                ApplyEntityRadius();
                ApplyDefinitions();
                // As the game does for Detail Quality: the manager takes the new distances on its next update
                if (pManager && shUpdateDistances)
                    UpdateDistances(pManager, nullptr);
                Log("Settings", std::format("distance x{:.2f}", fDistance));
            });

            Log("Settings", std::format("ground triangles {} (game 512), records {} (game 512), matrix limit {}, matrix pool {} (game 7000), drawable references {} (game 13000), render cache {} (game 9692, scans to 8192), density x{:.2f}, "
                "distance x{:.2f}, in reflections {}, spawns an update {}",
                nGroundTriangles, Records(), nMatrixLimit ? std::to_string(nMatrixLimit) : "not set (already changed by another mod)", nMatrixPool, nDrawableRefs, nRenderCache, fDensity, fDistance, bInReflections ? "yes" : "no", nSpawnsPerUpdate));
        };

        // With ProceduralLog, every 30 s while the generator runs: its time, how many props, and what hit a limit
        FusionFix::onGameProcessEvent() += []()
        {
            using namespace Procedural;
            if (!bLog)
                return;
            static uint64_t lastFlush = 0;
            if (bTrace && GetTickCount64() - lastFlush >= 1000)
            {
                lastFlush = GetTickCount64();
                FlushTrace();
            }
            static uint64_t last = GetTickCount64();
            auto now = GetTickCount64();
            if (now - last < 30000)
                return;
            last = now;
            if (!nUpdates)
                return;
            AcquireSRWLockShared(&propsLock);
            auto alive = props.size();
            ReleaseSRWLockShared(&propsLock);
            Log("Stats", std::format("update {:.3f} ms on average, {:.3f} at most, over {}; props out {}, records in use {} (most {} of {}); "
                "queue at most {}, held over at most {}, dropped {}; parked {}, made with the cone full {}, let go behind {}; reflections skipped {}, small far left out {}, grass left to GPU grass {}, heavy out of cascades {}, heavy left out {}, faded far left out {}; under covers not queued {}, let go {}, covers kept {}; box full {}, candidates capped {}",
                fUpdateMs / nUpdates, fUpdateMaxMs, nUpdates, alive, UsedRecords(), nUsedMax, Records(),
                nQueueMax, nWaitingMax, nDropped, nParkedNow, nConeFull, nConeReleased, nReflectionsSkipped, nSmallFarCulled, nGrassHidden, nHeavyCascades, nHeavyScene, nFadeCulled, nCoverSkipped, nCoverReleased, covers.size(), nEntitiesCapped, nCandidatesCapped));
            nCoverSkipped = nCoverReleased = 0;
            fUpdateMs = fUpdateMaxMs = 0.0;
            nUpdates = 0;
            nQueueMax = nWaitingMax = 0;
            nReflectionsSkipped = nSmallFarCulled = nGrassHidden = nHeavyCascades = nHeavyScene = nFadeCulled = 0;
            nConeFull = 0;
            nConeReleased = 0;
        };
    }
} ProceduralProps;

namespace ProceduralGround
{
    uint8_t* Definitions()
    {
        return Procedural::pDefinitions;
    }

    uint32_t CoversVersion()
    {
        return Procedural::nCoversVersion;
    }

    void CopyCovers(std::vector<Cover>& out, float x, float y, float reach)
    {
        using namespace Procedural;
        out.clear();
        if (!bCovers)
            return;
        AcquireSRWLockShared(&coversLock);
        for (const auto& [key, c] : covers)
        {
            if ((c.x1 + 1) * kCoverCell < x - reach || c.x0 * kCoverCell > x + reach || (c.y1 + 1) * kCoverCell < y - reach || c.y0 * kCoverCell > y + reach)
                continue;
            out.insert(out.end(), c.triangles, c.triangles + c.count);
        }
        ReleaseSRWLockShared(&coversLock);
    }
}
