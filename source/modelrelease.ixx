module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <intrin.h>
#include <unordered_map>

export module modelrelease;

import common;
import comvars;

// ModelReleaseLog = 1: every second in which entities lost their models (driving as well as in the menu), how many and from where, to find what empties
// the world around the camera (a dump taken right after the pause menu had most map entities within 100 m without a
// model, with the streaming memory nearly empty and its drawable references free). An entity's model goes in its
// vtable's +0x44, CE 0xa30af0 for buildings and plain entities (CEntity::DeleteDrawable: the drawable reference back,
// +0x34 cleared, the model's count down); CE 0xaf4700, taken first, turned out to be every draw's touch of the list at
// 0x1173750. The log has the caller and the caller's caller as CE addresses, how many of the entities were the map's
// (not props, bit 0x01000000) within 100 m of the camera, the menu's state and the streaming memory in use
namespace ModelRelease
{
    bool bLog = false;
    uintptr_t delta = 0;
    std::mutex mutex;
    std::map<std::pair<uintptr_t, uintptr_t>, uint32_t> callers;
    uint32_t released = 0, nearMap = 0, inMenu = 0;
    float* pCamera = nullptr;   // CE 0x128e340
    SafetyHookMid releaseHook, reclaimEntryHook, reclaimLoopHook;

    // The streaming's reclaim (CE 0xa8ad20, from 0x8d858d): it lets entities go of their models in stages (CE 0xa8a520,
    // the near and visible ones too in the later stages) while the resources' virtual heap has less room than asked for
    // (0x103e8f4 used against 0x103e8f0 at most) or the free physical memory it is told of (CE 0x4276b0 with 0.9) is
    // under what was asked for. Its calls, turns of its loop, the most asked for and the least free it saw
    uint32_t reclaims = 0, reclaimTurns = 0;
    int64_t mostVirtualAsked = 0, mostPhysicalAsked = 0, leastPhysicalFree = INT64_MAX;

    uintptr_t InGame(uintptr_t address)
    {
        uintptr_t ce = address - delta;
        return ce >= 0x401000 && ce < 0x1000000 ? ce : 0;
    }

