module;

#include <common.hxx>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

export module entityclass;

import common;

// What kind of entity each draw of the scene belongs to: the map's own geometry, which never moves,
// or a prop (an object, a fragment, a skinned entity), which can be pushed, knocked over or broken.
// Props use the same world shaders and materials as the ground, so the shaders cannot tell them apart.
//
// CEntity's draw list add (0xa2fe70, game thread) is the only maker of CDrawEntityDC; it marks the
// entity's kind in the command's spare byte +0x37. On the render thread the execute (vtable slot 1)
// of CDrawEntityDC, CDrawFragDC, CDrawFragTypeDC and CDrawSkinnedEntityDC calls the hooks added
// here before and after the draw, in every render phase.
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

    void Execute(DrawExecute original, void* dc, EntityClass::Kind kind)
    {
        const auto& hooks = Hooks();
        for (const auto& [before, after] : hooks)
            if (before)
                before(kind);
        original(dc);
        for (auto it = hooks.rbegin(); it != hooks.rend(); ++it)
            if (it->second)
                it->second(kind);
    }

    void __fastcall DrawEntityExecute(void* dc, void*)
    {
        const auto kind = static_cast<EntityClass::Kind>(reinterpret_cast<uint8_t*>(dc)[kDrawEntityKindByte]);
        Execute(drawEntityExecute, dc, kind <= EntityClass::Kind::Prop ? kind : EntityClass::Kind::Unknown);
    }

    template <DrawExecute* original>
    void __fastcall DrawPropExecute(void* dc, void*)
    {
        Execute(*original, dc, EntityClass::Kind::Prop);
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
        hookExecute(fragVtable, drawFragExecute, reinterpret_cast<void*>(&DrawPropExecute<&drawFragExecute>));
        hookExecute(fragTypeVtable, drawFragTypeExecute, reinterpret_cast<void*>(&DrawPropExecute<&drawFragTypeExecute>));
        hookExecute(skinnedVtable, drawSkinnedExecute, reinterpret_cast<void*>(&DrawPropExecute<&drawSkinnedExecute>));
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
