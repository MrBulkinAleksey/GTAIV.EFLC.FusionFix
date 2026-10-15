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
// bright side towards the sun rolls off from 1.2 to at most 2 times the timecycle's colour there.
constexpr float kPalette = 0.35f;
constexpr float kKnee = 1.2f;
constexpr float kCeiling = 2.0f;
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
              nSunDirectionIdx = 0;
static bool bParamsRegistered = false;

export void RegisterPbrSkyParams()
{
    nSkyColorIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "SkyColor");
    nAzimuthColorIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "AzimuthColor");
    nAzimuthColorEastIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "AzimuthColorEast");
    nAzimuthStrengthIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "AzimuthStrength");
    nAzimuthHeightIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "AzimuthHeight");
    nSunDirectionIdx = rage::grmShaderInfo::registerShaderParam("gta_atmoscatt_clouds.fxc", "SunDirection");
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
    const D3DXVECTOR4 match(kPalette, 0.0f, kKnee, kCeiling);
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
        DrawTables(pDevice, sunHeight, haze);
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