    // The caller's own return address, where it keeps a frame; on its own, as __try takes no objects to unwind
    uintptr_t OuterReturn(uintptr_t ebp)
    {
        __try
        {
            return ebp ? *(uintptr_t*)(ebp + 4) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // Within 100 m of the camera, from the entity's matrix (+0x20, its row at +0x30) or its position (+0x10)
    bool Near(uintptr_t entity)
    {
        if (!pCamera)
            return false;
        auto matrix = *(const float**)(entity + 0x20);
        const float* position = matrix ? matrix + 12 : (const float*)(entity + 0x10);
        float dx = position[0] - pCamera[0], dy = position[1] - pCamera[1];
        return dx * dx + dy * dy < 100.0f * 100.0f;
    }

    void Released(SafetyHookContext& regs)
    {
        uintptr_t entity = regs.ecx;
        if (!entity || !*(uint32_t*)(entity + 0x34))
            return;     // no model to let go of
        uintptr_t from = InGame(*(uintptr_t*)regs.esp);
        uintptr_t outer = InGame(OuterReturn(regs.ebp));
        bool prop = (*(uint32_t*)(entity + 0x24) & 0x01000000) != 0;
        bool nearby = !prop && Near(entity);
        std::lock_guard lock(mutex);
        callers[{ from, outer }]++;
        released++;
        nearMap += nearby;
        inMenu += CMenuManager::m_MenuActive && *CMenuManager::m_MenuActive;
    }

    void ReclaimTurn(SafetyHookContext& regs)
    {
        int64_t virtualAsked = int32_t(regs.ebp), physicalAsked = *(int32_t*)(regs.esp + 0x18), physicalFree = int32_t(regs.esi);
        std::lock_guard lock(mutex);
        reclaimTurns++;
        mostVirtualAsked = std::max(mostVirtualAsked, virtualAsked);
        mostPhysicalAsked = std::max(mostPhysicalAsked, physicalAsked);
        leastPhysicalFree = std::min(leastPhysicalFree, physicalFree);
    }

    void Write()
    {
        std::vector<std::pair<uint32_t, std::pair<uintptr_t, uintptr_t>>> top;
        uint32_t count, nearby, menu, calls, turns;
        int64_t virtualAsked, physicalAsked, physicalFree;
        {
            std::lock_guard lock(mutex);
            calls = reclaims;
            turns = reclaimTurns;
            virtualAsked = mostVirtualAsked;
            physicalAsked = mostPhysicalAsked;
            physicalFree = leastPhysicalFree;
            reclaims = reclaimTurns = 0;
            mostVirtualAsked = mostPhysicalAsked = 0;
            leastPhysicalFree = INT64_MAX;
            count = released;
            nearby = nearMap;
            menu = inMenu;
            released = nearMap = inMenu = 0;
            for (auto& [where, n] : callers)
                top.push_back({ n, where });
            callers.clear();
        }
        if (!count && !calls)
            return;
        std::sort(top.rbegin(), top.rend());
        // While the menu is open the game's frame doesn't run, and what went then comes in one line after it
        std::string text = std::format("{} entities let go of their models, {} of them the map's within 100 m, {} with the menu open", count, nearby, menu);
        for (size_t i = 0; i < top.size() && i < 6; i++)
            text += std::format("; {} from {:#x} (in {:#x})", top[i].first, top[i].second.first, top[i].second.second);
        if (auto info = CStreamingEngine::ms_info)
            text += std::format("; streaming {} MB in use, {} requests; virtual heap {} of {} MB", info->m_PhysicalUsed >> 20, info->m_NumRequests,
                info->m_VirtualUsed >> 20, info->m_ResourceVirtualMax >> 20);
        if (calls)
            text += std::format("; reclaim called {} times, {} turns, asked at most {} KB virtual and {} KB physical, saw {} KB physical free at least",
                calls, turns, virtualAsked >> 10, physicalAsked >> 10, physicalFree == INT64_MAX ? -1 : physicalFree >> 10);
        FusionLog::WriteText("ModelRelease", "Stats", text);
    }
}


// TextureAccountTrace = 1: the game's own count of the video memory its resources take (the resource cache's +0x40268,
// what CE 0x4276b0 takes off its budget for the free memory the streaming reclaims for) grew to 114 GB in a dump while
// some 5500 resources were on its books: textures freed past its books leave their size counted for good, and then
// every reclaim (the pause menu, a change of settings, streaming while driving) lets the whole world go. A resource is
// put on its books in CE 0x4292d0 (created: its record +4 type, +0x34 size, +0x40 the D3D object; the size added at CE
// 0x42946f) or taken back on in CE 0x429042 (record in esi), and off them in CE 0x429900 (the D3D object the argument;
// skipped when not on its books). This keeps its own list of the textures on the books, and finds each texture whose
// D3D object goes (its Release to 0) while still on them, with who made it and who let it go; and releases the game
// asked for of objects it doesn't know of. Every 10 s to GTAIV.EFLC.FusionFix.TextureAccount.log
namespace TextureAccount
{
    bool bTrace = false;
    uintptr_t delta = 0;
    std::mutex mutex;
    struct Entry { uint32_t type, size, width, height, format; uintptr_t creator; bool taken; };
    std::unordered_map<uintptr_t, Entry> live;
    uint8_t* cache = nullptr;
    thread_local uintptr_t creatorReturn = 0;
    uint64_t liveBytes = 0;
    uint32_t created = 0, retaken = 0, released = 0, unknownReleases = 0, duplicates = 0, bypassed = 0;
    uint64_t bypassedBytes = 0;
    std::map<std::pair<std::string, std::string>, std::pair<uint32_t, uint64_t>> bypassBy;  // creator, releaser: count, bytes
    std::map<std::string, uint32_t> unknownBy;
    std::vector<std::string> details;
    SafetyHookMid entryHook, addHook, retakeHook, releaseHook;
    ULONG(__stdcall* RealRelease)(IUnknown*) = nullptr;

    std::string Where(uintptr_t address)
    {
        if (!address)
            return "?";
        uintptr_t ce = address - delta;
        if (ce >= 0x401000 && ce < 0x1400000)
            return std::format("GTAIV {:#x}", ce);
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module) && module)
        {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            std::wstring name = path;
            name = name.substr(name.find_last_of(L"\\/") + 1);
            std::string narrow;
            for (wchar_t c : name)
                narrow.push_back(c < 128 ? char(c) : '?');
            return std::format("{}+{:#x}", narrow, address - uintptr_t(module));
        }
        return std::format("{:#x}", address);
    }

