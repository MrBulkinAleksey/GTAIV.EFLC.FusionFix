module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <format>
#include <mutex>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

export module grasswind;

import common;
import comvars;
import settings;
import inireload;
import entityclass;
import postfxresources;

// Grass Wind (Graphics > Lighting, [PROCEDURAL] GrassWind): the wind sways the grass, weeds and small plants the game
// scatters on the ground (procedural props: proc_grass*, *_proc_plant_*, *weed*, sm_plant*). They are props drawn with
// gta_default (gta_cutout), whose deferred vertex shaders move each vertex when b12 is on
// (shaders/patches/world_grass_wind.patch). Around each G-buffer draw of such a model (by its name's hash, model info
// +0x3c) b12 is set and c241 gets the model's height and its bend and flutter at the tip; at the start of the pass c240
// and c242 get the wind's direction and the phases of the gust wave and the flutter.
//
// The game has no wind direction; the clouds' drift (0.93, 0.37) is taken so the grass and the clouds agree. The
// strength is the weather's own (CWeather::Update's table, lerped between the old and the new weather), without the
// game's rise with the camera's height, eased over a few seconds; none indoors. Game time, so the grass stands still
// while the game is paused.
//
// [PROCEDURAL] GrassWindStrength scales the sway; GrassWindModels adds model names (comma separated) to the list.
// GrassWindDebug (Graphics > Debug) sways four times as hard, to see which draws take it. GrassWindLog = 1 ([DEBUG])
// writes the wind and the grass draws to GTAIV.EFLC.FusionFix.GrassWind.log every 600 G-buffer passes.
class GrassWind
{
    static constexpr int kWindRegister = 240;       // c240: direction / wavelength, gust phase, flutter phase
    static constexpr int kModelRegister = 241;      // c241: 1 / height, bend and flutter at the tip (metres)
    static constexpr int kDirectionRegister = 242;  // c242.xy: the wind's unit direction
    static constexpr int kSwayFlag = 12;            // b12 (vertex shader)

    static constexpr float kDirection[2] = { 0.929f, 0.370f };
    // CWeather::Update's wind by weather type (0x103c878): EXTRASUNNY, SUNNY, SUNNY_WINDY, CLOUDY, RAIN, DRIZZLE,
    // FOGGY, LIGHTNING
    static constexpr float kWeatherWind[8] = { 0.15f, 0.25f, 0.8f, 0.2f, 0.5f, 0.3f, 0.1f, 1.0f };
    static constexpr float kGustWavelength = 8.0f;  // metres between gusts along the wind

    static inline float fStrength = 1.0f;
    static inline bool bLog = false;
    // GrassWindModels, read on the ini's thread; the list is made again on the render thread
    static inline std::mutex extraModelsMutex;
    static inline std::string sExtraModels;
    static inline std::atomic<bool> bRemakeList = true;

    // Render thread
    static inline bool bInGBuffer = false;
    static inline bool bBound = false;
    static inline int32_t lastModel = -1;
    static inline float fWind = -1.0f;              // eased, -1 until the first pass
    static inline double gustPhase = 0.0, flutterPhase = 0.0;
    static inline int32_t lastTime = -1;
    static inline float fBendPerHeight = 0.0f, fFlutterPerHeight = 0.0f;

    // By model index: the hash the entry was made for and the model's height, 0 if not grass
    struct ModelEntry { uint32_t hash = 0; float height = -1.0f; };
    static inline std::vector<ModelEntry> models;
    static inline std::unordered_set<uint32_t> grassHashes;

    // GrassWindLog
    static inline uint32_t logPasses = 0;
    static inline uint64_t logGrassDraws = 0, logPropDraws = 0;
    static inline std::unordered_set<uint16_t> logModels;

    static int32_t Pref(std::optional<std::reference_wrapper<int32_t>>& ref, int32_t missing)
    {
        return ref ? ref->get() : missing;
    }

    static void AddName(std::string_view name)
    {
        std::string text(name);
        grassHashes.insert(hashStringLowercaseFromSeed(text.c_str(), 0));
    }

