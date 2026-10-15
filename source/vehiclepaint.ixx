module;

#include <common.hxx>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

export module vehiclepaint;

import common;
import comvars;
import settings;
import inireload;
import postfxresources;

// Car paint (gta_vehicle_paint1/2/3, shaders/patches/vehicle_paint.patch), each part on its own in the graphics menu:
//  - Vehicle Paint (Textures): a clearcoat, the albedo's baked sun highlight turned down, richer colour and a pearl tint
//    towards the silhouette, on clean paint; dirt keeps the stock look.
//  - Car Raindrops (Lighting): in the rain, beads and running drops on the bodywork in the car's own space, and a wet
//    film's gloss over all of it.
// Their tuning is read from the ini ([TEXTURES] VehiclePaint*, [WEATHER] CarRaindrops*, not in the shipped file) at
// startup and on Ctrl+Shift+F10. The shaders read c175-c177, set here at the start of every G-buffer pass; zeros give
// the stock paint, so the shaders need no switch of their own.
class VehiclePaint
{
    static inline float fClearcoat = 1.0f;          // the clearcoat's reflectance, 0 off
    static inline float fClearcoatSmoothness = 0.75f; // the lowest gloss of clean paint: Phong power 512 times this
    static inline float fBakedHighlightCut = 0.2f;  // share of the baked sun highlight taken out
    static inline float fColorRichness = 1.25f;     // saturation, 1 as the game
    static inline float fPearl = 0.4f;              // the blue-violet tint at the silhouette, 0 off

    static inline float fRainAmount = 0.85f;        // share of the bead cells holding a drop in heavy rain
    static inline float fRainDensity = 40.0f;       // bead cells per metre; running drops' columns are half as dense
    static inline float fRainBump = 0.002f;         // a drop's height in metres
    static inline float fRainRunning = 1.0f;        // how many running drops on the sides, against the beads
    static inline float fRainSpeed = 0.15f;         // how fast running drops slide, metres per second

    // Missing prefs count as on.
    static int32_t Pref(std::optional<std::reference_wrapper<int32_t>>& ref)
    {
        return ref ? ref->get() : 1;
    }

    static void ReadIni(CIniReader& iniReader)
    {
        fClearcoat = std::clamp(iniReader.ReadFloat("TEXTURES", "VehiclePaintClearcoat", 1.0f), 0.0f, 2.0f);
        fClearcoatSmoothness = std::clamp(iniReader.ReadFloat("TEXTURES", "VehiclePaintClearcoatSmoothness", 0.75f), 0.0f, 1.0f);
        fBakedHighlightCut = std::clamp(iniReader.ReadFloat("TEXTURES", "VehiclePaintBakedHighlightCut", 0.2f), 0.0f, 1.0f);
        fColorRichness = std::clamp(iniReader.ReadFloat("TEXTURES", "VehiclePaintColorRichness", 1.25f), 0.5f, 2.0f);
        fPearl = std::clamp(iniReader.ReadFloat("TEXTURES", "VehiclePaintPearl", 0.4f), 0.0f, 1.0f);
        fRainAmount = std::clamp(iniReader.ReadFloat("WEATHER", "CarRaindropsAmount", 0.85f), 0.0f, 2.5f);
        fRainDensity = std::clamp(iniReader.ReadFloat("WEATHER", "CarRaindropsDensity", 40.0f), 5.0f, 200.0f);
        fRainBump = std::clamp(iniReader.ReadFloat("WEATHER", "CarRaindropsBump", 0.002f), 0.0f, 0.02f);
        fRainRunning = std::clamp(iniReader.ReadFloat("WEATHER", "CarRaindropsRunning", 1.0f), 0.0f, 1.0f);
        fRainSpeed = std::clamp(iniReader.ReadFloat("WEATHER", "CarRaindropsSpeed", 0.15f), 0.0f, 3.0f);
    }

    // First command of the G-buffer pass: the paint's constants for this frame.
    static void BeginGBuffer()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        auto& R = PostFxResources;

        static auto paintPref = FusionFixSettings.GetRef("PREF_VEHICLE_PAINT");
        static auto raindropsPref = FusionFixSettings.GetRef("PREF_CAR_RAINDROPS");
        const bool paint = Pref(paintPref) != 0;
        // Drops fall while it rains, the film dries as the wet ground does; none indoors.
        const bool raindrops = Pref(raindropsPref) != 0 && !R.bInteriorScene;
        const float rain = (raindrops && CWeather::Rain) ? std::clamp(*CWeather::Rain / 0.7f, 0.0f, 1.5f) : 0.0f;
        const float film = raindrops ? std::clamp((std::max)(R.fWetness, rain), 0.0f, 1.0f) : 0.0f;
        // Game time, so drops stand still while the game is paused; how far they slid wraps every 1000 m.
        const double seconds = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds * 0.001 : 0.0;
        const float slide = static_cast<float>(std::fmod(seconds * fRainSpeed, 1000.0));

        const float params[12] = {
            // c175: clearcoat reflectance, its gloss floor (stored as the square root of power / 512), richness - 1, pearl tint
            paint ? fClearcoat : 0.0f, paint ? std::sqrt(fClearcoatSmoothness) : 0.0f, paint ? fColorRichness - 1.0f : 0.0f, paint ? fPearl : 0.0f,
            // c176: the baked highlight's cut, the share of clean paint taking the gloss floor, the share of cells holding a
            // drop, how far running drops slid in metres
            paint ? fBakedHighlightCut : 0.0f, paint ? (std::min)(fClearcoat, 1.0f) : 0.0f, (std::min)(rain * fRainAmount * 0.6f, 0.9f), slide,
            // c177: bead cells per metre, a drop's height, the running drops' share, the wet film
            fRainDensity, fRainBump, fRainRunning, film,
        };
        pDevice->SetPixelShaderConstantF(175, params, 3);
    }

public:
    VehiclePaint()
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
                auto cb = new T_CB_Generic_NoArgs(BeginGBuffer);
                if (cb)
                    cb->Append();
            };
        };
    }
} VehiclePaint;
