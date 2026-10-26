// PropsInLampCaches: the props a lamp lights go into its cached map too. A cache is baked by slot 0 in mode 3 (six
// cube faces, resampled into the atlas): BuildRenderList (0xD782E0) has its phase walk the static world only
// (+0x15/+0x16 = 1/0) with the entity mask 0x800280, so no object (entity type 4) ever reaches its lists. A slot in
// mode 4 copies the cache in and draws the objects, peds and cars over it, so a prop shadowed from a lamp only
// while the lamp held a slot: a scaffold walkway's shadows from its own ceiling lights came and went with every car
// that took a slot nearby. A capture of a bake: its buildings are drawn by the lamp pass (0xAE3DE0: lists 1, 0, 5, 7
// in mode 1) with the same local depth shaders (vs B0D, ps 353) as the props in a slot's map, the cube face in c56.
//
// So where the render list sorts an opaque entity into its lists (0xAE73B1, before the 0x8000 test that puts an
// object into list 5 of the phases in [0x159B750]), an object within the light of the lamp slot 0 bakes this frame
// goes into list 5 of slot 0's phase as well (0xAE43A0). Slot 0's phase number and lamp are taken as BuildRenderList
// numbers the phase (0xD78370). The objects come from the frame's scene walks of all phases, so one no phase sees is
// left out until it is seen.
//
// A cache is redrawn only when the count of its phase's lists 0 and 1 (0xAE5EA0, buildings) differs from the one kept
// with it (0xD77A88: edi against the record's +0xF4, stored as the entry's +0xEC at 0xD77D49 from [esp+0x2C]); a sum
// of a hash of each added object's entity, place (quarter metres) and tilt goes into both, so a prop coming into the
// light, leaving it, moved or swapped redraws the cache.
namespace PropCaches
{
    static bool enabled = false;
    static std::string status = "off";
    static uintptr_t game = 0;
    static SafetyHookMid numbered, collect, redraw;
    static std::atomic<uint32_t> phaseBit{0}, phaseFrame{0};
    static std::atomic<float> lamp[4]{}; // x, y, z, reach
    static std::atomic<uint32_t> signature{0};
    static std::atomic<uint32_t> bakes{0}, added{0}, redraws{0};

    static uint32_t Frame() noexcept { return CTimer::m_frameCount ? *CTimer::m_frameCount : 0; }

    static uint32_t Hash(uintptr_t entity, const float* matrix) noexcept
    {
        uint32_t h = static_cast<uint32_t>(entity) * 0x9E3779B1u;
        const auto mix = [&h](int32_t v) { h ^= static_cast<uint32_t>(v) + 0x7F4A7C15u + (h << 6) + (h >> 2); };
        for (int i = 12; i < 15; ++i) mix(static_cast<int32_t>(std::floor(matrix[i] * 4.0f)));
        for (int i = 8; i < 11; ++i) mix(static_cast<int32_t>(std::floor(matrix[i] * 10.0f)));
        return h | 1;
    }

    // 0xD78370 in BuildRenderList: ESI the phase, EAX its number this frame (0xB05750)
    static void Numbered(SafetyHookContext& regs) noexcept
    {
        if (*reinterpret_cast<const uint32_t*>(regs.esi + 0x940) != 0) return;
        const auto record = game + 0xD9F100; // slot 0
        const bool bake = enabled && bExtraNightShadows && regs.eax < 32 &&
            *reinterpret_cast<const uint32_t*>(record + 0xFC) == 3 && *reinterpret_cast<const uint8_t*>(record + 0xED) == 1;
        signature.store(0, std::memory_order_relaxed);
        phaseFrame.store(Frame(), std::memory_order_relaxed);
        if (!bake) { phaseBit.store(0, std::memory_order_relaxed); return; }
        const float* at = reinterpret_cast<const float*>(record + 0xC0);
        const float radius = *reinterpret_cast<const float*>(record + 0xE0);
        if (!std::isfinite(at[0]) || !std::isfinite(radius) || radius <= 0.0f) { phaseBit.store(0, std::memory_order_relaxed); return; }
        for (int i = 0; i < 3; ++i) lamp[i].store(at[i], std::memory_order_relaxed);
        lamp[3].store(radius + 2.0f, std::memory_order_relaxed);
        phaseBit.store(1u << regs.eax, std::memory_order_release);
        ++bakes;
    }

