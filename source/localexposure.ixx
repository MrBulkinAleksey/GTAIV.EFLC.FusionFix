module;

#include <common.hxx>
#include <algorithm>

export module localexposure;

import common;
import comvars;
import settings;
import inireload;
import postfxresources;

using RageDirect3DDevice9::RealDevice;
using RageDirect3DDevice9::SetTextureBoth;
using RageDirect3DDevice9::SetSamplerStateBoth;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

// Local Exposure (Graphics > Lighting, off by default): the game exposes the whole frame by one adapted luminance, so
// at night a lit shop front burns out while the alley beside it sinks into black. Right before the composite post fx
// pass the game's quarter screen scene ("Quarter Screen 0", half the size each way) is shrunk twice by four with
// StretchRect into own float targets, a thirty-second of the screen, on s12; the pass
// (shaders/patches/world_local_exposure.patch) exposes each pixel by (adapted / the luminance around it)^k, between
// 0.6 and 1.8. Bright edges can take a faint halo, as the copy is a plain average. c157 = k, the least and most scale;
// c158 = one texel of the copy.
// Tuning: hidden keys of [POSTFX] (LocalExposure*), read again on Ctrl+Shift+F10.
class LocalExposure
{
    static constexpr DWORD kSampler = 12;

    static inline float fStrength = 0.3f;
    static inline float fMin = 0.6f;
    static inline float fMax = 1.8f;
    // Render thread
    static inline IDirect3DTexture9* small[2] = {};
    static inline IDirect3DSurface9* smallSurf[2] = {};
    static inline UINT smallSize[2] = {};

    static void ReadIni(CIniReader& iniReader)
    {
        fStrength = std::clamp(iniReader.ReadFloat("POSTFX", "LocalExposureStrength", 0.3f), 0.0f, 1.0f);
        fMin = std::clamp(iniReader.ReadFloat("POSTFX", "LocalExposureMin", 0.6f), 0.1f, 1.0f);
        fMax = std::clamp(iniReader.ReadFloat("POSTFX", "LocalExposureMax", 1.8f), 1.0f, 10.0f);
    }

    static bool Enabled()
    {
        static auto pref = FusionFixSettings.GetRef("PREF_LOCAL_EXPOSURE");
        return pref && pref->get() != 0;
    }

    static void Release()
    {
        for (int i = 0; i < 2; ++i)
        {
            SAFE_RELEASE(smallSurf[i]);
            SAFE_RELEASE(small[i]);
            smallSize[i] = 0;
        }
    }

    // The quarter screen shrunk into small[1]; false when it can't be.
    static bool Shrink(IDirect3DDevice9* pDevice)
    {
        auto& R = PostFxResources;
        if (!R.mHalfScreenRT || !R.mHalfScreenRT->mD3DTexture)
            return false;
        IDirect3DSurface9* quarter = nullptr;
        D3DSURFACE_DESC desc = {};
        if (FAILED(R.mHalfScreenRT->mD3DTexture->GetSurfaceLevel(0, &quarter)) || FAILED(quarter->GetDesc(&desc)))
        {
            SAFE_RELEASE(quarter);
            return false;
        }
        const UINT w[2] = { (std::max)(desc.Width / 4, 1u), (std::max)(desc.Width / 16, 1u) };
        const UINT h[2] = { (std::max)(desc.Height / 4, 1u), (std::max)(desc.Height / 16, 1u) };
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i)
        {
            if (small[i] && smallSize[i] != (w[i] << 16 | h[i]))
            {
                SAFE_RELEASE(smallSurf[i]);
                SAFE_RELEASE(small[i]);
            }
            if (!small[i])
            {
                ok = SUCCEEDED(RealDevice(pDevice)->CreateTexture(w[i], h[i], 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
                                                                  &small[i], nullptr)) &&
                     SUCCEEDED(small[i]->GetSurfaceLevel(0, &smallSurf[i]));
                smallSize[i] = ok ? (w[i] << 16 | h[i]) : 0;
            }
        }
        ok = ok && SUCCEEDED(pDevice->StretchRect(quarter, nullptr, smallSurf[0], nullptr, D3DTEXF_LINEAR)) &&
             SUCCEEDED(pDevice->StretchRect(smallSurf[0], nullptr, smallSurf[1], nullptr, D3DTEXF_LINEAR));
        SAFE_RELEASE(quarter);
        if (!ok)
            Release();
        return ok;
    }

public:
    static void Bind(IDirect3DDevice9* pDevice)
    {
        const bool on = Enabled() && fStrength > 0.0f && Shrink(pDevice);
        const float params[8] = {
            on ? fStrength : 0.0f, fMin, fMax, 0.0f,
            on ? 1.0f / float(smallSize[1] >> 16) : 0.0f, on ? 1.0f / float(smallSize[1] & 0xFFFF) : 0.0f, 0.0f, 0.0f,
        };
        pDevice->SetPixelShaderConstantF(157, params, 2);
        if (auto real = RealDevice(pDevice); real != pDevice)
            real->SetPixelShaderConstantF(157, params, 2);
        if (!on)
            return;
        SetTextureBoth(pDevice, kSampler, small[1]);
        for (auto [type, value] : { std::pair{ D3DSAMP_ADDRESSU, DWORD(D3DTADDRESS_CLAMP) }, std::pair{ D3DSAMP_ADDRESSV, DWORD(D3DTADDRESS_CLAMP) },
                 std::pair{ D3DSAMP_MINFILTER, DWORD(D3DTEXF_LINEAR) }, std::pair{ D3DSAMP_MAGFILTER, DWORD(D3DTEXF_LINEAR) },
                 std::pair{ D3DSAMP_MIPFILTER, DWORD(D3DTEXF_NONE) }, std::pair{ D3DSAMP_SRGBTEXTURE, DWORD(FALSE) } })
            SetSamplerStateBoth(pDevice, kSampler, type, value);
    }

    LocalExposure()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };
        // Default pool targets must be gone before the device resets.
        FusionFix::onBeforeReset() += []() { Release(); };
    }
} LocalExposure;

// Right before the game's composite post fx pass (postfx.ixx DrawCallPostFX).
export void BindLocalExposure(IDirect3DDevice9* pDevice)
{
    LocalExposure::Bind(pDevice);
}
