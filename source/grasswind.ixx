module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <atomic>
#include <format>
#include <mutex>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module grasswind;

import common;
import comvars;
import settings;
import inireload;
import entityclass;
import postfxresources;
import nearbybodies;

// For other grass (gpugrass.ixx), render thread: the grass models' list and this frame's wind, set at the start of the
// G-buffer pass.
export namespace GrassShared
{
    bool IsGrassModel(uint32_t nameHash);
    // What a grass model is, by its name: 0 not grass, 1 grass (proc_grass*, grass0_*, GrassWindModels), 2 weeds, 3 small
    // plants with broad leaves (*proc_plant_*, sm_plant*)
    int GrassKind(uint32_t nameHash);
    inline float BendPerHeight = 0.0f, FlutterPerHeight = 0.0f;   // at the tip, per metre of height
    inline float GustPhase = 0.0f, FlutterPhase = 0.0f;           // cycles
    inline float Direction[2] = { 0.929f, 0.370f };
    inline float GustWavelength = 5.0f;                           // metres
    // This G-buffer pass's bending: the bodies (none with Grass Bending off), a body's push at the tip of the plant's
    // height (0 = off), the downwash's pulse; the trample grid (nullptr when not trampling) and its side (m)
    inline const std::vector<NearbyBodies::Body>* Bodies = nullptr;
    inline float PushPerHeight = 0.0f;
    inline float RotorPulse = 1.0f;
    inline IDirect3DTexture9* TrampleTexture = nullptr;
    inline float TrampleGridSide = 64.0f;
}

// Grass Wind (Graphics > Lighting, [PROCEDURAL] GrassWind): the wind sways the grass, weeds and small plants the game
// scatters on the ground (procedural props: proc_grass*, *_proc_plant_*, *weed*, sm_plant*). They are buildings from
// the building pool (EntityClass counts them as buildings) drawn with gta_default (gta_cutout), whose deferred vertex
// shaders move each vertex when b12 is on (shaders/patches/world_grass_wind.patch). Around each G-buffer draw of such a
// model (by its name's hash, model info +0x3c) b12 is set and c241 gets the model's height and its bend and flutter at
// the tip; at the start of the pass c240 and c242 get the wind's direction and the phases of the gust wave and the
// flutter.
//
// The game has no wind direction; the clouds' drift (0.93, 0.37) is taken so the grass and the clouds agree. The
// strength is the weather's own (CWeather::Update's table, lerped between the old and the new weather), without the
// game's rise with the camera's height, eased over a few seconds; none indoors. Game time, so the grass stands still
// while the game is paused.
//
// [PROCEDURAL] GrassWindStrength scales the sway; GrassWindModels adds model names (comma separated) to the list.
// Grass Bending (Graphics > Lighting, [PROCEDURAL] GrassBending): the cars and people around the player (NearbyBodies,
// 60 m) push the blades away and lay them down under a car, and the wind dies where they press. Per grass draw the two
// bodies nearest the prop whose reach takes it (a car's footprint and half a metre past it, a person's feet and a metre around)
// and that stand on its ground go to c248-c251; none clears them.
//
// Helicopters with their blades turning, standing or up to 30 m above a prop, blow the grass away from under them (their
// downwash), by the blades' speed, wider and weaker the higher they fly, pulsing with the blades.
//
// Grass Trample (Graphics > Lighting, [PROCEDURAL] GrassTrample): where people walked and wheels rolled the grass stays
// down and rises again over GrassTrampleSeconds (12). A grid of 128 x 128 cells of 0.5 m around the player, tied to the
// world (a cell's place in it is its world position modulo 64 m, so the grid only clears the rows and columns the player
// leaves behind), is stamped along each body's path in the last frame (a person's feet, a car's four wheels, a bike's
// two) with the way it went, and goes to a vertex texture (vs s1, A8R8G8B8: direction, strength, the ground's height
// modulo 64 m) for the G-buffer pass; c243 = 1 / 64 m, the strength (0 = off). Needs vertex textures (DXVK has them).
//
// GrassWindDebug (Graphics > Debug) sways four times as hard, to see which draws take it. GrassWindLog = 1 ([DEBUG])
// writes the wind and the grass draws to GTAIV.EFLC.FusionFix.GrassWind.log every 600 G-buffer passes.
class GrassWind
{
    static constexpr int kWindRegister = 240;       // c240: direction / wavelength, gust phase, flutter phase
    static constexpr int kModelRegister = 241;      // c241: 1 / height, bend and flutter at the tip (metres)
    static constexpr int kDirectionRegister = 242;  // c242.xy: the wind's unit direction
    static constexpr int kTrampleRegister = 243;    // c243: 1 / the trample grid's side (m), its strength (0 = off)
    static constexpr DWORD kTrampleSampler = D3DVERTEXTEXTURESAMPLER1;  // vs s1
    static constexpr int kBodyRegister = 248;       // c248-c251: two bodies (centre, forward; half length, half width, 1 / reach, strength)
    static constexpr int kSwayFlag = 12;            // b12 (vertex shader)
    static constexpr float kPushPerHeight = 0.75f;  // a body's push at the tip, of the plant's height

