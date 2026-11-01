#include "../../source/helper/own_frameinterpolation.hpp"
#include <cstdio>
#include <cstddef>
int main() {
    HMODULE module = LoadLibraryW(L"GTAIV.EFLC.FusionFix.FrameInterpolation.dll");
    if (!module) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    const auto create = reinterpret_cast<decltype(&OwnFiCreate)>(GetProcAddress(module, "OwnFiCreate"));
    const auto prepare = reinterpret_cast<decltype(&OwnFiPrepare)>(GetProcAddress(module, "OwnFiPrepare"));
    const auto generate = reinterpret_cast<decltype(&OwnFiGenerate)>(GetProcAddress(module, "OwnFiGenerate"));
    const auto destroy = reinterpret_cast<decltype(&OwnFiDestroy)>(GetProcAddress(module, "OwnFiDestroy"));
    if (!create || !prepare || !generate || !destroy) return 2;
    OwnFiContext* context = reinterpret_cast<OwnFiContext*>(uintptr_t(1));
    OwnFiCreateDescription desc{};
    if (create(nullptr, &context) != OwnFiInvalid || context) return 3;
    if (create(&desc, &context) != OwnFiInvalid || context) return 4;
    if (prepare(nullptr, nullptr, nullptr) != OwnFiInvalid) return 5;
    if (generate(nullptr, nullptr, nullptr) != OwnFiInvalid) return 6;
    destroy(nullptr);
    OwnFrameInterpolation loader;
    if (loader.Create(desc) != OwnFiInvalid || loader) return 7;
    printf("DLL load/exports/invalid-input/loader passed; ABI v1 sizes=%zu,%zu,%zu\n", sizeof(desc),sizeof(OwnFiPrepareDescription),sizeof(OwnFiGenerateDescription));
    FreeLibrary(module);
    return 0;
}
