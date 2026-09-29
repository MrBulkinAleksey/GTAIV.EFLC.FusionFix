#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <Psapi.h>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

// Crash log and minidump next to the ASI (GTAIV.EFLC.FusionFix.crash.log and .dmp), written by
// an unhandled exception filter installed as early as the ASI loads, before the update folder
// archives are built. It chains to the filter that was there before, so the game and other
// ASIs see the crash as they would without it. Each crash overwrites the previous one.
//
// UpdateLog: one line per .img or .rpf folder in update, GTAIV.EFLC.FusionFix.update.log.
namespace CrashLog
{
    inline wchar_t basePath[MAX_PATH] = {}; // the ASI's path without its extension
    inline LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
    inline volatile LONG handling = 0;

    inline std::wstring PathWithSuffix(const wchar_t* suffix)
    {
        return std::wstring(basePath) + suffix;
    }

    // Address as module+offset, the form a disassembler or the pdb takes.
    inline void PrintAddress(FILE* f, uintptr_t address)
    {
        HMODULE module = nullptr;
        wchar_t name[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &module) && module &&
            GetModuleFileNameW(module, name, MAX_PATH))
        {
            const wchar_t* file = wcsrchr(name, L'\\');
            fwprintf(f, L"%08IX  %s+%IX", address, file ? file + 1 : name, address - reinterpret_cast<uintptr_t>(module));
        }
        else
            fwprintf(f, L"%08IX", address);
    }

    inline bool IsCode(uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION region = {};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) || region.State != MEM_COMMIT ||
            region.Type != MEM_IMAGE)
            return false;
        return (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    inline void WriteLog(EXCEPTION_POINTERS* info)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, PathWithSuffix(L".crash.log").c_str(), L"w, ccs=UTF-8") || !f)
            return;

        const auto* e = info->ExceptionRecord;
        const auto* c = info->ContextRecord;
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::time_t now = std::time(nullptr);
        std::tm local = {};
        localtime_s(&local, &now);
        wchar_t when[64] = {};
        wcsftime(when, std::size(when), L"%Y-%m-%d %H:%M:%S", &local);
        fwprintf(f, L"Crash at %s in %s, thread %lu\n\n", when, exe, GetCurrentThreadId());

        fwprintf(f, L"Exception %08lX at ", e->ExceptionCode);
        PrintAddress(f, reinterpret_cast<uintptr_t>(e->ExceptionAddress));
        fwprintf(f, L"\n");
        if (e->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && e->NumberParameters >= 2)
        {
            const auto op = e->ExceptionInformation[0];
            fwprintf(f, L"Access violation %s %08IX\n", op == 0 ? L"reading" : op == 1 ? L"writing" : L"executing",
                     static_cast<uintptr_t>(e->ExceptionInformation[1]));
        }

        fwprintf(f, L"\nEAX %08lX  EBX %08lX  ECX %08lX  EDX %08lX\nESI %08lX  EDI %08lX  EBP %08lX  ESP %08lX\nEIP %08lX  EFLAGS %08lX\n",
                 c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags);

        // Values on the stack that point into code: most are return addresses, the callers
        // that led here, though some are stale or plain function pointers.
        fwprintf(f, L"\nCode addresses on the stack:\n");
        uint32_t stack[1024] = {};
        SIZE_T read = 0;
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(c->Esp), &region, sizeof(region)) && region.State == MEM_COMMIT)
        {
            const auto end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
            const SIZE_T bytes = (std::min)(static_cast<SIZE_T>(end - c->Esp), sizeof(stack));
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(c->Esp), stack, bytes, &read);
        }
        int shown = 0;
        for (SIZE_T i = 0; i < read / sizeof(uint32_t) && shown < 48; ++i)
        {
            if (!IsCode(stack[i]))
                continue;
            fwprintf(f, L"  [ESP+%04IX] ", i * sizeof(uint32_t));
            PrintAddress(f, stack[i]);
            fwprintf(f, L"\n");
            ++shown;
        }

        fwprintf(f, L"\nModules:\n");
        HMODULE modules[512] = {};
        DWORD needed = 0;
        if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        {
            for (DWORD i = 0; i < (std::min)(needed / DWORD(sizeof(HMODULE)), DWORD(std::size(modules))); ++i)
            {
                MODULEINFO mi = {};
                wchar_t name[MAX_PATH] = {};
                K32GetModuleInformation(GetCurrentProcess(), modules[i], &mi, sizeof(mi));
                GetModuleFileNameW(modules[i], name, MAX_PATH);
                fwprintf(f, L"  %08IX-%08IX  %s\n", reinterpret_cast<uintptr_t>(mi.lpBaseOfDll),
                         reinterpret_cast<uintptr_t>(mi.lpBaseOfDll) + mi.SizeOfImage, name);
            }
        }
        fclose(f);
    }

    inline void WriteDump(EXCEPTION_POINTERS* info)
    {
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        if (!dbghelp)
            return;
        using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        auto writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        HANDLE file = CreateFileW(PathWithSuffix(L".crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (writeDump && file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION exception = { GetCurrentThreadId(), info, FALSE };
            writeDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                      &exception, nullptr, nullptr);
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }

    inline LONG WINAPI Filter(EXCEPTION_POINTERS* info)
    {
        if (info && info->ExceptionRecord && info->ContextRecord && InterlockedExchange(&handling, 1) == 0)
        {
            WriteLog(info);
            WriteDump(info);
        }
        return previousFilter ? previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    }

    inline void Install()
    {
        if (!basePath[0])
        {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&Install), &self);
            GetModuleFileNameW(self, basePath, MAX_PATH);
            if (auto dot = wcsrchr(basePath, L'.'))
                *dot = L'\0';
        }
        // Installed again once the game runs, in case something replaced it meanwhile.
        auto previous = SetUnhandledExceptionFilter(Filter);
        if (previous != Filter)
            previousFilter = previous;
    }
}

namespace UpdateLog
{
    inline void Write(const std::wstring& line)
    {
        static std::mutex mutex;
        static bool started = false;
        std::lock_guard lock(mutex);
        if (!CrashLog::basePath[0])
            return;
        FILE* f = nullptr;
        if (_wfopen_s(&f, CrashLog::PathWithSuffix(L".update.log").c_str(), started ? L"a, ccs=UTF-8" : L"w, ccs=UTF-8") || !f)
            return;
        started = true;
        fwprintf(f, L"%s\n", line.c_str());
        fclose(f);
    }
}