    void OnBooks(uint8_t* record, uint8_t* books, uintptr_t creator, bool retake)
    {
        uint32_t type = *(uint32_t*)(record + 4), size = *(uint32_t*)(record + 0x34);
        uintptr_t object = *(uintptr_t*)(record + 0x40);
        if (!object)
            return;
        Entry entry{ type, size, 0, 0, 0, creator, retake };
        if (type == 0)
        {
            D3DSURFACE_DESC desc{};
            if (SUCCEEDED(((IDirect3DTexture9*)object)->GetLevelDesc(0, &desc)))
            {
                entry.width = desc.Width;
                entry.height = desc.Height;
                entry.format = desc.Format;
            }
        }
        std::lock_guard lock(mutex);
        cache = books;
        (retake ? retaken : created)++;
        auto [it, inserted] = live.try_emplace(object, entry);
        if (!inserted)
        {
            duplicates++;
            liveBytes -= it->second.size;
            it->second = entry;
        }
        liveBytes += size;
    }

    void OffBooks(uintptr_t object, uintptr_t caller)
    {
        std::lock_guard lock(mutex);
        auto it = live.find(object);
        if (it == live.end())
        {
            unknownReleases++;
            unknownBy[Where(caller)]++;
            return;
        }
        released++;
        liveBytes -= it->second.size;
        live.erase(it);
    }

    ULONG __stdcall Release(IUnknown* object)
    {
        uintptr_t caller = uintptr_t(_ReturnAddress());
        ULONG left = RealRelease(object);
        if (left)
            return left;
        std::lock_guard lock(mutex);
        auto it = live.find(uintptr_t(object));
        if (it == live.end())
            return left;
        // Gone while still on the game's books: its size stays counted
        auto& e = it->second;
        auto key = std::make_pair(e.taken ? "taken back on" : Where(e.creator), Where(caller));
        auto& by = bypassBy[key];
        by.first++;
        by.second += e.size;
        bypassed++;
        bypassedBytes += e.size;
        if (details.size() < 40)
            details.push_back(std::format("  {}x{} format {} ({} KB), put on the books from {}, let go by {}", e.width, e.height, e.format, e.size >> 10,
                key.first, key.second));
        liveBytes -= e.size;
        live.erase(it);
        return left;
    }

    void Write()
    {
        std::lock_guard lock(mutex);
        uint64_t counted = cache ? *(uint64_t*)(cache + 0x40268) : 0;
        std::string text = std::format("the game counts {} MB, its books as kept here {} MB in {} resources; since the last: {} put on, {} taken back on, {} taken off, "
            "{} of objects it never had, {} put on twice; textures gone while on its books: {} ({} MB in all)",
            counted >> 20, liveBytes >> 20, live.size(), created, retaken, released, unknownReleases, duplicates, bypassed, bypassedBytes >> 20);
        created = retaken = released = unknownReleases = duplicates = 0;
        for (auto& [key, by] : bypassBy)
            text += std::format("\n  gone while on the books: {} from {}, let go by {}: {} ({} MB)", by.first, key.first, key.second, by.first, by.second >> 20);
        for (auto& [where, n] : unknownBy)
            text += std::format("\n  release asked for an object not on the books, by {}: {}", where, n);
        for (auto& line : details)
            text += "\n" + line;
        details.clear();
        bypassBy.clear();
        unknownBy.clear();
        FusionLog::WriteText("TextureAccount", "Stats", text);
    }

