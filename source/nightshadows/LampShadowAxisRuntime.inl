// LampShadowAxis: which way a lamp's dual paraboloid shadow map looks.
//
// The game builds every lamp's map with the same fixed rotation (CE 0x924E50 for point lights, 0x925070 for spots,
// both called from the slot render at 0xD784C6/0xD784EA): the halves' axis is world +x, so the seam between them is
// the vertical plane through the lamp across x. The map's triangles are drawn straight in paraboloid space while
// their true image is curved, which is fine near a half's centre and far off near its rim: a shop front's awning
// 1-3 m from a lamp 4 m out from the wall came out bent around the lamp (the wall bowed 0.3 m, the awning dipped
// 1.7 m behind the wall at the seam), and its shadow had a lit hole in it.
//
// After either builder this turns the slot's camera rows (+0x40 x, +0x50 y, +0x60 the halves' axis, +0x70 position
// and the builder's shift along the axis) and the view matrix made from them (+0x80, the lighting's ShadowMatrix,
// c72-c75) to another axis: down for a point light, so the ground and the fronts under it sit near the lower half's
// centre, and along the beam for a spot. Everything else reads the axis from the slot: the map's render, the
// lighting's lookup, the cube faces (0x926D04) and LampShadowFaceCulling. A cached map keeps the matrix it was drawn
// with in its own record, so changing the mode leaves each cached map right until it is drawn again.

namespace LampShadowAxis
{
    static SafetyHookInline pointHook, spotHook;
    static std::atomic<int> mode{1};   // 0 the game's world +x, 1 down for points and along the beam for spots, 2 points
                                       // level toward the player (down when the player is under the lamp), spots along the beam
    // Mode 2: where the player is, from the slot selection (PlayerShadowAllocation, before the maps are built). A lamp's
    // map edge is where its halves' rims are: down put the walls beside a lamp there (a tunnel's cable cast a thick wavy
    // band along the wall), world +x the road along a tunnel running the other way (a car's shadow behind it drawn as
    // long black spikes); level toward the player puts you, your car and its shadow and the street toward you near a
    // half's centre, and the map is drawn again each frame, so it follows you
    static std::atomic<float> target[3]{};
    static std::atomic<bool> haveTarget{false};
    static void SetTarget(float x, float y, float z) noexcept
    {
        if (!std::isfinite(x + y + z)) return;
        target[0].store(x, std::memory_order_relaxed);
        target[1].store(y, std::memory_order_relaxed);
        target[2].store(z, std::memory_order_relaxed);
        haveTarget.store(true, std::memory_order_release);
    }
    static std::string status = "off";

    constexpr uintptr_t SlotRecordsRva = 0xD9F100, SlotSize = 0x110;

    static void Normalize(float (&v)[3]) noexcept
    {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (float& c : v) c /= length;
    }

