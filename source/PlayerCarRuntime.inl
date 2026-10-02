// The player's car, the one he drives or last drove, for everything here that treats it apart
// from traffic. Kept by the pool slot and its generation, so a car the game deleted and another
// that took its slot are not taken for it.
namespace PlayerCar
{
    // Identify the live pool slot and generation, not only a reusable pointer.
    static uint64_t VehicleToken(uintptr_t vehicle)
    {
        const auto pool = CVehicle::GetVehiclePool();
        if (!pool || !pool->m_aStorage || !pool->m_aFlags ||
            pool->m_nStorageSize <= 0 || pool->m_nSize <= 0) return 0;
        const auto start = reinterpret_cast<uintptr_t>(pool->m_aStorage);
        if (vehicle < start) return 0;
        const auto offset = vehicle - start;
        const auto stride = static_cast<uint32_t>(pool->m_nStorageSize);
        const auto index = offset / stride;
        if (offset % stride || index >= static_cast<uint32_t>(pool->m_nSize) ||
            index > 0x7FFFFF || pool->GetIsFree(index)) return 0;
        const auto handle = (index << 8) | pool->GetReference(index);
        return (static_cast<uint64_t>(handle) << 32) | vehicle;
    }

    // Vehicle offset of the driver where the layout is known (CE 0xF50), 0 elsewhere; the last car
    // stops being the player's once someone else drives it.
    static uint32_t driverOffset = 0;
    static std::atomic<uintptr_t> ped{0};
    static std::atomic<uint64_t> lastToken{0};

    // The last car while it is still the same car, the occupied one among them.
    static uintptr_t Last() noexcept
    {
        const auto token = lastToken.load(std::memory_order_acquire);
        if (!token) return 0;
        const auto vehicle = static_cast<uintptr_t>(static_cast<uint32_t>(token));
        return VehicleToken(vehicle) == token ? vehicle : 0;
    }

    static bool IsLast(uintptr_t vehicle) noexcept
    {
        return vehicle && Last() == vehicle;
    }

    // The player as the shadows around him take him: his ped, the car he sits in, and where he
    // is, the car's position while he is in one. Read when asked, on whichever thread asks.
    struct Focus
    {
        uintptr_t ped = 0, car = 0;
        float position[3]{};
    };

    // False without a ped or a position for him.
    static bool ReadFocus(Focus& focus) noexcept
    {
        if (!CPlayer::getLocalPlayerPed || !CPlayer::findPlayerCar) return false;
        focus.ped = CPlayer::getLocalPlayerPed();
        if (!CEntity::GetPosition(focus.ped, focus.position)) return false;
        focus.car = CPlayer::findPlayerCar();
        float car[3];
        if (CEntity::GetPosition(focus.car, car))
            std::copy(std::begin(car), std::end(car), focus.position);
        return true;
    }

    // Once a game frame, from the process callback.
    static void Update() noexcept
    {
        if (!CPlayer::getLocalPlayerPed || !CPlayer::findPlayerCar) return;
        const auto next = CPlayer::getLocalPlayerPed();
        if (next != ped.exchange(next, std::memory_order_acq_rel))
            lastToken.store(0, std::memory_order_release);
        if (!next) return;
        if (const auto car = CPlayer::findPlayerCar())
        {
            lastToken.store(VehicleToken(car), std::memory_order_release);
            return;
        }
        const auto last = Last();
        const auto driver = last && driverOffset ? *reinterpret_cast<const uintptr_t*>(last + driverOffset) : 0;
        if (!last || (driver && driver != next))
            lastToken.store(0, std::memory_order_release);
    }
}
