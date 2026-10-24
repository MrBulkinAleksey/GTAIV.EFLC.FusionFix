module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

export module framecapture;

import common;
import comvars;
import drawcapture;
import entityclass;
import framehistory;
import inireload;
import renderskip;

// Frame Capture: one whole scene frame (from the first draw after the scene begins to the next scene) to
// GTAIV.EFLC.FusionFix.FrameCapture.log, armed from the ini ([DEBUG] FrameCapture = 1, hidden, read again with Ctrl+Shift+F10 or
// the debug server's ini_set, with FrameCaptureOptions), the debug server's frame_capture or Ctrl+Shift+F5 (with
// FrameCaptureKeyOptions, by default the peds' draws, the G-buffer and the lighting pass saved whole), for the player to keep the frame
// something looks wrong in. It writes
//  - every stretch of draws into one set of colour targets: the game's target name, rt0-rt3 and depth (size, format),
//    the viewport, its draws by pixel shader, and pictures of its targets as the frame leaves them, with the raw values
//    at the probe points (so the G-buffer's albedo, normals, material ID and depth, the light and the fog passes);
//  - every draw that matches the filters: what it draws (entity kind, model hash, place), states, shaders, the
//    textures on s0-s15, a picture of rt0 after the first ones, and all non-zero shader constants of the first few.
// Options, "key=value" apart by spaces in FrameCaptureOptions or each a field of frame_capture:
//  kinds            all (default) or a list apart by commas of none (draws outside entity commands), unmarked,
//                   buildings, props, vehicles, peds
//  model            a model hash (hex), 0 any
//  ps, vs           a FusionShaders number as the log gives it (hex), 0 any
//  target           part of the game's target name, any case; also which stretches are pictured
//  lines            matching draws written (3000)
//  pictures         pictures of stretches' targets (128)
//  draw_pictures    matching draws followed by a picture of rt0 (24)
//  constants        matching draws with every non-zero shader constant (4)
//  stretch_pictures 0 for none of the stretches' targets (1)
//  probes           points x,y (shares of the target's size, 0 to 1) apart by '/', read in every picture
//  raw              part of a target name, any case: the targets of its stretches are also saved whole as their format
//                   holds them (.raw next to the pictures, DrawCapture::SaveSurface's layout); "" none (default)
//  raw_ps           pixel shader numbers (hex) apart by commas: the stretches that drew with any of them are saved
//                   whole too (the lighting pass, without every other stretch of its target's name)
export namespace FrameCapture
{
    struct Options
    {
        uint32_t kinds = 0x3F;  // bit 0: no entity, bit 1 + EntityClass::Kind
        uint32_t model = 0, ps = 0, vs = 0;
        std::string target;
        uint32_t lines = 3000, pictures = 128, drawPictures = 24, constants = 4;
        bool stretchPictures = true;
        std::vector<DrawCapture::Point> probes;
        std::string raw;
        std::vector<uint32_t> rawPixelShaders;
    };

    // One option by its key, throws for a key or value it does not know
    void SetOption(Options& options, const std::string& key, const std::string& value);

    // The next whole scene frame is captured with these options; the capture's number
    uint32_t Arm(const Options& options);

    // The last capture written: its number (0 for none), its stretches in short and the pictures it wrote
    uint32_t Done();
    std::string Summary();
    std::vector<std::string> Pictures();
}

namespace FrameCapture
{
    static constexpr const char* kKindNames[] = { "none", "unmarked", "buildings", "props", "vehicles", "peds" };

    static std::mutex mutex;            // the options armed and the last capture's results, between the threads
    static Options armedOptions;
    static std::atomic<bool> armed = false;
    static std::atomic<uint32_t> asked = 0, done = 0;
    static std::string lastSummary;
    static std::vector<std::string> lastPictures;

    // Render thread only
    static Options options;
    static bool capturing = false;
    static uint32_t armedFrame = 0, captureFrame = 0, captureNumber = 0, draws = 0, matches = 0, pictures = 0, drawPictures = 0, dumps = 0;
    static uint32_t pendingDraw = 0;    // a matching draw whose rt0 is pictured at the next draw, once it is drawn
    static std::string text, summary;
    static std::vector<std::string> pictureNames;
    static EntityClass::Kind currentKind = EntityClass::Kind::Unknown;
    static bool inEntity = false;

