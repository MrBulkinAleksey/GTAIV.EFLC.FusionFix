module;

#include <common.hxx>
#include <d3dx9.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>
#include "FusionLog.hpp"

export module farpropinstancing;

import common;
import comvars;
import d3dx9_43;
import inireload;
import entityclass;
import framegeneration;
import framehistory;
import grasswind;
import procedural;
import temporal;
import vtablepatch;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

// Far Prop Instancing ([PERFORMANCE] FarPropInstancing): the grass, weeds and small plants the game scatters on the
// ground (procedural props, GrassShared::GrassKind) further than kFromDistance from the camera are drawn in the scene's
// G-buffer pass with one instanced draw per model instead of one draw command each. Thousands of them 50-200 m out
// took 2.5-3.5 ms of CPU a frame, which is why ProceduralSmallFarCull thinned them past 60 m; the models drawn here are
// left out of that thinning (ProceduralGround::DrawnInstanced). FarPropInstancing = 2 (the default) takes the rest of
// procedural.dat's PROCOBJ models too, the litter, rocks and rubble, but not those whose record has +0x37 set (they
// float on water); 1 the plants only, 0 none.
//
// The game's draw command for them, CDrawEntityDC, keeps the entity's place as a quaternion (+0x10, x y z w) and a
// position (+0x20); its execute (CE 0x8dcbf0) makes the matrix from them (0x6708d0, rows at +0, +0x10, +0x20) and has
// the model's drawable draw itself with it: the vertex shaders take it as gWorld (c0-c3) and gWorldViewProj (c8-c11)
// (gta_default deferred VS4/VS5). In the G-buffer pass (EntityClass::SetEntityCommandHook) such a command far enough
// off is not executed but copied, grouped by model, fade alpha (+0x32, in kAlphaSteps steps), the two other scalars
// (+0x33, +0x34), the bucket (+0x35) and the flags (+0x36). At the end of the pass, before the depth copy and the
// motion vectors (TemporalAA::AppendBeforeSceneEnd), each group's first copy is executed once, turned to no rotation
// and moved in front of the camera, its bounding sphere wholly in view (GroupOrigin), so gWorld is a translation by
// that point; it is drawn with the pass states its group's commands stood in (StatesHere). Its device
// draws are made instanced: stream 1 holds per instance three rows that take a vertex from the model into the space of
// that point (the entity's rotation, its position less the point), and a copy of the draw's vertex shader, made once
// from the game's by disassembling and assembling it again (D3DX), puts the position, normal, tangent and binormal
// through them first. The rest of the shader is the game's: the world position, the projection, the wind's sway
// (b12, grasswind.ixx) and the fade come out as for the entity itself.
//
// Only CDrawEntityDC is taken: an entity with a matrix of its own (entity +0x24 bit 0x4000000, a scaled prop) gets a
// command of 0x60 bytes holding that matrix instead (CE 0x8dae60, CEntity's add 0xa2fe70) and is drawn as before.
// A model whose draw cannot be instanced (a shader the copy cannot be made of, more than one vertex stream, a draw that
// is not indexed) is drawn the game's way from then on; for that frame the copies are executed one by one, as they are
// when the sample drew nothing. The drawable draws its highest detail for the sample, as the execute always asks: its
// draw (the wrapper at CE 0x6a1d10 -> 0x6a4320) reads that detail's mesh list at drawable +0x40 without a check, and a
// few props have none there (CJ_PROC_BRICK8-10), so for those the copies are executed one by one as well.
//
// FragmentInstancing = 1 ([PERFORMANCE]): objects with fragments the game draws whole without a cache entry
// (CDrawFragTypeDC, EntityClass::SetFragTypeCommandHook: benches, bins, posts) are taken the same way past
// kFromDistance. Each of their parts is a drawable of its own drawn with the object's matrix (CE 0x5fd190 -> 0x9ffee0 ->
// 0xaec7b0), so a breakable bench of 23 parts took 48 device draws (67 us of CPU) each; the command keeps the matrix's
// rows (+0x10, +0x20, +0x30) and position (+0x40), the sample gets identity rows. Its copies are grouped by model and the
// command's other fields (+0x50, the bucket mask +0x54, +0x5a-0x5c, EntityClass's mark +0x5f; +0x5d and +0x5e
// are never written).
//
// FarPropInstancingLog = 1 ([DEBUG]): every 600 passes, the instances, groups, instanced draws and copies executed
// one by one per pass, and the models given up on, to GTAIV.EFLC.FusionFix.FarPropInstancing.log.
class FarPropInstancing
{
    static constexpr float kFromDistance = 30.0f;       // metres from the camera
    static constexpr float kSampleDistance = 4.0f;
    static constexpr uint32_t kMaxInstances = 32768;    // a pass
    static constexpr int kAlphaSteps = 8;
    static constexpr UINT kInstanceStride = 48;         // three float4 rows
    static constexpr BYTE kInstanceUsage = 13;          // TEXCOORD13-15
    static constexpr uint32_t kLogPasses = 600;
    static constexpr int kMissesToGiveUp = 30;

    struct Copy { uint8_t dc[0x60]; bool frag; uint8_t states; };    // CDrawEntityDC's 0x38 bytes or CDrawFragTypeDC's 0x60
    struct FragKey
    {
        uint64_t a, b;
        bool operator==(const FragKey&) const = default;
    };
    struct FragKeyHash { size_t operator()(const FragKey& k) const { return std::hash<uint64_t>()(k.a ^ (k.b * 0x9E3779B97F4A7C15ull)); } };
    struct Batch { UINT count = 0; UINT offset = 0; int draws = 0; bool failed = false; const char* reason = ""; };

    static inline bool bEnabled = true;
    static inline bool bProps = true;
    static inline bool bFragments = true;
    static inline volatile bool bStatesStale = false;  // FarPropInstancing's mode changed: models looked at again
    static inline bool bLog = false;