    // The game's and our data's grass, weed and small plant props; names that do not exist cost nothing.
    static void MakeModelList()
    {
        grassHashes.clear();
        for (int i = 0; i <= 40; ++i)
        {
            AddName(std::format("cj_proc_plant_{}", i));
            AddName(std::format("ag_proc_plant_{}", i));
            AddName(std::format("proc_plant_{}", i));
        }
        for (int i = 0; i <= 20; ++i)
        {
            AddName(std::format("proc_grass{:02}", i));
            AddName(std::format("proc_weed{:02}", i));
            AddName(std::format("ag_weed{:02}", i));
            AddName(std::format("sm_weed{}_", i));
            AddName(std::format("sm_weed{}", i));
            AddName(std::format("sm_plant{}", i));
            AddName(std::format("grass0_{}", i));
            AddName(std::format("cj_proc_weed{}", i));
        }
        AddName("proc_grass_dry");
        AddName("cj_proc_weed");
        for (size_t pos = 0; pos < sExtraModels.size();)
        {
            size_t end = sExtraModels.find(',', pos);
            if (end == std::string::npos)
                end = sExtraModels.size();
            auto name = std::string_view(sExtraModels).substr(pos, end - pos);
            while (!name.empty() && name.front() == ' ')
                name.remove_prefix(1);
            while (!name.empty() && name.back() == ' ')
                name.remove_suffix(1);
            if (!name.empty())
                AddName(name);
            pos = end + 1;
        }
        models.assign(65536, {});
    }

    static void MakeModelListIfAsked()
    {
        if (!bRemakeList.exchange(false))
            return;
        std::lock_guard lock(extraModelsMutex);
        MakeModelList();
    }

    static void ReadIni(CIniReader& iniReader)
    {
        fStrength = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GrassWindStrength", 1.0f), 0.0f, 4.0f);
        bLog = iniReader.ReadInteger("DEBUG", "GrassWindLog", 0) != 0;
        {
            std::lock_guard lock(extraModelsMutex);
            sExtraModels = iniReader.ReadString("PROCEDURAL", "GrassWindModels", "");
        }
        bRemakeList = true;
    }

    // The model's height if it is grass, else 0
    static float GrassHeight(uint16_t model)
    {
        const auto info = EntityClass::ModelInfo(model);
        if (!info || models.empty())
            return 0.0f;
        const uint32_t hash = *reinterpret_cast<const uint32_t*>(info + 0x3c);
        auto& entry = models[model];
        if (entry.height < 0.0f || entry.hash != hash)
        {
            entry.hash = hash;
            entry.height = 0.0f;
            if (grassHashes.contains(hash))
            {
                // The props stand on their origin: the bounding sphere's centre is half way up; a model centred on its
                // origin falls back to the radius.
                const float centreZ = *reinterpret_cast<const float*>(info + 0x18);
                const float radius = *reinterpret_cast<const float*>(info + 0x1c);
                const float height = centreZ > 0.05f ? centreZ * 2.0f : radius;
                entry.height = std::isfinite(height) ? std::clamp(height, 0.1f, 4.0f) : 1.0f;
            }
        }
        return entry.height;
    }

    static float WeatherWind()
    {
        if (!CWeather::OldWeatherType || !CWeather::NewWeatherType || !CWeather::InterpolationValue)
            return 0.25f;
        const auto at = [](uint32_t type) { return kWeatherWind[(std::min)(type, 7u)]; };
        const float t = std::clamp(*CWeather::InterpolationValue, 0.0f, 1.0f);
        return at(*CWeather::OldWeatherType) * (1.0f - t) + at(*CWeather::NewWeatherType) * t;
    }

    static void SetFlag(IDirect3DDevice9* pDevice, bool on)
    {
        const BOOL flag = on;
        pDevice->SetVertexShaderConstantB(kSwayFlag, &flag, 1);
        bBound = on;
    }

    // First command of the G-buffer pass: this frame's wind.
    static void BeginGBuffer()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        static auto windPref = FusionFixSettings.GetRef("PREF_GRASS_WIND");
        static auto debugPref = FusionFixSettings.GetRef("PREF_GRASS_WIND_DEBUG");
        auto& R = PostFxResources;
        MakeModelListIfAsked();