    struct Stretch
    {
        uint32_t number = 0, firstDraw = 0, draws = 0, matches = 0;
        std::string name;
        IDirect3DSurface9* targets[4]{};
        std::map<uint32_t, uint32_t> byPixelShader;
    };
    static Stretch stretch;
    static uint32_t stretches = 0;

    static uint32_t ParseNumber(const std::string& value, bool hex)
    {
        const bool prefixed = value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
        return uint32_t(std::stoul(prefixed ? value.substr(2) : value, nullptr, hex || prefixed ? 16 : 10));
    }

    static std::string Lower(std::string text)
    {
        for (auto& c : text)
            c = char(tolower(uint8_t(c)));
        return text;
    }

    void SetOption(Options& o, const std::string& key, const std::string& value)
    {
        if (key == "kinds")
        {
            o.kinds = 0;
            std::stringstream list(Lower(value));
            for (std::string name; std::getline(list, name, ',');)
            {
                if (name == "all")
                {
                    o.kinds = 0x3F;
                    continue;
                }
                bool known = false;
                for (size_t i = 0; i < std::size(kKindNames); ++i)
                    if (name == kKindNames[i] || name + "s" == kKindNames[i])
                        o.kinds |= 1u << i, known = true;
                if (!known)
                    throw std::runtime_error("kinds: all, none, unmarked, buildings, props, vehicles, peds");
            }
        }
        else if (key == "model")
            o.model = ParseNumber(value, true);
        else if (key == "ps")
            o.ps = ParseNumber(value, true);
        else if (key == "vs")
            o.vs = ParseNumber(value, true);
        else if (key == "target")
            o.target = Lower(value);
        else if (key == "lines")
            o.lines = ParseNumber(value, false);
        else if (key == "pictures")
            o.pictures = std::min(ParseNumber(value, false), 1000u);
        else if (key == "draw_pictures")
            o.drawPictures = std::min(ParseNumber(value, false), 1000u);
        else if (key == "constants")
            o.constants = ParseNumber(value, false);
        else if (key == "stretch_pictures")
            o.stretchPictures = value != "0" && value != "false";
        else if (key == "raw")
            o.raw = Lower(value);
        else if (key == "raw_ps")
        {
            o.rawPixelShaders.clear();
            std::stringstream list(value);
            for (std::string number; std::getline(list, number, ',');)
                if (!number.empty())
                    o.rawPixelShaders.push_back(ParseNumber(number, true));
        }
        else if (key == "probes")
        {
            o.probes.clear();
            std::stringstream list(value);
            for (std::string point; std::getline(list, point, '/');)
            {
                const auto comma = point.find(',');
                if (comma == std::string::npos)
                    throw std::runtime_error("probes: x,y/x,y, shares of the target's size");
                o.probes.push_back({ std::stof(point.substr(0, comma)), std::stof(point.substr(comma + 1)) });
            }
        }
        else
            throw std::runtime_error("unknown frame capture option '" + key + "'");
    }

    uint32_t Arm(const Options& o)
    {
        std::lock_guard lock(mutex);
        armedOptions = o;
        armed = true;
        return ++asked;
    }

    uint32_t Done()
    {
        return done;
    }

    std::string Summary()
    {
        std::lock_guard lock(mutex);
        return lastSummary;
    }

    std::vector<std::string> Pictures()
    {
        std::lock_guard lock(mutex);
        return lastPictures;
    }

    // When the game started, in the files' names: the captures count from 1 each run, and would write over the last run's
    static const std::string& RunStamp()
    {
        static const std::string stamp = []
        {
            SYSTEMTIME t{};
            GetLocalTime(&t);
            return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        }();
        return stamp;
    }

    static bool RawWanted()
    {
        for (const auto ps : options.rawPixelShaders)
            if (stretch.byPixelShader.contains(ps))
                return true;
        return !options.raw.empty() && Lower(stretch.name).find(options.raw) != std::string::npos;
    }

