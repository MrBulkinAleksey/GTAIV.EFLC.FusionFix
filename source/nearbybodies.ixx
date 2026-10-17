module;

#include <common.hxx>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

export module nearbybodies;

import common;
import comvars;

// The cars and people around the player, for effects that react to them (the grass bending under them, the box
// shadows of cars).
//  - VehicleBox: a car's box from its matrix and its model's bounds, from the wheels' bottom to the roof; cars, vans,
//    buses and bikes only, upright and whole.
//  - Bodies: taken on the main thread once a game frame while anyone asked for them in the last second (Want), the
//    nearest the player first; the render thread copies the latest whole set (Copy). Each has its velocity on the
//    ground from its last position. Helicopters with their blades turning come as rotors: their centre, height and
//    blade speed, for the downwash.
export namespace NearbyBodies
{
    struct CarBox
    {
        float centre[3];
        float forward[2];   // unit, on the ground
        float halfLength, halfWidth, halfHeight;
        float local[3];     // the box's middle in the car's own axes
        const float* lo;    // the model's bounds (+0x20 least, +0x30 most): x right, y forward, z up
        const float* hi;
        const float* matrix;
    };

    // False for what is not a car, van, bus or bike (helicopters with their rotors, anything broken) or lies on its side.
    bool VehicleBox(uintptr_t vehicle, CarBox& box);

    // A body as a box on the ground: a car's footprint, or a person's feet as a small square; its influence
    // reaches `reach` metres past the box.
    struct Body
    {
        float centre[2];
        float forward[2];
        float halfLength, halfWidth;
        float bottom;       // the ground it stands on: a car's wheels, a person's feet
        float reach;
        float distance2;    // from the player, squared
        float velocity[2];  // m/s on the ground, 0 for the first sight
        enum Kind : uint8_t { Person, Car, Rotor } kind;
        float rotorSpeed;   // a rotor's blades, 0..1 of full speed
    };
    constexpr int kMaxBodies = 32;
    constexpr float kRange = 60.0f; // metres from the player

    // Any thread, every frame the bodies are needed: they are taken for a second after the last call.
    void Want();

    // Render thread: the latest set, the nearest first; false when none was taken yet.
    bool Copy(std::vector<Body>& bodies, float (&focus)[3]);
}

namespace
{
    std::atomic<DWORD> lastWant = 0;
    std::mutex latestMutex;
    std::vector<NearbyBodies::Body> latest;
    float latestFocus[3] = {};
    bool latestTaken = false;

    // Where each body was seen last, for its velocity; a slot taken by another entity jumps and gets none
    struct Seen { float position[2]; float velocity[2]; int32_t time; uint32_t pass; };
    std::unordered_map<uintptr_t, Seen> seen;
    uint32_t seenPass = 0;

    void TakeVelocity(uintptr_t entity, const float* position, int32_t now, float (&velocity)[2])
    {
        auto [it, added] = seen.try_emplace(entity, Seen{ { position[0], position[1] }, { 0.0f, 0.0f }, now, seenPass });
        auto& s = it->second;
        if (!added)
        {
            const float dt = (now - s.time) * 0.001f;
            if (dt > 0.0f)
            {
                const float vx = (position[0] - s.position[0]) / dt, vy = (position[1] - s.position[1]) / dt;
                const bool sane = dt < 0.5f && vx * vx + vy * vy < 80.0f * 80.0f;
                s.velocity[0] = sane ? vx : 0.0f;
                s.velocity[1] = sane ? vy : 0.0f;
                s.position[0] = position[0], s.position[1] = position[1];
                s.time = now;
            }
            s.pass = seenPass;
        }
        velocity[0] = s.velocity[0], velocity[1] = s.velocity[1];
    }

    // CVehicle +0x1304: the vehicle's type, 4 a helicopter (SET_HELI_BLADES_FULL_SPEED's handler, CE 0xbce2f0, checks it)
    bool IsHeli(uintptr_t vehicle)
    {
        return *reinterpret_cast<const int32_t*>(vehicle + 0x1304) == 4;
    }

    // CHeli +0x1ED4: the blades' speed, 0..1, rising while the engine runs and falling after (CE 0xb6cd90)
    float BladeSpeed(uintptr_t heli)
    {
        const float speed = *reinterpret_cast<const float*>(heli + 0x1ED4);
        return std::isfinite(speed) ? std::clamp(speed, 0.0f, 1.0f) : 0.0f;
    }

