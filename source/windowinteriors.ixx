module;

#include <common.hxx>
#include <algorithm>
#include <cmath>

export module windowinteriors;

import common;
import comvars;
import settings;
import inireload;

// Window Interiors (Graphics > Lighting): rooms behind the lit windows at night. The night windows' glow is drawn
// by the forward "draw" technique of gta_emissivenight, gta_glass_emissivenight and
// gta_normal_spec_reflect_emissivenight; shaders/patches/world_window_interiors.patch has each lit pixel of a wall
// look into a box room behind it (interior mapping) and shades the glow by what the view ray meets there. The rooms
// are a grid on the wall in metres, so they need no model data; the glow keeps its own pattern and colour.
// Registers, set before the transparent pass: c161 = camera position, strength (0 = off); c162 = 1 / room width,
// height and depth, part of the rooms with a blind; c163 = fade with the distance as a * distance + b, debug, gain.
// Tuning: hidden keys of [LIGHTING] (WindowInteriors*), read again on Ctrl+Shift+F10.
class WindowInteriors
{
    static constexpr UINT kRegister = 161;

    static inline float fStrength = 1.0f;
    static inline float fRoomWidth = 4.0f;
    static inline float fRoomHeight = 3.4f;
    static inline float fRoomDepth = 5.0f;
    static inline float fBlinds = 0.4f;
    static inline float fGain = 1.4f;
    static inline float fFadeStart = 80.0f;
    static inline float fFadeEnd = 220.0f;

    static void ReadIni(CIniReader& iniReader)
    {
        fStrength = std::clamp(iniReader.ReadFloat("LIGHTING", "WindowInteriorsStrength", 1.0f), 0.0f, 1.0f);
        fRoomWidth = std::clamp(iniReader.ReadFloat("LIGHTING", "WindowInteriorsRoomWidth", 4.0f), 0.5f, 50.0f);
        fRoomHeight = std::clamp(iniReader.ReadFloat("LIGHTING", "WindowInteriorsRoomHeight", 3.4f), 0.5f, 50.0f);
        fRoomDepth = std::clamp(iniReader.ReadFloat("LIGHTING", "WindowInteriorsRoomDepth", 5.0f), 0.5f, 50.0f);
        fBlinds = std::clamp(iniReader.ReadFloat("LIGHTING", "WindowInteriorsBlinds", 0.4f), 0.0f, 1.0f);
        fGain = std::clamp(iniReader.ReadFloat("LIGHTING", "WindowInteriorsGain", 1.4f), 0.0f, 4.0f);
        fFadeStart = std::max(iniReader.ReadFloat("LIGHTING", "WindowInteriorsFadeStart", 80.0f), 0.0f);
        fFadeEnd = std::max(iniReader.ReadFloat("LIGHTING", "WindowInteriorsFadeEnd", 220.0f), fFadeStart + 1.0f);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_WINDOW_INTERIORS");
        return pref ? pref->get() != 0 : true;
    }

    static bool Debug()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_WINDOW_INTERIORS_DEBUG");
        return pref && pref->get() != 0;
    }

    // Render thread, once the lights are drawn: the glows come in the transparent pass after.
    static void SetConstants()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        float camera[3] = {};
        bool perspective = false;
        if (auto viewport = rage::GetCurrentViewport(); viewport && viewport->mIsPerspective)
        {
            for (int i = 0; i < 3; ++i)
                camera[i] = viewport->mViewInverseMatrix[3][i];
            perspective = true;
        }
        const bool on = (Enabled() || Debug()) && perspective;
        const float fadeRange = fFadeEnd - fFadeStart;
        const float params[12] = {
            camera[0], camera[1], camera[2], on ? (Debug() ? 1.0f : fStrength) : 0.0f,
            1.0f / fRoomWidth, 1.0f / fRoomHeight, 1.0f / fRoomDepth, fBlinds,
            -1.0f / fadeRange, fFadeEnd / fadeRange, Debug() ? 1.0f : 0.0f, fGain,
        };
        pDevice->SetPixelShaderConstantF(kRegister, params, 3);
    }

public:
    WindowInteriors()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };

        FusionFix::onGameInitEvent() += []()
        {
            CRenderPhaseDeferredLighting_LightsToScreen::OnAfterBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(SetConstants);
                if (cb)
                    cb->Append();
            };
        };
    }
} WindowInteriors;
