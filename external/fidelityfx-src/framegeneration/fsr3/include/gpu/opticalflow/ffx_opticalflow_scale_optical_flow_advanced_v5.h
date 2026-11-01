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

#ifndef FFX_OPTICALFLOW_SCALE_OPTICAL_FLOW_ADVANCED_V5_H
#define FFX_OPTICALFLOW_SCALE_OPTICAL_FLOW_ADVANCED_V5_H

#define WG_WIDTH FFX_OPTICALFLOW_THREAD_GROUP_WIDTH
#define WG_HEIGHT FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT
#define WG_DEPTH FFX_OPTICALFLOW_THREAD_GROUP_DEPTH
FFX_GROUPSHARED FfxInt32x2 nearestVectors[4][WG_HEIGHT][WG_WIDTH];
FFX_GROUPSHARED FfxUInt32 localRegion[4][WG_HEIGHT][WG_WIDTH];
FFX_GROUPSHARED FfxUInt32 sads[4][WG_HEIGHT][WG_WIDTH];

// The filtered vector of a position, computed here from the search's own result instead of read from the filtered
// image: the filter pass's arithmetic (each vector becomes the one of its 3 by 3 neighbourhood that lies nearest to
// the other eight), word for word, with the position handed in. The image the scale pass reads is bound to the
// search's result for the levels from 1 to 6, whose filter dispatch is then not scheduled: a level's filtered image
// has exactly this one reader. A position outside the image is zero, as a load of the filtered image outside itself
// was; the search's image is the same size as the filtered one was.
FfxInt32x2 FilteredOpticalFlow(FfxInt32x2 iPos)
{
    FfxUInt32 uWidth;
    FfxUInt32 uHeight;
    r_optical_flow.GetDimensions(uWidth, uHeight);
    if (FfxUInt32(iPos.x) >= uWidth || FfxUInt32(iPos.y) >= uHeight)
    {
        return FfxInt32x2(0, 0);
    }

    FfxInt32x2 tmpMV[9];
    FfxInt32 idx = 0;
    for (FfxInt32 xx = -1; xx < 2; xx++)
    {
        for (FfxInt32 yy = -1; yy < 2; yy++)
        {

            tmpMV[idx] = LoadOpticalFlow(iPos + FfxInt32x2(xx, yy));
            idx++;
        }
    }

    FfxUInt32  ret = 0xFFFFFFFF;
    for (FfxInt32 i = 0; i < 9; ++i)
    {
        FfxUInt32 tmp = 0;
        for (FfxInt32 j = 0; j < 9; ++j)
        {
            FfxInt32x2 delta = tmpMV[i] - tmpMV[j];
            tmp = delta.x * delta.x + (delta.y * delta.y + tmp);
        }

        ret = min(((tmp) << 4) | i, ret);
    }

    FfxUInt32 minIdx = ret & 0xF;

    return tmpMV[minIdx];
}

void ScaleOpticalFlowAdvanced(FfxInt32x3 iGlobalId, FfxInt32x3 iLocalId)
{
    if (IsSceneChanged())
    {
        StoreOpticalFlowNextLevel(iGlobalId.xy, FfxInt32x2(0, 0));

        return;
    }

    int xOffset = (iLocalId.z % 2) - 1 + iGlobalId.x % 2;
    int yOffset = (iLocalId.z / 2) - 1 + iGlobalId.y % 2;

    FfxInt32x2 srcOFPos = FfxInt32x2(
        (iGlobalId.x / 2) + xOffset,
        (iGlobalId.y / 2) + yOffset
    );

    FfxInt32x2 nearestVector = FilteredOpticalFlow(srcOFPos);
    nearestVectors[iLocalId.z][iLocalId.y][iLocalId.x] = nearestVector * 2;

    int maxY = 4;
    for (int n = iLocalId.z; n < maxY; n += WG_DEPTH)
    {
        {
            FfxInt32x2 lumaPos = FfxInt32x2((iGlobalId.x) * 4, iGlobalId.y * maxY + n);
            FfxUInt32 firstPixel = LoadFirstImagePackedLuma(lumaPos);
            localRegion[n][iLocalId.y][iLocalId.x] = firstPixel;
        }
    }
    FFX_GROUP_MEMORY_BARRIER;

    uint sad = 0;
    for (int n = 0; n < maxY; n++)
    {
        {
            FfxInt32x2 lumaPos = FfxInt32x2((iGlobalId.x) * 4, (iGlobalId.y * maxY + n)) + nearestVector;
            FfxUInt32 secondPixel = LoadSecondImagePackedLuma(lumaPos);
            sad += Sad(localRegion[n][iLocalId.y][iLocalId.x], secondPixel);
        }
    }
    sads[iLocalId.z][iLocalId.y][iLocalId.x] = sad;

    FFX_GROUP_MEMORY_BARRIER;

    {
        if (iLocalId.z == 0)
        {
            uint bestSad = 0xffffffff;
            uint bestId = 0;

            for (int n = 0; n < 4; n++)
            {
                if ((sads[n][iLocalId.y][iLocalId.x]) < bestSad)
                {
                    bestSad = sads[n][iLocalId.y][iLocalId.x];
                    bestId = n;
                }
            }

            FfxInt32x2 outputVector = nearestVectors[bestId][iLocalId.y][iLocalId.x];

            StoreOpticalFlowNextLevel(iGlobalId.xy, outputVector);
        }
    }
}

#endif // FFX_OPTICALFLOW_SCALE_OPTICAL_FLOW_ADVANCED_V5_H
