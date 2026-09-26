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