    // Render thread
    static inline bool bInScene = false;
    static inline bool bExecuting = false;      // copies being executed: not taken again
    static inline bool bInstancing = false;     // the sample's draws are made instanced
    static inline DWORD instancingThread = 0;
    static inline Batch batch;
    static inline uint32_t cameraFrame = ~0u;
    static inline float cameraPos[3] = {};
    static inline bool cameraKnown = false;
    static inline std::vector<Copy> copies;
    static inline std::unordered_map<FragKey, std::vector<uint32_t>, FragKeyHash> groups;     // the key, the states
    static inline std::unordered_map<FragKey, std::vector<uint32_t>, FragKeyHash> fragGroups;

    // By model: 0 not looked at, 1 drawn instanced, 2 not a candidate, 3 given up on. Written on the render thread, read
    // on the game thread (DrawnInstanced)
    static inline volatile uint8_t modelState[0x10000] = {};
    static inline bool modelIsProp[0x10000] = {};       // a PROCOBJ model that is not a plant
    static inline std::vector<bool> procObjModels;      // PROCOBJ models instanced as props, made once the records are in
    static inline uint8_t modelMisses[0x10000] = {};
    // The same for the models of CDrawFragTypeDC copies: 0 not looked at, 1 drawn instanced, 3 given up on
    static inline uint8_t fragState[0x10000] = {};
    static inline uint8_t fragMisses[0x10000] = {};
    // By model, a bit per bucket (+0x35, 0-7): its commands there draw nothing, the game's own executes neither (the
    // game adds an entity to the lists of several buckets, a weed has meshes in one). Dropped then, looked at again
    // every kEmptyRecheck passes.
    static inline uint8_t emptyBuckets[0x10000] = {};
    static constexpr uint32_t kEmptyRecheck = 64;
    static inline uint32_t passCount = 0;
    static inline uint32_t deviceDraws = 0;     // the game's draws while copies are executed one by one

    static inline IDirect3DVertexBuffer9* instanceVB = nullptr;
    static inline UINT instanceCursor = 0;
    static inline std::unordered_map<IDirect3DVertexShader9*, IDirect3DVertexShader9*> shaders;      // the game's (held) -> ours, null if it failed
    static inline std::unordered_map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*> decls;

    static inline HRESULT(__stdcall* RealDrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;
    static inline HRESULT(__stdcall* RealDrawIndexedPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) = nullptr;
    static inline HRESULT(__stdcall* RealDrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT) = nullptr;
    static inline HRESULT(__stdcall* RealDrawIndexedPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) = nullptr;
    static inline void** hookedVTable = nullptr;

    // FarPropInstancingLog
    static inline uint32_t logPasses = 0;
    static inline uint64_t logInstances = 0, logGroups = 0, logDraws = 0, logOneByOne = 0, logCapped = 0, logEmptyDropped = 0;
    static inline uint64_t logPropInstances = 0, logNoMeshList = 0, logFragInstances = 0;
    static inline uint32_t logShadersMade = 0, logShadersFailed = 0;

    // The pass states the draw list sets around its buckets (CE 0x437140's types: 3 alpha test, 4 and 5 the blend, 8
    // alpha ref, 9 alpha func, 10 alpha blending, 23 blend op), as they were where each copy's command stood. The copies
    // are executed at the pass's end, after the list turned alpha blending on for its last bucket: a hydrant's sample
    // blended its G-buffer into the scene's (capture 2026-10-28: blend 1 5/6), and every fire hydrant and bin drawn from
    // it looked see-through. Each group's set goes on for its sample and its copies, and the pass's comes back after.
    static constexpr uint32_t kStateTypes[] = { 3, 4, 5, 8, 9, 10, 23 };
    struct States
    {
        uint32_t values[std::size(kStateTypes)];
        bool operator==(const States&) const = default;
    };
    static inline void(__cdecl* setPassState)(uint32_t type, uint32_t value) = nullptr;
    static inline const uint32_t* passStateCache = nullptr;
    static inline std::vector<States> stateSets;    // this pass's

    static States CurrentStates()
    {
        States now{};
        for (size_t i = 0; i < std::size(kStateTypes); ++i)
            now.values[i] = passStateCache[kStateTypes[i]];
        return now;
    }

    // The index of the states where a command stands now, 0xFF when they cannot be kept (the copy is not taken then)
    static uint8_t StatesHere()
    {
        if (!passStateCache || !setPassState)
            return 0xFF;
        const States now = CurrentStates();
        for (size_t i = 0; i < stateSets.size(); ++i)
            if (stateSets[i] == now)
                return uint8_t(i);
        if (stateSets.size() >= 0xFF)
            return 0xFF;
        stateSets.push_back(now);
        return uint8_t(stateSets.size() - 1);
    }

    static void SetStates(const States& states)
    {
        for (size_t i = 0; i < std::size(kStateTypes); ++i)
            if (passStateCache[kStateTypes[i]] != states.values[i])
                setPassState(kStateTypes[i], states.values[i]);
    }

    static void ReadIni(CIniReader& iniReader)
    {
        const int mode = std::clamp(iniReader.ReadInteger("PERFORMANCE", "FarPropInstancing", 2), 0, 2);
        if (bEnabled != (mode != 0) || bProps != (mode == 2))
            bStatesStale = true;
        bEnabled = mode != 0;
        bProps = mode == 2;
        bFragments = iniReader.ReadInteger("PERFORMANCE", "FragmentInstancing", 1) != 0;
        bLog = iniReader.ReadInteger("DEBUG", "FarPropInstancingLog", 0) != 0;
    }

    // The PROCOBJ models taken as props, false until procedural.dat's records are in
    static bool ProcObjProp(uint16_t model)
    {
        if (procObjModels.empty())
        {
            const uint8_t* defs = ProceduralGround::Definitions();
            if (!defs)
                return false;
            procObjModels.assign(0x10000, false);
            const uint32_t count = *reinterpret_cast<const uint32_t*>(defs + 4);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t* record = defs + 8 + size_t(i) * 0x44;
                const int32_t index = *reinterpret_cast<const int32_t*>(record + 4);
                if (index >= 0 && index <= 0xFFFF && record[0x37] == 0)
                    procObjModels[index] = true;
            }
        }
        return procObjModels[model];
    }