        const int32_t now = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds : 0;
        const float dt = lastTime < 0 ? 0.0f : std::clamp((now - lastTime) * 0.001f, 0.0f, 0.1f);
        lastTime = now;

        const float target = R.bInteriorScene ? 0.0f : WeatherWind();
        fWind = fWind < 0.0f ? target : fWind + (target - fWind) * (1.0f - std::exp(-dt / 3.0f));

        // The gusts run downwind faster in a strong wind; the flutter quickens with it.
        gustPhase = std::fmod(gustPhase + dt * (1.5f + 5.0f * fWind) / kGustWavelength, 1.0);
        flutterPhase = std::fmod(flutterPhase + dt * (0.8f + 2.2f * fWind), 1.0);

        const bool on = Pref(windPref, 1) != 0 && fStrength > 0.0f && EntityClass::Installed();
        const float scale = on ? fStrength * (Pref(debugPref, 0) ? 4.0f : 1.0f) : 0.0f;
        fBendPerHeight = scale * (0.03f + 0.22f * fWind);
        fFlutterPerHeight = scale * (0.008f + 0.035f * fWind);

        const float wind[8] = {
            kDirection[0] / kGustWavelength, kDirection[1] / kGustWavelength, -static_cast<float>(gustPhase), static_cast<float>(flutterPhase),
            kDirection[0], kDirection[1], 0.0f, 0.0f,
        };
        pDevice->SetVertexShaderConstantF(kWindRegister, wind, 1);
        pDevice->SetVertexShaderConstantF(kDirectionRegister, wind + 4, 1);
        SetFlag(pDevice, false);
        lastModel = -1;
        bInGBuffer = on;

        if (bLog && ++logPasses >= 600)
        {
            std::string heights;
            for (const auto model : logModels)
                heights += std::format(" {:08x}:{:.2f}", models[model].hash, models[model].height);
            FusionLog::Write("GrassWind", "Stats", "wind %.2f (weather %.2f) scale %.2f, per pass: %.1f grass draws of %.1f prop draws; %zu grass models (hash:height m):%s\n",
                fWind, target, scale, double(logGrassDraws) / logPasses, double(logPropDraws) / logPasses, logModels.size(), heights.c_str());
            logPasses = 0;
            logGrassDraws = logPropDraws = 0;
            logModels.clear();
        }
    }

    static void EndGBuffer()
    {
        if (!bInGBuffer)
            return;
        bInGBuffer = false;
        if (auto pDevice = rage::grcDevice::GetD3DDevice(); pDevice && bBound)
            SetFlag(pDevice, false);
    }

    static void BeforeEntityDraw(EntityClass::Kind kind)
    {
        if (!bInGBuffer || kind != EntityClass::Kind::Prop)
            return;
        uint16_t model;
        const float* place;
        if (!EntityClass::CurrentModel(model, place))
            return;
        if (bLog)
            ++logPropDraws;
        const float height = GrassHeight(model);
        if (height <= 0.0f)
            return;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        if (model != lastModel)
        {
            const float params[4] = { 1.0f / height, height * fBendPerHeight, height * fFlutterPerHeight, 0.0f };
            pDevice->SetVertexShaderConstantF(kModelRegister, params, 1);
            lastModel = model;
        }
        SetFlag(pDevice, true);
        if (bLog)
        {
            ++logGrassDraws;
            logModels.insert(model);
        }
    }

    static void AfterEntityDraw(EntityClass::Kind)
    {
        if (!bBound)
            return;
        if (auto pDevice = rage::grcDevice::GetD3DDevice())
            SetFlag(pDevice, false);
    }

public:
    GrassWind()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };

        EntityClass::AddDrawHooks(BeforeEntityDraw, AfterEntityDraw);

        FusionFix::onGameInitEvent() += []()
        {
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(BeginGBuffer);
                if (cb)
                    cb->Append();
            };
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnAfterBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(EndGBuffer);
                if (cb)
                    cb->Append();
            };
        };

        FusionFix::onBeforeReset() += []()
        {
            bInGBuffer = false;
            bBound = false;
        };
    }
} GrassWind;
