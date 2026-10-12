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
    uint32_t released = 0, nearMap = 0;
    float* pCamera = nullptr;   // CE 0x128e340
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
    }

    void Write()
    {
        std::vector<std::pair<uint32_t, std::pair<uintptr_t, uintptr_t>>> top;
        uint32_t count, nearby;
        {
            std::lock_guard lock(mutex);
            count = released;
            nearby = nearMap;
            released = nearMap = 0;
            for (auto& [where, n] : callers)
                top.push_back({ n, where });
            callers.clear();
        }
        if (!count)
            return;
        std::sort(top.rbegin(), top.rend());
        std::string text = std::format("{} entities let go of their models, {} of them the map's within 100 m{}", count, nearby,
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
            if (auto camera = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ? 56 8B 75 0C"); !camera.empty())
                pCamera = *camera.get_first<float*>(12);
            auto pattern = hook::pattern("56 8B F1 8B 46 34 85 C0 74 ? 50 B9 ? ? ? ? E8 ? ? ? ? 0F BF 46 2E C7 46 34 00 00 00 00");
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
