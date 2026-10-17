module;

#include <common.hxx>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
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
//    nearest the player first; the render thread copies the latest whole set (Copy).
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

    // A body as a box on the ground: a car's footprint, or a person as a point (no length or width); its influence
    // reaches `reach` metres past the box.
    struct Body
    {
        float centre[2];
        float forward[2];
        float halfLength, halfWidth;
        float bottom;       // the ground it stands on: a car's wheels, a person's feet
        float reach;
        float distance2;    // from the player, squared
        bool car;
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
        found.clear();
        boxes.clear();
        const auto near2 = [&](const float* p) { const float x = p[0] - focus[0], y = p[1] - focus[1], z = p[2] - focus[2]; return x * x + y * y + z * z; };
        CVehicle::ForEachVehicle([&](uintptr_t vehicle)
        {
            CarBox box;
            if (!VehicleBox(vehicle, box))
                return;
            const float d2 = near2(box.centre);
            if (d2 > kRange * kRange)
                return;
            boxes.push_back(box);
            found.push_back({ { box.centre[0], box.centre[1] }, { box.forward[0], box.forward[1] }, box.halfLength, box.halfWidth,
                box.centre[2] - box.halfHeight, 0.5f, d2, true });
        });
        // People on foot; one inside a car's box sits in it
        CPed::ForEachPed([&](uintptr_t ped)
        {
            float p[3];
            if (!CEntity::GetPosition(ped, p))
                return;
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
            // The ped's position is about a metre above its feet
            found.push_back({ { p[0], p[1] }, { 1.0f, 0.0f }, 0.0f, 0.0f, p[2] - 1.0f, 0.6f, d2, false });
        });
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