    static constexpr float kDirection[2] = { 0.929f, 0.370f };
    // CWeather::Update's wind by weather type (0x103c878): EXTRASUNNY, SUNNY, SUNNY_WINDY, CLOUDY, RAIN, DRIZZLE,
    // FOGGY, LIGHTNING
    static constexpr float kWeatherWind[8] = { 0.15f, 0.25f, 0.8f, 0.2f, 0.5f, 0.3f, 0.1f, 1.0f };
    static constexpr float kGustWavelength = 5.0f;  // metres between gusts along the wind

    // Downwash: from a helicopter's position (about a metre over its skids) above the prop, gone at 30 m
    static constexpr float kRotorLow = 0.3f, kRotorHigh = 30.0f;

    // Trample grid
    static constexpr int kCells = 128;
    static constexpr float kCell = 0.5f;
    static constexpr float kGridSide = kCells * kCell;  // 64 m
    static constexpr float kHeightWrap = 64.0f;         // the ground's height in the texture, modulo this
    static constexpr float kStampRange = 30.0f;         // bodies this near the player trample

    static inline float fStrength = 1.0f;
    static inline float fTrampleSeconds = 12.0f;
    static inline bool bLog = false;
    // GrassWindModels, read on the ini's thread; the list is made again on the render thread
    static inline std::mutex extraModelsMutex;
    static inline std::string sExtraModels;
    static inline std::atomic<bool> bRemakeList = true;

    // Render thread
    static inline bool bInGBuffer = false;
    static inline bool bBound = false;
    static inline int32_t lastModel = -1;
    static inline float fWind = -1.0f;              // eased, -1 until the first pass
    static inline double gustPhase = 0.0, flutterPhase = 0.0;
    static inline int32_t lastTime = -1;
    static inline float fBendPerHeight = 0.0f, fFlutterPerHeight = 0.0f, fPushPerHeight = 0.0f;
    static inline std::vector<NearbyBodies::Body> bodies;
    static inline bool bBodiesBound = false;
    static inline float rotorPulse = 1.0f;

    // Trample grid, render thread: by world cell modulo kCells
    struct Cell { float strength; float direction[2]; float ground; };
    static inline std::vector<Cell> cells;
    static inline int gridOrigin[2] = { INT_MIN, INT_MIN };   // the window's first cell, world cells
    static inline IDirect3DTexture9* trampleTexture = nullptr;
    static inline int trampleSupport = -1;                    // vertex textures: -1 not checked yet, 0 none, 1 there
    static inline bool trampleAny = false, trampleUploaded = false, bTrampleBound = false;

    // By model index: the hash the entry was made for and the model's height, 0 if not grass
    struct ModelEntry { uint32_t hash = 0; float height = -1.0f; };
    static inline std::vector<ModelEntry> models;
    static inline std::unordered_map<uint32_t, int> grassHashes; // the kind, as GrassShared::GrassKind

    // GrassWindLog
    static inline uint32_t logPasses = 0;
    static inline uint64_t logGrassDraws = 0, logCheckedDraws = 0, logPressedDraws = 0;
    static inline size_t logBodies = 0, logTrampled = 0;
    static inline std::unordered_set<uint16_t> logModels;

    static int32_t Pref(std::optional<std::reference_wrapper<int32_t>>& ref, int32_t missing)
    {
        return ref ? ref->get() : missing;
    }

