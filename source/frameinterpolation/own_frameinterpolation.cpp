#include "own_frameinterpolation.h"
#include "../../external/fidelityfx-src/backend/dx12/ffx_dx12.h"
#include "../../external/fidelityfx-src/framegeneration/fsr3/include/ffx_frameinterpolation.h"
#include "../../external/fidelityfx-src/framegeneration/fsr3/include/ffx_opticalflow.h"
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

struct OwnFiContext {
    OwnFiCreateDescription desc{};
    FfxInterface shared{}, effects{}, rawEffects{};
    FfxErrorCode backendError = FFX_OK;
    std::vector<uint8_t> sharedScratch, effectScratch;
    FfxUInt32 sharedId = 0;
    FfxFrameInterpolationContext fi{};
    FfxOpticalflowContext of{};
    FfxResourceInternal resources[5]{};
    unsigned allocated = 0;
    bool sharedLive = false, fiLive = false, ofLive = false;
    bool prepared = false, history = false, failed = false;
    uint64_t previous = 0;
    OwnFiPrepareDescription prepare{};
};

// SDK paths sometimes ignore callback return values. Latch them across a public
// call, so a failed PSO or job execution never becomes successful generation.
static thread_local OwnFiContext* active = nullptr;
struct BackendCall {
    OwnFiContext* old;
    explicit BackendCall(OwnFiContext* c) : old(active) { active = c; c->backendError = FFX_OK; }
    ~BackendCall() { active = old; }
};
static FfxErrorCode checked(FfxErrorCode code) {
    if (code != FFX_OK && active->backendError == FFX_OK) active->backendError = code;
    return code;
}
static FfxErrorCode createBackend(FfxInterface* i, FfxEffect effect, FfxEffectBindlessConfig* config, FfxUInt32* id) {
    const auto code = checked(active->rawEffects.fpCreateBackendContext(i, effect, config, id));
    if (code == FFX_OK) {
        if (effect == FFX_EFFECT_FRAMEINTERPOLATION) active->fiLive = true;
        if (effect == FFX_EFFECT_OPTICALFLOW) active->ofLive = true;
    }
    return code;
}
static FfxErrorCode createPipeline(FfxInterface* i, FfxShaderBlob* b, const FfxPipelineDescription* d, FfxUInt32 id, FfxPipelineState* out) {
    return checked(active->rawEffects.fpCreatePipeline(i, b, d, id, out));
}
static FfxErrorCode schedule(FfxInterface* i, const FfxGpuJobDescription* job) {
    return checked(active->rawEffects.fpScheduleGpuJob(i, job));
}
static FfxErrorCode execute(FfxInterface* i, FfxCommandList list, FfxUInt32 id) {
    if (active->backendError != FFX_OK) return active->backendError;
    return checked(active->rawEffects.fpExecuteGpuJobs(i, list, id));
}

// Required by the two AMD effect objects' memory queries. No provider/loader is linked.
FfxErrorCode GetResourceSizeFromDescription(FfxDevice device, const FfxCreateResourceDescription* d, uint64_t* size, uint64_t* alignment)
{
    return ffxGetResourceSizeFromDescriptionDX12(device, d, size, alignment);
}

void OwnFiDestroy(OwnFiContext* c)
{
    if (!c) return;
    if (c->fiLive) ffxFrameInterpolationContextDestroy(&c->fi);
    if (c->ofLive) ffxOpticalflowContextDestroy(&c->of);
    for (unsigned i = 0; i < c->allocated; ++i)
        c->shared.fpDestroyResource(&c->shared, c->resources[i], c->sharedId);
    if (c->sharedLive) c->shared.fpDestroyBackendContext(&c->shared, c->sharedId);
    if (c->desc.device) c->desc.device->Release();
    delete c;
}

