#pragma once
#include <windows.h>
#include "../frameinterpolation/own_frameinterpolation.h"

// Helper-only loader. The caller retires GPU work before Destroy or replacing a context.
// No signed runtime entry point, queue, swapchain or present path is replaced here.
class OwnFrameInterpolation {
    HMODULE module_ = nullptr;
    OwnFiContext* context_ = nullptr;
    decltype(&OwnFiCreate) create_ = nullptr;
    decltype(&OwnFiPrepare) prepare_ = nullptr;
    decltype(&OwnFiGenerate) generate_ = nullptr;
    decltype(&OwnFiDestroy) destroy_ = nullptr;
public:
    OwnFrameInterpolation() = default;
    OwnFrameInterpolation(const OwnFrameInterpolation&) = delete;
    OwnFrameInterpolation& operator=(const OwnFrameInterpolation&) = delete;
    ~OwnFrameInterpolation() { Destroy(); }
    int32_t Create(const OwnFiCreateDescription& desc) {
        Destroy();
        wchar_t path[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!n || n >= MAX_PATH) return OwnFiFailure;
        wchar_t* slash = wcsrchr(path, L'\\');
        constexpr wchar_t name[] = L"GTAIV.EFLC.FusionFix.FrameInterpolation.dll";
        if (!slash || size_t(slash - path + 1) + _countof(name) > MAX_PATH) return OwnFiFailure;
        wcscpy_s(slash + 1, MAX_PATH - size_t(slash - path + 1), name);
        module_ = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) return OwnFiFailure;
        create_ = reinterpret_cast<decltype(create_)>(GetProcAddress(module_, "OwnFiCreate"));
        prepare_ = reinterpret_cast<decltype(prepare_)>(GetProcAddress(module_, "OwnFiPrepare"));
        generate_ = reinterpret_cast<decltype(generate_)>(GetProcAddress(module_, "OwnFiGenerate"));
        destroy_ = reinterpret_cast<decltype(destroy_)>(GetProcAddress(module_, "OwnFiDestroy"));
        if (!create_ || !prepare_ || !generate_ || !destroy_) { Destroy(); return OwnFiFailure; }
        const int32_t result = create_(&desc, &context_);
        if (result != OwnFiOk) Destroy();
        return result;
    }
    int32_t Prepare(ID3D12GraphicsCommandList* list, const OwnFiPrepareDescription& desc) {
        return context_ ? prepare_(context_, list, &desc) : OwnFiInvalid;
    }
    int32_t Generate(ID3D12GraphicsCommandList* list, const OwnFiGenerateDescription& desc) {
        return context_ ? generate_(context_, list, &desc) : OwnFiInvalid;
    }
    void Destroy() {
        if (context_ && destroy_) destroy_(context_);
        context_ = nullptr;
        if (module_) FreeLibrary(module_);
        module_ = nullptr; create_ = nullptr; prepare_ = nullptr; generate_ = nullptr; destroy_ = nullptr;
    }
    explicit operator bool() const { return context_ != nullptr; }
};
