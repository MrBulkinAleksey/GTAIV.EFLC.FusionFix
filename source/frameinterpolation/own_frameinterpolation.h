#pragma once
#include <d3d12.h>
#ifdef OWN_FI_BUILD
#include "../../external/fidelityfx-src/api/include/ffx_api_types.h"
#else
// Use the helper's existing API types; do not include a second SDK copy.
#include <ffx_api_types.h>
#endif

// ABI v1 is x64 only. No queue, fence, swapchain or presentation ownership.
// All calls on a context are serial. ALL preceding work (including a Prepare
// without Generate) must retire before the next Prepare or Destroy. Prepare
// replaces an abandoned prepared frame and invalidates history. Generate must
// match the latest prepared ID. A gap or resize also invalidates history.
// OwnFiNoFrame: submit/fence recorded history work, but do not present output.
// Resources carry their actual entry FfxApiResourceState; successful calls restore it.
// Rebind descriptor heaps/root signature after recording. Never submit a list after
// a negative return; discard it, retire prior submissions, destroy and use stock FG.
#ifdef OWN_FI_BUILD
#define OWN_FI_API extern "C" __declspec(dllexport)
#else
#define OWN_FI_API extern "C"
#endif
struct OwnFiContext;
enum OwnFiResult : int32_t { OwnFiOk = 0, OwnFiNoFrame = 1, OwnFiInvalid = -1, OwnFiFailure = -2 };
struct OwnFiCreateDescription {
    uint32_t abiVersion = 1;
    ID3D12Device* device = nullptr;
    FfxApiDimensions2D maxRenderSize{}, displaySize{};
    FfxApiSurfaceFormat backBufferFormat{}, historyFormat{};
    // Bits 0 depth-inverted, 1 infinite-depth, 3 HDR, 4 display-res MV, 5 jittered MV.
    uint32_t flags = 0;
};
struct OwnFiPrepareDescription {
    uint64_t frameID = 0;
    FfxApiDimensions2D renderSize{};
    FfxApiFloatCoords2D jitter{}, motionVectorScale{};
    float frameTimeDelta = 0, cameraNear = 0, cameraFar = 0;
    float viewSpaceToMetersFactor = 1, cameraFovAngleVertical = 0;
    float cameraPosition[3]{}, cameraUp[3]{}, cameraRight[3]{}, cameraForward[3]{};
    FfxApiResource depth{}, motionVectors{};
};
struct OwnFiGenerateDescription {
    uint64_t frameID = 0;
    FfxApiResource color{}, hudless{}, output{};
    FfxApiRect2D interpolationRect{};
    FfxApiBackbufferTransferFunction transferFunction{};
    float minMaxLuminance[2]{0, 1};
    uint32_t reset = 0;
};
OWN_FI_API int32_t OwnFiCreate(const OwnFiCreateDescription*, OwnFiContext**);
OWN_FI_API int32_t OwnFiPrepare(OwnFiContext*, ID3D12GraphicsCommandList*, const OwnFiPrepareDescription*);
OWN_FI_API int32_t OwnFiGenerate(OwnFiContext*, ID3D12GraphicsCommandList*, const OwnFiGenerateDescription*);
OWN_FI_API void OwnFiDestroy(OwnFiContext*);
