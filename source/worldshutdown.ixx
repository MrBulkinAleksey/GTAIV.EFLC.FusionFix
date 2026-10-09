module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <TlHelp32.h>
#include <algorithm>
#include <array>
#include <format>
#include <intrin.h>
#include <string>
#include <unordered_map>
#include <vector>

export module worldshutdown;

import common;

// The world's shutdown on exit (CE 0x9415e0, from 0xb1ea00) walks the entity lists of every sector cell (the
// 120x120 grid at CE 0x11a9d20 and the others before it), takes each entity out of the world (CE 0x940780) and
// deletes it through its vtable. Every exit stopped there with the game's own fatal error SMPA50: a list still
// held a building (type 1, entity +0x28 & 0x3c0 == 0x40) destroyed long before, its vtable already back to the
// base CEntity's, whose deleting destructor gives the pool's memory to the game's heap. Who destroyed it without
// taking it out of its cell is not known yet: each building's destruction keeps the code addresses on its stack
// here, and when the shutdown meets one destroyed before, it is left alone and the log
// (GTAIV.EFLC.FusionFix.World.log) tells where it is and who destroyed it.
namespace WorldShutdown
{
    constexpr size_t MaxFrames = 20;
    constexpr size_t StackDwords = 160;

    struct Destruction
    {
        DWORD thread = 0;
        uint32_t count = 0;
        std::array<uintptr_t, MaxFrames> frames{};
    };

    struct CodeRange { uintptr_t begin, end; };
    std::vector<CodeRange> codeRanges;   // executable sections of the modules loaded at init
    std::unordered_map<uintptr_t, Destruction> destroyed;
    SRWLOCK lock = SRWLOCK_INIT;

    uintptr_t pBaseVtable = 0;
    uintptr_t pShutdown = 0;             // CE 0x9415e0, about 0x150 bytes
    constexpr uintptr_t ShutdownSize = 0x200;
    uint32_t nSkipped = 0;

    void CollectCodeRanges()
    {
        auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        MODULEENTRY32W module{ sizeof(module) };
        for (auto ok = Module32FirstW(snapshot, &module); ok; ok = Module32NextW(snapshot, &module))
        {
            auto base = uintptr_t(module.modBaseAddr);
            auto dos = (IMAGE_DOS_HEADER*)base;
            auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            auto section = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
                if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                    codeRanges.push_back({ base + section->VirtualAddress, base + section->VirtualAddress + section->Misc.VirtualSize });
        }
        CloseHandle(snapshot);
        std::sort(codeRanges.begin(), codeRanges.end(), [](auto& a, auto& b) { return a.begin < b.begin; });
    }

    bool IsCode(uintptr_t address)
    {
        auto it = std::upper_bound(codeRanges.begin(), codeRanges.end(), address, [](uintptr_t value, auto& range) { return value < range.begin; });
        return it != codeRanges.begin() && address < (it - 1)->end;
    }

