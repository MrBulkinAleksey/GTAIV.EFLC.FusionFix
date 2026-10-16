module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module entityclass;

import common;
import comvars;
import inireload;
import renderskip;
import temporal;

// What kind of entity each draw of the scene belongs to: the map's own geometry, which never moves;
// a prop (an object), which can be pushed, knocked over or broken; a vehicle or a ped. Props use the
// same world shaders and materials as the ground, so the shaders cannot tell them apart.
//
// The game thread marks the entity's kind in a spare byte of each draw command as it is made:
// CDrawEntityDC +0x37 (made only by CEntity's draw list add, 0xa2fe70), CDrawFragDC and
// CDrawFragTypeDC +0x5f (made by it and by CDynamicEntity's, 0xb10360, which vehicles and objects
// with fragments go through). On the render thread the execute (vtable slot 1) of those, of
// CDrawSkinnedEntityDC (always a prop) and of the peds' own commands (CDrawPedDC, CDrawPlayerDC,
// CDrawPedPropsDC, CDrawPedPropDC, CDrawSLODPedDC) calls the hooks added here before and after the
// draw, in every render phase.
//
// EntityDrawLog = 1 ([DEBUG]): every 600 scene frames, the draws of each kind and the CPU time their
// execute took, per frame and per target the game had locked (RenderSkip::CurrentTarget), to
// GTAIV.EFLC.FusionFix.EntityClass.log: what leaving a kind out of a pass would save. For the G-buffer and the water's
// reflection also by the distance from the viewport's camera and by the size on screen (the sphere's reach over the
// distance), for the lamps' maps the same from the lamp of the half drawn, and for the G-buffer and the lamps' maps the
// costliest models, by the hash of their name (model info +0x3c), with their mean distance.
//
// SkipLeftOutDraws = 1 ([PERFORMANCE]): RenderSkip leaves passes out on the GPU (the reflections and far cascades
// taking turns, the night's cascades) by dropping their device draws, but the game still works through every draw
// command of them on the render thread. In a pass it drops, the commands hooked here are not executed at all.
//
// LampShadowFaceCulling ([PERFORMANCE]): the lamps' shadow maps (SHADOW_MAP_COLOUR_POINT) are dual paraboloid: one
// tile a lamp, the half facing one way written to red and the other to green (CSetColorWrite 1 and 2), and every
// entity of the lamp is drawn into both. The slot render (0xd77a00, mode 4) makes a face command (0xd77930, face at
// +0x10) before each half; as it is made, the slot's position (+0xc0) and the halves' axis (the third row of its
// matrix at +0x40, world +x) go into a ring here, its index into the command's spare bytes +0x16/+0x17. On the render
// thread that command's execute says which half and lamp the next draws are for, until CSetColorWrite 0xf. An entity,
// fragment (vehicles, objects with fragments) or fragment type draw whose bounding sphere lies wholly on the other side of the plane through the lamp across the
// axis is left out. The first half (red, face 0) faces -x and the second +x: the other way round dropped props'
// shadows in game. The sphere is the model's (model info +0x10 centre, +0x1c radius) around the command's position,
// grown by the centre's offset so the entity's turn needs no matrix. A fragment command (CDrawFragDC: vehicles,
// objects with fragments) keeps the entity's position as the list was made, as the slot's lamp is taken, so a moving
// car and its own headlights match; its skeleton may leave the rest pose, so its sphere grows by a quarter and a
// metre. With it 0, EntityDrawLog counts what it would leave out.
//
// LampShadowRangeCulling ([PERFORMANCE]): the lamp's render list reaches far past its light (props 40+ m from the lamp
// took half the pass's time), but the map's depth is the distance over the slot's radius (+0xe0), and anything wholly
// further from the lamp than that is further than every point it lights, so it cannot shadow them. An entity,
// fragment or fragment type draw whose sphere (as above) lies wholly outside the radius is left out of the lamp's map. With it 0,
// EntityDrawLog counts what it would leave out.
//
// LampShadowSmallCull ([PERFORMANCE]): a prop whose sphere's reach is under 2% of its distance from the lamp (a 0.4 m
// can 20 m off) covers a few texels of the lamp's map, a shadow too faint to see under the filtering, but costs a draw
// like any other. Such props are left out of the lamp's map; buildings never are. The same ones each time for a lamp,
// so nothing flickers. With it 0, EntityDrawLog counts what it would leave out.
//
// LampShadowConeCulling ([PERFORMANCE]): a spot lamp lights nothing outside its outer cone, so an entity, fragment
// or fragment type draw whose sphere lies wholly outside it, by a 5 degree margin, is left out of its map. The slot keeps only the
// light's position, direction, radius and key (the selection, 0x9280fd, copies them from the light list): there the
// light's type (+0x44), outer cone (+0x5c, a cosine: the game draws a spot whose cosine is under 0 as clamped, 0xac1437)
// and key (+0x60) are kept by slot, and the face command takes them while the slot still holds that key. Moving a
// headlight's map back along its beam only narrows what it sees of the cone. With it 0, EntityDrawLog counts what it
// would leave out.
export namespace EntityClass
{
    enum class Kind : uint8_t
    {
        Unknown,
        Building,   // CBuilding, CAnimatedBuilding, CInteriorInst, CPortalInst
        Prop,       // CObject, CDummyObject, CCutsceneObject and the rest, skinned entities
        Vehicle,    // CVehicle and what derives from it
        Ped,        // CPed and what derives from it
        Kinds
    };

    using DrawHook = void (*)(Kind kind);

    // Hooks are added from module constructors or init events, before the first frame: the list is
    // read on the render thread without a lock.
    void AddDrawHooks(DrawHook before, DrawHook after);

    // Whether the game's code was found and hooked.
    bool Installed();

    // By the classes in the entity's vtable RTTI (its own and its bases).
    Kind OfEntity(void* entity);

    // Inside a draw hook: the model index and position the draw's command keeps (entity, fragment and fragment
    // type commands); false for the others and outside the hooks.
    bool CurrentModel(uint16_t& model, const float*& place);

    // A model index's model info (its name's hash at +0x3c, bounding sphere centre +0x10 and radius +0x1c), 0 if none.
    uintptr_t ModelInfo(uint16_t model);
}