int32_t OwnFiCreate(const OwnFiCreateDescription* d, OwnFiContext** out)
{
    if (!out) return OwnFiInvalid;
    *out = nullptr;
    if (!d || d->abiVersion != 1 || !d->device ||
        (d->mode != OwnFiMode::Reference && d->mode != OwnFiMode::Fused) ||
        !d->maxRenderSize.width || !d->maxRenderSize.height || !d->displaySize.width || !d->displaySize.height ||
        (d->flags & ~uint32_t(1 | 2 | 8 | 16 | 32))) return OwnFiInvalid;
    auto* c = new (std::nothrow) OwnFiContext;
    if (!c) return OwnFiFailure;
    c->desc = *d; d->device->AddRef();
    auto fail = [&]() { OwnFiDestroy(c); return int32_t(OwnFiFailure); };
    try {
        c->sharedScratch.resize(ffxGetScratchMemorySizeDX12(1));
        c->effectScratch.resize(ffxGetScratchMemorySizeDX12(2));
    } catch (...) { return fail(); }
    BackendCall call(c);
    auto device = ffxGetDeviceDX12(d->device);
    if (ffxGetInterfaceDX12(&c->shared, device, c->sharedScratch.data(), c->sharedScratch.size(), 1) != FFX_OK ||
        ffxGetInterfaceDX12(&c->effects, device, c->effectScratch.data(), c->effectScratch.size(), 2) != FFX_OK) return fail();
    if (c->shared.fpCreateBackendContext(&c->shared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &c->sharedId) != FFX_OK) return fail();
    c->sharedLive = true;
    c->rawEffects = c->effects;
    c->effects.fpCreateBackendContext = createBackend;
    c->effects.fpCreatePipeline = createPipeline;
    c->effects.fpScheduleGpuJob = schedule;
    c->effects.fpExecuteGpuJobs = execute;
    FfxOpticalflowContextDescription of{};
    of.backendInterface = c->effects; of.resolution = d->displaySize;
    if (ffxOpticalflowContextCreate(&c->of, &of) != FFX_OK || c->backendError != FFX_OK) return fail();
    c->ofLive = true;
    FfxFrameInterpolationContextDescription fi{};
    fi.backendInterface = c->effects;
    fi.flags = d->flags | (d->mode == OwnFiMode::Fused ? (1u << 30) : 0);
    fi.maxRenderSize = d->maxRenderSize; fi.displaySize = d->displaySize;
    fi.backBufferFormat = d->backBufferFormat; fi.previousInterpolationSourceFormat = d->historyFormat;
    if (ffxFrameInterpolationContextCreate(&c->fi, &fi) != FFX_OK || c->backendError != FFX_OK) return fail();
    c->fiLive = true;
    FfxOpticalflowSharedResourceDescriptions ofr{};
    FfxFrameInterpolationSharedResourceDescriptions fir{};
    if (ffxOpticalflowGetSharedResourceDescriptions(&c->of, &ofr) != FFX_OK ||
        ffxFrameInterpolationGetSharedResourceDescriptions(&c->fi, &fir) != FFX_OK) return fail();
    const FfxCreateResourceDescription* r[] = {&ofr.opticalFlowVector, &ofr.opticalFlowSCD,
        &fir.dilatedDepth, &fir.dilatedMotionVectors, &fir.reconstructedPrevNearestDepth};
    for (unsigned i = 0; i < 5; ++i) {
        if (c->shared.fpCreateResource(&c->shared, r[i], c->sharedId, &c->resources[i]) != FFX_OK) return fail();
        ++c->allocated;
    }
    *out = c; return OwnFiOk;
}

static FfxApiResource resource(OwnFiContext* c, unsigned i)
{
    return c->shared.fpGetResource(&c->shared, c->resources[i]);
}
static bool extent(const FfxApiResource& r, FfxApiDimensions2D size)
{
    return r.resource && r.description.width >= size.width && r.description.height >= size.height;
}
int32_t OwnFiPrepare(OwnFiContext* c, ID3D12GraphicsCommandList* list, const OwnFiPrepareDescription* d)
{
    if (!c || !list || !d || c->failed || !d->renderSize.width || !d->renderSize.height ||
        d->renderSize.width > c->desc.maxRenderSize.width || d->renderSize.height > c->desc.maxRenderSize.height ||
        !extent(d->depth, d->renderSize) || !extent(d->motionVectors, (c->desc.flags & 16) ? c->desc.displaySize : d->renderSize) ||
        !(d->cameraNear > 0) || !(d->cameraFovAngleVertical > 0 && d->cameraFovAngleVertical < 3.141593f) ||
        !(d->viewSpaceToMetersFactor > 0) || !(d->frameTimeDelta > 0) || !std::isfinite(d->frameTimeDelta)) return OwnFiInvalid;
    // A replaced Prepare is an intentionally abandoned frame. Its commands must
    // already have retired; the next Generate only seeds fresh history.
    if (c->prepared || (c->history && (d->frameID != c->previous + 1 ||
        d->renderSize.width != c->prepare.renderSize.width || d->renderSize.height != c->prepare.renderSize.height)))
        c->history = false;
    BackendCall call(c);
    FfxFrameInterpolationPrepareDescription p{};
    p.commandList = ffxGetCommandListDX12(list); p.renderSize = d->renderSize;
    p.jitterOffset = d->jitter; p.motionVectorScale = d->motionVectorScale;
    p.frameTimeDelta = d->frameTimeDelta; p.cameraNear = d->cameraNear; p.cameraFar = d->cameraFar;
    p.viewSpaceToMetersFactor = d->viewSpaceToMetersFactor; p.cameraFovAngleVertical = d->cameraFovAngleVertical;
    p.depth = d->depth; p.motionVectors = d->motionVectors; p.frameID = d->frameID;
    p.dilatedDepth = resource(c, 2); p.dilatedMotionVectors = resource(c, 3); p.reconstructedPrevDepth = resource(c, 4);
    std::memcpy(&p.cameraPosition, d->cameraPosition, sizeof(d->cameraPosition));
    std::memcpy(&p.cameraUp, d->cameraUp, sizeof(d->cameraUp));
    std::memcpy(&p.cameraRight, d->cameraRight, sizeof(d->cameraRight));
    std::memcpy(&p.cameraForward, d->cameraForward, sizeof(d->cameraForward));
    if (ffxFrameInterpolationPrepare(&c->fi, &p) != FFX_OK || c->backendError != FFX_OK) { c->failed = true; return OwnFiFailure; }
    c->prepare = *d; c->prepared = true; return OwnFiOk;
}

