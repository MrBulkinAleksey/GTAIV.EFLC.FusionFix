module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cinttypes>
#include <intrin.h>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

export module devicereset;

import common;
import comvars;
import vtablepatch;

// What keeps a device reset from going through. D3D9 (and DXVK) refuse Reset with D3DERR_INVALIDCALL while any
// D3DPOOL_DEFAULT resource is alive, and the game then stops with its fatal error DD3D80. Each default pool texture,
// surface and buffer made on the device is booked here with what it is and the code on the stack that made it; a
// COM object of ours rides in its private data (D3DSPD_IUNKNOWN), which the runtime releases with the resource and so
// takes it off the books. A Reset that fails lists the ones still alive in GTAIV.EFLC.FusionFix.DeviceReset.log.
// The booking scans the stack and formats a line for every such resource the game streams in, so it runs only with
// DeviceResetLog = 1 in [DEBUG]; without it a failed Reset is still logged, without the list.
namespace DeviceReset
{
    // {6D3A1F52-9C47-4E0B-A8B1-2F5C7E9D0A13}
    constexpr GUID TrackerGuid = { 0x6d3a1f52, 0x9c47, 0x4e0b, { 0xa8, 0xb1, 0x2f, 0x5c, 0x7e, 0x9d, 0x0a, 0x13 } };
    constexpr size_t StackDepth = 8;

    struct Entry
    {
        uint64_t id = 0;
        std::string what;
        DWORD thread = 0;
        ULONGLONG made = 0;
        uintptr_t caller = 0;
        uintptr_t stack[StackDepth]{};
    };

    std::mutex Mutex;
    std::unordered_map<IUnknown*, Entry> Alive;
    uint64_t NextId = 1;
    uint32_t Resets = 0;
    bool bBooking = false;

    struct Module { uintptr_t begin = 0, end = 0; std::string name; };
    std::vector<Module> Modules;   // the game's and ours, where code on the stack is looked for

    void AddModule(HMODULE module, const char* name)
    {
        if (!module)
            return;
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        auto begin = reinterpret_cast<uintptr_t>(module);
        Modules.push_back({ begin, begin + nt->OptionalHeader.SizeOfImage, name });
    }

    const Module* ModuleAt(uintptr_t address)
    {
        for (auto& m : Modules)
            if (address >= m.begin + 0x1000 && address < m.end)
                return &m;
        return nullptr;
    }

