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

#ifndef FFX_FRAMEINTERPOLATION_GAME_MOTION_VECTOR_FIELD_H
#define FFX_FRAMEINTERPOLATION_GAME_MOTION_VECTOR_FIELD_H

FfxUInt32 getPriorityFactorFromViewSpaceDepth(FfxFloat32 fViewSpaceDepthInMeters)
{
    fViewSpaceDepthInMeters = ffxPow(fViewSpaceDepthInMeters, 0.33f);

    FfxUInt32 uPriorityFactor = FfxUInt32(FfxFloat32(1 - (fViewSpaceDepthInMeters * (1.0f / (1.0f + fViewSpaceDepthInMeters)))) * PRIORITY_HIGH_MAX);

    return ffxMax(1, uPriorityFactor);
}

void computeGameFieldMvs(FfxInt32x2 iPxPos)
{
    const FfxFloat32x2 fUvInScreenSpace            = (FfxFloat32x2(iPxPos) + 0.5f) / RenderSize();

    const FfxFloat32x2 fDistortionFieldUv          = SampleDistortionField(fUvInScreenSpace);
    FfxInt32x2 iDistortionPixelOffset              = FfxInt32x2(fDistortionFieldUv.xy * RenderSize());

    const FfxFloat32x2 fUvInInterpolationRectStart = FfxFloat32x2(InterpolationRectBase()) / DisplaySize();
    const FfxFloat32x2 fUvLetterBoxScale           = FfxFloat32x2(InterpolationRectSize()) / DisplaySize();
    const FfxFloat32x2 fUvInInterpolationRect      = fUvInInterpolationRectStart + fUvInScreenSpace * fUvLetterBoxScale;

    const FfxFloat32 fDepthSample = LoadDilatedDepth(iPxPos + iDistortionPixelOffset);
    const FfxFloat32x2 fGameMotionVector = LoadDilatedMotionVector(iPxPos + iDistortionPixelOffset);
    const FfxFloat32x2 fMotionVectorHalf = fGameMotionVector * 0.5f;
    const FfxFloat32x2 fInterpolatedLocationUv = fUvInScreenSpace + fMotionVectorHalf;

    const FfxFloat32 fViewSpaceDepth = ConvertFromDeviceDepthToViewSpace(fDepthSample);
    const FfxUInt32 uHighPriorityFactorPrimary = getPriorityFactorFromViewSpaceDepth(fViewSpaceDepth);

    // pixel position in current frame + Game Motion Vector -> pixel position in previous frame
    FfxFloat32x3 prevBackbufferCol = SamplePreviousBackbuffer(fUvInInterpolationRect+ fGameMotionVector * fUvLetterBoxScale).xyz; //returns color of current frame's pixel in previous frame buffer
    FfxFloat32x3 curBackbufferCol  = SampleCurrentBackbuffer(fUvInInterpolationRect).xyz; // returns color of current frame's pixel in current frame buffer
    FfxFloat32   prevLuma          = 0.001f + RawRGBToLuminance(prevBackbufferCol);
    FfxFloat32   currLuma          = 0.001f + RawRGBToLuminance(curBackbufferCol);

    FfxUInt32 uLowPriorityFactor = FfxUInt32(ffxRound(ffxPow(MinDividedByMax(prevLuma, currLuma), 1.0f / 1.0f) * PRIORITY_LOW_MAX))
    * FfxUInt32(IsUvInside(fUvInInterpolationRect + fGameMotionVector * fUvLetterBoxScale));

    // Update primary motion vectors
    {
        const FfxUInt32x2 packedVectorPrimary = PackVectorFieldEntries(true, uHighPriorityFactorPrimary, uLowPriorityFactor, fMotionVectorHalf);

        BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fInterpolatedLocationUv, RenderSize());
        for (FfxInt32 iSampleIndex = 0; iSampleIndex < 4; iSampleIndex++)
        {
            const FfxInt32x2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
            const FfxInt32x2 iSamplePos = bilinearInfo.iBasePos + iOffset;

            if (IsOnScreen(iSamplePos, RenderSize()))
            {
                // The depth splat of the reconstruct-previous-depth pass, done here on the values this pass computes
                // anyway (the same loads, the same four positions), so that pass's dispatch is not scheduled. With
                // the look first: the atomic is taken only where this depth is nearer than what the cell holds; the
                // cell only moves towards the nearest depth, so the image ends the same, and nothing reads it before
                // the disocclusion mask pass, three passes later.
                if (bilinearInfo.fWeights[iSampleIndex] > fReconstructedDepthBilinearWeightThreshold)
                {
                    const FfxUInt32 uDepthSample = ffxAsUInt32(fDepthSample);
#if FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH
                    if (rw_reconstructed_depth_interpolated_frame[iSamplePos] < uDepthSample)
                    {
                        InterlockedMax(rw_reconstructed_depth_interpolated_frame[iSamplePos], uDepthSample);
                    }
#else
                    if (rw_reconstructed_depth_interpolated_frame[iSamplePos] > uDepthSample)
                    {
                        InterlockedMin(rw_reconstructed_depth_interpolated_frame[iSamplePos], uDepthSample);
                    }
#endif
                }
                // A cell of the field only grows inside this pass: the setup zeroed it and every write of it is an
                // atomic maximum. A value no larger than what a plain load finds there cannot change the cell's final
                // content, so the atomic is taken only where the load saw something smaller. The field ends as the
                // maximum of the same set of values, whatever the order the threads ran in.
                if (rw_game_motion_vector_field_x[iSamplePos] < packedVectorPrimary.x)
                {
                    InterlockedMax(rw_game_motion_vector_field_x[iSamplePos], packedVectorPrimary.x);
                }
                if (rw_game_motion_vector_field_y[iSamplePos] < packedVectorPrimary.y)
                {
                    InterlockedMax(rw_game_motion_vector_field_y[iSamplePos], packedVectorPrimary.y);
                }
            }
        }
    }

    // Update secondary vectors
    // Main purpose of secondary vectors is to improve quality of inpainted vectors
    const FfxBoolean bWriteSecondaryVectors = length(fMotionVectorHalf * RenderSize()) > FFX_FRAMEINTERPOLATION_EPSILON;
    if (bWriteSecondaryVectors)
    {
        FfxBoolean bWriteSecondary = true;
        FfxUInt32 uNumPrimaryHits = 0;
        const FfxFloat32 fSecondaryStepScale = length(1.0f / RenderSize());
        const FfxFloat32x2 fStepMv = normalize(fGameMotionVector);
        const FfxFloat32 fBreakDist = ffxMin(length(fMotionVectorHalf), length(FfxFloat32x2(0.5f, 0.5f)));

        for (FfxFloat32 fMvScale = fSecondaryStepScale; fMvScale <= fBreakDist && bWriteSecondary; fMvScale += fSecondaryStepScale)
        {
            const FfxFloat32x2 fSecondaryLocationUv = fInterpolatedLocationUv - fStepMv * fMvScale;
            BilinearSamplingData bilinearInfo = GetBilinearSamplingData(fSecondaryLocationUv, RenderSize());

            // Reverse depth prio for secondary vectors
            FfxUInt32 uHighPriorityFactorSecondary = ffxMax(1, PRIORITY_HIGH_MAX - uHighPriorityFactorPrimary);

            const FfxFloat32x2 fToCenter = normalize(FfxFloat32x2(0.5f, 0.5f) - fSecondaryLocationUv);
            uLowPriorityFactor = FfxUInt32(ffxMax(0.0f, dot(fToCenter, fStepMv)) * PRIORITY_LOW_MAX);
            const FfxUInt32x2 packedVectorSecondary = PackVectorFieldEntries(false, uHighPriorityFactorSecondary, uLowPriorityFactor, fMotionVectorHalf);

            // Only write secondary mvs to single bilinear location
            for (FfxInt32 iSampleIndex = 0; iSampleIndex < 1; iSampleIndex++)
            {
                const FfxInt32x2 iOffset = bilinearInfo.iOffsets[iSampleIndex];
                const FfxInt32x2 iSamplePos = bilinearInfo.iBasePos + iOffset;

                bWriteSecondary = bWriteSecondary && IsOnScreen(iSamplePos, RenderSize());

                if (bWriteSecondary)
                {
                    // The Ex update with the same look first. "What was there" is the atomic's own answer where the
                    // atomic is taken and the load's where it is not; both are a look at a cell other threads are
                    // still writing, which is what the walk's count of primary hits always read.
                    FfxUInt32 uPreviousValueX = rw_game_motion_vector_field_x[iSamplePos];
                    if (uPreviousValueX < packedVectorSecondary.x)
                    {
                        InterlockedMax(rw_game_motion_vector_field_x[iSamplePos], packedVectorSecondary.x, uPreviousValueX);
                    }
                    FfxUInt32 uPreviousValueY = rw_game_motion_vector_field_y[iSamplePos];
                    if (uPreviousValueY < packedVectorSecondary.y)
                    {
                        InterlockedMax(rw_game_motion_vector_field_y[iSamplePos], packedVectorSecondary.y, uPreviousValueY);
                    }
                    const FfxUInt32 uExistingVectorFieldEntry = ffxMax(uPreviousValueX, uPreviousValueY);

                    uNumPrimaryHits += FfxUInt32(PackedVectorFieldEntryIsPrimary(uExistingVectorFieldEntry));
                    bWriteSecondary = bWriteSecondary && (uNumPrimaryHits <= 3);
                }
            }
        }
    }
}

#endif // FFX_FRAMEINTERPOLATION_GAME_MOTION_VECTOR_FIELD_H
