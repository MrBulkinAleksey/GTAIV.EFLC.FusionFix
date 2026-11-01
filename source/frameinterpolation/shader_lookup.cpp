#include "../../external/fidelityfx-src/framegeneration/fsr3/internal/ffx_frameinterpolation_shaderblobs.h"
#include "../../external/fidelityfx-src/framegeneration/fsr3/internal/ffx_opticalflow_shaderblobs.h"
#include "own_shader_blobs.h"
extern "C" FfxErrorCode frameInterpolationGetPermutationBlobByIndex(FfxFrameInterpolationPass pass, uint32_t flags, FfxShaderBlob* out)
{
    if (!out || unsigned(pass) >= 12 || flags >= 32) return FFX_ERROR_INVALID_ARGUMENT;
    *out = own_frameinterpolation[unsigned(pass)][flags];
    return FFX_OK;
}
extern "C" FfxErrorCode opticalflowGetPermutationBlobByIndex(FfxOpticalflowPass pass, uint32_t flags, FfxShaderBlob* out)
{
    if (!out || unsigned(pass) >= 7 || flags >= 4) return FFX_ERROR_INVALID_ARGUMENT;
    *out = own_opticalflow[unsigned(pass)][flags];
    return FFX_OK;
}
extern "C" FfxErrorCode frameInterpolationIsWave64(uint32_t flags, bool& out) { out = (flags & 8) != 0; return FFX_OK; }
extern "C" FfxErrorCode opticalflowIsWave64(uint32_t flags, bool& out) { out = (flags & 1) != 0; return FFX_OK; }
