module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
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
//  - ProceduralPool: more records than 512, so props don't vanish or pop where many are around.
//  - Grass & Props Distance: grass, surface props and 2dfx props go further (and props' own draw distance).
//  - ProceduralDensity: more or fewer props (not grass).
//  - The 64 candidates of one entity's 2dfx props are capped, which the game itself never checked.
//
// And what keeps the cost of more props down (the game already keeps them out of shadows: it sets
// entity +0x24 0x10000 on each, and the render lists then drop the shadow phases, CE 0xae85a5/0xae7058):
//  - ProceduralSmallPropDistance: props drawn no further than this keep their distance (litter, cans).
//  - ProceduralInReflections = 0: the render lists drop the reflection phases (water, type 0x11, and type
//    0x1f, mask CE 0x159af28) for props, as for shadows.
//  - ProceduralSpawnsPerUpdate: the queue of positions makes no more props than this an update, the rest
//    wait for the next ones, so a jump in distance or a new area doesn't stall a frame. Waiting positions
//    of a provider the game removed meanwhile (its requests at +0x522c, processed first) are dropped.
//  - The log has the time of the generator's update (CE 0xc0aa90) and how many props are out.
namespace Procedural
{
    constexpr uint32_t VanillaRecords = 512;
    constexpr uint32_t RecordSize = 0x18;
    constexpr uint32_t QueueEntrySize = 0x20;
    constexpr int32_t EntityCandidates = 64;
    constexpr float VanillaEntityRadius = 30.0f;
    constexpr float QueryLead = 40.0f;

    // Settings
    int32_t nPool = 4096;
    int32_t nMatrixLimit = 1024;
    float fDensity = 1.0f;
    float fDistance = 1.0f;
    float fSmallProp = 30.0f;
    bool bInReflections = false;
    int32_t nSpawnsPerUpdate = 128;

    // The game's
    uint8_t* pGenerator = nullptr;      // CE 0x1683290
    uint8_t* pManager = nullptr;        // CE 0x16fb6a0, taken from the first call that has it
    uint8_t* pDefinitions = nullptr;    // CE 0x16c8fb0, procedural.dat's PROCOBJ records
    void(__fastcall* ListPush)(void* list, void* edx, void* node) = nullptr;
    void* (__cdecl* GameAlloc)(size_t) = nullptr;
    void(__cdecl* GameFree)(void*) = nullptr;
    void(__cdecl* SetGrassFade)(float nearDistance, float farDistance) = nullptr;
    float* pRadius[3] = {};             // the 30 m half extents of the 2dfx box, in code
    uint32_t* pReflectionPhases = nullptr; // CE 0x159af28, a bit a reflection render phase

    std::vector<uint8_t> extraRecords;
    float fBaseNear = 0.0f;             // the manager's own near (20), read before it's first changed

