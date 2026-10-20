module;

#include <common.hxx>
#include <algorithm>
#include <cmath>

export module waterdetails;

import common;
import comvars;
import settings;
import inireload;
import postfxresources;

using RageDirect3DDevice9::RealDevice;

// Water Details (Graphics > Lighting): foam along the shores and caustics on what lies under the water
// (shaders/patches/world_water_details.patch).
//  - Foam: the water's pixel shaders read the scene's depth behind the surface; where it is close (a shore, a pier's
//    leg, a hull) foam comes in, broken up by the waves' texture. c187, set right before the water is drawn
//    (postfx.ixx WaterRenderHook), through the game's device wrapper and the real device, as the rain rings are.
//  - Caustics: under the sea's level the sun's term of deferred_lighting takes the net of bright lines the waves
//    focus, worked out where the ray towards the sun meets the surface. c195/c196, set as the lighting begins.
// Tuning: hidden keys of [WEATHER] (WaterFoam*, WaterCaustics*), read again on Ctrl+Shift+F10.
class WaterDetails
{
    static inline float fFoam = 1.0f;
    static inline float fFoamWidth = 1.5f;
    static inline float fFoamBrightness = 0.9f;
    static inline float fCaustics = 0.6f;
    static inline float fCausticsScale = 0.6f;
    static inline float fCausticsDepth = 4.0f;

    static void ReadIni(CIniReader& iniReader)
    {
        fFoam = std::clamp(iniReader.ReadFloat("WEATHER", "WaterFoam", 1.0f), 0.0f, 1.0f);
        fFoamWidth = std::clamp(iniReader.ReadFloat("WEATHER", "WaterFoamWidth", 1.5f), 0.1f, 20.0f);
        fFoamBrightness = std::clamp(iniReader.ReadFloat("WEATHER", "WaterFoamBrightness", 0.9f), 0.0f, 4.0f);
        fCaustics = std::clamp(iniReader.ReadFloat("WEATHER", "WaterCaustics", 0.6f), 0.0f, 2.0f);
        fCausticsScale = std::clamp(iniReader.ReadFloat("WEATHER", "WaterCausticsScale", 0.6f), 0.05f, 10.0f);
        fCausticsDepth = std::clamp(iniReader.ReadFloat("WEATHER", "WaterCausticsDepth", 4.0f), 0.1f, 100.0f);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_WATER_DETAILS");
        return pref ? pref->get() != 0 : true;
    }

    static bool Debug()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_WATER_DETAILS_DEBUG");
        return pref && pref->get() != 0;
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

    // As the lighting begins: the caustics' constants for the sun's pass. WaterDetailsDebug puts the whole world under
    // the water, shallow, so the net shows wherever the sun reaches.
    static void SetCausticsConstants()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        auto& R = PostFxResources;
        const bool on = (Enabled() || Debug()) && !R.bInteriorScene;
        float level = R.pWaterLevel ? *R.pWaterLevel : 0.0f;
        float depth = fCausticsDepth;
        if (Debug())
        {
            level = 100000.0f;
            depth = 1000000.0f;
        }
        const float params[8] = {
            level, Seconds(), on ? (Debug() ? 1.0f : fCaustics) : 0.0f, fCausticsScale,
            1.0f / depth, 0.0f, 0.0f, 0.0f,
        };
        SetBoth(pDevice, 195, params, 2);
    }

public:
    // Right before the water is drawn: the foam's constants. WaterDetailsDebug makes it five times as wide, full.
    static void SetFoamConstants()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        const bool on = Enabled() || Debug();
        const float width = Debug() ? fFoamWidth * 5.0f : fFoamWidth;
        const float params[4] = { on ? (Debug() ? 1.0f : fFoam) : 0.0f, 1.0f / width, fFoamBrightness, 0.0f };
        SetBoth(pDevice, 187, params, 1);
    }

    WaterDetails()
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
} WaterDetails;

// Right before the water is drawn (postfx.ixx WaterRenderHook).
export void SetWaterFoamConstants()
{
    WaterDetails::SetFoamConstants();
}
