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
// LampShadowCaches 32: the records and their per-buffer copies move into the plugin, 32 of each (the game has room
// for 16 only, other state right after them), every instruction that names them is pointed there (49 sites: record
// fields by index * 0x100, loop starts and ends, the copies' base, size and the buffer step shl 4 -> 5), and the
// atlas keeps its 25 tiles: caches 0 to 11 have whole tiles (0 to 11), caches 12 to 31 quarter tiles (four in each of
// tiles 12 to 15 and 24) at half the resolution. The two places drawing into or out of a cache's tile at the map's
// whole size take its rectangle's size instead: the rewarp's quad (0x926852) and the copy into a slot's map
// (0x925CEA). A lamp nearer than CacheWholeTileDistance takes a whole tile and a farther one a quarter, where there
// is one to take (CacheChoice, from the selection's own pick at 0x927E29); a lamp in a quarter tile coming nearer
// gives it up for a whole one (LayOut).
//
// Only before the game creates the atlas: one made 4N already keeps its size until the device is reset.
namespace ShadowCache16
{
    static bool enabled = false, moved = false;
    static std::string status = "off";
    static float tileScale = 5.0f; // the atlas in maps, for the patched mulss operands
    static float wholeDistance = 25.0f; // CacheWholeTileDistance
    static constexpr unsigned WholeTiles = 12;
    static std::atomic<uint32_t> rectsFixed{0}, promotions{0}, choices{0};

    static constexpr uintptr_t SlotRects = 0xD9F980;       // 0x119F980
    static constexpr uintptr_t CacheAtlas = 0xD9CFF0;      // 0x119CFF0
    static constexpr uintptr_t MapSize = 0xD9D09C;         // 0x119D09C, N

    alignas(16) static uint8_t records[32 * 0x100];
    alignas(16) static uint8_t copies[2 * 32 * 0x100];

    static void Rect(float* rect, unsigned tile) noexcept
    {
        rect[0] = float(tile % 5) * 0.2f;
        rect[1] = float(tile / 5) * 0.2f;
        rect[2] = 0.0f;
        rect[3] = 0.2f;
    }

    // Cache i's rectangle on the atlas
    static void CacheRect(float* rect, unsigned i) noexcept
    {
        if (!moved || i < WholeTiles) { Rect(rect, i); return; }
        static constexpr unsigned quarterTiles[] = { 12, 13, 14, 15, 24 };
        const unsigned q = i - WholeTiles, tile = quarterTiles[(q / 4) % 5];
        rect[0] = float(tile % 5) * 0.2f + float(q & 1) * 0.1f;
        rect[1] = float(tile / 5) * 0.2f + float((q >> 1) & 1) * 0.1f;
        rect[2] = 0.0f;
        rect[3] = 0.1f;
    }

    static bool Whole(int i) noexcept { return !moved || i < int(WholeTiles); }

    // Entry fields as 0x9265A0 resets one
    static void Reset(uintptr_t record) noexcept
    {
        *reinterpret_cast<int32_t*>(record + 0xDC) = 0;
        *reinterpret_cast<int32_t*>(record + 0xE0) = 0;
        *reinterpret_cast<int32_t*>(record + 0xE4) = -1;
        *reinterpret_cast<int32_t*>(record + 0xE8) = -1;
        *reinterpret_cast<int32_t*>(record + 0xEC) = -1;
        *reinterpret_cast<float*>(record + 0xD0) = 65536.0f;
        *reinterpret_cast<int32_t*>(record + 0xF0) = 0;
    }