    std::string Describe(uintptr_t address)
    {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module) && module)
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(module, path, MAX_PATH);
            std::wstring name = path;
            name = name.substr(name.find_last_of(L"\\/") + 1);
            auto offset = address - uintptr_t(module);
            auto text = std::format("{:08X} {}+{:X}", address, std::string(name.begin(), name.end()), offset);
            if (module == GetModuleHandleW(nullptr))
                text += std::format(" (CE {:X})", offset + 0x400000);
            return text;
        }
        return std::format("{:08X}", address);
    }

    // Start of the CEntity destructor (CE 0xa2fd80), run by every entity's
    void OnDestroy(uintptr_t entity, uintptr_t esp)
    {
        if ((*(uint32_t*)(entity + 0x28) & 0x3C0) != 0x40)
            return;
        Destruction record;
        record.thread = GetCurrentThreadId();
        auto stackBase = uintptr_t(__readfsdword(4)); // TEB StackBase, the top of this thread's stack
        auto dwords = std::min<size_t>(StackDwords, (stackBase - esp) / 4);
        size_t n = 0;
        for (size_t i = 0; i < dwords && n < MaxFrames; i++)
        {
            auto value = *(uintptr_t*)(esp + i * 4);
            if (IsCode(value))
                record.frames[n++] = value;
        }
        AcquireSRWLockExclusive(&lock);
        auto& slot = destroyed[entity];
        record.count = slot.count + 1;
        slot = record;
        ReleaseSRWLockExclusive(&lock);
    }

    SafetyHookInline shDeletingDestructor;
    void* __fastcall DeletingDestructor(uintptr_t entity, void* edx, int flags)
    {
        auto caller = uintptr_t(_ReturnAddress());
        if (caller >= pShutdown && caller < pShutdown + ShutdownSize && *(uintptr_t*)entity == pBaseVtable)
        {
            nSkipped++;
            auto model = *(int16_t*)(entity + 0x2E);
            auto position = (float*)(entity + 0x10);
            std::string text = std::format("the shutdown met entity {:08X}, destroyed before and still in a sector list; left alone. "
                "model {}, at {:.1f} {:.1f} {:.1f}, LOD parent {:08X}, +0x04 {:08X}, +0x24 {:08X}, +0x28 {:08X}\n",
                entity, model, position[0], position[1], position[2], *(uint32_t*)(entity + 0x4C),
                *(uint32_t*)(entity + 0x04), *(uint32_t*)(entity + 0x24), *(uint32_t*)(entity + 0x28));
            AcquireSRWLockShared(&lock);
            auto it = destroyed.find(entity);
            if (it != destroyed.end())
            {
                text += std::format("  destroyed {} time(s); the last on thread {}, code on its stack:\n", it->second.count, it->second.thread);
                for (auto frame : it->second.frames)
                    if (frame)
                        text += "    " + Describe(frame) + "\n";
            }
            else
                text += "  its destruction was not seen\n";
            ReleaseSRWLockShared(&lock);
            FusionLog::WriteText("World", "Shutdown", text);
            return (void*)entity;
        }
        return shDeletingDestructor.unsafe_fastcall<void*>(entity, edx, flags);
    }
}

class WorldShutdownGuard
{
public:
    WorldShutdownGuard()
    {
        FusionFix::onInitEvent() += []()
        {
            using namespace WorldShutdown;
            auto destructor = hook::pattern("56 8B F1 F7 46 24 00 00 00 08 C7 06 ? ? ? ? 74 05 E8");
            auto shutdown = hook::pattern("53 55 56 57 BB ? ? ? ? 8D A4 24 00 00 00 00 8B 33 85 F6 74 21");
            if (destructor.empty() || shutdown.empty())
            {
                FusionLog::WriteText("World", "Shutdown", "patterns not found, the guard is off");
                return;
            }

            CollectCodeRanges();
            pShutdown = uintptr_t(shutdown.get_first(0));
            pBaseVtable = *destructor.get_first<uintptr_t>(12);
            destroyed.reserve(32768);

            static auto DestroyHook = safetyhook::create_mid(destructor.get_first(0), [](SafetyHookContext& regs)
            {
                WorldShutdown::OnDestroy(regs.ecx, regs.esp);
            });
            shDeletingDestructor = safetyhook::create_inline(*(void**)pBaseVtable, DeletingDestructor);
            static auto ShutdownHook = safetyhook::create_mid(shutdown.get_first(0), [](SafetyHookContext& regs)
            {
                AcquireSRWLockShared(&WorldShutdown::lock);
                auto count = WorldShutdown::destroyed.size();
                ReleaseSRWLockShared(&WorldShutdown::lock);
                FusionLog::WriteText("World", "Shutdown", std::format("the world's shutdown starts; {} buildings were destroyed this run", count));
            });

            FusionLog::WriteText("World", "Shutdown", std::format("on: destructor {}, deleting destructor {} ({}), shutdown {}, {} code ranges",
                Describe(uintptr_t(destructor.get_first(0))), Describe(*(uintptr_t*)pBaseVtable),
                shDeletingDestructor ? "hooked" : "NOT hooked", Describe(pShutdown), codeRanges.size()));
        };
    }
} WorldShutdownGuard;
