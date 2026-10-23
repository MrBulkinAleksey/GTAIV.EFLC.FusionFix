module;

#include <common.hxx>
#include <d3dx9tex.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <regex>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include "FusionLog.hpp"

export module vehicleboxshadows;

import common;
import comvars;
import d3dx9_43;
import framegeneration;
import framehistory;
import hdr;
import natives;
import settings;
import shaders;
import renderscale;
import renderskip;
import temporal;
import upscaler;
import postfxresources;
import nearbybodies;

// VehicleBoxShadows: a light whose shadow map holds no cars (no dynamic slot this frame: lamps lit from their cache
// or casting no shadows, most headlights) left the cars under it without a shadow, with only the dark frame of the
// contact shadow round them. Each car is taken as a box with rounded edges, which the light shaders shadow
// (shaders/patches/local_light_vehicle_box_shadows.patch, c143-c149). The boxes are taken on the main thread as the
// game hands the frame's light list over (CE 0xac2dd0) and kept with that list, so the render thread shades a frame
// with the cars where they stood when its lights were made.
export struct VehicleBoxShadows
{
    struct Box
    {
        float centre[3], halfLength;
        float forward[2], halfWidth, halfHeight;
        uintptr_t vehicle;
        float local[3];   // the box's middle in the car's own axes
        int32_t slot;     // the car's place in the vehicle pool and its flags then, to tell it is still the same car
        uint8_t flags;
        float velocity[3]; // m/s, from the last hand over
        float turning;     // radians a second about the vertical, from the last hand over
    };
    static constexpr int kBoxes = 160, kPerLight = 5, kLists = 8;
    // PedCapsuleShadows: people on foot, each a standing capsule the same light shaders shadow (c150-c154,
    // shaders/patches/ped_capsule_shadows.patch), up to kPedsPerLight a light. Their position: x, y, z of the ped.
    static constexpr int kPeds = 96, kPedsPerLight = 4;
    // PropShapeShadows: lamp posts, bins, hydrants, benches, containers, barriers, kiosks and other props, each one or
    // two simple shapes the same light shaders shadow (shaders/patches/prop_shape_shadows.patch): a capsule between
    // two points or a box turned about the vertical. A light's shapes go into a 64 x 1 float texture on s7 just
    // before it is drawn, two texels each, their count in c115.x; up to 32, PropShapesPerLight of them. Posts are in
    // no light's map (the caster loop takes an object into the wd_local pass only with +0x210 bit 0x800, from a map
    // dummy, which posts lack), so they shadow under every light, lamps with a slot too; other props only under a
    // light without a map this frame, and with the bit only under one without a cache either.
    struct Shape
    {
        // capsule: one end and the radius, the other end and -1; box: the centre and the half length, the forward
        // x and y, the half width and the half height (above 0)
        float t0[4], t1[4];
    };
    struct Prop
    {
        Shape shapes[2];
        int shapeCount;
        float centre[3], reach;     // a sphere round its shapes, to pick them by
        uintptr_t entity;
        bool inMaps;    // +0x210 bit 0x800: drawn into the lamps' maps by the game, their caches too
        bool post;      // a lamp post, which no lamp's map ever shows
    };
    static constexpr int kProps = 512, kShapesPerLight = 32, kShapeTexels = kShapesPerLight * 2;
    static constexpr int kBodyShapes = 16;
    struct List
    {
        std::atomic<uintptr_t> lights{0}; // the light list these boxes go with
        uint32_t sequence = 0;            // which hand over: the game's three lists come round again every third
        int count = 0;
        float focus[3] = {};              // the player, where the boxes' shadows are seen
        uintptr_t watched = 0;            // the car nearest the player, which VehicleBoxShadowsLog follows
        float frameSeconds = 0.0f;        // since the hand over before
        LARGE_INTEGER handedAt{};         // when, for the log
        Box boxes[kBoxes];
        int pedCount = 0;
        float peds[kPeds][3];
        int propCount = 0;
        Prop props[kProps];
        // PedBoneShadows: the player's capsules along the bones (BodyShapes), a sphere round them to pick them by
        int bodyCount = 0;
        Shape body[kBodyShapes];
        float bodyCentre[3] = {}, bodyReach = 0.0f;
    };
    static inline List lists[kLists]{};
    static inline int nextList = 0;
    static inline uintptr_t* lightListBuilt = nullptr; // CE 0x103eedc, the list the main thread fills
    static inline uintptr_t* lightListDrawn = nullptr; // CE 0x103eed0, the list the render thread draws
    static inline uintptr_t modelInfos = 0;             // CE 0x1295cd8, model infos by index
    static inline SafetyHookMid shHandOver{};
    // For the log: frames drawn with their boxes found or not, lights shaded with some.
    static inline std::atomic<uint32_t> framesMatched{0}, framesUnmatched{0}, lightsWithBoxes{0}, pedsWithShadows{0};
    static inline const List* drawn = nullptr;
    static inline List lastFound{}; // a copy, render thread: the ring's entry may be reused while it is drawn
    static inline uintptr_t drawnFor = 0;
    static inline std::atomic<uint32_t> latestHandOver{0}; // the last hand over's sequence, for the log
    static inline bool constantsOn = true;
    // The cars each lamp took last, render thread.
    // What each lamp took last, render thread: its cars, and since when its own map shows them.
    static constexpr uint8_t kPending = 0, kReal = 1, kBox = 2;
    static constexpr DWORD kDecideMs = 500; // a car coming under a lamp without a slot waits this long for one
    struct Fading
    {
        Box box{};
        float strength = 0.0f;
        bool chosen = false;
        float shape[3] = { -1.0f, -1.0f, -1.0f }; // along, across and height shares from the light's angle, eased
        // VehicleBoxShadowsUntilLeaving: which shadow the car has from the lamp until it leaves, decided as it came
        uint8_t latch = kPending;
        DWORD takenTick = 0;
    };
    struct LampState
    {
        std::array<uintptr_t, kPerLight> cars{}; // the cars it took last frame
        std::array<Fading, kPerLight> boxes{};   // the boxes it draws, fading in (taken) or out (left)
        int count = 0;
        float slotStrength = 0.0f;               // all its boxes, fading out while its own map shows the cars
        float rounding = -1.0f;                  // the edges' rounding it gave its boxes, easing to the limit
        bool wasMappedForFade = false;           // its own map showed the cars last frame
        DWORD lastTick = 0;
        DWORD leftOutTick = 0;                   // its slot's map last left its boxed cars out (BoxedCars)
        // VehicleBoxShadowsLog: what it last did with the watched car
        bool hadWatched = false, wasMapped = false, watchedLow = false;
        uint8_t watchedLatch = 0xFF;
    };
    static inline std::unordered_map<uint64_t, LampState> held;
    // The props each lamp took last, render thread
    static inline std::unordered_map<uint64_t, std::array<uintptr_t, kShapesPerLight>> heldProps;
    static inline IDirect3DTexture9* shapesTexture = nullptr; // on the real device, released when it is lost
    static inline bool shapesBound = false;
    static inline std::atomic<uint32_t> propsWithShadows{0};
    static inline std::atomic<uint32_t> bodiesWithShadows{0}; // lights given the player's bones (PedBoneShadows)
    // The lamps and the cars they keep boxes for (VehicleBoxShadowsUntilLeaving), gathered as a frame's lights are
    // drawn and handed to the night shadow slots on the next; render thread, main thread copies.
    static constexpr int kPairs = 256;
    static inline std::array<VehicleBoxShadowsBridge::BoxedPair, kPairs> pairsBuilding{}, pairsPublished{};
    static inline int pairsBuildingCount = 0, pairsPublishedCount = 0;
    static inline DWORD pairsPublishedTick = 0;
    static inline std::mutex pairsLock;
    static void PublishPairs()
    {
        std::lock_guard lock(pairsLock);
        std::copy_n(pairsBuilding.begin(), pairsBuildingCount, pairsPublished.begin());
        pairsPublishedCount = pairsBuildingCount;
        pairsPublishedTick = GetTickCount();
        pairsBuildingCount = 0;
    }
    static int CopyBoxedPairs(VehicleBoxShadowsBridge::BoxedPair* pairs, int max)
    {
        std::lock_guard lock(pairsLock);
        if (!PostFxResources.VehicleBoxShadowsEnabled() || GetTickCount() - pairsPublishedTick > 500)
            return 0;
        const int n = (std::min)(pairsPublishedCount, max);
        std::copy_n(pairsPublished.begin(), n, pairs);
        return n;
    }
    // For the log: lamps whose slot drew its map without its boxed cars, and those cars.
    static inline std::atomic<uint32_t> mapsWithoutBoxed{0}, carsLeftOut{0};
    // For the log: the nearest car's model bounds (least, most) and its box, as taken.
    static inline float nearestBounds[6] = {}, nearestBox[4] = {}, nearestDistance = -1.0f;

    // VehicleBoxShadowsPerCar: under a row of lamps a car had a box shadow from every one that reached it, some
    // ten in a log, many of them lamps the game gives no shadow at all, each shadow faint but together a fan of
    // them. Each lamp drawn notes how much it lights each car it reaches; on the next frame only the strongest
    // few on a car may give it a box. Render thread.
    struct CarLamps
    {
        static constexpr int kKept = 8;
        std::array<std::pair<uint64_t, float>, kKept> seen{};    // this frame: lamp and how much it lights the car
        int seenCount = 0;
        std::array<std::pair<uint64_t, float>, kKept> before{};  // the frame before, for lamps not drawn in this one
        int beforeCount = 0;
        std::array<uint64_t, kKept> allowed{};                    // from the last frame: those that may give a box
        int allowedCount = 0;
        bool ranked = false;                                      // allowed holds a ranking (not the car's first frame)
    };
    static inline std::unordered_map<uintptr_t, CarLamps> carLamps;
    static inline uint32_t carLampsFrame = 0; // TemporalAA::SceneFrame of the last ranking

    static bool LampAllowed(const CarLamps& car, uint64_t lamp)
    {
        return std::find(car.allowed.begin(), car.allowed.begin() + car.allowedCount, lamp) != car.allowed.begin() + car.allowedCount;
    }

