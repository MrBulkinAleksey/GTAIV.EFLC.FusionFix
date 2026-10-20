module;

#include <common.hxx>
#include <algorithm>

export module hairstrands;

import common;
import comvars;
import settings;
import inireload;

// Hair Strands (Graphics > Lighting): hair takes the sun's and the lamps' highlights in streaks along its strands
// instead of one smooth blob. The hair shaders' G-buffer pass (shaders/patches/world_hair_strands.patch) leans the
// normal along each strand by an amount of its own and sharpens the highlight a little. c186 = strength, how far each
// strand leans, the shift towards the root, the strands' direction in the texture (0 along v, 1 along u); set as the
// G-buffer pass begins.
// Tuning: hidden keys of [LIGHTING] (HairStrands*), read again on Ctrl+Shift+F10.
class HairStrands
{
    static inline float fStrength = 1.0f;
    static inline float fJitter = 0.35f;
    static inline float fShift = -0.1f;
    static inline float fAlongU = 0.0f;

    static void ReadIni(CIniReader& iniReader)
    {
        fStrength = std::clamp(iniReader.ReadFloat("LIGHTING", "HairStrandsStrength", 1.0f), 0.0f, 3.0f);
        fJitter = std::clamp(iniReader.ReadFloat("LIGHTING", "HairStrandsJitter", 0.35f), 0.0f, 2.0f);
        fShift = std::clamp(iniReader.ReadFloat("LIGHTING", "HairStrandsShift", -0.1f), -1.0f, 1.0f);
        fAlongU = iniReader.ReadInteger("LIGHTING", "HairStrandsAlongU", 0) != 0 ? 1.0f : 0.0f;
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_HAIR_STRANDS");
        return pref ? pref->get() != 0 : true;
    }

    static bool Debug()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_HAIR_STRANDS_DEBUG");
        return pref && pref->get() != 0;
    }

    static void SetConstants()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        float strength = Enabled() ? fStrength : 0.0f;
        float jitter = fJitter;
        if (Debug())
        {
            strength = 1.0f;
            jitter = 1.5f;
        }
        const float params[4] = { strength, jitter, fShift, fAlongU };
        pDevice->SetPixelShaderConstantF(186, params, 1);
    }

public:
    HairStrands()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };

        FusionFix::onGameInitEvent() += []()
        {
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(SetConstants);
                if (cb)
                    cb->Append();
            };
        };
    }
} HairStrands;
