module;

#include <common.hxx>
#include <d3dx9.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
// left out of that thinning (ProceduralGround::DrawnInstanced).
//
// The game's draw command for them, CDrawEntityDC, keeps the entity's place as a quaternion (+0x10, x y z w) and a
// position (+0x20); its execute (CE 0x8dcbf0) makes the matrix from them (0x6708d0, rows at +0, +0x10, +0x20) and has
// the model's drawable draw itself with it: the vertex shaders take it as gWorld (c0-c3) and gWorldViewProj (c8-c11)
// (gta_default deferred VS4/VS5). In the G-buffer pass (EntityClass::SetEntityCommandHook) such a command far enough
// off is not executed but copied, grouped by model, fade alpha (+0x32, in kAlphaSteps steps), the two other scalars
// (+0x33, +0x34), the bucket (+0x35) and the flags (+0x36). At the end of the pass, before the depth copy and the
// motion vectors (TemporalAA::AppendBeforeSceneEnd), each group's first copy is executed once, turned to no rotation
// and moved to a point kSampleDistance in front of the camera, so gWorld is a translation by that point. Its device
// draws are made instanced: stream 1 holds per instance three rows that take a vertex from the model into the space of
// that point (the entity's rotation, its position less the point), and a copy of the draw's vertex shader, made once
// from the game's by disassembling and assembling it again (D3DX), puts the position, normal, tangent and binormal
// through them first. The rest of the shader is the game's: the world position, the projection, the wind's sway
// (b12, grasswind.ixx) and the fade come out as for the entity itself.
//
// A model whose draw cannot be instanced (a shader the copy cannot be made of, more than one vertex stream, a draw that
// is not indexed) is drawn the game's way from then on; for that frame the copies are executed one by one, as they are
// when the sample drew nothing. The drawable draws its highest detail for the sample, as the execute always asks.
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

    struct Copy { uint8_t dc[0x38]; };     // CDrawEntityDC's size
    struct Batch { UINT count = 0; UINT offset = 0; int draws = 0; bool failed = false; };

    static inline bool bEnabled = true;
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
    static inline std::unordered_map<uint64_t, std::vector<uint32_t>> groups;

    // By model: 0 not looked at, 1 drawn instanced, 2 not a candidate, 3 given up on. Written on the render thread, read
    // on the game thread (DrawnInstanced)
    static inline volatile uint8_t modelState[0x10000] = {};
    static inline uint8_t modelMisses[0x10000] = {};

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
    static inline uint64_t logInstances = 0, logGroups = 0, logDraws = 0, logOneByOne = 0, logCapped = 0;
    static inline uint32_t logShadersMade = 0, logShadersFailed = 0;

    static void ReadIni(CIniReader& iniReader)
    {
        bEnabled = iniReader.ReadInteger("PERFORMANCE", "FarPropInstancing", 1) != 0;
        bLog = iniReader.ReadInteger("DEBUG", "FarPropInstancingLog", 0) != 0;
    }

    static bool Candidate(uint16_t model)
    {
        auto state = modelState[model];
        if (state == 0)
        {
            const auto info = EntityClass::ModelInfo(model);
            if (!info)
                return false;
            state = GrassShared::GrassKind(*reinterpret_cast<const uint32_t*>(info + 0x3c)) != 0 ? uint8_t(1) : uint8_t(2);
            modelState[model] = state;
        }
        return state == 1;
    }

    static void GiveUp(uint16_t model, const char* why)
    {
        modelState[model] = 3;
        if (bLog)
        {
            const auto info = EntityClass::ModelInfo(model);
            FusionLog::Write("FarPropInstancing", "Model", "model %u (hash %08x) drawn the game's way: %s\n", model,
                info ? *reinterpret_cast<const uint32_t*>(info + 0x3c) : 0u, why);
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
            return true;
        const float* place = reinterpret_cast<const float*>(bytes + 0x20);
        const float dx = place[0] - cameraPos[0], dy = place[1] - cameraPos[1], dz = place[2] - cameraPos[2];
        if (dx * dx + dy * dy + dz * dz < kFromDistance * kFromDistance)
            return true;
        if (copies.size() >= kMaxInstances)
        {
            logCapped++;
            return true;
        }
        Copy copy;
        std::memcpy(copy.dc, bytes, sizeof(copy.dc));
        const uint8_t alpha = bytes[0x32];
        const uint64_t alphaStep = alpha == 255 ? kAlphaSteps : alpha * kAlphaSteps / 256;
        const uint64_t key = uint64_t(model) | alphaStep << 16 | uint64_t(bytes[0x33]) << 24 | uint64_t(bytes[0x34]) << 32 |
                             uint64_t(bytes[0x35]) << 40 | uint64_t(bytes[0x36]) << 48;
        groups[key].push_back(uint32_t(copies.size()));
        copies.push_back(copy);
        return false;
    }

    static void Execute(void* dc)
    {
        using Exec = void(__fastcall*)(void*, void*);
        (*reinterpret_cast<Exec**>(dc))[1](dc, nullptr);
    }

    static void ExecuteOneByOne(const std::vector<uint32_t>& items)
    {
        bExecuting = true;
        for (auto i : items)
            Execute(copies[i].dc);
        bExecuting = false;
        logOneByOne += items.size();
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
                if (PatchShaderText(source, patched))
                {
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
                    SAFE_RELEASE(binary);
                    SAFE_RELEASE(errors);
                }
            }
        }
        (made ? logShadersMade : logShadersFailed)++;
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
                for (const auto& e : elements)
                    usable &= e.Stream == 0 && !(e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex >= kInstanceUsage);
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

    static bool Instancing()
    {
        return bInstancing && GetCurrentThreadId() == instancingThread;
    }

    static HRESULT __stdcall DrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count)
    {
        if (!Instancing())
            return RealDrawIndexedPrimitive(device, type, base, minIndex, vertices, start, count);
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
            batch.failed = true;
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
            return RealDrawPrimitive(device, type, start, count);
        batch.failed = true;
        return S_OK;
    }

    static HRESULT __stdcall DrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride)
    {
        if (!Instancing())
            return RealDrawPrimitiveUP(device, type, count, data, stride);
        batch.failed = true;
        return S_OK;
    }

    static HRESULT __stdcall DrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT count, const void* indices,
                                                    D3DFORMAT format, const void* data, UINT stride)
    {
        if (!Instancing())
            return RealDrawIndexedPrimitiveUP(device, type, minIndex, vertices, count, indices, format, data, stride);
        batch.failed = true;
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
            const uint8_t* dc = copies[i].dc;
            float rotation[3][3];
            Rotation(dc, rotation);
            const float* place = reinterpret_cast<const float*>(dc + 0x20);
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

    // A point kSampleDistance along the camera's axis that projects into the view
    static bool SamplePoint(float point[3])
    {
        const auto& camera = FrameHistory::Current();
        D3DXMATRIX inverse;
        if (!camera.Valid || !D3DXMatrixInverse(&inverse, nullptr, &camera.View))
            return false;
        for (float side : { 1.0f, -1.0f })
        {
            const D3DXVECTOR3 p(inverse._41 + side * kSampleDistance * inverse._31, inverse._42 + side * kSampleDistance * inverse._32,
                                inverse._43 + side * kSampleDistance * inverse._33);
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

    static void Flush()
    {
        bInScene = false;
        if (groups.empty())
        {
            copies.clear();
            return;
        }
        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        float origin[3];
        const bool ready = device && InstallDrawHooks(device) && SamplePoint(origin);
        for (auto& [key, items] : groups)
        {
            const uint16_t model = uint16_t(key & 0xFFFF);
            logInstances += items.size();
            logGroups++;
            if (!ready || modelState[model] != 1 || !Upload(device, items, origin))
            {
                ExecuteOneByOne(items);
                continue;
            }
            Copy sample = copies[items[0]];
            static constexpr float identity[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            std::memcpy(sample.dc + 0x10, identity, sizeof(identity));
            std::memcpy(sample.dc + 0x20, origin, sizeof(origin));
            batch.draws = 0;
            batch.failed = false;
            instancingThread = GetCurrentThreadId();
            bExecuting = true;
            bInstancing = true;
            Execute(sample.dc);
            bInstancing = false;
            bExecuting = false;
            logDraws += batch.draws;
            if (batch.failed)
            {
                GiveUp(model, "a draw could not be instanced");
                ExecuteOneByOne(items);
            }
            else if (batch.draws == 0)
            {
                if (++modelMisses[model] >= kMissesToGiveUp)
                    GiveUp(model, "the sample drew nothing");
                ExecuteOneByOne(items);
            }
            else
                modelMisses[model] = 0;
        }
        groups.clear();
        copies.clear();

        if (bLog && ++logPasses >= kLogPasses)
        {
            uint32_t givenUp = 0;
            for (auto state : modelState)
                givenUp += state == 3;
            const double n = double(logPasses);
            FusionLog::Write("FarPropInstancing", "Stats", "%u passes; per pass: %.0f instances in %.1f groups, %.1f instanced draws, %.1f "
                "copies executed one by one, %.1f left to the game (store full); shader copies made %u, failed %u; models given up on %u\n",
                logPasses, logInstances / n, logGroups / n, logDraws / n, logOneByOne / n, logCapped / n, logShadersMade, logShadersFailed, givenUp);
            logPasses = 0;
            logInstances = logGroups = logDraws = logOneByOne = logCapped = 0;
        }
    }

    static void BeginScene()
    {
        bInScene = bEnabled;
        groups.clear();
        copies.clear();
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
            ProceduralGround::DrawnInstanced = &DrawnInstanced;
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
            copies.clear();
            ReleaseResources();
        };
    }
} FarPropInstancing;
