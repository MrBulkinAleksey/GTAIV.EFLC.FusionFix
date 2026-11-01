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

#ifndef FFX_OPTICALFLOW_COMPUTE_OPTICAL_FLOW_V5_H
#define FFX_OPTICALFLOW_COMPUTE_OPTICAL_FLOW_V5_H

#define CompareSize (4 * 2)
#define BlockSizeY 8
#define BlockSizeX 8
#define ThreadCount (4 * 16)
#define SearchRadiusX (8)
#define SearchRadiusY (8)
#define BlockCount 2

#define SearchBufferSizeX ((CompareSize + SearchRadiusX*2)/4)
#define SearchBufferSizeY  (CompareSize + SearchRadiusY*2)

FFX_GROUPSHARED FfxUInt32 pixels[CompareSize][CompareSize / 4];
FFX_GROUPSHARED FfxUInt32 searchBuffer[1][SearchBufferSizeY * SearchBufferSizeX];
#define bankBreaker 1
FFX_GROUPSHARED FfxUInt32 sadMapBuffer[4][SearchRadiusY * 2][(SearchRadiusX * 2) / 4 + bankBreaker];

#define MaxWaves 2
FFX_GROUPSHARED FfxUInt32 sWaveSad[MaxWaves];
FFX_GROUPSHARED FfxUInt32 sWaveMin[MaxWaves];

FfxUInt32 BlockSad64(FfxUInt32 blockSadSum, FfxInt32 iLocalIndex, FfxInt32 iLaneToBlockId, FfxInt32 block)
{
    if (iLaneToBlockId != block)
    {
        blockSadSum = 0u;
    }
    blockSadSum = ffxWaveSum(blockSadSum);

    if (ffxWaveLaneCount() == 32)
    {
        FfxInt32 waveId = iLocalIndex >> 5u;
        if (ffxWaveIsFirstLane())
        {
            sWaveSad[waveId] = blockSadSum;
        }
        FFX_GROUP_MEMORY_BARRIER;
        blockSadSum += sWaveSad[waveId ^ 1];
    }

    return blockSadSum;
}

FfxUInt32 SadMapMinReduction256(FfxInt32x2 iSearchId, FfxInt32 iLocalIndex)
{
    FfxUInt32 min01 = ffxMin(sadMapBuffer[0][iSearchId.y][iSearchId.x], sadMapBuffer[1][iSearchId.y][iSearchId.x]);
    FfxUInt32 min23 = ffxMin(sadMapBuffer[2][iSearchId.y][iSearchId.x], sadMapBuffer[3][iSearchId.y][iSearchId.x]);
    FfxUInt32 min0123 = ffxMin(min01, min23);
    min0123 = ffxWaveMin(min0123);

    if (ffxWaveLaneCount() == 32)
    {
        FfxInt32 waveId = iLocalIndex >> 5u;

        if (ffxWaveIsFirstLane())
        {
            sWaveMin[waveId] = min0123;
        }
        FFX_GROUP_MEMORY_BARRIER;
        min0123 = ffxMin(min0123, sWaveMin[waveId ^ 1]);
    }

    return min0123;
}

void LoadSearchBuffer(FfxInt32 iLocalIndex, FfxInt32x2 iPxPosShifted)
{
    FfxInt32 baseX = (iPxPosShifted.x - SearchRadiusX);
    FfxInt32 baseY = (iPxPosShifted.y - SearchRadiusY);

    for (FfxInt32 id = iLocalIndex; id < SearchBufferSizeX * SearchBufferSizeY; id += ThreadCount)
    {
        FfxInt32 idx = id % SearchBufferSizeX;
        FfxInt32 idy = id / SearchBufferSizeX;
        FfxInt32 x = baseX + idx * 4;
        FfxInt32 y = baseY + idy;
        searchBuffer[0][id] = LoadSecondImagePackedLuma(FfxInt32x2(x, y));
    }
    FFX_GROUP_MEMORY_BARRIER;
}

