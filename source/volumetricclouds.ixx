module;

#include <common.hxx>
#include <d3dx9tex.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <regex>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include "FusionLog.hpp"

export module volumetricclouds;

import common;
import comvars;
import d3dx9_43;
import framegeneration;
import framehistory;
import hdr;
import natives;
import pbrsky;
import settings;
import shaders;
import renderscale;
import renderskip;
import temporal;
import upscaler;
import postfxprofiler;
import postfxresources;
import screenquad;
import ssrtrace;
import postfxpass;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;
using namespace PostFxProfiler;
using namespace PostFxPass;

// Volumetric clouds (Clouds.fx) over the sky, in the fog pass and in reflections.

static constexpr struct { D3DRENDERSTATETYPE state; DWORD value; } kCloudRenderStates[] =
{
    { D3DRS_ZENABLE,          FALSE },
    { D3DRS_ZWRITEENABLE,     FALSE },
    // For the reflections, which test against the sky's depth (far) instead.
    { D3DRS_ZFUNC,            D3DCMP_LESSEQUAL },
    { D3DRS_ALPHATESTENABLE,  FALSE },
    { D3DRS_STENCILENABLE,    FALSE },
    { D3DRS_FOGENABLE,        FALSE },
    { D3DRS_CLIPPING,         FALSE },
    { D3DRS_SCISSORTESTENABLE, FALSE },
    { D3DRS_CULLMODE,         D3DCULL_NONE },
    { D3DRS_SRGBWRITEENABLE,  FALSE },
    { D3DRS_ALPHABLENDENABLE, TRUE },
    { D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
    { D3DRS_BLENDOP,          D3DBLENDOP_ADD },
    // The cloud's light plus what shows through it; RGB only, the scene alpha stays.
    { D3DRS_SRCBLEND,         D3DBLEND_ONE },
    { D3DRS_DESTBLEND,        D3DBLEND_SRCALPHA },
    { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE },
    // The march's second target.
    { D3DRS_COLORWRITEENABLE1, 0xF },
};

// The reflections' half size target, made for the reflection's viewport and made again when that changes size
static bool ReflectionHalfTarget(IDirect3DDevice9* pDevice, float width, float height)
{
    auto& R = PostFxResources;
    const UINT w = (std::max)(1u, UINT(width) / 2), h = (std::max)(1u, UINT(height) / 2);
    static uint32_t uses = 0;
    uses++;
    PostFxResource::CloudReflectionHalfTarget* slot = nullptr;
    for (auto& target : R.CloudReflectionHalf)
        if (target.surf && target.width == w && target.height == h)
            slot = &target;
    if (!slot)
    {
        // The emptiest or least lately used slot is made again; made on the real device, past the game's books,
        // as FusionFix's other textures of its own
        slot = &R.CloudReflectionHalf[0];
        for (auto& target : R.CloudReflectionHalf)
            if (!target.surf || target.used < slot->used)
                slot = &target;
        SAFE_RELEASE(slot->surf);
        SAFE_RELEASE(slot->tex);
        *slot = {};
        auto device = RageDirect3DDevice9::RealDevice(pDevice);
        if (FAILED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &slot->tex, nullptr)) || !slot->tex)
        {
            slot->tex = nullptr;
            R.bVolumetricCloudsReflectionsHalfRes = false;
            return false;
        }
        slot->tex->GetSurfaceLevel(0, &slot->surf);
        slot->width = w;
        slot->height = h;
    }
    slot->used = uses;
    R.CloudReflectionHalfTex = slot->tex;
    R.CloudReflectionHalfSurf = slot->surf;
    return slot->surf != nullptr;
}

