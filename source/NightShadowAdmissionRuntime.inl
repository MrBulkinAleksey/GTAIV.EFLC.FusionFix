// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

// FusionFix's existing light-admission workaround moved from fixes.ixx.
// It edits CE 927BD4 inside the allocator's guarded selection function. Keep
// ONE installer, after both startup guards and allocator hook preparation.
namespace NightShadowAdmission
{
    static SafetyHookMid admissionHook;
    static uintptr_t skip = 0;

    // Experimental: a lamp whose map is already in the static cache and published for lighting
    // drops the dynamic flag here, so the selection treats it as the game's static-only lamps:
    // lit from its cached map (the lookup falls back to it) without taking one of the seven
    // dynamic slots. esi+edi is the light, eax its flags.
    static bool cachedLamps = false;
    static std::atomic<uint32_t> cachedLampsKept{0};
    static void PreferCachedLamp(SafetyHookContext& regs) noexcept
    {
        constexpr uint32_t staticAndDynamic = rage::LF_STATIC_SHADOW | rage::LF_DYNAMIC_SHADOW;
        if ((regs.eax & staticAndDynamic) != staticAndDynamic || (regs.eax & rage::LF_VEHICLE) ||
            !ShadowLookupGuard::ready || !PlayerShadowAllocation::Enabled()) return;
        const auto& light = *reinterpret_cast<const rage::CLightSource*>(regs.edi + regs.esi);
        const int cache = light.mShadowCacheIndex;
        const auto key = static_cast<uint32_t>(light.mCastShadows);
        if (cache < 0 || cache >= 8 || !key || key == UINT32_MAX) return;
        const auto base = GameBase();
        if (*reinterpret_cast<const uint32_t*>(base + fusionfix::shadows::ce::allocation::CacheAge0Rva + cache * 0x100 + 4) != key) return;
        const int buffer = ShadowLookupGuard::ReadBuffer();
        if (buffer < 0 || buffer > 1 ||
            *reinterpret_cast<const uint32_t*>(base + shadow_lookup_layout::StaticKey0Rva + buffer * 0x1000 + cache * 0x100) != key) return;
        regs.eax &= ~static_cast<uint32_t>(rage::LF_DYNAMIC_SHADOW);
        ++cachedLampsKept;
    }

    static bool Install()
    {
        auto pattern = hook::pattern("A8 ? 0F 84 ? ? ? ? 8B C8");
        if (pattern.empty()) return false;
        skip = resolve_next_displacement(pattern.get_first(0)).value();
        injector::MakeNOP(pattern.get_first(2), 6);
        admissionHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
        {
            static auto extra = FusionFixSettings.GetRef("PREF_EXTRANIGHTSHADOWS");
            if (extra->get())
            {
                if ((regs.eax & 6) != 0) { if (cachedLamps) PreferCachedLamp(regs); return; }
            }
            else if ((regs.eax & 6) != 0 && Natives::IsInteriorScene()) return;
            return_to(skip);
        });
        return static_cast<bool>(admissionHook);
    }
}