    // The light a lamp gives a car: its intensity times a falloff over its radius.
    static void NoteCarLamp(uintptr_t vehicle, uint64_t lamp, float light)
    {
        auto& car = carLamps[vehicle];
        // A lamp giving the car a box now keeps it against one up to a third stronger, so two alike lamps do not
        // take the box from each other frame by frame
        if (LampAllowed(car, lamp))
            light *= 1.33f;
        for (int i = 0; i < car.seenCount; ++i)
            if (car.seen[i].first == lamp)
            {
                car.seen[i].second = (std::max)(car.seen[i].second, light);
                return;
            }
        if (car.seenCount < CarLamps::kKept)
            car.seen[car.seenCount++] = { lamp, light };
        else
        {
            auto weakest = std::min_element(car.seen.begin(), car.seen.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            if (weakest->second < light)
                *weakest = { lamp, light };
        }
    }

    // At a new scene's lights: each car's strongest lamps of the last two frames may give it a box in this one. By
    // the scene frame, not by the light list drawn: lights drawn from more than one list in a frame ranked the cars
    // from part of their lamps each time, and a lamp took and left a car every other frame (a log: 49 times in a
    // few seconds, its box never faded in). A lamp not drawn in one frame keeps its place from the one before.
    static void RankCarLamps(int perCar)
    {
        if (carLamps.size() > 1024)
            carLamps.clear();
        for (auto it = carLamps.begin(); it != carLamps.end();)
        {
            auto& car = it->second;
            if (!car.seenCount && !car.beforeCount)
            {
                it = carLamps.erase(it);
                continue;
            }
            std::array<std::pair<uint64_t, float>, CarLamps::kKept * 2> both{};
            int n = 0;
            for (int i = 0; i < car.seenCount; ++i)
                both[n++] = car.seen[i];
            for (int i = 0; i < car.beforeCount; ++i)
            {
                bool again = false;
                for (int j = 0; j < car.seenCount; ++j)
                    again |= car.seen[j].first == car.before[i].first;
                if (!again)
                    both[n++] = car.before[i];
            }
            std::sort(both.begin(), both.begin() + n, [](const auto& a, const auto& b) { return a.second > b.second; });
            car.allowedCount = (std::min)(n, perCar);
            for (int i = 0; i < car.allowedCount; ++i)
                car.allowed[i] = both[i].first;
            car.ranked = true;
            car.before = car.seen;
            car.beforeCount = car.seenCount;
            car.seenCount = 0;
            ++it;
        }
    }

    // Main thread: every car's box, the nearest the player first when there are more than fit.
    // The lamp post models (their names' hashes, model info +0x3c): their bounds take in the arm out to the lamp, so
    // their post stands at the entity's own position, its foot, PropShapePostRadius thick, and the arm reaches from
    // its top to the far end of the bounds
    static bool IsPost(uint32_t hash)
    {
        switch (hash)
        {
        case 0x67105BD6: case 0x46C33C49: case 0xD7B45B49: case 0x12272A5F: case 0xBA49A074: case 0xBAA79CDA:
        case 0x3A8020FF: case 0x70AB8D51: case 0x9EED69D4: case 0x97DFDA59: case 0x4AF44083: case 0x5B38E534:
        case 0x6CB90840: case 0x0B9145F2: case 0x1CD7687E: case 0x307B9012: case 0x2D7E0A07: case 0x2E9B0C05:
        case 0x70A9D8E5: case 0xBEF47579: case 0x1485E82F: case 0xD4E8589D: case 0x9A978A05: case 0x3D4E4F74:
        case 0x0F1C59B2: case 0x517FD33A:
            return true;
        default:
            return false;
        }
    }

    static void Capsule(Prop& prop, const float a[3], const float b[3], float radius)
    {
        auto& shape = prop.shapes[prop.shapeCount++];
        std::copy_n(a, 3, shape.t0);
        shape.t0[3] = radius;
        std::copy_n(b, 3, shape.t1);
        shape.t1[3] = -1.0f;
    }

    // Objects within PropShapeDistance of the player as shapes, the nearest kProps:
    // - lamp posts: the post and its arm, two capsules;
    // - upright and narrow (0.7 m across at most): a standing capsule as thick as its narrow way (a sign's plate is
    //   no thicker than its pole);
    // - upright and wider: a box turned as the prop, no taller than 3.2 m nor wider than 6 m (a billboard high on
    //   its post would cast a wall);
    // - leaning or lying, one way clearly the longest: a capsule along it.
    static void CaptureProps(List& list, const float focus[3])
    {
        const auto& R = PostFxResources;
        static std::array<std::pair<float, Prop>, 2048> found{};
        int n = 0;
        const float reach2 = R.fPropShapeDistance * R.fPropShapeDistance;
        CObject::ForEachObject([&](uintptr_t object)
        {
            if (n >= int(found.size()))
                return;
            const float* m = CEntity::GetMatrix(object);
            const auto index = *reinterpret_cast<const int16_t*>(object + 0x2E);
            if (!m || index < 0 || index >= 31000)
                return;
            const float x = m[12] - focus[0], y = m[13] - focus[1], z = m[14] - focus[2];
            const float d2 = x * x + y * y + z * z;
            if (!(d2 < reach2))
                return;
            const auto info = *reinterpret_cast<const uintptr_t*>(modelInfos + index * 4);
            if (!info)
                return;
            const float* lo = reinterpret_cast<const float*>(info + 0x20);
            const float* hi = reinterpret_cast<const float*>(info + 0x30);
            float half[3], mid[3];
            for (int k = 0; k < 3; ++k)
            {
                half[k] = (hi[k] - lo[k]) * 0.5f;
                mid[k] = (hi[k] + lo[k]) * 0.5f;
            }
            if (!(half[0] >= 0.0f && half[1] >= 0.0f && half[2] >= 0.0f) || (std::max)({ half[0], half[1], half[2] }) < 0.15f ||
                (std::max)({ half[0], half[1], half[2] }) > 8.0f)
                return;
            // the model's place p in the world
            const auto world = [m](const float p[3], float out[3])
            {
                for (int k = 0; k < 3; ++k)
                    out[k] = m[12 + k] + m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2];
            };
            Prop prop{};
            prop.entity = object;
            prop.inMaps = (*reinterpret_cast<const uint32_t*>(object + 0x210) & 0x800) != 0;
            const bool upright = m[10] > 0.95f;
            prop.post = upright && IsPost(*reinterpret_cast<const uint32_t*>(info + 0x3C));
            if (prop.post)
            {
                const float radius = R.fPropShapePostRadius;
                const float footLocal[3] = { 0.0f, 0.0f, lo[2] }, topLocal[3] = { 0.0f, 0.0f, hi[2] - radius };
                float foot[3], top[3];
                world(footLocal, foot);
                world(topLocal, top);
                Capsule(prop, foot, top, radius);
                // the arm out to the lamp: the bounds' far end along the way they reach furthest from the post
                const float reachX = (std::max)(std::abs(lo[0]), std::abs(hi[0])), reachY = (std::max)(std::abs(lo[1]), std::abs(hi[1]));
                const bool alongX = reachX >= reachY;
                const float armReach = alongX ? reachX : reachY;
                if (armReach > 1.0f)
                {
                    const float end = alongX ? (std::abs(hi[0]) >= std::abs(lo[0]) ? hi[0] : lo[0])
                                             : (std::abs(hi[1]) >= std::abs(lo[1]) ? hi[1] : lo[1]);
                    const float armZ = hi[2] - 0.35f;
                    const float startLocal[3] = { 0.0f, 0.0f, armZ };
                    const float endLocal[3] = { alongX ? end * 0.9f : 0.0f, alongX ? 0.0f : end * 0.9f, armZ };
                    float start[3], tip[3];
                    world(startLocal, start);
                    world(endLocal, tip);
                    Capsule(prop, start, tip, R.fPropShapeArmRadius);
                }
            }
            else if (upright && (std::max)(half[0], half[1]) <= 0.35f)
            {
                if (half[2] < 0.25f)
                    return;
                // PropShapeScale: the bounds take in a bin's lid, wider than its body, and some room; a capsule as wide
                // threw a shadow a fifth wider than the game's own of the bin
                const float radius = std::clamp((std::min)(half[0], half[1]) * R.fPropShapeScale, 0.05f, 0.35f);
                const float a[3] = { mid[0], mid[1], lo[2] + (std::min)(radius, half[2]) };
                const float b[3] = { mid[0], mid[1], hi[2] - (std::min)(radius, half[2]) };
                float wa[3], wb[3];
                world(a, wa);
                world(b, wb);
                Capsule(prop, wa, wb, radius);
            }
            else if (upright)
            {
                if (half[2] > 1.6f || (std::max)(half[0], half[1]) > 3.0f || half[2] < 0.15f)
                    return;
                const float flat = std::sqrt(m[0] * m[0] + m[1] * m[1]);
                if (!(flat > 0.5f))
                    return;
                auto& shape = prop.shapes[prop.shapeCount++];
                // no side under 5 cm: the shader divides by their squares, and a flat sign's 0 made its pixels NaN, black
                world(mid, shape.t0);
                shape.t0[3] = (std::max)(half[0] * R.fPropShapeScale, 0.05f);
                shape.t1[0] = m[0] / flat;
                shape.t1[1] = m[1] / flat;
                shape.t1[2] = (std::max)(half[1] * R.fPropShapeScale, 0.05f);
                shape.t1[3] = (std::max)(half[2], 0.05f);
            }
            else
            {
                int k = 0;
                for (int j = 1; j < 3; ++j)
                    if (half[j] > half[k])
                        k = j;
                const float other = (std::max)(half[(k + 1) % 3], half[(k + 2) % 3]);
                if (half[k] < 1.5f * other || half[k] < 0.4f)
                    return;
                const float radius = std::clamp(other * R.fPropShapeScale, 0.05f, 0.5f);
                float a[3] = { mid[0], mid[1], mid[2] }, b[3] = { mid[0], mid[1], mid[2] };
                a[k] -= half[k] - radius;
                b[k] += half[k] - radius;
                float wa[3], wb[3];
                world(a, wa);
                world(b, wb);
                Capsule(prop, wa, wb, radius);
            }
            world(mid, prop.centre);
            prop.reach = std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
            if (!prop.shapeCount || !std::isfinite(prop.centre[0] + prop.centre[1] + prop.centre[2]))
                return;
            found[n++] = { d2, prop };
        });
        if (n > kProps)
            std::nth_element(found.begin(), found.begin() + kProps, found.begin() + n,
                [](const auto& a, const auto& b) { return a.first < b.first; });
        list.propCount = (std::min)(n, kProps);
        for (int i = 0; i < list.propCount; ++i)
            list.props[i] = found[i].second;
    }

