module;

#include <common.hxx>
#include <algorithm>
#include <cmath>

export module waterlook;

import common;
import comvars;
import settings;
import inireload;
import postfxresources;

using RageDirect3DDevice9::RealDevice;

// Improved Water (Graphics > Lighting, [WEATHER] WaterLook): patches of wind on the surface, colour by depth, shore foam,
// a wider far glint and caustics, in the game's water and sun shaders (shaders/patches/zz_zz_zz_water_look.py). The sea
// stays flat and the physics as they are. Constants: c176, c177, c187 right before the water is drawn (postfx.ixx
// WaterRenderHook), through the game's device wrapper and the real device, as the rain rings are; c186, c196 as the
// lighting begins. Tuning: hidden keys of [WEATHER] (Water*), read again on Ctrl+Shift+F10.
class WaterLook
{
    static inline float fGust = 0.6f;
    static inline float fSlick = 0.7f;
    static inline float fFoam = 1.0f;
    static inline float fFoamWidth = 1.5f;
    static inline float fFoamBrightness = 0.9f;
    static inline float fShallowDepth = 6.0f;
    static inline float fShallowR = 0.75f, fShallowG = 1.25f, fShallowB = 1.1f;
    static inline float fGlintWiden = 0.002f;
    static inline float fCaustics = 0.6f;
    static inline float fCausticsScale = 0.6f;
    static inline float fCausticsDepth = 4.0f;

    static inline float fDriftX = 0.0f, fDriftY = 0.0f;
    static inline uint32_t nLastTime = 0;

    // The game's wind by weather, as the grass takes it: 0 calm to 1 storm.
    static float WeatherWind()
    {
        static constexpr float kWind[8] = { 0.15f, 0.25f, 0.8f, 0.2f, 0.5f, 0.3f, 0.1f, 1.0f };
        if (!CWeather::OldWeatherType || !CWeather::NewWeatherType || !CWeather::InterpolationValue)
            return 0.25f;
        const auto at = [](uint32_t type) { return kWind[(std::min)(type, 7u)]; };
        const float t = std::clamp(*CWeather::InterpolationValue, 0.0f, 1.0f);
        return at(*CWeather::OldWeatherType) * (1.0f - t) + at(*CWeather::NewWeatherType) * t;
    }

    static void ReadIni(CIniReader& iniReader)
    {
        fGust = std::clamp(iniReader.ReadFloat("WEATHER", "WaterGust", 0.6f), 0.0f, 3.0f);
        fSlick = std::clamp(iniReader.ReadFloat("WEATHER", "WaterSlick", 0.7f), 0.0f, 1.0f);
        fFoam = std::clamp(iniReader.ReadFloat("WEATHER", "WaterFoam", 1.0f), 0.0f, 1.0f);
        fFoamWidth = std::clamp(iniReader.ReadFloat("WEATHER", "WaterFoamWidth", 1.5f), 0.1f, 20.0f);
        fFoamBrightness = std::clamp(iniReader.ReadFloat("WEATHER", "WaterFoamBrightness", 0.9f), 0.0f, 4.0f);
        fShallowDepth = std::clamp(iniReader.ReadFloat("WEATHER", "WaterShallowDepth", 6.0f), 0.1f, 100.0f);
        fShallowR = std::clamp(iniReader.ReadFloat("WEATHER", "WaterShallowR", 0.75f), 0.0f, 4.0f);
        fShallowG = std::clamp(iniReader.ReadFloat("WEATHER", "WaterShallowG", 1.25f), 0.0f, 4.0f);
        fShallowB = std::clamp(iniReader.ReadFloat("WEATHER", "WaterShallowB", 1.1f), 0.0f, 4.0f);
        fGlintWiden = std::clamp(iniReader.ReadFloat("WEATHER", "WaterGlintWiden", 0.002f), 0.0f, 0.1f);
        fCaustics = std::clamp(iniReader.ReadFloat("WEATHER", "WaterCaustics", 0.6f), 0.0f, 2.0f);
        fCausticsScale = std::clamp(iniReader.ReadFloat("WEATHER", "WaterCausticsScale", 0.6f), 0.05f, 10.0f);
        fCausticsDepth = std::clamp(iniReader.ReadFloat("WEATHER", "WaterCausticsDepth", 4.0f), 0.1f, 100.0f);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_WATER_LOOK");
        return pref ? pref->get() != 0 : true;
    }

    static float Seconds()
    {
        const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
        return float(std::fmod(seconds, 1000.0));
    }

    static void SetBoth(IDirect3DDevice9* pDevice, UINT reg, const float* values, UINT count)
    {
        pDevice->SetPixelShaderConstantF(reg, values, count);
        if (auto real = RealDevice(pDevice); real != pDevice)
            real->SetPixelShaderConstantF(reg, values, count);
    }

    static void SetCausticsConstants()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        auto& R = PostFxResources;
        const bool on = Enabled() && !R.bInteriorScene;
        const float level[4] = { R.pWaterLevel ? *R.pWaterLevel : 0.0f, Seconds(), on ? fCaustics : 0.0f, fCausticsScale };
        const float fade[4] = { 1.0f / fCausticsDepth, 0.0f, 0.0f, 0.0f };
        SetBoth(pDevice, 186, level, 1);
        SetBoth(pDevice, 196, fade, 1);
    }

public:
    // Right before the water is drawn.
    static void SetSurfaceConstants()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        const bool on = Enabled();
        const float wind = WeatherWind();

        // The patches drift with the wind, by the time passed so a change of weather does not jump them.
        const uint32_t now = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds : 0;
        const float seconds = nLastTime && now > nLastTime ? (std::min)((now - nLastTime) * 0.001f, 0.25f) : 0.0f;
        nLastTime = now;
        const float speed = 1.5f + 6.0f * wind;
        fDriftX = std::fmod(fDriftX - 0.77f * speed * seconds, 100000.0f);
        fDriftY = std::fmod(fDriftY - 0.64f * speed * seconds, 100000.0f);

        // Rough patches with wind, smooth ones in calm weather.
        const float surface[4] = { on ? fGust * (0.4f + 0.6f * wind) : 0.0f, on ? fSlick * (1.0f - wind) : 0.0f, fDriftX, fDriftY };
        const float tint[4] = { on ? fShallowR - 1.0f : 0.0f, on ? fShallowG - 1.0f : 0.0f, on ? fShallowB - 1.0f : 0.0f, on ? fGlintWiden : 0.0f };
        const float foam[4] = { on ? fFoam : 0.0f, 1.0f / fFoamWidth, fFoamBrightness, -1.44269502f / fShallowDepth };
        SetBoth(pDevice, 176, surface, 1);
        SetBoth(pDevice, 177, tint, 1);
        SetBoth(pDevice, 187, foam, 1);
    }

    WaterLook()
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
                auto cb = new T_CB_Generic_NoArgs(SetCausticsConstants);
                if (cb)
                    cb->Append();
            };
        };
    }
} WaterLook;

export void SetWaterLookConstants()
{
    WaterLook::SetSurfaceConstants();
}
