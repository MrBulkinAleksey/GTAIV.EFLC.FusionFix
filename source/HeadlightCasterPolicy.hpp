// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include "ShadowAdapterCE.hpp"

namespace fusionfix::shadows::caster
{
    // Only copied identities, never dereference an opaque light key.
    struct Context
    {
        std::uintptr_t car = 0;
        std::array<std::uintptr_t, 9> occupants{};
        bool ownBeam = false;
        std::uintptr_t trafficBeamKey = 0; // Validated submitted beam; never dereferenced.
        std::uintptr_t nearPed = 0; // The player on foot right at the car of the beam.
    };

    inline bool OwnBeam(std::uint32_t slot, std::uint32_t kind, bool active,
                        std::uintptr_t key, std::uintptr_t car) noexcept
    {
        return slot >= 1 && slot <= 7 && kind == 4 && active &&
            ce::IsVehicleBeam(key, car);
    }

    inline bool Exclude(const Context& context, std::uintptr_t entity,
                        std::uint32_t type, bool artificial) noexcept
    {
        if (!entity || !artificial) return false;
        if (type == 2 && ce::IsVehicleBeam(context.trafficBeamKey, entity)) return true;
        if (type == 3 && entity == context.nearPed) return true;
        if (!context.ownBeam || !context.car) return false;
        if (type == 2) return entity == context.car;
        if (type == 3)
            for (const auto occupant : context.occupants)
                if (occupant == entity) return true;
        return false;
    }

    // Even an invalid nested pass replaces the outer context until it returns.
    class Scope
    {
        Context& current;
        Context saved;
    public:
        Scope(Context& target, const Context& next) noexcept : current(target), saved(target)
        { current = next; }
        ~Scope() noexcept { current = saved; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
}