    void Install()
    {
        delta = GameBase() - 0x400000;
        auto entry = hook::pattern("83 EC 18 53 55 8B 6C 24 24 56 8D 75 04 8B D9 8B 0E B8 6C 08 76 88");
        auto add = hook::pattern("8B 45 34 01 83 68 02 04 00 83 93 6C 02 04 00 00 8B 0E 8B 45 38");
        auto retake = hook::pattern("8B 46 34 01 83 68 02 04 00 83 93 6C 02 04 00 00");
        auto release = hook::pattern("83 EC 10 53 55 8B D9 56 8B 43 0C 57 8D 7B 08 8B CF 85 C0 74");
        if (entry.empty() || add.empty() || retake.empty() || release.empty())
        {
            FusionLog::Write("TextureAccount", "Settings", "the resource cache's books not found, nothing traced\n");
            bTrace = false;
            return;
        }
        entryHook = safetyhook::create_mid(entry.get_first(0), [](SafetyHookContext& regs) { creatorReturn = *(uintptr_t*)regs.esp; });
        addHook = safetyhook::create_mid(add.get_first(0), [](SafetyHookContext& regs) { OnBooks((uint8_t*)regs.ebp, (uint8_t*)regs.ebx, creatorReturn, false); });
        retakeHook = safetyhook::create_mid(retake.get_first(0), [](SafetyHookContext& regs) { OnBooks((uint8_t*)regs.esi, (uint8_t*)regs.ebx, 0, true); });
        releaseHook = safetyhook::create_mid(release.get_first(0), [](SafetyHookContext& regs) { OffBooks(*(uintptr_t*)(regs.esp + 4), *(uintptr_t*)regs.esp); });
        FusionLog::Write("TextureAccount", "Settings", "tracing the resource cache's books\n");
    }

    // The textures' Release, from one of the device's own (all its 2D textures share the class)
    void HookRelease()
    {
        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        IDirect3DTexture9* probe = nullptr;
        if (!device || FAILED(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, nullptr)) || !probe)
        {
            FusionLog::Write("TextureAccount", "Settings", "no texture to take Release from, textures freed past the books not traced\n");
            return;
        }
        auto vtable = *(void***)probe;
        probe->Release();
        if (vtable[2] == (void*)&Release)
            return;
        RealRelease = (decltype(RealRelease))vtable[2];
        injector::WriteMemory(&vtable[2], (void*)&Release, true);
    }
}

class ModelReleaseFeature
{
public:
    ModelReleaseFeature()
    {
        using namespace ModelRelease;

        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");
            bLog = iniReader.ReadInteger("MISC", "ModelReleaseLog", 0) != 0;
            if (!bLog)
                return;
            delta = GameBase() - 0x400000;
            if (auto camera = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ? 56 8B 75 0C"); !camera.empty())
                pCamera = *camera.get_first<float*>(12);
            auto pattern = hook::pattern("56 8B F1 8B 46 34 85 C0 74 ? 50 B9 ? ? ? ? E8 ? ? ? ? 0F BF 46 2E C7 46 34 00 00 00 00");
            if (pattern.empty())
            {
                FusionLog::Write("ModelRelease", "Settings", "the release of a model not found, nothing logged\n");
                return;
            }
            releaseHook = safetyhook::create_mid(pattern.get_first(0), Released);
            auto entry = hook::pattern("53 55 56 57 8B F9 E8 ? ? ? ? 51 C7 47 04 00 00 00 00 C7 04 24 66 66 66 3F");
            auto loop = hook::pattern("A1 ? ? ? ? 2B C5 39 05 ? ? ? ? 77 06 3B 74 24 18 7D");
            if (!entry.empty() && !loop.empty())
            {
                reclaimEntryHook = safetyhook::create_mid(entry.get_first(0), [](SafetyHookContext&)
                {
                    std::lock_guard lock(mutex);
                    reclaims++;
                });
                reclaimLoopHook = safetyhook::create_mid(loop.get_first(0), ReclaimTurn);
            }
            FusionLog::Write("ModelRelease", "Settings", "logging every second the entities that let go of their models\n");
        };

        // TextureAccountTrace: in the init, as other hooks in the game's encrypted code are (memory.ixx)
        FusionFix::onInitEvent() += []()
        {
            CIniReader iniReader("");
            TextureAccount::bTrace = iniReader.ReadInteger("MISC", "TextureAccountTrace", 0) != 0;
            if (TextureAccount::bTrace)
                TextureAccount::Install();
        };

        FusionFix::onGameInitEvent() += []()
        {
            if (TextureAccount::bTrace)
                TextureAccount::HookRelease();
        };

        FusionFix::onGameProcessEvent() += []()
        {
            static uint64_t lastBooks = 0;
            if (TextureAccount::bTrace && GetTickCount64() - lastBooks >= 10000)
            {
                lastBooks = GetTickCount64();
                TextureAccount::Write();
            }
        };

        FusionFix::onGameProcessEvent() += []()
        {
            static uint64_t last = 0;
            if (!bLog)
                return;
            uint64_t now = GetTickCount64();
            if (now - last < 1000)
                return;
            last = now;
            Write();
        };
    }
} ModelReleaseFeature;
