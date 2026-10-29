module;

#include <common.hxx>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "FusionLog.hpp"

export module shadowmerge;

import common;
import comvars;
import cpuscopes;
import entityclass;
import inireload;
import renderskip;

// Shadow Mesh Merge ([PERFORMANCE] ShadowMeshMerge): in the sun's cascades and the lamps' shadow maps a building's model
// draws each of its geometries on its own, one device draw each, though the pass draws most of them with the same depth
// shaders (its technique group picks each shader's depth technique): a frame capture by day found one building of 306
// draws in the cascades (about a hundred geometries, two textures) and 1703 draws there for 129 entities, 1565 in four
// lamp maps for 65. The draws were most of the render thread's time, the bound of the frame. Here a model's geometries
// that share a shader, a vertex declaration and a stride are put together into one vertex and index buffer (up to 65535
// vertices each) the first time the model is drawn in such a pass, and the game draws those instead, its own code drawing
// them with that shader as it does any geometry.
//
// The game's pieces (CE): grmModel::Draw (0x444c50, thiscall, the shader group, the bucket and a flag; ret 0xc) walks the
// model's geometries (+4 their array, +0x1a their count, +0x10 a u16 shader index each, +0xc a bounding sphere each after
// the model's own, which it tests against the viewport) and draws each with its shader, only those of the bucket asked
// for (the shader's +9). A forced shader ([0x17f59d4]) is not set in the shadow passes: a first build that merged only
// under one merged nothing. Skinned models go another way while [0x1b4f714] is set. A geometry (grmGeometryQB, Draw
// 0x463490): +0xc its vertex buffer, +0x1c its index buffer (four of each, the first unless +0x48), +0x2c the index count,
// +0x36 the primitive type (3 a triangle list), +0x40 the vertex declaration, +0x44 a second stream's buffer. Its draw
// (0x426180) binds the index buffer's D3D buffer (+0xc) with its count (+4) and the vertex buffer's (+0x1c) with its
// stride (+0xc) and vertex count (u16 +4) from their start. Both keep their data in memory (vertex buffer +0x20, CE
// 0x446140; index buffer +0x10, 16-bit, CE 0x471fb0) to make the D3D buffer again after a reset, which is what is read here.
//
// The model drawn instead is a copy of the game's with its geometry array, count and shader indices replaced (merged
// geometries first, then those left as they were) and no per-geometry spheres, so nothing is culled by them (a geometry
// outside the viewport draws nothing in a depth pass anyway). Each merged geometry is a copy of its group's first with
// copies of its buffers' objects holding the merged buffers (managed pool: a reset leaves them). Only buildings and
// props (no skinning), and not while Far Prop Instancing makes a sample's draws instanced.
//
// ShadowMeshMergeLog = 1 ([DEBUG]): every 600 frames, models merged and drawn, draws saved, the cache's size.
export namespace ShadowMerge
{
    // Far Prop Instancing: true while its sample's draws are made instanced (set at init)
    inline bool (*pInstancing)() = nullptr;
}

namespace ShadowMerge
{
    constexpr size_t kModelCopy = 0x20, kGeometryCopy = 0x50, kVertexBufferCopy = 0x40, kIndexBufferCopy = 0x20;
    constexpr uint32_t kMaxVertices = 65535;
    constexpr int kBuildsPerFrame = 6;
    constexpr uint32_t kUnusedFrames = 1800;        // an entry not drawn this long is let go
    constexpr size_t kMaxBytes = 192u << 20;
    constexpr uint32_t kLogFrames = 600;

    bool bEnabled = true, bLog = false;
    SafetyHookInline shModelDraw{};
    void** pSkinning = nullptr;         // [0x1b4f714]

    EntityClass::Kind currentKind = EntityClass::Kind::Unknown;
    bool inEntity = false;

