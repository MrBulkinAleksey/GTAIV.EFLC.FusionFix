module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

export module entityclass;

import common;
import inireload;
import renderskip;
import temporal;

// What kind of entity each draw of the scene belongs to: the map's own geometry, which never moves;
// a prop (an object), which can be pushed, knocked over or broken; a vehicle or a ped. Props use the
// same world shaders and materials as the ground, so the shaders cannot tell them apart.
//
// The game thread marks the entity's kind in a spare byte of each draw command as it is made:
// CDrawEntityDC +0x37 (made only by CEntity's draw list add, 0xa2fe70), CDrawFragDC and
// CDrawFragTypeDC +0x5f (made by it and by CDynamicEntity's, 0xb10360, which vehicles and objects
// with fragments go through). On the render thread the execute (vtable slot 1) of those and of
// CDrawSkinnedEntityDC (always a prop) calls the hooks added here before and after the draw, in every
// render phase.
//
// EntityDrawLog = 1 ([DEBUG]): every 600 scene frames, the draws of each kind and the CPU time their
// execute took, per frame and per target the game had locked (RenderSkip::CurrentTarget), to
// GTAIV.EFLC.FusionFix.EntityClass.log: what leaving a kind out of a pass would save.
export namespace EntityClass
{
    enum class Kind : uint8_t
    {
        Unknown,
        Building,   // CBuilding, CAnimatedBuilding, CInteriorInst, CPortalInst
        Prop,       // CObject, CDummyObject, CCutsceneObject and the rest, skinned entities
        Vehicle,    // CVehicle and what derives from it
        Ped,        // CPed and what derives from it
        Kinds
    };

    using DrawHook = void (*)(Kind kind);

    // Hooks are added from module constructors or init events, before the first frame: the list is
    // read on the render thread without a lock.
    void AddDrawHooks(DrawHook before, DrawHook after);

    // Whether the game's code was found and hooked.
    bool Installed();

    // By the classes in the entity's vtable RTTI (its own and its bases).
    Kind OfEntity(void* entity);
}

namespace
{
    using EntityClass::Kind;
    constexpr ptrdiff_t kDrawEntityKindByte = 0x37;
    constexpr ptrdiff_t kDrawFragKindByte = 0x5f;     // CDrawFragDC and CDrawFragTypeDC, 0x60 bytes
    using DrawExecute = void(__thiscall*)(void*);
    DrawExecute drawEntityExecute = nullptr;
    DrawExecute drawFragExecute = nullptr;
    DrawExecute drawFragTypeExecute = nullptr;
    DrawExecute drawSkinnedExecute = nullptr;
    bool bInstalled = false;

    std::vector<std::pair<EntityClass::DrawHook, EntityClass::DrawHook>>& Hooks()
    {
        static std::vector<std::pair<EntityClass::DrawHook, EntityClass::DrawHook>> hooks;
        return hooks;
    }

    // Which draw command a draw came from
    enum Source : uint8_t { FromEntity, FromFrag, FromFragType, FromSkinned, Sources };
    constexpr const char* SourceNames[Sources] = { "entity", "fragment", "fragment type", "skinned" };
    constexpr const char* KindNames[size_t(Kind::Kinds)] = { "unmarked", "buildings", "props", "vehicles", "peds" };

    // EntityDrawLog: render thread only, but for the switch
    bool bDrawLog = false;
    constexpr uint32_t kDrawLogFrames = 600;
    struct TargetStats
    {
        std::string name;
        uint64_t draws[size_t(Kind::Kinds)]{};
        int64_t ticks[size_t(Kind::Kinds)]{};
        uint64_t sourceDraws[Sources]{};
        int64_t Total() const
        {
            int64_t total = 0;
            for (auto t : ticks)
                total += t;
            return total;
        }
    };
    std::vector<TargetStats> targetStats;
    const char* lastTargetName = nullptr;
    size_t lastTarget = 0;
    uint32_t statsFrom = 0;
    int64_t qpcFrequency = 0;

    TargetStats& StatsFor(const char* name)
    {
        if (name == lastTargetName && lastTarget < targetStats.size())
            return targetStats[lastTarget];
        const char* shown = !name ? "unknown (no lock hooks)" : !*name ? "no target locked" : name;
        auto it = std::find_if(targetStats.begin(), targetStats.end(), [&](auto& s) { return s.name == shown; });
        if (it == targetStats.end())
        {
            targetStats.push_back({ shown });
            it = targetStats.end() - 1;
        }
        lastTargetName = name;
        lastTarget = size_t(it - targetStats.begin());
        return *it;
    }