// Blends the volumetric clouds (Clouds.fx) into the lit scene, from the fog pass before it reads
// the scene; with reflection, into the reflection map's target right after its sky
// (DrawSkyReflection), at full size, on the sky only by the depth test, at the brightness of
// that simpler sky. Leaves the device as it found it.
export void RenderVolumetricClouds(IDirect3DDevice9* pDevice, IDirect3DBaseTexture9* sceneBase, bool reflection = false,
                                   int reflectionSection = kProfCloudReflectionMap)
{
    ProfilerScope timedAll(pDevice, reflection ? kProfCloudReflection : -1);
    ProfilerScope timed(pDevice, reflection ? reflectionSection : kProfClouds);
    auto& R = PostFxResources;
    auto skip = [&](const char* why) { (reflection ? R.szCloudsReflectionStatus : R.szCloudsStatus) = why; };
    if (!R.VolumetricCloudsEnabled())
        return skip("off in the ini");
    if (!R.CloudsEffect)
        return skip("no effect");
    if (!R.bCloudParamsRegistered)
        return skip("cloud parameters not registered");
    if (!sceneBase && !reflection)
        return skip("no scene texture");
    if (reflection && !R.bVolumetricCloudsReflections)
        return skip("VolumetricCloudsReflections 0");
    // At night the clouds are black against a black sky and their reflections showed nothing, at some 2.6 GPU
    // milliseconds a frame in a profile: while the sun is low (RenderSkip's SunShadowsMinElevation, or 22:00 to 05:00)
    // the reflections keep the game's sky
    if (reflection && !R.bVolumetricCloudsReflectionsAtNight && RenderSkip::SunIsLow())
        return skip("night, VolumetricCloudsReflectionsAtNight 0");
    if (!R.mDepthRT || !R.mDepthRT->mD3DTexture)
        return skip("no depth texture");
    rage::grcViewport* vp = rage::GetCurrentViewport();
    if (!vp)
        return skip("no viewport");

    // The game's clouds: until the sky has been drawn once these read zero.
    // gta_atmoscatt_clouds colours its clouds CloudColor, brightened by CloudInscatteringRange
    // towards the sun, less the cloud's own shadow, plus SunsetColor where the sun lights them.
    // TopCloudColor belongs to its separate high layer and tinted these clouds cyan.
    const auto& cloudColour = rage::grmShaderInfo::getShaderParamData(R.CloudColorIdx);
    const auto& sunsetColour = rage::grmShaderInfo::getShaderParamData(R.SunsetColorIdx);
    const float inscattering = rage::grmShaderInfo::getShaderParamData(R.CloudInscatteringIdx)[0];
    const auto& sunDirection = rage::grmShaderInfo::getShaderParamData(R.CloudSunDirectionIdx);
    float exposure = rage::grmShaderInfo::getShaderParamData(R.CloudExposureIdx)[0] * R.fVolumetricCloudsBrightness;
    if (exposure <= 0.0f)
        return skip("HDRExposure of the sky reads zero");
    // The reflection map's sky (the sky's 0x40000 branch) takes the timecycle's colours without the
    // HDR exposure. The water's reflection draws its sky through the main branch, exposed as the
    // scene's: with the reflection map's exposure its clouds came out some 30 times too dark, black.
    if (reflection && reflectionSection != kProfCloudReflectionWater)
        exposure = R.fVolumetricCloudsBrightness * R.fVolumetricCloudsReflectionBrightness;
    else if (reflection)
        exposure *= R.fVolumetricCloudsReflectionBrightness;
    // The sky's SunDirection is y up; the world is z up.
    D3DXVECTOR4 sun(sunDirection[0], -sunDirection[2], sunDirection[1], 0.0f);
    const float sunLength = std::sqrt(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    if (sunLength <= 0.0f)
        return skip("SunDirection of the sky reads zero");
    sun /= sunLength;
    const D3DXVECTOR4 skySun = sun;
    // Below the horizon the sun hands the clouds over to the moon. The sun still lights their
    // bellies a little way below it, at dusk; then its light fades out, and the moon's fades in
    // from the same zero, so the light never jumps from one to the other at full strength.
    // MoonPosition is the direction to the moon, y up like SunDirection.
    float lightStrength = 1.0f;
    bool moonlit = false;
    {
        auto smoothstep = [](float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
        const float day = smoothstep(-0.15f, -0.05f, sun.z);
        const auto& moonPosition = rage::grmShaderInfo::getShaderParamData(R.CloudMoonPositionIdx);
        D3DXVECTOR4 moon(moonPosition[0], -moonPosition[2], moonPosition[1], 0.0f);
        const float moonLength = std::sqrt(moon.x * moon.x + moon.y * moon.y + moon.z * moon.z);
        moon.x *= R.CloudSkyAxisSign[0];
        moon.y *= R.CloudSkyAxisSign[1];
        if (day >= 0.5f || moonLength <= 0.0f)
            lightStrength = day >= 0.5f ? day * 2.0f - 1.0f : 0.0f;
        else
        {
            moon /= moonLength;
            sun = moon;
            moonlit = true;
            lightStrength = (1.0f - day * 2.0f) * smoothstep(0.0f, 0.1f, moon.z) * R.fVolumetricCloudsMoonlight;
        }
    }

    auto coverage = R.CloudNoiseTex();
    auto detail = R.CloudDetailTex();
    if (!coverage)
        return skip("no coverage texture");
    if (!detail)
        return skip("no detail texture");

    IDirect3DSurface9* sceneSurface = nullptr;
    if (reflection)
        pDevice->GetRenderTarget(0, &sceneSurface);
    else
    {
        IDirect3DTexture9* scene = nullptr;
        if (FAILED(sceneBase->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&scene))) || !scene)
            return skip("the scene is no 2D texture");
        scene->GetSurfaceLevel(0, &sceneSurface);
        scene->Release();
    }
    if (!sceneSurface)
        return skip("no scene surface");
    D3DSURFACE_DESC desc = {};
    sceneSurface->GetDesc(&desc);
    float width = float(desc.Width), height = float(desc.Height);
    // A reflection is drawn through its own viewport, which may be a part of its target (a map
    // drawn in parts, as two halves of one texture): the clouds go into that part only, the
    // rays from its own corner. Insisting on the whole target left the water with no clouds.
    float originX = 0.0f, originY = 0.0f;
    if (reflection)
    {
        D3DVIEWPORT9 current = {};
        pDevice->GetViewport(&current);
        R.CloudReflectionViewport = current;
        R.CloudReflectionTarget[0] = desc.Width;
        R.CloudReflectionTarget[1] = desc.Height;
        if (current.Width == 0 || current.Height == 0 || current.X + current.Width > desc.Width || current.Y + current.Height > desc.Height)
        {
            sceneSurface->Release();
            return skip("viewport outside its target");
        }
        originX = float(current.X);
        originY = float(current.Y);
        width = float(current.Width);
        height = float(current.Height);
    }
    skip("drawn");

    ID3DXEffect* effect = R.CloudsEffect;
    const D3DMATRIX& proj = *(const D3DMATRIX*)vp->mProjectionMatrix;
    const D3DXVECTOR4 projInfo = ProjInfo(proj, width, height);
    const float invViewportSize[] = { 1.0f / width, 1.0f / height };
    effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec2InvViewportSize"), invViewportSize, 2);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4ProjInfo"), &projInfo);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fNearPlane"), vp->mNearClip);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fFarDivNear"), vp->mFarClip / vp->mNearClip);

    const D3DXMATRIX& viewInv = *(const D3DXMATRIX*)vp->mViewInverseMatrix;
    D3DXVECTOR4 toView[3];
    WorldToViewRows(vp, toView);
    const D3DXVECTOR4 worldX(toView[0].x, toView[1].x, toView[2].x, viewInv.m[3][0]);
    const D3DXVECTOR4 worldY(toView[0].y, toView[1].y, toView[2].y, viewInv.m[3][1]);
    const D3DXVECTOR4 worldZ(toView[0].z, toView[1].z, toView[2].z, viewInv.m[3][2]);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4WorldX"), &worldX);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4WorldY"), &worldY);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4WorldZ"), &worldZ);

    if (!reflection)
        std::memcpy(R.CloudLastSkySun, &sun.x, sizeof(R.CloudLastSkySun));
    // By day the game's own directional light, taken in the lighting phase of this frame or the
    // last: the sky's SunDirection, remapped from its y up space, lit the clouds from the
    // mirror image of the sun across the sky, their far sides lit and the near ones dark.
    const bool gameLight = !moonlit && R.nCloudLightFrame && FrameHistory::Frame() - R.nCloudLightFrame <= 2 && R.CloudLightDir[2] > 0.0f;
    if (gameLight)
    {
        // Learn the sky's axis signs while both directions are clear of the zenith.
        for (int axis = 0; axis < 2; ++axis)
        {
            const float skyAxis = (&sun.x)[axis], gameAxis = R.CloudLightDir[axis];
            if (std::fabs(skyAxis) > 0.2f && std::fabs(gameAxis) > 0.2f)
                R.CloudSkyAxisSign[axis] = (skyAxis > 0.0f) == (gameAxis > 0.0f) ? 1.0f : -1.0f;
        }
        // Its way round, at the height of the sun the sky draws: the game holds its light high, at 30
        // degrees with the sky's sun on the horizon at 20:00, and the clouds of a sunset were lit, and
        // took the sun's colour, as at midday.
        const float flat = std::sqrt(R.CloudLightDir[0] * R.CloudLightDir[0] + R.CloudLightDir[1] * R.CloudLightDir[1]);
        const float across = std::sqrt((std::max)(0.0f, 1.0f - skySun.z * skySun.z));
        sun = flat > 1e-3f ? D3DXVECTOR4(R.CloudLightDir[0] / flat * across, R.CloudLightDir[1] / flat * across, skySun.z, 0.0f)
                           : D3DXVECTOR4(R.CloudLightDir[0], R.CloudLightDir[1], R.CloudLightDir[2], 0.0f);
    }
    if (!reflection)
        std::memcpy(R.CloudLastUsedSun, &sun.x, sizeof(R.CloudLastUsedSun));
    // The sun the physical sky's table was drawn about, in the world: the game's light's way round at the sky sun's
    // height by day, else the sky's sun with the axis signs learnt from it.
    const D3DXVECTOR4 tableSun = gameLight ? sun : D3DXVECTOR4(skySun.x * R.CloudSkyAxisSign[0], skySun.y * R.CloudSkyAxisSign[1], skySun.z, 0.0f);
    // How much of their light the clouds take from the physical sky (pbrsky.ixx), as much as the sky took of it.
    const float pbrShare = PbrSkyShare();
    // VolumetricCloudsDebug 15: the sun straight overhead, to test the sun's direction.
    if (R.VolumetricCloudsDebug() == 15)
        sun = D3DXVECTOR4(0.0f, 0.0f, 1.0f, 0.0f);
    effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec3SunDir"), &sun.x, 3);
    float litColour[3], shadeColour[3], sunsetLit[3];
    for (int i = 0; i < 3; ++i)
    {
        litColour[i] = cloudColour[i] * R.kCloudLitGain * exposure;
        shadeColour[i] = cloudColour[i] * R.fVolumetricCloudsShade * exposure;
        sunsetLit[i] = sunsetColour[i] * exposure;
    }
    // Matched to the sky in the scene behind them, while the march has targets of its own to draw
    // into and can read the scene; the reflections keep the game's cloud colour.
    const bool canReadScene = !reflection && R.CloudSurf[0] && R.CloudSurf[1] && R.CloudSurf[2] && R.CloudSkyRefSurf &&
                              R.CloudMarchSurf[0] && R.CloudMarchSurf[1];
    // The shaded side is lit by the sky above it rather than the sun: it takes the sky's hue at its
    // own brightness, by VolumetricCloudsSkyLight, and the sunlit side some of it too. The hue of
    // the sky on screen (CloudsSkyRef, in Clouds.fx), the reflections that of the frame before:
    // the game's SkyColor stays bluish under a pink evening sky, and the clouds came out white
    // against it. SkyColor's hue only while there is no sky colour of a recent frame.
    // The lit side's share of it goes with the physical sky's share: under its blue the sunlit sides
    // came out a grey blue. They take the sun's colour, the sky lights only the shade.
    const float litSkyHue = 0.4f * (1.0f - pbrShare);
    const bool skyRefRecent = R.CloudSkyRefTex && R.nCloudSkyRefFrame && FrameHistory::Frame() - R.nCloudSkyRefFrame <= 2;
    {
        const float skyHue[3] = { skyRefRecent ? R.fVolumetricCloudsSkyLight : 0.0f, skyRefRecent ? litSkyHue : 0.0f,
                                  R.fVolumetricCloudsSaturation };
        effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec3SkyHue"), skyHue, 3);
    }
    if (!skyRefRecent)
    {
        const auto& skyColour = rage::grmShaderInfo::getShaderParamData(R.CloudSkyColorIdx);
        const float skyLuma = 0.2126f * skyColour[0] + 0.7152f * skyColour[1] + 0.0722f * skyColour[2];
        const float shadeLuma = 0.2126f * shadeColour[0] + 0.7152f * shadeColour[1] + 0.0722f * shadeColour[2];
        if (skyLuma > 1e-4f)
            for (int i = 0; i < 3; ++i)
                shadeColour[i] += ((std::max)(skyColour[i], 0.0f) / skyLuma * shadeLuma - shadeColour[i]) * R.fVolumetricCloudsSkyLight;
    }
    if (!skyRefRecent)
    {
        const auto& skyColour = rage::grmShaderInfo::getShaderParamData(R.CloudSkyColorIdx);
        const float skyLuma = 0.2126f * skyColour[0] + 0.7152f * skyColour[1] + 0.0722f * skyColour[2];
        const float litLuma = 0.2126f * litColour[0] + 0.7152f * litColour[1] + 0.0722f * litColour[2];
        if (skyLuma > 1e-4f)
            for (int i = 0; i < 3; ++i)
                litColour[i] += ((std::max)(skyColour[i], 0.0f) / skyLuma * litLuma - litColour[i]) * litSkyHue;
    }
    // VolumetricCloudsSaturation, about each colour's luma.
    for (float* colour : { litColour, shadeColour, sunsetLit })
    {
        const float luma = 0.2126f * colour[0] + 0.7152f * colour[1] + 0.0722f * colour[2];
        for (int i = 0; i < 3; ++i)
            colour[i] = (std::max)(luma + (colour[i] - luma) * R.fVolumetricCloudsSaturation, 0.0f);
    }
    // The ceiling the brightest cloud rolls off towards: room above the silver lining's peak for
    // the glow next to the sun. gta_atmoscatt_clouds clamps the sky and its clouds to
    // HDRExposureClamp unless FusionFix's volumetric fog or SkyHDR is on, and past that clamp our
    // clouds turned white while the sky around them stayed at it, so then the ceiling is the
    // clamp.
    float ceiling = 0.0f;
    {
        static auto volumetricFog = FusionFixSettings.GetRef("PREF_VOLUMETRICFOG");
        const auto& clamp = rage::grmShaderInfo::getShaderParamData(R.CloudExposureClampIdx);
        const float brightest = (std::max)({ litColour[0], litColour[1], litColour[2] });
        // Matched to the sky the lit side is VolumetricCloudsSkyMatch times its brightness, and the
        // rims and the glow next to the sun roll off below three times that.
        ceiling = brightest * (R.fVolumetricCloudsSkyMatch > 0.0f ? 3.0f : (1.0f + inscattering) * 1.5f);
        const float clampMin = (std::min)({ clamp[0], clamp[1], clamp[2] });
        if (!reflection && !(volumetricFog && volumetricFog->get()) && !bSkyHDR && clampMin > 0.0f)
            ceiling = (std::min)(ceiling, clampMin);
        ceiling = (std::max)(ceiling, 1e-3f);
        if (!reflection)
        {
            std::memcpy(R.CloudLastLit, litColour, sizeof(litColour));
            std::memcpy(R.CloudLastShade, shadeColour, sizeof(shadeColour));
            std::memcpy(R.CloudLastClamp, clamp.data(), sizeof(R.CloudLastClamp));
            R.CloudLastCeiling = ceiling;
            R.CloudLastLightStrength = lightStrength;
            R.bCloudLastMoonlit = moonlit;
        }
    }
    effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec3LitColour"), litColour, 3);
    effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec3ShadeColour"), shadeColour, 3);
    effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec3SunsetColour"), sunsetLit, 3);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fSilver"), inscattering);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fCeiling"), ceiling);
    {
        const float litLuma = 0.2126f * litColour[0] + 0.7152f * litColour[1] + 0.0722f * litColour[2];
        // Under the physical sky twice as bright against it, by its share: matched to its whole dome, a darker
        // blue than the timecycle's teal on screen, the clouds at twice its brightness came out grey on it.
        const float skyMatch = R.fVolumetricCloudsSkyMatch * R.Cloud.skyMatch * (1.0f + pbrShare);
        effect->SetFloat(effect->GetParameterByName(nullptr, "fSkyMatch"), canReadScene && litLuma > 1e-4f ? skyMatch / litLuma : 0.0f);
    }
    effect->SetFloat(effect->GetParameterByName(nullptr, "fLightStrength"), lightStrength);
    // How high the sun (or the moon) stands: 0 up to 5 degrees, 1 from 35 up. With it low, the clouds
    // towards it are lit from behind: their bodies darker than the sky, down to 0.3 of their light
    // straight towards it, and the glow of their edges three times as strong. Through the day
    // nothing changes; nor across the sky from a low sun, where the clouds face its light. The
    // least of the light inside them stays: lowered with the sun, it darkened the sides the sun
    // lights too, grey where they should take its colour.
    effect->SetFloat(effect->GetParameterByName(nullptr, "fMinLight"), R.fVolumetricCloudsMinLight);
    {
        const float lowSun = std::clamp((sun.z - 0.0872f) / (0.5736f - 0.0872f), 0.0f, 1.0f);
        const float day = lowSun * lowSun * (3.0f - 2.0f * lowSun);
        effect->SetFloat(effect->GetParameterByName(nullptr, "fBacklight"), 0.3f + 0.7f * day);
        effect->SetFloat(effect->GetParameterByName(nullptr, "fGlowBoost"), 1.0f + 2.0f * (1.0f - day));
    }
    effect->SetFloat(effect->GetParameterByName(nullptr, "fMottle"), R.fVolumetricCloudsMottle);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fDetailFlow"), R.fVolumetricCloudsDetailFlow);
    {
        const D3DXVECTOR4 style(float(R.nVolumetricCloudsStyle), R.fVolumetricCloudsStyleCut, R.fVolumetricCloudsStyleDetail, R.fVolumetricCloudsStyleBase);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4Style"), &style);
        const float styleSoft[2] = { R.fVolumetricCloudsStyleSoftBase, R.fVolumetricCloudsStyleSoftTop };
        effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec2StyleSoft"), styleSoft, 2);
        const D3DXVECTOR4 styleLight(float(R.nVolumetricCloudsStyleLight), R.fVolumetricCloudsStyleSun, R.fVolumetricCloudsStyleAmbient, R.fVolumetricCloudsStylePhase);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4StyleLight"), &styleLight);
    }
    // The sun's hue at brightness 1 (Rec. 709 luma), each channel kept within 0 to 2, mixed
    // towards white by VolumetricCloudsSunTint. The moon's is a cool white. With the physical sky,
    // the sunlight that gets through its air to the middle of the layer, by its share.
    {
        static const float moonColour[3] = { 0.85f, 0.95f, 1.2f };
        const float* sunColour = moonlit ? moonColour : rage::grmShaderInfo::getShaderParamData(R.CloudSunColorIdx).data();
        const float luma = 0.2126f * sunColour[0] + 0.7152f * sunColour[1] + 0.0722f * sunColour[2];
        float hue[3] = { 1.0f, 1.0f, 1.0f };
        if (luma > 1e-4f)
            for (int i = 0; i < 3; ++i)
                hue[i] = std::clamp(sunColour[i] / luma, 0.0f, 2.0f);
        float pbrHue[3];
        if (!moonlit && pbrShare > 0.0f && PbrSkySunHue((R.Cloud.base + 0.5f * R.Cloud.thickness) / 1000.0f, sun.z, pbrHue))
            for (int i = 0; i < 3; ++i)
                hue[i] += (pbrHue[i] - hue[i]) * pbrShare;
        // Under the physical sky most of the air's colour: at the game's 0.3 a sunset's clouds took
        // a pale peach for its orange.
        const float sunTint = R.fVolumetricCloudsSunTint + (0.7f - R.fVolumetricCloudsSunTint) * pbrShare;
        float tint[3];
        for (int i = 0; i < 3; ++i)
            tint[i] = 1.0f + (hue[i] - 1.0f) * sunTint;
        effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec3SunTint"), tint, 3);
        // The glow is the sunlight straight through the cloud's edges: it takes the sun's hue
        // whole, gold in the evening, whatever VolumetricCloudsSunTint does to the lit sides.
        float glow[3];
        for (int i = 0; i < 3; ++i)
            glow[i] = litColour[i] * hue[i];
        effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec3GlowColour"), glow, 3);
        if (!reflection)
            std::memcpy(R.CloudLastSunHue, hue, sizeof(hue));
    }
    // The physical sky's SkyView table (s10) on screen: times HDRSunExposure as the sky takes it and the exposure, held
    // to the sky's clamp where the sky is. CloudsSkyRef takes the sky's colour from it, and CloudsLight the sky the
    // haze fades distant cloud into, each by the share.
    {
        static auto volumetricFog = FusionFixSettings.GetRef("PREF_VOLUMETRICFOG");
        const float units = PbrSkySunExposure() * rage::grmShaderInfo::getShaderParamData(R.CloudExposureIdx)[0];
        const bool tableUsed = !reflection && pbrShare > 0.0f && units > 0.0f && R.SkyViewTex;
        const D3DXVECTOR4 skyTable(tableUsed ? pbrShare : 0.0f, units, 0.0f, 0.0f);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4SkyTable"), &skyTable);
        const auto& clamp = rage::grmShaderInfo::getShaderParamData(R.CloudExposureClampIdx);
        const bool clamped = !(volumetricFog && volumetricFog->get()) && !bSkyHDR && (std::min)({ clamp[0], clamp[1], clamp[2] }) > 0.0f;
        const float tableClamp[3] = { clamped ? clamp[0] : 1e6f, clamped ? clamp[1] : 1e6f, clamped ? clamp[2] : 1e6f };
        effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec3SkyTableClamp"), tableClamp, 3);
        effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec3TableSun"), &tableSun.x, 3);
        if (!reflection)
        {
            R.CloudLastPbrShare = skyTable.x;
            R.CloudLastSkyUnits = units;
        }
    }
    const D3DXVECTOR4 layer(R.Cloud.base, R.Cloud.thickness, 1.0f / R.fCloudShadowsScale, R.Cloud.coverage);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Layer"), &layer);
    // The detail drifts with the wind too, half as fast, so the billows change as they go.
    const D3DXVECTOR4 wind(R.fCloudWindX, R.fCloudWindY, float(std::fmod(R.fCloudDetailDrift * 0.93, 1.0)),
                           float(std::fmod(R.fCloudDetailDrift * 0.37, 1.0)));
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Wind"), &wind);
    const D3DXVECTOR4 shape(R.fVolumetricCloudsDensity * R.Cloud.density, 1.0f / R.fVolumetricCloudsDetailScale,
                            (std::min)(R.fVolumetricCloudsDetail * R.Cloud.detail, 1.5f),
                            R.fVolumetricCloudsHaze * (1.0f - 0.6f * R.Cloud.stratus));
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Shape"), &shape);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fStratus"), R.Cloud.stratus);
    // The density's soft compressor, d (1 + k) / (1 + k d), its k from the base to the top: 3 to 12,
    // or 1 to 3 where the weather wants smoky edges.
    {
        const float compress[2] = { 3.0f - 2.0f * R.Cloud.softness, 12.0f - 9.0f * R.Cloud.softness };
        effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec2Compress"), compress, 2);
    }
    effect->SetFloat(effect->GetParameterByName(nullptr, "fEvolution"), float(std::fmod(R.fCloudEvolution, 1.0)));
    effect->SetFloat(effect->GetParameterByName(nullptr, "fTranslucency"), (std::min)(R.fVolumetricCloudsTranslucency * R.Cloud.translucency, 0.9f));
    effect->SetFloat(effect->GetParameterByName(nullptr, "fLightAbsorption"), R.fVolumetricCloudsAbsorption * R.Cloud.absorption);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fGlow"), R.Cloud.glow);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fSunPower"), R.Cloud.sunPower);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fBaseRound"), R.Cloud.baseRound);
    // How far the billows are swept along the coarse noise at the tops, in their own size: more
    // in the wind. Up to 3 in a strong wind they drew out into parallel brush strokes.
    effect->SetFloat(effect->GetParameterByName(nullptr, "fCurl"), std::clamp(0.8f + 0.4f * R.Cloud.wind, 1.0f, 1.8f));
    // The wind's shear: a heap's top lies up to 120 m downwind of its base at the weather's
    // wind, at most 200 m; at 200 m, up to 300, the tops trailed off in streaks. The wind carries the map along (0.93, 0.37), so the clouds drift the
    // other way and their tops lean that way.
    {
        const float lean = (std::min)(120.0f * R.Cloud.wind, 200.0f) / R.fCloudShadowsScale;
        const float shear[2] = { 0.9293f * lean, 0.3697f * lean };
        effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec2Shear"), shear, 2);
    }
    effect->SetFloat(effect->GetParameterByName(nullptr, "fDebug"), float(R.VolumetricCloudsDebug()));
    // The golden ratio's fraction per frame: each frame's march noise falls between the last
    // ones', and temporal anti-aliasing averages it away.
    effect->SetFloat(effect->GetParameterByName(nullptr, "fFrameJitter"), static_cast<float>(std::fmod(FrameHistory::Frame() * 0.6180339887, 1.0)));
    // The outline wanders by up to 150 metres.
    effect->SetFloat(effect->GetParameterByName(nullptr, "fWarp"), 150.0f / R.fCloudShadowsScale);
    const D3DXVECTOR4 morph(R.CloudMorphPhase(), R.kCloudMorphReach / R.fCloudShadowsScale, R.kCloudWeatherScale, R.kCloudWeatherReach);
    effect->SetVector(effect->GetParameterByName(nullptr, "vec4Morph"), &morph);
    effect->SetFloat(effect->GetParameterByName(nullptr, "fMaxDistance"), R.fVolumetricCloudsMaxDistance);

    IDirect3DSurface9* oldTarget = nullptr;
    IDirect3DSurface9* oldTarget1 = nullptr;
    IDirect3DSurface9* oldDepth = nullptr;
    IDirect3DPixelShader9* oldPS = nullptr;
    IDirect3DVertexShader9* oldVS = nullptr;
    IDirect3DBaseTexture9* oldTextures[11] = {};
    D3DVIEWPORT9 oldViewport = {};
    DWORD savedRenderStates[std::size(kCloudRenderStates)] = {};
    static constexpr D3DSAMPLERSTATETYPE kSamplerStates[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW,
                                                               D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
                                                               D3DSAMP_SRGBTEXTURE };
    DWORD savedSamplerStates[11][std::size(kSamplerStates)] = {};

    pDevice->GetRenderTarget(0, &oldTarget);
    pDevice->GetRenderTarget(1, &oldTarget1);
    pDevice->GetDepthStencilSurface(&oldDepth);
    pDevice->GetViewport(&oldViewport);
    SavedVertexInput savedVertexInput(pDevice);
    pDevice->GetPixelShader(&oldPS);
    pDevice->GetVertexShader(&oldVS);
    pDevice->GetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
    pDevice->GetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
    for (DWORD slot = 0; slot < 11; ++slot)
    {
        pDevice->GetTexture(slot, &oldTextures[slot]);
        for (size_t i = 0; i < std::size(kSamplerStates); ++i)
            pDevice->GetSamplerState(slot, kSamplerStates[i], &savedSamplerStates[slot][i]);
    }
    for (size_t i = 0; i < std::size(kCloudRenderStates); ++i)
    {
        pDevice->GetRenderState(kCloudRenderStates[i].state, &savedRenderStates[i]);
        pDevice->SetRenderState(kCloudRenderStates[i].state, kCloudRenderStates[i].value);
    }

    // The reflections keep their depth buffer: the quad at the far plane passes the depth test
    // only where nothing but sky was drawn, whether before or after the sky.
    if (reflection)
        pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    else
        pDevice->SetDepthStencilSurface(nullptr);
    pDevice->SetStreamSource(0, nullptr, 0, 0);
    pDevice->SetVertexDeclaration(nullptr);
    pDevice->SetFVF(kScreenVertexFVF);
    const float quadZ = reflection ? 1.0f : 0.0f;

    // Half size, accumulated, laid over the scene at full size; one full size pass straight into
    // the scene while the half size targets are missing, and for the reflections.
    const bool halfSize = !reflection && R.CloudSurf[0] && R.CloudSurf[1] && R.CloudSurf[2] && R.CloudMarchSurf[0] && R.CloudMarchSurf[1];
    const int prevAccum = 1 + R.nCloudAccumIndex;
    const int nextAccum = 1 + (R.nCloudAccumIndex ^ 1);
    D3DSURFACE_DESC halfDesc = {};
    if (halfSize)
        R.CloudSurf[0]->GetDesc(&halfDesc);
    const float halfWidth = halfSize ? float(halfDesc.Width) : width, halfHeight = halfSize ? float(halfDesc.Height) : height;
    const float depthTexel[2] = { 1.0f / width, 1.0f / height };
    effect->SetFloatArray(effect->GetParameterByName(nullptr, "vec2DepthTexel"), depthTexel, 2);

    // Last frame's view projection by columns: a direction (w 0) leaves its translation out.
    const bool history = halfSize && FrameHistory::CanReproject(R.nCloudAccumFrame);
    {
        const D3DXMATRIX& m = FrameHistory::Previous().ViewProjectionNoJitter;
        const D3DXVECTOR4 prevX(m._11, m._21, m._31, 0.0f), prevY(m._12, m._22, m._32, 0.0f), prevW(m._14, m._24, m._34, 0.0f);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4PrevX"), &prevX);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4PrevY"), &prevY);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4PrevW"), &prevW);
        // A sixth of each frame: with the march's offset moving on every frame, its noise
        // averages out over about a dozen frames.
        const D3DXVECTOR4 historyInfo(history ? 1.0f : 0.0f, 0.15f, 1.0f / halfWidth, 1.0f / halfHeight);
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4History"), &historyInfo);
    }

    IDirect3DTexture9* reflectionHalf = nullptr;    // while CloudsUpscale lays it over the reflection
    auto bindTextures = [&](bool readScene, bool readSkyRef, bool readMarch)
    {
        // The samplers have fixed registers: s0 depth, s1 coverage, s2 detail, s3 this frame's
        // half size clouds, s4 the history, s5 the accumulated clouds, s6 the scene, for the sky's
        // brightness, never while drawing into it, s7 that brightness, s8 and s9 the march's sums, s10 the
        // physical sky's SkyView table.
        // The reflections have no depth texture of their own: none reads as sky everywhere.
        SetTextureBoth(pDevice, 0, reflection ? nullptr : R.mDepthRT->mD3DTexture);
        SetTextureBoth(pDevice, 1, coverage);
        SetTextureBoth(pDevice, 2, detail);
        SetTextureBoth(pDevice, 3, halfSize && !readMarch ? R.CloudTex[0]->mD3DTexture : nullptr);
        SetTextureBoth(pDevice, 4, halfSize ? R.CloudTex[prevAccum]->mD3DTexture : reflectionHalf);
        SetTextureBoth(pDevice, 5, halfSize ? R.CloudTex[nextAccum]->mD3DTexture : nullptr);
        SetTextureBoth(pDevice, 6, readScene ? sceneBase : nullptr);
        SetTextureBoth(pDevice, 7, readSkyRef && R.CloudSkyRefTex ? R.CloudSkyRefTex->mD3DTexture : nullptr);
        SetTextureBoth(pDevice, 8, readMarch ? R.CloudMarchTex[0]->mD3DTexture : nullptr);
        SetTextureBoth(pDevice, 9, readMarch ? R.CloudMarchTex[1]->mD3DTexture : nullptr);
        SetTextureBoth(pDevice, 10, !reflection && pbrShare > 0.0f && R.SkyViewTex ? R.SkyViewTex->mD3DTexture : nullptr);
        for (DWORD slot = 0; slot < 11; ++slot)
        {
            const bool wrap = slot == 1 || slot == 2;
            const bool linear = slot != 0 && slot != 3 && slot != 8 && slot != 9;
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSU, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSV, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_ADDRESSW, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            SetSamplerStateBoth(pDevice, slot, D3DSAMP_MIPFILTER, slot == 1 ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        }
        SetSamplerStateBoth(pDevice, 10, D3DSAMP_SRGBTEXTURE, FALSE);
    };
    // The passes are timed apart only in the scene's clouds, not in the reflection map's.
    auto drawPass = [&](const char* techniqueName, IDirect3DSurface9* target, float w, float h, bool blend, int profile)
    {
        // Only the reflection's own viewport starts away from the target's corner.
        const float x0 = target == sceneSurface ? originX : 0.0f, y0 = target == sceneSurface ? originY : 0.0f;
        pDevice->SetRenderTarget(0, target);
        D3DVIEWPORT9 viewport = { DWORD(x0), DWORD(y0), DWORD(w), DWORD(h), 0.0f, 1.0f };
        pDevice->SetViewport(&viewport);
        pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, blend ? TRUE : FALSE);
        pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, blend ? (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE) : 0xF);
        // VPOS counts from the target's corner: the rays start from the viewport's.
        D3DXVECTOR4 info = ProjInfo(proj, w, h);
        info.z -= x0 * info.x;
        info.w -= y0 * info.y;
        effect->SetVector(effect->GetParameterByName(nullptr, "vec4ProjInfo"), &info);
        const ScreenQuad screenQuad = ScreenQuad::Rect(x0, y0, w, h, quadZ);
        UINT passes = 0;
        // By handle: the effect is large address aware, a name would be taken for one.
        effect->SetTechnique(effect->GetTechniqueByName(techniqueName));
        effect->Begin(&passes, D3DXFX_DONOTSAVESTATE);
        effect->BeginPass(0);
        effect->CommitChanges();
        // Never the scene while drawing into it.
        bindTextures(halfSize && target != sceneSurface, target != R.CloudSkyRefSurf, halfSize && target == R.CloudSurf[0]);
        BindEffectConstantsOnly(pDevice, effect);
        ProfilerScope timedPass(pDevice, reflection ? -1 : profile);
        screenQuad.Draw(pDevice);
        effect->EndPass();
        effect->End();
    };

    if (halfSize)
    {
        // The sky's brightness first, blended into its 1 x 1 target; the march reads it.
        if (R.CloudSkyRefSurf)
        {
            // Cleared after frames without it (a loading screen), since it keeps 0.9 of itself a frame and whatever
            // a loading frame left in it stayed, then blended 30 times to 96% of this frame's sky.
            if (!skyRefRecent)
                RealDevice(pDevice)->ColorFill(R.CloudSkyRefSurf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
            const D3DXVECTOR4 skyRefProj = ProjInfo(proj, 1.0f, 1.0f);
            effect->SetVector(effect->GetParameterByName(nullptr, "vec4SkyRefProj"), &skyRefProj);
            for (int i = skyRefRecent ? 1 : 30; i > 0; --i)
                drawPass("CloudsSkyRef", R.CloudSkyRefSurf, 1.0f, 1.0f, true, kProfCloudsSkyRef);
            R.nCloudSkyRefFrame = FrameHistory::Frame();
        }
        // The march into its two targets, then its light into the half size clouds.
        pDevice->SetRenderTarget(1, R.CloudMarchSurf[1]);
        drawPass("CloudsMarch", R.CloudMarchSurf[0], halfWidth, halfHeight, false, kProfCloudsMarch);
        pDevice->SetRenderTarget(1, nullptr);
        drawPass("CloudsLight", R.CloudSurf[0], halfWidth, halfHeight, false, kProfCloudsLight);
        drawPass("CloudsResolve", R.CloudSurf[nextAccum], halfWidth, halfHeight, false, kProfCloudsResolve);
        drawPass("CloudsComposite", sceneSurface, width, height, true, kProfCloudsComposite);
        R.nCloudAccumIndex ^= 1;
        R.nCloudAccumFrame = FrameHistory::Frame();
    }
    else if (reflection && R.bVolumetricCloudsReflectionsHalfRes && ReflectionHalfTarget(pDevice, width, height))
    {
        // Into the half size target on its own, every pixel (the reflection's depth buffer is of its own size),
        // then over the reflection where its depth is the sky's
        D3DSURFACE_DESC halfDesc = {};
        R.CloudReflectionHalfSurf->GetDesc(&halfDesc);
        pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        drawPass("Clouds", R.CloudReflectionHalfSurf, float(halfDesc.Width), float(halfDesc.Height), false, kProfCloudsMarch);
        pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        reflectionHalf = R.CloudReflectionHalfTex;
        drawPass("CloudsUpscale", sceneSurface, width, height, true, kProfCloudsComposite);
        reflectionHalf = nullptr;
    }
    else
        drawPass("Clouds", sceneSurface, width, height, true, kProfCloudsMarch);

    for (size_t i = 0; i < std::size(kCloudRenderStates); ++i)
        pDevice->SetRenderState(kCloudRenderStates[i].state, savedRenderStates[i]);
    for (DWORD slot = 0; slot < 11; ++slot)
    {
        SetTextureBoth(pDevice, slot, oldTextures[slot]);
        for (size_t i = 0; i < std::size(kSamplerStates); ++i)
            pDevice->SetSamplerState(slot, kSamplerStates[i], savedSamplerStates[slot][i]);
        SAFE_RELEASE(oldTextures[slot]);
    }
    pDevice->SetPixelShaderConstantF(0, savedPSConsts, kPSConstCount);
    pDevice->SetVertexShaderConstantF(0, savedVSConsts, kVSConstCount);
    pDevice->SetPixelShader(oldPS);
    pDevice->SetVertexShader(oldVS);
    pDevice->SetRenderTarget(0, oldTarget);
    pDevice->SetRenderTarget(1, oldTarget1);
    pDevice->SetDepthStencilSurface(oldDepth);
    pDevice->SetViewport(&oldViewport);

    SAFE_RELEASE(oldPS);
    SAFE_RELEASE(oldVS);
    SAFE_RELEASE(oldDepth);
    SAFE_RELEASE(oldTarget);
    SAFE_RELEASE(oldTarget1);
    SAFE_RELEASE(sceneSurface);
}
