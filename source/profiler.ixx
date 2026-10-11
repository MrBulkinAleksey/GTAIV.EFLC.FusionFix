module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "winmm.lib")

export module profiler;

import common;

// A sampling profiler ([MISC] Profiler in the ini): a thread of its own stops the busiest threads of the game
// Profiler times a second for an instant each, takes where each one is (its instruction and the return addresses
// on its stack) and lets it go, and every ProfilerInterval seconds writes GTAIV.EFLC.FusionFix.Profiler.log: every
// thread's CPU time, and for the sampled ones the functions they were in (self) and the ones on their stack (total).
// Game functions are named by their address in the Complete Edition exe (base 0x400000), whatever base it got;
// others by their module and the nearest export.
//
// While a thread is stopped only its context and a copy of the top of its stack are taken: nothing is allocated
// or locked then, as the stopped thread may hold the heap's lock or the loader's.
namespace Profiler
{
    constexpr uint32_t StackCopy = 1024;    // bytes of a stopped thread's stack looked at for return addresses
    constexpr uint32_t MaxFrames = 12;
    constexpr uint32_t TopFunctions = 20;

    int32_t nRate = 0;                      // samples a second of each sampled thread, 0: off
    int32_t nThreads = 6;                   // how many of the busiest threads are sampled
    int32_t nInterval = 30;                 // seconds a report
    DWORD nMainThread = 0;
    std::atomic<bool> bRunning = false;

    // Executable ranges of the loaded modules, and their exports
    struct Module
    {
        uintptr_t base = 0, end = 0;
        std::string name;
        bool game = false;
        std::vector<std::pair<uintptr_t, uintptr_t>> code;      // executable, committed
        std::vector<std::pair<uintptr_t, std::string>> exports; // sorted by address
    };
    std::vector<Module> modules;