namespace
{
    using EntityClass::Kind;
    constexpr ptrdiff_t kDrawEntityKindByte = 0x37;
    constexpr ptrdiff_t kDrawFragKindByte = 0x5f;     // CDrawFragDC and CDrawFragTypeDC, 0x60 bytes
    using DrawExecute = void(__thiscall*)(void*);
    DrawExecute drawEntityExecute = nullptr;
    DrawExecute drawFragExecute = nullptr;
    DrawExecute drawFragTypeExecute = nullptr;
    DrawExecute drawSkinnedExecute = nullptr;
    // CDrawPedDC, CDrawPlayerDC, CDrawPedPropsDC, CDrawPedPropDC, CDrawSLODPedDC
    constexpr size_t kPedCommands = 5;
    DrawExecute drawPedExecute[kPedCommands] = {};
    bool bInstalled = false;
    // The command being executed, while the hooks run (render thread)
    void* currentDc = nullptr;

    std::vector<std::pair<EntityClass::DrawHook, EntityClass::DrawHook>>& Hooks()
    {
        static std::vector<std::pair<EntityClass::DrawHook, EntityClass::DrawHook>> hooks;
        return hooks;
    }

    // Which draw command a draw came from
    enum Source : uint8_t { FromEntity, FromFrag, FromFragType, FromSkinned, FromPed, Sources };
    Source currentSource = FromEntity;  // with currentDc
    constexpr const char* SourceNames[Sources] = { "entity", "fragment", "fragment type", "skinned", "ped" };
    constexpr const char* KindNames[size_t(Kind::Kinds)] = { "unmarked", "buildings", "props", "vehicles", "peds" };

    // EntityDrawLog and LampShadowFaceCulling: render thread only, but for the switches
    bool bDrawLog = false;
    bool bFaceCullingSetting = true;   // LampShadowFaceCulling
    bool bFaceCulling = false;          // and its hooks in place
    bool bRangeCullingSetting = true;  // LampShadowRangeCulling
    bool bRangeCulling = false;         // and the face hooks in place
    bool bSmallCullSetting = true;     // LampShadowSmallCull
    bool bSmallCulling = false;         // and the face hooks in place
    constexpr float kLampSmallSize = 0.02f;
    bool bConeCullSetting = true;      // LampShadowConeCulling
    bool bConeCulling = false;          // and its hooks in place
    bool bConeHook = false;
    constexpr float kConeMargin = 5.0f * 3.14159265f / 180.0f;
    bool bSkipLeftOut = true;           // SkipLeftOutDraws
    uintptr_t modelInfos = 0;       // CE 0x1295cd8, model infos by index
    constexpr uint32_t kDrawLogFrames = 600;
    struct TargetStats
    {
        std::string name;
        uint64_t draws[size_t(Kind::Kinds)]{};
        int64_t ticks[size_t(Kind::Kinds)]{};
        uint64_t sourceDraws[Sources]{};
        // Locks of the target that had draws, and device viewports within them (the faces of a lamp's cube map);
        // per lock the entities drawn into it, by model and place (entity, fragment and fragment type commands), to tell how
        // often one entity is drawn into one lamp's map
        uint64_t locks = 0, faces = 0;
        uint64_t keyedDraws = 0, entities = 0;
        uint32_t keysLock = ~0u;
        std::unordered_set<uint64_t> keys;
        // Draws outside the face drawn: left out with LampShadowFaceCulling, else counted only
        uint64_t outside = 0;
        // Draws in a pass RenderSkip left out: not executed with SkipLeftOutDraws, else timed as the rest
        uint64_t leftOutPass = 0;
        // Lamp maps: draws with the half known, and where their spheres are against the plane between the halves: on
        // the side the half drawn faces, on the other, across
        uint64_t faceKnown = 0, faceUnknown = 0, ahead = 0, behind = 0, across = 0;
        // Lamp maps: draws wholly outside the lamp's radius (left out with LampShadowRangeCulling, else counted only),
        // and the radius summed over the draws with the half known, drawn or left out
        uint64_t beyond = 0, radiusDraws = 0;
        // Lamp maps: draws and time by the lamp's radius (under 15, 15-30, 30-50, 50+ m), and within each by the angle
        // of the draw off the slot's direction (+0xd0): under 45, 45-90, 90-135, 135+ degrees, no direction
        uint64_t radiusAngleDraws[4][5]{};
        int64_t radiusAngleTicks[4][5]{};
        // Lamp maps: props too small from the lamp (left out with LampShadowSmallCull, else counted only)
        uint64_t tiny = 0;
        // Lamp maps: draws outside a spot lamp's cone (left out with LampShadowConeCulling, else counted only), and
        // the draws whose lamp was a spot, with its cone known
        uint64_t outsideCone = 0, spotDraws = 0;
        double spotConeSum = 0.0;
        double radiusSum = 0.0;
        // The G-buffer and the water's reflection: draws and time by distance and by size on screen (keyed draws);
        // the lamps' maps: the same from the lamp
        bool camera = false, lamp = false;
        uint64_t distanceDraws[6]{}, sizeDraws[6]{};
        int64_t distanceTicks[6]{}, sizeTicks[6]{};
        // The G-buffer: by model
        bool models = false;
        struct ModelStats { uint64_t draws = 0, placed = 0; int64_t ticks = 0; double distance = 0.0; Kind kind = Kind::Unknown; };
        std::unordered_map<uint16_t, ModelStats> byModel;
        int64_t Total() const
        {
            int64_t total = 0;
            for (auto t : ticks)
                total += t;
            return total;
        }
    };
    std::vector<TargetStats> targetStats;
    const char* lastTargetName = nullptr;
    size_t lastTarget = 0;
    uint32_t statsFrom = 0;
    uint32_t lastLock = ~0u;
    uint32_t lastViewport = ~0u;
    int64_t qpcFrequency = 0;

    TargetStats& StatsFor(const char* name)
    {
        if (name == lastTargetName && lastTarget < targetStats.size())
            return targetStats[lastTarget];
        const char* shown = !name ? "unknown (no lock hooks)" : !*name ? "no target locked" : name;
        auto it = std::find_if(targetStats.begin(), targetStats.end(), [&](auto& s) { return s.name == shown; });
        if (it == targetStats.end())
        {
            targetStats.push_back({ shown });
            it = targetStats.end() - 1;
            it->camera = !std::strcmp(shown, "_DEFERRED_GBUFFER_0_") || !std::strcmp(shown, "WATER_REFLECTION_COLOUR");
            it->lamp = !std::strcmp(shown, "SHADOW_MAP_COLOUR_POINT");
            it->camera |= it->lamp;
            it->models = !std::strcmp(shown, "_DEFERRED_GBUFFER_0_") || it->lamp;
        }
        lastTargetName = name;
        lastTarget = size_t(it - targetStats.begin());
        return *it;
    }

