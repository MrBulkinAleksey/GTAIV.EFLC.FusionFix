#pragma once
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <cstdint>
#pragma comment(lib,"psapi.lib")
namespace traffic_signal::fresh_page {
using Check=bool(*)(uintptr_t,size_t) noexcept;
using Query=BOOL(WINAPI*)(HANDLE,PVOID,DWORD);
struct Preserve {
 alignas(16) unsigned char state[512];DWORD error;
 Preserve() noexcept {_fxsave(state);error=GetLastError();}
 ~Preserve() noexcept {SetLastError(error);_fxrstor(state);}
};
inline DWORD PageSize() noexcept {
 static const DWORD value=[]{SYSTEM_INFO info;GetSystemInfo(&info);return info.dwPageSize;}();
 return value;
}
__declspec(noinline) inline bool Readable(uintptr_t address,size_t size,Check fallback,Query query=QueryWorkingSetEx) noexcept {
 const Preserve preserve;
 if(address<0x10000||!size||address>UINTPTR_MAX-size)return false;
 const auto page=PageSize();
 if(!page||(page&(page-1))||address/page!=(address+size-1)/page)return fallback(address,size);
 PSAPI_WORKING_SET_EX_INFORMATION info{};info.VirtualAddress=reinterpret_cast<void*>(address);
 if(!query(GetCurrentProcess(),&info,sizeof(info))||!info.VirtualAttributes.Valid)return fallback(address,size);
 const auto protection=info.VirtualAttributes.Win32Protection;
 if(protection&(PAGE_GUARD|PAGE_NOACCESS))return false;
 // Reserved bits are unspecified and must be ignored. Reject supported exotic flags.
 if(info.VirtualAttributes.Locked||info.VirtualAttributes.LargePage||info.VirtualAttributes.Bad)return fallback(address,size);
 switch(protection){
 case PAGE_READONLY:case PAGE_READWRITE:case PAGE_WRITECOPY:
 case PAGE_EXECUTE_READ:case PAGE_EXECUTE_READWRITE:case PAGE_EXECUTE_WRITECOPY:
  return true;
 default:return fallback(address,size); // Includes execute-only and modifiers.
 }
}
}