    struct Merged
    {
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9* ib = nullptr;
        uint8_t vbObject[kVertexBufferCopy] = {}, ibObject[kIndexBufferCopy] = {}, geometry[kGeometryCopy] = {};
    };
    struct Entry
    {
        // what the model was when merged, checked at each draw: a model let go and its memory taken by another
        uintptr_t geometries = 0, firstGeometry = 0, firstVertexBuffer = 0;
        uint16_t count = 0;
        bool gain = false;      // something merged; else the model is drawn the game's way
        uint32_t used = 0;
        size_t bytes = 0;
        int saved = 0;          // draws fewer each time
        std::vector<Merged> merged;
        std::vector<uintptr_t> array;   // the copy's geometries: the merged ones' copies, then the game's left over
        std::vector<uint16_t> shaders;
        uint8_t model[kModelCopy] = {};
    };
    std::unordered_map<uintptr_t, Entry> entries;
    size_t totalBytes = 0;
    uint32_t frame = 0;
    int buildsThisFrame = 0;
    struct
    {
        uint64_t drawn = 0, saved = 0, built = 0, noGain = 0, failed = 0, letGo = 0;
        // models of buildings and props in a shadow pass drawn the game's way, and why
        uint64_t skinned = 0, instancing = 0, single = 0, budget = 0;
    } stats;

    template <typename T> T Read(uintptr_t address, size_t offset) { return *reinterpret_cast<const T*>(address + offset); }

    void Release(Entry& entry)
    {
        for (auto& m : entry.merged)
        {
            if (m.vb)
                m.vb->Release();
            if (m.ib)
                m.ib->Release();
        }
        entry.merged.clear();
        totalBytes -= (std::min)(totalBytes, entry.bytes);
        entry.bytes = 0;
    }

    bool ShadowPass()
    {
        const char* target = RenderSkip::CurrentTarget();
        return target && (std::strncmp(target, "CASCADE_TARGET", 14) == 0 || std::strncmp(target, "SHADOW_MAP_COLOUR", 17) == 0);
    }

    // A geometry that can go into a merged one: one stream, a triangle list, its data in memory
    bool Mergeable(uintptr_t g)
    {
        if (!g || !Read<uintptr_t>(g, 4) || Read<uintptr_t>(g, 0x44) || Read<uint32_t>(g, 0x48) || Read<uint16_t>(g, 0x36) != 3)
            return false;
        const uintptr_t vb = Read<uintptr_t>(g, 0xc), ib = Read<uintptr_t>(g, 0x1c);
        if (!vb || !ib || !Read<uintptr_t>(vb, 0x20) || !Read<uintptr_t>(ib, 0x10) || !Read<uint16_t>(vb, 4) || !Read<uint32_t>(vb, 0xc))
            return false;
        const uint32_t indices = Read<uint32_t>(g, 0x2c);
        return indices > 0 && indices % 3 == 0 && indices <= Read<uint32_t>(ib, 4);
    }