int32_t OwnFiGenerate(OwnFiContext* c, ID3D12GraphicsCommandList* list, const OwnFiGenerateDescription* d)
{
    if (!c || !list || !d || c->failed || !c->prepared || d->frameID != c->prepare.frameID ||
        !extent(d->color, c->desc.displaySize) || !extent(d->output, c->desc.displaySize) ||
        (d->hudless.resource && !extent(d->hudless, c->desc.displaySize)) ||
        d->color.description.width != c->desc.displaySize.width || d->color.description.height != c->desc.displaySize.height ||
        d->output.description.width != c->desc.displaySize.width || d->output.description.height != c->desc.displaySize.height ||
        !(d->output.description.usage & FFX_API_RESOURCE_USAGE_UAV) ||
        d->color.resource == d->output.resource || d->hudless.resource == d->output.resource ||
        d->interpolationRect.left < 0 || d->interpolationRect.top < 0 ||
        d->interpolationRect.width < 0 || d->interpolationRect.height < 0 ||
        uint64_t(d->interpolationRect.left) + uint64_t(d->interpolationRect.width) > c->desc.displaySize.width ||
        uint64_t(d->interpolationRect.top) + uint64_t(d->interpolationRect.height) > c->desc.displaySize.height ||
        d->transferFunction < FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB ||
        d->transferFunction > FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB) return OwnFiInvalid;
    BackendCall call(c);
    auto& p = c->prepare;
    const bool reset = d->reset || !c->history || d->frameID != c->previous + 1;
    FfxOpticalflowDispatchDescription of{};
    of.commandList = ffxGetCommandListDX12(list); of.color = d->hudless.resource ? d->hudless : d->color;
    of.opticalFlowVector = resource(c, 0); of.opticalFlowSCD = resource(c, 1);
    of.reset = reset; of.backbufferTransferFunction = d->transferFunction;
    of.minMaxLuminance = {d->minMaxLuminance[0], d->minMaxLuminance[1]};
    if (ffxOpticalflowContextDispatch(&c->of, &of) != FFX_OK || c->backendError != FFX_OK) { c->failed = true; return OwnFiFailure; }
    FfxFrameInterpolationDispatchDescription fi{};
    fi.commandList = of.commandList; fi.displaySize = c->desc.displaySize; fi.renderSize = p.renderSize;
    fi.currentBackBuffer = d->color; fi.currentBackBuffer_HUDLess = d->hudless; fi.output = d->output;
    fi.interpolationRect = d->interpolationRect;
    fi.opticalFlowVector = resource(c, 0); fi.opticalFlowSceneChangeDetection = resource(c, 1);
    fi.opticalFlowBufferSize = {(c->desc.displaySize.width + 7) / 8, (c->desc.displaySize.height + 7) / 8};
    fi.opticalFlowBlockSize = 8;
    fi.opticalFlowScale = {1.f / c->desc.displaySize.width, 1.f / c->desc.displaySize.height};
    fi.cameraNear = p.cameraNear; fi.cameraFar = p.cameraFar; fi.cameraFovAngleVertical = p.cameraFovAngleVertical;
    fi.viewSpaceToMetersFactor = p.viewSpaceToMetersFactor; fi.frameTimeDelta = p.frameTimeDelta;
    fi.reset = reset; fi.backBufferTransferFunction = d->transferFunction;
    fi.minMaxLuminance[0] = d->minMaxLuminance[0]; fi.minMaxLuminance[1] = d->minMaxLuminance[1];
    fi.frameID = d->frameID; fi.dilatedDepth = resource(c, 2); fi.dilatedMotionVectors = resource(c, 3);
    fi.reconstructedPrevDepth = resource(c, 4);
    if (ffxFrameInterpolationDispatch(&c->fi, &fi) != FFX_OK || c->backendError != FFX_OK) { c->failed = true; return OwnFiFailure; }
    c->prepared = false; c->history = true; c->previous = d->frameID;
    return reset ? OwnFiNoFrame : OwnFiOk;
}
