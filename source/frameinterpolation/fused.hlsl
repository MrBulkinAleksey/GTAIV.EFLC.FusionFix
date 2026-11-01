// Derived from AMD FidelityFX SDK 2.3.0. See external/fidelityfx-src/LICENSE-AMD-FidelityFX-SDK.txt.
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_MOTION_VECTORS 0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DILATED_DEPTH 1
#define FFX_FRAMEINTERPOLATION_BIND_SRV_PREVIOUS_INTERPOLATION_SOURCE 2
#define FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE 3
#define FFX_FRAMEINTERPOLATION_BIND_SRV_DISTORTION_FIELD 4
#define FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_X 0
#define FFX_FRAMEINTERPOLATION_BIND_UAV_GAME_MOTION_VECTOR_FIELD_Y 1
#define FFX_FRAMEINTERPOLATION_BIND_UAV_RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME 2
#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION 0
#include "frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h"
#include "frameinterpolation/ffx_frameinterpolation_common.h"
#include "frameinterpolation/ffx_frameinterpolation_reconstruct_previous_depth.h"
#include "own_game_field.h"
[numthreads(8, 8, 1)]
void CS(FfxInt32x2 p : SV_DispatchThreadID)
{
    computeGameFieldMvs(p);
}