    std::string Describe(uintptr_t address)
    {
        if (auto m = ModuleAt(address))
            return std::format("{}+{:X}", m->name, address - m->begin);
        HMODULE module = nullptr;
        char path[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) &&
            GetModuleFileNameA(module, path, MAX_PATH))
        {
            std::string name = path;
            if (auto slash = name.find_last_of("\\/"); slash != std::string::npos)
                name = name.substr(slash + 1);
            return std::format("{}+{:X}", name, address - reinterpret_cast<uintptr_t>(module));
        }
        return std::format("{:08X}", address);
    }

    // A return address follows a call: E8 rel32, or FF /2 through a register, [reg], [reg+disp8] or [disp32 / reg+disp32]
    bool FollowsCall(uintptr_t address)
    {
        auto p = reinterpret_cast<const uint8_t*>(address);
        if (p[-5] == 0xE8)
            return true;
        if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)
            return true;
        if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
            return true;
        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)
            return true;
        return false;
    }

    // The return addresses into the game and the plugin on the stack, nearest first; a raw scan, frame pointers or not
    void ScanStack(uintptr_t (&out)[StackDepth])
    {
        auto top = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
        auto base = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
        size_t found = 0;
        for (auto slot = top; slot + 4 <= base && slot < top + 0x2000 && found < StackDepth; slot += 4)
        {
            auto value = *reinterpret_cast<const uintptr_t*>(slot);
            if (ModuleAt(value) && FollowsCall(value))
                out[found++] = value;
        }
    }

    struct Tracker final : IUnknown
    {
        LONG refs = 1;
        IUnknown* resource;
        uint64_t id;

        Tracker(IUnknown* resource, uint64_t id) : resource(resource), id(id) {}

        HRESULT __stdcall QueryInterface(REFIID riid, void** object) override
        {
            if (!object)
                return E_POINTER;
            if (riid == __uuidof(IUnknown))
            {
                *object = static_cast<IUnknown*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }
        ULONG __stdcall AddRef() override { return InterlockedIncrement(&refs); }
        ULONG __stdcall Release() override
        {
            auto left = InterlockedDecrement(&refs);
            if (left == 0)
            {
                {
                    std::lock_guard lock(Mutex);
                    if (auto it = Alive.find(resource); it != Alive.end() && it->second.id == id)
                        Alive.erase(it);
                }
                delete this;
            }
            return left;
        }
    };

    void Book(IDirect3DResource9* resource, std::string what, uintptr_t caller)
    {
        if (!resource)
            return;
        Entry entry;
        entry.what = std::move(what);
        entry.thread = GetCurrentThreadId();
        entry.made = GetTickCount64();
        entry.caller = caller;
        ScanStack(entry.stack);
        uint64_t id;
        {
            std::lock_guard lock(Mutex);
            id = NextId++;
            entry.id = id;
            Alive[resource] = std::move(entry);
        }
        auto tracker = new Tracker(resource, id);
        if (FAILED(resource->SetPrivateData(TrackerGuid, static_cast<IUnknown*>(tracker), sizeof(IUnknown*), D3DSPD_IUNKNOWN)))
        {
            std::lock_guard lock(Mutex);
            if (auto it = Alive.find(resource); it != Alive.end() && it->second.id == id)
                it->second.what += " (not tracked: SetPrivateData refused)";
        }
        tracker->Release();
    }

    std::string Size(UINT width, UINT height)
    {
        return std::format("{}x{}", width, height);
    }

    using CreateTextureFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
    using CreateVolumeTextureFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
    using CreateCubeTextureFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
    using CreateVertexBufferFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
    using CreateIndexBufferFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
    using CreateSurfaceFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
    using CreateOffscreenPlainSurfaceFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*);
    using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    CreateTextureFn RealCreateTexture = nullptr;
    CreateVolumeTextureFn RealCreateVolumeTexture = nullptr;
    CreateCubeTextureFn RealCreateCubeTexture = nullptr;
    CreateVertexBufferFn RealCreateVertexBuffer = nullptr;
    CreateIndexBufferFn RealCreateIndexBuffer = nullptr;
    CreateSurfaceFn RealCreateRenderTarget = nullptr;
    CreateSurfaceFn RealCreateDepthStencilSurface = nullptr;
    CreateOffscreenPlainSurfaceFn RealCreateOffscreenPlainSurface = nullptr;
    ResetFn RealReset = nullptr;

    HRESULT __stdcall CreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
        IDirect3DTexture9** texture, HANDLE* shared)
    {
        auto hr = RealCreateTexture(device, width, height, levels, usage, format, pool, texture, shared);
        if (SUCCEEDED(hr) && pool == D3DPOOL_DEFAULT && texture)
            Book(*texture, std::format("texture {} levels {} format {} usage {:X}", Size(width, height), levels, static_cast<int>(format), usage),
                reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return hr;
    }

    HRESULT __stdcall CreateVolumeTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
        D3DPOOL pool, IDirect3DVolumeTexture9** texture, HANDLE* shared)
    {
        auto hr = RealCreateVolumeTexture(device, width, height, depth, levels, usage, format, pool, texture, shared);
        if (SUCCEEDED(hr) && pool == D3DPOOL_DEFAULT && texture)
            Book(*texture, std::format("volume texture {}x{} format {} usage {:X}", Size(width, height), depth, static_cast<int>(format), usage),
                reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return hr;
    }

    HRESULT __stdcall CreateCubeTexture(IDirect3DDevice9* device, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
        IDirect3DCubeTexture9** texture, HANDLE* shared)
    {
        auto hr = RealCreateCubeTexture(device, edge, levels, usage, format, pool, texture, shared);
        if (SUCCEEDED(hr) && pool == D3DPOOL_DEFAULT && texture)
            Book(*texture, std::format("cube texture {} format {} usage {:X}", edge, static_cast<int>(format), usage),
                reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return hr;
    }

    HRESULT __stdcall CreateVertexBuffer(IDirect3DDevice9* device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** buffer,
        HANDLE* shared)
    {
        auto hr = RealCreateVertexBuffer(device, length, usage, fvf, pool, buffer, shared);
        if (SUCCEEDED(hr) && pool == D3DPOOL_DEFAULT && buffer)
            Book(*buffer, std::format("vertex buffer {} bytes usage {:X}", length, usage), reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return hr;
    }

    HRESULT __stdcall CreateIndexBuffer(IDirect3DDevice9* device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** buffer,
        HANDLE* shared)
    {
        auto hr = RealCreateIndexBuffer(device, length, usage, format, pool, buffer, shared);
        if (SUCCEEDED(hr) && pool == D3DPOOL_DEFAULT && buffer)
            Book(*buffer, std::format("index buffer {} bytes usage {:X}", length, usage), reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return hr;
    }

    HRESULT __stdcall CreateRenderTarget(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples, DWORD quality,
        BOOL lockable, IDirect3DSurface9** surface, HANDLE* shared)
    {
        auto hr = RealCreateRenderTarget(device, width, height, format, samples, quality, lockable, surface, shared);
        if (SUCCEEDED(hr) && surface)
            Book(*surface, std::format("render target surface {} format {} samples {}", Size(width, height), static_cast<int>(format), static_cast<int>(samples)),
                reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return hr;
    }

    HRESULT __stdcall CreateDepthStencilSurface(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE samples,
        DWORD quality, BOOL discard, IDirect3DSurface9** surface, HANDLE* shared)
    {
        auto hr = RealCreateDepthStencilSurface(device, width, height, format, samples, quality, discard, surface, shared);
        if (SUCCEEDED(hr) && surface)
            Book(*surface, std::format("depth stencil surface {} format {} samples {}", Size(width, height), static_cast<int>(format), static_cast<int>(samples)),
                reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return hr;
    }

    HRESULT __stdcall CreateOffscreenPlainSurface(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, D3DPOOL pool,
        IDirect3DSurface9** surface, HANDLE* shared)
    {
        auto hr = RealCreateOffscreenPlainSurface(device, width, height, format, pool, surface, shared);
        if (SUCCEEDED(hr) && pool == D3DPOOL_DEFAULT && surface)
            Book(*surface, std::format("offscreen plain surface {} format {}", Size(width, height), static_cast<int>(format)),
                reinterpret_cast<uintptr_t>(_ReturnAddress()));
        return hr;
    }

    HRESULT __stdcall Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
    {
        auto hr = RealReset(device, parameters);
        ++Resets;

        std::vector<std::pair<IUnknown*, Entry>> alive;
        {
            std::lock_guard lock(Mutex);
            alive.assign(Alive.begin(), Alive.end());
        }
        if (SUCCEEDED(hr))
        {
            if (bBooking)
                FusionLog::Write("DeviceReset", "Reset", "reset %u to %ux%u: done, %zu default pool resources alive after it\n", Resets,
                    parameters ? parameters->BackBufferWidth : 0, parameters ? parameters->BackBufferHeight : 0, alive.size());
            else
                FusionLog::Write("DeviceReset", "Reset", "reset %u to %ux%u: done\n", Resets,
                    parameters ? parameters->BackBufferWidth : 0, parameters ? parameters->BackBufferHeight : 0);
            return hr;
        }
        if (!bBooking)
        {
            FusionLog::Write("DeviceReset", "Failed", "reset %u to %ux%u FAILED with %08X: a default pool resource is still alive; "
                "DeviceResetLog = 1 in [DEBUG] lists them\n", Resets, parameters ? parameters->BackBufferWidth : 0,
                parameters ? parameters->BackBufferHeight : 0, static_cast<uint32_t>(hr));
            return hr;
        }

        std::sort(alive.begin(), alive.end(), [](auto& a, auto& b) { return a.second.id < b.second.id; });
        FusionLog::Block log("DeviceReset", "Failed");
        log.Printf("reset %u to %ux%u FAILED with %08X: %zu default pool resources still alive, each must be released before a reset\n", Resets,
            parameters ? parameters->BackBufferWidth : 0, parameters ? parameters->BackBufferHeight : 0, static_cast<uint32_t>(hr), alive.size());
        auto now = GetTickCount64();
        for (auto& [resource, entry] : alive)
        {
            // The count of references the game and the plugins hold; 0 while only the device keeps it, bound
            resource->AddRef();
            auto references = resource->Release();
            std::string stack;
            for (auto address : entry.stack)
                if (address)
                    stack += " " + Describe(address);
            log.Printf("  #%" PRIu64 " %p %s, %u references, made %.1f s ago on thread %u by %s; stack:%s\n", entry.id, resource, entry.what.c_str(),
                references, (now - entry.made) / 1000.0, entry.thread, Describe(entry.caller).c_str(), stack.c_str());
        }
        return hr;
    }

    void Install(IDirect3DDevice9* device)
    {
        static void** hooked = nullptr;
        auto vtable = *reinterpret_cast<void***>(device);
        if (vtable == hooked)
            return;
        if (Modules.empty())
        {
            AddModule(GetModuleHandleW(nullptr), "GTAIV");
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Install), &self);
            AddModule(self, "FusionFix");
        }
        PatchVTable(vtable, 16, RealReset, &Reset);
        hooked = vtable;
        static const bool booking = [] { CIniReader iniReader(""); return iniReader.ReadInteger("DEBUG", "DeviceResetLog", 0) != 0; }();
        bBooking = booking;
        if (!bBooking)
            return;
        PatchVTable(vtable, 23, RealCreateTexture, &CreateTexture);
        PatchVTable(vtable, 24, RealCreateVolumeTexture, &CreateVolumeTexture);
        PatchVTable(vtable, 25, RealCreateCubeTexture, &CreateCubeTexture);
        PatchVTable(vtable, 26, RealCreateVertexBuffer, &CreateVertexBuffer);
        PatchVTable(vtable, 27, RealCreateIndexBuffer, &CreateIndexBuffer);
        PatchVTable(vtable, 28, RealCreateRenderTarget, &CreateRenderTarget);
        PatchVTable(vtable, 29, RealCreateDepthStencilSurface, &CreateDepthStencilSurface);
        PatchVTable(vtable, 36, RealCreateOffscreenPlainSurface, &CreateOffscreenPlainSurface);
    }
}

class DeviceResetFeature
{
public:
    DeviceResetFeature()
    {
        RageDirect3DDevice9::OnDeviceStored = &DeviceReset::Install;
    }
} DeviceResetFeature;