    static void Picture(IDirect3DDevice9* real, IDirect3DSurface9* surface, int index, const std::string& point = "", bool raw = false)
    {
        std::string name, rawName;
        if (pictures < options.pictures + options.drawPictures)
        {
            name = std::format("GTAIV.EFLC.FusionFix.FrameCapture.{}-{}.{:03}-s{}-rt{}{}.bmp", RunStamp(), captureNumber, pictures++, stretch.number, index, point);
            pictureNames.push_back(name);
        }
        if (raw)
        {
            rawName = std::format("GTAIV.EFLC.FusionFix.FrameCapture.{}-{}.s{}-rt{}{}.raw", RunStamp(), captureNumber, stretch.number, index, point);
            pictureNames.push_back(rawName);
        }
        text += std::format("      rt{}{}: {}\n", index, point, DrawCapture::SaveSurface(real, surface, name, options.probes, rawName));
    }

    static bool TargetWanted()
    {
        return options.target.empty() || Lower(stretch.name).find(options.target) != std::string::npos;
    }

    // The stretch ends: what it drew, and its targets as it left them
    static void CloseStretch(IDirect3DDevice9* real)
    {
        if (!stretch.number)
            return;
        auto line = std::format("== stretch {} '{}' draws {}-{} ({}), {} matching; by pixel shader:", stretch.number, stretch.name, stretch.firstDraw,
            stretch.firstDraw + stretch.draws - 1, stretch.draws, stretch.matches);
        for (const auto& [ps, n] : stretch.byPixelShader)
            line += std::format(" {:X}x{}", ps, n);
        text += line + "\n";
        summary += line + "\n";
        const bool wanted = options.stretchPictures && TargetWanted(), raw = RawWanted();
        for (int i = 0; i < 4; ++i)
            if (auto surface = stretch.targets[i])
            {
                if (raw)
                    Picture(real, surface, i, "", true);
                else if (wanted && (pictures < options.pictures || !options.probes.empty()))
                {
                    // Over the stretches' share of pictures only the probes are read
                    if (pictures < options.pictures)
                        Picture(real, surface, i);
                    else
                        text += std::format("      rt{}: {}\n", i, DrawCapture::SaveSurface(real, surface, "", options.probes));
                }
                surface->Release();
                stretch.targets[i] = nullptr;
            }
        stretch = {};
    }

    static void Begin(IDirect3DDevice9* real, uint32_t frame)
    {
        capturing = true;
        captureFrame = frame;
        draws = matches = pictures = drawPictures = dumps = stretches = pendingDraw = 0;
        pictureNames.clear();
        summary.clear();
        std::string kinds;
        for (size_t i = 0; i < std::size(kKindNames); ++i)
            if (options.kinds & (1u << i))
                kinds += std::string(kinds.empty() ? "" : ",") + kKindNames[i];
        text = std::format("capture {} (files FrameCapture.{}-{}.*): scene frame {}; kinds {} model {:08X} ps {:X} vs {:X} target '{}' raw '{}' lines {} pictures {} draw pictures {} constants {} probes",
            captureNumber, RunStamp(), captureNumber, frame, kinds, options.model, options.ps, options.vs, options.target, options.raw, options.lines, options.pictures, options.drawPictures,
            options.constants);
        for (const auto& p : options.probes)
            text += std::format(" {:.3f},{:.3f}", p.x, p.y);
        text += "\n";
        if (const auto& camera = FrameHistory::Current(); camera.Valid)
        {
            const auto& v = camera.View;
            text += std::format("      camera at {:.2f} {:.2f} {:.2f}\n", -(v._41 * v._11 + v._42 * v._12 + v._43 * v._13),
                -(v._41 * v._21 + v._42 * v._22 + v._43 * v._23), -(v._41 * v._31 + v._42 * v._32 + v._43 * v._33));
        }
    }

    static void End(IDirect3DDevice9* real)
    {
        CloseStretch(real);
        const auto totals = std::format("capture {}: {} draws in {} stretches, {} matching, {} pictures\n", captureNumber, draws, stretches, matches, pictures);
        text += totals;
        summary += totals;
        FusionLog::WriteText("FrameCapture", "Frame", text);
        text.clear();
        text.shrink_to_fit();
        {
            std::lock_guard lock(mutex);
            lastSummary = std::move(summary);
            lastPictures = std::move(pictureNames);
        }
        summary.clear();
        pictureNames.clear();
        capturing = false;
        done = captureNumber;
    }