    static void Cross(const float (&a)[3], const float (&b)[3], float (&out)[3]) noexcept
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    static void Turn(uint32_t slotIndex, bool spot) noexcept
    {
        if (mode.load(std::memory_order_relaxed) == 0 || slotIndex >= 16)
            return;
        const auto slot = GameBase() + SlotRecordsRva + slotIndex * SlotSize;
        auto* rows = reinterpret_cast<float*>(slot + 0x40);     // 4 rows of 4
        auto* view = reinterpret_cast<float*>(slot + 0x80);     // 4x4
        const auto* position = reinterpret_cast<const float*>(slot + 0xC0);
        const auto* direction = reinterpret_cast<const float*>(slot + 0xD0);

        float axis[3] = { 0.0f, 0.0f, -1.0f };
        if (!spot && mode.load(std::memory_order_relaxed) == 2 && haveTarget.load(std::memory_order_acquire))
        {
            const float dx = target[0].load(std::memory_order_relaxed) - position[0];
            const float dy = target[1].load(std::memory_order_relaxed) - position[1];
            const float level = std::sqrt(dx * dx + dy * dy);
            if (std::isfinite(level) && level > 2.0f)
                axis[0] = dx / level, axis[1] = dy / level, axis[2] = 0.0f;
        }
        if (spot)
        {
            const float length2 = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
            if (std::isfinite(length2) && length2 > 0.25f)
            {
                std::copy_n(direction, 3, axis);
                Normalize(axis);
            }
        }
        // The builder's shift of the position along its axis (0x924E50: the slot's +0x10; 0x925070: none)
        float shift = 0.0f;
        for (int i = 0; i < 3; ++i)
            shift += (rows[12 + i] - position[i]) * rows[8 + i];
        if (!std::isfinite(shift))
            return;

        // x = helper x axis, y = x x axis: the same handedness as the game's rows ((0,1,0), (0,0,-1), (1,0,0))
        const float helper[3] = { std::fabs(axis[2]) > 0.9f ? 1.0f : 0.0f, 0.0f, std::fabs(axis[2]) > 0.9f ? 0.0f : 1.0f };
        float x[3], y[3];
        Cross(helper, axis, x);
        Normalize(x);
        Cross(x, axis, y);
        const float* axes[3] = { x, y, axis };
        float eye[3];
        for (int i = 0; i < 3; ++i)
            eye[i] = position[i] + shift * axis[i];

        for (int r = 0; r < 3; ++r)
            std::copy_n(axes[r], 3, rows + r * 4);
        std::copy_n(eye, 3, rows + 12);
        for (int i = 0; i < 3; ++i)
        {
            for (int r = 0; r < 3; ++r)
                view[i * 4 + r] = axes[r][i];
            view[i * 4 + 3] = 0.0f;
        }
        for (int r = 0; r < 3; ++r)
            view[12 + r] = -(eye[0] * axes[r][0] + eye[1] * axes[r][1] + eye[2] * axes[r][2]);
        view[15] = 1.0f;
    }

    static void __cdecl Point(uint32_t a, uint32_t slotIndex, uint32_t c)
    {
        pointHook.unsafe_ccall(a, slotIndex, c);
        Turn(slotIndex, false);
    }

    static void __cdecl Spot(uint32_t a, uint32_t slotIndex, uint32_t c)
    {
        spotHook.unsafe_ccall(a, slotIndex, c);
        Turn(slotIndex, true);
    }

    static void Install() noexcept
    {
        using HeadlightEnhancement::CodeCheck;
        // Both builders: their prologue, the x row's first store (+0x40), the axis row's first store (+0x60) and the
        // view matrix handed on (lea ecx, [esi + slot + 0x80]).
        const auto check = CodeCheck()
            .Bytes(0x524E50, {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0xCC,0x00,0x00,0x00})
            .Bytes(0x524ED0, {0xF3,0x0F,0x11,0x96}).Address(0x524ED4, 0xD9F140)
            .Bytes(0x524F69, {0xF3,0x0F,0x11,0xBE}).Address(0x524F6D, 0xD9F160)
            .Bytes(0x52504E, {0x8D,0x8E}).Address(0x525050, 0xD9F180)
            .Bytes(0x525070, {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0xCC,0x00,0x00,0x00})
            .Bytes(0x5250EB, {0xF3,0x0F,0x11,0x96}).Address(0x5250EF, 0xD9F140)
            .Bytes(0x525184, {0xF3,0x0F,0x11,0xBE}).Address(0x525188, 0xD9F160)
            .Bytes(0x525269, {0x8D,0x8E}).Address(0x52526B, 0xD9F180);
        if (check)
        {
            pointHook = safetyhook::create_inline(GameBase() + 0x524E50, Point);
            spotHook = safetyhook::create_inline(GameBase() + 0x525070, Spot);
            status = pointHook && spotHook ? "installed" : "hook failed";
            if (!pointHook || !spotHook) { pointHook = {}; spotHook = {}; }
        }
        else
            status = check.Status();
        FusionLog::Write("NightShadows", "LampShadowAxis", "%s, mode %d\n", status.c_str(), mode.load());
    }
}
