module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

export module waterreflectioncapture;

import common;
import comvars;
import drawcapture;
import entityclass;
import framehistory;
import inireload;
import renderskip;

// Water Reflection Capture ([DEBUG] WaterReflectionCapture, hidden, 0): each time the ini is read with it at 1 (Ctrl+Shift+F10,
// the debug server's ini_set), the next water reflection drawn (WATER_REFLECTION_COLOUR, not a frame RenderSkip leaves
// out) goes to GTAIV.EFLC.FusionFix.WaterReflectionCapture.log: its target and depth, the mirrored camera (the current
// grcViewport: view inverse, projection, near and far), every draw in order with what it draws (an entity's kind, model
// hash and distance, or none), its states and shaders, then the draws by kind and distance; and pictures of the target
// at the first draw, before the first entity and at the end. For the far part of the reflection drawn from a cache.
namespace WaterReflectionCapture
{
    static bool armed = false, capturing = false;
    static std::string text;
    static uint32_t captureNumber = 0, pictures = 0, draws = 0, entityDraws = 0;
    static IDirect3DSurface9* target = nullptr;
    static EntityClass::Kind currentKind = EntityClass::Kind::Unknown;
    struct Tally { uint32_t draws = 0; uint64_t primitives = 0; };
    static std::map<std::string, Tally> tally;
    static float eye[3]{};

    static void Picture(IDirect3DDevice9* real, IDirect3DSurface9* surface, const char* pointName)
    {
        const auto name = std::format("GTAIV.EFLC.FusionFix.WaterReflectionCapture.{}.{}-{}.bmp", captureNumber, pictures, pointName);
        text += std::format("----- picture {} {}: {}\n", pictures, pointName, DrawCapture::SaveSurface(real, surface, name));
        ++pictures;
    }

    static void PictureOfTarget(IDirect3DDevice9* real, const char* pointName)
    {
        IDirect3DSurface9* target = nullptr;
        if (SUCCEEDED(real->GetRenderTarget(0, &target)) && target)
        {
            Picture(real, target, pointName);
            target->Release();
        }
    }

    static std::string Matrix(const char* name, const float (&m)[4][4])
    {
        std::string s = std::format("      {}:", name);
        for (int r = 0; r < 4; ++r)
            s += std::format(" [{:.4f} {:.4f} {:.4f} {:.4f}]", m[r][0], m[r][1], m[r][2], m[r][3]);
        return s + "\n";
    }

    static void Begin(IDirect3DDevice9* real)
    {
        ++captureNumber;
        pictures = draws = entityDraws = 0;
        tally.clear();
        text = std::format("capture {}: the water reflection, target {}\n", captureNumber, RenderSkip::CurrentTarget());
        if (SUCCEEDED(real->GetRenderTarget(0, &target)) && target)
        {
            D3DSURFACE_DESC d{};
            target->GetDesc(&d);
            text += std::format("      target {}x{} format {}\n", d.Width, d.Height, int(d.Format));
        }
        IDirect3DSurface9* depth = nullptr;
        if (SUCCEEDED(real->GetDepthStencilSurface(&depth)) && depth)
        {
            D3DSURFACE_DESC d{};
            depth->GetDesc(&d);
            text += std::format("      depth {}x{} format {}\n", d.Width, d.Height, int(d.Format));
            depth->Release();
        }
        else
            text += "      no depth surface\n";
        float plane[4]{};
        real->GetClipPlane(0, plane);
        text += std::format("      clip plane 0 {:.4f} {:.4f} {:.4f} {:.4f}\n", plane[0], plane[1], plane[2], plane[3]);
        if (auto vp = rage::GetCurrentViewport())
        {
            text += std::format("      viewport {}x{} fov {:.3f} aspect {:.3f} near {:.3f} far {:.1f} perspective {}\n", vp->mWidth, vp->mHeight,
                vp->mFov, vp->mAspect, vp->mNearClip, vp->mFarClip, vp->mIsPerspective);
            text += Matrix("view inverse", vp->mViewInverseMatrix);
            text += Matrix("view", vp->mViewMatrix);
            text += Matrix("projection", vp->mProjectionMatrix);
        }
        // The frame's camera, not the viewport's: its view has the mirror in it (the position (x, -z, -y) of the
        // mirrored eye)
        if (const auto& camera = FrameHistory::Current(); camera.Valid)
        {
            const auto& v = camera.View;
            eye[0] = -(v._41 * v._11 + v._42 * v._12 + v._43 * v._13);
            eye[1] = -(v._41 * v._21 + v._42 * v._22 + v._43 * v._23);
            eye[2] = -(v._41 * v._31 + v._42 * v._32 + v._43 * v._33);
            text += std::format("      the frame's camera at {:.2f} {:.2f} {:.2f}\n", eye[0], eye[1], eye[2]);
        }
        if (target)
            Picture(real, target, "first-draw");
    }