    // PedBoneShadows: the player as 16 capsules between the bones as posed this frame (CPed::GetBoneMatrix), into
    // list.body; none where a bone is missing or stands off the ped (another skeleton, a ped not yet posed). Each
    // capsule's other end carries -2 for -1: the light shaders leave such shapes out on peds' and the player's own
    // pixels, as they would shadow the body they stand in (zz_ped_capsule_look.patch).
    static void BodyShapes(List& list, uintptr_t ped, const float position[3])
    {
        list.bodyCount = 0;
        if (!CPed::GetBoneMatrix || !ped)
            return;
        enum { Pelvis, Head, LThigh, LCalf, LFoot, RThigh, RCalf, RFoot, LUpper, LFore, LHand, RUpper, RFore, RHand, Bones };
        static constexpr uint32_t ids[Bones] = { 0x1A1, 0x4B5, 0x1A2, 0x1A3, 0x1A4, 0x1A7, 0x1A8, 0x1A9, 0x4C1, 0x4C2, 0x4C3, 0x4C8, 0x4C9, 0x4D0 };
        float at[Bones][3];
        for (int i = 0; i < Bones; ++i)
        {
            float m[16]{};
            CPed::GetBoneMatrix(ped, nullptr, m, ids[i]);
            const float dx = m[12] - position[0], dy = m[13] - position[1], dz = m[14] - position[2];
            if (!std::isfinite(dx + dy + dz) || dx * dx + dy * dy + dz * dz > 2.0f * 2.0f)
                return;
            at[i][0] = m[12]; at[i][1] = m[13]; at[i][2] = m[14];
        }
        const auto capsule = [&list](const float a[3], const float b[3], float radius)
        {
            auto& shape = list.body[list.bodyCount++];
            std::copy_n(a, 3, shape.t0);
            shape.t0[3] = radius;
            std::copy_n(b, 3, shape.t1);
            shape.t1[3] = -2.0f;
        };
        // the way from the pelvis to the head: the body's up, which the neck and the head's middle lie along
        float up[3] = { at[Head][0] - at[Pelvis][0], at[Head][1] - at[Pelvis][1], at[Head][2] - at[Pelvis][2] };
        const float upLength = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (!(upLength > 0.2f))
            return;
        for (auto& v : up)
            v /= upLength;
        const auto along = [](const float from[3], const float way[3], float metres, float out[3])
        {
            for (int k = 0; k < 3; ++k)
                out[k] = from[k] + way[k] * metres;
        };
        float neck[3], headLow[3], headHigh[3], hips[3];
        along(at[Head], up, -0.08f, neck);
        along(at[Head], up, 0.03f, headLow);
        along(at[Head], up, 0.11f, headHigh);
        along(at[Pelvis], up, 0.05f, hips);
        capsule(hips, neck, 0.14f);                    // the body
        capsule(at[LUpper], at[RUpper], 0.08f);        // the shoulders
        capsule(at[LThigh], at[RThigh], 0.1f);         // the hips
        capsule(headLow, headHigh, 0.1f);              // the head
        // the feet forward from the ankles, low: the way from the thigh's knee over the ankle gives no forward, the ped's
        // matrix does
        const float* m = CEntity::GetMatrix(ped);
        float forward[3] = { m ? m[4] : 0.0f, m ? m[5] : 0.0f, 0.0f };
        const float forwardLength = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
        if (forwardLength > 0.1f)
            forward[0] /= forwardLength, forward[1] /= forwardLength;
        for (const int side : { 0, 1 })
        {
            const int thigh = side ? RThigh : LThigh, calf = side ? RCalf : LCalf, foot = side ? RFoot : LFoot;
            const int upper = side ? RUpper : LUpper, fore = side ? RFore : LFore, hand = side ? RHand : LHand;
            capsule(at[thigh], at[calf], 0.085f);
            capsule(at[calf], at[foot], 0.06f);
            float toe[3];
            along(at[foot], forward, 0.14f, toe);
            toe[2] -= 0.05f;
            capsule(at[foot], toe, 0.045f);
            capsule(at[upper], at[fore], 0.055f);
            capsule(at[fore], at[hand], 0.045f);
            // the hand on from the wrist, the way the forearm points
            float way[3] = { at[hand][0] - at[fore][0], at[hand][1] - at[fore][1], at[hand][2] - at[fore][2] };
            const float wayLength = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
            if (wayLength > 0.05f)
            {
                for (auto& v : way)
                    v /= wayLength;
                float tip[3];
                along(at[hand], way, 0.09f, tip);
                capsule(at[hand], tip, 0.045f);
            }
        }
        std::copy_n(at[Pelvis], 3, list.bodyCentre);
        list.bodyReach = 1.3f;
    }

    static void Capture()
    {
        auto& list = lists[nextList];
        nextList = (nextList + 1) % kLists;
        list.lights.store(0, std::memory_order_release);
        list.count = 0;
        static uint32_t handOvers = 0;
        list.sequence = ++handOvers;
        QueryPerformanceCounter(&list.handedAt);
        latestHandOver.store(handOvers, std::memory_order_relaxed);
        if (!PostFxResources.VehicleBoxShadowsEnabled() || !lightListBuilt || !modelInfos)
            return;
        float focus[3] = {};
        if (!CPlayer::getLocalPlayerPed || !CEntity::GetPosition(CPlayer::getLocalPlayerPed(), focus))
            return;
        static std::array<std::pair<float, Box>, 256> found{};
        int n = 0;
        const auto pool = CVehicle::GetCheckedVehiclePool();
        float nearest = 1.0e30f;
        list.watched = 0;
        CVehicle::ForEachVehicle([&](uintptr_t vehicle)
        {
            if (n >= int(found.size()))
                return;
            NearbyBodies::CarBox car;
            if (!NearbyBodies::VehicleBox(vehicle, car))
                return;
            const float* m = car.matrix;
            const float* lo = car.lo;
            const float* hi = car.hi;
            Box box{};
            std::copy_n(car.centre, 3, box.centre);
            std::copy_n(car.forward, 2, box.forward);
            box.halfLength = car.halfLength;
            box.halfWidth = car.halfWidth;
            box.halfHeight = car.halfHeight;
            box.vehicle = vehicle;
            std::copy_n(car.local, 3, box.local);
            box.slot = -1;
            if (pool && vehicle >= uintptr_t(pool->m_aStorage))
            {
                const auto offset = vehicle - uintptr_t(pool->m_aStorage);
                if (offset % pool->m_nStorageSize == 0 && offset / pool->m_nStorageSize < uint32_t(pool->m_nSize))
                {
                    box.slot = int32_t(offset / pool->m_nStorageSize);
                    box.flags = pool->m_aFlags[box.slot];
                }
            }
            const float x = box.centre[0] - focus[0], y = box.centre[1] - focus[1], z = box.centre[2] - focus[2];
            const float d2 = x * x + y * y + z * z;
            if (d2 < nearest)
            {
                nearest = d2;
                list.watched = vehicle;
                std::copy_n(lo, 3, nearestBounds);
                std::copy_n(hi, 3, nearestBounds + 3);
                nearestBox[0] = box.halfLength; nearestBox[1] = box.halfWidth; nearestBox[2] = box.halfHeight;
                nearestBox[3] = box.centre[2] - m[14]; // the box's middle above the car's position
            }
            found[n++] = { d2, box };
        });
        if (n > kBoxes)
            std::nth_element(found.begin(), found.begin() + kBoxes, found.begin() + n,
                [](const auto& a, const auto& b) { return a.first < b.first; });
        nearestDistance = n ? std::sqrt(nearest) : -1.0f;
        std::copy_n(focus, 3, list.focus);
        // Each car's speed since the last hand over, which VehicleBoxShadowsLeadFrames moves its box ahead by. A
        // second hand over in the same frame (CE 0xac507b) keeps the speeds it had.
        {
            struct Seen { float centre[3], velocity[3], forward[2], turning; };
            static std::unordered_map<uintptr_t, Seen> previous;
            static LARGE_INTEGER frequency{}, before{};
            LARGE_INTEGER time{};
            QueryPerformanceCounter(&time);
            if (!frequency.QuadPart)
                QueryPerformanceFrequency(&frequency);
            const float dt = before.QuadPart ? float(time.QuadPart - before.QuadPart) / float(frequency.QuadPart) : 0.0f;
            // A new frame's hand over; one after a pause (a load, the menu) gives no speeds, but is the time the
            // next is measured from: kept only on a step, the time stuck at the first pause over 0.25 s and every
            // car had no speed from then on (a log: speed 0 in every line, the guess 1 to 8 m behind the drawn car)
            const bool fresh = !before.QuadPart || dt > 0.002f;
            const bool step = fresh && before.QuadPart && dt < 0.25f;
            std::unordered_map<uintptr_t, Seen> current;
            current.reserve(size_t(n));
            for (int i = 0; i < n; ++i)
            {
                auto& box = found[i].second;
                const auto it = previous.find(box.vehicle);
                Seen seen{};
                std::copy_n(box.centre, 3, seen.centre);
                std::copy_n(box.forward, 2, seen.forward);
                if (it != previous.end() && (step || !fresh))
                {
                    if (step)
                    {
                        for (int k = 0; k < 3; ++k)
                            seen.velocity[k] = std::clamp((box.centre[k] - it->second.centre[k]) / dt, -80.0f, 80.0f);
                        // the signed angle from the forward then to the forward now
                        const float* f0 = it->second.forward;
                        const float angle = std::atan2(f0[0] * box.forward[1] - f0[1] * box.forward[0], f0[0] * box.forward[0] + f0[1] * box.forward[1]);
                        seen.turning = std::clamp(angle / dt, -10.0f, 10.0f);
                    }
                    else
                    {
                        std::copy_n(it->second.centre, 3, seen.centre);
                        std::copy_n(it->second.velocity, 3, seen.velocity);
                        std::copy_n(it->second.forward, 2, seen.forward);
                        seen.turning = it->second.turning;
                    }
                }
                std::copy_n(seen.velocity, 3, box.velocity);
                box.turning = seen.turning;
                current.emplace(box.vehicle, seen);
            }
            // a second hand over in the frame keeps the frame's time too, its boxes moved on as the first's
            static float frameSeconds = 0.0f;
            if (fresh)
            {
                previous.swap(current);
                before = time;
                frameSeconds = step ? dt : 0.0f;
            }
            list.frameSeconds = frameSeconds;
        }
        list.count = (std::min)(n, kBoxes);
        for (int i = 0; i < list.count; ++i)
            list.boxes[i] = found[i].second;
        // People on foot within 80 m of the player, the player too unless as bones (PedBoneShadows); one inside a car's
        // box sits in it, in its shadow.
        list.pedCount = 0;
        list.bodyCount = 0;
        const uintptr_t player = CPlayer::getLocalPlayerPed();
        if (PostFxResources.bPedCapsuleShadows && PostFxResources.bPedBoneShadows && !CPlayer::findPlayerCar())
            BodyShapes(list, player, focus);
        if (PostFxResources.bPedCapsuleShadows)
            CPed::ForEachPed([&](uintptr_t ped)
            {
                float p[3];
                if (list.pedCount >= kPeds || (ped == player && list.bodyCount) || !CEntity::GetPosition(ped, p) ||
                    !std::isfinite(p[0] + p[1] + p[2]))
                    return;
                const float x = p[0] - focus[0], y = p[1] - focus[1], z = p[2] - focus[2];
                if (x * x + y * y + z * z > 80.0f * 80.0f)
                    return;
                for (int i = 0; i < list.count; ++i)
                {
                    const auto& box = list.boxes[i];
                    const float dx = p[0] - box.centre[0], dy = p[1] - box.centre[1];
                    if (std::abs(dx * box.forward[0] + dy * box.forward[1]) < box.halfLength &&
                        std::abs(dx * box.forward[1] - dy * box.forward[0]) < box.halfWidth && std::abs(p[2] - box.centre[2]) < box.halfHeight + 0.5f)
                        return;
                }
                std::copy_n(p, 3, list.peds[list.pedCount++]);
            });
        list.propCount = 0;
        if (PostFxResources.bPropShapeShadows)
            CaptureProps(list, focus);
        list.lights.store(*lightListBuilt, std::memory_order_release);
    }