    static std::string Textures(IDirect3DDevice9* real)
    {
        std::string s;
        for (DWORD stage = 0; stage < 16; ++stage)
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

    // Every non-zero float constant, and the bool and integer ones set
    static std::string Constants(IDirect3DDevice9* real)
    {
        std::string s = "      ps:";
        float ps[224][4]{};
        real->GetPixelShaderConstantF(0, &ps[0][0], 224);
        for (int i = 0; i < 224; ++i)
            if (ps[i][0] != 0.0f || ps[i][1] != 0.0f || ps[i][2] != 0.0f || ps[i][3] != 0.0f)
                s += std::format(" c{} ({:.6g} {:.6g} {:.6g} {:.6g})", i, ps[i][0], ps[i][1], ps[i][2], ps[i][3]);
        BOOL bools[16]{};
        real->GetPixelShaderConstantB(0, bools, 16);
        s += " | b";
        for (int i = 0; i < 16; ++i)
            if (bools[i])
                s += std::format(" {}", i);
        int ints[16][4]{};
        real->GetPixelShaderConstantI(0, &ints[0][0], 16);
        for (int i = 0; i < 16; ++i)
            if (ints[i][0] || ints[i][1] || ints[i][2] || ints[i][3])
                s += std::format(" i{} ({} {} {} {})", i, ints[i][0], ints[i][1], ints[i][2], ints[i][3]);
        s += "\n      vs:";
        float vs[256][4]{};
        real->GetVertexShaderConstantF(0, &vs[0][0], 256);
        for (int i = 0; i < 256; ++i)
            if (vs[i][0] != 0.0f || vs[i][1] != 0.0f || vs[i][2] != 0.0f || vs[i][3] != 0.0f)
                s += std::format(" c{} ({:.6g} {:.6g} {:.6g} {:.6g})", i, vs[i][0], vs[i][1], vs[i][2], vs[i][3]);
        BOOL vsBools[16]{};
        real->GetVertexShaderConstantB(0, vsBools, 16);
        s += " | b";
        for (int i = 0; i < 16; ++i)
            if (vsBools[i])
                s += std::format(" {}", i);
        return s + "\n";
    }

    static void DrawObserver(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT primitives)
    {
        if (!armed && !capturing)
            return;
        auto real = RageDirect3DDevice9::RealDevice(device);
        const uint32_t frame = FrameHistory::Frame();
        if (!capturing)
        {
            // Armed: from the first draw of the next scene
            if (!armedFrame)
            {
                armedFrame = frame ? frame : 1;
                return;
            }
            if (frame == armedFrame)
                return;
            {
                std::lock_guard lock(mutex);
                options = armedOptions;
                captureNumber = asked;
                armed = false;
            }
            armedFrame = 0;
            Begin(real, frame);
        }
        else if (frame != captureFrame || draws >= 200000)
        {
            End(real);
            return;
        }
        // The last matching draw is done by now: rt0 as it left it
        if (pendingDraw && stretch.targets[0])
            Picture(real, stretch.targets[0], 0, std::format("-after-draw-{}", pendingDraw));
        pendingDraw = 0;
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
                    text += std::format(" rt{} {}x{} f{}", i, d.Width, d.Height, int(d.Format));
                }
            IDirect3DSurface9* depth = nullptr;
            if (SUCCEEDED(real->GetDepthStencilSurface(&depth)) && depth)
            {
                D3DSURFACE_DESC d{};
                depth->GetDesc(&d);
                text += std::format(" depth {}x{} f{}", d.Width, d.Height, int(d.Format));
                depth->Release();
            }
            text += "\n";
        }
        ++stretch.draws;

        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexShader9* vs = nullptr;
        real->GetPixelShader(&ps);
        real->GetVertexShader(&vs);
        const uint32_t psNumber = DrawCapture::ShaderNumber(ps, true), vsNumber = DrawCapture::ShaderNumber(vs, false);
        if (ps)
            ps->Release();
        if (vs)
            vs->Release();
        ++stretch.byPixelShader[psNumber];