    void WriteDrawLog(uint32_t frames)
    {
        std::sort(targetStats.begin(), targetStats.end(), [](auto& a, auto& b) { return a.Total() > b.Total(); });
        const double perFrame = 1.0 / frames;
        const double msPerFrame = 1000.0 / double(qpcFrequency) / frames;
        std::string text = std::format("over {} scene frames, per frame, the draws and the CPU time of their execute:\n", frames);
        for (auto& s : targetStats)
        {
            text += std::format("  {:<28} {:6.3f} ms:", s.name, s.Total() * msPerFrame);
            for (size_t k = 1; k <= size_t(Kind::Kinds); ++k)
            {
                const size_t kind = k % size_t(Kind::Kinds);    // unmarked last, and only if any
                if (kind || s.draws[kind])
                    text += std::format(" {} {:.1f} ({:.3f} ms),", KindNames[kind], s.draws[kind] * perFrame, s.ticks[kind] * msPerFrame);
            }
            text += " by command";
            for (int i = 0; i < Sources; ++i)
                text += std::format("{} {} {:.1f}", i ? "," : "", SourceNames[i], s.sourceDraws[i] * perFrame);
            text += "\n";
        }
        FusionLog::WriteText("EntityClass", "Draws", text);
        targetStats.clear();
        lastTargetName = nullptr;
    }

    void Execute(DrawExecute original, void* dc, Kind kind, Source source)
    {
        const auto& hooks = Hooks();
        for (const auto& [before, after] : hooks)
            if (before)
                before(kind);
        if (!bDrawLog)
            original(dc);
        else
        {
            LARGE_INTEGER start, end;
            QueryPerformanceCounter(&start);
            original(dc);
            QueryPerformanceCounter(&end);
            const uint32_t frame = TemporalAA::SceneFrame;
            if (!statsFrom || frame < statsFrom)
                statsFrom = frame;
            else if (frame - statsFrom >= kDrawLogFrames)
            {
                WriteDrawLog(frame - statsFrom);
                statsFrom = frame;
            }
            auto& stats = StatsFor(RenderSkip::CurrentTarget());
            stats.draws[size_t(kind)]++;
            stats.ticks[size_t(kind)] += end.QuadPart - start.QuadPart;
            stats.sourceDraws[source]++;
        }
        for (auto it = hooks.rbegin(); it != hooks.rend(); ++it)
            if (it->second)
                it->second(kind);
    }

    Kind MarkOf(void* dc, ptrdiff_t at)
    {
        const auto kind = reinterpret_cast<uint8_t*>(dc)[at];
        return kind < uint8_t(Kind::Kinds) ? Kind(kind) : Kind::Unknown;
    }

    template <DrawExecute* original, Source source, ptrdiff_t at>
    void __fastcall DrawMarkedExecute(void* dc, void*)
    {
        Execute(*original, dc, MarkOf(dc, at), source);
    }

    void __fastcall DrawSkinnedExecute(void* dc, void*)
    {
        Execute(drawSkinnedExecute, dc, Kind::Prop, FromSkinned);
    }

    void Mark(uintptr_t dc, ptrdiff_t at, uintptr_t entity)
    {
        if (dc && entity)
            reinterpret_cast<uint8_t*>(dc)[at] = uint8_t(EntityClass::OfEntity(reinterpret_cast<void*>(entity)));
    }