    struct Definition { float spacing, inverseSquare, distanceSquared; };
    std::vector<Definition> definitions;  // as procedural.dat set them

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
    uint32_t nDistances[5] = {};        // props' own draw distances: under 15, 30, 60, 100 m, and further

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
        if (queueSize > VanillaRecords && GameAlloc && GameFree)
        {
            if (auto queue = GameAlloc(size_t(queueSize) * QueueEntrySize))
            {
                GameFree(*(void**)(generator + 0x5224));
                *(void**)(generator + 0x5224) = queue;
                *(uint16_t*)(generator + 0x5228) = 0;
                *(uint16_t*)(generator + 0x522A) = uint16_t(queueSize);
                *(uint32_t*)(generator + 0x5220) = queueSize;
            }
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

    // A prop's own draw distance (entity +0x50) goes with the distance it now spawns at, but for small ones
    SafetyHookInline shCreate;
    uint8_t* __fastcall Create(uint8_t* generator, void* edx, void* a1, void* a2, void* a3, void* a4, void* a5)
    {
        auto record = shCreate.unsafe_fastcall<uint8_t*>(generator, edx, a1, a2, a3, a4, a5);
        if (record)
        {
            if (auto entity = *(uint8_t**)(record + 8))
            {
                AcquireSRWLockExclusive(&propsLock);
                props.insert(uintptr_t(entity));
                ReleaseSRWLockExclusive(&propsLock);

                auto& drawDistance = *(float*)(entity + 0x50);
                if (std::isfinite(drawDistance) && drawDistance > 0.0f && drawDistance < 2000.0f)
                {
                    nDistances[drawDistance < 15.0f ? 0 : drawDistance < 30.0f ? 1 : drawDistance < 60.0f ? 2 : drawDistance < 100.0f ? 3 : 4]++;
                    if (fDistance != 1.0f && drawDistance >= fSmallProp)
                        drawDistance *= fDistance;
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
        if (nCarried && removals && removalCount)
        {
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
                    continue;
                }
                if (write != i)
                    memcpy(queue + size_t(write) * QueueEntrySize, entry, QueueEntrySize);
                write++;
            }
            count = uint16_t(write);
        }
        nCarried = 0;
        nQueueMax = std::max<uint32_t>(nQueueMax, count);

        waiting.clear();
        if (nSpawnsPerUpdate > 0 && count > uint32_t(nSpawnsPerUpdate))
        {
            waiting.assign(queue + size_t(nSpawnsPerUpdate) * QueueEntrySize, queue + size_t(count) * QueueEntrySize);
            count = uint16_t(nSpawnsPerUpdate);
        }
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
        if (bInReflections || !pReflectionPhases || !(phases & *pReflectionPhases))
            return phases;
        // Props have 0x10000 (no shadows) and 0x40000000 from the generator; few other entities have both
        constexpr uint32_t propFlags = 0x10000 | 0x40000000;
        if ((*(uint32_t*)(entity + 0x24) & propFlags) != propFlags || !IsProp(entity))
            return phases;
        nReflectionsSkipped++;
        return phases & ~*pReflectionPhases;
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
            nPool = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralPool", 4096), int32_t(VanillaRecords), 32768);
            nMatrixLimit = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralMatrixLimit", 1024), int32_t(VanillaRecords), nPool);
            fDensity = std::clamp(iniReader.ReadFloat("PROCEDURAL", "ProceduralDensity", 1.0f), 0.25f, 2.0f);
            fDistance = DistanceFromPref(FusionFixSettings.Get("PREF_PROCEDURAL_DISTANCE"));
            fSmallProp = std::clamp(iniReader.ReadFloat("PROCEDURAL", "ProceduralSmallPropDistance", 30.0f), 0.0f, 1000.0f);
            bInReflections = iniReader.ReadInteger("PROCEDURAL", "ProceduralInReflections", 0) != 0;
            nSpawnsPerUpdate = std::clamp(iniReader.ReadInteger("PROCEDURAL", "ProceduralSpawnsPerUpdate", 128), 0, 0xFFFF);

            // Generator init: the extra records and the larger queue
            auto pattern = hook::pattern("53 55 56 8B D9 57 8D 73 14 BF 00 02 00 00");
            auto queue = hook::pattern("68 00 40 00 00 E8 ? ? ? ? 83 C4 04 89 83 24 52 00 00");
            auto queueFree = hook::pattern("FF B5 24 52 00 00 E8");
            if (pattern.empty() || queue.empty() || queueFree.empty())
            {
                Log("Pool", "the procedural object generator was not found (not the Complete Edition?), nothing changed");
                return;
            }
            ListPush = (decltype(ListPush))injector::GetBranchDestination(pattern.get_first(0x32)).as_int();
            GameAlloc = (decltype(GameAlloc))injector::GetBranchDestination(queue.get_first(5)).as_int();
            GameFree = (decltype(GameFree))injector::GetBranchDestination(queueFree.get_first(6)).as_int();
            shGeneratorInit = safetyhook::create_inline(pattern.get_first(0), GeneratorInit);

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
                shUpdateDistances = safetyhook::create_inline(pattern.get_first(0), UpdateDistances);

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

            pattern = hook::pattern("81 EC B8 02 00 00 A1 ? ? ? ? 33 C4 89 84 24 B4 02 00 00 53 8B D9");
            if (!pattern.empty())
                shUpdate = safetyhook::create_inline(pattern.get_first(0), Update);

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

            Log("Settings", std::format("records {} (game 512), matrix limit {}, density x{:.2f}, distance x{:.2f}, "
                "small props under {:.0f} m keep theirs, in reflections {}, spawns an update {}",
                Records(), nMatrixLimit, fDensity, fDistance, fSmallProp, bInReflections ? "yes" : "no", nSpawnsPerUpdate));
        };

        // Every 30 s while the generator runs: its time, how many props, and what hit a limit
        FusionFix::onGameProcessEvent() += []()
        {
            using namespace Procedural;
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
                "queue at most {}, held over at most {}, dropped {}; reflections skipped {}; box full {}, candidates capped {}; "
                "own draw distance <15 m {}, <30 {}, <60 {}, <100 {}, further {}",
                fUpdateMs / nUpdates, fUpdateMaxMs, nUpdates, alive, UsedRecords(), nUsedMax, Records(),
                nQueueMax, nWaitingMax, nDropped, nReflectionsSkipped, nEntitiesCapped, nCandidatesCapped,
                nDistances[0], nDistances[1], nDistances[2], nDistances[3], nDistances[4]));
            fUpdateMs = fUpdateMaxMs = 0.0;
            nUpdates = 0;
            nQueueMax = nWaitingMax = 0;
            nReflectionsSkipped = 0;
        };
    }
} ProceduralProps;
