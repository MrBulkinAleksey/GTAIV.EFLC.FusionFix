module;

#include <common.hxx>
#include <d3dx9tex.h>
#include <algorithm>
#include <cmath>
#include <iterator>

export module pbrsky;

import common;
import comvars;
import d3dx9_43;
import framehistory;
import settings;
import postfxresources;
import screenquad;
import postfxpass;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxPass;

// PBR sky (Physical Sky in the graphics menu): the clear sky's colour from SkyAtmosphere.fx's tables in place of the
// timecycle's gradient. The tables are drawn before the frame's first sky (PbrSkyBeforeDraw from the sky's draw hooks
// in postfx.ixx); gta_atmoscatt_clouds reads SkyView on s10 while c194.x says how much of it to take (sky_pbr.patch).
// Every sky drawn through those hooks gets it: the scene's, the water's reflection and the reflection map. The game's
// sun, moon, stars, clouds and fog stay as they were.

// The pixel shader constant the sky takes the table's share from (no game shader has a parameter there) and the
// sampler it reads the table on.
constexpr UINT kStrengthRegister = 194;
constexpr DWORD kSkyViewStage = 10;

// The table keeps the timecycle's brightness at every height (see SkyAtmosphere.fx), takes a third of its hue, and its
// bright side towards the sun rolls off from 1.2 to at most 2 times the timecycle's colour there; the glow within 20
// degrees of the sun up to 8 times, in the sunlight's own hue.
constexpr float kPalette = 0.35f;
constexpr float kKnee = 1.2f;
constexpr float kCeiling = 2.0f;
constexpr float kSunCeiling = 8.0f;
// Haze: Mie phase asymmetry, ground albedo and the camera's height above the sea in km.
constexpr float kMieG = 0.76f;
constexpr float kGroundAlbedo = 0.3f;
constexpr float kCameraHeight = 0.1f;
// The haze, times an Earth-like clear day's, in CWeather::eWeatherType order.
constexpr float kWeatherHaze[] = { 1.0f, 1.3f, 1.1f, 2.5f, 4.0f, 3.0f, 8.0f, 4.0f };
// The two tables drawn again once the haze has moved this much from what they were drawn for.
constexpr float kHazeRedraw = 0.02f;
// Below the horizon the sky hands back to the timecycle: all the table's from a sun height (sine) of -0.07 (4
// degrees below), none from -0.17 (10 degrees).
constexpr float kNightStart = -0.07f;
constexpr float kNightEnd = -0.17f;

static float Smoothstep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float WeatherHaze()
{
    if (!CWeather::OldWeatherType || !CWeather::NewWeatherType || !CWeather::InterpolationValue)
        return 1.0f;
    const auto pick = [](uint32_t type) { return type < std::size(kWeatherHaze) ? kWeatherHaze[type] : 1.0f; };
    const float t = std::clamp(*CWeather::InterpolationValue, 0.0f, 1.0f);
    return pick(*CWeather::OldWeatherType) * (1.0f - t) + pick(*CWeather::NewWeatherType) * t;
}

// The sky's parameters, registered at start (registering while drawing would grow the list the shader parameter hook
// may be reading); shaders.ixx and the clouds register some of them too, and get the same indices.
static size_t nSkyColorIdx = 0, nAzimuthColorIdx = 0, nAzimuthColorEastIdx = 0, nAzimuthStrengthIdx = 0, nAzimuthHeightIdx = 0,
              nSunDirectionIdx = 0, nSunExposureIdx = 0, nExposureIdx = 0;
static bool bParamsRegistered = false;

export void RegisterPbrSkyParams()
{
    nSkyColorIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "SkyColor");
    nAzimuthColorIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "AzimuthColor");
    nAzimuthColorEastIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "AzimuthColorEast");
    nAzimuthStrengthIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "AzimuthStrength");
    nAzimuthHeightIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "AzimuthHeight");
    nSunDirectionIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "SunDirection");
    nSunExposureIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "HDRSunExposure");
    nExposureIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "HDRExposure");
    bParamsRegistered = true;
}

