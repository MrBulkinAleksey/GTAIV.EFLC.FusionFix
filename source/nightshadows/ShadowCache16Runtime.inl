// LampShadowCaches 16: twice the game's static lamp shadow caches. The game was built for 16: its records
// (0x11A0B10, 0x100 apart), their per-buffer copies (0x119D0F0 + buffer * 0x1000), the lookup (0x925DB0) and the
// map binder (0x9268B0, index 8 on = cache index + 8) all hold 16, and 0x925EB0 lays all 16 out on the atlas.
// But the atlas SHADOW_MAP_COLOUR_CACHE is 4 x 4 tiles of the map size N, and the dynamic slots copy into tiles
// 8 to 15 (0x927640 with slot + 8), on top of caches 8 to 15, so the game uses only 8: the selection's loops end
// at the eighth record (victim 0x925D40, key search 0x928170, the per-run reset unrolled by four at 0x927AB7) and the
// refresh rotation counts & 7 (0x927F45).
//
// Here the atlas is 5 x 5 tiles: caches 0 to 15 in tiles 0 to 15, the slots in tiles 16 to 23 (slot 0, the
// scratch, at 16), tile 24 unused. Patched: the atlas size 4N -> 5N (0x92549A), the tile scale 4N -> 5N where a
// cache's tile is copied into a slot's map (0x925C74) and where the rewarp draws into it (0x926656, 0x926787),
// the slot copy's tiles per row 4 -> 5 (0x92764B) and its tile slot + 8 -> slot + 16 (0xD77FBC, 0xD781A6), the
// three loop ends and the rotation mask. The tiles' rectangles are data: the slots' table (0x119F980) and each
// cache record's +0xC0 (x, y, 0, size), rewritten after every selection run, since 0x925EB0 lays them out 4 x 4.
// The shaders take a tile's rectangle (c77) and sample inside it, whatever its size.
//
// Only before the game creates the atlas: one made 4N already keeps its size until the device is reset.
namespace ShadowCache16
{
    static bool enabled = false;
    static std::string status = "off";
    static float tileScale = 5.0f; // the atlas in maps, for the patched mulss operands

    static constexpr uintptr_t CacheRecord0 = 0xDA0B10;    // 0x11A0B10
    static constexpr uintptr_t SlotRects = 0xD9F980;       // 0x119F980
    static constexpr uintptr_t CacheAtlas = 0xD9CFF0;      // 0x119CFF0

    static void Rect(float* rect, unsigned tile) noexcept
    {
        rect[0] = float(tile % 5) * 0.2f;
        rect[1] = float(tile / 5) * 0.2f;
        rect[2] = 0.0f;
        rect[3] = 0.2f;
    }

    // After every selection run (and before the game copies the records for the frame's buffer)
    static void LayOut() noexcept
    {
        if (!enabled) return;
        const auto base = GameBase();
        for (unsigned slot = 0; slot < 8; ++slot)
            Rect(reinterpret_cast<float*>(base + SlotRects + slot * 0x10), 16 + slot);
        for (unsigned cache = 0; cache < fusionfix::shadows::MaxCacheEntries; ++cache)
            Rect(reinterpret_cast<float*>(base + CacheRecord0 + cache * 0x100 + 0xC0), cache);
    }

    static void Install(bool requested) noexcept
    {
        if (!requested) return;
        const auto base = GameBase();
        using HeadlightEnhancement::CodeCheck;
        const auto check = CodeCheck()
            .Bytes(0x52549A, {0x8D,0x04,0xB5,0x00,0x00,0x00,0x00})        // lea eax, [esi*4]: the atlas, 4N
            .Bytes(0x525D8D, {0x3D}).Address(0x525D8E, 0xDA13EC)            // victim: cmp eax, 0x11A13EC
            .Bytes(0x52818A, {0x3D}).Address(0x52818B, 0xDA13F0)            // key search: cmp eax, 0x11A13F0
            .Bytes(0x527B92, {0x81,0xF9}).Address(0x527B94, 0xDA14F4)       // per-run reset: cmp ecx, 0x11A14F4
            .Bytes(0x527F45, {0x81,0xE1,0x07,0x00,0x00,0x80})              // rotation: and ecx, 0x80000007
            .Bytes(0x527F4E, {0x83,0xC9,0xF8})                              // or ecx, -8
            .Bytes(0x525C74, {0xF3,0x0F,0x59,0x15}).Address(0x525C78, 0xBE8AB8) // mulss xmm2, [4.0]: cache into slot
            .Bytes(0x526656, {0xF3,0x0F,0x59,0x15}).Address(0x52665A, 0xBE8AB8) // mulss xmm2, [4.0]: rewarp
            .Bytes(0x526787, {0x8D,0x04,0x8D,0x00,0x00,0x00,0x00})        // lea eax, [ecx*4]: rewarp
            .Bytes(0x52764B, {0x6A,0x04})                                   // push 4: tiles per row
            .Bytes(0x977FBC, {0x8D,0x4B,0x08}).Bytes(0x9781A6, {0x8D,0x4B,0x08}); // lea ecx, [ebx+8]: slot tile
        if (!check) { status = check.Status(); return; }
        if (*reinterpret_cast<const uintptr_t*>(base + CacheAtlas)) { status = "atlas_already_created"; return; }

        const auto write32 = [base](uintptr_t rva, uint32_t value) { injector::WriteMemory<uint32_t>(base + rva, value, true); };
        const auto write8 = [base](uintptr_t rva, std::initializer_list<uint8_t> bytes)
        {
            uintptr_t at = base + rva;
            for (const auto b : bytes) injector::WriteMemory<uint8_t>(at++, b, true);
        };
        // Records and rectangles first, then the code that reaches them
        enabled = true;
        LayOut();
        write8(0x52549A, {0x8D,0x04,0xB6,0x90,0x90,0x90,0x90});          // lea eax, [esi+esi*4]
        write8(0x526787, {0x8D,0x04,0x89,0x90,0x90,0x90,0x90});          // lea eax, [ecx+ecx*4]
        write32(0x525C78, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&tileScale)));
        write32(0x52665A, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&tileScale)));
        write8(0x52764C, {0x05});
        write8(0x977FBE, {0x10});
        write8(0x9781A8, {0x10});
        write32(0x525D8E, static_cast<uint32_t>(base + 0xDA1BEC));
        write32(0x52818B, static_cast<uint32_t>(base + 0xDA1BF0));
        write32(0x527B94, static_cast<uint32_t>(base + 0xDA1CF4));
        write8(0x527F47, {0x0F});
        write8(0x527F50, {0xF0});
        fusionfix::shadows::cacheEntries = 16;
        status = "16";
    }
}