    bool Build(uintptr_t model, Entry& entry)
    {
        static const CpuScopes::Id kCpuScope("ShadowMerge.Build");
        CpuScopes::Scope cpuScope(kCpuScope);
        auto device = rage::grcDevice::GetD3DDevice();
        auto real = device ? RageDirect3DDevice9::RealDevice(device) : nullptr;
        if (!real)
            return false;
        const uintptr_t geometries = Read<uintptr_t>(model, 4);
        const uint16_t count = Read<uint16_t>(model, 0x1a);
        const uintptr_t shaderMap = Read<uintptr_t>(model, 0x10);
        // Groups by shader, declaration and stride, in the model's order: a merged geometry is drawn with its group's
        // shader, as each of them was
        struct Group { uint16_t shader; uintptr_t declaration; uint32_t stride; std::vector<int> members; };
        std::vector<Group> groups;
        std::vector<int> left;
        for (int i = 0; i < count; ++i)
        {
            const uintptr_t g = Read<uintptr_t>(geometries, i * 4);
            if (!Mergeable(g))
            {
                left.push_back(i);
                continue;
            }
            const uint16_t shader = shaderMap ? Read<uint16_t>(shaderMap, i * 2) : 0;
            const uintptr_t declaration = Read<uintptr_t>(g, 0x40);
            const uint32_t stride = Read<uint32_t>(Read<uintptr_t>(g, 0xc), 0xc);
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& x)
                { return x.shader == shader && x.declaration == declaration && x.stride == stride; });
            if (it == groups.end())
                groups.push_back({ shader, declaration, stride, { i } });
            else
                it->members.push_back(i);
        }
        entry.array.clear();
        entry.shaders.clear();
        int drawsAfter = 0;
        for (auto& group : groups)
        {
            if (group.members.size() < 2)
            {
                left.push_back(group.members[0]);
                continue;
            }
            // Chunks of at most kMaxVertices
            size_t at = 0;
            while (at < group.members.size())
            {
                uint32_t vertices = 0, indices = 0;
                size_t end = at;
                while (end < group.members.size())
                {
                    const uintptr_t g = Read<uintptr_t>(geometries, group.members[end] * 4);
                    const uint32_t v = Read<uint16_t>(Read<uintptr_t>(g, 0xc), 4);
                    if (vertices + v > kMaxVertices)
                        break;
                    vertices += v;
                    indices += Read<uint32_t>(g, 0x2c);
                    ++end;
                }
                if (end == at)  // (a geometry of 65535 vertices alone)
                {
                    left.push_back(group.members[at++]);
                    continue;
                }
                if (end - at == 1)
                {
                    left.push_back(group.members[at++]);
                    continue;
                }
                Merged m;
                if (FAILED(real->CreateVertexBuffer(vertices * group.stride, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m.vb, nullptr)) ||
                    FAILED(real->CreateIndexBuffer(indices * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &m.ib, nullptr)))
                {
                    if (m.vb)
                        m.vb->Release();
                    for (int k = int(at); k < int(end); ++k)
                        left.push_back(group.members[k]);
                    at = end;
                    continue;
                }
                uint8_t* vData = nullptr;
                uint16_t* iData = nullptr;
                bool locked = SUCCEEDED(m.vb->Lock(0, 0, reinterpret_cast<void**>(&vData), 0));
                locked = SUCCEEDED(m.ib->Lock(0, 0, reinterpret_cast<void**>(&iData), 0)) && locked;
                if (locked)
                {
                    uint32_t base = 0;
                    for (size_t k = at; k < end; ++k)
                    {
                        const uintptr_t g = Read<uintptr_t>(geometries, group.members[k] * 4);
                        const uintptr_t vb = Read<uintptr_t>(g, 0xc), ib = Read<uintptr_t>(g, 0x1c);
                        const uint32_t v = Read<uint16_t>(vb, 4), n = Read<uint32_t>(g, 0x2c);
                        std::memcpy(vData + size_t(base) * group.stride, Read<const uint8_t*>(vb, 0x20), size_t(v) * group.stride);
                        const uint16_t* source = Read<const uint16_t*>(ib, 0x10);
                        for (uint32_t j = 0; j < n; ++j)
                            *iData++ = uint16_t((std::min)(uint32_t(source[j]), v - 1) + base);
                        base += v;
                    }
                }
                if (vData)
                    m.vb->Unlock();
                if (iData)
                    m.ib->Unlock();
                if (!locked)
                {
                    m.vb->Release();
                    m.ib->Release();
                    for (int k = int(at); k < int(end); ++k)
                        left.push_back(group.members[k]);
                    at = end;
                    continue;
                }
                // The copies of the group's first geometry and its buffers' objects, holding the merged buffers
                const uintptr_t first = Read<uintptr_t>(geometries, group.members[at] * 4);
                std::memcpy(m.geometry, reinterpret_cast<const void*>(first), kGeometryCopy);
                std::memcpy(m.vbObject, reinterpret_cast<const void*>(Read<uintptr_t>(first, 0xc)), kVertexBufferCopy);
                std::memcpy(m.ibObject, reinterpret_cast<const void*>(Read<uintptr_t>(first, 0x1c)), kIndexBufferCopy);
                *reinterpret_cast<uint16_t*>(m.vbObject + 4) = uint16_t(vertices);
                *reinterpret_cast<IDirect3DVertexBuffer9**>(m.vbObject + 0x1c) = m.vb;
                *reinterpret_cast<uint32_t*>(m.ibObject + 4) = indices;
                *reinterpret_cast<IDirect3DIndexBuffer9**>(m.ibObject + 0xc) = m.ib;
                entry.bytes += size_t(vertices) * group.stride + size_t(indices) * 2;
                entry.merged.push_back(m);
                entry.shaders.push_back(group.shader);
                at = end;
                ++drawsAfter;
            }
        }
        // (pointers into entry.merged are taken once it no longer grows)
        for (auto& m : entry.merged)
        {
            for (int k = 0; k < 4; ++k)
            {
                *reinterpret_cast<uintptr_t*>(m.geometry + 0xc + k * 4) = reinterpret_cast<uintptr_t>(m.vbObject);
                *reinterpret_cast<uintptr_t*>(m.geometry + 0x1c + k * 4) = reinterpret_cast<uintptr_t>(m.ibObject);
            }
            *reinterpret_cast<uint32_t*>(m.geometry + 0x2c) = *reinterpret_cast<uint32_t*>(m.ibObject + 4);
            entry.array.push_back(reinterpret_cast<uintptr_t>(m.geometry));
        }
        std::sort(left.begin(), left.end());
        for (int i : left)
        {
            entry.array.push_back(Read<uintptr_t>(geometries, i * 4));
            entry.shaders.push_back(shaderMap ? Read<uint16_t>(shaderMap, i * 2) : 0);
        }
        drawsAfter += int(left.size());
        entry.gain = !entry.merged.empty();
        entry.saved = count - drawsAfter;
        if (entry.gain)
        {
            std::memcpy(entry.model, reinterpret_cast<const void*>(model), kModelCopy);
            *reinterpret_cast<uintptr_t*>(entry.model + 4) = reinterpret_cast<uintptr_t>(entry.array.data());
            *reinterpret_cast<uintptr_t*>(entry.model + 0xc) = 0;
            *reinterpret_cast<uintptr_t*>(entry.model + 0x10) = reinterpret_cast<uintptr_t>(entry.shaders.data());
            *reinterpret_cast<uint16_t*>(entry.model + 0x1a) = uint16_t(entry.array.size());
            totalBytes += entry.bytes;
        }
        return true;
    }

    // Lets go of the entries drawn least lately until the cache is under its size, and of those long unused
    void Trim()
    {
        for (auto it = entries.begin(); it != entries.end();)
            if (frame - it->second.used > kUnusedFrames)
            {
                Release(it->second);
                it = entries.erase(it);
                ++stats.letGo;
            }
            else
                ++it;
        while (totalBytes > kMaxBytes && !entries.empty())
        {
            auto oldest = std::min_element(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.second.used < b.second.used; });
            Release(oldest->second);
            entries.erase(oldest);
            ++stats.letGo;
        }
    }

    void __fastcall ModelDraw(void* model, void* edx, void* group, int32_t bucket, int32_t flag)
    {
        const uintptr_t m = reinterpret_cast<uintptr_t>(model);
        auto original = [&] { shModelDraw.unsafe_fastcall(model, edx, group, bucket, flag); };
        if (!bEnabled || !m || !inEntity || (currentKind != EntityClass::Kind::Building && currentKind != EntityClass::Kind::Prop))
            return original();
        if (!ShadowPass())
            return original();
        const uint16_t count = Read<uint16_t>(m, 0x1a);
        const uintptr_t geometries = Read<uintptr_t>(m, 4);
        uint64_t* reason = (pSkinning && *pSkinning) ? &stats.skinned
                         : (pInstancing && pInstancing()) ? &stats.instancing : (count < 2 || !geometries) ? &stats.single : nullptr;
        if (reason)
        {
            ++*reason;
            return original();
        }
        const uintptr_t firstGeometry = Read<uintptr_t>(geometries, 0);
        const uintptr_t firstVertexBuffer = firstGeometry ? Read<uintptr_t>(firstGeometry, 0xc) : 0;
        auto it = entries.find(m);
        if (it != entries.end() && (it->second.geometries != geometries || it->second.count != count ||
            it->second.firstGeometry != firstGeometry || it->second.firstVertexBuffer != firstVertexBuffer))
        {
            Release(it->second);
            entries.erase(it);
            it = entries.end();
        }
        if (it == entries.end())
        {
            if (buildsThisFrame >= kBuildsPerFrame)
            {
                ++stats.budget;
                return original();
            }
            ++buildsThisFrame;
            Entry entry;
            entry.geometries = geometries;
            entry.count = count;
            entry.firstGeometry = firstGeometry;
            entry.firstVertexBuffer = firstVertexBuffer;
            if (!Build(m, entry))
            {
                ++stats.failed;
                return original();
            }
            ++(entry.gain ? stats.built : stats.noGain);
            it = entries.emplace(m, std::move(entry)).first;
            if (it->second.gain)
            {
                // (the vectors moved with the entry: point the copy at their new storage)
                auto& e = it->second;
                *reinterpret_cast<uintptr_t*>(e.model + 4) = reinterpret_cast<uintptr_t>(e.array.data());
                *reinterpret_cast<uintptr_t*>(e.model + 0x10) = reinterpret_cast<uintptr_t>(e.shaders.data());
            }
        }
        auto& entry = it->second;
        entry.used = frame;
        if (!entry.gain)
            return original();
        ++stats.drawn;
        stats.saved += uint64_t(entry.saved);
        shModelDraw.unsafe_fastcall(entry.model, edx, group, bucket, flag);
    }

    void EndFrame()
    {
        ++frame;
        buildsThisFrame = 0;
        if (frame % 60 == 0)
            Trim();
        if (bLog && frame % kLogFrames == 0)
        {
            const double n = double(kLogFrames);
            FusionLog::Write("ShadowMerge", "Stats", "per frame over %u: %.1f merged models drawn, %.0f draws saved; drawn the game's way: "
                "skinned %.1f, instanced sample %.1f, one geometry %.1f, waiting to be built %.1f; built %llu, "
                "nothing to merge %llu, failed %llu, let go %llu; cache %zu models, %.1f MB\n", kLogFrames, stats.drawn / n, stats.saved / n,
                stats.skinned / n, stats.instancing / n, stats.single / n, stats.budget / n,
                stats.built, stats.noGain, stats.failed, stats.letGo, entries.size(), totalBytes / 1048576.0);
            stats = {};
        }
    }

    void ReadIni(CIniReader& iniReader)
    {
        bEnabled = iniReader.ReadInteger("PERFORMANCE", "ShadowMeshMerge", 1) != 0;
        bLog = iniReader.ReadInteger("DEBUG", "ShadowMeshMergeLog", 0) != 0;
    }
}