    void WriteDrawLog(uint32_t frames)
    {
        std::sort(targetStats.begin(), targetStats.end(), [](auto& a, auto& b) { return a.Total() > b.Total(); });
        const double perFrame = 1.0 / frames;
        const double msPerFrame = 1000.0 / double(qpcFrequency) / frames;
        std::string text = std::format("over {} scene frames, per frame, the draws and the CPU time of their execute:\n", frames);
        for (auto& s : targetStats)
        {
            text += std::format("  {:<28} {:6.3f} ms:", s.name, s.Total() * msPerFrame);
            std::string lampLines;
            for (size_t k = 1; k <= size_t(Kind::Kinds); ++k)
            {
                const size_t kind = k % size_t(Kind::Kinds);    // unmarked last, and only if any
                if (kind || s.draws[kind])
                    text += std::format(" {} {:.1f} ({:.3f} ms),", KindNames[kind], s.draws[kind] * perFrame, s.ticks[kind] * msPerFrame);
            }
            if (s.keyedDraws)
                text += std::format(" locks {:.1f}, faces per lock {:.2f}, draws per entity in a lock {:.2f},", s.locks * perFrame,
                    s.locks ? double(s.faces) / double(s.locks) : 0.0, double(s.keyedDraws) / double(s.entities));
            if (s.faceKnown + s.faceUnknown)
                text += std::format(" half known for {:.0f}%, spheres on the half's side {:.1f}, the other side {:.1f}, across {:.1f},",
                    100.0 * s.faceKnown / double(s.faceKnown + s.faceUnknown), s.ahead * perFrame, s.behind * perFrame, s.across * perFrame);
            if (s.leftOutPass)
                text += std::format(" in a pass left out {:.1f} ({}),", s.leftOutPass * perFrame, bSkipLeftOut ? "not executed" : "executed");
            if (s.outside)
                text += std::format(" outside their face {:.1f} ({}),", s.outside * perFrame, bFaceCulling ? "left out" : "drawn");
            if (s.radiusDraws || s.beyond)
                text += std::format(" beyond the lamp's radius {:.1f} ({}), mean lamp radius {:.1f} m,", s.beyond * perFrame,
                    bRangeCulling ? "left out" : "drawn", s.radiusDraws ? s.radiusSum / double(s.radiusDraws) : 0.0);
            if (s.tiny)
                text += std::format(" props under {:.0f}% of their distance {:.1f} ({}),", kLampSmallSize * 100.0f, s.tiny * perFrame,
                    bSmallCulling ? "left out" : "drawn");
            if (s.outsideCone || s.spotDraws)
                text += std::format(" outside the spot's cone {:.1f} ({}), drawn for spots {:.1f} with a mean cone of {:.0f} deg,", s.outsideCone * perFrame,
                    bConeCulling ? "left out" : "drawn", s.spotDraws * perFrame, s.spotDraws ? s.spotConeSum / double(s.spotDraws) : 0.0);
            if (s.lamp)
            {
                static constexpr const char* radiusNames[4] = { "under 15 m", "15-30 m", "30-50 m", "50+ m" };
                for (int r = 0; r < 4; ++r)
                {
                    uint64_t draws = 0;
                    int64_t ticks = 0;
                    for (int a = 0; a < 5; ++a)
                        draws += s.radiusAngleDraws[r][a], ticks += s.radiusAngleTicks[r][a];
                    if (!draws)
                        continue;
                    lampLines += std::format("    lamps of radius {}: {:.1f} ({:.3f} ms); off their direction under 45 deg {:.1f} ({:.3f} ms), 45-90 {:.1f} ({:.3f} ms), "
                        "90-135 {:.1f} ({:.3f} ms), 135+ {:.1f} ({:.3f} ms), no direction {:.1f} ({:.3f} ms)\n", radiusNames[r], draws * perFrame, ticks * msPerFrame,
                        s.radiusAngleDraws[r][0] * perFrame, s.radiusAngleTicks[r][0] * msPerFrame, s.radiusAngleDraws[r][1] * perFrame,
                        s.radiusAngleTicks[r][1] * msPerFrame, s.radiusAngleDraws[r][2] * perFrame, s.radiusAngleTicks[r][2] * msPerFrame,
                        s.radiusAngleDraws[r][3] * perFrame, s.radiusAngleTicks[r][3] * msPerFrame, s.radiusAngleDraws[r][4] * perFrame,
                        s.radiusAngleTicks[r][4] * msPerFrame);
                }
            }
            text += " by command";
            for (int i = 0; i < Sources; ++i)
                text += std::format("{} {} {:.1f}", i ? "," : "", SourceNames[i], s.sourceDraws[i] * perFrame);
            text += "\n" + lampLines;
            if (s.camera)
            {
                static constexpr const char* cameraDistances[6] = { "0-25 m", "25-50 m", "50-100 m", "100-200 m", "200-400 m", "400+ m" };
                static constexpr const char* lampDistances[6] = { "0-2 m", "2-5 m", "5-10 m", "10-20 m", "20-40 m", "40+ m" };
                static constexpr const char* cameraSizes[6] = { "under 0.5%", "0.5-1%", "1-2%", "2-5%", "5-10%", "10%+" };
                static constexpr const char* lampSizes[6] = { "under 2%", "2-5%", "5-10%", "10-20%", "20-50%", "50%+" };
                const auto distances = s.lamp ? lampDistances : cameraDistances;
                const auto sizes = s.lamp ? lampSizes : cameraSizes;
                text += s.lamp ? "    by distance from the lamp:" : "    by distance:";
                for (int i = 0; i < 6; ++i)
                    text += std::format("{} {} {:.1f} ({:.3f} ms)", i ? "," : "", distances[i], s.distanceDraws[i] * perFrame, s.distanceTicks[i] * msPerFrame);
                text += "\n    by size on screen (radius over distance):";
                for (int i = 0; i < 6; ++i)
                    text += std::format("{} {} {:.1f} ({:.3f} ms)", i ? "," : "", sizes[i], s.sizeDraws[i] * perFrame, s.sizeTicks[i] * msPerFrame);
                text += "\n";
            }
            if (s.models && !s.byModel.empty())
            {
                std::vector<std::pair<uint16_t, TargetStats::ModelStats>> top(s.byModel.begin(), s.byModel.end());
                std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second.ticks > b.second.ticks; });
                int64_t all = 0;
                for (auto& [model, m] : top)
                    all += m.ticks;
                text += std::format("    {} models; the costliest (name hash, radius, kind, draws, ms, us a draw, mean distance{}):\n",
                    top.size(), s.lamp ? " from the lamp" : "");
                int64_t shown = 0;
                for (size_t i = 0; i < top.size() && i < 20; ++i)
                {
                    const auto& [model, m] = top[i];
                    const auto info = modelInfos ? *reinterpret_cast<const uintptr_t*>(modelInfos + model * 4) : 0;
                    const uint32_t hash = info ? *reinterpret_cast<const uint32_t*>(info + 0x3c) : 0;
                    const float radius = info ? *reinterpret_cast<const float*>(info + 0x1c) : 0.0f;
                    text += std::format("      {:08X} {:6.1f} m {:<9} {:7.2f} {:6.3f} ms {:6.2f} {:6.1f} m\n", hash, radius, KindNames[size_t(m.kind)],
                        m.draws * perFrame, m.ticks * msPerFrame, m.draws ? m.ticks * 1e6 / double(qpcFrequency) / m.draws : 0.0,
                        m.placed ? m.distance / double(m.placed) : 0.0);
                    shown += m.ticks;
                }
                text += std::format("      the top 20 take {:.0f}% of the models' time\n", all ? 100.0 * shown / all : 0.0);
            }
        }
        FusionLog::WriteText("EntityClass", "Draws", text);
        targetStats.clear();
        lastTargetName = nullptr;
    }

    // The model index and the position the command keeps: CDrawEntityDC +0x30 and +0x20, CDrawFragTypeDC
    // +0x58 and +0x40, CDrawFragDC (vehicles, objects with fragments) +0x46 and +0x10 (the entity's matrix row +0x40,
    // its ctor 0x8dad40); false for the other commands
    bool ModelAndPlace(void* dc, Source source, uint16_t& model, const float*& place)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(dc);
        if (source == FromEntity)
            model = *reinterpret_cast<const uint16_t*>(bytes + 0x30), place = reinterpret_cast<const float*>(bytes + 0x20);
        else if (source == FromFragType)
            model = *reinterpret_cast<const uint16_t*>(bytes + 0x58), place = reinterpret_cast<const float*>(bytes + 0x40);
        else if (source == FromFrag)
            model = *reinterpret_cast<const uint16_t*>(bytes + 0x46), place = reinterpret_cast<const float*>(bytes + 0x10);
        else
            return false;
        return true;
    }

    bool IsLampMaps(const char* target)
    {
        static const char* lastName = nullptr;
        static bool lastIs = false;
        if (target != lastName)
        {
            lastName = target;
            lastIs = target && !std::strcmp(target, "SHADOW_MAP_COLOUR_POINT");
        }
        return lastIs;
    }

    // The halves of the lamps' maps: where each face command's lamp was as it was made (game thread), and on the
    // render thread the half being drawn now (-1 none) with its lamp
    struct FaceOrigin
    {
        float position[3];
        float axis[3];
        float direction[3];
        float radius;
        float coneAngle;    // a spot's outer half angle in radians; negative when not a spot or not known
    };
    FaceOrigin faceOrigins[256]{};
    uint8_t nextFaceOrigin = 0;
    constexpr ptrdiff_t kFaceOriginByte = 0x16, kFaceMarkByte = 0x17;
    constexpr uint8_t kFaceMark = 0xA5;
    uintptr_t slotRecords = 0;      // CE 0x119f100, 0x110 bytes a slot
    // The light each of the game's 8 slots was given last: its key (CLightSource +0x60, the slot's +0xf8), its spot
    // cone or a negative
    struct SlotLight
    {
        uint32_t key = 0;
        float coneAngle = -1.0f;
    };
    SlotLight slotLights[8]{};
    DrawExecute faceExecute = nullptr;
    DrawExecute colorWriteExecute = nullptr;
    bool bFaceHooks = false;
    int currentFace = -1;
    FaceOrigin currentOrigin{};

    void MarkFace(uintptr_t dc, uintptr_t slotOffset)
    {
        if (!dc || !slotRecords)
            return;
        const auto slot = slotRecords + slotOffset;
        auto& origin = faceOrigins[nextFaceOrigin];
        std::memcpy(origin.position, reinterpret_cast<const float*>(slot + 0xc0), sizeof(origin.position));
        std::memcpy(origin.axis, reinterpret_cast<const float*>(slot + 0x60), sizeof(origin.axis));
        std::memcpy(origin.direction, reinterpret_cast<const float*>(slot + 0xd0), sizeof(origin.direction));
        origin.radius = *reinterpret_cast<const float*>(slot + 0xe0);
        origin.coneAngle = -1.0f;
        if (const auto index = slotOffset / 0x110; index < std::size(slotLights) && slotLights[index].key &&
            slotLights[index].key == *reinterpret_cast<const uint32_t*>(slot + 0xf8))
            origin.coneAngle = slotLights[index].coneAngle;
        auto* bytes = reinterpret_cast<uint8_t*>(dc);
        bytes[kFaceOriginByte] = nextFaceOrigin++;
        bytes[kFaceMarkByte] = kFaceMark;
    }

    // The selection fills a slot from a light: ecx the light list, edi the light's index * 0x80, esi the slot + 0xc8
    void SlotFilled(uintptr_t list, uintptr_t offset, uintptr_t slotAt)
    {
        if (!list || !slotRecords || slotAt < slotRecords + 0xc8)
            return;
        const auto index = (slotAt - 0xc8 - slotRecords) / 0x110;
        if (index >= std::size(slotLights))
            return;
        const auto light = list + offset;
        auto& s = slotLights[index];
        s.key = *reinterpret_cast<const uint32_t*>(light + 0x60);
        const int type = *reinterpret_cast<const int32_t*>(light + 0x44);
        const float outerCos = *reinterpret_cast<const float*>(light + 0x5c);
        s.coneAngle = type == 2 && outerCos > 0.0f && outerCos <= 1.0f ? std::acos(outerCos) : -1.0f;
    }

    void __fastcall FaceExecute(void* dc, void*)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(dc);
        const int face = *reinterpret_cast<const int32_t*>(bytes + 0x10);
        if (bytes[kFaceMarkByte] == kFaceMark && (face == 0 || face == 1))
        {
            currentFace = face;
            currentOrigin = faceOrigins[bytes[kFaceOriginByte]];
        }
        else
            currentFace = -1;
        faceExecute(dc);
    }

    void __fastcall ColorWriteExecute(void* dc, void*)
    {
        if (*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(dc) + 8) == 0xf)
            currentFace = -1;
        colorWriteExecute(dc);
    }

    // Where the draw's bounding sphere is against the lamp of the half drawn. side: against the plane between the
    // lamp's halves, -1 wholly on the side the half drawn does not face, 1 wholly on its side, 0 across or unknown (the
    // first half faces -axis, the second +axis); beyond: wholly outside the lamp's radius; tiny: a prop whose reach is
    // under kLampSmallSize of its distance from the lamp
    struct LampPlace
    {
        int side = 0;
        bool beyond = false;
        bool tiny = false;
        bool outsideCone = false;
    };
    LampPlace SphereAtLamp(void* dc, Kind kind, Source source)
    {
        uint16_t model;
        const float* place;
        if (currentFace < 0 || !modelInfos || !ModelAndPlace(dc, source, model, place))
            return {};
        const auto info = *reinterpret_cast<const uintptr_t*>(modelInfos + model * 4);
        if (!info)
            return {};
        const float* centre = reinterpret_cast<const float*>(info + 0x10);
        const float radius = *reinterpret_cast<const float*>(info + 0x1c);
        if (!(radius > 0.0f))
            return {};
        // A fragment's bones leave the model's rest pose (open doors, a hanging bumper, a turned rotor): its sphere
        // grows by a quarter and a metre
        const float rest = radius + std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
        const float reach = source == FromFrag ? rest * 1.25f + 1.0f : rest;
        const auto& o = currentOrigin;
        float along = 0.0f, distance2 = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            const float d = place[i] - o.position[i];
            along += d * o.axis[i];
            distance2 += d * d;
        }
        if (currentFace == 0)
            along = -along;
        LampPlace result;
        result.side = along > reach ? 1 : along < -reach ? -1 : 0;
        const float outer = o.radius + reach;
        result.beyond = o.radius > 0.0f && distance2 > outer * outer;
        result.tiny = kind == Kind::Prop && reach * reach < kLampSmallSize * kLampSmallSize * distance2;
        // Outside the cone: the angle off the direction past the cone's by more than the sphere's own and the margin
        if (o.coneAngle >= 0.0f && distance2 > reach * reach)
        {
            const float* dir = o.direction;
            const float dirLength = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (dirLength > 0.5f)
            {
                float dot = 0.0f;
                for (int i = 0; i < 3; ++i)
                    dot += (place[i] - o.position[i]) * dir[i];
                const float distance = std::sqrt(distance2);
                const float off = std::acos(std::clamp(dot / (dirLength * distance), -1.0f, 1.0f));
                result.outsideCone = off > o.coneAngle + std::asin(reach / distance) + kConeMargin;
            }
        }
        return result;
    }

    // The draw's distance from the current viewport's camera (in the lamps' maps from the lamp of the half drawn) and
    // its size on screen, into their buckets; the distance, or a negative when not known
    float CountByCamera(TargetStats& stats, uint16_t model, const float* place, int64_t ticks)
    {
        auto viewport = rage::GetCurrentViewport();
        if (!modelInfos || (stats.lamp ? currentFace < 0 : !viewport))
            return -1.0f;
        const auto info = *reinterpret_cast<const uintptr_t*>(modelInfos + model * 4);
        if (!info)
            return -1.0f;
        const float* centre = reinterpret_cast<const float*>(info + 0x10);
        const float radius = *reinterpret_cast<const float*>(info + 0x1c);
        const float reach = radius + std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
        const float* eye = stats.lamp ? currentOrigin.position : viewport->mViewInverseMatrix[3];
        const float dx = place[0] - eye[0], dy = place[1] - eye[1], dz = place[2] - eye[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        static constexpr float cameraDistanceEdges[5] = { 25.0f, 50.0f, 100.0f, 200.0f, 400.0f };
        static constexpr float lampDistanceEdges[5] = { 2.0f, 5.0f, 10.0f, 20.0f, 40.0f };
        static constexpr float cameraSizeEdges[5] = { 0.005f, 0.01f, 0.02f, 0.05f, 0.1f };
        static constexpr float lampSizeEdges[5] = { 0.02f, 0.05f, 0.1f, 0.2f, 0.5f };
        const auto distanceEdges = stats.lamp ? lampDistanceEdges : cameraDistanceEdges;
        const auto sizeEdges = stats.lamp ? lampSizeEdges : cameraSizeEdges;
        const float size = reach / (std::max)(distance, 1.0f);
        int d = 0, z = 0;
        while (d < 5 && distance >= distanceEdges[d])
            ++d;
        while (z < 5 && size >= sizeEdges[z])
            ++z;
        stats.distanceDraws[d]++;
        stats.distanceTicks[d] += ticks;
        stats.sizeDraws[z]++;
        stats.sizeTicks[z] += ticks;
        return distance;
    }

    void Execute(DrawExecute original, void* dc, Kind kind, Source source)
    {
        const bool leftOutPass = RenderSkip::DropsDraws();
        if (leftOutPass && bSkipLeftOut)
        {
            if (bDrawLog)
                StatsFor(RenderSkip::CurrentTarget()).leftOutPass++;
            return;
        }
        const bool lampCulling = bFaceCulling || bRangeCulling || bSmallCulling || bConeCulling;
        const char* target = bDrawLog || lampCulling ? RenderSkip::CurrentTarget() : nullptr;
        const bool lampMaps = (bDrawLog || lampCulling) && IsLampMaps(target);
        const auto atLamp = lampMaps ? SphereAtLamp(dc, kind, source) : LampPlace{};
        const int side = atLamp.side;
        // The half drawn faces away from the sphere
        const bool outside = side < 0;
        if ((outside && bFaceCulling) || (atLamp.beyond && bRangeCulling) || (atLamp.tiny && bSmallCulling) ||
            (atLamp.outsideCone && bConeCulling))
        {
            if (bDrawLog)
            {
                auto& stats = StatsFor(target);
                if (outside)
                    stats.outside++;
                if (atLamp.beyond)
                    stats.beyond++;
                if (atLamp.tiny)
                    stats.tiny++;
                if (atLamp.outsideCone)
                    stats.outsideCone++;
                stats.faceKnown++;
                stats.radiusDraws++;
                stats.radiusSum += currentOrigin.radius;
                (side < 0 ? stats.behind : side > 0 ? stats.ahead : stats.across)++;
            }
            return;
        }

        const auto& hooks = Hooks();
        currentDc = dc;
        currentSource = source;
        for (const auto& [before, after] : hooks)
            if (before)
                before(kind);
        if (!bDrawLog)
            original(dc);
        else
        {
            LARGE_INTEGER start, end;
            QueryPerformanceCounter(&start);
            original(dc);
            QueryPerformanceCounter(&end);
            const uint32_t frame = TemporalAA::SceneFrame;
            if (!statsFrom || frame < statsFrom)
                statsFrom = frame;
            else if (frame - statsFrom >= kDrawLogFrames)
            {
                WriteDrawLog(frame - statsFrom);
                statsFrom = frame;
            }
            auto& stats = StatsFor(target);
            stats.draws[size_t(kind)]++;
            stats.ticks[size_t(kind)] += end.QuadPart - start.QuadPart;
            stats.sourceDraws[source]++;
            if (outside)
                stats.outside++;
            if (atLamp.beyond)
                stats.beyond++;
            if (atLamp.tiny)
                stats.tiny++;
            if (atLamp.outsideCone)
                stats.outsideCone++;
            if (lampMaps && currentFace >= 0)
            {
                stats.radiusDraws++;
                stats.radiusSum += currentOrigin.radius;
                if (currentOrigin.coneAngle >= 0.0f)
                {
                    stats.spotDraws++;
                    stats.spotConeSum += currentOrigin.coneAngle * 180.0 / 3.14159265;
                }
                uint16_t model;
                const float* place;
                if (ModelAndPlace(dc, source, model, place))
                {
                    const auto& o = currentOrigin;
                    const int r = o.radius < 15.0f ? 0 : o.radius < 30.0f ? 1 : o.radius < 50.0f ? 2 : 3;
                    const float* dir = o.direction;
                    const float dirLength = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                    float dot = 0.0f, length2 = 0.0f;
                    for (int i = 0; i < 3; ++i)
                    {
                        const float d = place[i] - o.position[i];
                        dot += d * dir[i];
                        length2 += d * d;
                    }
                    int a = 4;
                    if (dirLength > 0.5f && length2 > 1e-4f)
                    {
                        const float c = dot / (dirLength * std::sqrt(length2));
                        a = c > 0.7071f ? 0 : c > 0.0f ? 1 : c > -0.7071f ? 2 : 3;
                    }
                    stats.radiusAngleDraws[r][a]++;
                    stats.radiusAngleTicks[r][a] += end.QuadPart - start.QuadPart;
                }
            }
            if (leftOutPass)
                stats.leftOutPass++;
            if (lampMaps)
            {
                uint16_t model;
                const float* place;
                if (ModelAndPlace(dc, source, model, place))
                    (currentFace >= 0 ? stats.faceKnown : stats.faceUnknown)++;
                if (currentFace >= 0)
                    (side < 0 ? stats.behind : side > 0 ? stats.ahead : stats.across)++;
            }
            const auto lock = RenderSkip::LockCount();
            if (lock != lastLock)
            {
                lastLock = lock;
                stats.locks++;
            }
            if (const auto viewport = RenderSkip::ViewportCount(); viewport != lastViewport)
            {
                lastViewport = viewport;
                stats.faces++;
            }
            uint16_t model;
            const float* place;
            if (ModelAndPlace(dc, source, model, place))
            {
                const int64_t ticks = end.QuadPart - start.QuadPart;
                const float distance = stats.camera ? CountByCamera(stats, model, place, ticks) : -1.0f;
                if (stats.models)
                {
                    auto& m = stats.byModel[model];
                    m.draws++;
                    m.ticks += ticks;
                    m.kind = kind;
                    if (distance >= 0.0f)
                    {
                        m.placed++;
                        m.distance += distance;
                    }
                }
                uint64_t key = model;
                for (int i = 0; i < 3; ++i)
                    key = key * 0x9E3779B97F4A7C15ull ^ reinterpret_cast<const uint32_t*>(place)[i];
                if (stats.keysLock != lock)
                {
                    stats.keysLock = lock;
                    stats.keys.clear();
                }
                stats.keyedDraws++;
                if (stats.keys.insert(key).second)
                    stats.entities++;
            }
        }
        for (auto it = hooks.rbegin(); it != hooks.rend(); ++it)
            if (it->second)
                it->second(kind);
        currentDc = nullptr;
    }

    Kind MarkOf(void* dc, ptrdiff_t at)
    {
        const auto kind = reinterpret_cast<uint8_t*>(dc)[at];
        return kind < uint8_t(Kind::Kinds) ? Kind(kind) : Kind::Unknown;
    }

    template <DrawExecute* original, Source source, ptrdiff_t at>
    void __fastcall DrawMarkedExecute(void* dc, void*)
    {
        Execute(*original, dc, MarkOf(dc, at), source);
    }

    void __fastcall DrawSkinnedExecute(void* dc, void*)
    {
        Execute(drawSkinnedExecute, dc, Kind::Prop, FromSkinned);
    }

    template <size_t command>
    void __fastcall DrawPedExecute(void* dc, void*)
    {
        Execute(drawPedExecute[command], dc, Kind::Ped, FromPed);
    }

    void Mark(uintptr_t dc, ptrdiff_t at, uintptr_t entity)
    {
        if (dc && entity)
            reinterpret_cast<uint8_t*>(dc)[at] = uint8_t(EntityClass::OfEntity(reinterpret_cast<void*>(entity)));
    }

    // The lamps' paraboloid halves (see LampShadowFaceCulling): the slot render's code is encrypted in the file, so its
    // patterns are only found once the game has decrypted it, which it has by init
    void InstallFaceHooks()
    {
        auto face0 = hook::pattern("C7 44 24 40 00 00 00 00 E8 ? ? ? ? EB 02 33 C0 50 E8");
        auto face1 = hook::pattern("C7 44 24 40 01 00 00 00 E8 ? ? ? ? EB 02 33 C0 50 E8");
        auto slots = hook::pattern("8D 85 ? ? ? ? 50 68 ? ? ? ? C6 44 24 27 01 C6 44 24 26 01");
        const char* status = "patterns not found";
        if (!face0.empty() && !face1.empty() && !slots.empty())
        {
            auto callTarget = [](uint8_t* call) { return call + 5 + *reinterpret_cast<int32_t*>(call + 1); };
            auto* site = face0.get_first<uint8_t>(0);
            auto* faceCtor = callTarget(site + 8);
            // The CSetColorWrite of the first half after it: push 1; mov ecx, eax; call
            auto* colorWriteCall = site + 0x27;
            const bool colorWriteFound = !std::memcmp(colorWriteCall, "\x6A\x01\x8B\xC8\xE8", 5);
            auto* colorWriteCtor = colorWriteFound ? callTarget(colorWriteCall + 4) : nullptr;
            // Each constructor's mov dword ptr [ecx], vtable
            if (!colorWriteCtor || faceCtor[0x28] != 0xC7 || faceCtor[0x29] != 0x01 || colorWriteCtor[0x24] != 0xC7 || colorWriteCtor[0x25] != 0x01)
                status = "constructors not as expected";
            else
            {
                slotRecords = *slots.get_first<uint32_t>(2) - 0xf0;
                auto** faceVtable = *reinterpret_cast<void***>(faceCtor + 0x2a);
                auto** colorWriteVtable = *reinterpret_cast<void***>(colorWriteCtor + 0x26);
                static auto face0Hook = safetyhook::create_mid(face0.get_first(17), [](SafetyHookContext& regs) { MarkFace(regs.eax, regs.ebp); });
                static auto face1Hook = safetyhook::create_mid(face1.get_first(17), [](SafetyHookContext& regs) { MarkFace(regs.eax, regs.ebp); });
                faceExecute = reinterpret_cast<DrawExecute>(faceVtable[1]);
                injector::WriteMemory(&faceVtable[1], reinterpret_cast<void*>(&FaceExecute), true);
                colorWriteExecute = reinterpret_cast<DrawExecute>(colorWriteVtable[1]);
                injector::WriteMemory(&colorWriteVtable[1], reinterpret_cast<void*>(&ColorWriteExecute), true);
                bFaceHooks = true;
                status = "in place";
                // The selection copying the light into its slot: movss xmm0, [edi+ecx+24h]; movss xmm1, [edi+ecx+28h];
                // mov eax, [edi+ecx+20h]; mov [esi-8], eax
                auto fill = hook::pattern("F3 0F 10 44 0F 24 F3 0F 10 4C 0F 28 8B 44 0F 20 89 46 F8");
                if (!fill.empty())
                {
                    static auto fillHook = safetyhook::create_mid(fill.get_first(0), [](SafetyHookContext& regs) { SlotFilled(regs.ecx, regs.edi, regs.esi); });
                    bConeHook = bool(fillHook);
                }
                if (!bConeHook)
                    status = "in place, the slots' lights not found (no cone culling)";
            }
        }
        FusionLog::Write("EntityClass", "Settings", "the lamps' paraboloid halves: %s\n", status);
    }

    void Install()
    {
        auto pattern = hook::pattern("56 E8 ? ? ? ? 8B 75 0C EB 02 33 C0 50 E8");
        auto fragTypeEntity = hook::pattern("FF 75 10 56 E8 ? ? ? ? 8B 75 0C 50 E8");
        auto fragTypeDynamic = hook::pattern("FF 74 24 3C E8 ? ? ? ? 50 E8 ? ? ? ? EB");
        auto fragDynamic = hook::pattern("FF 74 24 40 E8 ? ? ? ? 50 E8 ? ? ? ? E9");
        auto entityVtable = hook::pattern("C7 07 ? ? ? ? 66 89 77 30 8B 41 30");
        auto fragVtable = hook::pattern("C7 07 ? ? ? ? E8 ? ? ? ? 8B 5C 24 18 66 89 77 46");
        auto fragTypeVtable = hook::pattern("C7 07 ? ? ? ? E8 ? ? ? ? 8B 4C 24 1C 66 89 77");
        auto skinnedVtable = hook::pattern("C7 06 ? ? ? ? 66 89 7E 08 89 46 1C");
        // CDrawEntityDC's execute: movzx eax, word [esi+0x30]; mov eax, [eax*4 + model infos]
        if (auto infos = hook::pattern("0F B7 46 30 8B 04 85 ? ? ? ? 85 C0"); !infos.empty())
            modelInfos = *infos.get_first<uintptr_t>(7);
        hook::pattern pedVtables[kPedCommands] = {
            hook::pattern("C7 03 ? ? ? ? E8 ? ? ? ? 8A 44 24 2C"),
            hook::pattern("C7 03 ? ? ? ? E8 ? ? ? ? 8A 44 24 20"),
            hook::pattern("C7 07 ? ? ? ? E8 ? ? ? ? 8A 44 24 2C"),
            hook::pattern("C7 06 ? ? ? ? E8 ? ? ? ? 8B 4C 24 0C 8B 01 89 86 A0 00 00 00"),
            hook::pattern("C7 06 ? ? ? ? E8 ? ? ? ? 83 C4 20 8B C6"),
        };
        for (auto& ped : pedVtables)
            if (ped.empty())
                return;
        if (pattern.empty() || fragTypeEntity.empty() || fragTypeDynamic.empty() || fragDynamic.empty() ||
            entityVtable.empty() || fragVtable.empty() || fragTypeVtable.empty() || skinnedVtable.empty())
            return;

        // Right after each constructor: eax the command; the entity in edi in CEntity's add, in esi in CDynamicEntity's
        static auto entityHook = safetyhook::create_mid(pattern.get_first(6), [](SafetyHookContext& regs) { Mark(regs.eax, kDrawEntityKindByte, regs.edi); });
        static auto fragTypeEntityHook = safetyhook::create_mid(fragTypeEntity.get_first(9), [](SafetyHookContext& regs) { Mark(regs.eax, kDrawFragKindByte, regs.edi); });
        static auto fragTypeDynamicHook = safetyhook::create_mid(fragTypeDynamic.get_first(9), [](SafetyHookContext& regs) { Mark(regs.eax, kDrawFragKindByte, regs.esi); });
        static auto fragDynamicHook = safetyhook::create_mid(fragDynamic.get_first(9), [](SafetyHookContext& regs) { Mark(regs.eax, kDrawFragKindByte, regs.esi); });

        auto hookExecute = [](hook::pattern& ctor, DrawExecute& original, void* replacement)
        {
            auto slot = *ctor.get_first<void**>(2) + 1;
            original = reinterpret_cast<DrawExecute>(*slot);
            injector::WriteMemory(slot, replacement, true);
        };
        hookExecute(entityVtable, drawEntityExecute, reinterpret_cast<void*>(&DrawMarkedExecute<&drawEntityExecute, FromEntity, kDrawEntityKindByte>));
        hookExecute(fragVtable, drawFragExecute, reinterpret_cast<void*>(&DrawMarkedExecute<&drawFragExecute, FromFrag, kDrawFragKindByte>));
        hookExecute(fragTypeVtable, drawFragTypeExecute, reinterpret_cast<void*>(&DrawMarkedExecute<&drawFragTypeExecute, FromFragType, kDrawFragKindByte>));
        hookExecute(skinnedVtable, drawSkinnedExecute, reinterpret_cast<void*>(&DrawSkinnedExecute));
        hookExecute(pedVtables[0], drawPedExecute[0], reinterpret_cast<void*>(&DrawPedExecute<0>));
        hookExecute(pedVtables[1], drawPedExecute[1], reinterpret_cast<void*>(&DrawPedExecute<1>));
        hookExecute(pedVtables[2], drawPedExecute[2], reinterpret_cast<void*>(&DrawPedExecute<2>));
        hookExecute(pedVtables[3], drawPedExecute[3], reinterpret_cast<void*>(&DrawPedExecute<3>));
        hookExecute(pedVtables[4], drawPedExecute[4], reinterpret_cast<void*>(&DrawPedExecute<4>));
        bInstalled = true;
        InstallFaceHooks();
    }

    class EntityClassInit
    {
    public:
        EntityClassInit()
        {
            FusionFix::onInitEvent() += []()
            {
                Install();
                LARGE_INTEGER frequency;
                QueryPerformanceFrequency(&frequency);
                qpcFrequency = frequency.QuadPart;
                auto readIni = [](CIniReader& iniReader)
                {
                    bDrawLog = iniReader.ReadInteger("DEBUG", "EntityDrawLog", 0) != 0;
                    bFaceCullingSetting = iniReader.ReadInteger("PERFORMANCE", "LampShadowFaceCulling", 1) != 0;
                    bFaceCulling = bFaceCullingSetting && bFaceHooks;
                    bRangeCullingSetting = iniReader.ReadInteger("PERFORMANCE", "LampShadowRangeCulling", 1) != 0;
                    bRangeCulling = bRangeCullingSetting && bFaceHooks;
                    bSmallCullSetting = iniReader.ReadInteger("PERFORMANCE", "LampShadowSmallCull", 1) != 0;
                    bSmallCulling = bSmallCullSetting && bFaceHooks;
                    bConeCullSetting = iniReader.ReadInteger("PERFORMANCE", "LampShadowConeCulling", 1) != 0;
                    bConeCulling = bConeCullSetting && bFaceHooks && bConeHook;
                    bSkipLeftOut = iniReader.ReadInteger("PERFORMANCE", "SkipLeftOutDraws", 1) != 0;
                    FusionLog::Write("EntityClass", "Settings", "EntityDrawLog %d, LampShadowFaceCulling %d (%s), LampShadowRangeCulling %d (%s), LampShadowSmallCull %d (%s), LampShadowConeCulling %d (%s), SkipLeftOutDraws %d\n",
                        int(bDrawLog), int(bFaceCullingSetting), !bFaceCullingSetting ? "off" : bFaceCulling ? "on" : "no hooks",
                        int(bRangeCullingSetting), !bRangeCullingSetting ? "off" : bRangeCulling ? "on" : "no hooks",
                        int(bSmallCullSetting), !bSmallCullSetting ? "off" : bSmallCulling ? "on" : "no hooks",
                        int(bConeCullSetting), !bConeCullSetting ? "off" : bConeCulling ? "on" : "no hooks", int(bSkipLeftOut));
                };
                CIniReader iniReader("");
                readIni(iniReader);
                IniReload::Add(readIni);
            };
        }
    } EntityClassInit;
}

