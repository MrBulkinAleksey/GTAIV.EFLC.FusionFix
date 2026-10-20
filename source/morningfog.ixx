module;

#include <common.hxx>
#include <algorithm>
#include <cmath>

export module morningfog;

import common;
import comvars;
import settings;
import inireload;

// Morning Fog (Graphics > Lighting, with Volumetric Fog): a low fog lying in the streets and over the water around
// dawn. The volumetric fog of every shader is an exponential height fog (c211: density, height fall-off, power,
// altitude tweak, from the timecycle); in the morning its density and its fall-off both grow, so the fog gets thicker
// near the ground and thinner above it, and building tops stand out of it. How much: by the hour (rising from 4:30 to
// 6, full to 8, gone by 9:30) and the weather (calm and clear or overcast full, foggy full, windy a little, rain half).
// Tuning: hidden keys of [WEATHER] (MorningFog*), read again on Ctrl+Shift+F10.
class MorningFog
{
    static inline float fDensity = 3.0f;
    static inline float fFalloff = 3.0f;

    static void ReadIni(CIniReader& iniReader)
    {
        fDensity = std::clamp(iniReader.ReadFloat("WEATHER", "MorningFogDensity", 3.0f), 0.0f, 50.0f);
        fFalloff = std::clamp(iniReader.ReadFloat("WEATHER", "MorningFogFalloff", 3.0f), 0.0f, 50.0f);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_MORNING_FOG");
        return pref ? pref->get() != 0 : true;
    }

    static bool Debug()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_MORNING_FOG_DEBUG");
        return pref && pref->get() != 0;
    }

    static float WeatherShare(CWeather::eWeatherType type)
    {
        switch (type)
        {
        case CWeather::EXTRASUNNY:
        case CWeather::SUNNY:
        case CWeather::CLOUDY:
        case CWeather::FOGGY: return 1.0f;
        case CWeather::SUNNY_WINDY: return 0.3f;
        case CWeather::RAIN:
        case CWeather::DRIZZLE: return 0.5f;
        default: return 0.2f;
        }
    }

    // 0 to 1: how much morning fog there is now.
    static float Amount()
    {
        if (Debug())
            return 1.0f;
        if (!Enabled() || !CClock::ms_nGameClockHours || !CClock::ms_nGameClockMinutes || !CWeather::OldWeatherType ||
            !CWeather::NewWeatherType || !CWeather::InterpolationValue)
            return 0.0f;
        const float hour = *CClock::ms_nGameClockHours + *CClock::ms_nGameClockMinutes / 60.0f;
        const float time = std::clamp((hour - 4.5f) / 1.5f, 0.0f, 1.0f) * std::clamp((9.5f - hour) / 1.5f, 0.0f, 1.0f);
        const float t = std::clamp(*CWeather::InterpolationValue, 0.0f, 1.0f);
        const float weather = WeatherShare(*CWeather::OldWeatherType) * (1.0f - t) + WeatherShare(*CWeather::NewWeatherType) * t;
        return time * weather;
    }

public:
    // c211 as the timecycle gives it, changed in place: density, height fall-off, power, altitude tweak.
    static void Apply(float* c211)
    {
        const float amount = Amount();
        if (amount <= 0.0f)
            return;
        c211[0] *= 1.0f + fDensity * amount;
        c211[1] *= 1.0f + fFalloff * amount;
    }

    MorningFog()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };
    }
} MorningFog;

// Before the fog constants are set (shaders.ixx).
export void ApplyMorningFog(float* c211)
{
    MorningFog::Apply(c211);
}