    static void AddName(std::string_view name, int kind = 1)
    {
        std::string text(name);
        grassHashes.emplace(hashStringLowercaseFromSeed(text.c_str(), 0), kind);
    }

    // The game's and our data's grass, weed and small plant props; names that do not exist cost nothing.
    static void MakeModelList()
    {
        grassHashes.clear();
        for (int i = 0; i <= 40; ++i)
        {
            AddName(std::format("cj_proc_plant_{}", i), 3);
            AddName(std::format("ag_proc_plant_{}", i), 3);
            AddName(std::format("proc_plant_{}", i), 3);
        }
        for (int i = 0; i <= 20; ++i)
        {
            AddName(std::format("proc_grass{:02}", i));
            AddName(std::format("proc_weed{:02}", i), 2);
            AddName(std::format("ag_weed{:02}", i), 2);
            AddName(std::format("sm_weed{}_", i), 2);
            AddName(std::format("sm_weed{}", i), 2);
            AddName(std::format("sm_plant{}", i), 3);
            AddName(std::format("grass0_{}", i));
            AddName(std::format("cj_proc_weed{}", i), 2);
        }
        AddName("proc_grass_dry");
        AddName("cj_proc_weed", 2);
        for (size_t pos = 0; pos < sExtraModels.size();)
        {
            size_t end = sExtraModels.find(',', pos);
            if (end == std::string::npos)
                end = sExtraModels.size();
            auto name = std::string_view(sExtraModels).substr(pos, end - pos);
            while (!name.empty() && name.front() == ' ')
                name.remove_prefix(1);
            while (!name.empty() && name.back() == ' ')
                name.remove_suffix(1);
            if (!name.empty())
                AddName(name);
            pos = end + 1;
        }
        models.assign(65536, {});
    }

    static void MakeModelListIfAsked()
    {
        if (!bRemakeList.exchange(false))
            return;
        std::lock_guard lock(extraModelsMutex);
        MakeModelList();
    }

    static void ReadIni(CIniReader& iniReader)
    {
        fStrength = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GrassWindStrength", 1.0f), 0.0f, 4.0f);
        fTrampleSeconds = std::clamp(iniReader.ReadFloat("PROCEDURAL", "GrassTrampleSeconds", 12.0f), 1.0f, 300.0f);
        bLog = iniReader.ReadInteger("DEBUG", "GrassWindLog", 0) != 0;
        {
            std::lock_guard lock(extraModelsMutex);
            sExtraModels = iniReader.ReadString("PROCEDURAL", "GrassWindModels", "");
        }
        bRemakeList = true;
    }

    // The model's height if it is grass, else 0
    static float GrassHeight(uint16_t model)
    {
        const auto info = EntityClass::ModelInfo(model);
        if (!info || models.empty())
            return 0.0f;
        const uint32_t hash = *reinterpret_cast<const uint32_t*>(info + 0x3c);
        auto& entry = models[model];
        if (entry.height < 0.0f || entry.hash != hash)
        {
            entry.hash = hash;
            entry.height = 0.0f;
            if (grassHashes.contains(hash))
            {
                // The props stand on their origin: the bounding sphere's centre is half way up; a model centred on its
                // origin falls back to the radius.
                const float centreZ = *reinterpret_cast<const float*>(info + 0x18);
                const float radius = *reinterpret_cast<const float*>(info + 0x1c);
                const float height = centreZ > 0.05f ? centreZ * 2.0f : radius;
                entry.height = std::isfinite(height) ? std::clamp(height, 0.1f, 4.0f) : 1.0f;
            }
        }
        return entry.height;
    }

    static float WeatherWind()
    {
        if (!CWeather::OldWeatherType || !CWeather::NewWeatherType || !CWeather::InterpolationValue)
            return 0.25f;
        const auto at = [](uint32_t type) { return kWeatherWind[(std::min)(type, 7u)]; };
        const float t = std::clamp(*CWeather::InterpolationValue, 0.0f, 1.0f);
        return at(*CWeather::OldWeatherType) * (1.0f - t) + at(*CWeather::NewWeatherType) * t;
    }

    static void SetFlag(IDirect3DDevice9* pDevice, bool on)
    {
        const BOOL flag = on;
        pDevice->SetVertexShaderConstantB(kSwayFlag, &flag, 1);
        bBound = on;
    }

