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

// What kind of entity each draw of the scene belongs to: the map's own geometry, which never moves,
// or a prop (an object, a fragment, a skinned entity), which can be pushed, knocked over or broken.
// Props use the same world shaders and materials as the ground, so the shaders cannot tell them apart.
//
// CEntity's draw list add (0xa2fe70, game thread) is the only maker of CDrawEntityDC; it marks the
// entity's kind in the command's spare byte +0x37. On the render thread the execute (vtable slot 1)
// of CDrawEntityDC, CDrawFragDC, CDrawFragTypeDC and CDrawSkinnedEntityDC calls the hooks added
// here before and after the draw, in every render phase.
//
// EntityDrawLog = 1 ([DEBUG]): every 600 scene frames, the draws of buildings and props and the CPU time
// their execute took, per frame and per target the game had locked (RenderSkip::CurrentTarget), to
// GTAIV.EFLC.FusionFix.EntityClass.log: what leaving props out of a pass would save.
export namespace EntityClass
{
    enum class Kind : uint8_t
    {
        Unknown,
        Building,   // CBuilding, CAnimatedBuilding, CInteriorInst, CPortalInst
        Prop,       // CObject, CDummyObject, CCutsceneObject, fragments, skinned entities
    };

    using DrawHook = void (*)(Kind kind);

    // Hooks are added from module constructors or init events, before the first frame: the list is
    // read on the render thread without a lock.
    void AddDrawHooks(DrawHook before, DrawHook after);

    // Whether the game's code was found and hooked.
    bool Installed();

    // By the class name in the entity's vtable RTTI.
    Kind OfEntity(void* entity);
}

namespace
{
    constexpr ptrdiff_t kDrawEntityKindByte = 0x37;
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
    constexpr const char* SourceNames[Sources] = { "objects", "fragments", "fragment types", "skinned" };

    // EntityDrawLog: render thread only, but for the switch
    bool bDrawLog = false;
    constexpr uint32_t kDrawLogFrames = 600;
    struct TargetStats
    {
        std::string name;
        uint64_t draws[3]{};            // by Kind
        int64_t ticks[3]{};
        uint64_t propDraws[Sources]{};
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
        std::sort(targetStats.begin(), targetStats.end(), [](auto& a, auto& b)
        {
            return a.ticks[0] + a.ticks[1] + a.ticks[2] > b.ticks[0] + b.ticks[1] + b.ticks[2];
        });
        const double perFrame = 1.0 / frames;
        const double msPerFrame = 1000.0 / double(qpcFrequency) / frames;
        std::string text = std::format("over {} scene frames, per frame, the draws and the CPU time of their execute:\n", frames);
        for (auto& s : targetStats)
        {
            text += std::format("  {:<28} buildings {:7.1f} ({:6.3f} ms), props {:7.1f} ({:6.3f} ms:", s.name,
                s.draws[1] * perFrame, s.ticks[1] * msPerFrame, s.draws[2] * perFrame, s.ticks[2] * msPerFrame);
            for (int i = 0; i < Sources; ++i)
                text += std::format("{} {} {:.1f}", i ? "," : "", SourceNames[i], s.propDraws[i] * perFrame);
            text += ")";
            if (s.draws[0])
                text += std::format(", unmarked {:.1f} ({:.3f} ms)", s.draws[0] * perFrame, s.ticks[0] * msPerFrame);
            text += "\n";
        }
        FusionLog::WriteText("EntityClass", "Draws", text);
        targetStats.clear();
        lastTargetName = nullptr;
    }

    void Execute(DrawExecute original, void* dc, EntityClass::Kind kind, Source source)
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
            if (kind == EntityClass::Kind::Prop)
                stats.propDraws[source]++;
        }
        for (auto it = hooks.rbegin(); it != hooks.rend(); ++it)
            if (it->second)
                it->second(kind);
    }

    void __fastcall DrawEntityExecute(void* dc, void*)
    {
        const auto kind = static_cast<EntityClass::Kind>(reinterpret_cast<uint8_t*>(dc)[kDrawEntityKindByte]);
        Execute(drawEntityExecute, dc, kind <= EntityClass::Kind::Prop ? kind : EntityClass::Kind::Unknown, FromEntity);
    }

    template <DrawExecute* original, Source source>
    void __fastcall DrawPropExecute(void* dc, void*)
    {
        Execute(*original, dc, EntityClass::Kind::Prop, source);
    }

    void Install()
    {
        auto pattern = hook::pattern("56 E8 ? ? ? ? 8B 75 0C EB 02 33 C0 50 E8");
        auto entityVtable = hook::pattern("C7 07 ? ? ? ? 66 89 77 30 8B 41 30");
        auto fragVtable = hook::pattern("C7 07 ? ? ? ? E8 ? ? ? ? 8B 5C 24 18 66 89 77 46");
        auto fragTypeVtable = hook::pattern("C7 07 ? ? ? ? E8 ? ? ? ? 8B 4C 24 1C 66 89 77");
        auto skinnedVtable = hook::pattern("C7 06 ? ? ? ? 66 89 7E 08 89 46 1C");
        if (pattern.empty() || entityVtable.empty() || fragVtable.empty() || fragTypeVtable.empty() || skinnedVtable.empty())
            return;

        // Right after the CDrawEntityDC constructor: eax the command, edi the entity
        static auto addHook = safetyhook::create_mid(pattern.get_first(6), [](SafetyHookContext& regs)
        {
            reinterpret_cast<uint8_t*>(regs.eax)[kDrawEntityKindByte] = uint8_t(EntityClass::OfEntity(reinterpret_cast<void*>(regs.edi)));
        });

        auto hookExecute = [](hook::pattern& ctor, DrawExecute& original, void* replacement)
        {
            auto slot = *ctor.get_first<void**>(2) + 1;
            original = reinterpret_cast<DrawExecute>(*slot);
            injector::WriteMemory(slot, replacement, true);
        };
        hookExecute(entityVtable, drawEntityExecute, reinterpret_cast<void*>(&DrawEntityExecute));
        hookExecute(fragVtable, drawFragExecute, reinterpret_cast<void*>(&DrawPropExecute<&drawFragExecute, FromFrag>));
        hookExecute(fragTypeVtable, drawFragTypeExecute, reinterpret_cast<void*>(&DrawPropExecute<&drawFragTypeExecute, FromFragType>));
        hookExecute(skinnedVtable, drawSkinnedExecute, reinterpret_cast<void*>(&DrawPropExecute<&drawSkinnedExecute, FromSkinned>));
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
        const auto locator = *reinterpret_cast<uintptr_t*>(vtable - 4);
        const auto type = *reinterpret_cast<uintptr_t*>(locator + 12);
        const char* name = reinterpret_cast<const char*>(type + 8);
        Kind kind = Kind::Prop;
        for (auto building : { ".?AVCBuilding@@", ".?AVCAnimatedBuilding@@", ".?AVCInteriorInst@@", ".?AVCPortalInst@@" })
            if (std::strcmp(name, building) == 0)
                kind = Kind::Building;
        byVtable.emplace(vtable, kind);
        return kind;
    }
}