    void Install()
    {
        auto pattern = hook::pattern("56 E8 ? ? ? ? 8B 75 0C EB 02 33 C0 50 E8");
        auto fragTypeEntity = hook::pattern("FF 75 10 56 E8 ? ? ? ? 8B 75 0C 50 E8");
        auto fragTypeDynamic = hook::pattern("FF 74 24 3C E8 ? ? ? ? 50 E8 ? ? ? ? EB");
        auto fragDynamic = hook::pattern("FF 74 24 40 E8 ? ? ? ? 50 E8 ? ? ? ? E9");
        auto entityVtable = hook::pattern("C7 07 ? ? ? ? 66 89 77 30 8B 41 30");
        auto fragVtable = hook::pattern("C7 07 ? ? ? ? E8 ? ? ? ? 8B 5C 24 18 66 89 77 46");
        auto fragTypeVtable = hook::pattern("C7 07 ? ? ? ? E8 ? ? ? ? 8B 4C 24 1C 66 89 77");
        auto skinnedVtable = hook::pattern("C7 06 ? ? ? ? 66 89 7E 08 89 46 1C");
        if (pattern.empty() || fragTypeEntity.empty() || fragTypeDynamic.empty() || fragDynamic.empty() ||
            entityVtable.empty() || fragVtable.empty() || fragTypeVtable.empty() || skinnedVtable.empty())
            return;

        // Right after each constructor: eax the command; the entity in edi in CEntity's add, in esi in CDynamicEntity's
        static auto entityHook = safetyhook::create_mid(pattern.get_first(6), [](SafetyHookContext& regs) { Mark(regs.eax, kDrawEntityKindByte, regs.edi); });
        static auto fragTypeEntityHook = safetyhook::create_mid(fragTypeEntity.get_first(9), [](SafetyHookContext& regs) { Mark(regs.eax, kDrawFragKindByte, regs.edi); });
        static auto fragTypeDynamicHook = safetyhook::create_mid(fragTypeDynamic.get_first(9), [](SafetyHookContext& regs) { Mark(regs.eax, kDrawFragKindByte, regs.esi); });
        static auto fragDynamicHook = safetyhook::create_mid(fragDynamic.get_first(9), [](SafetyHookContext& regs) { Mark(regs.eax, kDrawFragKindByte, regs.esi); });

        auto hookExecute = [](hook::pattern& ctor, DrawExecute& original, void* replacement)
        {
            auto slot = *ctor.get_first<void**>(2) + 1;
            original = reinterpret_cast<DrawExecute>(*slot);
            injector::WriteMemory(slot, replacement, true);
        };
        hookExecute(entityVtable, drawEntityExecute, reinterpret_cast<void*>(&DrawMarkedExecute<&drawEntityExecute, FromEntity, kDrawEntityKindByte>));
        hookExecute(fragVtable, drawFragExecute, reinterpret_cast<void*>(&DrawMarkedExecute<&drawFragExecute, FromFrag, kDrawFragKindByte>));
        hookExecute(fragTypeVtable, drawFragTypeExecute, reinterpret_cast<void*>(&DrawMarkedExecute<&drawFragTypeExecute, FromFragType, kDrawFragKindByte>));
        hookExecute(skinnedVtable, drawSkinnedExecute, reinterpret_cast<void*>(&DrawSkinnedExecute));
        bInstalled = true;
    }

    class EntityClassInit
    {
    public:
        EntityClassInit()
        {
            FusionFix::onInitEvent() += []()
            {
                Install();
                LARGE_INTEGER frequency;
                QueryPerformanceFrequency(&frequency);
                qpcFrequency = frequency.QuadPart;
                auto readIni = [](CIniReader& iniReader) { bDrawLog = iniReader.ReadInteger("DEBUG", "EntityDrawLog", 0) != 0; };
                CIniReader iniReader("");
                readIni(iniReader);
                IniReload::Add(readIni);
            };
        }
    } EntityClassInit;
}

namespace EntityClass
{
    void AddDrawHooks(DrawHook before, DrawHook after)
    {
        Hooks().emplace_back(before, after);
    }

    bool Installed()
    {
        return bInstalled;
    }

    Kind OfEntity(void* entity)
    {
        thread_local std::unordered_map<uintptr_t, Kind> byVtable;
        const auto vtable = *reinterpret_cast<uintptr_t*>(entity);
        if (auto it = byVtable.find(vtable); it != byVtable.end())
            return it->second;
        // The complete object locator: +16 the class hierarchy, whose +8 base count and +12 base array hold
        // base class descriptors, each with its type descriptor first (the class itself among them)
        const auto locator = *reinterpret_cast<uintptr_t*>(vtable - 4);
        const auto hierarchy = *reinterpret_cast<uintptr_t*>(locator + 16);
        const auto bases = *reinterpret_cast<uint32_t*>(hierarchy + 8);
        const auto array = *reinterpret_cast<uintptr_t**>(hierarchy + 12);
        Kind kind = Kind::Prop;
        for (uint32_t i = 0; i < bases && kind == Kind::Prop; ++i)
        {
            const auto type = *reinterpret_cast<uintptr_t*>(array[i]);
            const char* name = reinterpret_cast<const char*>(type + 8);
            if (!std::strcmp(name, ".?AVCVehicle@@"))
                kind = Kind::Vehicle;
            else if (!std::strcmp(name, ".?AVCPed@@"))
                kind = Kind::Ped;
            else
                for (auto building : { ".?AVCBuilding@@", ".?AVCAnimatedBuilding@@", ".?AVCInteriorInst@@", ".?AVCPortalInst@@" })
                    if (!std::strcmp(name, building))
                        kind = Kind::Building;
        }
        byVtable.emplace(vtable, kind);
        return kind;
    }
}
