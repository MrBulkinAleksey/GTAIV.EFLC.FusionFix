#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
namespace crash_path_context {
struct Block { uint32_t address{}, bytes{}; uint8_t data[8192]{}; };
struct Record {
    uint32_t magic=0x31504353, version=1, bytes=sizeof(Record), tick{}, eip{}, gameBase{};
    Block currentNode, linkedNode, linkData, pathManager;
};
inline HANDLE file=INVALID_HANDLE_VALUE;
// Called only while the primary observer's nonblocking reentrancy guard is held.
// Static storage avoids adding a large record to an already-failing thread stack.
inline Record storage{};
inline void Read(Block& block,uint32_t address,SIZE_T requested) noexcept {
    block.address=address;block.bytes=0;
    if(address<0x10000 || requested>sizeof(block.data)) return;
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region)) ||
       region.State!=MEM_COMMIT || (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return;
    const auto end=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
    const SIZE_T available=end-address;
    const SIZE_T length=available<requested ? available:requested;
    SIZE_T count=0;
    ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),block.data,length,&count);
    block.bytes=static_cast<uint32_t>(count);
}
inline void Capture(uint32_t base,const CONTEXT& c) noexcept {
    if(file==INVALID_HANDLE_VALUE || (c.Eip-base!=0x4E7DC5 && c.Eip-base!=0x4E7E3B)) return;
    std::memset(&storage,0,sizeof(storage));storage.magic=0x31504353;storage.version=1;
    storage.bytes=sizeof(storage);storage.tick=GetTickCount();storage.eip=c.Eip;storage.gameBase=base;
    Read(storage.currentNode,c.Edi,64);
    Read(storage.linkedNode,c.Esi,64);
    uint32_t stack[20]{};SIZE_T copied=0;
    if(ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(c.Esp),stack,sizeof(stack),&copied) && copied==sizeof(stack)) {
        Read(storage.pathManager,stack[0x18/4],0x1C10);
        // At the first load fault, EDX holds the index; the stack copy is not
        // written until the following instructions. At the later fault EDX
        // has been repurposed, so use the now-valid stack copy instead.
        const uint32_t index=c.Eip-base==0x4E7DC5 ? c.Edx:stack[0x4C/4];
        const auto pointer=static_cast<uint64_t>(stack[0x30/4])+static_cast<uint64_t>(index)*8;
        if(pointer<=UINT32_MAX) Read(storage.linkData,static_cast<uint32_t>(pointer),64);
    }
    DWORD written=0;WriteFile(file,&storage,sizeof(storage),&written,nullptr);FlushFileBuffers(file);
}
inline bool Install(const wchar_t* path) noexcept {
    file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    return file!=INVALID_HANDLE_VALUE;
}
inline void Remove() noexcept { if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;} }
}