    void LoadExports(Module& module)
    {
        auto base = (uint8_t*)module.base;
        auto dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return;
        auto nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return;
        auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!directory.VirtualAddress || !directory.Size)
            return;
        auto exports = (IMAGE_EXPORT_DIRECTORY*)(base + directory.VirtualAddress);
        auto names = (DWORD*)(base + exports->AddressOfNames);
        auto ordinals = (WORD*)(base + exports->AddressOfNameOrdinals);
        auto functions = (DWORD*)(base + exports->AddressOfFunctions);
        for (DWORD i = 0; i < exports->NumberOfNames; i++)
        {
            auto rva = functions[ordinals[i]];
            // Forwarders point into the export directory itself
            if (rva >= directory.VirtualAddress && rva < directory.VirtualAddress + directory.Size)
                continue;
            module.exports.emplace_back(module.base + rva, (const char*)(base + names[i]));
        }
        std::sort(module.exports.begin(), module.exports.end());
    }

    void LoadModules()
    {
        modules.clear();
        HMODULE handles[1024];
        DWORD needed = 0;
        auto process = GetCurrentProcess();
        if (!EnumProcessModules(process, handles, sizeof(handles), &needed))
            return;
        auto game = GetModuleHandleW(nullptr);
        for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), DWORD(std::size(handles))); i++)
        {
            MODULEINFO info{};
            char name[MAX_PATH] = {};
            if (!GetModuleInformation(process, handles[i], &info, sizeof(info)))
                continue;
            GetModuleBaseNameA(process, handles[i], name, sizeof(name));
            Module module;
            module.base = uintptr_t(info.lpBaseOfDll);
            module.end = module.base + info.SizeOfImage;
            module.name = name;
            module.game = handles[i] == game;
            for (uintptr_t address = module.base; address < module.end;)
            {
                MEMORY_BASIC_INFORMATION memory{};
                if (!VirtualQuery((void*)address, &memory, sizeof(memory)) || !memory.RegionSize)
                    break;
                auto regionEnd = std::min<uintptr_t>(module.end, uintptr_t(memory.BaseAddress) + memory.RegionSize);
                constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                if (memory.State == MEM_COMMIT && (memory.Protect & executable) && !(memory.Protect & PAGE_GUARD))
                {
                    if (!module.code.empty() && module.code.back().second == address)
                        module.code.back().second = regionEnd;
                    else
                        module.code.emplace_back(address, regionEnd);
                }
                address = regionEnd;
            }
            if (!module.game)
                LoadExports(module);
            modules.push_back(std::move(module));
        }
        std::sort(modules.begin(), modules.end(), [](auto& a, auto& b) { return a.base < b.base; });
    }

    const Module* FindModule(uintptr_t address)
    {
        auto it = std::upper_bound(modules.begin(), modules.end(), address, [](uintptr_t a, const Module& m) { return a < m.base; });
        if (it == modules.begin())
            return nullptr;
        --it;
        return address < it->end ? &*it : nullptr;
    }

    // The executable range of a module that address is in
    const std::pair<uintptr_t, uintptr_t>* FindCode(const Module* module, uintptr_t address)
    {
        if (!module)
            return nullptr;
        for (auto& range : module->code)
            if (address >= range.first && address < range.second)
                return &range;
        return nullptr;
    }

    // Whether the instruction before address is a call: E8 rel32, or FF /2 as call reg, [reg], [reg+disp8],
    // [reg+disp32], [disp32] and with a SIB byte
    bool IsReturnAddress(uintptr_t address)
    {
        auto module = FindModule(address);
        auto code = FindCode(module, address);
        if (!code || address < code->first + 8)
            return false;
        auto p = (const uint8_t*)address;
        if (p[-5] == 0xE8)
            return true;
        auto isCall = [](uint8_t modrm) { return ((modrm >> 3) & 7) == 2; };
        if (p[-2] == 0xFF && isCall(p[-1]) && (p[-1] >> 6) == 3)
            return true;
        if (p[-2] == 0xFF && isCall(p[-1]) && (p[-1] >> 6) == 0 && (p[-1] & 7) != 4 && (p[-1] & 7) != 5)
            return true;
        if (p[-3] == 0xFF && isCall(p[-2]) && (p[-2] >> 6) == 1 && (p[-2] & 7) != 4)
            return true;
        if (p[-3] == 0xFF && isCall(p[-2]) && (p[-2] >> 6) == 0 && (p[-2] & 7) == 4)
            return true;
        if (p[-4] == 0xFF && isCall(p[-3]) && (p[-3] >> 6) == 1 && (p[-3] & 7) == 4)
            return true;
        if (p[-6] == 0xFF && isCall(p[-5]) && ((p[-5] >> 6) == 2 || ((p[-5] >> 6) == 0 && (p[-5] & 7) == 5)) && (p[-5] & 7) != 4)
            return true;
        if (p[-7] == 0xFF && isCall(p[-6]) && ((p[-6] & 7) == 4) && ((p[-6] >> 6) == 2 || ((p[-6] >> 6) == 0 && (p[-5] & 7) == 5)))
            return true;
        return false;
    }

    // Where the function holding address starts: functions are 16-byte aligned with int3 padding before them
    // (or follow a ret right at their start), so the nearest such boundary below it, within 64 KB
    std::unordered_map<uintptr_t, uintptr_t> functionStarts;
    uintptr_t FunctionStart(uintptr_t address)
    {
        if (auto it = functionStarts.find(address); it != functionStarts.end())
            return it->second;
        uintptr_t start = address;
        auto module = FindModule(address);
        if (auto code = FindCode(module, address))
        {
            uintptr_t floor = std::max(code->first + 1, address > 0x10000 ? address - 0x10000 : 0);
            for (uintptr_t a = address & ~uintptr_t(15); a >= floor; a -= 16)
            {
                auto before = *(const uint8_t*)(a - 1);
                if (before == 0xCC || before == 0xC3)
                {
                    start = a;
                    break;
                }
            }
        }
        functionStarts.emplace(address, start);
        return start;
    }

    std::string Name(uintptr_t function)
    {
        auto module = FindModule(function);
        if (!module)
            return std::format("{:#x}", function);
        if (module->game)
            return std::format("GTAIV {:#x}", function - module->base + 0x400000);
        auto it = std::upper_bound(module->exports.begin(), module->exports.end(), function,
            [](uintptr_t a, const std::pair<uintptr_t, std::string>& e) { return a < e.first; });
        if (it != module->exports.begin())
        {
            --it;
            if (function - it->first < 0x4000)
                return function == it->first ? std::format("{}!{}", module->name, it->second)
                                             : std::format("{}!{}+{:#x}", module->name, it->second, function - it->first);
        }
        return std::format("{}+{:#x}", module->name, function - module->base);
    }

    // Threads
    using NtQueryInformationThread_t = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    NtQueryInformationThread_t pNtQueryInformationThread = nullptr;
    using GetThreadDescription_t = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    GetThreadDescription_t pGetThreadDescription = nullptr;

    struct Thread
    {
        DWORD id = 0;
        HANDLE handle = nullptr;
        std::string name;
        uint64_t cpuLast = 0, cpuSecond = 0, cpuInterval = 0;
        bool alive = false, sampled = false;
        uintptr_t stackLow = 0, stackHigh = 0;  // the committed region the stack was last found in
        uint32_t samples = 0;
        std::unordered_map<uintptr_t, uint32_t> self, total;
    };
    std::unordered_map<DWORD, Thread> threads;

    uint64_t CpuTime(HANDLE handle)
    {
        FILETIME creation, exit, kernel, user;
        if (!GetThreadTimes(handle, &creation, &exit, &kernel, &user))
            return 0;
        return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) + (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
    }

    std::string ThreadName(DWORD id, HANDLE handle)
    {
        std::string name;
        if (pGetThreadDescription)
        {
            PWSTR description = nullptr;
            if (SUCCEEDED(pGetThreadDescription(handle, &description)) && description)
            {
                for (auto p = description; *p; p++)
                    name += *p < 0x80 ? char(*p) : '?';
                LocalFree(description);
            }
        }
        uintptr_t start = 0;
        if (pNtQueryInformationThread)
            pNtQueryInformationThread(handle, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
        auto at = start ? Name(start) : std::string("?");
        if (id == nMainThread)
            return name.empty() ? "main, " + at : name + ", main, " + at;
        return name.empty() ? at : name + ", " + at;
    }

    // Once a second: new threads in, ended ones out, the CPU time of each, and the busiest picked for sampling
    void RefreshThreads()
    {
        for (auto& [id, thread] : threads)
            thread.alive = false;
        auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 entry{ sizeof(entry) };
            auto self = GetCurrentThreadId();
            auto process = GetCurrentProcessId();
            for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                    continue;
                auto& thread = threads[entry.th32ThreadID];
                if (!thread.handle)
                {
                    thread.id = entry.th32ThreadID;
                    thread.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                    if (!thread.handle)
                    {
                        threads.erase(entry.th32ThreadID);
                        continue;
                    }
                    thread.name = ThreadName(thread.id, thread.handle);
                    thread.cpuLast = CpuTime(thread.handle);
                }
                thread.alive = true;
                auto cpu = CpuTime(thread.handle);
                thread.cpuSecond = cpu - std::min(cpu, thread.cpuLast);
                thread.cpuInterval += thread.cpuSecond;
                thread.cpuLast = cpu;
            }
            CloseHandle(snapshot);
        }
        for (auto it = threads.begin(); it != threads.end();)
        {
            if (!it->second.alive && !it->second.samples && !it->second.cpuInterval)
            {
                CloseHandle(it->second.handle);
                it = threads.erase(it);
            }
            else
                ++it;
        }

        std::vector<Thread*> busiest;
        for (auto& [id, thread] : threads)
        {
            thread.sampled = false;
            if (thread.alive && thread.cpuSecond > 100000) // 10 ms of the last second
                busiest.push_back(&thread);
        }
        std::sort(busiest.begin(), busiest.end(), [](auto a, auto b) { return a->cpuSecond > b->cpuSecond; });
        for (size_t i = 0; i < busiest.size() && i < size_t(nThreads); i++)
            busiest[i]->sampled = true;
    }

    // One sample of a thread: stopped, its instruction and the top of its stack copied, let go
    void Sample(Thread& thread)
    {
        alignas(16) uint8_t stack[StackCopy];
        uint32_t copied = 0;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(thread.handle) == DWORD(-1))
            return;
        bool ok = GetThreadContext(thread.handle, &context) != FALSE;
        if (ok)
        {
            uintptr_t esp = context.Esp;
            if (esp < thread.stackLow || esp >= thread.stackHigh)
            {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery((void*)esp, &memory, sizeof(memory)) && memory.State == MEM_COMMIT && !(memory.Protect & PAGE_GUARD))
                {
                    thread.stackLow = uintptr_t(memory.BaseAddress);
                    thread.stackHigh = thread.stackLow + memory.RegionSize;
                }
                else
                    thread.stackLow = thread.stackHigh = 0;
            }
            if (esp >= thread.stackLow && esp < thread.stackHigh)
            {
                copied = uint32_t(std::min<uintptr_t>(StackCopy, thread.stackHigh - esp)) & ~3u;
                memcpy(stack, (void*)esp, copied);
            }
        }
        ResumeThread(thread.handle);
        if (!ok)
            return;

        uintptr_t frames[MaxFrames + 1];
        uint32_t count = 0;
        frames[count++] = FunctionStart(context.Eip);
        for (uint32_t offset = 0; offset + 4 <= copied && count <= MaxFrames; offset += 4)
        {
            uintptr_t value = *(uint32_t*)(stack + offset);
            if (IsReturnAddress(value))
                frames[count++] = FunctionStart(value);
        }
        thread.samples++;
        thread.self[frames[0]]++;
        for (uint32_t i = 0; i < count; i++)
        {
            bool seen = false;
            for (uint32_t j = 0; j < i && !seen; j++)
                seen = frames[j] == frames[i];
            if (!seen)
                thread.total[frames[i]]++;
        }
    }

    uint64_t selfCpuLast = 0;

    void Report(double seconds)
    {
        auto selfCpu = CpuTime(GetCurrentThread());
        std::string text = std::format("{:.1f} s, {} samples a second of the {} busiest threads; the profiler itself took {:.1f}% of a core\n",
            seconds, nRate, nThreads, (selfCpu - selfCpuLast) * 100.0 / (seconds * 1.0e7));
        selfCpuLast = selfCpu;
        std::vector<Thread*> order;
        for (auto& [id, thread] : threads)
            if (thread.cpuInterval || thread.samples)
                order.push_back(&thread);
        std::sort(order.begin(), order.end(), [](auto a, auto b) { return a->cpuInterval > b->cpuInterval; });

        text += "CPU time by thread:\n";
        for (auto thread : order)
            if (thread->cpuInterval * 100.0 / (seconds * 1.0e7) >= 0.5)
                text += std::format("  {:6.1f}%  tid {:5}  {}\n", thread->cpuInterval * 100.0 / (seconds * 1.0e7), thread->id, thread->name);

        auto top = [&](const std::unordered_map<uintptr_t, uint32_t>& counts, uint32_t samples, const char* title)
        {
            std::vector<std::pair<uintptr_t, uint32_t>> sorted(counts.begin(), counts.end());
            std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
            text += std::format("    {}:\n", title);
            for (size_t i = 0; i < sorted.size() && i < TopFunctions; i++)
                text += std::format("      {:5.1f}%  {}\n", sorted[i].second * 100.0 / samples, Name(sorted[i].first));
        };
        for (auto thread : order)
        {
            if (!thread->samples)
                continue;
            text += std::format("tid {} ({}), {} samples, CPU {:.1f}%\n", thread->id, thread->name, thread->samples,
                thread->cpuInterval * 100.0 / (seconds * 1.0e7));
            top(thread->self, thread->samples, "in (self)");
            top(thread->total, thread->samples, "on the stack (total)");
        }
        FusionLog::WriteText("Profiler", "Report", text);

        for (auto& [id, thread] : threads)
        {
            thread.samples = 0;
            thread.cpuInterval = 0;
            thread.self.clear();
            thread.total.clear();
        }
    }

    void Run()
    {
        timeBeginPeriod(1);
        LoadModules();
        auto ntdll = GetModuleHandleW(L"ntdll.dll");
        pNtQueryInformationThread = ntdll ? (NtQueryInformationThread_t)GetProcAddress(ntdll, "NtQueryInformationThread") : nullptr;
        auto kernelBase = GetModuleHandleW(L"KernelBase.dll");
        pGetThreadDescription = kernelBase ? (GetThreadDescription_t)GetProcAddress(kernelBase, "GetThreadDescription") : nullptr;
        if (!pGetThreadDescription)
            pGetThreadDescription = (GetThreadDescription_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        LARGE_INTEGER frequency, now, lastRefresh, lastReport, next;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&now);
        lastRefresh = lastReport = next = now;
        selfCpuLast = CpuTime(GetCurrentThread());
        RefreshThreads();
        uint64_t moduleRefresh = 0;
        auto tick = frequency.QuadPart / std::max(nRate, 1);
        while (bRunning)
        {
            for (auto& [id, thread] : threads)
                if (thread.sampled && thread.alive)
                    Sample(thread);

            // Sleep takes whole milliseconds (1 to 2 with timeBeginPeriod), so at 1000 a second and more the rate is
            // what the timer gives; the report counts the samples taken
            next.QuadPart += tick;
            QueryPerformanceCounter(&now);
            if (next.QuadPart < now.QuadPart)
                next = now;
            Sleep(DWORD(std::max<LONGLONG>(1, (next.QuadPart - now.QuadPart) * 1000 / frequency.QuadPart)));

            QueryPerformanceCounter(&now);
            if (now.QuadPart - lastRefresh.QuadPart >= frequency.QuadPart)
            {
                lastRefresh = now;
                RefreshThreads();
                // Modules loaded later (plugins, the driver) are named too
                if (++moduleRefresh % 10 == 0)
                {
                    LoadModules();
                    functionStarts.clear();
                }
            }
            if (now.QuadPart - lastReport.QuadPart >= frequency.QuadPart * nInterval)
            {
                Report(double(now.QuadPart - lastReport.QuadPart) / double(frequency.QuadPart));
                lastReport = now;
            }
        }
        timeEndPeriod(1);
    }
}

class ProfilerFeature
{
public:
    ProfilerFeature()
    {
        using namespace Profiler;

        FusionFix::onInitEvent() += []()
        {
            CIniReader iniReader("");
            nRate = std::clamp(iniReader.ReadInteger("MISC", "Profiler", 0), 0, 4000);
            nThreads = std::clamp(iniReader.ReadInteger("MISC", "ProfilerThreads", 6), 1, 32);
            nInterval = std::clamp(iniReader.ReadInteger("MISC", "ProfilerInterval", 30), 5, 600);
            if (!nRate)
                return;
            nMainThread = GetCurrentThreadId();
            bRunning = true;
            std::thread(Run).detach();
            FusionLog::Write("Profiler", "Settings", "%d samples a second of the %d busiest threads, a report every %d s; main thread %u\n",
                nRate, nThreads, nInterval, nMainThread);
        };

        FusionFix::onShutdownEvent() += []()
        {
            Profiler::bRunning = false;
        };
    }
} ProfilerFeature;
