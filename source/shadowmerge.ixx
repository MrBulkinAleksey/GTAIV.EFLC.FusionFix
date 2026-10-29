module;

#include <common.hxx>
#include <algorithm>
#include <cstring>
#include <format>
#include <string>
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
// that draw alike in a depth pass (an opaque shader's effect, any other shader itself; ShaderKey) and share a vertex
// declaration and a stride are put together into one vertex and index buffer (up to 65535 vertices each) the first time
// the model is drawn in such a pass, and the game draws those instead, its own code drawing them with the first one's
// shader as it does any geometry. By shader alone a second build merged 6 of 39 models (147 draws a frame saved): a
// building's geometries mostly have shaders of their own, one material each.
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
    constexpr size_t kDrawableCopy = 0x60, kModelCopy = 0x20, kGeometryCopy = 0x50, kVertexBufferCopy = 0x40, kIndexBufferCopy = 0x20;
    constexpr uint32_t kMaxVertices = 65535;
    constexpr int kBuildsPerFrame = 6;
    constexpr double kBuildMsPerFrame = 0.5;      // no further build once a frame's builds took this long (the first one always)
    constexpr uint32_t kUnusedFrames = 1800;        // an entry not drawn this long is let go
    constexpr size_t kMaxBytes = 192u << 20;
    constexpr uint32_t kLogFrames = 600;

    bool bEnabled = true, bLog = false;
    SafetyHookInline shModelDraw{}, shDrawNoShaders{};
    void** pSkinning = nullptr;         // [0x1b4f714]

    EntityClass::Kind currentKind = EntityClass::Kind::Unknown;
    bool inEntity = false;

    struct Merged
    {
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9* ib = nullptr;
        // the geometry copied (its index in the model) and its vertex declaration, checked at each draw (Current)
        int source = 0;
        uintptr_t declaration = 0;
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
    std::unordered_map<uint64_t, Entry> entries;    // by model, and 1 << 32 for those grouped by bucket
    size_t totalBytes = 0;
    uint32_t frame = 0;
    int buildsThisFrame = 0;
    int64_t buildTicksThisFrame = 0, buildBudgetTicks = 0;
    LARGE_INTEGER frequency{};
    struct
    {
        uint64_t drawn = 0, saved = 0, built = 0, noGain = 0, failed = 0, letGo = 0;
        // the longest build, its model's geometries and what it holds
        int64_t slowestTicks = 0;
        uint16_t slowestCount = 0;
        size_t slowestBytes = 0;
        // models of buildings and props in a shadow pass drawn the game's way, and why
        uint64_t skinned = 0, instancing = 0, single = 0, budget = 0, nothing = 0;
        // all model draws, those outside an entity's draw, of another kind, outside a shadow pass
        uint64_t calls = 0, outside = 0, otherKind = 0, otherPass = 0;
    } stats;
    // The targets model draws of buildings and props were seen in, for the log (render thread)
    std::unordered_map<std::string, uint64_t> targetsSeen;

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

    // What a geometry's shader draws in a depth pass: an opaque one (bucket 0, +9) any shader of its effect alike (the effect
    // name, +0x44, hashed), as its textures and colours do not touch the depth; any other its own shader (an alpha test
    // reads its texture). Read under SEH: the pointers are the game's
    uint64_t ShaderKey(uintptr_t group, uint16_t index)
    {
        __try
        {
            const auto shaders = *reinterpret_cast<const uintptr_t* const*>(group + 8);
            const uintptr_t shader = shaders && index < *reinterpret_cast<const uint16_t*>(group + 0xc) ? shaders[index] : 0;
            if (!shader)
                return index;
            const char* name = *reinterpret_cast<const char* const*>(shader + 0x44);
            if (*reinterpret_cast<const uint8_t*>(shader + 9) != 0 || !name)
                return index;
            uint64_t hash = 0xcbf29ce484222325ull;
            for (int i = 0; i < 64 && name[i]; ++i)
                hash = (hash ^ uint8_t(name[i])) * 0x100000001b3ull;
            return hash | (1ull << 63);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return index;
        }
    }

    // A shader's bucket (+9), for a draw without shaders of their own; under SEH
    uint64_t BucketKey(uintptr_t group, uint16_t index)
    {
        __try
        {
            const auto shaders = *reinterpret_cast<const uintptr_t* const*>(group + 8);
            const uintptr_t shader = shaders && index < *reinterpret_cast<const uint16_t*>(group + 0xc) ? shaders[index] : 0;
            return shader ? (2ull << 62) | *reinterpret_cast<const uint8_t*>(shader + 9) : index;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return index;
        }
    }

    bool Build(uintptr_t model, uintptr_t shaderGroup, bool byBucket, Entry& entry)
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
        // Groups by what their shaders draw in a depth pass (ShaderKey), declaration and stride, in the model's order: a
        // merged geometry is drawn with its first's shader
        struct Group { uint64_t key; uint16_t shader; uintptr_t declaration; uint32_t stride; std::vector<int> members; };
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
            const uint64_t key = !shaderGroup ? shader : byBucket ? BucketKey(shaderGroup, shader) : ShaderKey(shaderGroup, shader);
            const uintptr_t declaration = Read<uintptr_t>(g, 0x40);
            const uint32_t stride = Read<uint32_t>(Read<uintptr_t>(g, 0xc), 0xc);
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& x)
                { return x.key == key && x.declaration == declaration && x.stride == stride; });
            if (it == groups.end())
                groups.push_back({ key, shader, declaration, stride, { i } });
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
                // No data in memory and not dirty: a buffer flagged dirty (vertex buffer +0x28 bit 0x100, index buffer +0x14 bit 2) is filled again
                // at its draw from its data in memory (CE 0x446140 / 0x471fb0), which the copies' counts would overread
                // (the first geometry's data, smaller than the merged buffer)
                *reinterpret_cast<uintptr_t*>(m.vbObject + 0x20) = 0;
                *reinterpret_cast<uint32_t*>(m.vbObject + 0x28) &= ~0x100u;
                *reinterpret_cast<uintptr_t*>(m.ibObject + 0x10) = 0;
                *reinterpret_cast<uint32_t*>(m.ibObject + 0x14) &= ~0x2u;
                m.source = group.members[at];
                m.declaration = Read<uintptr_t>(first, 0x40);
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

    // Whether the merged geometries' copies still hold the model's vertex declarations: a declaration (geometry +0x40) is
    // the game's, counted by the geometries holding it (+4) and let go with the last of them (CE 0x45f540 -> 0x45ed50 ->
    // 0x422cd0), and the copies hold none. A model let go and loaded again where it was keeps its geometries' and buffers'
    // places (the checks in Lookup) but may take a declaration made anew, which the copies would draw with let go
    bool Current(const Entry& entry, uintptr_t geometries)
    {
        for (auto& m : entry.merged)
        {
            const uintptr_t g = Read<uintptr_t>(geometries, m.source * 4);
            if (!g || Read<uintptr_t>(g, 0x40) != m.declaration)
                return false;
        }
        return true;
    }

    // The model's entry, built if need be (false: drawn the game's way, the budget spent or nothing to merge). byBucket:
    // grouped for a draw without shaders of their own (DrawNoShaders)
    const Entry* Lookup(uintptr_t m, uintptr_t group, bool byBucket)
    {
        const uint16_t count = Read<uint16_t>(m, 0x1a);
        const uintptr_t geometries = Read<uintptr_t>(m, 4);
        if (count < 2 || !geometries)
        {
            ++stats.single;
            return nullptr;
        }
        const uintptr_t firstGeometry = Read<uintptr_t>(geometries, 0);
        const uintptr_t firstVertexBuffer = firstGeometry ? Read<uintptr_t>(firstGeometry, 0xc) : 0;
        const uint64_t key = uint64_t(m) | (byBucket ? 1ull << 32 : 0);
        auto it = entries.find(key);
        if (it != entries.end() && (it->second.geometries != geometries || it->second.count != count ||
            it->second.firstGeometry != firstGeometry || it->second.firstVertexBuffer != firstVertexBuffer || !Current(it->second, geometries)))
        {
            Release(it->second);
            entries.erase(it);
            it = entries.end();
        }
        if (it == entries.end())
        {
            if (buildsThisFrame >= kBuildsPerFrame || (buildsThisFrame > 0 && buildTicksThisFrame >= buildBudgetTicks))
            {
                ++stats.budget;
                return nullptr;
            }
            ++buildsThisFrame;
            Entry entry;
            entry.geometries = geometries;
            entry.count = count;
            entry.firstGeometry = firstGeometry;
            entry.firstVertexBuffer = firstVertexBuffer;
            LARGE_INTEGER start, end;
            QueryPerformanceCounter(&start);
            const bool built = Build(m, group, byBucket, entry);
            QueryPerformanceCounter(&end);
            const int64_t ticks = end.QuadPart - start.QuadPart;
            buildTicksThisFrame += ticks;
            if (ticks > stats.slowestTicks)
            {
                stats.slowestTicks = ticks;
                stats.slowestCount = count;
                stats.slowestBytes = entry.bytes;
            }
            if (!built)
            {
                ++stats.failed;
                return nullptr;
            }
            ++(entry.gain ? stats.built : stats.noGain);
            it = entries.emplace(key, std::move(entry)).first;
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
        {
            ++stats.nothing;
            return nullptr;
        }
        ++stats.drawn;
        stats.saved += uint64_t(entry.saved);
        return &entry;
    }

    // G-buffer counts (ShadowMeshMergeLog; nothing drawn otherwise): what merging a building's or prop's geometries there would
    // save, a frame capture one evening having found a third of the buildings' 1527 G-buffer draws repeating an earlier draw's
    // shaders and textures within the same entity. There each geometry is drawn with its own shader, so only those of one
    // material may go together: by shader instance (the same pointer), by material (the effect, the bucket and every
    // parameter alike: a texture by pointer, any other by its values) and by textures alone (the parameters' values left
    // out: what the capture saw, an upper bound). A shader's parameters (grmShaderFx +0x14, CE 0x437170): +0x14 their data
    // pointers, +0x1c their count, +0x24 a byte each, 0 a texture, else the float4 registers it fills
    struct GBufferModel { uintptr_t geometries = 0; uint16_t count = 0, inBucket = 0, byInstance = 0, byMaterial = 0, byTextures = 0, unmergeable = 0; };
    std::unordered_map<uint64_t, GBufferModel> gbufferModels;     // by model and bucket
    struct { uint64_t models = 0, draws = 0, byInstance = 0, byMaterial = 0, byTextures = 0, unmergeable = 0; } gbuffer;

    uint64_t Fnv(uint64_t hash, const void* p, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            hash = (hash ^ static_cast<const uint8_t*>(p)[i]) * 0x100000001b3ull;
        return hash;
    }

    // A shader's key by material (its parameters' values too) or by its effect, bucket and textures alone; 0 when unreadable.
    // Under SEH: the pointers are the game's
    uint64_t MaterialKey(uintptr_t shader, bool constants)
    {
        __try
        {
            uint64_t hash = 0xcbf29ce484222325ull;
            const char* name = *reinterpret_cast<const char* const*>(shader + 0x44);
            for (int i = 0; name && i < 64 && name[i]; ++i)
                hash = Fnv(hash, &name[i], 1);
            hash = Fnv(hash, reinterpret_cast<const void*>(shader + 9), 1);
            const auto data = *reinterpret_cast<const uintptr_t* const*>(shader + 0x14);
            const uint32_t count = (std::min)(*reinterpret_cast<const uint32_t*>(shader + 0x1c) & 0xffff, 256u);
            const auto types = *reinterpret_cast<const uint8_t* const*>(shader + 0x24);
            for (uint32_t i = 0; data && types && i < count; ++i)
            {
                hash = Fnv(hash, &types[i], 1);
                if (types[i] == 0)
                    hash = Fnv(hash, &data[i], sizeof(uintptr_t));
                else if (constants && data[i])
                    hash = Fnv(hash, reinterpret_cast<const void*>(data[i]), size_t(types[i]) * 16);
            }
            return hash | 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    void GBufferCount(uintptr_t m, uintptr_t group, int32_t bucket)
    {
        const uint16_t count = Read<uint16_t>(m, 0x1a);
        const uintptr_t geometries = Read<uintptr_t>(m, 4), shaderMap = Read<uintptr_t>(m, 0x10);
        if (!count || !geometries || !shaderMap || !group)
            return;
        auto& model = gbufferModels[uint64_t(m) | (uint64_t(uint32_t(bucket)) << 32)];
        if (model.geometries != geometries || model.count != count)
        {
            model = { geometries, count };
            const auto shaders = Read<const uintptr_t*>(group, 8);
            const uint16_t shaderCount = Read<uint16_t>(group, 0xc);
            struct Key { uint64_t key; uintptr_t declaration; uint32_t stride; };
            std::vector<Key> instance, material, textures;
            auto add = [](std::vector<Key>& keys, const Key& k)
            {
                if (std::none_of(keys.begin(), keys.end(), [&](const Key& x) { return x.key == k.key && x.declaration == k.declaration && x.stride == k.stride; }))
                    keys.push_back(k);
            };
            for (int i = 0; i < count; ++i)
            {
                const uint16_t index = Read<uint16_t>(shaderMap, i * 2);
                const uintptr_t shader = shaders && index < shaderCount ? shaders[index] : 0;
                if (!shader || int32_t(Read<uint8_t>(shader, 9)) != bucket)
                    continue;
                ++model.inBucket;
                const uintptr_t g = Read<uintptr_t>(geometries, i * 4);
                const uint64_t byMaterial = MaterialKey(shader, true), byTextures = MaterialKey(shader, false);
                if (!Mergeable(g) || !byMaterial || !byTextures)
                {
                    ++model.unmergeable;
                    continue;
                }
                const uintptr_t declaration = Read<uintptr_t>(g, 0x40);
                const uint32_t stride = Read<uint32_t>(Read<uintptr_t>(g, 0xc), 0xc);
                add(instance, { shader, declaration, stride });
                add(material, { byMaterial, declaration, stride });
                add(textures, { byTextures, declaration, stride });
            }
            const int mergeable = model.inBucket - model.unmergeable;
            model.byInstance = uint16_t(mergeable - instance.size());
            model.byMaterial = uint16_t(mergeable - material.size());
            model.byTextures = uint16_t(mergeable - textures.size());
        }
        ++gbuffer.models;
        gbuffer.draws += model.inBucket;
        gbuffer.byInstance += model.byInstance;
        gbuffer.byMaterial += model.byMaterial;
        gbuffer.byTextures += model.byTextures;
        gbuffer.unmergeable += model.unmergeable;
    }

    // Whether a draw of this entity in this pass may be merged; counts why not
    bool Eligible()
    {
        ++stats.calls;
        if (!inEntity || (currentKind != EntityClass::Kind::Building && currentKind != EntityClass::Kind::Prop))
        {
            ++(inEntity ? stats.otherKind : stats.outside);
            return false;
        }
        if (bLog && targetsSeen.size() < 64)
        {
            const char* target = RenderSkip::CurrentTarget();
            ++targetsSeen[target ? target : "(no lock hooks)"];
        }
        if (!ShadowPass())
        {
            ++stats.otherPass;
            return false;
        }
        uint64_t* reason = (pSkinning && *pSkinning) ? &stats.skinned : (pInstancing && pInstancing()) ? &stats.instancing : nullptr;
        if (reason)
        {
            ++*reason;
            return false;
        }
        return true;
    }

    void __fastcall ModelDraw(void* model, void* edx, void* group, int32_t bucket, int32_t flag)
    {
        const uintptr_t m = reinterpret_cast<uintptr_t>(model);
        if (bLog && m && inEntity && (currentKind == EntityClass::Kind::Building || currentKind == EntityClass::Kind::Prop) &&
            !(pSkinning && *pSkinning))
            if (const char* target = RenderSkip::CurrentTarget(); target && std::strcmp(target, "_DEFERRED_GBUFFER_0_") == 0)
                GBufferCount(m, reinterpret_cast<uintptr_t>(group), bucket);
        const Entry* entry = bEnabled && m && Eligible() ? Lookup(m, reinterpret_cast<uintptr_t>(group), false) : nullptr;
        shModelDraw.unsafe_fastcall(entry ? static_cast<const void*>(entry->model) : model, edx, group, bucket, flag);
    }

    // rmcDrawable's draw under a forced shader (CE 0x6a1d50, vtable +0x2c; thiscall: the matrix, the detail, the bucket or
    // -1, a mask of its models or 0; ret 0x10): the pass has bound the shader, and each geometry of the detail's models
    // (the list at drawable +0x40 + detail * 4: the models' array and a u16 count) whose shader (+8 the group) is of the
    // bucket is drawn as it is (its vtable +8), nothing of its own bound. The cascades and the lamps' maps draw most
    // entities so (CDrawEntityDC's execute, CE 0x8dcbf0, when 0x444940 gives a forced shader or the command's +0x36 & 3);
    // the first builds hooked grmModel::Draw alone and saw no model of the lamps' maps at all. Here any geometries of
    // one bucket, declaration and stride draw alike, so they are merged whatever their shaders. The game's function is
    // given a copy of the drawable whose list holds the merged models (in their places, the mask's bits still theirs).
    void __fastcall DrawNoShaders(void* drawable, void* edx, void* matrix, int32_t detail, int32_t bucket, void* mask)
    {
        const uintptr_t d = reinterpret_cast<uintptr_t>(drawable);
        auto original = [&] { shDrawNoShaders.unsafe_fastcall(drawable, edx, matrix, detail, bucket, mask); };
        if (!bEnabled || !d || detail < 0 || detail > 3 || !Eligible())
            return original();
        const uintptr_t list = Read<uintptr_t>(d, 0x40 + detail * 4);
        const uintptr_t models = list ? Read<uintptr_t>(list, 0) : 0;
        const uint16_t count = list ? Read<uint16_t>(list, 4) : 0;
        static uintptr_t merged[32];
        if (!models || !count || count > 32)
            return original();
        bool any = false;
        for (int i = 0; i < count; ++i)
        {
            const uintptr_t m = Read<uintptr_t>(models, i * 4);
            const Entry* entry = m ? Lookup(m, Read<uintptr_t>(d, 8), true) : nullptr;
            merged[i] = entry ? reinterpret_cast<uintptr_t>(entry->model) : m;
            any = any || entry;
        }
        if (!any)
            return original();
        struct { uintptr_t models; uint16_t count, size; } copyList = { reinterpret_cast<uintptr_t>(merged), count, count };
        alignas(16) uint8_t copy[kDrawableCopy];
        std::memcpy(copy, drawable, kDrawableCopy);
        *reinterpret_cast<uintptr_t*>(copy + 0x40 + detail * 4) = reinterpret_cast<uintptr_t>(&copyList);
        shDrawNoShaders.unsafe_fastcall(copy, edx, matrix, detail, bucket, mask);
    }

    void EndFrame()
    {
        ++frame;
        buildsThisFrame = 0;
        buildTicksThisFrame = 0;
        if (frame % 60 == 0)
            Trim();
        if (bLog && frame % kLogFrames == 0)
        {
            const double n = double(kLogFrames);
            FusionLog::Write("ShadowMerge", "Stats", "per frame over %u: %.1f merged models drawn, %.0f draws saved; drawn the game's way: "
                "skinned %.1f, instanced sample %.1f, one geometry %.1f, nothing to merge %.1f, waiting to be built %.1f; built %llu, "
                "nothing to merge %llu, failed %llu, let go %llu; slowest build %.2f ms (%u geometries, %.2f MB); cache %zu models, %.1f MB\n",
                kLogFrames, stats.drawn / n, stats.saved / n, stats.skinned / n, stats.instancing / n, stats.single / n, stats.nothing / n,
                stats.budget / n, stats.built, stats.noGain, stats.failed, stats.letGo, stats.slowestTicks * 1000.0 / double(frequency.QuadPart),
                unsigned(stats.slowestCount), stats.slowestBytes / 1048576.0, entries.size(), totalBytes / 1048576.0);
            std::string targets;
            for (auto& [name, count] : targetsSeen)
                targets += std::format("{} {:.1f}, ", name, count / n);
            FusionLog::Write("ShadowMerge", "Calls", "per frame: %.1f model and drawable draws, %.1f outside an entity's draw, %.1f of vehicles and people, "
                "%.1f outside a shadow pass; buildings' and props' by target: %s\n", stats.calls / n, stats.outside / n, stats.otherKind / n,
                stats.otherPass / n, targets.c_str());
            FusionLog::Write("ShadowMerge", "GBuffer", "per frame: %.1f buildings' and props' model draws, %.1f geometries drawn, %.1f not mergeable; "
                "draws merging would save: by shader instance %.1f, by material %.1f, by textures alone %.1f; %zu models seen\n", gbuffer.models / n,
                gbuffer.draws / n, gbuffer.unmergeable / n, gbuffer.byInstance / n, gbuffer.byMaterial / n, gbuffer.byTextures / n, gbufferModels.size());
            gbuffer = {};
            if (gbufferModels.size() > 20000)
                gbufferModels.clear();
            targetsSeen.clear();
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
            QueryPerformanceFrequency(&frequency);
            buildBudgetTicks = int64_t(kBuildMsPerFrame * 0.001 * double(frequency.QuadPart));
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
            // rmcDrawable's draw under a forced shader (CE 0x6a1d50): sub esp, 8 / push ebx / push ebp / push esi /
            // push [esp+18h] / mov ebx, ecx / mov ecx, [viewport] / mov [esp+14h], ebx / call / mov eax, [esp+1Ch] / mov edx, 1 /
            // mov esi, [ebx+eax*4+40h]
            if (auto noShaders = hook::pattern("83 EC 08 53 55 56 FF 74 24 18 8B D9 8B 0D ? ? ? ? 89 5C 24 14 E8 ? ? ? ? 8B 44 24 1C BA 01 00 00 00 8B 74 83 40");
                !noShaders.empty())
                shDrawNoShaders = safetyhook::create_inline(noShaders.get_first(0), reinterpret_cast<void*>(&DrawNoShaders));
            FusionLog::Write("ShadowMerge", "Init", "grmModel::Draw hooked, rmcDrawable's draw under a forced shader %s\n",
                shDrawNoShaders ? "hooked" : "not found");
            EntityClass::AddDrawHooks([](EntityClass::Kind kind) { currentKind = kind; inEntity = true; },
                                      [](EntityClass::Kind) { inEntity = false; });
            FusionFix::onBeforePresent() += []() { EndFrame(); };
        };
        // (managed buffers outlive a reset; nothing to let go)
    }
} ShadowMergeFeature;
