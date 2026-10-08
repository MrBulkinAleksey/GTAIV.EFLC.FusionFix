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
// Props come from procedural.dat by surface (spacing, how far they spawn) and from 2dfx effects of map
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
//  - The 64 candidates of one entity's 2dfx props are capped, which the game itself never checked.
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
    int32_t nDrawBufferMB = 8;
    float fDensity = 1.0f;
    float fDistance = 1.0f;
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

    std::vector<uint8_t> extraRecords;
    // The larger queue is ours, not from the game's heap: the game's own buffer of 512 is kept aside and
    // put back before the game frees it (reset CE 0xc0aa2e, destructor CE 0xc08dc5)
    std::vector<uint8_t> queueStorage;
    void* pGameQueue = nullptr;
    float fBaseNear = 0.0f;             // the manager's own near (20), read before it's first changed

    struct Definition { float spacing, inverseSquare, distanceSquared; };
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
    volatile LONG nEvictions = 0;       // matrices the game took from an entity to give to another, since the last update

    // The pool of entity matrices (CE 0x12ddf60, 7000 at CE 0xa32210) that props with a matrix share with tilted
    // map entities, vehicles, peds and objects: 0x50-byte nodes at +0x1e0, the owner at +0x40 (0 when free).
    // When none is free, the least recently used is taken from its owner (CE 0xa33cb0), which keeps only its heading
    uint8_t* pMatrixPool = nullptr;
    // The two buffers the frame's draw commands go to (CE 0x1175c58), with the most a frame used and how many
    // times one ran out (the game then starts over at its beginning, over this frame's own commands)
    volatile LONG nDrawPeak = 0, nDrawWraps = 0;
    uint32_t nMatrixPoolSize = 0;
    float lastCamera[3] = {};
    uint64_t lastCameraTime = 0;

    // The props that are out, for the render lists
    std::unordered_set<uintptr_t> props;
    SRWLOCK propsLock = SRWLOCK_INIT;

    bool IsProp(uintptr_t entity)
    {
        AcquireSRWLockShared(&propsLock);
        bool found = props.contains(entity);
        ReleaseSRWLockShared(&propsLock);
        return found;
    }

    // Matrices of the pool in use, and how many of them props hold
    std::pair<uint32_t, uint32_t> MatrixPoolUse()
    {
        auto nodes = pMatrixPool ? *(uint8_t**)(pMatrixPool + 0x1E0) : nullptr;
        if (!nodes)
            return {};
        uint32_t used = 0, byProps = 0;
        AcquireSRWLockShared(&propsLock);
        for (uint32_t i = 0; i < nMatrixPoolSize; i++)
        {
            if (auto owner = *(uintptr_t*)(nodes + i * 0x50 + 0x40))
            {
                used++;
                byProps += props.contains(owner);
            }
        }
        ReleaseSRWLockShared(&propsLock);
        return { used, byProps };
    }

    // Positions held over to the next update, first in the queue then
    std::vector<uint8_t> waiting;
    uint32_t nCarried = 0;

    // Statistics for the log
    uint32_t nUsedMax = 0;
    uint32_t nCandidatesCapped = 0, nEntitiesCapped = 0;
    uint32_t nQueueMax = 0, nWaitingMax = 0, nDropped = 0;
    uint32_t nReflectionsSkipped = 0;
    uint32_t nUpdates = 0;
    double fUpdateMs = 0.0, fUpdateMaxMs = 0.0;

    int32_t BuildingsFree()
    {
        auto pool = pBuildingPool ? *pBuildingPool : nullptr;
        return pool ? int32_t(*(uint32_t*)(pool + 8) - *(uint32_t*)(pool + 0x14)) : -1;
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
        nCarried = 0;
        waiting.clear();
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
            *(float*)(record + 0x10) = definitions[i].distanceSquared * fDistance * fDistance;
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
        if (record)
        {
            if (auto entity = *(uint8_t**)(record + 8))
            {
                AcquireSRWLockExclusive(&propsLock);
                props.insert(uintptr_t(entity));
                ReleaseSRWLockExclusive(&propsLock);

                // Drawn while its own distance times the phase's multiplier (about 1) plus Detail Quality's extra
                // is more than the camera's distance (CE 0xaec13a), so for the slider to take that whole reach
                // k times further the prop's own distance becomes k * own + (k - 1) * extra
                auto& drawDistance = *(float*)(entity + 0x50);
                if (fDistance != 1.0f && std::isfinite(drawDistance) && drawDistance > 0.0f && drawDistance < 2000.0f)
                {
                    float extra = pDetailExtra ? *pDetailExtra : 0.0f;
                    drawDistance = drawDistance * fDistance + (fDistance - 1.0f) * std::max(extra, 0.0f);
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
        // side as twice as far, one behind as three times), and all close to the camera now, so nothing
        // appears late where one already is or is looking
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

    // The render lists: reflection phases off for props
    uint32_t DropReflections(uintptr_t entity, uint32_t phases)
    {
        if (bInReflections || !pReflectionPhases || !pScenePhase)
            return phases;
        auto reflections = *pReflectionPhases & ~*pScenePhase;
        if (!(phases & reflections))
            return phases;
        // Props have 0x10000 (no shadows) and 0x40000000 from the generator; few other entities have both
        constexpr uint32_t propFlags = 0x10000 | 0x40000000;
        if ((*(uint32_t*)(entity + 0x24) & propFlags) != propFlags || !IsProp(entity))
            return phases;
        nReflectionsSkipped++;
        return phases & ~reflections;
    }

    SafetyHookInline shUpdate;
    void __fastcall Update(uint8_t* generator, void* edx)
    {
        LARGE_INTEGER start, end, frequency;
        QueryPerformanceCounter(&start);
        shUpdate.unsafe_fastcall(generator, edx);
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
            auto matrices = MatrixPoolUse();
            Trace(std::format("U {} n={} cam={:.1f},{:.1f},{:.1f} heading={:.0f} pitch={:.0f} speed={:.1f} grassfar={:.0f} gather={:.0f} "
                "triangles={} buildings_free={} queue={} new={} made={} waiting={} dropped={} failed={} records={} "
                "prop_matrices={} matrices={}/{} by_props={} evicted={} draw_kb={}/{} draw_wraps={} ms={:.3f}",
                When(), updateIndex++, pCamera[0], pCamera[1], pCamera[2], heading, pitch, speed, grassFar, gather,
                LONG(nTriangles), BuildingsFree(), lastQueue, lastNew, lastMade, lastWaiting, lastDropped, lastFailed, UsedRecords(),
                *(uint32_t*)generator, matrices.first, nMatrixPoolSize, matrices.second, InterlockedExchange(&nEvictions, 0),
                InterlockedExchange(&nDrawPeak, 0) >> 10, nDrawBufferMB << 10, InterlockedExchange(&nDrawWraps, 0), ms));
            lastQueue = lastNew = lastMade = lastWaiting = lastDropped = lastFailed = 0;
        }
    }

    // Triangles of the ground the manager gathers (CE 0xc86540 fills one: centre +0x30, +0x55 bit 0 grass,
    // bit 1 props; null when it has neither) and lets go (CE 0xc87010)
    SafetyHookInline shAddTriangle;
    uint8_t* __fastcall AddTriangle(uint8_t* triangle, void* edx, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8)
    {
        auto result = shAddTriangle.unsafe_fastcall<uint8_t*>(triangle, edx, a1, a2, a3, a4, a5, a6, a7, a8);
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
            nPool = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralPool", 16384), int32_t(VanillaRecords), 65535);
            nMatrixLimit = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralMatrixLimit", 4096), int32_t(VanillaRecords), nPool);
            fDensity = std::clamp(iniReader.ReadFloat("PROCEDURAL", "ProceduralDensity", 1.0f), 0.25f, 2.0f);
            fDistance = DistanceFromPref(FusionFixSettings.Get("PREF_PROCEDURAL_DISTANCE"));
            nGroundTriangles = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralGroundTriangles", 8192), 512, 32768);

            // The manager's capacity of ground triangles (mov [ecx+0x18], 0x200 in its constructor). Its three arrays
            // (0x60 bytes a triangle, a 0x10 header) are made and let go only in its init (CE 0xc87150), here from our
            // memory and not the game's heap
            if (auto capacity = hook::pattern("C7 41 18 00 02 00 00 C7 41 1C 00 00 A0 42"); !capacity.empty())
            {
                auto allocs = hook::pattern("50 E8 ? ? ? ? 83 C4 04 85 C0 74 07 89 38 83 C0 10");
                auto free1 = hook::pattern("50 E8 ? ? ? ? 83 C4 04 C7 86 30 0F 00 00 00 00 00 00");
                auto free2 = hook::pattern("50 E8 ? ? ? ? 8B 4C 24 10 83 C4 04 C7 07 00 00 00 00");
                if (allocs.size() == 2 && !free1.empty() && !free2.empty())
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

            // The frame's draw commands go to one of two buffers of 2 MB (CE 0x8deb60 makes them, 0x8de140 swaps them
            // each frame). Run out, and the game starts over at the buffer's beginning (CE 0x8dc3a0, 0x8dc7e0, 0x8dcad0),
            // over commands of the same frame, so with many more props drawn anything could vanish for a frame: cars,
            // the player's too, trees, fences. Theirs are from our memory here, larger, and the four checks follow
            nDrawBufferMB = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralDrawBuffer", 8), 2, 32);
            {
                auto alloc1 = hook::pattern("68 10 27 20 00 8B F1 E8");
                auto alloc2 = hook::pattern("68 10 27 20 00 89 06 E8");
                auto release = hook::pattern("FF 36 E8 ? ? ? ? FF 76 04 C7 06 00 00 00 00 E8");
                auto check1 = hook::pattern("89 0D ? ? ? ? 81 F9 00 00 20 00");
                auto check2 = hook::pattern("3D 00 00 20 00 72 ? 8B 47 14");
                auto check3 = hook::pattern("3D 00 00 20 00 72 ? 8B 41 14");
                auto check4 = hook::pattern("3D 00 00 20 00 73 ? 8B 41 10");
                auto swap = hook::pattern("56 8B F1 B9 ? ? ? ? E8 ? ? ? ? B8 01 00 00 00 2B 46 10");
                if (nDrawBufferMB != 2 && !alloc1.empty() && !alloc2.empty() && !release.empty() && !check1.empty() &&
                    !check2.empty() && !check3.empty() && !check4.empty())
                {
                    struct DrawBuffers
                    {
                        static void* __cdecl Alloc(size_t size) { return _aligned_malloc(size, 16); }
                        static void __cdecl Free(void* memory) { _aligned_free(memory); }
                    };
                    uint32_t size = uint32_t(nDrawBufferMB) << 20;
                    injector::WriteMemory(alloc1.get_first(1), size + 0x2710, true);
                    injector::WriteMemory(alloc2.get_first(1), size + 0x2710, true);
                    injector::MakeCALL(alloc1.get_first(7), DrawBuffers::Alloc, true);
                    injector::MakeCALL(alloc2.get_first(7), DrawBuffers::Alloc, true);
                    injector::MakeCALL(release.get_first(2), DrawBuffers::Free, true);
                    injector::MakeCALL(release.get_first(16), DrawBuffers::Free, true);
                    injector::WriteMemory(check1.get_first(8), size, true);
                    injector::WriteMemory(check2.get_first(1), size, true);
                    injector::WriteMemory(check3.get_first(1), size, true);
                    injector::WriteMemory(check4.get_first(1), size, true);
                }
                else
                    nDrawBufferMB = 2;

                // Before the swap: how far this frame's two streams got (+0x18, +0x1c) and whether either ran out (+8, +9)
                if (!swap.empty())
                {
                    static auto SwapHook = safetyhook::create_mid(swap.get_first(0), [](SafetyHookContext& regs)
                    {
                        if (!bTrace)
                            return;
                        auto buffers = (uint8_t*)regs.ecx;
                        LONG used = LONG(std::max(*(uint32_t*)(buffers + 0x18), *(uint32_t*)(buffers + 0x1C)));
                        for (LONG peak = nDrawPeak; used > peak; peak = nDrawPeak)
                            if (InterlockedCompareExchange(&nDrawPeak, used, peak) == peak)
                                break;
                        if (*(buffers + 8) || *(buffers + 9))
                            InterlockedIncrement(&nDrawWraps);
                    });
                }
            }

            // Props are buildings: the free places of that pool, for the trace
            if (auto pool = hook::pattern("8B 0D ? ? ? ? E8 ? ? ? ? 3D F4 01 00 00"); !pool.empty())
                pBuildingPool = *pool.get_first<uint8_t**>(2);
            bInReflections = iniReader.ReadInteger("PROCEDURAL", "ProceduralInReflections", 0) != 0;
            nSpawnsPerUpdate = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralSpawnsPerUpdate", 128), 0, 0xFFFF);
            auto logLevel = iniReader.ReadInteger("PROCEDURAL", "ProceduralLog", 0);
            bLog = logLevel >= 1;
            bTrace = logLevel >= 2;

            // The matrix pool (push 7000, mov ecx, pool in its init) and its evictions (mov ecx, [pool's oldest];
            // mov ecx, [ecx+0x40]; call CE 0xa33cb0), both for the trace
            if (bTrace)
            {
                if (auto init = hook::pattern("68 ? ? ? ? B9 ? ? ? ? C7 05 ? ? ? ? 00 00 80 3F"); !init.empty())
                {
                    nMatrixPoolSize = *init.get_first<uint32_t>(1);
                    pMatrixPool = *init.get_first<uint8_t*>(6);
                }
                auto evictions = hook::pattern("8B 0D ? ? ? ? 8B 49 40 E8 ? ? ? ? B9");
                static std::vector<SafetyHookMid> evictionHooks;
                for (size_t i = 0; i < evictions.size(); i++)
                    evictionHooks.push_back(safetyhook::create_mid(evictions.get(i).get<void>(9), [](SafetyHookContext&)
                    {
                        InterlockedIncrement(&nEvictions);
                    }));
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

            // The camera position the generator measures from (movss xmm2, [0x128e340] in CE 0xc09440)
            pattern = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ? 56 8B 75 0C");
            if (!pattern.empty())
                pCamera = *pattern.get_first<float*>(12);

            // Caps on props with a matrix of their own: the matrices come from a pool that evicts the
            // least used when full, so they're raised less than the records
            pattern = hook::pattern("81 3D ? ? ? ? 00 02 00 00 7C ? 57 B9");
            if (!pattern.empty())
                injector::WriteMemory(pattern.get_first(6), uint32_t(nMatrixLimit), true);
            pattern = hook::pattern("81 F9 00 02 00 00 0F 8D ? ? ? ? 8B 4F 08 E8");
            if (!pattern.empty())
                injector::WriteMemory(pattern.get_first(2), uint32_t(nMatrixLimit), true);

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

            // The box keeps 128 entities at most: counted when the loop leaves full
            pattern = hook::pattern("8B 5C 24 18 33 F6 85 ED 7E");
            if (!pattern.empty())
            {
                static auto EntitiesHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
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

            // The generator's update is timed only for the log
            pattern = hook::pattern("81 EC B8 02 00 00 A1 ? ? ? ? 33 C4 89 84 24 B4 02 00 00 53 8B D9");
            if (bLog && !pattern.empty())
                shUpdate = safetyhook::create_inline(pattern.get_first(0), Update);

            if (bTrace)
            {
                pattern = hook::pattern("55 8B EC 83 E4 F0 83 EC 18 8B 55 10 56 8B 75 0C 57 8B 7D 08");
                if (!pattern.empty())
                    shAddTriangle = safetyhook::create_inline(pattern.get_first(0), AddTriangle);
                pattern = hook::pattern("56 8B F1 F6 46 55 04 C7 46 48 00 00 00 00");
                if (!pattern.empty())
                    shRemoveTriangle = safetyhook::create_inline(pattern.get_first(0), RemoveTriangle);
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
            // ecx its phases (the second also keeps them at [esp+0x28])
            pattern = hook::pattern("85 0D ? ? ? ? 75 ? 0F B6 46 63 3D EF 00 00 00");
            auto listA = hook::pattern("F7 43 40 00 01 00 00 F3 0F 10 47 04");
            auto listB = hook::pattern("89 4C 24 28 A9 00 00 00 08");
            if (!pattern.empty() && !listA.empty() && !listB.empty())
            {
                pReflectionPhases = *pattern.get_first<uint32_t*>(2);
                pScenePhase = *listB.get_first<uint32_t*>(0xD); // test [0x159af24], edx after the je
                static auto ListAHook = safetyhook::create_mid(listA.get_first(0), [](SafetyHookContext& regs)
                {
                    regs.ecx = DropReflections(regs.esi, uint32_t(regs.ecx));
                });
                static auto ListBHook = safetyhook::create_mid(listB.get_first(4), [](SafetyHookContext& regs)
                {
                    regs.ecx = DropReflections(regs.esi, uint32_t(regs.ecx));
                    *(uint32_t*)(regs.esp + 0x28) = uint32_t(regs.ecx);
                });
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

            Log("Settings", std::format("ground triangles {} (game 512), records {} (game 512), matrix limit {}, matrix pool {} (game 7000), draw buffers {} MB (game 2), density x{:.2f}, "
                "distance x{:.2f}, in reflections {}, spawns an update {}",
                nGroundTriangles, Records(), nMatrixLimit, nMatrixPool, nDrawBufferMB, fDensity, fDistance, bInReflections ? "yes" : "no", nSpawnsPerUpdate));
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
                "queue at most {}, held over at most {}, dropped {}; reflections skipped {}; box full {}, candidates capped {}",
                fUpdateMs / nUpdates, fUpdateMaxMs, nUpdates, alive, UsedRecords(), nUsedMax, Records(),
                nQueueMax, nWaitingMax, nDropped, nReflectionsSkipped, nEntitiesCapped, nCandidatesCapped));
            fUpdateMs = fUpdateMaxMs = 0.0;
            nUpdates = 0;
            nQueueMax = nWaitingMax = 0;
            nReflectionsSkipped = 0;
        };
    }
} ProceduralProps;