        // The filters
        const uint32_t kindBit = inEntity ? 1u << (1 + std::min<size_t>(size_t(currentKind), 4)) : 1u;
        if (!(options.kinds & kindBit) || (options.ps && psNumber != options.ps) || (options.vs && vsNumber != options.vs) || !TargetWanted())
            return;
        uint16_t model = 0;
        const float* place = nullptr;
        uint32_t hash = 0;
        if (inEntity && EntityClass::CurrentModel(model, place))
            if (const auto info = EntityClass::ModelInfo(model))
                hash = *reinterpret_cast<const uint32_t*>(info + 0x3c);
        if (options.model && hash != options.model)
            return;
        ++matches;
        ++stretch.matches;
        if (matches <= options.lines)
        {
            const char* kind = inEntity ? kKindNames[1 + std::min<size_t>(size_t(currentKind), 4)] : "none";
            std::string what = kind;
            if (hash)
                what += std::format(" {:08X}", hash);
            if (place)
                what += std::format(" at {:.1f} {:.1f} {:.1f}", place[0], place[1], place[2]);
            text += std::format("{:6} {} prim {} x{} | {} | tex{}\n", draws, what, int(type), primitives, DrawCapture::States(real), Textures(real));
        }
        if (dumps < options.constants)
        {
            ++dumps;
            text += Constants(real);
        }
        if (drawPictures < options.drawPictures)
        {
            ++drawPictures;
            pendingDraw = draws;
        }
    }

    // Ctrl+Shift+F5: a capture with FrameCaptureKeyOptions, for the player to take the frame something looks wrong in
    static std::string keyOptions = "kinds=peds draw_pictures=24 constants=8 raw=gbuffer_0 raw_ps=8";

    static bool ParseOptions(const std::string& text, const char* key, Options& o)
    {
        std::stringstream list(text);
        try
        {
            for (std::string pair; list >> pair;)
                if (const auto equals = pair.find('='); equals != std::string::npos)
                    SetOption(o, pair.substr(0, equals), pair.substr(equals + 1));
        }
        catch (const std::exception& e)
        {
            FusionLog::Write("FrameCapture", "Options", "%s: %s\n", key, e.what());
            return false;
        }
        return true;
    }

    static void ReadIni(CIniReader& iniReader)
    {
        {
            std::lock_guard lock(mutex);
            keyOptions = iniReader.ReadString("DEBUG", "FrameCaptureKeyOptions", "kinds=peds draw_pictures=24 constants=8 raw=gbuffer_0 raw_ps=8");
        }
        if (iniReader.ReadInteger("DEBUG", "FrameCapture", 0) == 0 || capturing)
            return;
        Options o;
        if (ParseOptions(iniReader.ReadString("DEBUG", "FrameCaptureOptions", ""), "FrameCaptureOptions", o))
            Arm(o);
    }

    // Once a game frame
    static void PollKey()
    {
        static bool keyWasDown = false;
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F5) & 0x8000);
        const bool pressed = down && !keyWasDown;
        keyWasDown = down;
        if (!pressed || armed || capturing)
            return;
        std::string text;
        {
            std::lock_guard lock(mutex);
            text = keyOptions;
        }
        Options o;
        if (!ParseOptions(text, "FrameCaptureKeyOptions", o))
            return;
        const uint32_t number = Arm(o);
        FusionLog::Write("FrameCapture", "Keys", "Ctrl+Shift+F5: capture %u of the next frame, files FrameCapture.%s-%u.* (%s)\n", number, RunStamp().c_str(), number, text.c_str());
    }
}

class FrameCaptureInit
{
public:
    FrameCaptureInit()
    {
        FusionFix::onInitEvent() += []()
        {
            CIniReader iniReader("");
            FrameCapture::ReadIni(iniReader);
            IniReload::Add(FrameCapture::ReadIni);
            FusionFix::onGameProcessEvent() += FrameCapture::PollKey;
            DrawCapture::AddObserver(&FrameCapture::DrawObserver);
            EntityClass::AddDrawHooks([](EntityClass::Kind kind) { FrameCapture::currentKind = kind; FrameCapture::inEntity = true; },
                                      [](EntityClass::Kind) { FrameCapture::currentKind = EntityClass::Kind::Unknown; FrameCapture::inEntity = false; });
        };
    }
} FrameCaptureInit;