    static bool Candidate(uint16_t model)
    {
        auto state = modelState[model];
        if (state == 0)
        {
            const auto info = EntityClass::ModelInfo(model);
            if (!info)
                return false;
            const bool plant = GrassShared::GrassKind(*reinterpret_cast<const uint32_t*>(info + 0x3c)) != 0;
            const bool prop = !plant && bProps && ProcObjProp(model);
            if (!plant && !prop && bProps && procObjModels.empty())
                return false;   // not looked at until the records are in
            state = plant || prop ? uint8_t(1) : uint8_t(2);
            modelIsProp[model] = prop;
            modelState[model] = state;
        }
        return state == 1;
    }

    // The sample's draw reads the highest detail's mesh list (drawable +0x40) unchecked in the game's drawable draw
    static bool HasMeshList(uint16_t model)
    {
        static constexpr uint8_t kDrawWrapper[7] = { 0xFF, 0x74, 0x24, 0x10, 0x83, 0xC1, 0x10 };    // CE 0x6a1d10
        const auto drawable = EntityClass::ModelDrawable(model);
        if (!drawable)
            return false;
        const auto vtable = *reinterpret_cast<const uintptr_t*>(drawable);
        const auto draw = vtable ? *reinterpret_cast<const uint8_t* const*>(vtable + 0x20) : nullptr;
        if (!draw || std::memcmp(draw, kDrawWrapper, sizeof(kDrawWrapper)) != 0)
            return true;    // another drawable class: nothing known to check
        return *reinterpret_cast<const uintptr_t*>(drawable + 0x40) != 0;
    }

    static void GiveUp(uint16_t model, const char* why, bool frag = false)
    {
        if (frag)
            fragState[model] = 3;
        else
            modelState[model] = 3;
        if (bLog)
        {
            const auto info = EntityClass::ModelInfo(model);
            FusionLog::Write("FarPropInstancing", "Model", "%s model %u (hash %08x) drawn the game's way: %s\n", frag ? "fragment" : "prop",
                model, info ? *reinterpret_cast<const uint32_t*>(info + 0x3c) : 0u, why);
        }
    }

    // CDrawEntityDC's matrix as its execute makes it (CE 0x6708d0 from the quaternion it normalises): the rotation's rows
    static void Rotation(const uint8_t* dc, float rows[3][3])
    {
        float q[4];
        std::memcpy(q, dc + 0x10, sizeof(q));
        const float length2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        const float scale = (length2 > 0.0f ? 1.0f / std::sqrt(length2) : 1.0f) * 1.41421356f;
        const float x = q[0] * scale, y = q[1] * scale, z = q[2] * scale, w = q[3] * scale;
        rows[0][0] = 1.0f - (z * z + y * y);
        rows[0][1] = y * x + w * z;
        rows[0][2] = z * x - w * y;
        rows[1][0] = y * x - w * z;
        rows[1][1] = 1.0f - (z * z + x * x);
        rows[1][2] = w * x + z * y;
        rows[2][0] = w * y + z * x;
        rows[2][1] = z * y - w * x;
        rows[2][2] = 1.0f - (y * y + x * x);
    }

    // A copy's rotation rows and position
    static const float* Placement(const Copy& copy, float rows[3][3])
    {
        if (!copy.frag)
        {
            Rotation(copy.dc, rows);
            return reinterpret_cast<const float*>(copy.dc + 0x20);
        }
        for (int r = 0; r < 3; ++r)
            std::memcpy(rows[r], copy.dc + 0x10 + r * 0x10, 3 * sizeof(float));
        return reinterpret_cast<const float*>(copy.dc + 0x40);
    }

    // Far enough from the camera to be taken
    static bool Far(const float* place)
    {
        if (cameraFrame != FrameHistory::Frame())
        {
            cameraFrame = FrameHistory::Frame();
            const auto& camera = FrameHistory::Current();
            D3DXMATRIX inverse;
            cameraKnown = camera.Valid && D3DXMatrixInverse(&inverse, nullptr, &camera.View) != nullptr;
            if (cameraKnown)
                cameraPos[0] = inverse._41, cameraPos[1] = inverse._42, cameraPos[2] = inverse._43;
        }
        if (!cameraKnown)
            return false;
        const float dx = place[0] - cameraPos[0], dy = place[1] - cameraPos[1], dz = place[2] - cameraPos[2];
        return dx * dx + dy * dy + dz * dz >= kFromDistance * kFromDistance;
    }

    // EntityClass::SetFragTypeCommandHook: far objects with fragments drawn whole are copied instead of executed
    static bool OnFragTypeCommand(EntityClass::Kind kind, void* dc)
    {
        if (!bEnabled || !bFragments || !bInScene || bExecuting || kind != EntityClass::Kind::Prop)
            return true;
        const auto* bytes = static_cast<const uint8_t*>(dc);
        const uint16_t model = *reinterpret_cast<const uint16_t*>(bytes + 0x58);
        if (fragState[model] == 3)
            return true;
        if (!Far(reinterpret_cast<const float*>(bytes + 0x40)))
            return true;
        if (copies.size() >= kMaxInstances)
        {
            logCapped++;
            return true;
        }
        const uint8_t states = StatesHere();
        if (states == 0xFF)
            return true;
        fragState[model] = 1;
        Copy copy;
        std::memcpy(copy.dc, bytes, sizeof(copy.dc));
        copy.frag = true;
        copy.states = states;
        // +0x5d and +0x5e are left as they were by the constructor (any bytes, a group a bench before)
        uint64_t a = 0, b = 0;
        std::memcpy(&a, bytes + 0x50, 8);
        std::memcpy(&b, bytes + 0x58, 8);
        b = (b & ~(0xFFFFull << 40)) | uint64_t(states) << 40;
        fragGroups[{ a, b }].push_back(uint32_t(copies.size()));
        copies.push_back(copy);
        return false;
    }