    static Cell& CellAt(int x, int y)
    {
        return cells[(y & (kCells - 1)) * kCells + (x & (kCells - 1))];
    }

    // Keeps the window of cells around the player; cells it takes in from the far side are cleared.
    static void MoveGrid(const float* focus)
    {
        const int ox = int(std::floor(focus[0] / kCell)) - kCells / 2, oy = int(std::floor(focus[1] / kCell)) - kCells / 2;
        if (cells.empty())
            cells.assign(kCells * kCells, {});
        if (gridOrigin[0] == INT_MIN || std::abs(ox - gridOrigin[0]) >= kCells || std::abs(oy - gridOrigin[1]) >= kCells)
        {
            std::fill(cells.begin(), cells.end(), Cell{});
            gridOrigin[0] = ox, gridOrigin[1] = oy;
            return;
        }
        // Columns, then rows, that come in: those past the old window's end, or before its start
        const auto clearColumns = [](int from, int to) { for (int x = from; x < to; ++x) for (int y = 0; y < kCells; ++y) CellAt(x, y) = {}; };
        const auto clearRows = [](int from, int to) { for (int y = from; y < to; ++y) for (int x = 0; x < kCells; ++x) CellAt(x, y) = {}; };
        if (ox > gridOrigin[0])
            clearColumns(gridOrigin[0] + kCells, ox + kCells);
        else if (ox < gridOrigin[0])
            clearColumns(ox, gridOrigin[0]);
        if (oy > gridOrigin[1])
            clearRows(gridOrigin[1] + kCells, oy + kCells);
        else if (oy < gridOrigin[1])
            clearRows(oy, gridOrigin[1]);
        gridOrigin[0] = ox, gridOrigin[1] = oy;
    }

    // Lays the grass down within `radius` of (x, y), fading out over the next half metre, towards `direction`.
    static void Stamp(float x, float y, float ground, float radius, float strength, const float* direction)
    {
        const int x0 = int(std::floor((x - radius) / kCell)) - 2, x1 = int(std::floor((x + radius) / kCell)) + 2;
        const int y0 = int(std::floor((y - radius) / kCell)) - 2, y1 = int(std::floor((y + radius) / kCell)) + 2;
        for (int cy = (std::max)(y0, gridOrigin[1]); cy <= (std::min)(y1, gridOrigin[1] + kCells - 1); ++cy)
        {
            for (int cx = (std::max)(x0, gridOrigin[0]); cx <= (std::min)(x1, gridOrigin[0] + kCells - 1); ++cx)
            {
                auto& cell = CellAt(cx, cy);
                const float dx = (cx + 0.5f) * kCell - x, dy = (cy + 0.5f) * kCell - y;
                const float w = std::clamp(1.0f - (std::sqrt(dx * dx + dy * dy) - radius) / kCell, 0.0f, 1.0f);
                if (w <= 0.0f)
                {
                    // An empty cell next to the stamp takes its ground, so the filtered height between them holds
                    if (cell.strength <= 0.0f)
                        cell.ground = ground;
                    continue;
                }
                const float s = w * strength;
                if (s <= cell.strength)
                    continue;
                float lx = direction[0] * s + cell.direction[0] * cell.strength, ly = direction[1] * s + cell.direction[1] * cell.strength;
                const float length = std::sqrt(lx * lx + ly * ly);
                if (length > 1e-4f)
                    cell.direction[0] = lx / length, cell.direction[1] = ly / length;
                cell.strength = s;
                cell.ground = ground;
                trampleAny = true;
            }
        }
    }

    // Stamps along the way a point went in the last `dt` seconds, a step every quarter metre.
    static void StampPath(float x, float y, float ground, const float* velocity, float dt, float radius, float strength)
    {
        const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
        if (speed < 0.3f)
            return;
        const float direction[2] = { velocity[0] / speed, velocity[1] / speed };
        const float length = (std::min)(speed * dt, 3.0f);
        const int steps = (std::max)(1, int(std::ceil(length / 0.25f)));
        for (int i = 0; i < steps; ++i)
        {
            const float back = length * (1.0f - float(i + 1) / steps);
            Stamp(x - direction[0] * back, y - direction[1] * back, ground, radius, strength, direction);
        }
    }

