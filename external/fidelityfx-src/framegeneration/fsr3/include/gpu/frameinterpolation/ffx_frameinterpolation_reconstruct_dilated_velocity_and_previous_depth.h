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

#ifndef FFX_FRAMEINTERPOLATION_RECONSTRUCT_DILATED_VELOCITY_AND_PREVIOUS_DEPTH_H
#define FFX_FRAMEINTERPOLATION_RECONSTRUCT_DILATED_VELOCITY_AND_PREVIOUS_DEPTH_H

// The depth update with a look first: the image is cleared to the farthest depth before the pass, inside the pass the
// cell only moves towards the nearest depth, by these atomics alone, and nothing reads it back here. A depth not
// nearer than what a plain load finds cannot change the cell's final value, so the atomic is taken only where the
// load saw something farther. The image ends as the extreme of the same set of values, whatever the order.
void UpdateReconstructedDepthPreviousFrameTested(FfxInt32x2 iPxSample, FfxFloat32 fDepth)
{
    FfxUInt32 uDepth = ffxAsUInt32(fDepth);
#if FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH
    if (rw_reconstructed_depth_previous_frame[iPxSample] < uDepth)
    {
        InterlockedMax(rw_reconstructed_depth_previous_frame[iPxSample], uDepth);
    }
#else
    if (rw_reconstructed_depth_previous_frame[iPxSample] > uDepth)
    {
        InterlockedMin(rw_reconstructed_depth_previous_frame[iPxSample], uDepth);
    }
#endif
}

void ReconstructPrevDepth(FfxInt32x2 iPxPos, FfxFloat32 fDepth, FfxFloat32x2 fMotionVector, FfxInt32x2 iPxDepthSize)
{
    fMotionVector *= FfxFloat32(length(fMotionVector * DisplaySize()) > 0.1f);

    FfxFloat32x2 fUv = (iPxPos + FfxFloat32(0.5)) / iPxDepthSize;
    FfxFloat32x2 fReprojectedUv = fUv + fMotionVector;
 
    BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fReprojectedUv, RenderSize());

    // Project current depth into previous frame locations.
    // Push to all pixels having some contribution if reprojection is using bilinear logic.
    for (FfxInt32 iSampleIndex = 0; iSampleIndex < 4; iSampleIndex++) {
        
        const FfxInt32x2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
        FfxFloat32 fWeight = bilinearInfo.fWeights[iSampleIndex];

        if (fWeight > fReconstructedDepthBilinearWeightThreshold) {

            FfxInt32x2 iStorePos = bilinearInfo.iBasePos + iOffset;
            if (IsOnScreen(iStorePos, iPxDepthSize)) {
                UpdateReconstructedDepthPreviousFrameTested(iStorePos, fDepth);
            }
        }
    }
}

void FindNearestDepth(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxInt32x2 iPxSize, FFX_PARAMETER_OUT FfxFloat32 fNearestDepth, FFX_PARAMETER_OUT FfxInt32x2 fNearestDepthCoord)
{
    const FfxInt32 iSampleCount = 9;
    const FfxInt32x2 iSampleOffsets[iSampleCount] = {
        FfxInt32x2(+0, +0),
        FfxInt32x2(+1, +0),
        FfxInt32x2(+0, +1),
        FfxInt32x2(+0, -1),
        FfxInt32x2(-1, +0),
        FfxInt32x2(-1, +1),
        FfxInt32x2(+1, +1),
        FfxInt32x2(-1, -1),
        FfxInt32x2(+1, -1),
    };

    // pull out the depth loads to allow SC to batch them
    FfxFloat32 depth[9];
    FfxInt32 iSampleIndex = 0;
    FFX_UNROLL
    for (iSampleIndex = 0; iSampleIndex < iSampleCount; ++iSampleIndex) {

        FfxInt32x2 iPos = iPxPos + iSampleOffsets[iSampleIndex];
        depth[iSampleIndex] = LoadInputDepth(iPos);
    }

    // find closest depth
    fNearestDepthCoord = iPxPos;
    fNearestDepth = depth[0];
    FFX_UNROLL
    for (iSampleIndex = 1; iSampleIndex < iSampleCount; ++iSampleIndex) {

        FfxInt32x2 iPos = iPxPos + iSampleOffsets[iSampleIndex];
        if (IsOnScreen(iPos, iPxSize)) {

            FfxFloat32 fNdDepth = depth[iSampleIndex];
#if FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH
            if (fNdDepth > fNearestDepth) {
#else
            if (fNdDepth < fNearestDepth) {
#endif
                fNearestDepthCoord = iPos;
                fNearestDepth = fNdDepth;
            }
        }
    }
}

void ReconstructAndDilate(FfxInt32x2 iPxLrPos)
{
    FfxFloat32 fDilatedDepth;
    FfxInt32x2 iNearestDepthCoord;

    FindNearestDepth(iPxLrPos, RenderSize(), fDilatedDepth, iNearestDepthCoord);

#if FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS
    FfxInt32x2 iSamplePos = iPxLrPos;
    FfxInt32x2 iMotionVectorPos = iNearestDepthCoord;
#else
    FfxInt32x2 iSamplePos = ComputeHrPosFromLrPos(iPxLrPos);
    FfxInt32x2 iMotionVectorPos = ComputeHrPosFromLrPos(iNearestDepthCoord);
#endif

    FfxFloat32x2 fDilatedMotionVector = LoadInputMotionVector(iMotionVectorPos);

    StoreDilatedDepth(iPxLrPos, fDilatedDepth);
    StoreDilatedMotionVectors(iPxLrPos, fDilatedMotionVector);

    ReconstructPrevDepth(iPxLrPos, fDilatedDepth, fDilatedMotionVector, RenderSize());
}


#endif //!defined( FFX_FRAMEINTERPOLATION_RECONSTRUCT_DILATED_VELOCITY_AND_PREVIOUS_DEPTH_H )