// The timecycle's sky as gta_atmoscatt_clouds' vertex shader makes it: SkyColor, plus the azimuth colours (west and
// east, here their mean) towards the horizon, AzimuthStrength of them at the horizon, none from 1 / AzimuthHeight up.
static void TimecycleSky(D3DXVECTOR4& sky, D3DXVECTOR4& azimuth)
{
    const auto& skyColour = rage::grmShaderInfo::getShaderParamData(nSkyColorIdx);
    const auto& west = rage::grmShaderInfo::getShaderParamData(nAzimuthColorIdx);
    const auto& east = rage::grmShaderInfo::getShaderParamData(nAzimuthColorEastIdx);
    const float strength = rage::grmShaderInfo::getShaderParamData(nAzimuthStrengthIdx)[0];
    const float azimuthHeight = rage::grmShaderInfo::getShaderParamData(nAzimuthHeightIdx)[0];
    sky = D3DXVECTOR4(skyColour[0], skyColour[1], skyColour[2], 0.0f);
    azimuth = D3DXVECTOR4(0.5f * (west[0] + east[0]) * strength, 0.5f * (west[1] + east[1]) * strength,
                          0.5f * (west[2] + east[2]) * strength, azimuthHeight);
}

// Draws the tables the frame needs into their targets. Leaves the device as it found it.
static bool DrawTables(IDirect3DDevice9* pDevice, float sunHeight, float haze)
{
    auto& R = PostFxResources;
    ID3DXEffect* effect = R.SkyEffect;

    SavedTargets targets(pDevice);
    IDirect3DSurface9* oldTarget1 = nullptr;
    pDevice->GetRenderTarget(1, &oldTarget1);
    pDevice->SetRenderTarget(1, nullptr);
    SavedShaderConstants constants(pDevice);
    SavedEffectPassStates states(pDevice);
    SavedSamplerSlots samplers(pDevice);
    SavedVertexInput vertexInput(pDevice);
    pDevice->SetDepthStencilSurface(nullptr);
    for (const auto& [state, value] : kSSRRenderStates)
        pDevice->SetRenderState(state, value);
    pDevice->SetFVF(kScreenVertexFVF);

    const D3DXVECTOR4 air(haze, kCameraHeight, kGroundAlbedo, kMieG);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Air"), &air);
    const float sunCos = std::sqrt(std::max(0.0f, 1.0f - sunHeight * sunHeight));
    const D3DXVECTOR4 sun(sunCos, 0.0f, sunHeight, 0.0f);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec3Sun"), &sun);
    D3DXVECTOR4 skyColour, azimuth;
    TimecycleSky(skyColour, azimuth);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec3SkyColour"), &skyColour);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Azimuth"), &azimuth);
    const D3DXVECTOR4 match(kPalette, kSunCeiling, kKnee, kCeiling);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Match"), &match);

    auto draw = [&](const char* technique, IDirect3DSurface9* target, float width, float height)
    {
        pDevice->SetRenderTarget(0, target);
        D3DVIEWPORT9 viewport = { 0, 0, DWORD(width), DWORD(height), 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);
        SavedShaders shaders(pDevice);
        UINT passes = 0;
        // By handle: the effect is large address aware, a name would be taken for one.
        effect->SetTechnique(effect->GetTechniqueByName(technique));
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        // s0 the transmittance, s1 the multiple scattering; never the table being drawn.
        SetTextureBoth(pDevice, 0, target == R.SkyTransmittanceSurf ? nullptr : R.SkyTransmittanceTex->mD3DTexture);
        SetTextureBoth(pDevice, 1, target == R.SkyTransmittanceSurf || target == R.SkyMultiScatterSurf ? nullptr : R.SkyMultiScatterTex->mD3DTexture);
        for (DWORD slot = 0; slot < 2; ++slot)
        {
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        BindEffectConstantsOnly(pDevice, effect);
        ScreenQuad::Full(width, height).Draw(pDevice);
        effect->EndPass();
        effect->End();
    };

    if (R.fSkyTablesHaze < 0.0f || std::abs(haze - R.fSkyTablesHaze) > kHazeRedraw * R.fSkyTablesHaze)
    {
        draw("SkyTransmittance", R.SkyTransmittanceSurf, 256.0f, 64.0f);
        draw("SkyMultiScatter", R.SkyMultiScatterSurf, 32.0f, 32.0f);
        R.fSkyTablesHaze = haze;
    }
    draw("SkyView", R.SkyViewSurf, 192.0f, 108.0f);

    pDevice->SetRenderTarget(1, oldTarget1);
    SAFE_RELEASE(oldTarget1);
    return true;
}

