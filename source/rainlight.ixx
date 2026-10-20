module;

#include <common.hxx>
#include <algorithm>
#include <cmath>

export module rainlight;

import common;
import comvars;
import settings;
import inireload;

// Rain Light Scattering (Graphics > Lighting): in rain, drizzle and fog the engine's light shafts of street lamps and
// headlights grow brighter, and in the rain falling streaks run through them (shaders/patches/world_rain_light_shafts.patch).
// With Volumetric Light off the shafts come only with the rain. c185 = the streaks' strength, the clock in their
// cycles, 1 / the column width in pixels, cycles per pixel down the screen; set as the lighting begins.
// Tuning: hidden keys of [WEATHER] (RainLight*), read again on Ctrl+Shift+F10.
class RainLight
{
    static inline float fBoost = 1.5f;
    static inline float fStreaks = 0.35f;
    static inline float fFog = 0.6f;
    static inline float fColumnPixels = 3.0f;
    static inline float fStreaksPerScreen = 9.0f;
    static inline float fSpeed = 6.0f;

    static void ReadIni(CIniReader& iniReader)
    {
        fBoost = std::clamp(iniReader.ReadFloat("WEATHER", "RainLightBoost", 1.5f), 0.0f, 10.0f);
        fStreaks = std::clamp(iniReader.ReadFloat("WEATHER", "RainLightStreaks", 0.35f), 0.0f, 2.0f);
        fFog = std::clamp(iniReader.ReadFloat("WEATHER", "RainLightFog", 0.6f), 0.0f, 1.0f);
        fColumnPixels = std::clamp(iniReader.ReadFloat("WEATHER", "RainLightColumnPixels", 3.0f), 1.0f, 32.0f);
        fStreaksPerScreen = std::clamp(iniReader.ReadFloat("WEATHER", "RainLightStreaksPerScreen", 9.0f), 1.0f, 100.0f);
        fSpeed = std::clamp(iniReader.ReadFloat("WEATHER", "RainLightSpeed", 6.0f), 0.0f, 60.0f);
    }

    static bool Debug()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_RAIN_LIGHT_DEBUG");
        return pref && pref->get() != 0;
    }

    // 0 to 1: how much the air scatters the light, from the rain and from fog.
    static float Wetness()
    {
        if (Debug())
            return 1.0f;
        float wet = CWeather::Rain ? std::clamp(*CWeather::Rain / 0.7f, 0.0f, 1.0f) : 0.0f;
        if (CWeather::OldWeatherType && CWeather::NewWeatherType && CWeather::InterpolationValue)
        {
            const float t = std::clamp(*CWeather::InterpolationValue, 0.0f, 1.0f);
            const float fog = (*CWeather::OldWeatherType == CWeather::FOGGY ? 1.0f - t : 0.0f) + (*CWeather::NewWeatherType == CWeather::FOGGY ? t : 0.0f);
            wet = (std::max)(wet, fog * fFog);
        }
        return wet;
    }

    static float Rain()
    {
        if (Debug())
            return 1.0f;
        return CWeather::Rain ? std::clamp(*CWeather::Rain / 0.7f, 0.0f, 1.0f) : 0.0f;
    }

    static void SetConstants()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        D3DVIEWPORT9 vp = {};
        pDevice->GetViewport(&vp);
        const float height = vp.Height ? float(vp.Height) : 1080.0f;
        const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
        const float params[4] = {
            Enabled() ? fStreaks * Rain() : 0.0f, float(std::fmod(seconds * fSpeed, 1000.0)), 1.0f / fColumnPixels, fStreaksPerScreen / height,
        };
        pDevice->SetPixelShaderConstantF(185, params, 1);
    }

public:
    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_RAIN_LIGHT");
        return Debug() || (pref ? pref->get() != 0 : true);
    }

    // What a light's shaft intensity is multiplied by: 1 + boost in full rain with Volumetric Light on; with it off
    // the rain's share alone, so the shafts come with the rain; 0 leaves the light without a shaft.
    static float ShaftScale(bool volumetricLight)
    {
        if (!Enabled())
            return volumetricLight ? 1.0f : 0.0f;
        const float wet = Wetness();
        return volumetricLight ? 1.0f + fBoost * wet : wet;
    }

    RainLight()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };

        FusionFix::onGameInitEvent() += []()
        {
            CRenderPhaseDeferredLighting_LightsToScreen::OnBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(SetConstants);
                if (cb)
                    cb->Append();
            };
        };
    }
} RainLight;

// As each light is copied for the frame (postfx.ixx OnAfterCopyLight).
export float RainLightShaftScale(bool volumetricLight)
{
    return RainLight::ShaftScale(volumetricLight);
}