    // EntityClass::SetEntityCommandHook: far candidates are copied instead of executed
    static bool OnCommand(EntityClass::Kind kind, void* dc)
    {
        if (!bEnabled || !bInScene || bExecuting)
            return true;
        if (kind != EntityClass::Kind::Building && kind != EntityClass::Kind::Prop)
            return true;
        const auto* bytes = static_cast<const uint8_t*>(dc);
        const uint16_t model = *reinterpret_cast<const uint16_t*>(bytes + 0x30);
        if (!Candidate(model))
            return true;
        const uint8_t bucket = bytes[0x35];
        if (bucket < 8 && (emptyBuckets[model] & (1u << bucket)) && (passCount + model) % kEmptyRecheck != 0)
        {
            logEmptyDropped++;
            return false;
        }
        if (!Far(reinterpret_cast<const float*>(bytes + 0x20)))
            return true;
        if (copies.size() >= kMaxInstances)
        {
            logCapped++;
            return true;
        }
        const uint8_t states = StatesHere();
        if (states == 0xFF)
            return true;
        Copy copy;
        std::memcpy(copy.dc, bytes, 0x38);
        copy.frag = false;
        copy.states = states;
        const uint8_t alpha = bytes[0x32];
        const uint64_t alphaStep = alpha == 255 ? kAlphaSteps : alpha * kAlphaSteps / 256;
        // +0x37 too: EntityClass marks the command there (ground surfaces' b15), and the sample's mark is the group's
        const uint64_t key = uint64_t(model) | alphaStep << 16 | uint64_t(bytes[0x33]) << 24 | uint64_t(bytes[0x34]) << 32 |
                             uint64_t(bytes[0x35]) << 40 | uint64_t(bytes[0x36]) << 48 | uint64_t(bytes[0x37]) << 56;
        groups[{ key, states }].push_back(uint32_t(copies.size()));
        copies.push_back(copy);
        return false;
    }

    static void Execute(void* dc)
    {
        using Exec = void(__fastcall*)(void*, void*);
        (*reinterpret_cast<Exec**>(dc))[1](dc, nullptr);
    }

    // The game's device draws they made
    static uint32_t ExecuteOneByOne(const std::vector<uint32_t>& items)
    {
        instancingThread = GetCurrentThreadId();
        deviceDraws = 0;
        bExecuting = true;
        const States pass = CurrentStates();
        for (auto i : items)
        {
            SetStates(stateSets[copies[i].states]);
            Execute(copies[i].dc);
        }
        SetStates(pass);
        bExecuting = false;
        logOneByOne += items.size();
        return deviceDraws;
    }

    // While copies are executed one by one on the render thread
    static void CountDraw()
    {
        if (bExecuting && !bInstancing && GetCurrentThreadId() == instancingThread && !FrameGeneration::PresentingInDraw())
            deviceDraws++;
    }

    // ---------------------------------------------------------------------------------------------
    // The vertex shader's copy: v(position), v(normal), v(tangent), v(binormal) read through the instance's rows