class ShadowMergeFeature
{
public:
    ShadowMergeFeature()
    {
        using namespace ShadowMerge;
        FusionFix::onInitEvent() += []()
        {
            CIniReader iniReader("");
            ReadIni(iniReader);
            IniReload::Add(ReadIni);
            // grmModel::Draw (CE 0x444c50): push ebp / mov ebp, esp / and esp, -8 / sub esp, 20h / mov eax, [skinning] / push esi /
            // push edi / mov edi, ecx / mov [esp+24h], eax / mov eax, [edi+0Ch] / test eax, eax / jz / cmp byte [x], 0 / jz /
            // push ecx / mov ecx, [viewport]; and at +0x78 the forced shader's read, mov edx, [forced], to check the build
            auto pattern = hook::pattern("55 8B EC 83 E4 F8 83 EC 20 A1 ? ? ? ? 56 57 8B F9 89 44 24 24 8B 47 0C 85 C0 74 ? 80 3D ? ? ? ? 00 74 ? 51 8B 0D");
            if (pattern.empty())
            {
                FusionLog::Write("ShadowMerge", "Init", "grmModel::Draw not found: off\n");
                return;
            }
            auto at = pattern.get_first<uint8_t>(0);
            if (at[0x78] != 0x8B || at[0x79] != 0x15)
            {
                FusionLog::Write("ShadowMerge", "Init", "grmModel::Draw is not as expected: off\n");
                return;
            }
            pSkinning = *reinterpret_cast<void***>(at + 10);
            shModelDraw = safetyhook::create_inline(at, reinterpret_cast<void*>(&ModelDraw));
            EntityClass::AddDrawHooks([](EntityClass::Kind kind) { currentKind = kind; inEntity = true; },
                                      [](EntityClass::Kind) { inEntity = false; });
            FusionFix::onBeforePresent() += []() { EndFrame(); };
        };
        // (managed buffers outlive a reset; nothing to let go)
    }
} ShadowMergeFeature;