    // After every selection run (and before the game copies the records for the frame's buffer)
    static void LayOut() noexcept
    {
        if (!enabled) return;
        const auto base = GameBase();
        for (unsigned slot = 0; slot < 8; ++slot)
            Rect(reinterpret_cast<float*>(base + SlotRects + slot * 0x10), 16 + slot);
        const int size = *reinterpret_cast<const int32_t*>(base + MapSize);
        for (unsigned cache = 0; cache < fusionfix::shadows::cacheEntries; ++cache)
        {
            const auto record = fusionfix::shadows::CacheRecord(base, cache);
            CacheRect(reinterpret_cast<float*>(record + 0xC0), cache);
            // The map's size for the shadow filter's texel (0x9268B0)
            if (moved && size > 0) *reinterpret_cast<int32_t*>(record + 4) = Whole(int(cache)) ? size : size / 2;
        }
        if (!moved) return;
        // A lamp in a quarter tile that came near gives it up, once a whole tile is free or held by a farther lamp:
        // the lamp is then uncached and its new pick (CacheChoice) is a whole tile
        bool wholeFree = false;
        for (unsigned i = 0; i < WholeTiles && !wholeFree; ++i)
        {
            const auto record = fusionfix::shadows::CacheRecord(base, i);
            const int state = *reinterpret_cast<const int32_t*>(record + 0xE4);
            wholeFree = state == -1 || *reinterpret_cast<const float*>(record + 0xD0) > wholeDistance;
        }
        if (!wholeFree) return;
        for (unsigned i = WholeTiles; i < fusionfix::shadows::cacheEntries; ++i)
        {
            const auto record = fusionfix::shadows::CacheRecord(base, i);
            if (*reinterpret_cast<const int32_t*>(record + 0xE4) != 1) continue;
            if (*reinterpret_cast<const float*>(record + 0xD0) >= wholeDistance * 0.75f) continue;
            Reset(record);
            ++promotions;
            break; // one a run
        }
    }

    // The selection's pick of an entry for a new cache (0x927E29, EDX; -1 none) for a lamp at distance (game's measure):
    // the same rule as the game's (the oldest unused entry, else the farthest one farther than the lamp), first among
    // the whole tiles for a near lamp and the quarter ones for a far one, then the other kind.
    static int CacheChoice(int native, float distance) noexcept
    {
        if (!moved || !std::isfinite(distance)) return native;
        const auto base = GameBase();
        const auto pick = [&](unsigned from, unsigned to) noexcept
        {
            int unused = -1, farthest = -1, oldest = 0;
            float most = distance;
            for (unsigned i = from; i < to; ++i)
            {
                const auto record = fusionfix::shadows::CacheRecord(base, i);
                const int state = *reinterpret_cast<const int32_t*>(record + 0xE4);
                const int age = *reinterpret_cast<const int32_t*>(record + 0xDC);
                const float d = *reinterpret_cast<const float*>(record + 0xD0);
                if (state == -1) { if (unused == -1 || age > oldest) { unused = int(i); oldest = age; } }
                else if (d > most) { most = d; farthest = int(i); }
            }
            return unused != -1 ? unused : farthest;
        };
        const bool near = distance < wholeDistance;
        int choice = near ? pick(0, WholeTiles) : pick(WholeTiles, fusionfix::shadows::cacheEntries);
        if (choice < 0) choice = near ? pick(WholeTiles, fusionfix::shadows::cacheEntries) : pick(0, WholeTiles);
        ++choices;
        return choice;
    }

    // 0x926852, the rewarp's quad (0x438030: x0, y0, x1, y1 in clip space on the stack), ESI the cache index * 0x100
    static void RewarpQuad(SafetyHookContext& regs) noexcept
    {
        const unsigned i = regs.esi >> 8;
        if (i >= fusionfix::shadows::cacheEntries) return;
        const auto* r = reinterpret_cast<const float*>(fusionfix::shadows::CacheRecord(GameBase(), i) + 0xC0);
        auto* quad = reinterpret_cast<float*>(regs.esp);
        quad[0] = r[0] * 2.0f - 1.0f;
        quad[1] = -(r[1] * 2.0f - 1.0f);
        quad[2] = (r[0] + r[3]) * 2.0f - 1.0f;
        quad[3] = -((r[1] + r[3]) * 2.0f - 1.0f);
        ++rectsFixed;
    }

