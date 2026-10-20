module;

#include <common.hxx>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "FusionLog.hpp"

export module modellights;

import common;
import comvars;

// A model's lights: its 2dfx light effects, the .light files of openFormats (see debug/light.md). The game turns
// each into a light in CE 0xC1E200, called only from 0xC1E1A4 with the entity in ESI; the light's data
// ("descriptor", the .light fields 4 bytes on) comes from the effect's virtual +0xC, kept in EDI:
//   +0x28 colour RGB bytes, +0x30 shaft intensity (_f2C), +0x34 shaft scale (_f30), +0x38 radius (Distance),
//   +0x3C intensity (Range), +0x40 corona size, +0x44 inner cone (HotSpot), +0x48 outer cone (Falloff),
//   +0x4C flags, +0x50 corona hash, +0x54 projected texture hash, +0x58 function byte (TypeFlags, 0-16, the
//   switch at 0xC1E246), +0x5B type (0 corona only, 1 point, 2 spot), +0x5C corona intensity (_f58),
//   +0x60 light fade distance (_f5C), +0x64 shaft fade distance (_f60).
// The game itself fades the light and its shaft by the two fade distances (in 0xC1E1A4's caller, passed in as
// brightness and fade) and gives the authored shaft to the submit (0xABCCD0), so those need nothing from us.
//
// What this module adds:
// - Current(): while the game submits a model light, which entity, model and descriptor it comes from, for the
//   light copy (OnAfterCopyLight runs inside the submit: 0xABCCD0 -> 0xABD2C0 -> 0xABC8A0, all synchronous).
// - GTAIV.EFLC.FusionFix.ModelLights.dat next to the plugin: radius, intensity and flags of a model's lights,
//   written into its descriptors the first time one of its lights is seen, as if the model had been edited:
//   the game's culling, shadows and coronas all see the new values.
export namespace ModelLights
{
    struct Context
    {
        uintptr_t entity = 0;
        uintptr_t descriptor = 0;
        uint32_t model = 0; // the model's name hash, 0 if unknown
        bool changed = false; // a ModelLights.dat line covers this light: Building Fill Lights leaves it be
    };

    // The model light being submitted, nullptr outside a model light's submit (main thread only).
    const Context* Current();
}

namespace
{
    ModelLights::Context context{};
    bool inside = false;

    struct Value
    {
        enum Kind : uint8_t { Keep, Set, Multiply } kind = Keep;
        float value = 0.0f;
        float Apply(float v) const { return kind == Set ? value : kind == Multiply ? v * value : v; }
    };

    struct Rule
    {
        uint8_t type = 0; // 0 any, 1 point, 2 spot
        Value radius, intensity;
        uint32_t flagsOn = 0, flagsOff = 0;
    };

    std::unordered_multimap<uint32_t, Rule> rules;

    // A descriptor we wrote: its values before and after, so the same address reused for a fresh copy (a model
    // streamed out and in again) is seen as new and written again.
    struct Written
    {
        uint32_t model;
        float radius, intensity;
        uint32_t flags;
    };
    std::unordered_map<uintptr_t, Written> written;

    uint32_t ModelOf(uintptr_t entity)
    {
        static const uintptr_t modelInfos = GameBase() + 0xE95CD8; // CE 0x1295cd8, model infos by index
        if (entity < 0x10000)
            return 0;
        const auto index = *reinterpret_cast<const int16_t*>(entity + 0x2E);
        if (index < 0 || index >= 31000)
            return 0;
        const auto info = *reinterpret_cast<const uintptr_t*>(modelInfos + index * 4);
        return info >= 0x10000 ? *reinterpret_cast<const uint32_t*>(info + 0x3C) : 0;
    }

    bool ParseValue(const std::string& text, Value& out)
    {
        if (text == "-")
            return true;
        const bool multiply = !text.empty() && (text[0] == 'x' || text[0] == '*');
        char* end = nullptr;
        const char* start = text.c_str() + (multiply ? 1 : 0);
        const float v = std::strtof(start, &end);
        if (end == start || *end || !std::isfinite(v) || v < 0.0f)
            return false;
        out = { multiply ? Value::Multiply : Value::Set, v };
        return true;
    }

    bool ParseFlags(const std::string& text, Rule& rule)
    {
        if (text == "-")
            return true;
        std::istringstream parts(text);
        std::string part;
        while (std::getline(parts, part, ','))
        {
            if (part.size() < 2 || (part[0] != '+' && part[0] != '-'))
                return false;
            char* end = nullptr;
            const auto bits = uint32_t(std::strtoul(part.c_str() + 1, &end, 0));
            if (*end || !bits)
                return false;
            (part[0] == '+' ? rule.flagsOn : rule.flagsOff) |= bits;
        }
        return true;
    }