    static bool IsRegister(const std::string& line, size_t at, char letter)
    {
        return line[at] == letter && at + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[at + 1])) &&
               (at == 0 || !(std::isalnum(static_cast<unsigned char>(line[at - 1])) || line[at - 1] == '_'));
    }

    static bool PatchShaderText(const std::string& source, std::string& out)
    {
        std::vector<std::string> lines;
        for (size_t start = 0; start < source.size();)
        {
            size_t end = source.find('\n', start);
            if (end == std::string::npos)
                end = source.size();
            std::string line = source.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
            start = end + 1;
        }
        // Inputs by usage; the highest input and temporary registers
        int position = -1, directions[3] = { -1, -1, -1 };     // normal, tangent, binormal
        bool directionW[3] = {}, positionW = true;
        int maxInput = -1, maxTemp = -1;
        bool version = false;
        for (const auto& raw : lines)
        {
            const std::string line = raw.substr(0, raw.find("//"));
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos)
                continue;
            const std::string text = line.substr(first);
            if (text.rfind("vs_", 0) == 0)
            {
                if (text.rfind("vs_3_0", 0) != 0)
                    return false;
                version = true;
                continue;
            }
            if (text.rfind("dcl_", 0) == 0)
            {
                const size_t space = text.find_first_of(" \t");
                if (space == std::string::npos)
                    continue;
                const std::string usage = text.substr(4, space - 4);
                const size_t operand = text.find_first_not_of(" \t", space);
                if (operand == std::string::npos || text[operand] != 'v')
                    continue;
                const int index = std::atoi(text.c_str() + operand + 1);
                const size_t dot = text.find('.', operand);
                const bool hasW = dot == std::string::npos || text.find('w', dot) != std::string::npos;
                maxInput = (std::max)(maxInput, index);
                if (usage.rfind("texcoord", 0) == 0)
                {
                    const int usageIndex = usage.size() > 8 ? std::atoi(usage.c_str() + 8) : 0;
                    if (usageIndex >= kInstanceUsage)
                        return false;
                }
                if (usage == "position" || usage == "position0")
                    position = index, positionW = hasW;
                else if (usage == "normal" || usage == "normal0")
                    directions[0] = index, directionW[0] = hasW;
                else if (usage == "tangent" || usage == "tangent0")
                    directions[1] = index, directionW[1] = hasW;
                else if (usage == "binormal" || usage == "binormal0")
                    directions[2] = index, directionW[2] = hasW;
                continue;
            }
            if (line.find("v[") != std::string::npos)
                return false;
            for (size_t i = 0; i < line.size(); ++i)
                if (IsRegister(line, i, 'r'))
                    maxTemp = (std::max)(maxTemp, std::atoi(line.c_str() + i + 1));
        }
        if (!version || position < 0 || maxInput + 3 > 15)
            return false;
        // Temporaries: three for the rows, one for the position and one for each direction
        const int rows = maxTemp + 1, positionTemp = rows + 3;
        int directionTemp[3] = { -1, -1, -1 };
        int next = positionTemp + 1;
        for (int k = 0; k < 3; ++k)
            if (directions[k] >= 0)
                directionTemp[k] = next++;
        if (next - 1 > 31)
            return false;
        const int rowInput = maxInput + 1;
        const auto replaced = [&](int input) -> int
        {
            if (input == position)
                return positionTemp;
            for (int k = 0; k < 3; ++k)
                if (input == directions[k])
                    return directionTemp[k];
            return -1;
        };

        std::string declarations, prologue;
        for (int k = 0; k < 3; ++k)
        {
            declarations += "    dcl_texcoord" + std::to_string(kInstanceUsage + k) + " v" + std::to_string(rowInput + k) + "\n";
            prologue += "    mov r" + std::to_string(rows + k) + ", v" + std::to_string(rowInput + k) + "\n";
        }
        const auto transform = [&](int input, int temp, bool translate, bool hasW)
        {
            static constexpr const char* kAxes = "xyz";
            for (int k = 0; k < 3; ++k)
                prologue += std::string("    dp3 r") + std::to_string(temp) + "." + kAxes[k] + ", r" + std::to_string(rows + k) + ", v" + std::to_string(input) + "\n";
            if (translate)
                for (int k = 0; k < 3; ++k)
                    prologue += std::string("    add r") + std::to_string(temp) + "." + kAxes[k] + ", r" + std::to_string(temp) + "." + kAxes[k] +
                                ", r" + std::to_string(rows + k) + ".w\n";
            if (hasW)
                prologue += "    mov r" + std::to_string(temp) + ".w, v" + std::to_string(input) + ".w\n";
        };
        transform(position, positionTemp, true, positionW);
        for (int k = 0; k < 3; ++k)
            if (directions[k] >= 0)
                transform(directions[k], directionTemp[k], false, directionW[k]);

        out.clear();
        bool declared = false, started = false;
        for (const auto& raw : lines)
        {
            const size_t comment = raw.find("//");
            std::string line = raw.substr(0, comment);
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos)
            {
                out += raw + "\n";
                continue;
            }
            const std::string text = line.substr(first);
            if (text.rfind("vs_", 0) == 0)
            {
                out += line + "\n";
                if (!declared)
                    out += declarations, declared = true;
                continue;
            }
            if (text.rfind("dcl_", 0) == 0 || text.rfind("def", 0) == 0)
            {
                out += line + "\n";
                continue;
            }
            if (!started)
                out += prologue, started = true;
            std::string patched;
            for (size_t i = 0; i < line.size();)
            {
                if (IsRegister(line, i, 'v'))
                {
                    size_t j = i + 1;
                    while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])))
                        ++j;
                    const int temp = replaced(std::atoi(line.c_str() + i + 1));
                    if (temp >= 0)
                    {
                        patched += "r" + std::to_string(temp);
                        i = j;
                        continue;
                    }
                }
                patched += line[i++];
            }
            out += patched + "\n";
        }
        return declared && started;
    }

    static IDirect3DVertexShader9* InstancedShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader)
    {
        if (auto it = shaders.find(shader); it != shaders.end())
            return it->second;
        shader->AddRef();
        IDirect3DVertexShader9* made = nullptr;
        const char* why = "its code could not be read";
        std::string inputs;
        UINT size = 0;
        if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size)
        {
            std::vector<DWORD> code(size / 4 + 1);
            ID3DXBuffer* text = nullptr;
            if (SUCCEEDED(shader->GetFunction(code.data(), &size)) && SUCCEEDED(D3DXDisassembleShader(code.data(), FALSE, nullptr, &text)) && text)
            {
                std::string source(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
                text->Release();
                while (!source.empty() && source.back() == '\0')
                    source.pop_back();
                std::string patched;
                why = "its text could not be patched";
                // Its version, inputs and any relative input, for the log
                for (size_t at = 0; bLog && at < source.size();)
                {
                    size_t end = source.find('\n', at);
                    if (end == std::string::npos)
                        end = source.size();
                    const std::string line = source.substr(at, (std::min)(end, source.find("//", at)) - at);
                    const size_t first = line.find_first_not_of(" \t");
                    if (first != std::string::npos && (line.compare(first, 3, "vs_") == 0 || line.compare(first, 4, "dcl_") == 0 ||
                                                       line.find("v[", first) != std::string::npos))
                        inputs += (inputs.empty() ? "" : "; ") + line.substr(first);
                    at = end + 1;
                }
                if (PatchShaderText(source, patched))
                {
                    why = "the copy could not be made";
                    ID3DXBuffer* binary = nullptr;
                    ID3DXBuffer* errors = nullptr;
                    if (SUCCEEDED(D3DXAssembleShader(patched.c_str(), UINT(patched.size()), nullptr, nullptr, 0, &binary, &errors)) && binary)
                    {
                        if (FAILED(device->CreateVertexShader(static_cast<const DWORD*>(binary->GetBufferPointer()), &made)))
                            made = nullptr;
                    }
                    else if (bLog)
                        FusionLog::Write("FarPropInstancing", "Shader", "assembling the copy failed: %s\n",
                            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no error text");
                    if (!made)
                        why = "assembling or creating the copy failed";
                    SAFE_RELEASE(binary);
                    SAFE_RELEASE(errors);
                }
            }
        }
        (made ? logShadersMade : logShadersFailed)++;
        if (!made && bLog)
            FusionLog::Write("FarPropInstancing", "Shader", "no instanced copy of vertex shader %p: %s (%s)\n", static_cast<void*>(shader), why, inputs.c_str());
        shaders[shader] = made;
        return made;
    }

    static IDirect3DVertexDeclaration9* InstancedDecl(IDirect3DDevice9* device, IDirect3DVertexDeclaration9* decl)
    {
        if (auto it = decls.find(decl); it != decls.end())
            return it->second;
        decl->AddRef();
        IDirect3DVertexDeclaration9* made = nullptr;
        UINT count = 0;
        if (SUCCEEDED(decl->GetDeclaration(nullptr, &count)) && count)
        {
            std::vector<D3DVERTEXELEMENT9> elements(count);
            if (SUCCEEDED(decl->GetDeclaration(elements.data(), &count)))
            {
                while (!elements.empty() && elements.back().Stream == 0xFF)
                    elements.pop_back();
                bool usable = true;
                std::string list;
                for (const auto& e : elements)
                {
                    usable &= e.Stream == 0 && !(e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex >= kInstanceUsage);
                    list += std::format("{}s{} u{}.{} t{}", list.empty() ? "" : ", ", e.Stream, e.Usage, e.UsageIndex, e.Type);
                }
                if (!usable && bLog)
                    FusionLog::Write("FarPropInstancing", "Declaration", "no instanced copy of a declaration (stream, usage.index, type): %s\n", list.c_str());
                if (usable)
                {
                    for (BYTE k = 0; k < 3; ++k)
                        elements.push_back({ 1, WORD(k * 16), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, BYTE(kInstanceUsage + k) });
                    elements.push_back(D3DDECL_END());
                    if (FAILED(device->CreateVertexDeclaration(elements.data(), &made)))
                        made = nullptr;
                }
            }
        }
        decls[decl] = made;
        return made;
    }

    // ---------------------------------------------------------------------------------------------
    // Device draws on the real device: the sample's are made instanced, any it cannot be are dropped and fail the batch

    // Frame generation presents waiting frames from inside the render thread's draws (CheckPending), a sample's too: those
    // draws (the shown frame's sharpening from user memory, 2026-10-28) are no part of the sample, and failed its batch
    static bool Instancing()
    {
        return bInstancing && GetCurrentThreadId() == instancingThread && !FrameGeneration::PresentingInDraw();
    }

    static HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count)
    {
        if (!Instancing())
        {
            CountDraw();
            return RealDrawIndexedPrimitive(device, type, base, minIndex, vertices, start, count);
        }
        if (batch.failed)
            return S_OK;
        IDirect3DVertexShader9* shader = nullptr;
        IDirect3DVertexDeclaration9* decl = nullptr;
        device->GetVertexShader(&shader);
        device->GetVertexDeclaration(&decl);
        auto instancedShader = shader ? InstancedShader(device, shader) : nullptr;
        auto instancedDecl = decl ? InstancedDecl(device, decl) : nullptr;
        HRESULT result = S_OK;
        if (!instancedShader || !instancedDecl)
        {
            batch.failed = true;
            batch.reason = !instancedShader ? "its vertex shader has no instanced copy" : "its declaration has no instanced copy";
        }
        else
        {
            UINT frequency0 = 1, offset1 = 0, stride1 = 0;
            IDirect3DVertexBuffer9* stream1 = nullptr;
            device->GetStreamSourceFreq(0, &frequency0);
            device->GetStreamSource(1, &stream1, &offset1, &stride1);
            device->SetVertexShader(instancedShader);
            device->SetVertexDeclaration(instancedDecl);
            device->SetStreamSource(1, instanceVB, batch.offset, kInstanceStride);
            device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | batch.count);
            device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
            result = RealDrawIndexedPrimitive(device, type, base, minIndex, vertices, start, count);
            device->SetStreamSourceFreq(0, frequency0);
            device->SetStreamSourceFreq(1, 1);
            device->SetStreamSource(1, stream1, offset1, stride1);
            device->SetVertexShader(shader);
            device->SetVertexDeclaration(decl);
            SAFE_RELEASE(stream1);
            batch.draws++;
        }
        SAFE_RELEASE(shader);
        SAFE_RELEASE(decl);
        return result;
    }

    static HRESULT __stdcall DrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count)
    {
        if (!Instancing())
        {
            CountDraw();
            return RealDrawPrimitive(device, type, start, count);
        }
        batch.failed = true;
        batch.reason = "a draw is not indexed";
        return S_OK;
    }

    static HRESULT __stdcall DrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
    {
        if (!Instancing())
        {
            CountDraw();
            return RealDrawPrimitiveUP(device, type, count, data, stride);
        }
        batch.failed = true;
        batch.reason = "a draw is from user memory";
        return S_OK;
    }

    static HRESULT __stdcall DrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT count, const void* indices,
                                                    D3DFORMAT format, const void* data, UINT stride)
    {
        if (!Instancing())
        {
            CountDraw();
            return RealDrawIndexedPrimitiveUP(device, type, minIndex, vertices, count, indices, format, data, stride);
        }
        batch.failed = true;
        batch.reason = "a draw is from user memory";
        return S_OK;
    }

    static bool InstallDrawHooks(IDirect3DDevice9* device)
    {
        auto vtable = *reinterpret_cast<void***>(device);
        if (vtable != hookedVTable)
        {
            PatchVTable(vtable, 81, RealDrawPrimitive, &DrawPrimitive);
            PatchVTable(vtable, 82, RealDrawIndexedPrimitive, &DrawIndexedPrimitive);
            PatchVTable(vtable, 83, RealDrawPrimitiveUP, &DrawPrimitiveUP);
            PatchVTable(vtable, 84, RealDrawIndexedPrimitiveUP, &DrawIndexedPrimitiveUP);
            hookedVTable = vtable;
        }
        return RealDrawIndexedPrimitive != nullptr;
    }

    // ---------------------------------------------------------------------------------------------

    static bool Upload(IDirect3DDevice9* device, const std::vector<uint32_t>& items, const float* origin)
    {
        if (!instanceVB)
        {
            if (FAILED(device->CreateVertexBuffer(kMaxInstances * kInstanceStride * 2, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &instanceVB, nullptr)))
            {
                instanceVB = nullptr;
                return false;
            }
            instanceCursor = 0;
        }
        const UINT size = UINT(items.size()) * kInstanceStride;
        DWORD flags = D3DLOCK_NOOVERWRITE;
        if (instanceCursor + size > kMaxInstances * kInstanceStride * 2)
            instanceCursor = 0, flags = D3DLOCK_DISCARD;
        void* data = nullptr;
        if (FAILED(instanceVB->Lock(instanceCursor, size, &data, flags)) || !data)
            return false;
        // Written in order, never read back (write-combined memory)
        auto* out = static_cast<float*>(data);
        for (auto i : items)
        {
            float rotation[3][3];
            const float* place = Placement(copies[i], rotation);
            // Row k: the world's axis k from the model's x, y, z (the matrix's rows), and the position less the origin
            for (int k = 0; k < 3; ++k)
            {
                out[0] = rotation[0][k];
                out[1] = rotation[1][k];
                out[2] = rotation[2][k];
                out[3] = place[k] - origin[k];
                out += 4;
            }
        }
        instanceVB->Unlock();
        batch.offset = instanceCursor;
        batch.count = UINT(items.size());
        instanceCursor += size;
        return true;
    }

    // A point `distance` along the camera's axis that projects into the view
    static bool SamplePoint(float distance, float point[3])
    {
        const auto& camera = FrameHistory::Current();
        D3DXMATRIX inverse;
        if (!camera.Valid || !D3DXMatrixInverse(&inverse, nullptr, &camera.View))
            return false;
        for (float side : { 1.0f, -1.0f })
        {
            const D3DXVECTOR3 p(inverse._41 + side * distance * inverse._31, inverse._42 + side * distance * inverse._32,
                                inverse._43 + side * distance * inverse._33);
            D3DXVECTOR4 clip;
            D3DXVec3Transform(&clip, &p, &camera.ViewProjection);
            if (clip.w > 0.0f && std::abs(clip.x) < clip.w && std::abs(clip.y) < clip.w)
            {
                point[0] = p.x, point[1] = p.y, point[2] = p.z;
                return true;
            }
        }
        return false;
    }

    // The group's first copy at the sample point with no rotation, its device draws instanced: batch tells how it went
    static void ExecuteSample(const std::vector<uint32_t>& items, const float* origin)
    {
        Copy sample = copies[items[0]];
        if (!sample.frag)
        {
            static constexpr float identity[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            std::memcpy(sample.dc + 0x10, identity, sizeof(identity));
            std::memcpy(sample.dc + 0x20, origin, 3 * sizeof(float));
        }
        else
        {
            for (int r = 0; r < 3; ++r)
            {
                const float row[3] = { r == 0 ? 1.0f : 0.0f, r == 1 ? 1.0f : 0.0f, r == 2 ? 1.0f : 0.0f };
                std::memcpy(sample.dc + 0x10 + r * 0x10, row, sizeof(row));
            }
            std::memcpy(sample.dc + 0x40, origin, 3 * sizeof(float));
        }
        batch.draws = 0;
        batch.failed = false;
        batch.reason = "";
        instancingThread = GetCurrentThreadId();
        bExecuting = true;
        bInstancing = true;
        const States pass = CurrentStates();
        SetStates(stateSets[sample.states]);
        Execute(sample.dc);
        SetStates(pass);
        bInstancing = false;
        bExecuting = false;
        logDraws += batch.draws;
    }

    // Where a group's sample goes: its model's bounding sphere (model info +0x10 centre, +0x1c radius) on the camera's
    // axis, far enough to be wholly in view. The drawables leave out what is out of view part by part: a street lamp's
    // sample 4 m off kept only the pole, its arm and head above the view, and so did every lamp drawn from it.
    static bool GroupOrigin(uint16_t model, float origin[3])
    {
        float centre[3] = {}, radius = 0.0f;
        if (const auto info = EntityClass::ModelInfo(model))
        {
            std::memcpy(centre, reinterpret_cast<const void*>(info + 0x10), sizeof(centre));
            radius = *reinterpret_cast<const float*>(info + 0x1c);
            if (!(radius > 0.0f && radius < 200.0f) || !std::isfinite(centre[0] + centre[1] + centre[2]))
                radius = 0.0f, centre[0] = centre[1] = centre[2] = 0.0f;
        }
        const auto& projection = FrameHistory::Current().Projection;
        const float tanHalf = projection._11 > 0.0f && projection._22 > 0.0f ? (std::min)(1.0f / projection._11, 1.0f / projection._22) : 0.0f;
        float distance = kSampleDistance;
        if (radius > 0.0f && tanHalf > 0.0f)
            distance = (std::max)(distance, radius * 1.1f * std::sqrt(1.0f + tanHalf * tanHalf) / tanHalf);
        float point[3];
        if (!SamplePoint(distance, point))
            return false;
        for (int k = 0; k < 3; ++k)
            origin[k] = point[k] - centre[k];
        return true;
    }

    static void Flush()
    {
        bInScene = false;
        passCount++;
        if (groups.empty() && fragGroups.empty())
        {
            copies.clear();
            stateSets.clear();
            return;
        }
        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        float origin[3];
        const bool ready = device && InstallDrawHooks(device) && SamplePoint(kSampleDistance, origin);
        for (auto& [groupKey, items] : groups)
        {
            const uint64_t key = groupKey.a;
            const uint16_t model = uint16_t(key & 0xFFFF);
            const uint8_t bucket = uint8_t(key >> 40), bucketBit = bucket < 8 ? uint8_t(1u << bucket) : uint8_t(0);
            logInstances += items.size();
            if (modelIsProp[model])
                logPropInstances += items.size();
            logGroups++;
            if (ready && modelState[model] == 1 && !HasMeshList(model))
            {
                logNoMeshList += items.size();
                ExecuteOneByOne(items);
                continue;
            }
            if (!ready || modelState[model] != 1 || !GroupOrigin(model, origin) || !Upload(device, items, origin))
            {
                ExecuteOneByOne(items);
                continue;
            }
            ExecuteSample(items, origin);
            if (batch.failed)
            {
                GiveUp(model, batch.reason);
                ExecuteOneByOne(items);
            }
            else if (batch.draws == 0)
            {
                // Nothing drawn one by one either: a bucket the model has no meshes in, not a sample out of view
                if (ExecuteOneByOne(items) == 0)
                    emptyBuckets[model] |= bucketBit;
                else
                {
                    emptyBuckets[model] &= uint8_t(~bucketBit);
                    if (++modelMisses[model] >= kMissesToGiveUp)
                        GiveUp(model, "the sample drew nothing where its copies did");
                }
            }
            else
            {
                emptyBuckets[model] &= uint8_t(~bucketBit);
                modelMisses[model] = 0;
            }
        }
        for (auto& [key, items] : fragGroups)
        {
            const uint16_t model = uint16_t(key.b & 0xFFFF);
            logInstances += items.size();
            logFragInstances += items.size();
            logGroups++;
            if (!ready || fragState[model] != 1 || !GroupOrigin(model, origin) || !Upload(device, items, origin))
            {
                ExecuteOneByOne(items);
                continue;
            }
            ExecuteSample(items, origin);
            if (batch.failed)
            {
                GiveUp(model, batch.reason, true);
                ExecuteOneByOne(items);
            }
            else if (batch.draws == 0)
            {
                // The bucket mask is in the group's key: a sample that drew nothing where its copies do is a miss
                if (ExecuteOneByOne(items) != 0 && ++fragMisses[model] >= kMissesToGiveUp)
                    GiveUp(model, "the sample drew nothing where its copies did", true);
            }
            else
                fragMisses[model] = 0;
        }
        groups.clear();
        fragGroups.clear();
        copies.clear();
        stateSets.clear();

        if (bLog && ++logPasses >= kLogPasses)
        {
            uint32_t givenUp = 0;
            for (auto state : modelState)
                givenUp += state == 3;
            for (auto state : fragState)
                givenUp += state == 3;
            const double n = double(logPasses);
            FusionLog::Write("FarPropInstancing", "Stats", "%u passes; per pass: %.0f instances (%.0f of them props, %.0f objects with fragments) in %.1f groups, "
                "%.1f instanced draws, %.1f copies executed one by one (%.1f of models without a mesh list), %.1f dropped (a bucket "
                "the model draws nothing in), %.1f left to the game (store full); shader copies made %u, failed %u; models given up on %u\n",
                logPasses, logInstances / n, logPropInstances / n, logFragInstances / n, logGroups / n, logDraws / n, logOneByOne / n, logNoMeshList / n,
                logEmptyDropped / n, logCapped / n, logShadersMade, logShadersFailed, givenUp);
            logPasses = 0;
            logInstances = logGroups = logDraws = logOneByOne = logCapped = logEmptyDropped = logPropInstances = logNoMeshList = logFragInstances = 0;
        }
    }

    static void BeginScene()
    {
        if (bStatesStale)
        {
            bStatesStale = false;
            for (auto& state : modelState)
                if (state != 3)
                    state = 0;
            for (auto& state : fragState)
                if (state != 3)
                    state = 0;
        }
        bInScene = bEnabled;
        groups.clear();
        fragGroups.clear();
        copies.clear();
        stateSets.clear();
    }

    static void ReleaseResources()
    {
        SAFE_RELEASE(instanceVB);
        for (auto& [game, ours] : shaders)
        {
            SAFE_RELEASE(ours);
            game->Release();
        }
        shaders.clear();
        for (auto& [game, ours] : decls)
        {
            SAFE_RELEASE(ours);
            game->Release();
        }
        decls.clear();
    }

public:
    // Any thread
    static bool DrawnInstanced(uint16_t model)
    {
        return bEnabled && modelState[model] == 1;
    }

    FarPropInstancing()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
            EntityClass::SetEntityCommandHook(&OnCommand);
            EntityClass::SetFragTypeCommandHook(&OnFragTypeCommand);
            ProceduralGround::DrawnInstanced = &DrawnInstanced;
            // The pass state setter (CE 0x437140): mov eax, [esp+4] / mov edx, [esp+8] / push esi / mov esi, [types + eax*4] /
            // mov [values + eax*4], edx / cmp esi, -1 / je; without it nothing is taken
            if (auto passState = hook::pattern("8B 44 24 04 8B 54 24 08 56 8B 34 85 ? ? ? ? 89 14 85 ? ? ? ? 83 FE FF 74"); !passState.empty())
            {
                setPassState = passState.get_first<void(__cdecl)(uint32_t, uint32_t)>(0);
                passStateCache = *passState.get_first<const uint32_t*>(19);
            }
        };

        FusionFix::onGameInitEvent() += []()
        {
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(BeginScene);
                if (cb)
                    cb->Append();
            };
            // Before the end of the camera scene's pass (depth copy, motion vectors); for any other the end of its list
            TemporalAA::AppendBeforeSceneEnd = []()
            {
                auto cb = new T_CB_Generic_NoArgs(Flush);
                if (cb)
                    cb->Append();
            };
            CRenderPhaseDeferredLighting_SceneToGBuffer::OnAfterBuildRenderList() += []()
            {
                auto cb = new T_CB_Generic_NoArgs(Flush);
                if (cb)
                    cb->Append();
            };
        };

        FusionFix::onBeforeReset() += []()
        {
            bInScene = false;
            bInstancing = false;
            bExecuting = false;
            groups.clear();
            fragGroups.clear();
            copies.clear();
            stateSets.clear();
            ReleaseResources();
        };
    }
} FarPropInstancing;