    void Gather()
    {
        using namespace NearbyBodies;
        const DWORD want = lastWant.load(std::memory_order_relaxed);
        if (!want || GetTickCount() - want > 1000 || !CPlayer::getLocalPlayerPed)
            return;
        float focus[3];
        if (!CEntity::GetPosition(CPlayer::getLocalPlayerPed(), focus))
            return;
        static std::vector<Body> found;
        static std::vector<CarBox> boxes;
        static std::vector<std::array<float, 3>> helis;
        found.clear();
        boxes.clear();
        helis.clear();
        const int32_t now = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds : 0;
        ++seenPass;
        const auto near2 = [&](const float* p) { const float x = p[0] - focus[0], y = p[1] - focus[1], z = p[2] - focus[2]; return x * x + y * y + z * z; };
        CVehicle::ForEachVehicle([&](uintptr_t vehicle)
        {
            if (IsHeli(vehicle))
            {
                // Its downwash, from its position down, while the blades turn; how far it reaches is left to whoever
                // takes the bodies
                float p[3];
                if (!CEntity::GetPosition(vehicle, p))
                    return;
                helis.push_back({ p[0], p[1], p[2] });
                const float blades = BladeSpeed(vehicle);
                const float d2 = near2(p);
                if (blades < 0.05f || d2 > kRange * kRange)
                    return;
                Body rotor{ { p[0], p[1] }, { 1.0f, 0.0f }, 0.0f, 0.0f, p[2], 0.0f, d2, {}, Body::Rotor, blades };
                TakeVelocity(vehicle, p, now, rotor.velocity);
                found.push_back(rotor);
                return;
            }
            CarBox box;
            if (!VehicleBox(vehicle, box))
                return;
            const float d2 = near2(box.centre);
            if (d2 > kRange * kRange)
                return;
            boxes.push_back(box);
            Body car{ { box.centre[0], box.centre[1] }, { box.forward[0], box.forward[1] }, box.halfLength, box.halfWidth,
                box.centre[2] - box.halfHeight, 0.5f, d2, {}, Body::Car, 0.0f };
            TakeVelocity(vehicle, box.centre, now, car.velocity);
            found.push_back(car);
        });
        // People on foot; one inside a car's box sits in it, one within 4 m of a helicopter flies in it
        CPed::ForEachPed([&](uintptr_t ped)
        {
            float p[3];
            if (!CEntity::GetPosition(ped, p))
                return;
            for (const auto& h : helis)
            {
                const float x = p[0] - h[0], y = p[1] - h[1], z = p[2] - h[2];
                if (x * x + y * y + z * z < 16.0f)
                    return;
            }
            const float d2 = near2(p);
            if (d2 > kRange * kRange)
                return;
            for (const auto& box : boxes)
            {
                const float dx = p[0] - box.centre[0], dy = p[1] - box.centre[1];
                if (std::abs(dx * box.forward[0] + dy * box.forward[1]) < box.halfLength &&
                    std::abs(dx * box.forward[1] - dy * box.forward[0]) < box.halfWidth && std::abs(p[2] - box.centre[2]) < box.halfHeight + 0.5f)
                    return;
            }
            // The ped's position is about a metre above its feet; the feet and legs as a square of 0.3 m, reaching a metre
            // past it, so the grass leans away from a step before it and rises again behind it
            Body person{ { p[0], p[1] }, { 1.0f, 0.0f }, 0.15f, 0.15f, p[2] - 1.0f, 1.0f, d2, {}, Body::Person, 0.0f };
            TakeVelocity(ped, p, now, person.velocity);
            found.push_back(person);
        });
        std::erase_if(seen, [](const auto& entry) { return entry.second.pass != seenPass; });
        if (int(found.size()) > kMaxBodies)
        {
            std::nth_element(found.begin(), found.begin() + kMaxBodies, found.end(), [](const Body& a, const Body& b) { return a.distance2 < b.distance2; });
            found.resize(kMaxBodies);
        }
        std::sort(found.begin(), found.end(), [](const Body& a, const Body& b) { return a.distance2 < b.distance2; });
        std::lock_guard lock(latestMutex);
        latest = found;
        std::copy_n(focus, 3, latestFocus);
        latestTaken = true;
    }

    struct Init
    {
        Init()
        {
            FusionFix::onGameProcessEvent() += Gather;
        }
    } NearbyBodiesInit;
}

namespace NearbyBodies
{
    bool VehicleBox(uintptr_t vehicle, CarBox& box)
    {
        static const uintptr_t modelInfos = GameBase() + 0xE95CD8; // CE 0x1295cd8, model infos by index
        if (!vehicle)
            return false;
        const float* m = CEntity::GetMatrix(vehicle);
        const auto index = *reinterpret_cast<const int16_t*>(vehicle + 0x2E);
        if (!m || index < 0 || index >= 31000)
            return false;
        const auto info = *reinterpret_cast<const uintptr_t*>(modelInfos + index * 4);
        if (!info)
            return false;
        const auto lo = reinterpret_cast<const float*>(info + 0x20), hi = reinterpret_cast<const float*>(info + 0x30);
        const float halfWidth = (hi[0] - lo[0]) * 0.5f, halfLength = (hi[1] - lo[1]) * 0.5f;
        // From the wheels' bottom, so the box always meets the ground under the car (a box shadow meets the car where its
        // contact shadow is: only a car's or ped's pixel above the box's foot escapes it, by the material IDs)
        const float bottom = lo[2];
        const float halfHeight = (hi[2] - bottom) * 0.5f;
        // Cars, vans, buses and bikes; not helicopters with their rotors, nor anything broken
        if (!(halfWidth > 0.1f && halfWidth < 2.0f && halfLength > 0.2f && halfLength < 10.0f && halfHeight > 0.1f && halfHeight < 2.5f))
            return false;
        const float flat = std::sqrt(m[4] * m[4] + m[5] * m[5]);
        if (!(flat > 0.5f))
            return false;
        const float local[3] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (bottom + hi[2]) * 0.5f };
        for (int i = 0; i < 3; ++i)
            box.centre[i] = m[12 + i] + m[i] * local[0] + m[4 + i] * local[1] + m[8 + i] * local[2];
        if (!std::isfinite(box.centre[0]) || !std::isfinite(box.centre[1]) || !std::isfinite(box.centre[2]))
            return false;
        box.forward[0] = m[4] / flat;
        box.forward[1] = m[5] / flat;
        box.halfLength = halfLength;
        box.halfWidth = halfWidth;
        box.halfHeight = halfHeight;
        std::copy_n(local, 3, box.local);
        box.lo = lo;
        box.hi = hi;
        box.matrix = m;
        return true;
    }

    void Want()
    {
        lastWant.store((std::max)(GetTickCount(), DWORD(1)), std::memory_order_relaxed);
    }

    bool Copy(std::vector<Body>& bodies, float (&focus)[3])
    {
        std::lock_guard lock(latestMutex);
        if (!latestTaken)
            return false;
        bodies = latest;
        std::copy_n(latestFocus, 3, focus);
        return true;
    }
}