    // model type radius intensity flags; '#' starts a comment. The model is its name or its hash (0x...). Type is
    // any, point or spot. Radius and intensity: a number sets it, x<number> multiplies it, - keeps it. Flags are the
    // .light file's (32 day, 64 night, 0x4000 static shadow, 0x8000 dynamic shadow, 0x80000 shaft, 0x100000 filler),
    // +bits and -bits joined by commas, or -.
    void Load()
    {
        rules.clear();
        const auto path = GetThisModulePath() / L"GTAIV.EFLC.FusionFix.ModelLights.dat";
        std::ifstream file(path);
        if (!file)
        {
            FusionLog::Write("ModelLights", "Load", "no GTAIV.EFLC.FusionFix.ModelLights.dat, no model light changes");
            return;
        }
        std::string line;
        for (int number = 1; std::getline(file, line); ++number)
        {
            if (const auto hash = line.find('#'); hash != std::string::npos)
                line.resize(hash);
            std::istringstream words(line);
            std::string model, type, radius, intensity, flags;
            if (!(words >> model))
                continue;
            Rule rule;
            bool ok = bool(words >> type >> radius >> intensity >> flags);
            ok = ok && (type == "any" || type == "point" || type == "spot");
            rule.type = type == "point" ? 1 : type == "spot" ? 2 : 0;
            ok = ok && ParseValue(radius, rule.radius) && ParseValue(intensity, rule.intensity) && ParseFlags(flags, rule);
            if (!ok)
            {
                FusionLog::Write("ModelLights", "Load", "line %d not understood: %s", number, line.c_str());
                continue;
            }
            const uint32_t key = model.starts_with("0x") || model.starts_with("0X")
                ? uint32_t(std::strtoul(model.c_str(), nullptr, 16))
                : hashStringLowercaseFromSeed(model.c_str(), 0);
            rules.emplace(key, rule);
        }
        FusionLog::Write("ModelLights", "Load", "%zu rules", rules.size());
    }

    bool Covered(uintptr_t descriptor, uint32_t model)
    {
        const auto type = *reinterpret_cast<const uint8_t*>(descriptor + 0x5B);
        const auto [first, last] = rules.equal_range(model);
        for (auto it = first; it != last; ++it)
            if (!it->second.type || it->second.type == type)
                return true;
        return false;
    }

    // Writes the rules into the descriptor; true where a rule covers this light.
    bool Apply(uintptr_t descriptor, uint32_t model)
    {
        if (!descriptor || !model || rules.empty() || !Covered(descriptor, model))
            return false;
        auto& radius = *reinterpret_cast<float*>(descriptor + 0x38);
        auto& intensity = *reinterpret_cast<float*>(descriptor + 0x3C);
        auto& flags = *reinterpret_cast<uint32_t*>(descriptor + 0x4C);
        if (const auto it = written.find(descriptor); it != written.end() && it->second.model == model &&
            it->second.radius == radius && it->second.intensity == intensity && it->second.flags == flags)
            return true;
        const auto type = *reinterpret_cast<const uint8_t*>(descriptor + 0x5B);
        const auto [first, last] = rules.equal_range(model);
        bool changed = false;
        const float oldRadius = radius, oldIntensity = intensity;
        const uint32_t oldFlags = flags;
        for (auto it = first; it != last; ++it)
        {
            const Rule& rule = it->second;
            if (rule.type && rule.type != type)
                continue;
            radius = rule.radius.Apply(radius);
            intensity = rule.intensity.Apply(intensity);
            flags = (flags | rule.flagsOn) & ~rule.flagsOff;
            changed = true;
        }
        if (!changed)
            return false;
        written[descriptor] = { model, radius, intensity, flags };
        static int logged = 0;
        if (logged < 200 && ++logged)
            FusionLog::Write("ModelLights", "Apply", "model 0x%08X type %u: radius %.1f -> %.1f, intensity %.2f -> %.2f, flags 0x%X -> 0x%X",
                             model, unsigned(type), oldRadius, radius, oldIntensity, intensity, oldFlags, flags);
        return true;
    }
}

const ModelLights::Context* ModelLights::Current()
{
    return inside ? &context : nullptr;
}

class ModelLightsHooks
{
public:
    ModelLightsHooks()
    {
        FusionFix::onInitEventAsync() += []()
        {
            Load();
            // In 0xC1E200 right after the effect's descriptor is fetched (call edx; mov [esp+13h], al is before):
            // EAX the descriptor, the caller's ESI (the entity) pushed at [esp+4].
            auto inner = hook::pattern("8B CE 88 44 24 13 FF D2 80 7C 24 13 00 8B F8");
            // 0xC1E1A4: the only call, and the stack cleanup after it.
            auto call = hook::pattern("F3 0F 11 1C 24 50 57 E8 ? ? ? ? 83 C4 34 C6 44 24 17 01");
            if (inner.size() != 1 || call.size() != 1)
            {
                FusionLog::Write("ModelLights", "Hooks", "model light submit not found (%zu, %zu), no model light changes",
                                 inner.size(), call.size());
                return;
            }
            static auto enter = safetyhook::create_mid(inner.get_first(8), [](SafetyHookContext& regs)
            {
                const auto entity = *reinterpret_cast<const uintptr_t*>(regs.esp + 4);
                context = { entity, uintptr_t(regs.eax), ModelOf(entity) };
                inside = true;
                context.changed = Apply(context.descriptor, context.model);
            });
            static auto leave = safetyhook::create_mid(call.get_first(12), [](SafetyHookContext&)
            {
                inside = false;
            });
            FusionLog::Write("ModelLights", "Hooks", "at %p and %p", inner.get_first(8), call.get_first(12));
        };
    }
} ModelLightsHooks;