    // 0x925CEA, the copy of a cache's tile into a slot's map (0x9273B0): [esp+0x1C..0x28] the source x0, y0, x1, y1
    // in pixels, [ebp+8] the cache index
    static void CopySource(SafetyHookContext& regs) noexcept
    {
        const int i = *reinterpret_cast<const int32_t*>(regs.ebp + 8);
        if (i < 0 || unsigned(i) >= fusionfix::shadows::cacheEntries) return;
        const auto base = GameBase();
        const auto* r = reinterpret_cast<const float*>(fusionfix::shadows::CacheRecord(base, unsigned(i)) + 0xC0);
        const int atlas = *reinterpret_cast<const int32_t*>(base + MapSize) * 5;
        auto* source = reinterpret_cast<int32_t*>(regs.esp + 0x1C);
        const int side = int(r[3] * float(atlas) + 0.5f);
        source[2] = source[0] + side;
        source[3] = source[1] + side;
    }

    static SafetyHookMid rewarpHook, copyHook;

    // Moves the records and copies into the plugin for 32 (after the 16 patches, whose loop ends it rewrites)
    static bool Move(uintptr_t base, std::string& failed) noexcept
    {
        using HeadlightEnhancement::CodeCheck;
        constexpr uint32_t Old = 0x11A0B10, OldEnd = 0x11A1B10, OldCopies = 0x119D0F0, OldCopiesEnd = 0x119F0F0;
        // Field sites: the 4-byte operand (offset in the instruction) names record 0 plus a field (by index * 0x100)
        static constexpr std::pair<uint32_t, uint8_t> fields[] = {
            {0x92559C, 1}, {0x925AAD, 1}, {0x925B3C, 1}, {0x925C7C, 4}, {0x925C84, 4}, {0x925D5E, 1}, {0x925DEA, 2},
            {0x9260E8, 1}, {0x9265AC, 2}, {0x9265B6, 2}, {0x9265C0, 2}, {0x9265CA, 2}, {0x9265D4, 2}, {0x9265DE, 2},
            {0x9265E8, 2}, {0x926666, 4}, {0x926671, 4}, {0x9266A8, 4}, {0x926BDC, 2}, {0x926C20, 2}, {0x927AB7, 1},
            {0x927C2C, 2}, {0x927CE6, 2}, {0x927CF0, 2}, {0x927CF6, 4}, {0x927CFE, 2}, {0x927EF1, 2}, {0x927EFF, 2},
            {0x927F05, 2}, {0x927F0F, 2}, {0x927F19, 2}, {0x927F23, 2}, {0x927F67, 2}, {0x927F76, 2}, {0x927F83, 2},
            {0x928012, 2}, {0x928040, 2}, {0x92804A, 2}, {0x928179, 1}, {0xD77D49, 2}, {0xD78578, 2},
        };
        // Loop ends after the sixteenth record
        static constexpr std::pair<uint32_t, uint8_t> ends[] = {
            {0x9255A8, 1}, {0x925AFE, 2}, {0x925D8D, 1}, {0x92617E, 2}, {0x927B92, 2}, {0x92818A, 1},
        };
        // The per-buffer copies' base
        static constexpr std::pair<uint32_t, uint8_t> copySites[] = { {0x925B41, 2}, {0x925E34, 2}, {0x92695D, 2} };
        const auto operand = [base](uint32_t va, uint8_t at) { return base + (va - 0x400000) + at; };
        const auto value = [&](uint32_t va, uint8_t at) { return *reinterpret_cast<const uint32_t*>(operand(va, at)) - (base - 0x400000); };
        for (const auto& [va, at] : fields)
            if (const auto v = value(va, at); v < Old || v >= OldEnd) { failed = std::format("field {:X}", va); return false; }
        for (const auto& [va, at] : ends)
            if (const auto v = value(va, at); v < OldEnd || v >= OldEnd + 0x200) { failed = std::format("end {:X}", va); return false; }
        for (const auto& [va, at] : copySites)
            if (const auto v = value(va, at); v < OldCopies || v >= OldCopies + 0x100) { failed = std::format("copies {:X}", va); return false; }
        const auto check = CodeCheck()
            .Bytes(0x525B34, {0x68,0x00,0x10,0x00,0x00})   // push 0x1000: the copies' size
            .Bytes(0x525B39, {0xC1,0xE6,0x0C})             // shl esi, 0xC: a buffer's copies
            .Bytes(0x525E2C, {0xC1,0xE2,0x04})             // shl edx, 4: buffer * 16 in the lookup
            .Bytes(0x526952, {0xC1,0xE0,0x04})             // shl eax, 4: buffer * 16 in the binder
            .Bytes(0x527F45, {0x81,0xE1,0x0F,0x00,0x00,0x80}).Bytes(0x527F4E, {0x83,0xC9,0xF0}) // rotation & 15
            .Bytes(0x526852, {0xE8}).Bytes(0x525CEA, {0xE8});
        if (!check) { failed = check.Status(); return false; }

        // The game's 16 as they are; the others reset (0x9256F0 lays all out again when the device comes)
        const auto oldRecords = base + (Old - 0x400000);
        std::memcpy(records, reinterpret_cast<const void*>(oldRecords), 16 * 0x100);
        for (unsigned i = 16; i < 32; ++i)
        {
            std::memcpy(records + i * 0x100, records, 0x100);
            Reset(reinterpret_cast<uintptr_t>(records + i * 0x100));
        }
        std::memcpy(copies, records, sizeof(records));
        std::memcpy(copies + sizeof(records), records, sizeof(records));
        const auto newRecords = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(records));
        const auto newCopies = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(copies));
        const auto write32 = [](uintptr_t at, uint32_t v) { injector::WriteMemory<uint32_t>(at, v, true); };
        for (const auto& [va, at] : fields) write32(operand(va, at), newRecords + (value(va, at) - Old));
        for (const auto& [va, at] : ends) write32(operand(va, at), newRecords + (value(va, at) - Old) + 16 * 0x100);
        for (const auto& [va, at] : copySites) write32(operand(va, at), newCopies + (value(va, at) - OldCopies));
        write32(base + 0x525B35, 32 * 0x100);
        injector::WriteMemory<uint8_t>(base + 0x525B3B, 13, true); // shl esi, 13: 0x2000 a buffer
        injector::WriteMemory<uint8_t>(base + 0x525E2E, 5, true);
        injector::WriteMemory<uint8_t>(base + 0x526954, 5, true);
        injector::WriteMemory<uint8_t>(base + 0x527F47, 0x1F, true);
        injector::WriteMemory<uint8_t>(base + 0x527F50, 0xE0, true);
        fusionfix::shadows::cacheRecordsAt = reinterpret_cast<uintptr_t>(records);
        fusionfix::shadows::cacheCopiesAt = reinterpret_cast<uintptr_t>(copies);
        fusionfix::shadows::cacheCopyStride = 32 * 0x100;
        fusionfix::shadows::cacheEntries = 32;
        moved = true;
        rewarpHook = safetyhook::create_mid(base + 0x526852, RewarpQuad);
        copyHook = safetyhook::create_mid(base + 0x525CEA, CopySource);
        return true;
    }

    static void Install(int requested, float wholeTileDistance) noexcept
    {
        if (requested < 16) return;
        wholeDistance = std::clamp(wholeTileDistance, 0.0f, 200.0f);
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
        fusionfix::shadows::cacheEntries = 16;
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
        status = "16";
        if (requested < 32) return;
        std::string failed;
        if (Move(base, failed)) { LayOut(); status = "32"; }
        else status = "16 (32 failed: " + failed + ")";
    }
}