namespace EntityClass
{
    void AddDrawHooks(DrawHook before, DrawHook after)
    {
        Hooks().emplace_back(before, after);
    }

    bool Installed()
    {
        return bInstalled;
    }

    bool CurrentModel(uint16_t& model, const float*& place)
    {
        return currentDc && ModelAndPlace(currentDc, currentSource, model, place);
    }

    uintptr_t ModelInfo(uint16_t model)
    {
        return modelInfos ? *reinterpret_cast<const uintptr_t*>(modelInfos + model * 4) : 0;
    }

    Kind OfEntity(void* entity)
    {
        // Entities of one class mostly come in runs: the last one is checked before the map
        thread_local uintptr_t lastVtable = 0;
        thread_local Kind lastKind = Kind::Unknown;
        thread_local std::unordered_map<uintptr_t, Kind> byVtable;
        const auto vtable = *reinterpret_cast<uintptr_t*>(entity);
        if (vtable == lastVtable)
            return lastKind;
        lastVtable = vtable;
        if (auto it = byVtable.find(vtable); it != byVtable.end())
            return lastKind = it->second;
        // The complete object locator: +16 the class hierarchy, whose +8 base count and +12 base array hold
        // base class descriptors, each with its type descriptor first (the class itself among them)
        const auto locator = *reinterpret_cast<uintptr_t*>(vtable - 4);
        const auto hierarchy = *reinterpret_cast<uintptr_t*>(locator + 16);
        const auto bases = *reinterpret_cast<uint32_t*>(hierarchy + 8);
        const auto array = *reinterpret_cast<uintptr_t**>(hierarchy + 12);
        Kind kind = Kind::Prop;
        for (uint32_t i = 0; i < bases && kind == Kind::Prop; ++i)
        {
            const auto type = *reinterpret_cast<uintptr_t*>(array[i]);
            const char* name = reinterpret_cast<const char*>(type + 8);
            if (!std::strcmp(name, ".?AVCVehicle@@"))
                kind = Kind::Vehicle;
            else if (!std::strcmp(name, ".?AVCPed@@"))
                kind = Kind::Ped;
            else
                for (auto building : { ".?AVCBuilding@@", ".?AVCAnimatedBuilding@@", ".?AVCInteriorInst@@", ".?AVCPortalInst@@" })
                    if (!std::strcmp(name, building))
                        kind = Kind::Building;
        }
        byVtable.emplace(vtable, kind);
        return lastKind = kind;
    }
}
