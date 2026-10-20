module;

#include <common.hxx>
#include <algorithm>

export module ssrocclusion;

import common;
import comvars;
import settings;
import inireload;

// SSR Occlusion (Graphics > Lighting "Reflections Without Double AO"): the sun's and the ambient passes multiply the
// reflection by the G-buffer's occlusion, right for the game's sky-only parabolic map, but where SSR found the scene
// the occluder is already in the reflection and it was darkened twice (shaders/patches/world_ssr_occlusion.patch).
// There the occlusion fades out by SSR's weight. c155.x = strength (0 = as the game), set as the lighting begins.
// Tuning: hidden [LIGHTING] SSROcclusionStrength, read again on Ctrl+Shift+F10.
class SSROcclusion
{
    static inline float fStrength = 1.0f;

    static void ReadIni(CIniReader& iniReader)
    {
        fStrength = std::clamp(iniReader.ReadFloat("LIGHTING", "SSROcclusionStrength", 1.0f), 0.0f, 1.0f);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_SSR_OCCLUSION");
        return pref ? pref->get() != 0 : true;
    }

    static void SetConstants()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        const float params[4] = { Enabled() ? fStrength : 0.0f, 0.0f, 0.0f, 0.0f };
        pDevice->SetPixelShaderConstantF(155, params, 1);
    }

public:
    SSROcclusion()
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
} SSROcclusion;
