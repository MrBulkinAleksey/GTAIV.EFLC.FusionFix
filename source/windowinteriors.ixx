module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

export module windowinteriors;

import common;
import comvars;
import settings;
import inireload;
import entityclass;
import renderskip;

// Window Interiors (Graphics > Lighting): rooms behind the lit windows at night. The night windows' glow is drawn
// by the forward techniques of gta_emissivenight, gta_glass_emissivenight and gta_normal_spec_reflect_emissivenight,
// most of it by "unlit_draw" (PS2, the scene's forward pass after the lights, per the capture), the rest by "draw",
// "lightweight0_draw" and "lightweight4_draw"; shaders/patches/world_window_interiors.patch has
// each lit pixel of a wall look into a box room behind it (interior mapping) and shades the glow by what the view ray
// meets there. The rooms are a grid on the wall in metres, so they need no model data; the glow keeps its own pattern
// and colour.
// Registers, set before the transparent pass: c161 = camera position, strength (0 = off); c162 = 1 / room width,
// height and depth, part of the rooms with a blind; c163 = fade with the distance as a * distance + b, debug, gain.
// Tuning: hidden keys of [LIGHTING] (WindowInteriors*), read again on Ctrl+Shift+F10.
// Frame capture (WindowInteriorsCapture, hidden, 1 for now): Ctrl+Shift+F5 writes the next frame's draws to
// GTAIV.EFLC.FusionFix.WindowInteriorsCapture.log: each pixel shader's draws by the part of the frame they fall in, and
// for the emissive shaders' (FusionShaders numbers 167-224 and 380-401) the models and the constants they draw with.
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

    static inline bool bCaptureEnabled = true;

    static void ReadIni(CIniReader& iniReader)
    {
        bCaptureEnabled = iniReader.ReadInteger("LIGHTING", "WindowInteriorsCapture", 1) != 0;
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

    // Render thread, once the lights are drawn and again as the scene's forward pass begins (CRenderPhaseDrawScene, CE
    // 0xADD200: buckets 1 and 3 drawn with the lamps, techniques lightweight0/lightweight4 by the lamps on the entity,
    // between CE 0xABD880 and 0xABEC20).
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
        if (auto real = RageDirect3DDevice9::RealDevice(pDevice); real && real != pDevice)
            real->SetPixelShaderConstantF(kRegister, params, 3);
        if (capturing)
            captureText += std::format("      constants set in part {}: c161 {:.1f} {:.1f} {:.1f} {:.2f}, debug {:.0f}\n", PartName(capturePart), params[0], params[1], params[2], params[3], params[10]);
    }

    // Frame capture
    enum Part : uint32_t { Other, GBuffer, Lights, AfterLights, Scene, Parts };
    static const char* PartName(uint32_t part)
    {
        static constexpr const char* names[] = { "other", "G-buffer", "lights", "after the lights", "scene (forward)" };
        return part < std::size(names) ? names[part] : "?";
    }
    static inline bool capturing = false;
    static inline bool captureArmed = false;
    static inline bool captureKeyWasDown = false;
    static inline uint32_t capturePart = Other;
    static inline uint32_t captureDraws = 0;
    static inline uint32_t captureNumber = 0;
    static inline std::string captureText;
    static inline std::map<std::pair<uint32_t, uint32_t>, uint32_t> drawsByPartAndShader;
    struct EmissiveDraws
    {
        uint32_t draws = 0;
        float c161w[2] = { 1e9f, -1e9f };
        std::set<uint32_t> models;
        std::string first;
    };
    static inline std::map<std::pair<uint32_t, uint32_t>, EmissiveDraws> emissive;

    static bool IsEmissive(uint32_t ps) { return (ps >= 167 && ps <= 224) || (ps >= 380 && ps <= 401); }

    static uint32_t ShaderNumber(IDirect3DPixelShader9* shader)
    {
        static std::unordered_map<const void*, uint32_t> known;
        if (!shader)
            return 0;
        if (auto it = known.find(shader); it != known.end())
            return it->second;
        UINT size = 0;
        uint32_t number = 0;
        if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size >= 16)
        {
            std::vector<uint32_t> code(size / 4);
            if (SUCCEEDED(shader->GetFunction(code.data(), &size)))
                for (size_t i = 0; i + 3 < code.size(); ++i)
                    if (code[i] == 0x69737546 && code[i + 1] == 0x68536E6F && code[i + 2] == 0x72656461) // "FusionShaders"
                    {
                        number = code[i + 3];
                        break;
                    }
            if (!number)
                number = 0x80000000u | size; // no signature: its size, to tell it apart
        }
        known.emplace(shader, number);
        return number;
    }

    static void __cdecl MarkPart(uint32_t part)
    {
        if (!capturing)
            return;
        captureText += std::format("      part {} from draw {}\n", PartName(part), captureDraws);
        capturePart = part;
    }
    static void QueuePart(uint32_t part)
    {
        auto cb = new T_CB_Generic_NoArgs(part == GBuffer ? +[]() { MarkPart(GBuffer); } : part == Lights ? +[]() { MarkPart(Lights); } : +[]() { MarkPart(Scene); });
        if (cb)
            cb->Append();
    }

    static void CaptureDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE, UINT)
    {
        if (!capturing)
            return;
        ++captureDraws;
        IDirect3DPixelShader9* ps = nullptr;
        device->GetPixelShader(&ps);
        const uint32_t number = ShaderNumber(ps);
        if (ps)
            ps->Release();
        ++drawsByPartAndShader[{ capturePart, number }];
        if (!IsEmissive(number))
            return;
        auto real = RageDirect3DDevice9::RealDevice(device);
        float c[6][4]{};
        real->GetPixelShaderConstantF(161, c[0], 3);
        real->GetPixelShaderConstantF(45, c[3], 1);
        real->GetPixelShaderConstantF(66, c[4], 1);
        real->GetPixelShaderConstantF(51, c[5], 1);
        auto& e = emissive[{ capturePart, number }];
        ++e.draws;
        e.c161w[0] = std::min(e.c161w[0], c[0][3]);
        e.c161w[1] = std::max(e.c161w[1], c[0][3]);
        uint16_t model = 0;
        const float* place = nullptr;
        uint32_t hash = 0;
        if (EntityClass::CurrentModel(model, place))
            if (auto info = EntityClass::ModelInfo(model))
                hash = *reinterpret_cast<const uint32_t*>(info + 0x3C);
        if (e.models.size() < 12)
            e.models.insert(hash);
        if (e.first.empty())
        {
            DWORD blend = 0, source = 0, destination = 0, zWrite = 0;
            device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
            device->GetRenderState(D3DRS_SRCBLEND, &source);
            device->GetRenderState(D3DRS_DESTBLEND, &destination);
            device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
            const char* target = RenderSkip::CurrentTarget();
            e.first = std::format("draw {} target {} blend {} {}/{} zwrite {}; c161 {:.1f} {:.1f} {:.1f} {:.2f} c162 {:.3f} {:.3f} {:.3f} {:.2f} c163 {:.4f} {:.3f} {:.0f} {:.2f}; c45 {:.3f} {:.3f} {:.3f} {:.3f} c66 {:.3f} c51 {:.2f} {:.2f} {:.2f} {:.2f}",
                captureDraws, target ? target : "?", blend, source, destination, zWrite, c[0][0], c[0][1], c[0][2], c[0][3], c[1][0], c[1][1], c[1][2], c[1][3],
                c[2][0], c[2][1], c[2][2], c[2][3], c[3][0], c[3][1], c[3][2], c[3][3], c[4][0], c[5][0], c[5][1], c[5][2], c[5][3]);
        }
    }

    // Render thread, at the end of each frame: starts a capture on the key, writes one out at the end of its frame.
    static void CaptureFrameEnd()
    {
        if (capturing)
        {
            capturing = false;
            std::string text = std::format("capture {}: {} draws\n{}", captureNumber, captureDraws, captureText);
            text += "  draws by part and pixel shader (FusionShaders number, or 0x80000000 + size without one):\n";
            for (uint32_t part = 0; part < Parts; ++part)
            {
                std::vector<std::pair<uint32_t, uint32_t>> rows;
                for (const auto& [key, count] : drawsByPartAndShader)
                    if (key.first == part)
                        rows.emplace_back(count, key.second);
                if (rows.empty())
                    continue;
                std::sort(rows.rbegin(), rows.rend());
                text += std::format("    {}:", PartName(part));
                for (const auto& [count, number] : rows)
                    text += number & 0x80000000u ? std::format(" s{}x{}", number & 0x7FFFFFFFu, count) : std::format(" {}x{}", number, count);
                text += "\n";
            }
            text += "  emissive shaders (gta_emissivenight 167-178, gta_glass_emissivenight 203-213, gta_normal_spec_reflect_emissivenight 380-390; patched: 167 168 176 177, 203 204 211 212, 380 381 388 389):\n";
            for (const auto& [key, e] : emissive)
            {
                std::string models;
                for (auto hash : e.models)
                    models += std::format(" {:08X}", hash);
                text += std::format("    {} ps {}: {} draws, c161.w {:.2f}..{:.2f}, models{}\n      first: {}\n", PartName(key.first), key.second, e.draws, e.c161w[0], e.c161w[1], models, e.first);
            }
            FusionLog::WriteText("WindowInteriorsCapture", "Frame", text);
        }
        if (captureArmed)
        {
            captureArmed = false;
            capturing = true;
            ++captureNumber;
            captureDraws = 0;
            capturePart = Other;
            captureText.clear();
            drawsByPartAndShader.clear();
            emissive.clear();
        }
        if (bCaptureEnabled)
        {
            const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F5) & 0x8000);
            if (down && !captureKeyWasDown)
                captureArmed = true;
            captureKeyWasDown = down;
        }
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
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnBuildRenderList() += []() { QueuePart(GBuffer); };
            CRenderPhaseDeferredLighting_LightsToScreen::OnBuildRenderList() += []() { QueuePart(Lights); };
            CRenderPhaseDeferredLighting_LightsToScreen::OnAfterBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(+[]() { MarkPart(AfterLights); SetConstants(); });
                if (cb)
                    cb->Append();
            };
            CRenderPhaseDrawScene::OnBuildRenderList() += []()
            {
                QueuePart(Scene);
                auto cb = new T_CB_Generic_NoArgs(SetConstants);
                if (cb)
                    cb->Append();
            };
            RenderSkip::pDrawObserver = &CaptureDraw;
        };

        FusionFix::onEndScene() += []() { CaptureFrameEnd(); };
    }
} WindowInteriors;