    static void Install()
    {
        // The hand over of the frame's light list: mov ecx, [index]; xor eax, eax; inc ecx; cmp ecx, 3; cmove ecx, eax;
        // mov eax, [the list filled]; and where the render thread takes a list: add esi, lists; mov [drawn], esi
        auto pattern = hook::pattern("8B 0D ? ? ? ? 33 C0 41 83 F9 03 0F 44 C8 A1 ? ? ? ? A3");
        auto drawnPattern = hook::pattern("81 C6 ? ? ? ? 89 35 ? ? ? ? 8B 49 0C");
        if (pattern.empty() || drawnPattern.empty())
            return;
        lightListBuilt = *pattern.get_first<uintptr_t*>(16);
        lightListDrawn = *drawnPattern.get_first<uintptr_t*>(8);
        modelInfos = GameBase() + 0xE95CD8;
        shHandOver = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext&) { Capture(); });
        VehicleBoxShadowsBridge::BoxedCars = BoxedCars;
        VehicleBoxShadowsBridge::CopyBoxedPairs = CopyBoxedPairs;
        VehicleBoxShadowsBridge::Enabled = []() { return PostFxResources.VehicleBoxShadowsEnabled(); };
        VehicleBoxShadowsBridge::MaxDistance = []() { return PostFxResources.fVehicleBoxShadowsMaxDistance; };
    }

    // VehicleBoxShadowsLog: what happens to the shadows of the car nearest the player, to catch a flicker, written to
    // GTAIV.EFLC.FusionFix.VehicleBoxShadows.log only as it changes: a lamp taking the car or leaving it (with its
    // rank among the cars the lamp took and the box's strength), a lamp taking or losing its shadow slot, a lamp
    // left out as no higher than the roof, a frame drawn without its own boxes; each second how many lamps shade
    // the car. Ctrl+Shift+F7 or F12 put a mark where you saw something. Render thread; written every quarter second.
    static inline std::string traceText;
    static inline DWORD traceFlushed = 0, traceSecond = 0;
    static inline uint32_t traceStaleWatched = 0, traceLampsOnWatched = 0, traceLampsSeen = 0, traceDrawnWatched = 0, traceLeadWatched = 0;
    // Why the watched car's drawn matrix was not taken: not among the G-buffer's draws, none near enough (and how
    // far the nearest stood, its matrix's translation, and how many draws it had), facing another way
    static inline uint32_t traceDrawnNone = 0, traceDrawnFar = 0, traceDrawnHeading = 0, traceDrawnCount = 0, traceDrawnHeadingOnly = 0;
    static inline bool traceDrawsDescribed = false;
    static inline DWORD traceOffsetLast = 0;
    static inline float traceDrawnNearest = -1.0f;
    static inline float traceStrongest = 0.0f;
    static inline bool traceMarkDown = false, traceShotDown = false;
    static void Trace(const char* format, ...)
    {
        char line[512];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        traceText += std::format("t={} ", GetTickCount());
        traceText += line;
        traceText += '\n';
    }
    // Once a frame, as a new light list is drawn.
    static void TraceFrame()
    {
        const DWORD now = GetTickCount();
        const bool mark = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F7) & 0x8000);
        const bool shot = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (mark && !traceMarkDown)
            Trace("M mark (Ctrl+Shift+F7)");
        if (shot && !traceShotDown)
            Trace("M mark (screenshot)");
        traceMarkDown = mark;
        traceShotDown = shot;
        if (now - traceSecond >= 1000)
        {
            if (traceSecond)
                Trace("S in the last second: %u lamp draws shaded the watched car (strongest %.2f), %u lamps with boxes, %u times its box stood where it was handed over, %u where it was drawn, %u moved ahead by its speed; drawn: %u with the heading only; not drawn: %u not among the draws, %u none near (nearest %.2f m of %u draws), %u none facing its way",
                    traceLampsOnWatched, traceStrongest, traceLampsSeen, traceStaleWatched, traceDrawnWatched, traceLeadWatched, traceDrawnHeadingOnly,
                    traceDrawnNone, traceDrawnFar, traceDrawnNearest, traceDrawnCount, traceDrawnHeading);
            traceSecond = now;
            traceLampsOnWatched = traceLampsSeen = traceStaleWatched = traceDrawnWatched = traceLeadWatched = 0;
            traceDrawnNone = traceDrawnFar = traceDrawnHeading = traceDrawnCount = traceDrawnHeadingOnly = 0;
            traceDrawnNearest = -1.0f;
            traceStrongest = 0.0f;
            traceDrawsDescribed = false;
        }
        if (!traceText.empty() && now - traceFlushed >= 250)
        {
            FusionLog::WriteText("VehicleBoxShadows", "Trace", traceText);
            traceText.clear();
            traceFlushed = now;
        }
    }
    // After a lamp has chosen: its changes for the watched car.
    static void TraceLamp(const rage::CLightSource& light, LampState& state, bool mapped, bool leftOut, bool low, int taken,
        const std::pair<float, const Box*> (&nearest)[kPerLight])
    {
        const auto watched = drawn->watched;
        int rank = -1;
        for (int i = 0; i < taken; ++i)
            if (nearest[i].second->vehicle == watched)
                rank = i;
        float strength = 0.0f, distance = -1.0f;
        for (int i = 0; i < state.count; ++i)
            if (state.boxes[i].box.vehicle == watched)
            {
                strength = state.boxes[i].strength;
                const auto& c = state.boxes[i].box.centre;
                distance = std::sqrt((light.mPosition.x - c[0]) * (light.mPosition.x - c[0]) +
                    (light.mPosition.y - c[1]) * (light.mPosition.y - c[1]) + (light.mPosition.z - c[2]) * (light.mPosition.z - c[2]));
            }
        ++traceLampsSeen;
        const bool has = strength > 0.0f && state.slotStrength > 0.0f;
        if (has)
        {
            ++traceLampsOnWatched;
            traceStrongest = (std::max)(traceStrongest, strength * state.slotStrength);
        }
        const auto where = std::format("lamp at {:.1f} {:.1f} {:.1f} (flags 0x{:X}, radius {:.1f})", light.mPosition.x, light.mPosition.y,
            light.mPosition.z, light.mFlags, light.mRadius);
        if (rank >= 0 && !state.hadWatched)
            Trace("+ %s took the car, %d of %d, %.1f m off, strength %.2f, lamp strength %.2f", where.c_str(), rank + 1, taken, distance, strength, state.slotStrength);
        else if (rank < 0 && state.hadWatched)
            Trace("- %s left the car, %s, %d taken, fading %.2f", where.c_str(), low ? "no higher than its roof" : taken ? "others nearer" : "out of reach",
                taken, strength);
        if (mapped != state.wasMapped && (rank >= 0 || state.hadWatched))
            Trace("%c %s %s its shadow slot%s, lamp strength %.2f", mapped ? 'P' : 'U', where.c_str(), mapped ? "took" : "lost",
                leftOut ? " (its map leaves its boxed cars out)" : "", state.slotStrength);
        uint8_t latch = 0xFF;
        for (int i = 0; i < state.count; ++i)
            if (state.boxes[i].box.vehicle == watched && state.boxes[i].chosen)
                latch = state.boxes[i].latch;
        if (latch != state.watchedLatch && latch != 0xFF)
            Trace("K %s gives the car %s", where.c_str(), latch == kReal ? "its map's shadow" : latch == kBox ? "a box until it leaves" : "a box while it waits for a slot");
        state.watchedLatch = latch;
        if (low != state.watchedLow)
            Trace("L %s %s", where.c_str(), low ? "is no higher than the car's roof, left out" : "is above the car's roof again");
        state.hadWatched = rank >= 0;
        state.wasMapped = mapped;
        state.watchedLow = low;
    }

    // The car where this frame's G-buffer drew it: object motion vectors record each draw's world matrix, and the
    // lights are drawn after the G-buffer, so the box stands exactly where the car and its contact shadow are.
    // Handed over earlier and moved ahead by the car's speed, the box ran ahead of and behind the car by turns, as
    // the game moves cars in steps of its own, and the shadow's front and back flickered.
    static bool FromDrawn(Box& box, float lead, bool watched)
    {
        // The car's own position from the box: its right is its forward turned a quarter. Moved on by its speed
        // to when it is drawn: handed over two or three frames before, a car driven fast had gone further.
        const float right[2] = { box.forward[1], -box.forward[0] };
        const float around[3] = { box.centre[0] - right[0] * box.local[0] - box.forward[0] * box.local[1] + box.velocity[0] * lead,
                                box.centre[1] - right[1] * box.local[0] - box.forward[1] * box.local[1] + box.velocity[1] * lead,
                                box.centre[2] - box.local[2] + box.velocity[2] * lead };
        const float speed = std::sqrt(box.velocity[0] * box.velocity[0] + box.velocity[1] * box.velocity[1] + box.velocity[2] * box.velocity[2]);
        // Of the car's draws, the nearest facing its way, within 3 m (its parts: a log found the nearest draw always
        // facing away, a wheel or a door, the body drawn skinned with no matrix of its own at the car). Its
        // rotation is the car's as drawn; its position only where it stands at the car's own, else the box keeps
        // the one moved on by speed.
        int draws = 0;
        float nearest = -1.0f, m[16];
        bool body = false;
        if (!TemporalAA::DrawnWorldMatrix(reinterpret_cast<void*>(box.vehicle), around, 3.0f + (std::min)(speed * 0.05f, 2.0f), m, &draws, &nearest, box.forward, &body, 25.0f))
        {
            if (watched)
            {
                if (draws < 0)
                    ++traceDrawnNone;
                else if (nearest >= 0.0f)
                {
                    ++traceDrawnFar;
                    traceDrawnNearest = nearest;
                    traceDrawnCount = uint32_t(draws);
                }
                else
                    ++traceDrawnHeading;
                // Once a second, what the car's draws were (a log found none facing the car's way)
                if (!traceDrawsDescribed)
                {
                    traceDrawsDescribed = true;
                    traceText += std::format("t={} D {}\n", GetTickCount(), TemporalAA::DescribeDraws(reinterpret_cast<void*>(box.vehicle), around, box.forward));
                }
            }
            return false;
        }
        const float flat = std::sqrt(m[4] * m[4] + m[5] * m[5]);
        const float fx = m[4] / flat, fy = m[5] / flat;
        // The body (skinned, turned by its chassis) stands where the car was drawn: its position is taken however far
        // from the one moved on by speed, which is what was off when driven fast (0.3 to 1.9 m in a log), the box
        // then running ahead of and behind the car and its contact shadow. Another part only where it stands at the car
        const bool atCar = body || (nearest >= 0.0f && nearest < 0.3f);
        // Five times a second, where the body was drawn from where the car was handed over, along its way and
        // across, against its speed and the lead: off-screen cars are left with the guess by speed, which a log
        // found 3 to 9 m off when driven fast
        if (watched && body && GetTickCount() - traceOffsetLast >= 200)
        {
            traceOffsetLast = GetTickCount();
            const float handed[3] = { around[0] - box.velocity[0] * lead, around[1] - box.velocity[1] * lead, around[2] - box.velocity[2] * lead };
            const float dx = m[12] - handed[0], dy = m[13] - handed[1], dz = m[14] - handed[2];
            const float along = dx * box.forward[0] + dy * box.forward[1], across = dx * box.forward[1] - dy * box.forward[0];
            const float vAlong = box.velocity[0] * box.forward[0] + box.velocity[1] * box.forward[1];
            // and how old the hand over its boxes came from was: hand overs since, and milliseconds
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const float ageMs = lastFound.handedAt.QuadPart ? float(now.QuadPart - lastFound.handedAt.QuadPart) * 1000.0f / float(frequency.QuadPart) : -1.0f;
            Trace("V drawn from handed over: %.2f m along (%.3f s at its speed), %.2f across, %.2f up; speed %.1f m/s (%.1f along), lead %.3f s, frame %.3f s, guess off by %.2f m; boxes from %u hand overs back, %.1f ms ago",
                along, speed > 0.5f ? along / speed : 0.0f, across, dz, speed, vAlong, lead,
                PostFxResources.fVehicleBoxShadowLeadFrames != 0.0f ? lead / PostFxResources.fVehicleBoxShadowLeadFrames : 0.0f, nearest,
                latestHandOver.load(std::memory_order_relaxed) - lastFound.sequence, ageMs);
        }
        float centre[3];
        if (atCar)
            for (int i = 0; i < 3; ++i)
                centre[i] = m[12 + i] + m[i] * box.local[0] + m[4 + i] * box.local[1] + m[8 + i] * box.local[2];
        else
        {
            // turned to the drawn heading about the car's own position, moved on by speed
            const float r[2] = { fy, -fx };
            centre[0] = around[0] + r[0] * box.local[0] + fx * box.local[1];
            centre[1] = around[1] + r[1] * box.local[0] + fy * box.local[1];
            centre[2] = around[2] + box.local[2];
            if (watched)
                ++traceDrawnHeadingOnly;
        }
        if (!std::isfinite(centre[0]) || !std::isfinite(centre[1]) || !std::isfinite(centre[2]))
            return false;
        std::copy_n(centre, 3, box.centre);
        box.forward[0] = fx;
        box.forward[1] = fy;
        return true;
    }

    // VehicleBoxShadowsLive: the cars are drawn where they stand when the render thread draws them, which can be
    // ahead of where they stood when the light list was handed over, and the box shadows lagged the moving cars.
    // The box takes the car's matrix now, while its place in the pool still holds the same car.
    static bool Refresh(Box& box)
    {
        const auto pool = CVehicle::GetCheckedVehiclePool();
        if (!pool || box.slot < 0 || box.slot >= pool->m_nSize || pool->m_aFlags[box.slot] != box.flags ||
            uintptr_t(pool->m_aStorage) + uintptr_t(box.slot) * pool->m_nStorageSize != box.vehicle)
            return false;
        const float* m = CEntity::GetMatrix(box.vehicle);
        if (!m)
            return false;
        float centre[3];
        for (int i = 0; i < 3; ++i)
            centre[i] = m[12 + i] + m[i] * box.local[0] + m[4 + i] * box.local[1] + m[8 + i] * box.local[2];
        const float flat = std::sqrt(m[4] * m[4] + m[5] * m[5]);
        if (!(flat > 0.5f) || !std::isfinite(centre[0]) || !std::isfinite(centre[1]) || !std::isfinite(centre[2]))
            return false;
        std::copy_n(centre, 3, box.centre);
        box.forward[0] = m[4] / flat;
        box.forward[1] = m[5] / flat;
        return true;
    }

    // Lamps are told apart by where they stand, a quarter metre apart.
    static int64_t Cell(float v) { return int64_t(std::floor(v * 4.0f)); }
    static uint64_t LightKey(int64_t x, int64_t y, int64_t z)
    {
        return ((uint64_t(x) & 0x1FFFFF) << 42) | ((uint64_t(y) & 0x1FFFFF) << 21) | (uint64_t(z) & 0x1FFFFF);
    }

    // VehicleBoxShadowsBridge::BoxedCars, render thread, as a lamp's shadow slot is drawn (before its light): the
    // cars the lamp keeps boxes for until they leave (VehicleBoxShadowsUntilLeaving), its map then leaves them
    // out. A lamp that took a slot faded its boxes out for its map's shadows and back in when it lost the slot,
    // and with two lamps over a car the shadows jumped between box and map by turns.
    static int BoxedCars(const float position[3], uintptr_t* cars, int max)
    {
        if (!PostFxResources.VehicleBoxShadowsEnabled() || !PostFxResources.bVehicleBoxShadowsUntilLeaving ||
            PostFxResources.bVehicleBoxShadowsWithSlots || !std::isfinite(position[0]))
            return 0;
        const DWORD now = GetTickCount();
        const int64_t x = Cell(position[0]), y = Cell(position[1]), z = Cell(position[2]);
        // The slot's copy of the position may fall into the next cell
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz)
                {
                    const auto it = held.find(LightKey(x + dx, y + dy, z + dz));
                    if (it == held.end() || !it->second.lastTick || now - it->second.lastTick > 500)
                        continue;
                    auto& state = it->second;
                    int n = 0;
                    for (int i = 0; i < state.count && n < max; ++i)
                        if (state.boxes[i].chosen && state.boxes[i].latch == kBox && state.boxes[i].box.vehicle)
                            cars[n++] = state.boxes[i].box.vehicle;
                    if (n)
                        state.leftOutTick = now;
                    mapsWithoutBoxed.fetch_add(1, std::memory_order_relaxed);
                    carsLeftOut.fetch_add(uint32_t(n), std::memory_order_relaxed);
                    return n;
                }
        return 0;
    }

    // Render thread, for the light about to be drawn: up to three cars its light reaches, the nearest the light first,
    // without the car the light belongs to; none where the light's own map shows the cars.
    // Returns how much of a shadow its boxes give: the strongest box times the lamp's strength, 0 for none.
    // mapHasCars comes back false where the light's map left its boxed cars out (BoxedCars).
    // The boxes handed over with the light list the render thread draws now, into drawn
    static void FindDrawn()
    {
        auto& R = PostFxResources;
        const uintptr_t list = *lightListDrawn;
        // Looked up again once its entry has been reused for a later list too.
        if (list == drawnFor)
            return;
        drawnFor = list;
        PublishPairs();

        // The newest with this list: the ring holds more hand overs than the game has lists, so an older
        // one with the same list, three or six frames old, made the shadows lag the cars by fits
        const List* found = nullptr;
        for (auto& l : lists)
            if (l.lights.load(std::memory_order_acquire) == list && (!found || int32_t(l.sequence - found->sequence) > 0))
                found = &l;
        (found ? framesMatched : framesUnmatched).fetch_add(1, std::memory_order_relaxed);
        if (R.bVehicleBoxShadowsLog)
        {
            TraceFrame();
            if (!found)
                Trace("frame without its own boxes, drawn with the last ones");
        }
        // A frame whose boxes were not found keeps the last ones, a frame late, rather than shading with
        // none: every box shadow blinked out for that frame.
        if (found)
        {
            lastFound.count = found->count;
            std::copy_n(found->boxes, found->count, lastFound.boxes);
            lastFound.pedCount = found->pedCount;
            std::copy_n(&found->peds[0][0], found->pedCount * 3, &lastFound.peds[0][0]);
            lastFound.propCount = found->propCount;
            std::copy_n(found->props, found->propCount, lastFound.props);
            std::copy_n(found->focus, 3, lastFound.focus);
            lastFound.watched = found->watched;
            lastFound.sequence = found->sequence;
            lastFound.handedAt = found->handedAt;
            if (found->frameSeconds > 0.0f)
                lastFound.frameSeconds = found->frameSeconds;
            drawn = &lastFound;
        }
    }

    // How far the point q lies outside the shape (negative inside)
    static float ShapeDistance(const Shape& shape, const float q[3])
    {
        if (shape.t1[3] < 0.0f)
        {
            float d[3], r[3], dd = 0.0f, dr = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                d[k] = shape.t1[k] - shape.t0[k];
                r[k] = q[k] - shape.t0[k];
                dd += d[k] * d[k];
                dr += d[k] * r[k];
            }
            const float t = dd > 1.0e-6f ? std::clamp(dr / dd, 0.0f, 1.0f) : 0.0f;
            float l2 = 0.0f;
            for (int k = 0; k < 3; ++k)
                l2 += (r[k] - d[k] * t) * (r[k] - d[k] * t);
            return std::sqrt(l2) - shape.t0[3];
        }
        const float px = q[0] - shape.t0[0], py = q[1] - shape.t0[1], pz = q[2] - shape.t0[2];
        const float along = std::abs(px * shape.t1[0] + py * shape.t1[1]) - shape.t0[3];
        const float across = std::abs(px * shape.t1[1] - py * shape.t1[0]) - shape.t1[2];
        const float up = std::abs(pz) - shape.t1[3];
        const float ox = (std::max)(along, 0.0f), oy = (std::max)(across, 0.0f), oz = (std::max)(up, 0.0f);
        return std::sqrt(ox * ox + oy * oy + oz * oz) + (std::min)((std::max)({ along, across, up }), 0.0f);
    }

    // The camera's frustum planes, inside where a x + b y + c z + d >= 0 with (a, b, c) of length 1, from the viewport's
    // view and projection (row vectors, D3D clip space): left, right, bottom, top, near; the far one left out. 0 when
    // the viewport makes no sense.
    static int FrustumPlanes(const rage::grcViewport& viewport, float (&planes)[5][4])
    {
        float m[4][4];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += viewport.mViewMatrix[i][k] * viewport.mProjectionMatrix[k][j];
                if (!std::isfinite(sum))
                    return 0;
                m[i][j] = sum;
            }
        // clip = p * m: x within -w..w, y within -w..w, z from 0
        static constexpr struct { int column; float sign; bool withW; } kPlanes[5] = {
            { 0, 1.0f, true }, { 0, -1.0f, true }, { 1, 1.0f, true }, { 1, -1.0f, true }, { 2, 1.0f, false } };
        for (int n = 0; n < 5; ++n)
        {
            float length2 = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                planes[n][i] = kPlanes[n].sign * m[i][kPlanes[n].column] + (kPlanes[n].withW ? m[i][3] : 0.0f);
                if (i < 3)
                    length2 += planes[n][i] * planes[n][i];
            }
            if (!(length2 > 1.0e-12f))
                return 0;
            const float scale = 1.0f / std::sqrt(length2);
            for (auto& v : planes[n])
                v *= scale;
        }
        return 5;
    }

    // Whether the shadow the light at, reaching radius, throws of the sphere c, r may fall where the camera sees: it
    // lies within the hull of that sphere and the one where the light ends, along the way from the light through it
    // and grown with the distance. The hull is out of view only where both spheres lie wholly outside one plane.
    static bool ShadowMayShow(const float (&planes)[5][4], int count, const float at[3], float radius, const float c[3], float r)
    {
        float d[3] = { c[0] - at[0], c[1] - at[1], c[2] - at[2] };
        const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (distance <= r)
            return true;
        const float k = (std::max)(radius / distance, 1.0f);
        const float end[3] = { at[0] + d[0] * k, at[1] + d[1] * k, at[2] + d[2] * k };
        const float endRadius = r * k;
        for (int i = 0; i < count; ++i)
        {
            const float* p = planes[i];
            if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -r &&
                p[0] * end[0] + p[1] * end[1] + p[2] * end[2] + p[3] < -endRadius)
                return false;
        }
        return true;
    }

    // PropShapeShadows: the props nearest the light, weighed by how near the player they stand, as the cars, and by
    // their size (the radius of a sphere round them, 0.3 to 3 m: a container's shadow shows more than a hydrant's),
    // their shapes up to PropShapesPerLight; those it took last frame held at a quarter of that, so the choice does
    // not flicker between two. Left out: props outside its light or its cone, those whose shadow cannot fall where
    // the camera sees, the arm of the lamp's own post, a shape the light stands in.
    static int ShapesForLight(const rage::CLightSource& light, bool hasMap, Shape (&shapes)[kShapesPerLight])
    {
        const auto& R = PostFxResources;
        if (!drawn || (!drawn->propCount && !drawn->bodyCount))
            return 0;
        // Its light far from the camera: none, as the boxes
        if (R.fVehicleBoxShadowsMaxDistance > 0.0f)
            if (auto* vp = rage::GetCurrentViewport())
            {
                const float* eye = vp->mCameraMatrix[3];
                const float ex = light.mPosition.x - eye[0], ey = light.mPosition.y - eye[1], ez = light.mPosition.z - eye[2];
                if (std::sqrt(ex * ex + ey * ey + ez * ez) - light.mRadius > R.fVehicleBoxShadowsMaxDistance)
                    return 0;
            }
        float planes[5][4];
        const auto* viewport = rage::GetCurrentViewport();
        const int planeCount = viewport && R.bPropShapeViewCulling ? FrustumPlanes(*viewport, planes) : 0;
        const uint64_t lightKey = LightKey(Cell(light.mPosition.x), Cell(light.mPosition.y), Cell(light.mPosition.z));
        if (heldProps.size() > 4096)
            heldProps.clear();
        auto& last = heldProps[lightKey];
        const auto key = static_cast<uint32_t>(light.mCastShadows);
        const float at[3] = { light.mPosition.x, light.mPosition.y, light.mPosition.z };
        // a spot's cone: its direction and the cosine of its outer half angle (the game keeps the cosine)
        const bool spot = light.mType == rage::LT_SPOT && light.mOuterConeAngle > 0.0f && light.mOuterConeAngle < 1.0f;
        const float* dir = &light.mDirection.x;
        const float dirLength = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        const float cone = spot ? std::acos(light.mOuterConeAngle) : 0.0f;
        // PedBoneShadows: the player's capsules first, under a light without a map this frame (whose map shows the
        // player) that reaches them, nor one of ours on a facade unless PedCapsuleAddedLights; the props take the room
        // left
        int used = 0;
        if (drawn->bodyCount && !hasMap && (R.bPedCapsuleAddedLights || !VehicleBoxShadowsBridge::AddedLight ||
            !VehicleBoxShadowsBridge::AddedLight(key, light.mFlags)))
        {
            const float* c = drawn->bodyCentre;
            const float dx = c[0] - at[0], dy = c[1] - at[1], dz = c[2] - at[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            bool reached = d - drawn->bodyReach < light.mRadius && dz < -0.3f;
            if (reached && spot && dirLength > 0.5f && d > drawn->bodyReach)
            {
                const float off = std::acos(std::clamp((dx * dir[0] + dy * dir[1] + dz * dir[2]) / (dirLength * d), -1.0f, 1.0f));
                reached = off <= cone + std::asin((std::min)(drawn->bodyReach / d, 1.0f)) + 0.1f;
            }
            const float falloff = std::clamp(1.0f - (std::max)(d - drawn->bodyReach, 0.0f) / (std::max)(light.mRadius, 0.1f), 0.0f, 1.0f);
            if (reached && falloff * falloff >= R.fVehicleBoxShadowsMinLight)
                for (int j = 0; j < drawn->bodyCount; ++j)
                    if (ShapeDistance(drawn->body[j], at) > 0.2f)
                        shapes[used++] = drawn->body[j];
            bodiesWithShadows.fetch_add(used ? 1u : 0u, std::memory_order_relaxed);
        }
        const int most = (std::min)(std::clamp(R.nPropShapesPerLight, 1, kShapesPerLight), kShapesPerLight - used);
        std::pair<float, const Prop*> chosen[kShapesPerLight] = {};
        int taken = 0;
        for (int i = 0; i < (R.bPropShapeShadows ? drawn->propCount : 0); ++i)
        {
            const auto& p = drawn->props[i];
            // In this light's own map already. Its map of this frame shows every object but the posts: the caster loop
            // draws the rest in modes 1 and 2 without the 0x800 bit, and only the posts' shaders (all in bucket 4) gave
            // nothing there; its cache shows those with the bit (mode 0). Each a second, square shadow beside the real.
            if ((hasMap && !p.post) || (light.mShadowCacheIndex >= 0 && p.inMaps))
                continue;
            // A car light's map with the posts in it (PostShadowsFromCarLights)
            if (hasMap && p.post && R.bPostShadowsFromCarLights && (light.mFlags & rage::LF_VEHICLE))
                continue;
            const float dx = p.centre[0] - at[0], dy = p.centre[1] - at[1], dz = p.centre[2] - at[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float gap = (std::max)(d - p.reach, 0.0f);
            if (gap > light.mRadius)
                continue;
            if (spot && dirLength > 0.5f && d > p.reach)
            {
                const float off = std::acos(std::clamp((dx * dir[0] + dy * dir[1] + dz * dir[2]) / (dirLength * d), -1.0f, 1.0f));
                if (off > cone + std::asin((std::min)(p.reach / d, 1.0f)) + 0.1f)
                    continue;
            }
            const float falloff = std::clamp(1.0f - gap / (std::max)(light.mRadius, 0.1f), 0.0f, 1.0f);
            const bool holding = std::find(last.begin(), last.end(), p.entity) != last.end();
            if (falloff * falloff < R.fVehicleBoxShadowsMinLight * (holding ? 0.5f : 1.0f))
                continue;
            if (planeCount && !ShadowMayShow(planes, planeCount, at, light.mRadius, p.centre, p.reach))
                continue;
            const float fx = p.centre[0] - drawn->focus[0], fy = p.centre[1] - drawn->focus[1], fz = p.centre[2] - drawn->focus[2];
            // a quarter square metre more, so props the light stands among still go by their size
            float score = (gap * gap + 0.25f) * (1.0f + (fx * fx + fy * fy + fz * fz) / 400.0f) / std::clamp(p.reach, 0.3f, 3.0f);
            if (holding)
                score *= 0.25f;
            if (taken == most && score >= chosen[most - 1].first)
                continue;
            int place = taken < most ? taken++ : most - 1;
            for (; place > 0 && chosen[place - 1].first > score; --place)
                chosen[place] = chosen[place - 1];
            chosen[place] = { score, &p };
        }
        last = {};
        const int bodyUsed = used;
        for (int i = 0; i < taken && used - bodyUsed < most; ++i)
        {
            const auto& p = *chosen[i].second;
            last[i] = p.entity;
            // The lamp's own post (the light's key is the entity's address and the effect's place in it): its post
            // throws a shadow from the lamp out on the arm, the arm over the lamp none
            const int count = key - static_cast<uint32_t>(p.entity) < 0x320 ? 1 : p.shapeCount;
            for (int j = 0; j < count && used - bodyUsed < most; ++j)
                if (ShapeDistance(p.shapes[j], at) > 0.2f)
                    shapes[used++] = p.shapes[j];
        }
        propsWithShadows.fetch_add(uint32_t(used - bodyUsed), std::memory_order_relaxed);
        return used;
    }

    // The light's shapes into the texture on s7; false where it could not be made or filled
    static bool BindShapes(IDirect3DDevice9* pDevice, const Shape* shapes, int count)
    {
        auto real = RageDirect3DDevice9::RealDevice(pDevice);
        // Not made while the device is being reset: it would be alive at the Reset
        if (!shapesTexture && rage::grcDevice::IsDeviceLost())
            return false;
        if (!shapesTexture && FAILED(real->CreateTexture(kShapeTexels, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &shapesTexture, nullptr)))
        {
            shapesTexture = nullptr;
            return false;
        }
        D3DLOCKED_RECT locked = {};
        if (FAILED(shapesTexture->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD)))
            return false;
        // A32B32G32R32F keeps each texel as r, g, b, a floats
        std::memcpy(locked.pBits, shapes, sizeof(Shape) * size_t(count));
        shapesTexture->UnlockRect(0);
        RageDirect3DDevice9::SetTextureBoth(pDevice, 7, shapesTexture);
        RageDirect3DDevice9::SetSamplerStateBoth(pDevice, 7, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        RageDirect3DDevice9::SetSamplerStateBoth(pDevice, 7, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        RageDirect3DDevice9::SetSamplerStateBoth(pDevice, 7, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        RageDirect3DDevice9::SetSamplerStateBoth(pDevice, 7, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        RageDirect3DDevice9::SetSamplerStateBoth(pDevice, 7, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        shapesBound = true;
        return true;
    }

    // When the device is lost: the texture is in its default pool. Unbound first: lost between a light's BindShapes
    // and Off, it stayed on s7, and a default pool texture still bound keeps the Reset from going through (DD3D80)
    static void ReleaseShapes()
    {
        if (shapesBound)
            if (auto pDevice = rage::grcDevice::GetD3DDevice())
                RageDirect3DDevice9::SetTextureBoth(pDevice, 7, nullptr);
        if (shapesTexture)
        {
            shapesTexture->Release();
            shapesTexture = nullptr;
        }
        shapesBound = false;
    }

    static float SetForLight(IDirect3DDevice9* pDevice, const rage::CLightSource& light, bool& mapHasCars)
    {
        auto& R = PostFxResources;
        const bool hasMap = mapHasCars;
        // The props' shapes near the light, on s7, their count in c115.x
        Shape shapes[kShapesPerLight] = {};
        int propsUsed = 0;
        // Boxes 0-2 at c143-c148 with the lamp's own at c149, boxes 3 and 4 at c121-c124.
        float constants[kPerLight * 2 + 1][4] = {};
        int used = 0;
        float strength = 0.0f, rounding = R.fVehicleBoxShadowRounding;
        float boxStrength[kPerLight] = {};
        float pairBetween[3] = {}, pairSpacing = 0.0f; // the lamp's pair on one post, if any (set below)
        bool paired = false;
        // c150-c153: up to four people near the light, the foot of each and its height (0 for none)
        float peds[kPedsPerLight][4] = {};
        int pedsUsed = 0;
        float pedLightSize = 0.0f;
        if (R.VehicleBoxShadowsEnabled() && lightListDrawn &&
            (R.bVehicleBoxShadowsFromCarLights || !(light.mFlags & rage::LF_VEHICLE)) &&
            (light.mType == rage::LT_POINT || light.mType == rage::LT_SPOT || light.mType == rage::LT_CLAMPED))
        {
            FindDrawn();
            // Which three: the nearest the light, weighed by how near the player they stand, where their shadows
            // are seen; those the light took last frame are held at a quarter of that, so a car passing by the lamp does
            // not push a standing one out, and its shadow from that lamp flickered. Lamps are told apart by where
            // they stand, a quarter metre apart.
            const uint64_t lightKey = LightKey(Cell(light.mPosition.x), Cell(light.mPosition.y), Cell(light.mPosition.z));
            // A lamp of a pair on one post (PairedLampDistance): in the ranking of the lamps on a car both are one
            // lamp, standing between them, so they give their boxes to the same cars and take one of the car's
            // places, not both; and their boxes soften by half their spacing, the two shadows one soft shadow.
            paired = VehicleBoxShadowsBridge::LampPair &&
                VehicleBoxShadowsBridge::LampPair(uint32_t(light.mCastShadows), pairBetween, pairSpacing);
            const uint64_t rankKey = paired ? LightKey(Cell(pairBetween[0]), Cell(pairBetween[1]), Cell(pairBetween[2])) : lightKey;
            if (R.nVehicleBoxShadowsPerCar > 0 && TemporalAA::SceneFrame != carLampsFrame)
            {
                carLampsFrame = TemporalAA::SceneFrame;
                RankCarLamps(R.nVehicleBoxShadowsPerCar);
            }
            if (held.size() > 4096)
                held.clear();
            auto& state = held[lightKey];
            auto& last = state.cars;
            // VehicleBoxShadowsFadeIn / FadeOut: a box comes in and goes out over that many milliseconds, a car
            // taken by the lamp or left, and a lamp just seen; 0 at once. Its strength rides in c143.w.
            const DWORD now = GetTickCount();
            // Its slot's map was drawn without its boxed cars this frame: its boxes are their only shadow.
            const bool leftOut = mapHasCars && state.leftOutTick && now - state.leftOutTick < 200;
            const bool mapped = mapHasCars;
            // VehicleBoxShadowsUntilLeaving: each car's shadow from the lamp is chosen as it comes under it, the
            // map's if the lamp has a slot (or gets one within kDecideMs), else its box, which it keeps until it
            // leaves, the lamp's map leaving it out should the lamp take a slot meanwhile. A lamp's slot coming
            // and going no longer moves its cars' shadows between box and map. Only a lamp losing its slot turns
            // its cars with the map's shadow to boxes, at once.
            const bool until = R.bVehicleBoxShadowsUntilLeaving && !R.bVehicleBoxShadowsWithSlots;
            if (leftOut)
                mapHasCars = false;
            const bool fresh = !state.lastTick || now - state.lastTick > 1000;
            const float dt = fresh ? 0.0f : float(now - state.lastTick);
            state.lastTick = now;
            const auto step = [dt](uint32_t ms) { return ms ? dt / float(ms) : 1.0f; };
            if (fresh)
            {
                state.count = 0;
                state.slotStrength = R.nVehicleBoxShadowFadeInMs ? 0.0f : 1.0f;
            }
            // A lamp whose own map shows the cars (a shadow slot) has the real shadow: its boxes fade out over
            // VehicleBoxShadowsSlotFade after it takes the slot, so the shadow does not jump, and come back over
            // FadeIn when it loses it. VehicleBoxShadowsWithSlots keeps them.
            // When it loses the slot its real shadow is gone at once, so the boxes are back at once too: faded in
            // (VehicleBoxShadowsFadeIn) the car had neither shadow for a moment, a blink each time the lamp's slot
            // went (a trace: every few seconds in a tunnel). A lamp just seen still fades its boxes in.
            const bool lampFades = mapHasCars && !R.bVehicleBoxShadowsWithSlots && !until;
            if (lampFades)
                state.slotStrength = (std::max)(state.slotStrength - step(R.nVehicleBoxShadowSlotFadeMs), 0.0f);
            else if (state.wasMappedForFade)
                state.slotStrength = 1.0f;
            else
                state.slotStrength = (std::min)(state.slotStrength + step(R.nVehicleBoxShadowFadeInMs), 1.0f);
            state.wasMappedForFade = lampFades;
            strength = state.slotStrength;
            // VehicleBoxShadowsMaxDistance: a lamp whose light ends that far from the camera has no boxes, faded out
            // over the last 20 m. Its shader then leaves them all out; one with any box works all five of them. The
            // lamps took some 3.4 GPU ms a frame at night in a profile, the boxes a share of it
            if (R.fVehicleBoxShadowsMaxDistance > 0.0f)
                if (auto* vp = rage::GetCurrentViewport())
                {
                    const float* eye = vp->mCameraMatrix[3];
                    const float ex = light.mPosition.x - eye[0], ey = light.mPosition.y - eye[1], ez = light.mPosition.z - eye[2];
                    const float away = std::sqrt(ex * ex + ey * ey + ez * ez) - light.mRadius;
                    const float fadeFrom = (std::max)(R.fVehicleBoxShadowsMaxDistance - 20.0f, 0.0f);
                    strength *= std::clamp((R.fVehicleBoxShadowsMaxDistance - away) / (std::max)(R.fVehicleBoxShadowsMaxDistance - fadeFrom, 1.0f), 0.0f, 1.0f);
                }
            int taken = 0;
            bool watchedLow = false;
            const int perLamp = std::clamp(R.nVehicleBoxShadowsPerLamp, 1, kPerLight);
            std::pair<float, const Box*> nearest[kPerLight] = {};
            for (int i = 0; drawn && i < drawn->count; ++i)
            {
                const auto& box = drawn->boxes[i];
                const float dx = light.mPosition.x - box.centre[0], dy = light.mPosition.y - box.centre[1], dz = light.mPosition.z - box.centre[2];
                // A car the lamp holds goes only a little further out, so one at the edge of its light is not taken
                // and left frame by frame
                const bool holding = std::find(last.begin(), last.end(), box.vehicle) != last.end();
                const float reach = (light.mRadius + box.halfLength + box.halfWidth + box.halfHeight) * (holding ? 1.15f : 1.0f);
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > reach * reach)
                    continue;
                const float fx = box.centre[0] - drawn->focus[0], fy = box.centre[1] - drawn->focus[1], fz = box.centre[2] - drawn->focus[2];
                float score = d2 * (1.0f + (fx * fx + fy * fy + fz * fz) / 400.0f);
                if (holding)
                    score *= 0.25f;
                if (taken == perLamp && score >= nearest[perLamp - 1].first)
                    continue;
                // A lamp no higher than the car's roof: real lamps stand above cars, but some in tunnels light from
                // the road or walls with no lamp there, and a box's shadow from them spread over the whole road.
                // A car the lamp holds is left only 0.2 m under that, one it does not is taken only 0.2 m over it: on
                // a slope a lamp at the line was left and taken by turns, and its shadow went and came.
                if (!(light.mFlags & rage::LF_VEHICLE) && dz < box.halfHeight + (holding ? 0.1f : 0.5f))
                {
                    NoteLowLight(light, box, dz);
                    if (box.vehicle == drawn->watched)
                        watchedLow = true;
                    continue;
                }
                // A light within its own car (headlights, tail lights) is not shadowed by it.
                const float along = dx * box.forward[0] + dy * box.forward[1], across = dx * box.forward[1] - dy * box.forward[0];
                if (std::abs(along) < box.halfLength + 0.5f && std::abs(across) < box.halfWidth + 0.5f && std::abs(dz) < box.halfHeight + 0.5f)
                    continue;
                // VehicleBoxShadowsMinLight: a car at the edge of a lamp's light (33 m from one of 40 in a log) took a
                // box of a few hundredths and left it again as it drove; one it holds goes at half that
                const float falloff = std::clamp(1.0f - std::sqrt(d2) / (std::max)(light.mRadius, 0.1f), 0.0f, 1.0f);
                if (falloff * falloff < R.fVehicleBoxShadowsMinLight * (holding ? 0.5f : 1.0f))
                    continue;
                // VehicleBoxShadowsPerCar: only the strongest lamps on the car give it a box
                if (R.nVehicleBoxShadowsPerCar > 0)
                {
                    NoteCarLamp(box.vehicle, rankKey, (std::max)(light.mIntensity, 0.0f) * falloff * falloff);
                    const auto car = carLamps.find(box.vehicle);
                    if (car != carLamps.end() && car->second.ranked && !LampAllowed(car->second, rankKey))
                        continue;
                }
                int at = taken < perLamp ? taken++ : perLamp - 1;
                for (; at > 0 && nearest[at - 1].first > score; --at)
                    nearest[at] = nearest[at - 1];
                nearest[at] = { score, &box };
            }
            last = {};
            for (int i = 0; i < taken; ++i)
                last[i] = nearest[i].second->vehicle;
            // The cars taken now come in, up to full strength, with their boxes as handed over now; those left fade
            // out where they stood last, while there is room among the three.
            std::array<Fading, kPerLight> next{};
            int nextCount = 0;
            for (int i = 0; i < taken; ++i)
            {
                auto& f = next[nextCount++];
                f.box = *nearest[i].second;
                f.chosen = true;
                const Fading* before = nullptr;
                for (int j = 0; j < state.count; ++j)
                    if (state.boxes[j].box.vehicle == f.box.vehicle)
                    {
                        before = &state.boxes[j];
                        std::copy_n(before->shape, 3, f.shape);
                    }
                const float had = before ? before->strength : 0.0f;
                if (!until)
                {
                    f.strength = (std::min)(had + step(R.nVehicleBoxShadowFadeInMs), 1.0f);
                    continue;
                }
                f.latch = before ? before->latch : mapped ? kReal : kPending;
                f.takenTick = before ? before->takenTick : now;
                if (f.latch == kPending)
                    f.latch = mapped ? kReal : now - f.takenTick >= kDecideMs ? kBox : kPending;
                if (f.latch == kReal && !mapped)
                {
                    // The lamp lost its slot and with it the car's shadow: the box at once, and kept
                    f.latch = kBox;
                    f.strength = 1.0f;
                }
                else if (f.latch == kReal)
                    f.strength = (std::max)(had - step(R.nVehicleBoxShadowSlotFadeMs), 0.0f);
                else
                    f.strength = (std::min)(had + step(R.nVehicleBoxShadowFadeInMs), 1.0f);
            }
            for (int j = 0; j < state.count && nextCount < perLamp; ++j)
            {
                const auto& old = state.boxes[j];
                if (std::find(last.begin(), last.end(), old.box.vehicle) != last.end())
                    continue;
                // Under a lamp whose map leaves its boxed cars out, a car it leaves keeps its box this frame, as
                // this frame's map left it out, and is in the map from the next on (BoxedCars takes only the
                // chosen); faded out, it would have had both shadows at once after a frame without either.
                if (until && mapped && old.latch == kBox)
                {
                    if (old.chosen)
                    {
                        next[nextCount] = old;
                        next[nextCount++].chosen = false;
                    }
                    continue;
                }
                const float left = old.strength - step(R.nVehicleBoxShadowFadeOutMs);
                if (left > 0.0f)
                {
                    next[nextCount] = old;
                    next[nextCount].strength = left;
                    next[nextCount++].chosen = false;
                }
            }
            state.boxes = next;
            state.count = nextCount;
            if (until)
                for (int i = 0; i < nextCount; ++i)
                    if (next[i].chosen && next[i].latch == kBox && pairsBuildingCount < kPairs)
                        pairsBuilding[pairsBuildingCount++] = { { light.mPosition.x, light.mPosition.y, light.mPosition.z }, next[i].box.vehicle };
            used = strength > 0.0f ? nextCount : 0;
            // PedCapsuleShadows: the people nearest the light, weighed by how near the player they stand, as the cars;
            // not under a light whose map is drawn this frame, which shows them
            // PedCapsuleAddedLights 0: nor under a light whose shadow is ours, not the map's, which hardly ever takes a
            // slot: a facade's light floating in the air over the pavement left a blob under you with no lamp to be seen
            const bool pedLight = R.bPedCapsuleAddedLights || !VehicleBoxShadowsBridge::AddedLight ||
                !VehicleBoxShadowsBridge::AddedLight(uint32_t(light.mCastShadows), light.mFlags);
            if (R.bPedCapsuleShadows && strength > 0.0f && drawn && !mapped && pedLight)
            {
                std::pair<float, const float*> chosen[kPedsPerLight] = {};
                for (int i = 0; i < drawn->pedCount; ++i)
                {
                    const float* p = drawn->peds[i];
                    const float foot = p[2] - R.fPedCapsuleFootBelow;
                    const float dx = light.mPosition.x - p[0], dy = light.mPosition.y - p[1], dz = light.mPosition.z - foot;
                    const float d2 = dx * dx + dy * dy + dz * dz, reach = light.mRadius + R.fPedCapsuleHeight;
                    // out of its light, under the head (a light at their height would throw the shadow over the whole
                    // road), or the light on the ped itself
                    if (d2 > reach * reach || dz < R.fPedCapsuleHeight + 0.3f || dx * dx + dy * dy < 0.25f)
                        continue;
                    const float falloff = std::clamp(1.0f - std::sqrt(d2) / (std::max)(light.mRadius, 0.1f), 0.0f, 1.0f);
                    if (falloff * falloff < R.fVehicleBoxShadowsMinLight)
                        continue;
                    const float fx = p[0] - drawn->focus[0], fy = p[1] - drawn->focus[1], fz = p[2] - drawn->focus[2];
                    const float score = d2 * (1.0f + (fx * fx + fy * fy + fz * fz) / 400.0f);
                    if (pedsUsed == kPedsPerLight && score >= chosen[kPedsPerLight - 1].first)
                        continue;
                    int at = pedsUsed < kPedsPerLight ? pedsUsed++ : kPedsPerLight - 1;
                    for (; at > 0 && chosen[at - 1].first > score; --at)
                        chosen[at] = chosen[at - 1];
                    chosen[at] = { score, p };
                }
                for (int i = 0; i < pedsUsed; ++i)
                {
                    const float* p = chosen[i].second;
                    peds[i][0] = p[0]; peds[i][1] = p[1]; peds[i][2] = p[2] - R.fPedCapsuleFootBelow; peds[i][3] = R.fPedCapsuleHeight;
                }
                // PedCapsuleSoftening: the higher the light over the nearest one's foot, the larger it is taken, as the
                // shader's penumbra grows with the part of the way to the light the capsule stands at, small under a
                // high light
                if (pedsUsed)
                    pedLightSize = R.fPedCapsuleLightSize + R.fPedCapsuleSoftening * (std::max)(light.mPosition.z - peds[0][2] - 2.0f, 0.0f);
                pedsWithShadows.fetch_add(uint32_t(pedsUsed), std::memory_order_relaxed);
            }
            if (R.bVehicleBoxShadowsLog && drawn && drawn->watched)
                TraceLamp(light, state, mapped, leftOut, watchedLow, taken, nearest);
            // The model's bounds take in mirrors, aerials and bumpers, and the roof's height runs over the bonnet and
            // boot too: the box is that much smaller, its bottom kept where it is.
            const auto& scale = R.fVehicleBoxShadowScale;
            for (int i = 0; i < used; ++i)
            {
                Box box = state.boxes[i].box;
                if (R.bVehicleBoxShadowsLive)
                {
                    if (!Refresh(box) && drawn && box.vehicle == drawn->watched)
                        ++traceStaleWatched;
                }
                else if (drawn && FromDrawn(box, R.fVehicleBoxShadowLeadFrames * (std::min)(drawn->frameSeconds, 0.1f), box.vehicle == drawn->watched))
                {
                    if (box.vehicle == drawn->watched)
                        ++traceDrawnWatched;
                }
                else if (drawn)
                {
                    // Without object motion vectors: the lights are drawn a frame after the cars stood where the
                    // boxes were taken, ahead by the car's own speed, so the shadow neither lags nor jerks as a
                    // read of the moving car did.
                    const float lead = R.fVehicleBoxShadowLeadFrames * (std::min)(drawn->frameSeconds, 0.1f);
                    for (int k = 0; k < 3; ++k)
                        box.centre[k] += box.velocity[k] * lead;
                    // and turned on by the way it turns: moved only, the box of a car turning or spun kept the
                    // heading it had when handed over, and its shadow did not turn with the car
                    const float turn = box.turning * lead, c = std::cos(turn), s = std::sin(turn);
                    const float fx = box.forward[0] * c - box.forward[1] * s, fy = box.forward[0] * s + box.forward[1] * c;
                    box.forward[0] = fx;
                    box.forward[1] = fy;
                    if (box.vehicle == drawn->watched)
                        ++traceLeadWatched;
                }
                boxStrength[i] = state.boxes[i].strength;
                // The lower the light, the longer the shadow, and the plainer its box's square shape. The box's top
                // comes down to VehicleBoxShadowsLowLightHeight of its height, which shortens the shadow while its
                // foot keeps the car's size, where the contact shadow is; across the way the light comes it may also
                // narrow to VehicleBoxShadowsLowLightNarrow (the length for a light to the side, the width for one
                // ahead or behind). A light overhead keeps it whole.
                float along = 1.0f, across = 1.0f, lower = 1.0f;
                {
                    const float dx = light.mPosition.x - box.centre[0], dy = light.mPosition.y - box.centre[1], dz = light.mPosition.z - box.centre[2];
                    const float flat = std::sqrt(dx * dx + dy * dy), d = std::sqrt(flat * flat + dz * dz);
                    if (flat > 0.01f && d > 0.01f)
                    {
                        const float t = std::clamp((dz / d - 0.3f) / 0.6f, 0.0f, 1.0f);
                        // 0 overhead, 1 at 17 degrees and below; a shadow that does not follow the light stays short anyway
                        const float low = (1.0f - t * t * (3.0f - 2.0f * t)) * R.fVehicleBoxShadowFollowLight;
                        const float narrow = 1.0f - (1.0f - R.fVehicleBoxShadowLowLightNarrow) * low;
                        const float a = std::abs(dx * box.forward[0] + dy * box.forward[1]) / flat; // 1: ahead or behind
                        along = 1.0f + (narrow - 1.0f) * (1.0f - a);
                        across = 1.0f + (narrow - 1.0f) * a;
                        lower = 1.0f - (1.0f - R.fVehicleBoxShadowLowLightHeight) * low;
                    }
                }
                // Eased over VehicleBoxShadowsSizeFade, so the shadow's length does not jump with the angle
                {
                    auto& shape = state.boxes[i].shape;
                    const float target[3] = { along, across, lower };
                    const float k = R.nVehicleBoxShadowSizeFadeMs ? (std::min)(dt / float(R.nVehicleBoxShadowSizeFadeMs), 1.0f) : 1.0f;
                    for (int j = 0; j < 3; ++j)
                        shape[j] = shape[j] < 0.0f ? target[j] : shape[j] + (target[j] - shape[j]) * k;
                    along = shape[0];
                    across = shape[1];
                    lower = shape[2];
                }
                const float halfHeight = box.halfHeight * scale[2] * lower;
                float* c = constants[i * 2];
                c[0] = box.centre[0]; c[1] = box.centre[1]; c[2] = box.centre[2] - (box.halfHeight - halfHeight);
                c[3] = box.halfLength * scale[0] * along;
                c[4] = box.forward[0]; c[5] = box.forward[1];
                c[6] = box.halfWidth * scale[1] * across;
                c[7] = halfHeight;
            }
            // One rounding for the three boxes, no more than the smallest half size takes: beyond it a box grew
            // round instead of rounder, a low car's top above its roof.
            for (int i = 0; i < used; ++i)
            {
                const float* c = constants[i * 2];
                rounding = (std::min)(rounding, (std::min)({ c[3], c[6], c[7] }) - 0.05f);
            }
            rounding = (std::max)(rounding, 0.0f);
            // That limit changed at once as a small car came to the lamp or left it, and every shadow under the lamp
            // changed its shape with it; it eases to it over VehicleBoxShadowsSizeFade instead, the smallest box a
            // little large meanwhile.
            if (used)
            {
                if (fresh || state.rounding < 0.0f || !R.nVehicleBoxShadowSizeFadeMs)
                    state.rounding = rounding;
                else
                    state.rounding += (rounding - state.rounding) * (std::min)(dt / float(R.nVehicleBoxShadowSizeFadeMs), 1.0f);
                rounding = state.rounding;
            }
            for (int i = 0; i < used; ++i)
            {
                float* c = constants[i * 2];
                c[3] = (std::max)(c[3] - rounding, 0.05f);
                c[6] = (std::max)(c[6] - rounding, 0.05f);
                c[7] = (std::max)(c[7] - rounding, 0.05f);
                // The strength in fifteenths, sixteen apart above the half length (which stays under 16)
                c[3] = (std::min)(c[3], 15.9f) + 16.0f * std::round(std::clamp(boxStrength[i], 0.0f, 1.0f) * 15.0f);
            }
        }
        // Props for car lights too: a post's shadow from the headlights is the one most often seen; the player's bones
        // (PedBoneShadows) go with them
        if (R.VehicleBoxShadowsEnabled() && (R.bPropShapeShadows || (R.bPedCapsuleShadows && R.bPedBoneShadows)) && lightListDrawn &&
            (light.mType == rage::LT_POINT || light.mType == rage::LT_SPOT || light.mType == rage::LT_CLAMPED))
        {
            FindDrawn();
            propsUsed = ShapesForLight(light, hasMap, shapes);
            if (propsUsed && !BindShapes(pDevice, shapes, propsUsed))
                propsUsed = 0;
        }
        if (!used && !pedsUsed && !propsUsed)
        {
            if (constantsOn)
                Off(pDevice);
            return 0.0f;
        }
        // The shader works the shapes only where c149.w is above 0, and the props take none of it: a light whose boxes
        // have faded out or never came in still lets them through
        if (!used && !pedsUsed)
            strength = 0.001f;
        lightsWithBoxes.fetch_add(1, std::memory_order_relaxed);
        // The boxes left over stand far away, small.
        for (int i = used; i < kPerLight; ++i)
        {
            float* c = constants[i * 2];
            c[0] = c[1] = c[2] = 1.0e6f;
            c[3] = c[6] = c[7] = 0.05f;
            c[4] = 1.0f;
        }
        auto& lamp = constants[kPerLight * 2];
        lamp[0] = R.fVehicleBoxShadowLightSize + (paired ? pairSpacing * 0.5f : 0.0f);
        // The ends' rounding in 5 cm steps, two apart above the edges' (which stays under 1)
        lamp[1] = (std::min)(rounding, 0.99f) + 2.0f * std::round(std::clamp(R.fVehicleBoxShadowEndRounding, 0.0f, 2.0f) / 0.05f);
        lamp[2] = R.fVehicleBoxShadowFollowLight;
        lamp[3] = (std::min)(strength, 1.0f);
        pDevice->SetPixelShaderConstantF(143, constants[0], 6);
        pDevice->SetPixelShaderConstantF(149, lamp, 1);
        pDevice->SetPixelShaderConstantF(121, constants[6], 4);
        const float capsule[4] = { R.fPedCapsuleRadius, -(R.fPedCapsuleRadius + 0.3f), pedLightSize, R.fPedCapsuleBaseSoftness };
        pDevice->SetPixelShaderConstantF(150, peds[0], kPedsPerLight);
        pDevice->SetPixelShaderConstantF(154, capsule, 1);
        const float shapeCount[4] = { float(propsUsed), 0.0f, 0.0f, 0.0f };
        pDevice->SetPixelShaderConstantF(115, shapeCount, 1);
        constantsOn = true;
        float strongest = 0.0f;
        for (int i = 0; i < used; ++i)
            strongest = (std::max)(strongest, boxStrength[i]);
        return strongest * lamp[3];
    }

    // For the log: lamps whose boxes were left out as they stand no higher than the roof, and the last of them.
    static inline std::atomic<uint32_t> lowLights{0};
    static inline rage::CLightSource lastLowLight{};
    static inline float lastLowHeight = 0.0f;
    static void NoteLowLight(const rage::CLightSource& light, const Box& box, float aboveMiddle)
    {
        lowLights.fetch_add(1, std::memory_order_relaxed);
        lastLowLight = light;
        lastLowHeight = aboveMiddle + box.halfHeight; // above the box's bottom
    }

    // Before and after the main view's lights: lights drawn for other views (reflections, mirrors) take none.
    static void Off(IDirect3DDevice9* pDevice)
    {
        const float off[4] = {};
        pDevice->SetPixelShaderConstantF(149, off, 1);
        pDevice->SetPixelShaderConstantF(115, off, 1);
        if (shapesBound)
        {
            RageDirect3DDevice9::SetTextureBoth(pDevice, 7, nullptr);
            shapesBound = false;
        }
        constantsOn = false;
    }
};
