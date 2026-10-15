module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module entityclass;

import common;
import comvars;
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
// with fragments go through). On the render thread the execute (vtable slot 1) of those, of
// CDrawSkinnedEntityDC (always a prop) and of the peds' own commands (CDrawPedDC, CDrawPlayerDC,
// CDrawPedPropsDC, CDrawPedPropDC, CDrawSLODPedDC) calls the hooks added here before and after the
// draw, in every render phase.
//
// EntityDrawLog = 1 ([DEBUG]): every 600 scene frames, the draws of each kind and the CPU time their
// execute took, per frame and per target the game had locked (RenderSkip::CurrentTarget), to
// GTAIV.EFLC.FusionFix.EntityClass.log: what leaving a kind out of a pass would save.
//
// LampShadowFaceCulling = 1 ([PERFORMANCE]): the lamps' shadow maps (SHADOW_MAP_COLOUR_POINT) are dual paraboloid,
// two halves a lamp, and get every entity in both. An entity or fragment type draw whose bounding sphere is wholly
// behind the half being drawn (the current grcViewport's view: behind its eye, view z > reach as RAGE looks down -z)
// is left out; a perspective viewport is tested against its frustum instead. The sphere is the model's (model info
// +0x10 centre, +0x1c radius) around the command's position, grown by the centre's offset so the entity's turn
// needs no matrix. With the switch off, EntityDrawLog counts what it would leave out, and where the spheres lie.
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
    // CDrawPedDC, CDrawPlayerDC, CDrawPedPropsDC, CDrawPedPropDC, CDrawSLODPedDC
    constexpr size_t kPedCommands = 5;
    DrawExecute drawPedExecute[kPedCommands] = {};
    bool bInstalled = false;

    std::vector<std::pair<EntityClass::DrawHook, EntityClass::DrawHook>>& Hooks()
    {
        static std::vector<std::pair<EntityClass::DrawHook, EntityClass::DrawHook>> hooks;
        return hooks;
    }

    // Which draw command a draw came from
    enum Source : uint8_t { FromEntity, FromFrag, FromFragType, FromSkinned, FromPed, Sources };
    constexpr const char* SourceNames[Sources] = { "entity", "fragment", "fragment type", "skinned", "ped" };
    constexpr const char* KindNames[size_t(Kind::Kinds)] = { "unmarked", "buildings", "props", "vehicles", "peds" };

    // EntityDrawLog and LampShadowFaceCulling: render thread only, but for the switches
    bool bDrawLog = false;
    bool bFaceCulling = false;
    uintptr_t modelInfos = 0;       // CE 0x1295cd8, model infos by index
    constexpr uint32_t kDrawLogFrames = 600;
    struct TargetStats
    {
        std::string name;
        uint64_t draws[size_t(Kind::Kinds)]{};
        int64_t ticks[size_t(Kind::Kinds)]{};
        uint64_t sourceDraws[Sources]{};
        // Locks of the target that had draws, and device viewports within them (the faces of a lamp's cube map);
        // per lock the entities drawn into it, by model and place (entity and fragment type commands), to tell how
        // often one entity is drawn into one lamp's map
        uint64_t locks = 0, faces = 0;
        uint64_t keyedDraws = 0, entities = 0;
        uint32_t keysLock = ~0u;
        std::unordered_set<uint64_t> keys;
        // Draws outside the face drawn: left out with LampShadowFaceCulling, else counted only
        uint64_t outside = 0;
        // Lamp maps: the views (grcViewport view matrices) per lock, perspective or not, and where the draws' spheres
        // are against the view: ahead of the eye, behind it, across it
        uint64_t views = 0, perspective = 0, ahead = 0, behind = 0, across = 0;
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
    uint32_t lastLock = ~0u;
    uint32_t lastViewport = ~0u;
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
            if (s.keyedDraws)
                text += std::format(" locks {:.1f}, faces per lock {:.2f}, draws per entity in a lock {:.2f},", s.locks * perFrame,
                    s.locks ? double(s.faces) / double(s.locks) : 0.0, double(s.keyedDraws) / double(s.entities));
            if (s.ahead + s.behind + s.across)
                text += std::format(" views per lock {:.2f} ({:.0f}% perspective), spheres ahead {:.1f}, behind {:.1f}, across {:.1f},",
                    s.locks ? double(s.views) / double(s.locks) : 0.0, s.views ? 100.0 * s.perspective / s.views : 0.0,
                    s.ahead * perFrame, s.behind * perFrame, s.across * perFrame);
            if (s.outside)
                text += std::format(" outside their face {:.1f} ({}),", s.outside * perFrame, bFaceCulling ? "left out" : "drawn");
            text += " by command";
            for (int i = 0; i < Sources; ++i)
                text += std::format("{} {} {:.1f}", i ? "," : "", SourceNames[i], s.sourceDraws[i] * perFrame);
            text += "\n";
        }
        FusionLog::WriteText("EntityClass", "Draws", text);
        targetStats.clear();
        lastTargetName = nullptr;
    }

    // The model index and the position the command keeps: CDrawEntityDC +0x30 and +0x20, CDrawFragTypeDC
    // +0x58 and +0x40; false for the other commands
    bool ModelAndPlace(void* dc, Source source, uint16_t& model, const float*& place)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(dc);
        if (source == FromEntity)
            model = *reinterpret_cast<const uint16_t*>(bytes + 0x30), place = reinterpret_cast<const float*>(bytes + 0x20);
        else if (source == FromFragType)
            model = *reinterpret_cast<const uint16_t*>(bytes + 0x58), place = reinterpret_cast<const float*>(bytes + 0x40);
        else
            return false;
        return true;
    }

    bool IsLampMaps(const char* target)
    {
        static const char* lastName = nullptr;
        static bool lastIs = false;
        if (target != lastName)
        {
            lastName = target;
            lastIs = target && !std::strcmp(target, "SHADOW_MAP_COLOUR_POINT");
        }
        return lastIs;
    }

    // The current viewport's view, and for a perspective one its frustum as planes (x, y, z, w) with
    // w + dot(xyz, p) >= 0 inside, from view x projection (row vectors, D3D clip z 0 to w); made again when its
    // matrices change, which counts a view
    float frustum[6][4]{};
    float frustumFrom[2][4][4]{};
    bool frustumValid = false;
    bool viewPerspective = false;
    bool viewChanged = false;

    bool UpdateView()
    {
        auto viewport = rage::GetCurrentViewport();
        if (!viewport)
            return false;
        if (frustumValid && !std::memcmp(frustumFrom[0], viewport->mViewMatrix, sizeof(frustumFrom[0])) &&
            !std::memcmp(frustumFrom[1], viewport->mProjectionMatrix, sizeof(frustumFrom[1])))
            return true;
        viewChanged = true;
        std::memcpy(frustumFrom[0], viewport->mViewMatrix, sizeof(frustumFrom[0]));
        std::memcpy(frustumFrom[1], viewport->mProjectionMatrix, sizeof(frustumFrom[1]));
        viewPerspective = viewport->mIsPerspective;
        frustumValid = true;
        if (!viewPerspective)
            return true;
        float m[4][4];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                m[i][j] = 0.0f;
                for (int k = 0; k < 4; ++k)
                    m[i][j] += frustumFrom[0][i][k] * frustumFrom[1][k][j];
            }
        // Columns of m: clip x, y, z, w; inside -w <= x <= w, -w <= y <= w, 0 <= z <= w
        auto column = [&](int c, int i) { return m[i][c]; };
        for (int i = 0; i < 4; ++i)
        {
            frustum[0][i] = column(3, i) + column(0, i);
            frustum[1][i] = column(3, i) - column(0, i);
            frustum[2][i] = column(3, i) + column(1, i);
            frustum[3][i] = column(3, i) - column(1, i);
            frustum[4][i] = column(2, i);
            frustum[5][i] = column(3, i) - column(2, i);
        }
        for (auto& plane : frustum)
        {
            const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
            if (length > 0.0f)
                for (auto& v : plane)
                    v /= length;
        }
        return true;
    }

    // Where the draw's bounding sphere is against the view drawn: -1 wholly outside it (behind a paraboloid
    // half's eye, or outside a perspective frustum), 1 wholly ahead or inside, 0 across or unknown
    int SphereSide(void* dc, Source source)
    {
        uint16_t model;
        const float* place;
        if (!modelInfos || !ModelAndPlace(dc, source, model, place))
            return 0;
        const auto info = *reinterpret_cast<const uintptr_t*>(modelInfos + model * 4);
        if (!info)
            return 0;
        const float* centre = reinterpret_cast<const float*>(info + 0x10);
        const float radius = *reinterpret_cast<const float*>(info + 0x1c);
        if (!(radius > 0.0f) || !UpdateView())
            return 0;
        const float reach = radius + std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
        if (viewPerspective)
        {
            bool inside = true;
            for (const auto& plane : frustum)
            {
                const float d = plane[0] * place[0] + plane[1] * place[1] + plane[2] * place[2] + plane[3];
                if (d < -reach)
                    return -1;
                inside = inside && d >= reach;
            }
            return inside ? 1 : 0;
        }
        // View space z (row vectors: z = p . column 2 + m[3][2]); the eye looks down -z
        const auto& v = frustumFrom[0];
        const float z = place[0] * v[0][2] + place[1] * v[1][2] + place[2] * v[2][2] + v[3][2];
        return z > reach ? -1 : z < -reach ? 1 : 0;
    }

    void Execute(DrawExecute original, void* dc, Kind kind, Source source)
    {
        const char* target = bDrawLog || bFaceCulling ? RenderSkip::CurrentTarget() : nullptr;
        const bool lampMaps = (bDrawLog || bFaceCulling) && IsLampMaps(target);
        viewChanged = false;
        const int side = lampMaps ? SphereSide(dc, source) : 0;
        const bool outside = side < 0;
        if (outside && bFaceCulling)
        {
            if (bDrawLog)
            {
                auto& stats = StatsFor(target);
                stats.outside++;
                stats.behind++;
                if (viewChanged)
                {
                    stats.views++;
                    stats.perspective += viewPerspective;
                }
            }
            return;
        }

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
            auto& stats = StatsFor(target);
            stats.draws[size_t(kind)]++;
            stats.ticks[size_t(kind)] += end.QuadPart - start.QuadPart;
            stats.sourceDraws[source]++;
            if (outside)
                stats.outside++;
            if (lampMaps)
            {
                if (viewChanged)
                {
                    stats.views++;
                    stats.perspective += viewPerspective;
                }
                (side < 0 ? stats.behind : side > 0 ? stats.ahead : stats.across)++;
            }
            const auto lock = RenderSkip::LockCount();
            if (lock != lastLock)
            {
                lastLock = lock;
                stats.locks++;
            }
            if (const auto viewport = RenderSkip::ViewportCount(); viewport != lastViewport)
            {
                lastViewport = viewport;
                stats.faces++;
            }
            uint16_t model;
            const float* place;
            if (ModelAndPlace(dc, source, model, place))
            {
                uint64_t key = model;
                for (int i = 0; i < 3; ++i)
                    key = key * 0x9E3779B97F4A7C15ull ^ reinterpret_cast<const uint32_t*>(place)[i];
                if (stats.keysLock != lock)
                {
                    stats.keysLock = lock;
                    stats.keys.clear();
                }
                stats.keyedDraws++;
                if (stats.keys.insert(key).second)
                    stats.entities++;
            }
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

    template <size_t command>
    void __fastcall DrawPedExecute(void* dc, void*)
    {
        Execute(drawPedExecute[command], dc, Kind::Ped, FromPed);
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
        // CDrawEntityDC's execute: movzx eax, word [esi+0x30]; mov eax, [eax*4 + model infos]
        if (auto infos = hook::pattern("0F B7 46 30 8B 04 85 ? ? ? ? 85 C0"); !infos.empty())
            modelInfos = *infos.get_first<uintptr_t>(7);
        hook::pattern pedVtables[kPedCommands] = {
            hook::pattern("C7 03 ? ? ? ? E8 ? ? ? ? 8A 44 24 2C"),
            hook::pattern("C7 03 ? ? ? ? E8 ? ? ? ? 8A 44 24 20"),
            hook::pattern("C7 07 ? ? ? ? E8 ? ? ? ? 8A 44 24 2C"),
            hook::pattern("C7 06 ? ? ? ? E8 ? ? ? ? 8B 4C 24 0C 8B 01 89 86 A0 00 00 00"),
            hook::pattern("C7 06 ? ? ? ? E8 ? ? ? ? 83 C4 20 8B C6"),
        };
        for (auto& ped : pedVtables)
            if (ped.empty())
                return;
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
        hookExecute(pedVtables[0], drawPedExecute[0], reinterpret_cast<void*>(&DrawPedExecute<0>));
        hookExecute(pedVtables[1], drawPedExecute[1], reinterpret_cast<void*>(&DrawPedExecute<1>));
        hookExecute(pedVtables[2], drawPedExecute[2], reinterpret_cast<void*>(&DrawPedExecute<2>));
        hookExecute(pedVtables[3], drawPedExecute[3], reinterpret_cast<void*>(&DrawPedExecute<3>));
        hookExecute(pedVtables[4], drawPedExecute[4], reinterpret_cast<void*>(&DrawPedExecute<4>));
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
                auto readIni = [](CIniReader& iniReader)
                {
                    bDrawLog = iniReader.ReadInteger("DEBUG", "EntityDrawLog", 0) != 0;
                    bFaceCulling = iniReader.ReadInteger("PERFORMANCE", "LampShadowFaceCulling", 0) != 0;
                };
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
        // Entities of one class mostly come in runs: the last one is checked before the map
        thread_local uintptr_t lastVtable = 0;
        thread_local Kind lastKind = Kind::Unknown;
        thread_local std::unordered_map<uintptr_t, Kind> byVtable;
        const auto vtable = *reinterpret_cast<uintptr_t*>(entity);
        if (vtable == lastVtable)
            return lastKind;
        lastVtable = vtable;
        if (auto it = byVtable.find(vtable); it != byVtable.end())
            return lastKind = it->second;
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
        return lastKind = kind;
    }
}
