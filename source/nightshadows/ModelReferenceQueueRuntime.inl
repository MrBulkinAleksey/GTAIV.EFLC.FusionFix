// Liberty's Shadows 2.0.2 (OnyxOak, GPL-3.0): ModelReferenceQueue189, the model reference queue guard.
//
// Every entity draw (shadow passes too: 0x5D4663, 0x8DAA1F..0x8DB813, 0xA3DA1E, 0xAC1BCA, 0xAE6196) calls
// 0x942000 on the global list 0x11D4EC8: it AddRefs the model info [0x1295CD8 + model * 4] (0xB59F20) and
// pushes the u16 model into the current of four buffers ([+0x2040]; at +0x204C + i * 8 the array, +0x2050
// count, +0x2052 capacity) with no bounds check. 0x9424E0 releases the buffer's models (0xB5A5A0) when it
// drains it. Our extra shadow draws push more than the game ever did, past the buffer's end.
//
// A model already in the buffer is not pushed again (one held reference per model per buffer is enough until
// it drains), and one that finds the buffer full is held in a spill array of our own, released right after
// the game drains that buffer. No draw is left out.

namespace ModelReferenceQueue
{
    static SafetyHookInline addHook, drainHook;
    static std::string status = "not installed";
    static std::atomic<uint32_t> duplicates{0}, spilled{0}, invalid{0};

    constexpr uintptr_t ListRva = 0xDD4EC8, ModelsRva = 0xE95CD8, AddRefRva = 0x759F20, ReleaseRva = 0x75A5A0;

    struct Seen
    {
        std::array<uint32_t, 2048> bits{};
        uintptr_t pointer = 0;
        uint32_t count = 0;
        bool known = false;
        bool Contains(uint16_t model) const noexcept { return (bits[model >> 5] & (1u << (model & 31))) != 0; }
        void Mark(uint16_t model) noexcept { bits[model >> 5] |= 1u << (model & 31); }
        void Seed(const uint16_t* entries, uint32_t n) noexcept
        {
            bits.fill(0);
            for (uint32_t i = 0; i < n; ++i) Mark(entries[i]);
            pointer = reinterpret_cast<uintptr_t>(entries); count = n; known = true;
        }
    };
    static std::array<Seen, 4> seen;
    // Model indices are u16: with no model held twice a buffer's spill never needs more than 65536.
    static std::array<std::array<uint16_t, 65536>, 4> spill;
    static std::array<uint32_t, 4> spillUsed{};

    static void Reference(uint16_t model, bool add) noexcept
    {
        const auto info = reinterpret_cast<void* const*>(GameBase() + ModelsRva)[model];
        reinterpret_cast<void(__thiscall*)(void*)>(GameBase() + (add ? AddRefRva : ReleaseRva))(info);
    }

    // thiscall(list, model), ret 4: ecx, a dummy edx and the stack argument.
    static void __fastcall Add(void* list, void*, uint32_t model)
    {
        if (reinterpret_cast<uintptr_t>(list) != GameBase() + ListRva) { addHook.unsafe_thiscall<void>(list, model); return; }
        auto* bytes = static_cast<uint8_t*>(list);
        const auto slot = *reinterpret_cast<const uint32_t*>(bytes + 0x2040);
        auto* buffer = bytes + 0x204C + (slot & 3) * 8;
        auto* entries = *reinterpret_cast<uint16_t**>(buffer);
        const auto count = *reinterpret_cast<const uint16_t*>(buffer + 4), capacity = *reinterpret_cast<const uint16_t*>(buffer + 6);
        if (slot >= 4 || model > 0xFFFF || !entries || !capacity || count > capacity)
        {
            // Not a state the guard understands: as the game would.
            ++invalid;
            addHook.unsafe_thiscall<void>(list, model);
            return;
        }
        auto& s = seen[slot];
        if (!s.known || s.pointer != reinterpret_cast<uintptr_t>(entries) || s.count != count) s.Seed(entries, count);
        const auto id = static_cast<uint16_t>(model);
        if (s.Contains(id)) { ++duplicates; return; }
        if (count == capacity)
        {
            Reference(id, true);
            spill[slot][spillUsed[slot]++] = id;
            ++spilled;
        }
        else
            addHook.unsafe_thiscall<void>(list, model);
        s.Mark(id);
        s.count = *reinterpret_cast<const uint16_t*>(buffer + 4);
    }

    static void __fastcall Drain(void* list, void*)
    {
        if (reinterpret_cast<uintptr_t>(list) != GameBase() + ListRva) { drainHook.unsafe_thiscall<void>(list); return; }
        const auto slot = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(list) + 0x2040);
        drainHook.unsafe_thiscall<void>(list);
        if (slot >= 4) return;
        for (uint32_t i = 0; i < spillUsed[slot]; ++i) Reference(spill[slot][i], false);
        spillUsed[slot] = 0;
        seen[slot].known = false;
    }

    static void Install() noexcept
    {
        using HeadlightEnhancement::CodeCheck;
        // 0x942000: push esi / push edi / mov edi, [esp+C] / mov esi, ecx / mov ecx, [edi*4 + models] / call AddRef,
        // then the push with no bounds check; 0x9424E0: the drain's loop calling Release.
        const auto check = CodeCheck().Bytes(0x542000, {0x56,0x57,0x8B,0x7C,0x24,0x0C,0x8B,0xF1,0x8B,0x0C,0xBD})
            .Address(0x54200B, ModelsRva).Bytes(0x54200F, {0xE8}).Branch(0x542010, AddRefRva)
            .Bytes(0x542014, {0x8B,0x86,0x40,0x20,0x00,0x00,0x0F,0xB7,0x94,0xC6,0x50,0x20,0x00,0x00})
            .Bytes(0x5424E0, {0x53,0x56,0x57,0x8B,0xF9,0x33,0xC0,0x8B,0x9F,0x40,0x20,0x00,0x00})
            .Bytes(0x54250B, {0x8B,0x0C,0x8D}).Address(0x54250E, ModelsRva).Bytes(0x542512, {0xE8}).Branch(0x542513, ReleaseRva);
        if (!check) { status = check.Status(); return; }
        drainHook = safetyhook::create_inline(GameBase() + 0x5424E0, Drain);
        addHook = safetyhook::create_inline(GameBase() + 0x542000, Add);
        status = addHook && drainHook ? "installed" : "hook failed";
        if (!addHook || !drainHook) { addHook = {}; drainHook = {}; }
    }
}