// What s10 held before the sky's draw, put back after it.
static IDirect3DBaseTexture9* pSavedTexture = nullptr;
static DWORD savedSamplerStates[6] = {};
static constexpr D3DSAMPLERSTATETYPE kBoundStates[6] =
    { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
static bool bBound = false;

// The aerial perspective's SkyView on s14, from the fog pass to the end of the frame (AerialPerspectiveBind below)
constexpr DWORD kAerialStage = 14;
static IDirect3DBaseTexture9* pAerialSavedTexture = nullptr;
static DWORD aerialSavedStates[6] = {};
static bool bAerialBound = false;

static void SetStrength(IDirect3DDevice9* pDevice, float strength)
{
    // On the game's caching wrapper and on the device behind it, as SetTextureBoth does
    const float value[4] = { strength, 0.0f, 0.0f, 0.0f };
    pDevice->SetPixelShaderConstantF(kStrengthRegister, value, 1);
    if (auto real = RealDevice(pDevice); real && real != pDevice)
        real->SetPixelShaderConstantF(kStrengthRegister, value, 1);
}

// Before one of the sky's draws: draws the frame's tables if not yet drawn, binds SkyView and sets its share.
export void PbrSkyBeforeDraw(IDirect3DDevice9* pDevice)
{
    auto& R = PostFxResources;
    bBound = false;
    if (!pDevice)
        return;
    auto skip = [&](const char* why) { R.szPbrSkyStatus = why; R.fPbrSkyLastStrength = 0.0f; SetStrength(pDevice, 0.0f); };
    if (!R.PbrSkyEnabled())
        return skip("off in the menu");
    if (!R.SkyEffect)
        return skip("no effect");
    if (!bParamsRegistered)
        return skip("sky parameters not registered");
    if (!R.SkyTransmittanceSurf || !R.SkyMultiScatterSurf || !R.SkyViewSurf)
        return skip("no tables");

    // The sky's SunDirection is y up; until the sky has been drawn once it reads zero.
    const auto& sunDirection = rage::grmShaderInfo::getShaderParamData(nSunDirectionIdx);
    const float length = std::sqrt(sunDirection[0] * sunDirection[0] + sunDirection[1] * sunDirection[1] + sunDirection[2] * sunDirection[2]);
    if (length <= 0.0f)
        return skip("SunDirection of the sky reads zero");
    const float sunHeight = sunDirection[1] / length;
    const float strength = Smoothstep(kNightEnd, kNightStart, sunHeight);
    R.fPbrSkyLastSunHeight = sunHeight;
    if (strength <= 0.0f)
        return skip("night, the timecycle's sky");

    const uint32_t frame = FrameHistory::Frame();
    if (R.nSkyViewFrame != frame)
    {
        const float haze = WeatherHaze();
        R.fPbrSkyLastHaze = haze;
        // Never into SkyView while it is bound for the fog
        if (bAerialBound)
            SetTextureBoth(pDevice, kAerialStage, nullptr);
        DrawTables(pDevice, sunHeight, haze);
        if (bAerialBound)
            SetTextureBoth(pDevice, kAerialStage, R.SkyViewTex->mD3DTexture);
        R.nSkyViewFrame = frame;
    }

    pDevice->GetTexture(kSkyViewStage, &pSavedTexture);
    for (size_t i = 0; i < std::size(kBoundStates); ++i)
        pDevice->GetSamplerState(kSkyViewStage, kBoundStates[i], &savedSamplerStates[i]);
    SetTextureBoth(pDevice, kSkyViewStage, R.SkyViewTex->mD3DTexture);
    SetSamplerStateBoth(pDevice, kSkyViewStage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    SetSamplerStateBoth(pDevice, kSkyViewStage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    SetSamplerStateBoth(pDevice, kSkyViewStage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    SetSamplerStateBoth(pDevice, kSkyViewStage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    SetSamplerStateBoth(pDevice, kSkyViewStage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    SetSamplerStateBoth(pDevice, kSkyViewStage, D3DSAMP_SRGBTEXTURE, FALSE);
    SetStrength(pDevice, strength);
    bBound = true;
    ++R.nPbrSkyDraws;
    R.fPbrSkyLastStrength = strength;
    R.szPbrSkyStatus = "drawn";
}

// After it: the share back to 0, so a sky drawn any other way keeps the timecycle's, and s10 as it was.
export void PbrSkyAfterDraw(IDirect3DDevice9* pDevice)
{
    if (!pDevice)
        return;
    SetStrength(pDevice, 0.0f);
    if (!bBound)
        return;
    bBound = false;
    SetTextureBoth(pDevice, kSkyViewStage, pSavedTexture);
    for (size_t i = 0; i < std::size(kBoundStates); ++i)
        SetSamplerStateBoth(pDevice, kSkyViewStage, kBoundStates[i], savedSamplerStates[i]);
    SAFE_RELEASE(pSavedTexture);
}

// For the volumetric clouds (volumetricclouds.ixx), so they take their light from the same air as the sky.

// The share of the table the sky took this frame or the last: 0 while it is off, at night, or no sky was drawn.
export float PbrSkyShare()
{
    auto& R = PostFxResources;
    if (!R.PbrSkyEnabled() || !R.nSkyViewFrame || FrameHistory::Frame() - R.nSkyViewFrame > 1)
        return 0.0f;
    return R.fPbrSkyLastStrength;
}

// What the SkyView table's colour is times on screen, before the exposure: gta_atmoscatt_clouds takes it as it takes
// the timecycle's gradient, times HDRSunExposure.x (sky_pbr.patch), and all of the sky times HDRExposure after that.
export float PbrSkySunExposure()
{
    return bParamsRegistered ? rage::grmShaderInfo::getShaderParamData(nSunExposureIdx)[0] : 0.0f;
}

// The hue of the sunlight reaching a height (km above the sea) with the sun at sunSine above the horizon, through the
// air of SkyAtmosphere.fx at the weather's haze, at brightness 1 (Rec. 709 luma), each channel within 0 to 2. On the
// CPU, as the transmittance table: the clouds need it at one height only. With the sun below that height's horizon,
// the light grazing it.
export bool PbrSkySunHue(float heightKm, float sunSine, float hue[3])
{
    constexpr double kGround = 6360.0, kTop = 6460.0;
    constexpr double kRayleigh[3] = { 5.802e-3, 13.558e-3, 33.1e-3 };
    constexpr double kOzone[3] = { 0.650e-3, 1.881e-3, 0.085e-3 };
    constexpr double kMieExtinction = 4.440e-3;
    constexpr int kSteps = 40;
    const double haze = WeatherHaze();
    const double r = kGround + std::max(double(heightKm), 0.0);
    // Past the planet's edge as seen from that height, the last light that grazes it: the sky's sun goes down at
    // the ground's horizon while the clouds above still catch it, red.
    const double edge = -std::sqrt(std::max(0.0, 1.0 - kGround * kGround / (r * r)));
    const double mu = std::clamp(double(sunSine), edge + 1e-4, 1.0);
    const double b = r * mu;
    const double tMax = -b + std::sqrt(std::max(b * b - (r * r - kTop * kTop), 0.0));
    const double dt = tMax / kSteps;
    double depth[3] = {};
    for (int i = 0; i < kSteps; ++i)
    {
        const double t = (i + 0.5) * dt;
        const double h = std::max(std::sqrt(r * r + t * t + 2.0 * r * mu * t) - kGround, 0.0);
        const double rayleigh = std::exp(-h / 8.0), mie = std::exp(-h / 1.2) * haze;
        const double ozone = std::max(0.0, 1.0 - std::abs(h - 25.0) / 15.0);
        for (int c = 0; c < 3; ++c)
            depth[c] += (kRayleigh[c] * rayleigh + kMieExtinction * mie + kOzone[c] * ozone) * dt;
    }
    double transmittance[3];
    for (int c = 0; c < 3; ++c)
        transmittance[c] = std::exp(-depth[c]);
    const double luma = 0.2126 * transmittance[0] + 0.7152 * transmittance[1] + 0.0722 * transmittance[2];
    if (luma <= 1e-30)
        return false;
    for (int c = 0; c < 3; ++c)
        hue[c] = float(std::clamp(transmittance[c] / luma, 0.0, 2.0));
    return true;
}

// Aerial perspective (Aerial Perspective in the graphics menu) and the fog's colour under the Physical Sky, in every
// shader with the volumetric fog (zz_aerial_perspective.patch): the fog pass for the opaque world and the forward
// shaders drawn after it. From the fog pass to the end of the frame SkyView is on s14 (deferred_lighting's lamp
// shadow cache, read in the lighting before) and c104-c106 say how to take it; at the end of the frame both go back,
// so reflections and anything else drawn before the next fog pass keep the game's fog.

constexpr UINT kAerialRegister = 104;
// The air k times as thick as the Earth's, by the menu's level (Low, Medium, High). Through the real one the city's
// 2 km of air keep 94% of the blue; at k = 4 the 200-800 m a street view reaches lost 3-10% of it, which in game showed
// only in the far distance the game's fog hides anyway. Through 300 m and 1 km of air the blue keeps 91% and 72% at
// k = 10, 76% and 40% at 28, 67% and 27% at 40; the red 94%, 85% and 79% at a kilometre. Medium is 28: in game the
// look between 20 and 40 read best at street level.
constexpr float kAerialScale[] = { 10.0f, 28.0f, 40.0f };
// Aerial Perspective Debug View: the air this much thicker again, to see where it falls and that it is drawn at all.
constexpr float kAerialDebugScale = 25.0f;
constexpr float kRayleighHeightM = 8000.0f, kMieHeightM = 1200.0f;
constexpr float kLog2e = 1.44269504f;

static void SetAerialConstants(IDirect3DDevice9* pDevice, const float (&values)[12])
{
    pDevice->SetPixelShaderConstantF(kAerialRegister, values, 3);
    if (auto real = RealDevice(pDevice); real && real != pDevice)
        real->SetPixelShaderConstantF(kAerialRegister, values, 3);
}

// Before the fog pass: SkyView on s14 and the constants, if the Physical Sky is drawn.
export void AerialPerspectiveBind(IDirect3DDevice9* pDevice)
{
    auto& R = PostFxResources;
    R.fAerialLastShare = R.fAerialLastStrength = 0.0f;
    if (!pDevice || bAerialBound || !bParamsRegistered || !R.SkyViewTex)
        return;
    const float share = PbrSkyShare();
    const float units = PbrSkySunExposure() * rage::grmShaderInfo::getShaderParamData(nExposureIdx)[0];
    if (share <= 0.0f || units <= 0.0f)
        return;

    // The sun seen from above in the world (z up): the sky's SunDirection is y up, (x, -z, y) in the world.
    const auto& sunDirection = rage::grmShaderInfo::getShaderParamData(nSunDirectionIdx);
    float sunX = sunDirection[0], sunY = -sunDirection[2];
    const float flat = std::sqrt(sunX * sunX + sunY * sunY);
    if (flat > 1e-4f)
        sunX /= flat, sunY /= flat;
    else
        sunX = 1.0f, sunY = 0.0f;

    const int level = std::clamp(R.AerialPerspectiveLevel(), 0, int(std::size(kAerialScale)));
    const float strength = level ? share * (R.AerialPerspectiveDebug() ? kAerialDebugScale : 1.0f) : 0.0f;
    const float haze = WeatherHaze();
    const float perMetre = kAerialScale[std::max(level, 1) - 1] * kLog2e / 1000.0f;
    const float values[12] = {
        5.802e-3f * perMetre, 13.558e-3f * perMetre, 33.1e-3f * perMetre, 4.440e-3f * haze * perMetre,
        -kLog2e / kRayleighHeightM, -kLog2e / kMieHeightM, units, strength,
        sunX, sunY, 0.0f, share,
    };

    pDevice->GetTexture(kAerialStage, &pAerialSavedTexture);
    for (size_t i = 0; i < std::size(kBoundStates); ++i)
        pDevice->GetSamplerState(kAerialStage, kBoundStates[i], &aerialSavedStates[i]);
    SetTextureBoth(pDevice, kAerialStage, R.SkyViewTex->mD3DTexture);
    SetSamplerStateBoth(pDevice, kAerialStage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    SetSamplerStateBoth(pDevice, kAerialStage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    SetSamplerStateBoth(pDevice, kAerialStage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    SetSamplerStateBoth(pDevice, kAerialStage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    SetSamplerStateBoth(pDevice, kAerialStage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    SetSamplerStateBoth(pDevice, kAerialStage, D3DSAMP_SRGBTEXTURE, FALSE);
    SetAerialConstants(pDevice, values);
    bAerialBound = true;
    R.fAerialLastShare = share;
    R.fAerialLastStrength = strength;
    R.fAerialLastUnits = units;
    R.fAerialLastHaze = haze;
}

// At the end of the frame: the fog back to the game's and s14 as it was.
export void AerialPerspectiveEndFrame(IDirect3DDevice9* pDevice)
{
    if (!pDevice || !bAerialBound)
        return;
    bAerialBound = false;
    static const float none[12] = {};
    SetAerialConstants(pDevice, none);
    SetTextureBoth(pDevice, kAerialStage, pAerialSavedTexture);
    for (size_t i = 0; i < std::size(kBoundStates); ++i)
        SetSamplerStateBoth(pDevice, kAerialStage, kBoundStates[i], aerialSavedStates[i]);
    SAFE_RELEASE(pAerialSavedTexture);
}
