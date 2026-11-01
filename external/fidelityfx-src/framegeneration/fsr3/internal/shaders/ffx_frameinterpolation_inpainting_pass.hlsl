// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#define FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW_SCENE_CHANGE_DETECTION     0
#define FFX_FRAMEINTERPOLATION_BIND_SRV_INPAINTING_PYRAMID                      1
#define FFX_FRAMEINTERPOLATION_BIND_SRV_PRESENT_BACKBUFFER                      2
#define FFX_FRAMEINTERPOLATION_BIND_SRV_CURRENT_INTERPOLATION_SOURCE            3

#define FFX_FRAMEINTERPOLATION_BIND_UAV_OUTPUT                                  0
#define FFX_FRAMEINTERPOLATION_BIND_UAV_INPAINTING_MASK                         1
// The image kept for the next pair's interpolation, written by this pass where the dispatch says so
#define FFX_FRAMEINTERPOLATION_BIND_UAV_PREVIOUS_INTERPOLATION_SOURCE           2

#define FFX_FRAMEINTERPOLATION_BIND_CB_FRAMEINTERPOLATION                       0

#include "frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h"
#include "frameinterpolation/ffx_frameinterpolation_common.h"
#include "frameinterpolation/ffx_frameinterpolation_inpainting.h"

#ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH
#define FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH 8
#endif // #ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH
#ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT
#define FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT 8
#endif // #ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT
#ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_DEPTH
#define FFX_FRAMEINTERPOLATION_THREAD_GROUP_DEPTH 1
#endif // #ifndef FFX_FRAMEINTERPOLATION_THREAD_GROUP_DEPTH
#ifndef FFX_FRAMEINTERPOLATION_NUM_THREADS
#define FFX_FRAMEINTERPOLATION_NUM_THREADS [numthreads(FFX_FRAMEINTERPOLATION_THREAD_GROUP_WIDTH, FFX_FRAMEINTERPOLATION_THREAD_GROUP_HEIGHT, FFX_FRAMEINTERPOLATION_THREAD_GROUP_DEPTH)]
#endif // #ifndef FFX_FRAMEINTERPOLATION_NUM_THREADS

FFX_FRAMEINTERPOLATION_NUM_THREADS
FFX_FRAMEINTERPOLATION_EMBED_ROOTSIG_CONTENT
void CS(FfxInt32x2 iPxPos : SV_DispatchThreadID)
{
    // Where the dispatch says so (bit 30 of the dispatch flags, set by the C++ side of this pass and by nothing
    // else: a HUD-less image of 16-bit floats and no debug drawing), the HUD-less pixel is stored for the next
    // pair here, pixel by pixel, which the whole-picture copy at the dispatch's end did, and AMD's function is
    // called only for a pixel it can change: one whose weight asks for inpainting or where the two pictures
    // differ. For every other pixel its function loads three values and stores nothing.
    if ((GetDispatchFlags() & 0x40000000u) != 0)
    {
        const FfxFloat32x4 fSource = r_current_interpolation_source[iPxPos];
        rw_previous_interpolation_source[iPxPos] = fSource;
        if (rw_inpainting_mask[iPxPos] > FFX_FRAMEINTERPOLATION_EPSILON
            || any(FFX_GREATER_THAN(abs(fSource.rgb - LoadPresentBackbuffer(iPxPos).rgb), FfxFloat32x3(0.0, 0.0, 0.0))))
        {
            computeInpainting(iPxPos);
        }
        return;
    }
    // With a HUD-less image attached or a debug drawing asked for, AMD's function may change any pixel: one
    // thread a pixel, as AMD dispatches it (the C++ side decides by the same two values)
    if (GetHUDLessAttachedFactor() == 1
        || (GetDispatchFlags() & (FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_RESET_INDICATORS)) != 0)
    {
        computeInpainting(iPxPos);
        return;
    }
    // Otherwise this thread's 4 by 4 pixels, a sixteenth of the groups, and AMD's function for a pixel whose
    // weight asks for inpainting: for every other pixel it loads two values and stores nothing, so not calling
    // it changes nothing. A pixel beyond what AMD's dispatch covered is never touched, as before.
    const FfxInt32x2 iCovered = ((DisplaySize() + 7) / 8) * 8;      // what AMD's dispatch covered: one group for 8 by 8 pixels
    for (FfxInt32 iBlockY = 0; iBlockY < 4; iBlockY++)
    {
        for (FfxInt32 iBlockX = 0; iBlockX < 4; iBlockX++)
        {
            const FfxInt32x2 iPx = iPxPos * 4 + FfxInt32x2(iBlockX, iBlockY);
            if (iPx.x < iCovered.x && iPx.y < iCovered.y)
            {
                if (rw_inpainting_mask[iPx] > FFX_FRAMEINTERPOLATION_EPSILON)
                {
                    computeInpainting(iPx);
                }
            }
        }
    }
}
