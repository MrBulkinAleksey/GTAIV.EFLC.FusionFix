module;

#include <common.hxx>
#include <algorithm>
#include <cmath>

export module heathaze;

import common;
import comvars;
import settings;
import inireload;
import postfxresources;

using RageDirect3DDevice9::RealDevice;

// Heat Haze (Graphics > Lighting): the far scene wavers in the hot air on a clear day. The game's composite post fx
// pass reads the scene, its depth, blur and bloom at a wavering coordinate (shaders/patches/world_heat_haze.patch):
// a pixel or two, more with the distance, little in the sky. How strong: by the weather (extra sunny full, sunny
// three quarters, windy less, cloudy a little, none in rain or fog), the hour (from 9 to 11 it rises, from 16 to 19
// it falls), none while it rains or indoors.
// c188 = amplitude x, y (texture units), seconds, strength; c189 = -start / range, 1 / range, the sky's depth, the
// sky's share; set right before the pass.
// Tuning: hidden keys of [WEATHER] (HeatHaze*), read again on Ctrl+Shift+F10.
class HeatHaze
{
    static inline float fStrength = 1.0f;
    static inline float fPixels = 1.2f;
    static inline float fStart = 40.0f;
    static inline float fRange = 200.0f;
    static inline float fSky = 0.3f;

    static void ReadIni(CIniReader& iniReader)
    {
        fStrength = std::clamp(iniReader.ReadFloat("WEATHER", "HeatHaze", 1.0f), 0.0f, 2.0f);
        fPixels = std::clamp(iniReader.ReadFloat("WEATHER", "HeatHazePixels", 1.2f), 0.0f, 8.0f);
        fStart = std::max(iniReader.ReadFloat("WEATHER", "HeatHazeStart", 40.0f), 0.0f);
        fRange = std::max(iniReader.ReadFloat("WEATHER", "HeatHazeRange", 200.0f), 1.0f);
        fSky = std::clamp(iniReader.ReadFloat("WEATHER", "HeatHazeSky", 0.3f), 0.0f, 1.0f);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_HEAT_HAZE");
        return pref ? pref->get() != 0 : true;
    }

    static bool Debug()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_HEAT_HAZE_DEBUG");
        return pref && pref->get() != 0;
    }

    static float WeatherHeat(CWeather::eWeatherType type)
    {
        switch (type)
        {
        case CWeather::EXTRASUNNY: return 1.0f;
        case CWeather::SUNNY: return 0.75f;
        case CWeather::SUNNY_WINDY: return 0.4f;
        case CWeather::CLOUDY: return 0.1f;
        default: return 0.0f;
        }
    }

    // 0 to 1: how hot the air over the streets is now.
    static float Heat()
    {
        if (!CWeather::OldWeatherType || !CWeather::NewWeatherType || !CWeather::InterpolationValue || !CClock::ms_nGameClockHours ||
            !CClock::ms_nGameClockMinutes)
            return 0.0f;
        const float t = std::clamp(*CWeather::InterpolationValue, 0.0f, 1.0f);
        const float weather = WeatherHeat(*CWeather::OldWeatherType) * (1.0f - t) + WeatherHeat(*CWeather::NewWeatherType) * t;
        const float hour = *CClock::ms_nGameClockHours + *CClock::ms_nGameClockMinutes / 60.0f;
        const float day = std::clamp((hour - 9.0f) / 2.0f, 0.0f, 1.0f) * std::clamp((19.0f - hour) / 3.0f, 0.0f, 1.0f);
        const float dry = CWeather::Rain ? std::clamp(1.0f - *CWeather::Rain / 0.3f, 0.0f, 1.0f) : 1.0f;
        return weather * day * dry;
    }

public:
    static void SetConstants(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        float strength = 0.0f;
        if (Debug())
            strength = 1.0f;
        else if (Enabled() && !R.bInteriorScene)
            strength = fStrength * Heat();
        D3DVIEWPORT9 vp = {};
        if (FAILED(pDevice->GetViewport(&vp)) || !vp.Width || !vp.Height)
            strength = 0.0f;
        const float pixels = Debug() ? fPixels * 4.0f : fPixels;
        const float start = Debug() ? 0.0f : fStart;
        const float range = Debug() ? 30.0f : fRange;
        const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
        const float params[8] = {
            vp.Width ? pixels / vp.Width : 0.0f, vp.Height ? 0.6f * pixels / vp.Height : 0.0f, float(std::fmod(seconds, 1000.0)), strength,
            -start / range, 1.0f / range, 3000.0f, fSky,
        };
        pDevice->SetPixelShaderConstantF(188, params, 2);
        if (auto real = RealDevice(pDevice); real != pDevice)
            real->SetPixelShaderConstantF(188, params, 2);
    }

    HeatHaze()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };
    }
} HeatHaze;

// Right before the game's composite post fx pass (postfx.ixx DrawCallPostFX).
export void SetHeatHazeConstants(IDirect3DDevice9* pDevice)
{
    HeatHaze::SetConstants(pDevice);
}
