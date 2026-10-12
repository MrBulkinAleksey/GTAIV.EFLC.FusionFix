module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

export module modelrelease;

import common;
import comvars;

// ModelReleaseLog = 1: every second in which entities lost their models (driving as well as in the menu), how many and from where, to find what empties
// the world around the camera (a dump taken right after the pause menu had most map entities within 100 m without a
// model, with the streaming memory nearly empty and its drawable references free). An entity's model goes through
// CE 0xaf4700 (its drawable reference off the list at 0x1173750), called from six places; the log has the caller and
// the caller's caller as CE addresses, the menu's state and the streaming memory in use
namespace ModelRelease
{
    bool bLog = false;
    uintptr_t delta = 0;
    std::mutex mutex;
    std::map<std::pair<uintptr_t, uintptr_t>, uint32_t> callers;
    uint32_t released = 0;
    SafetyHookMid releaseHook;

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

    void Released(SafetyHookContext& regs)
    {
        uintptr_t from = InGame(*(uintptr_t*)regs.esp);
        uintptr_t outer = InGame(OuterReturn(regs.ebp));
        std::lock_guard lock(mutex);
        callers[{ from, outer }]++;
        released++;
    }

    void Write()
    {
        std::vector<std::pair<uint32_t, std::pair<uintptr_t, uintptr_t>>> top;
        uint32_t count;
        {
            std::lock_guard lock(mutex);
            count = released;
            released = 0;
            for (auto& [where, n] : callers)
                top.push_back({ n, where });
            callers.clear();
        }
        if (!count)
            return;
        std::sort(top.rbegin(), top.rend());
        std::string text = std::format("{} entities let go of their models{}", count,
            CMenuManager::m_MenuActive && *CMenuManager::m_MenuActive ? " (menu open)" : "");
        for (size_t i = 0; i < top.size() && i < 6; i++)
            text += std::format("; {} from {:#x} (in {:#x})", top[i].first, top[i].second.first, top[i].second.second);
        if (auto info = CStreamingEngine::ms_info)
            text += std::format("; streaming {} MB in use, {} requests", info->m_PhysicalUsed >> 20, info->m_NumRequests);
        FusionLog::WriteText("ModelRelease", "Stats", text);
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
            auto pattern = hook::pattern("FF 71 08 B9 ? ? ? ? E8 ? ? ? ? C2 04 00");
            if (pattern.empty())
            {
                FusionLog::Write("ModelRelease", "Settings", "the release of a model not found, nothing logged\n");
                return;
            }
            releaseHook = safetyhook::create_mid(pattern.get_first(0), Released);
            FusionLog::Write("ModelRelease", "Settings", "logging every second the entities that let go of their models\n");
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
