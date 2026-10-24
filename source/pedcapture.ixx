module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <format>
#include <map>
#include <string>

export module pedcapture;

import common;
import comvars;
import drawcapture;
import entityclass;
import framehistory;
import inireload;
import renderskip;

// Ped Capture ([DEBUG] PedCapture, hidden, 0): each time the ini is read with it at 1 (Ctrl+Shift+F10, the debug server's
// ini_set), the next whole frame goes to GTAIV.EFLC.FusionFix.PedCapture.log: every stretch of draws into one set of
// targets (the game's target name, the device's four colour targets) with its draws by pixel shader, every draw of a
// ped in it (model hash, states, shaders, c150 the skin mark), and pictures of its targets as the frame leaves them.
// For the cutscene heads whose skin comes out paler than the hair's edge over it: which pass draws which part, and
// where the two part.
namespace PedCapture
{
    constexpr uint32_t kMaxPictures = 96, kMaxDraws = 60000, kMaxPedLines = 600, kMaxPedPictures = 24;

    static bool armed = false, capturing = false;
    static uint32_t captureNumber = 0, captureFrame = 0, pictures = 0, draws = 0, pedLines = 0, pedPictures = 0;
    static uint32_t pendingPedDraw = 0;     // a ped's draw whose target is pictured at the next draw, once it is drawn
    static std::string text;
    static EntityClass::Kind currentKind = EntityClass::Kind::Unknown;

    struct Stretch
    {
        uint32_t number = 0, firstDraw = 0, draws = 0, pedDraws = 0;
        std::string name;
        IDirect3DSurface9* targets[4]{};
        std::map<uint32_t, uint32_t> byPixelShader;
    };
    static Stretch stretch;
    static uint32_t stretches = 0;

    static void Picture(IDirect3DDevice9* real, IDirect3DSurface9* surface, int index, const std::string& point = "")
    {
        if (pictures >= kMaxPictures)
        {
            text += std::format("      rt{}{}: no picture (the capture's {} taken)\n", index, point, kMaxPictures);
            return;
        }
        const auto name = std::format("GTAIV.EFLC.FusionFix.PedCapture.{}.{:03}-s{}-rt{}{}.bmp", captureNumber, pictures++, stretch.number, index, point);
        text += std::format("      rt{}{}: {}\n", index, point, DrawCapture::SaveSurface(real, surface, name));
    }

