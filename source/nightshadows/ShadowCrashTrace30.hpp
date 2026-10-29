// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

#pragma once
#include <windows.h>
#include <cstdint>
#include "CrashPathContext31.hpp"

// Private diagnostic build only. First-chance evidence is not proof that an
// exception was fatal. Never change exception context or consume an exception.
namespace shadow_crash_trace {
struct Record {
    std::uint32_t magic=0x30544353, version=1, bytes=sizeof(Record);
    std::uint32_t tick=0, thread=0, code=0, flags=0, gameBase=0, faultModule=0;
    std::uint32_t address=0, operation=0, target=0;
    std::uint32_t eax=0,ebx=0,ecx=0,edx=0,esi=0,edi=0,ebp=0,esp=0,eip=0,eflags=0;
    std::uint32_t stackBytes=0;
    std::uint32_t stack[256]{};
};
static HANDLE file=INVALID_HANDLE_VALUE;
static PVOID handler=nullptr;
static volatile LONG busy=0,count=0;
static std::uint32_t gameBase=0;
static LONG CALLBACK Observe(EXCEPTION_POINTERS* info) noexcept {
    if(!info||!info->ExceptionRecord||!info->ContextRecord||file==INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code=info->ExceptionRecord->ExceptionCode;
    if(code!=EXCEPTION_ACCESS_VIOLATION&&code!=EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedCompareExchange(&busy,1,0)!=0)return EXCEPTION_CONTINUE_SEARCH;
    if(InterlockedIncrement(&count)<=64) {
        Record r{};
        const auto* e=info->ExceptionRecord;const auto* c=info->ContextRecord;
        r.tick=GetTickCount();r.thread=GetCurrentThreadId();r.code=code;r.flags=e->ExceptionFlags;
        r.gameBase=gameBase;r.address=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(e->ExceptionAddress));
        if(e->NumberParameters>=2){r.operation=static_cast<std::uint32_t>(e->ExceptionInformation[0]);r.target=static_cast<std::uint32_t>(e->ExceptionInformation[1]);}
        r.eax=c->Eax;r.ebx=c->Ebx;r.ecx=c->Ecx;r.edx=c->Edx;r.esi=c->Esi;r.edi=c->Edi;
        r.ebp=c->Ebp;r.esp=c->Esp;r.eip=c->Eip;r.eflags=c->EFlags;
        MEMORY_BASIC_INFORMATION region{};
        if(VirtualQuery(e->ExceptionAddress,&region,sizeof(region)))
            r.faultModule=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(region.AllocationBase));
        if(VirtualQuery(reinterpret_cast<void*>(c->Esp),&region,sizeof(region))&&region.State==MEM_COMMIT&&!(region.Protect&(PAGE_GUARD|PAGE_NOACCESS))) {
            const auto end=reinterpret_cast<std::uintptr_t>(region.BaseAddress)+region.RegionSize;
            const auto available=end-c->Esp;
            const SIZE_T bytes=available<sizeof(r.stack)?available:sizeof(r.stack);
            SIZE_T read=0;
            ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(c->Esp),r.stack,bytes,&read);
            r.stackBytes=static_cast<std::uint32_t>(read);
        }
        crash_path_context::Capture(gameBase,*c);
        DWORD written=0;WriteFile(file,&r,sizeof(r),&written,nullptr);
        FlushFileBuffers(file);
    }
    InterlockedExchange(&busy,0);
    return EXCEPTION_CONTINUE_SEARCH;
}
inline bool Install(const wchar_t* path) noexcept {
    if(handler)return true;
    file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    gameBase=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    handler=AddVectoredExceptionHandler(1,Observe);
    if(!handler){CloseHandle(file);file=INVALID_HANDLE_VALUE;return false;}
    return true;
}
inline void Remove() noexcept {
    // Test-only teardown when no other thread can be in the handler.
    if(handler){RemoveVectoredExceptionHandler(handler);handler=nullptr;}
    crash_path_context::Remove();
    if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;}
}
}