    static bool MakeTrampleTexture(IDirect3DDevice9* pDevice)
    {
        if (trampleSupport < 0)
        {
            trampleSupport = 0;
            IDirect3D9* d3d = nullptr;
            if (pDevice->GetDirect3D(&d3d) == S_OK && d3d)
            {
                D3DDEVICE_CREATION_PARAMETERS cp{};
                D3DDISPLAYMODE mode{};
                pDevice->GetCreationParameters(&cp);
                d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode);
                trampleSupport = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                    D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)) ? 1 : 0;
                d3d->Release();
            }
            if (bLog)
                FusionLog::Write("GrassWind", "Trample", "vertex textures (A8R8G8B8): %s\n", trampleSupport ? "yes" : "no, no trampling");
            if (trampleSupport && FAILED(RageDirect3DDevice9::RealDevice(pDevice)->CreateTexture(kCells, kCells, 1, 0, D3DFMT_A8R8G8B8,
                    D3DPOOL_MANAGED, &trampleTexture, nullptr)))
            {
                trampleTexture = nullptr;
                trampleSupport = 0;
            }
            // Its first contents are whatever the driver left: the first update writes it whole
            trampleUploaded = true;
        }
        return trampleTexture != nullptr;
    }

    // The grid's cells to the texture: r, g the direction, b the strength, a the ground modulo kHeightWrap
    static void UploadTrample()
    {
        D3DLOCKED_RECT locked{};
        if (FAILED(trampleTexture->LockRect(0, &locked, nullptr, 0)))
            return;
        for (int y = 0; y < kCells; ++y)
        {
            auto row = reinterpret_cast<uint8_t*>(locked.pBits) + y * locked.Pitch;
            for (int x = 0; x < kCells; ++x)
            {
                const auto& cell = cells[y * kCells + x];
                const auto unorm = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                const float wrapped = cell.ground / kHeightWrap - std::floor(cell.ground / kHeightWrap);
                row[x * 4 + 0] = unorm(cell.strength);                     // B
                row[x * 4 + 1] = unorm(cell.direction[1] * 0.5f + 0.5f);   // G
                row[x * 4 + 2] = unorm(cell.direction[0] * 0.5f + 0.5f);   // R
                row[x * 4 + 3] = uint8_t(int(wrapped * 256.0f) & 255);     // A
            }
        }
        trampleTexture->UnlockRect(0);
    }

    // This frame's trampling: the grid follows the player, recovers by dt and takes the bodies' paths; to the texture.
    static bool UpdateTrample(IDirect3DDevice9* pDevice, const float* focus, float dt)
    {
        if (!MakeTrampleTexture(pDevice))
            return false;
        MoveGrid(focus);
        if (trampleAny && dt > 0.0f)
        {
            trampleAny = false;
            const float recover = dt / fTrampleSeconds;
            for (auto& cell : cells)
            {
                if (cell.strength <= 0.0f)
                    continue;
                cell.strength = (std::max)(cell.strength - recover, 0.0f);
                trampleAny |= cell.strength > 0.0f;
                logTrampled += bLog && cell.strength > 0.0f;
            }
        }
        for (const auto& body : bodies)
        {
            if (body.distance2 > kStampRange * kStampRange || body.kind == NearbyBodies::Body::Rotor)
                continue;
            if (body.kind == NearbyBodies::Body::Person)
            {
                StampPath(body.centre[0], body.centre[1], body.bottom, body.velocity, dt, 0.3f, 0.7f);
                continue;
            }
            // A car's four wheels, a bike's two
            const float fx = body.forward[0], fy = body.forward[1];
            const float along = body.halfLength * 0.7f, across = body.halfWidth < 0.6f ? 0.0f : body.halfWidth * 0.85f;
            for (int i = 0; i < 4; ++i)
            {
                const float a = (i & 1) ? along : -along, c = (i & 2) ? across : -across;
                if (across == 0.0f && (i & 2))
                    break;
                StampPath(body.centre[0] + fx * a + fy * c, body.centre[1] + fy * a - fx * c, body.bottom, body.velocity, dt, 0.3f, 1.0f);
            }
        }
        if (trampleAny || trampleUploaded)
        {
            UploadTrample();
            trampleUploaded = trampleAny;
        }
        return true;
    }

    static void BindTrample(IDirect3DDevice9* pDevice, bool on)
    {
        RageDirect3DDevice9::SetTextureBoth(pDevice, kTrampleSampler, on ? trampleTexture : nullptr);
        if (on)
        {
            for (auto [type, value] : { std::pair{ D3DSAMP_ADDRESSU, DWORD(D3DTADDRESS_WRAP) }, std::pair{ D3DSAMP_ADDRESSV, DWORD(D3DTADDRESS_WRAP) },
                     std::pair{ D3DSAMP_MINFILTER, DWORD(D3DTEXF_LINEAR) }, std::pair{ D3DSAMP_MAGFILTER, DWORD(D3DTEXF_LINEAR) },
                     std::pair{ D3DSAMP_MIPFILTER, DWORD(D3DTEXF_NONE) } })
                RageDirect3DDevice9::SetSamplerStateBoth(pDevice, kTrampleSampler, type, value);
        }
        bTrampleBound = on;
    }

    // First command of the G-buffer pass: this frame's wind.
    static void BeginGBuffer()
    {
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        static auto windPref = FusionFixSettings.GetRef("PREF_GRASS_WIND");
        static auto debugPref = FusionFixSettings.GetRef("PREF_GRASS_WIND_DEBUG");
        static auto bendingPref = FusionFixSettings.GetRef("PREF_GRASS_BENDING");
        static auto tramplePref = FusionFixSettings.GetRef("PREF_GRASS_TRAMPLE");
        auto& R = PostFxResources;
        MakeModelListIfAsked();

        const int32_t now = CTimer::m_snTimeInMilliseconds ? *CTimer::m_snTimeInMilliseconds : 0;
        const float dt = lastTime < 0 ? 0.0f : std::clamp((now - lastTime) * 0.001f, 0.0f, 0.1f);
        lastTime = now;

        const float target = R.bInteriorScene ? 0.0f : WeatherWind();
        fWind = fWind < 0.0f ? target : fWind + (target - fWind) * (1.0f - std::exp(-dt / 3.0f));

        // The gusts run downwind faster in a strong wind; the flutter quickens with it.
        gustPhase = std::fmod(gustPhase + dt * (3.0f + 8.0f * fWind) / kGustWavelength, 1.0);
        flutterPhase = std::fmod(flutterPhase + dt * (1.4f + 3.0f * fWind), 1.0);

        const bool windOn = Pref(windPref, 1) != 0 && fStrength > 0.0f;
        const bool bending = Pref(bendingPref, 1) != 0;
        const bool trampling = Pref(tramplePref, 1) != 0;
        const bool on = (windOn || bending || trampling) && EntityClass::Installed();
        const float scale = on && windOn ? fStrength * (Pref(debugPref, 0) ? 4.0f : 1.0f) : 0.0f;
        fBendPerHeight = scale * (0.03f + 0.22f * fWind);
        fFlutterPerHeight = scale * (0.012f + 0.045f * fWind);
        // Trampled grass bends by the same push; the live bodies only with bending on
        fPushPerHeight = on && (bending || trampling) ? kPushPerHeight : 0.0f;
        bodies.clear();
        float focus[3] = {};
        bool haveBodies = false;
        if (on && (bending || trampling))
        {
            NearbyBodies::Want();
            haveBodies = NearbyBodies::Copy(bodies, focus);
            if (!haveBodies)
                bodies.clear();
        }
        const bool trample = on && trampling && haveBodies && UpdateTrample(pDevice, focus, dt);
        if (!bending)
            bodies.clear();
        // The downwash pulses with the blades
        rotorPulse = 0.8f + 0.12f * std::sin(now * 0.0215f) + 0.08f * std::sin(now * 0.0347f);
        if (bLog)
            logBodies += bodies.size();

        const float wind[8] = {
            kDirection[0] / kGustWavelength, kDirection[1] / kGustWavelength, -static_cast<float>(gustPhase), static_cast<float>(flutterPhase),
            kDirection[0], kDirection[1], 0.0f, 0.0f,
        };
        GrassShared::BendPerHeight = fBendPerHeight;
        GrassShared::FlutterPerHeight = fFlutterPerHeight;
        GrassShared::GustPhase = static_cast<float>(gustPhase);
        GrassShared::FlutterPhase = static_cast<float>(flutterPhase);
        GrassShared::Direction[0] = kDirection[0], GrassShared::Direction[1] = kDirection[1];
        GrassShared::GustWavelength = kGustWavelength;
        GrassShared::Bodies = &bodies;
        GrassShared::PushPerHeight = fPushPerHeight;
        GrassShared::RotorPulse = rotorPulse;
        GrassShared::TrampleTexture = trample ? trampleTexture : nullptr;
        GrassShared::TrampleGridSide = kGridSide;
        pDevice->SetVertexShaderConstantF(kWindRegister, wind, 1);
        pDevice->SetVertexShaderConstantF(kDirectionRegister, wind + 4, 1);
        const float trampleParams[4] = { 1.0f / kGridSide, trample ? 1.0f : 0.0f, 0.0f, 0.0f };
        pDevice->SetVertexShaderConstantF(kTrampleRegister, trampleParams, 1);
        if (trample)
            BindTrample(pDevice, true);
        SetFlag(pDevice, false);
        ClearBodies(pDevice);
        lastModel = -1;
        bInGBuffer = on;

        if (bLog && ++logPasses >= 600)
        {
            std::string heights;
            for (const auto model : logModels)
                heights += std::format(" {:08x}:{:.2f}", models[model].hash, models[model].height);
            FusionLog::Write("GrassWind", "Stats", "wind %.2f (weather %.2f) scale %.2f, per pass: %.1f grass draws of %.1f building and prop draws, %.1f pressed by %.1f bodies near, %.1f trampled cells (trample %s); %zu grass models (hash:height m):%s\n",
                fWind, target, scale, double(logGrassDraws) / logPasses, double(logCheckedDraws) / logPasses, double(logPressedDraws) / logPasses,
                double(logBodies) / logPasses, double(logTrampled) / logPasses,
                trampleSupport < 0 ? "not asked" : trampleSupport ? "on" : "no vertex textures", logModels.size(), heights.c_str());
            logPasses = 0;
            logGrassDraws = logCheckedDraws = logPressedDraws = 0;
            logBodies = logTrampled = 0;
            logModels.clear();
        }
    }

    static void ClearBodies(IDirect3DDevice9* pDevice)
    {
        static const float none[16] = {};
        pDevice->SetVertexShaderConstantF(kBodyRegister, none, 4);
        bBodiesBound = false;
    }

    // The two bodies nearest the prop that reach it and stand on its ground, to c248-c251
    static void SetBodies(IDirect3DDevice9* pDevice, uint16_t model, const float* place, float height)
    {
        const NearbyBodies::Body* nearest[2] = {};
        float nearestGap[2] = { FLT_MAX, FLT_MAX };
        if (!bodies.empty())
        {
            const auto info = EntityClass::ModelInfo(model);
            // How far the model's blades reach from its origin: the bounding sphere's radius plus its centre's offset
            const auto sphere = info ? reinterpret_cast<const float*>(info + 0x10) : nullptr;
            const float radius = sphere ? std::clamp(sphere[3] + std::sqrt(sphere[0] * sphere[0] + sphere[1] * sphere[1]), 0.0f, 8.0f) : 2.0f;
            for (const auto& body : bodies)
            {
                if (body.kind == NearbyBodies::Body::Rotor)
                {
                    const float above = body.bottom - place[2];
                    if (above < kRotorLow || above > kRotorHigh)
                        continue;
                }
                else if (std::abs(place[2] - body.bottom) > height + 1.0f)
                    continue;
                const float dx = place[0] - body.centre[0], dy = place[1] - body.centre[1];
                const float along = std::abs(dx * body.forward[0] + dy * body.forward[1]) - body.halfLength;
                const float across = std::abs(dx * body.forward[1] - dy * body.forward[0]) - body.halfWidth;
                const float a = (std::max)(along, 0.0f), c = (std::max)(across, 0.0f);
                const float gap = std::sqrt(a * a + c * c);
                if (gap > Reach(body, place) + radius)
                    continue;
                if (gap < nearestGap[0])
                {
                    nearest[1] = nearest[0], nearestGap[1] = nearestGap[0];
                    nearest[0] = &body, nearestGap[0] = gap;
                }
                else if (gap < nearestGap[1])
                    nearest[1] = &body, nearestGap[1] = gap;
            }
        }
        if (!nearest[0])
        {
            if (bBodiesBound)
                ClearBodies(pDevice);
            return;
        }
        float values[16] = {};
        for (int i = 0; i < 2; ++i)
        {
            const auto* body = nearest[i];
            if (!body)
                break;
            float* v = values + i * 8;
            v[0] = body->centre[0], v[1] = body->centre[1], v[2] = body->forward[0], v[3] = body->forward[1];
            v[4] = body->halfLength, v[5] = body->halfWidth, v[6] = 1.0f / Reach(*body, place);
            // Fading over the last 10 m of the bodies' range, so none pops
            v[7] = std::clamp((NearbyBodies::kRange - std::sqrt(body->distance2)) * 0.1f, 0.0f, 1.0f);
            if (body->kind == NearbyBodies::Body::Rotor)
            {
                // By the blades' speed, full from standing on the ground, weaker towards 30 m
                const float above = body->bottom - place[2];
                const float rise = std::clamp((above - kRotorLow) / 0.7f, 0.0f, 1.0f), fall = 1.0f - above / kRotorHigh;
                v[7] *= rise * fall * fall * rotorPulse * body->rotorSpeed;
            }
        }
        pDevice->SetVertexShaderConstantF(kBodyRegister, values, 4);
        bBodiesBound = true;
        if (bLog)
            ++logPressedDraws;
    }

    // How far past its box a body moves the grass of a prop at `place`: a helicopter's downwash spreads as it climbs
    static float Reach(const NearbyBodies::Body& body, const float* place)
    {
        if (body.kind != NearbyBodies::Body::Rotor)
            return body.reach;
        return 6.0f + 0.4f * std::clamp(body.bottom - place[2], 0.0f, kRotorHigh);
    }

    static void EndGBuffer()
    {
        if (!bInGBuffer)
            return;
        bInGBuffer = false;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (pDevice && bBound)
            SetFlag(pDevice, false);
        if (pDevice && bTrampleBound)
            BindTrample(pDevice, false);
    }

    static void BeforeEntityDraw(EntityClass::Kind kind)
    {
        // Procedural props are buildings from the building pool; props placed by the map or scripts are objects
        if (!bInGBuffer || (kind != EntityClass::Kind::Building && kind != EntityClass::Kind::Prop))
            return;
        uint16_t model;
        const float* place;
        if (!EntityClass::CurrentModel(model, place))
            return;
        if (bLog)
            ++logCheckedDraws;
        const float height = GrassHeight(model);
        if (height <= 0.0f)
            return;
        auto pDevice = rage::grcDevice::GetD3DDevice();
        if (!pDevice)
            return;
        if (model != lastModel)
        {
            const float params[4] = { 1.0f / height, height * fBendPerHeight, height * fFlutterPerHeight, height * fPushPerHeight };
            pDevice->SetVertexShaderConstantF(kModelRegister, params, 1);
            lastModel = model;
        }
        if (fPushPerHeight > 0.0f)
            SetBodies(pDevice, model, place, height);
        SetFlag(pDevice, true);
        if (bLog)
        {
            ++logGrassDraws;
            logModels.insert(model);
        }
    }

    static void AfterEntityDraw(EntityClass::Kind)
    {
        if (!bBound)
            return;
        if (auto pDevice = rage::grcDevice::GetD3DDevice())
            SetFlag(pDevice, false);
    }

public:
    static bool IsGrass(uint32_t hash)
    {
        MakeModelListIfAsked();
        return grassHashes.contains(hash);
    }

    static int Kind(uint32_t hash)
    {
        MakeModelListIfAsked();
        const auto it = grassHashes.find(hash);
        return it != grassHashes.end() ? it->second : 0;
    }

    GrassWind()
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
            bBound = false;
            bTrampleBound = false;
        };
    }
} GrassWind;

namespace GrassShared
{
    bool IsGrassModel(uint32_t nameHash)
    {
        return GrassWind::IsGrass(nameHash);
    }

    int GrassKind(uint32_t nameHash)
    {
        return GrassWind::Kind(nameHash);
    }
}