    // The textures of stages 0 to 3: size, format and levels
    static std::string Textures(IDirect3DDevice9* real)
    {
        std::string s;
        for (DWORD stage = 0; stage < 4; ++stage)
        {
            IDirect3DBaseTexture9* texture = nullptr;
            real->GetTexture(stage, &texture);
            if (!texture)
                continue;
            if (texture->GetType() == D3DRTYPE_TEXTURE)
            {
                D3DSURFACE_DESC d{};
                static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &d);
                s += std::format(" s{} {}x{} f{} l{}", stage, d.Width, d.Height, int(d.Format), texture->GetLevelCount());
            }
            else
                s += std::format(" s{} type {}", stage, int(texture->GetType()));
            texture->Release();
        }
        return s.empty() ? " none" : s;
    }

    // The stretch ends: what it drew, and its targets as it left them
    static void CloseStretch(IDirect3DDevice9* real)
    {
        if (!stretch.number)
            return;
        text += std::format("== stretch {} '{}' draws {}-{} ({}), {} of peds; by pixel shader:", stretch.number, stretch.name, stretch.firstDraw,
            stretch.firstDraw + stretch.draws - 1, stretch.draws, stretch.pedDraws);
        for (const auto& [ps, n] : stretch.byPixelShader)
            text += std::format(" {:X}x{}", ps, n);
        text += "\n";
        for (int i = 0; i < 4; ++i)
            if (auto surface = stretch.targets[i])
            {
                Picture(real, surface, i);
                surface->Release();
                stretch.targets[i] = nullptr;
            }
        stretch = {};
    }

    static void End(IDirect3DDevice9* real)
    {
        CloseStretch(real);
        text += std::format("{} draws in {} stretches, {} pictures\n", draws, stretches, pictures);
        FusionLog::WriteText("PedCapture", "Frame", text);
        text.clear();
        text.shrink_to_fit();
        capturing = false;
    }

    static void DrawObserver(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitives)
    {
        if (!armed && !capturing)
            return;
        auto real = RageDirect3DDevice9::RealDevice(device);
        const uint32_t frame = FrameHistory::Frame();
        if (armed)
        {
            // From the first draw of a frame
            if (!captureFrame)
            {
                captureFrame = frame;
                return;
            }
            if (frame == captureFrame)
                return;
            armed = false;
            capturing = true;
            captureFrame = frame;
            ++captureNumber;
            pictures = draws = pedLines = pedPictures = stretches = pendingPedDraw = 0;
            text = std::format("capture {}: scene frame {}\n", captureNumber, frame);
        }
        else if (frame != captureFrame || draws >= kMaxDraws)
        {
            End(real);
            return;
        }
        // The last ped's draw is done by now: its first target as it left it
        if (pendingPedDraw && stretch.targets[0])
            Picture(real, stretch.targets[0], 0, std::format("-after-ped-draw-{}", pendingPedDraw));
        pendingPedDraw = 0;
        ++draws;

        // A new stretch whenever a colour target changes
        IDirect3DSurface9* targets[4]{};
        for (DWORD i = 0; i < 4; ++i)
            real->GetRenderTarget(i, &targets[i]);
        bool same = stretch.number != 0;
        for (int i = 0; i < 4 && same; ++i)
            same = targets[i] == stretch.targets[i];
        if (same)
        {
            for (auto surface : targets)
                if (surface)
                    surface->Release();
        }
        else
        {
            CloseStretch(real);
            stretch.number = ++stretches;
            stretch.firstDraw = draws;
            const char* name = RenderSkip::CurrentTarget();
            stretch.name = name ? name : "?";
            for (int i = 0; i < 4; ++i)
                stretch.targets[i] = targets[i];    // the reference GetRenderTarget gave, released as the stretch closes
            text += std::format("-- stretch {} '{}' from draw {}:", stretch.number, stretch.name, draws);
            for (int i = 0; i < 4; ++i)
                if (auto surface = stretch.targets[i])
                {
                    D3DSURFACE_DESC d{};
                    surface->GetDesc(&d);
                    text += std::format(" rt{} {}x{} format {}", i, d.Width, d.Height, int(d.Format));
                }
            text += "\n";
        }
        ++stretch.draws;

        IDirect3DPixelShader9* ps = nullptr;
        real->GetPixelShader(&ps);
        const uint32_t psNumber = DrawCapture::ShaderNumber(ps, true);
        if (ps)
            ps->Release();
        ++stretch.byPixelShader[psNumber];

        if (currentKind != EntityClass::Kind::Ped)
            return;
        ++stretch.pedDraws;
        if (pedLines++ >= kMaxPedLines)
            return;
        uint16_t model = 0;
        const float* place = nullptr;
        uint32_t hash = 0;
        if (EntityClass::CurrentModel(model, place))
            if (const auto info = EntityClass::ModelInfo(model))
                hash = *reinterpret_cast<const uint32_t*>(info + 0x3c);
        // c39 the stipple, c52 the material ID's step, c150 the skin mark (ped_skin_scattering_mask.patch)
        float c39[4]{}, c52[4]{}, c150[4]{};
        real->GetPixelShaderConstantF(39, c39, 1);
        real->GetPixelShaderConstantF(52, c52, 1);
        real->GetPixelShaderConstantF(150, c150, 1);
        text += std::format("{:5} ped {:08X} prim {} x{} | c39 {:.4f} {:.4f} c52 {:.6f} c150 {:.6f} | tex{} | {}\n", draws, hash, int(type), primitives,
            c39[0], c39[1], c52[0], c150[0], Textures(real), DrawCapture::States(real));
        if (pedPictures < kMaxPedPictures)
        {
            ++pedPictures;
            pendingPedDraw = draws;
        }
    }

    static void ReadIni(CIniReader& iniReader)
    {
        if (iniReader.ReadInteger("DEBUG", "PedCapture", 0) != 0 && !capturing)
        {
            armed = true;
            captureFrame = 0;
        }
    }
}

class PedCaptureInit
{
public:
    PedCaptureInit()
    {
        FusionFix::onInitEvent() += []()
        {
            CIniReader iniReader("");
            PedCapture::ReadIni(iniReader);
            IniReload::Add(PedCapture::ReadIni);
            DrawCapture::AddObserver(&PedCapture::DrawObserver);
            EntityClass::AddDrawHooks([](EntityClass::Kind kind) { PedCapture::currentKind = kind; },
                                      [](EntityClass::Kind) { PedCapture::currentKind = EntityClass::Kind::Unknown; });
        };
    }
} PedCaptureInit;