    // 0xAE73B1 in the render list (may run on its workers): ESI an opaque entity
    static void Collect(SafetyHookContext& regs) noexcept
    {
        const auto bit = phaseBit.load(std::memory_order_acquire);
        if (!bit || phaseFrame.load(std::memory_order_relaxed) != Frame()) return;
        const auto entity = static_cast<uintptr_t>(regs.esi);
        if (((*reinterpret_cast<const uint32_t*>(entity + 0x28) >> 6) & 15) != 4) return;
        // A weapon in someone's hand is an object too (its model info's type, vfunc +0xC, is 4; a prop's 1):
        // baked in, it would hang there and redraw the cache each time it moved
        const auto model = *reinterpret_cast<const int16_t*>(entity + 0x2E);
        const auto info = model >= 0 ? reinterpret_cast<const uintptr_t*>(game + 0xE95CD8)[model] : 0;
        if (!info || reinterpret_cast<uint8_t(__thiscall*)(uintptr_t)>((*reinterpret_cast<const uintptr_t* const*>(info))[3])(info) >= 4)
            return;
        const auto matrix = *reinterpret_cast<const float* const*>(entity + 0x20);
        if (!matrix) return;
        const float dx = matrix[12] - lamp[0].load(std::memory_order_relaxed), dy = matrix[13] - lamp[1].load(std::memory_order_relaxed),
            dz = matrix[14] - lamp[2].load(std::memory_order_relaxed), reach = lamp[3].load(std::memory_order_relaxed);
        if (dx * dx + dy * dy + dz * dz > reach * reach) return;
        reinterpret_cast<void(__cdecl*)(uint32_t, uintptr_t, float, int)>(game + 0x6E43A0)(bit, entity, 0.0f, 5);
        signature.fetch_add(Hash(entity, matrix), std::memory_order_relaxed);
        ++added;
    }

    // 0xD77A88 in the phase's render: EBP the slot record's offset (slot 0: 0), EDI and [esp+0x2C] the count
    static void Redraw(SafetyHookContext& regs) noexcept
    {
        if (regs.ebp != 0 || !phaseBit.load(std::memory_order_relaxed) || phaseFrame.load(std::memory_order_relaxed) != Frame()) return;
        const auto sum = signature.load(std::memory_order_relaxed);
        if (!sum) return;
        regs.edi += sum;
        *reinterpret_cast<uint32_t*>(regs.esp + 0x2C) += sum;
        ++redraws;
    }

    static void Install(bool requested, bool skipOwnObject) noexcept
    {
        game = GameBase();
        skipOwn = skipOwnObject;
        if (!requested) return;
        using HeadlightEnhancement::CodeCheck;
        const auto check = CodeCheck()
            .Bytes(0x978370, {0xFF,0xB6,0x40,0x09,0x00,0x00})       // push [esi+0x940]
            .Bytes(0x978366, {0xB9}).Address(0x978367, 0x1214C90)   // mov ecx, 0x1614C90 (the phase numbers)
            .Bytes(0x6E73B1, {0xF7,0x46,0x24,0x00,0x80,0x00,0x00})  // test [esi+0x24], 0x8000
            .Bytes(0x977A88, {0x3B,0xBD}).Address(0x977A8A, 0xD9F1F4) // cmp edi, [ebp+0x119F1F4]
            .Bytes(0x977D49, {0x89,0xB8}).Address(0x977D4B, 0xDA0BFC) // mov [eax+0x11A0BFC], edi
            .Bytes(0x6E6FF5, {0x8B,0x04,0x85}).Address(0x6E6FF8, 0xE95CD8); // mov eax, [eax*4+0x1295CD8]: the model infos
        if (!check) { status = check.Status(); return; }
        numbered = safetyhook::create_mid(game + 0x978370, Numbered);
        collect = safetyhook::create_mid(game + 0x6E73B1, Collect);
        redraw = safetyhook::create_mid(game + 0x977A88, Redraw);
        enabled = numbered && collect && redraw;
        status = enabled ? "on" : "hook failed";
    }

    // LampsSkipOwnObject: a light of an object (its 2dfx; the light's key is the object's address plus an offset under
    // 0x320, 0xC1E183) leaves that object out of its own map, the slot's and the cache's. A scaffold walkway's ceiling
    // lights hang inside the scaffold (lamp 16.1 m up, the object 12.6 to 20.4 m): drawn into their own maps, the
    // fitting right at the lamp covered all below it, and the light went out each time the lamp took a slot (you
    // standing under it, a car going by). A lamp post's own post and head go the same way.
    static bool skipOwn = true;
    static thread_local uint32_t scopeKey = 0;
    static std::atomic<uint32_t> ownSkipped{0};

    static bool OwnObject(uintptr_t entity, uint32_t type) noexcept
    {
        if (type != 4 || !scopeKey || scopeKey - static_cast<uint32_t>(entity) >= 0x320) return false;
        ++ownSkipped;
        return true;
    }

    // Around a phase's render (0xD77A00): the light of the slot it draws (+0x940, record +0xF8), slots 0 to 7
    class OwnScope
    {
        uint32_t previous;
    public:
        explicit OwnScope(void* phase) noexcept : previous(scopeKey)
        {
            scopeKey = 0;
            if (!skipOwn || !bExtraNightShadows || !phase || !game) return;
            const auto slot = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(phase) + 0x940);
            if (slot > 7 || *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(phase) + 0x938) == -1) return;
            const auto record = game + 0xD9F100 + slot * 0x110;
            if (*reinterpret_cast<const uint8_t*>(record + 0xED) != 1) return;
            const auto key = *reinterpret_cast<const uint32_t*>(record + 0xF8);
            if (key && key != UINT32_MAX) scopeKey = key;
        }
        ~OwnScope() noexcept { scopeKey = previous; }
        OwnScope(const OwnScope&) = delete;
        OwnScope& operator=(const OwnScope&) = delete;
    };
}
