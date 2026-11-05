module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <iterator>

export module backfacedepth;

import common;
import comvars;
import inireload;
import entityclass;
import postfxresources;
import vtablepatch;

// The depth of the back faces of the peds and vehicles, for SSR (SSRBackFaces, off by default). A ray that went behind
// what the depth buffer shows can only guess how far the thing goes, with SSRThickness, and the guess let rays stop at
// outlines they only passed near, drawing them out across a door. Each of their G-buffer draws is drawn a second time
// with the culling turned round into a depth of its own, in the same stored form as the scene's; where nothing was
// drawn it stays 1, as far as depth goes, and SSR.fx's TraceHit goes on as without it.
export namespace BackFaceDepth
{
    // Render thread, after the G-buffer: this frame's depth, nullptr if nothing was drawn into it.
    IDirect3DTexture9* Texture();
}

namespace
{
    using DrawIndexed = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    constexpr auto kIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

    DrawIndexed realDraw = nullptr;
    bool bEnabled = false;
    float fMaxDistance = 15.0f;
    // Render thread
    bool bInGBuffer = false;
    bool bBodyDraw = false;
    bool bDrawn = false;
    IDirect3DTexture9* texture = nullptr;
    IDirect3DSurface9* surface = nullptr;
    UINT width = 0, height = 0;
    uint32_t drawn = 0, doubleSided = 0;

    void Release()
    {
        if (surface)
            surface->Release();
        if (texture)
            texture->Release();
        surface = nullptr;
        texture = nullptr;
        width = height = 0;
    }

    bool Target(IDirect3DDevice9* device, UINT w, UINT h)
    {
        if (texture && width == w && height == h)
            return true;
        Release();
        if (FAILED(device->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, kIntz, D3DPOOL_DEFAULT, &texture, nullptr)) || !texture)
        {
            texture = nullptr;
            return false;
        }
        if (FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface)
        {
            surface = nullptr;
            Release();
            return false;
        }
        width = w;
        height = h;
        return true;
    }

    void DrawBackFaces(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count)
    {
        DWORD cull = D3DCULL_NONE;
        device->GetRenderState(D3DRS_CULLMODE, &cull);
        // Drawn from both sides: nothing to tell its back from
        if (cull == D3DCULL_NONE)
        {
            ++doubleSided;
            return;
        }
        IDirect3DSurface9* sceneDepth = nullptr;
        if (FAILED(device->GetDepthStencilSurface(&sceneDepth)) || !sceneDepth)
            return;
        D3DSURFACE_DESC desc = {};
        if (FAILED(sceneDepth->GetDesc(&desc)) || !Target(device, desc.Width, desc.Height))
        {
            sceneDepth->Release();
            return;
        }

        constexpr D3DRENDERSTATETYPE kStates[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_STENCILENABLE,
            D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3 };
        DWORD saved[std::size(kStates)] = {};
        for (size_t i = 0; i < std::size(kStates); ++i)
            device->GetRenderState(kStates[i], &saved[i]);

        device->SetDepthStencilSurface(surface);
        if (!bDrawn)
        {
            device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
            bDrawn = true;
        }
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE2, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE3, 0);
        device->SetRenderState(D3DRS_CULLMODE, cull == D3DCULL_CW ? D3DCULL_CCW : D3DCULL_CW);
        realDraw(device, type, base, minIndex, vertices, start, count);
        ++drawn;

        device->SetRenderState(D3DRS_CULLMODE, cull);
        for (size_t i = 0; i < std::size(kStates); ++i)
            device->SetRenderState(kStates[i], saved[i]);
        device->SetDepthStencilSurface(sceneDepth);
        sceneDepth->Release();
    }

    HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count)
    {
        if (bEnabled && bInGBuffer && bBodyDraw && !rage::grcDevice::IsDeviceLost())
            DrawBackFaces(device, type, base, minIndex, vertices, start, count);
        return realDraw(device, type, base, minIndex, vertices, start, count);
    }

    // Far cars cost most of the second draws and take up too few pixels of a reflection to matter. Peds keep
    // no position in their draw commands (and few draws to the cars' dozen each), so they are always drawn.
    bool NearEnough()
    {
        uint16_t model = 0;
        const float* place = nullptr;
        auto vp = rage::GetCurrentViewport();
        if (fMaxDistance <= 0.0f || !vp || !EntityClass::CurrentModel(model, place) || !place)
            return true;
        const float dx = place[0] - vp->mViewInverseMatrix[3][0];
        const float dy = place[1] - vp->mViewInverseMatrix[3][1];
        const float dz = place[2] - vp->mViewInverseMatrix[3][2];
        return dx * dx + dy * dy + dz * dz <= fMaxDistance * fMaxDistance;
    }

    void BeforeEntityDraw(EntityClass::Kind kind)
    {
        bBodyDraw = bEnabled && bInGBuffer &&
            (kind == EntityClass::Kind::Ped || (kind == EntityClass::Kind::Vehicle && NearEnough()));
    }

    void AfterEntityDraw(EntityClass::Kind)
    {
        bBodyDraw = false;
    }

    void BeginGBuffer()
    {
        bInGBuffer = true;
        bDrawn = false;
    }

    void EndGBuffer()
    {
        bInGBuffer = false;
        // The first frames that drew any, to see the draws are caught and turned round at all
        static int logged = 0;
        if (bDrawn && logged < 3)
        {
            ++logged;
            FusionLog::Write("BackFaces", "Frame", "%u draws turned round, %u drawn from both sides and left out, %ux%u\n", drawn, doubleSided, width, height);
        }
        drawn = doubleSided = 0;
    }

    void ReadIni(CIniReader& iniReader)
    {
        bEnabled = iniReader.ReadInteger("REFLECTIONS", "SSRBackFaces", 0) != 0;
        fMaxDistance = iniReader.ReadFloat("REFLECTIONS", "SSRBackFacesDistance", 15.0f);
    }
}

namespace BackFaceDepth
{
    IDirect3DTexture9* Texture()
    {
        return bEnabled && bDrawn ? texture : nullptr;
    }
}

class BackFaceDepthHooks
{
public:
    BackFaceDepthHooks()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
        };

        EntityClass::AddDrawHooks(BeforeEntityDraw, AfterEntityDraw);

        FusionFix::onGameInitEvent() += []()
        {
            auto device = RageDirect3DDevice9::GetRuntimeDevice();
            if (device)
            {
                D3DCAPS9 caps = {};
                device->GetDeviceCaps(&caps);
                FusionLog::Write("BackFaces", "Settings", "%s, the device takes pixel shaders of %u instruction slots\n",
                    bEnabled ? "on" : "off", caps.MaxPixelShader30InstructionSlots);
                PatchVTable(*reinterpret_cast<void***>(device), 82, realDraw, &DrawIndexedPrimitive);
            }
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(BeginGBuffer);
                if (cb)
                    cb->Append();
            };
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnAfterBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(EndGBuffer);
                if (cb)
                    cb->Append();
            };
        };

        FusionFix::onBeforeReset() += []()
        {
            bInGBuffer = false;
            bBodyDraw = false;
            bDrawn = false;
            auto& R = PostFxResources;
            if (R.SSREffect && R.SSREffectHandles.BackFaceDepthTex2D)
                R.SSREffect->SetTexture(R.SSREffectHandles.BackFaceDepthTex2D, nullptr);
            Release();
        };
    }
} BackFaceDepthHooks;
