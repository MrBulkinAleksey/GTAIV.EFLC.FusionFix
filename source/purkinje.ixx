module;

#include <common.hxx>
#include <algorithm>

export module purkinje;

import common;
import comvars;
import settings;
import inireload;
import postfxresources;

using RageDirect3DDevice9::RealDevice;

// Night Vision Shift (Graphics > Lighting): in the dark the eye sees with its rods, colours fade and lean to blue (the
// Purkinje shift). The game's composite post fx pass (shaders/patches/world_purkinje.patch) moves dark pixels towards
// their luminance in a cool tint, more the darker they are; bright lights keep their colour. How much by the clock:
// rising from 19:00 to 21:00, full through the night, gone from 5:00 to 7:00; none indoors. c181 = strength, 1 / the
// luminance where it ends; set right before the pass.
// Tuning: hidden keys of [POSTFX] (NightVision*), read again on Ctrl+Shift+F10.
class Purkinje
{
    static inline float fStrength = 0.6f;
    static inline float fEnd = 0.25f;

    static void ReadIni(CIniReader& iniReader)
    {
        fStrength = std::clamp(iniReader.ReadFloat("POSTFX", "NightVisionStrength", 0.6f), 0.0f, 1.0f);
        fEnd = std::clamp(iniReader.ReadFloat("POSTFX", "NightVisionLuminance", 0.25f), 0.01f, 4.0f);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_NIGHT_VISION");
        return pref ? pref->get() != 0 : true;
    }

    static bool Debug()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_NIGHT_VISION_DEBUG");
        return pref && pref->get() != 0;
    }

    static float Night()
    {
        if (!CClock::ms_nGameClockHours || !CClock::ms_nGameClockMinutes)
            return 0.0f;
        const float hour = *CClock::ms_nGameClockHours + *CClock::ms_nGameClockMinutes / 60.0f;
        const float evening = std::clamp((hour - 19.0f) / 2.0f, 0.0f, 1.0f);
        const float morning = std::clamp((7.0f - hour) / 2.0f, 0.0f, 1.0f);
        return (std::max)(evening, morning);
    }

public:
    static void SetConstants(IDirect3DDevice9* pDevice)
    {
        float strength = 0.0f;
        if (Debug())
            strength = 1.0f;
        else if (Enabled() && !PostFxResources.bInteriorScene)
            strength = fStrength * Night();
        const float params[4] = { strength, 1.0f / (Debug() ? fEnd * 4.0f : fEnd), 0.0f, 0.0f };
        pDevice->SetPixelShaderConstantF(181, params, 1);
        if (auto real = RealDevice(pDevice); real != pDevice)
            real->SetPixelShaderConstantF(181, params, 1);
    }

    Purkinje()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };
    }
} Purkinje;

// Right before the game's composite post fx pass (postfx.ixx DrawCallPostFX).
export void SetPurkinjeConstants(IDirect3DDevice9* pDevice)
{
    Purkinje::SetConstants(pDevice);
}