    static void End(IDirect3DDevice9* real)
    {
        text += std::format("{} draws, {} of entities; by what and how far:\n", draws, entityDraws);
        for (const auto& [key, t] : tally)
            text += std::format("      {:<28} {:5} draws {:8} primitives\n", key, t.draws, t.primitives);
        if (target)
        {
            Picture(real, target, "end");
            target->Release();
            target = nullptr;
        }
        FusionLog::WriteText("WaterReflectionCapture", "Pass", text);
        text.clear();
        text.shrink_to_fit();
        capturing = false;
    }

    static void DrawObserver(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitives)
    {
        if (!armed && !capturing)
            return;
        const char* name = RenderSkip::CurrentTarget();
        const bool inReflection = name && !std::strcmp(name, "WATER_REFLECTION_COLOUR");
        auto real = RageDirect3DDevice9::RealDevice(device);
        if (capturing && !inReflection)
        {
            End(real);
            return;
        }
        if (!inReflection || RenderSkip::DropsDraws())
            return;
        if (armed)
        {
            armed = false;
            capturing = true;
            Begin(real);
        }
        ++draws;
        uint16_t model = 0;
        const float* place = nullptr;
        std::string what = "no entity";
        std::string key = "no entity";
        if (EntityClass::CurrentModel(model, place) && place)
        {
            const auto info = EntityClass::ModelInfo(model);
            const uint32_t hash = info ? *reinterpret_cast<const uint32_t*>(info + 0x3c) : 0;
            const float dx = place[0] - eye[0], dy = place[1] - eye[1], dz = place[2] - eye[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            static constexpr const char* kKinds[] = { "unmarked", "building", "prop", "vehicle", "ped" };
            const char* kind = kKinds[std::min<size_t>(size_t(currentKind), 4)];
            what = std::format("{} {:08X} at {:.0f} m ({:.0f} {:.0f} {:.0f})", kind, hash, distance, place[0], place[1], place[2]);
            const char* band = distance < 100.0f ? "0-100" : distance < 200.0f ? "100-200" : distance < 300.0f ? "200-300" : distance < 400.0f ? "300-400" :
                distance < 800.0f ? "400-800" : "800+";
            key = std::format("{} {} m", kind, band);
            if (entityDraws++ == 0 && target)
                Picture(real, target, "first-entity");
        }
        auto& t = tally[key];
        ++t.draws;
        t.primitives += primitives;
        if (draws <= 400)
            text += std::format("{:4} {:<56} prim {} x{} | {}\n", draws, what, int(type), primitives, DrawCapture::States(real));
    }

    static void ReadIni(CIniReader& iniReader)
    {
        if (iniReader.ReadInteger("DEBUG", "WaterReflectionCapture", 0) != 0 && !capturing)
            armed = true;
    }
}

class WaterReflectionCaptureInit
{
public:
    WaterReflectionCaptureInit()
    {
        FusionFix::onInitEvent() += []()
        {
            CIniReader iniReader("");
            WaterReflectionCapture::ReadIni(iniReader);
            IniReload::Add(WaterReflectionCapture::ReadIni);
            DrawCapture::AddObserver(&WaterReflectionCapture::DrawObserver);
            EntityClass::AddDrawHooks([](EntityClass::Kind kind) { WaterReflectionCapture::currentKind = kind; },
                                      [](EntityClass::Kind) { WaterReflectionCapture::currentKind = EntityClass::Kind::Unknown; });
        };
    }
} WaterReflectionCaptureInit;