FfxUInt32x4 CalculateQSads2(FfxInt32x2 iSearchId)
{
    FfxUInt32x4 sad = ffxBroadcast4(0u);

#if FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION == 1

    FfxInt32 idx = iSearchId.y * 6 + iSearchId.x;

    sad = msad4(pixels[0][0], FfxUInt32x2(searchBuffer[0][idx],     searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[0][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[1][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[1][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[2][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[2][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[3][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[3][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[4][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[4][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[5][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[5][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[6][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[6][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);
    idx += 6;
    sad = msad4(pixels[7][0], FfxUInt32x2(searchBuffer[0][idx], searchBuffer[0][idx + 1]), sad);
    sad = msad4(pixels[7][1], FfxUInt32x2(searchBuffer[0][idx + 1], searchBuffer[0][idx + 2]), sad);

#else
    for (FfxInt32 dy = 0; dy < CompareSize; dy++)
    {
        FfxInt32 rowOffset = (iSearchId.y + dy) * SearchBufferSizeX;
        FfxUInt32 a0 = searchBuffer[0][rowOffset + iSearchId.x];
        FfxUInt32 a1 = searchBuffer[0][rowOffset + iSearchId.x + 1];
        FfxUInt32 a2 = searchBuffer[0][rowOffset + iSearchId.x + 2];
        sad += QSad(a0, a1, pixels[dy][0]);
        sad += QSad(a1, a2, pixels[dy][1]);
    }
#endif

    return sad;
}

FfxUInt32x2 abs_2(FfxInt32x2 val)
{
    FfxInt32x2 tmp = val;
    FfxInt32x2 mask = tmp >> 31;
    FfxUInt32x2 res = (tmp + mask) ^ mask;
    return res;
}

FfxUInt32 EncodeSearchCoord(FfxInt32x2 coord)
{
#if FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS == 1
    FfxUInt32x2 absCoord = FfxUInt32x2(abs_2(coord - 8));
    return FfxUInt32(absCoord.y << 12) | FfxUInt32(absCoord.x << 8) | FfxUInt32(coord.y << 4) | FfxUInt32(coord.x);
#else //FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS == 1
    return FfxUInt32(coord.y << 8) | FfxUInt32(coord.x);
#endif //FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS == 1
}

FfxInt32x2 DecodeSearchCoord(FfxUInt32 bits)
{
#if FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS == 1
    FfxInt32 dx = FfxInt32(bits & 0xfu) - SearchRadiusX;
    FfxInt32 dy = FfxInt32((bits >> 4) & 0xfu) - SearchRadiusY;

    return FfxInt32x2(dx, dy);
#else
    FfxInt32 dx = FfxInt32(bits & 0xffu) - SearchRadiusX;
    FfxInt32 dy = FfxInt32((bits >> 8) & 0xffu) - SearchRadiusY;

    return FfxInt32x2(dx, dy);
#endif
}

void PrepareSadMap(FfxInt32x2 iSearchId, FfxUInt32x4 qsad)
{
    sadMapBuffer[0][iSearchId.y][iSearchId.x] = (qsad.x << 16) | EncodeSearchCoord(FfxInt32x2(iSearchId.x * 4 + 0, iSearchId.y));
    sadMapBuffer[1][iSearchId.y][iSearchId.x] = (qsad.y << 16) | EncodeSearchCoord(FfxInt32x2(iSearchId.x * 4 + 1, iSearchId.y));
    sadMapBuffer[2][iSearchId.y][iSearchId.x] = (qsad.z << 16) | EncodeSearchCoord(FfxInt32x2(iSearchId.x * 4 + 2, iSearchId.y));
    sadMapBuffer[3][iSearchId.y][iSearchId.x] = (qsad.w << 16) | EncodeSearchCoord(FfxInt32x2(iSearchId.x * 4 + 3, iSearchId.y));
    FFX_GROUP_MEMORY_BARRIER;
}


uint ABfe(uint src, uint off, uint bits) { uint mask = (1u << bits) - 1u; return (src >> off) & mask; }
uint ABfi(uint src, uint ins, uint mask) { return (ins & mask) | (src & (~mask)); }
uint ABfiM(uint src, uint ins, uint bits) { uint mask = (1u << bits) - 1u; return (ins & mask) | (src & (~mask)); }
void MapThreads(in FfxInt32x2 iGroupId, in FfxInt32 iLocalIndex,
                out FfxInt32x2 iSearchId, out FfxInt32x2 iPxPos, out FfxInt32 iLaneToBlockId)
{
    iSearchId = FfxInt32x2(ABfe(iLocalIndex, 0u, 2u), ABfe(iLocalIndex, 2u, 4u));
    iLaneToBlockId = FfxInt32(ABfe(iLocalIndex, 1u, 1u) | (ABfe(iLocalIndex, 5u, 1u) << 1u));
    iPxPos = (iGroupId << 4u) + iSearchId * FfxInt32x2(4, 1);
}

#if FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION != 1
// THE SEARCH OF A GROUP'S FOUR BLOCKS AT ONCE, for the plain-arithmetic path. Thread i works for block i >> 4 and
// computes a whole row of the search, its row i & 15: sixteen sums, for which it takes apart 8 bytes of the block
// and 23 of the window a row of the block's 8, instead of four sums from 19 bytes each. Everything is whole-number
// arithmetic on AMD's own loads and words:
//   - the same four predictions, the same four windows, the same 64 words of the block, at the same positions;
//   - each of the 256 sums is the sum of the same 64 absolute differences;
//   - each is packed with its offset by AMD's EncodeSearchCoord, and the minimum of the 256 packed words is taken
//     (a minimum does not depend on the order);
//   - the fallback's sum, the test against it, the decoded vector and the cell it is stored in are AMD's.
// The 16 threads of a block lie in one wave when a wave is 32 or 64 lanes wide, so the two reductions (the smallest
// packed sum, and the block's own sum for the fallback) are wave reductions and nothing is exchanged between waves.
FFX_GROUPSHARED FfxUInt32 rowPixels[BlockCount * BlockCount][CompareSize][CompareSize / 4];
FFX_GROUPSHARED FfxUInt32 rowSearch[BlockCount * BlockCount][SearchBufferSizeY * SearchBufferSizeX];
// The pipelines' first static sampler, bound by its register alone (the name is not looked up): every optical flow
// pipeline is given point/clamp there. A gather of the integer luma returns the four texels of a 2 by 2 square in
// one fetch, so a pair of window rows costs two fetches instead of eight loads.
SamplerState s_OfRowGather : register(s0);

void ComputeOpticalFlowByRows(FfxInt32x2 iGroupId, FfxInt32 iLocalIndex)
{
    const FfxInt32   iBlock           = iLocalIndex >> 4;
    const FfxInt32   iRow             = iLocalIndex & 15;
    const FfxInt32x2 blockId          = FfxInt32x2(iBlock & 1, iBlock >> 1);
    const FfxInt32x2 ofGroupOffset    = iGroupId << 1u;
    const FfxInt32x2 pixelGroupOffset = iGroupId << 4u;

    if (IsSceneChanged())
    {
        if (iRow == 0)
        {
            StoreOpticalFlow(ofGroupOffset + blockId, FfxInt32x2(0, 0));
        }

        return;
    }

    const FfxBoolean bUsePredictionFromPreviousLevel = (OpticalFlowPyramidLevel() != OpticalFlowPyramidLevelCount() - 1);

    // this thread's word of its block (four pixels of the first image), and the fallback's differences on it
    const FfxInt32x2 iWordPos = pixelGroupOffset + blockId * 8 + FfxInt32x2((iRow & 1) * 4, iRow >> 1);
    const FfxUInt32  uFirst   = LoadFirstImagePackedLuma(iWordPos);
    rowPixels[iBlock][iRow >> 1][iRow & 1] = uFirst;
    const FfxUInt32  uOwnSad  = Sad(uFirst, LoadSecondImagePackedLuma(iWordPos));

    // the four predictions, and the four search windows: 4 times 144 words, loaded by the 64 threads. A word and
    // the word under it come from two gathers where the pair of rows and all four columns lie inside the picture;
    // AMD's own load, with its clamp and its fill of what lies outside, everywhere else. A gather at the corner the
    // four texels share returns exactly the texels (x, y + 1), (x + 1, y + 1), (x + 1, y), (x, y), and the words
    // are packed as AMD's GetPackedLuma packs them, the first texel in the low byte.
    FfxInt32x2 vectors[BlockCount * BlockCount];
    for (FfxInt32 k = 0; k < BlockCount * BlockCount; k++)
    {
        vectors[k] = LoadRwOpticalFlow(ofGroupOffset + FfxInt32x2(k & 1, k >> 1));
        if (!bUsePredictionFromPreviousLevel)
        {
            vectors[k] = FfxInt32x2(0, 0);
        }
    }
    const FfxInt32x2 iLumaSize = FfxInt32x2(DisplaySize().x >> OpticalFlowPyramidLevel(), DisplaySize().y >> OpticalFlowPyramidLevel());
    for (FfxInt32 id = iLocalIndex; id < BlockCount * BlockCount * SearchBufferSizeX * (SearchBufferSizeY / 2); id += ThreadCount)
    {
        const FfxInt32   k    = id / (SearchBufferSizeX * (SearchBufferSizeY / 2));
        const FfxInt32   h    = id % (SearchBufferSizeX * (SearchBufferSizeY / 2));
        const FfxInt32   w    = (h / SearchBufferSizeX) * 2 * SearchBufferSizeX + h % SearchBufferSizeX;
        const FfxInt32x2 base = pixelGroupOffset + FfxInt32x2(k & 1, k >> 1) * 8 + vectors[k] - FfxInt32x2(SearchRadiusX, SearchRadiusY);
        const FfxInt32x2 pos  = FfxInt32x2(base.x + (h % SearchBufferSizeX) * 4, base.y + (h / SearchBufferSizeX) * 2);
        if (pos.x >= 0 && pos.x <= iLumaSize.x - 4 && pos.y >= 0 && pos.y <= iLumaSize.y - 2)
        {
            const FfxUInt32x4 g0 = r_optical_flow_previous_input.GatherRed(s_OfRowGather, FfxFloat32x2(pos + FfxInt32x2(1, 1)) / FfxFloat32x2(iLumaSize));
            const FfxUInt32x4 g1 = r_optical_flow_previous_input.GatherRed(s_OfRowGather, FfxFloat32x2(pos + FfxInt32x2(3, 1)) / FfxFloat32x2(iLumaSize));
            rowSearch[k][w]                     = g0.w | (g0.z << 8) | (g1.w << 16) | (g1.z << 24);
            rowSearch[k][w + SearchBufferSizeX] = g0.x | (g0.y << 8) | (g1.x << 16) | (g1.y << 24);
        }
        else
        {
            rowSearch[k][w]                     = LoadSecondImagePackedLuma(pos);
            rowSearch[k][w + SearchBufferSizeX] = LoadSecondImagePackedLuma(pos + FfxInt32x2(0, 1));
        }
    }
    FFX_GROUP_MEMORY_BARRIER;

    // Sixteen sums of absolute differences: this thread's row of the search, every column. For whole numbers that
    // are not negative |a - b| = a + b - 2 min(a, b), so a sum over a block is the block's pixels' sum plus the
    // window's sum under it less twice the sum of the minima: a minimum and an addition a pair instead of a
    // subtraction, an absolute value and an addition. The sums are the same whole numbers (a sum is at most 64
    // times 255, nothing overflows), packed and compared as before. The window's sums for the sixteen columns are
    // sixteen sliding sums of 23 column totals.
    FfxUInt32 sads[SearchRadiusX * 2];
    FFX_UNROLL
    for (FfxInt32 x0 = 0; x0 < SearchRadiusX * 2; x0++)
    {
        sads[x0] = 0u;
    }
    FfxInt32 mins[SearchRadiusX * 2];
    FfxInt32 columns[SearchBufferSizeX * 4];
    FfxInt32 pixelSum = 0;
    FFX_UNROLL
    for (FfxInt32 x2 = 0; x2 < SearchRadiusX * 2; x2++)
    {
        mins[x2] = 0;
    }
    FFX_UNROLL
    for (FfxInt32 n2 = 0; n2 < SearchBufferSizeX * 4; n2++)
    {
        columns[n2] = 0;
    }
    for (FfxInt32 dy = 0; dy < CompareSize; dy++)
    {
        const FfxInt32 rowOffset = (iRow + dy) * SearchBufferSizeX;
        FfxInt32 s[SearchBufferSizeX * 4];
        FFX_UNROLL
        for (FfxInt32 n = 0; n < SearchBufferSizeX; n++)
        {
            const FfxUInt32 a = rowSearch[iBlock][rowOffset + n];
            s[n * 4 + 0] = FfxInt32((a >> 0) & 0xffu);
            s[n * 4 + 1] = FfxInt32((a >> 8) & 0xffu);
            s[n * 4 + 2] = FfxInt32((a >> 16) & 0xffu);
            s[n * 4 + 3] = FfxInt32((a >> 24) & 0xffu);
        }
        FfxInt32 p[CompareSize];
        FFX_UNROLL
        for (FfxInt32 m = 0; m < CompareSize / 4; m++)
        {
            const FfxUInt32 b = rowPixels[iBlock][dy][m];
            p[m * 4 + 0] = FfxInt32((b >> 0) & 0xffu);
            p[m * 4 + 1] = FfxInt32((b >> 8) & 0xffu);
            p[m * 4 + 2] = FfxInt32((b >> 16) & 0xffu);
            p[m * 4 + 3] = FfxInt32((b >> 24) & 0xffu);
        }
        FFX_UNROLL
        for (FfxInt32 j2 = 0; j2 < CompareSize; j2++)
        {
            pixelSum += p[j2];
        }
        FFX_UNROLL
        for (FfxInt32 n3 = 0; n3 < SearchRadiusX * 2 + CompareSize - 1; n3++)
        {
            columns[n3] += s[n3];
        }
        FFX_UNROLL
        for (FfxInt32 x = 0; x < SearchRadiusX * 2; x++)
        {
            FfxInt32 rowMin = 0;
            FFX_UNROLL
            for (FfxInt32 j = 0; j < CompareSize; j++)
            {
                rowMin += min(s[x + j], p[j]);
            }
            mins[x] += rowMin;
        }
    }
    FfxInt32 windowSum = 0;
    FFX_UNROLL
    for (FfxInt32 j3 = 0; j3 < CompareSize; j3++)
    {
        windowSum += columns[j3];
    }
    FFX_UNROLL
    for (FfxInt32 x3 = 0; x3 < SearchRadiusX * 2; x3++)
    {
        sads[x3] = FfxUInt32(windowSum + pixelSum - 2 * mins[x3]);
        if (x3 + 1 < SearchRadiusX * 2)
        {
            windowSum += columns[x3 + CompareSize] - columns[x3];
        }
    }

    // the smallest of this thread's sixteen packed sums (AMD's PrepareSadMap packs them the same way)
    FfxUInt32 uOwnMin = 0xFFFFFFFFu;
    FFX_UNROLL
    for (FfxInt32 x1 = 0; x1 < SearchRadiusX * 2; x1++)
    {
        uOwnMin = ffxMin(uOwnMin, (sads[x1] << 16) | EncodeSearchCoord(FfxInt32x2(x1, iRow)));
    }

    // over the block's sixteen threads, which share a wave: the smallest packed sum, and the block's own sum
    FfxUInt32 minSad      = 0xFFFFFFFFu;
    FfxUInt32 blockSadSum = 0u;
    for (FfxInt32 b0 = 0; b0 < BlockCount * BlockCount; b0++)
    {
        const FfxUInt32 uWaveMin = ffxWaveMin((iBlock == b0) ? uOwnMin : 0xFFFFFFFFu);
        const FfxUInt32 uWaveSad = ffxWaveSum((iBlock == b0) ? uOwnSad : 0u);
        if (iBlock == b0)
        {
            minSad      = uWaveMin;
            blockSadSum = uWaveSad;
        }
    }

    FfxInt32x2 newVector = vectors[iBlock] + DecodeSearchCoord(minSad);
#if FFX_LOCAL_SEARCH_FALLBACK == 1
    if (OpticalFlowPyramidLevel() == 0 && blockSadSum <= (minSad >> 16u))
    {
        newVector = FfxInt32x2(0, 0);
    }
#endif //FFX_LOCAL_SEARCH_FALLBACK

    if (iRow == 0)
    {
        StoreOpticalFlow(ofGroupOffset + blockId, newVector);
    }
}
#endif // FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION != 1

void ComputeOpticalFlowAdvanced(FfxInt32x2 iGlobalId, FfxInt32x2 iLocalId, FfxInt32x2 iGroupId, FfxInt32 iLocalIndex)
{
#if FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION != 1
    // The row path, where a block's sixteen threads share a wave; on a card whose waves are neither 32 nor 64
    // lanes wide AMD's own code below runs, untouched
    if (ffxWaveLaneCount() == 32 || ffxWaveLaneCount() == 64)
    {
        ComputeOpticalFlowByRows(iGroupId, iLocalIndex);
        return;
    }
#endif
    FfxInt32x2 iSearchId;
    FfxInt32x2 iPxPos;
    FfxInt32 iLaneToBlockId;
    MapThreads(iGroupId, iLocalIndex, iSearchId, iPxPos, iLaneToBlockId);

    FfxInt32x2 currentOFPos = iPxPos >> 3u;

    if (IsSceneChanged())
    {
        if ((iSearchId.y & 0x7) == 0 && (iSearchId.x & 0x1) == 0)
        {
            StoreOpticalFlow(currentOFPos, FfxInt32x2(0, 0));
        }

        return;
    }

    const FfxBoolean bUsePredictionFromPreviousLevel = (OpticalFlowPyramidLevel() != OpticalFlowPyramidLevelCount() - 1);

    FfxUInt32 packedLuma_4blocks = LoadFirstImagePackedLuma(iPxPos);

#if FFX_LOCAL_SEARCH_FALLBACK == 1
    FfxUInt32 prevPackedLuma_4blocks = LoadSecondImagePackedLuma(iPxPos);
    FfxUInt32 sad_4blocks = Sad(packedLuma_4blocks, prevPackedLuma_4blocks);
#endif //FFX_LOCAL_SEARCH_FALLBACK

    FfxInt32x2 ofGroupOffset = iGroupId << 1u;
    FfxInt32x2 pixelGroupOffset = iGroupId << 4u;

    FfxInt32x2 blockId;
    for (blockId.y = 0; blockId.y < BlockCount; blockId.y++)
    {
        for (blockId.x = 0; blockId.x < BlockCount; blockId.x++)
        {
            FfxInt32x2 currentVector = LoadRwOpticalFlow(ofGroupOffset + blockId);
            if (!bUsePredictionFromPreviousLevel)
            {
                currentVector = FfxInt32x2(0, 0);
            }

            if (iLaneToBlockId == blockId.y * 2 + blockId.x)
            {
                pixels[iSearchId.y & 0x7][iSearchId.x & 0x1] = packedLuma_4blocks;
            }

            LoadSearchBuffer(iLocalIndex, pixelGroupOffset + blockId * 8 + currentVector);

            FfxUInt32x4 qsad = CalculateQSads2(iSearchId);

            PrepareSadMap(iSearchId, qsad);
            FfxUInt32 minSad = SadMapMinReduction256(iSearchId, iLocalIndex);

            FfxInt32x2 minSadCoord = DecodeSearchCoord(minSad);
            FfxInt32x2 newVector = currentVector + minSadCoord;

#if FFX_LOCAL_SEARCH_FALLBACK == 1
            FfxUInt32 blockSadSum = BlockSad64(sad_4blocks, iLocalIndex, iLaneToBlockId, blockId.x + blockId.y * 2);
            if (OpticalFlowPyramidLevel() == 0 && blockSadSum <= (minSad >> 16u))
            {
                newVector = FfxInt32x2(0, 0);
            }
#endif //FFX_LOCAL_SEARCH_FALLBACK

            {
                StoreOpticalFlow(ofGroupOffset + blockId, newVector);
            }
        }
    }
}

#endif // FFX_OPTICALFLOW_COMPUTE_OPTICAL_FLOW_V5_H
