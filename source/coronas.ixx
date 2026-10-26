module;

#include <common.hxx>
#include "FusionLog.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

export module coronas;

import common;
import settings;

static constexpr auto NewLimitExponent = 14;

void IncreaseCoronaLimit()
{
    using namespace injector;

    auto nCoronasLimit = static_cast<uint32_t>(3 * pow(2.0, NewLimitExponent)); // 49152, default 3 * pow(2, 8) = 768

    static std::vector<uint32_t> aCoronas;
    static std::vector<uint32_t> aCoronas2;
    aCoronas.resize(nCoronasLimit * 0x3C * 4);
    aCoronas2.resize(nCoronasLimit * 0x3C * 4);

    int32_t counter1 = 0;
    int32_t counter2 = 0;

    uintptr_t range_start = (uintptr_t)hook::get_pattern("33 C0 C7 80 ? ? ? ? ? ? ? ? 83 C0 40 3D ? ? ? ? 72 EC C7 05 ? ? ? ? ? ? ? ? C3");
    uintptr_t range_end = (uintptr_t)hook::get_pattern("5E C3 FF 05 ? ? ? ? 5E C3");

    uintptr_t dword_temp = (uintptr_t)*hook::pattern("89 82 ? ? ? ? F3 0F 11 82 ? ? ? ? F3 0F 10 44 24 ? F3 0F 11 8A ? ? ? ? 8B 41 0C 0F B6 4C 24 ? 89 82").get(0).get<uint32_t*>(2);

    for (size_t i = dword_temp; i <= (dword_temp + 0x3C); i++)
    {
        auto GoThroughPatterns = [&](const char* pattern_str, int32_t pos) -> void
        {
            auto patternl = hook::range_pattern(range_start, range_end, pattern_str);
            for (size_t j = 0; j < patternl.size(); j++)
            {
                if (*patternl.get(j).get<uintptr_t>(pos) == i)
                {
                    AdjustPointer(patternl.get(j).get<uint32_t>(pos), &aCoronas[0], dword_temp, dword_temp + 0x3C);
                    counter1++;
                }
            }
        };

        GoThroughPatterns("83 8A", 2);
        GoThroughPatterns("88 82", 2);
        GoThroughPatterns("89 82", 2);
        GoThroughPatterns("89 82", 2);
        GoThroughPatterns("89 8A", 2);
        GoThroughPatterns("8B 82", 2);
        GoThroughPatterns("BE", 1);
        GoThroughPatterns("C7 80", 2);
        GoThroughPatterns("C7 82", 2);
        GoThroughPatterns("F3 0F 11 82", 4);
        GoThroughPatterns("F3 0F 11 8A", 4);
    }

    range_start = (uintptr_t)hook::get_pattern("8B 44 24 1C F3 0F 10 4C 24 ? F3 0F 10 46 ? F3 0F 59 05 ? ? ? ? 8D 0C 40");
    range_end = (uintptr_t)hook::get_pattern("0F 29 85 ? ? ? ? 0F 28 9D ? ? ? ? F3 0F 5C C2 F3 0F 11 5C 24 ? 0F 28 9D ? ? ? ? F3 0F 11 5C 24 ? 0F 28 D0");

    dword_temp = (uintptr_t)*hook::pattern("F3 0F 11 89 ? ? ? ? F3 0F 10 4C 24 ? F3 0F 11 89").get(0).get<uint32_t*>(4);

    for (size_t i = dword_temp; i <= (dword_temp + 0x1B); i++)
    {
        auto GoThroughPatterns = [&](const char* pattern_str, int32_t pos) -> void
        {
            auto patternl = hook::range_pattern(range_start, range_end, pattern_str);
            for (size_t j = 0; j < patternl.size(); j++)
            {
                if (*patternl.get(j).get<uintptr_t>(pos) == i)
                {
                    AdjustPointer(patternl.get(j).get<uint32_t>(pos), &aCoronas2[0], dword_temp, dword_temp + 0x1B);
                    counter2++;
                }
            }
        };

        GoThroughPatterns("0F 28 81", 3);
        GoThroughPatterns("0F B6 81", 3);
        GoThroughPatterns("0F B6 B1", 3);
        GoThroughPatterns("80 B8", 2);
        GoThroughPatterns("88 81", 2);
        GoThroughPatterns("88 91", 2);
        GoThroughPatterns("89 81", 2);
        GoThroughPatterns("BE", 1);
        GoThroughPatterns("F3 0F 10 81", 4);
        GoThroughPatterns("F3 0F 10 89", 4);
        GoThroughPatterns("F3 0F 11 81", 4);
        GoThroughPatterns("F3 0F 11 89", 4);
        GoThroughPatterns("F3 0F 11 91", 4);
    }

    if (counter1 != 24 || counter2 != 18)
        MessageBoxW(0, L"Fusion Fix", L"Fusion Fix is not fully compatible with this version of the game", 0);

    auto p = hook::pattern("BE ? ? ? ? BF FF 02 00 00 8D 64 24 00");
    AdjustPointer(p.get_first(1), &aCoronas[0], dword_temp, dword_temp + 0x3C);
    p = hook::pattern("BE ? ? ? ? BF FF 05 00 00 8D 64 24 00");
    AdjustPointer(p.get_first(1), &aCoronas2[0], dword_temp, dword_temp + 0x1B);

    auto pattern = hook::pattern("C1 E1 ? 03 4C 24 18 C1");
    WriteMemory<uint8_t>(pattern.get(0).get<uintptr_t>(2), NewLimitExponent, true);
    pattern = hook::pattern("C1 E1 ? 03 CF C1 E1");
    WriteMemory<uint8_t>(pattern.get(0).get<uintptr_t>(2), NewLimitExponent, true);
    pattern = hook::pattern("C1 E0 ? 03 C2 C1 E0 05 80 B8");
    WriteMemory<uint8_t>(pattern.get(0).get<uintptr_t>(2), NewLimitExponent, true);

    pattern = hook::pattern("81 FE ? ? ? ? 0F 8D ? ? ? ? 8B 44 24 08 8B 4C 24 1C F3 0F 10 44 24 ? C1 E2 06");
    WriteMemory<uint32_t>(pattern.get(0).get<uintptr_t>(2), nCoronasLimit, true);
    pattern = hook::pattern("3D ? ? ? ? 0F 8D ? ? ? ? 8B 44 24 1C F3 0F 10 4C 24");
    WriteMemory<uint32_t>(pattern.get(0).get<uintptr_t>(1), nCoronasLimit, true);
    pattern = hook::pattern("3D ? ? ? ? 72 EC C7 05 ? ? ? ? ? ? ? ? C3");
    WriteMemory<uint32_t>(pattern.get(0).get<uintptr_t>(1), nCoronasLimit * 64, true);
}

void IncreaseCoronaLimitEFLC()
{
    using namespace injector;

    auto nCoronasLimit = static_cast<uint32_t>(3 * pow(2.0, NewLimitExponent)); // 49152, default 3 * pow(2, 8) = 768

    static std::vector<uint32_t> aCoronas;
    static std::vector<uint32_t> aCoronas2;
    aCoronas.resize(nCoronasLimit * 0x3C * 4);
    aCoronas2.resize(nCoronasLimit * 0x3C * 4);

    int32_t counter1 = 0;
    int32_t counter2 = 0;

    uintptr_t range_start = (uintptr_t)hook::get_pattern("BE ? ? ? ? 83 7E 18 00");
    uintptr_t range_end = (uintptr_t)hook::get_pattern("5E C3 83 05 ? ? ? ? ? 5E C3");

    uintptr_t dword_temp = (uintptr_t)*hook::pattern("D9 98 ? ? ? ? F3 0F 10 44 24 20 F3 0F 11 88").get(0).get<uint32_t*>(2);

    for (size_t i = dword_temp; i <= (dword_temp + 0x3C); i++)
    {
        auto GoThroughPatterns = [&](const char* pattern_str, int32_t pos) -> void
        {
            auto patternl = hook::range_pattern(range_start, range_end, pattern_str);
            for (size_t j = 0; j < patternl.size(); j++)
            {
                if (*patternl.get(j).get<uintptr_t>(pos) == i)
                {
                    AdjustPointer(patternl.get(j).get<uint32_t>(pos), &aCoronas[0], dword_temp, dword_temp + 0x3C);
                    counter1++;
                }
            }
        };

        GoThroughPatterns("D9 98", 2);
        GoThroughPatterns("F3 0F 11 80", 4);
        GoThroughPatterns("F3 0F 11 88", 4);
        GoThroughPatterns("83 88 ? ? ? ? 10", 2);
        GoThroughPatterns("83 88 ? ? ? ? 06", 2);
        GoThroughPatterns("BE", 1);
        GoThroughPatterns("89 88", 2);
        GoThroughPatterns("88 90", 2);
        GoThroughPatterns("88 88", 2);
        GoThroughPatterns("8B 90", 2);
    }

    range_start = (uintptr_t)hook::get_pattern("D9 46 20 F3 0F 10 54 24 ? 0F B6 56 30"); //0x7E10AB
    range_end = (uintptr_t)hook::get_pattern("F3 0F 59 C3 F3 0F 5C F0 F3 0F 59 CB"); //0x7E1671

    dword_temp = (uintptr_t)*hook::pattern("F3 0F 11 90 ? ? ? ? F3 0F 10 54 24 24 F3 0F").get(0).get<uint32_t*>(4);

    for (size_t i = dword_temp; i <= (dword_temp + 0x1B); i++)
    {
        auto GoThroughPatterns = [&](const char* pattern_str, int32_t pos) -> void
        {
            auto patternl = hook::range_pattern(range_start, range_end, pattern_str);
            for (size_t j = 0; j < patternl.size(); j++)
            {
                if (*patternl.get(j).get<uintptr_t>(pos) == i)
                {
                    AdjustPointer(patternl.get(j).get<uint32_t>(pos), &aCoronas2[0], dword_temp, dword_temp + 0x1B);
                    counter2++;
                }
            }
        };

        GoThroughPatterns("0F 28 89", 3);
        GoThroughPatterns("88 90", 2);
        GoThroughPatterns("88 88", 2);
        GoThroughPatterns("F3 0F 11 90", 4);
        GoThroughPatterns("F3 0F 11 80", 4);
        GoThroughPatterns("F3 0F 10 81", 4);
        GoThroughPatterns("F3 0F 11 88", 4);
        GoThroughPatterns("F3 0F 10 A1", 4);
        GoThroughPatterns("F3 0F 10 99", 4);
        GoThroughPatterns("D9 98", 2);
        GoThroughPatterns("80 B9 ? ? ? ? 00", 2);
        GoThroughPatterns("0F B6 B1", 3);
        GoThroughPatterns("0F B6 81", 3);
        GoThroughPatterns("0F B6 91", 3);
    }

    if (counter1 != 24 || counter2 != 18)
        MessageBoxW(0, L"Fusion Fix", L"Fusion Fix is not fully compatible with this version of the game", 0);

    auto pattern = hook::pattern("C1 E0 ? 03 C3 C1 E0");
    WriteMemory<uint8_t>(pattern.get(0).get<uintptr_t>(2), NewLimitExponent, true);
    pattern = hook::pattern("C1 E1 ? 03 CF C1 E1");
    WriteMemory<uint8_t>(pattern.get(0).get<uintptr_t>(2), NewLimitExponent, true);
    pattern = hook::pattern("C1 E1 ? 03 C8 C1 E1");
    WriteMemory<uint8_t>(pattern.get(0).get<uintptr_t>(2), NewLimitExponent, true);

    pattern = hook::pattern("81 FE ? ? ? ? 0F 8D ? ? ? ? 8B 4C 24 08 8A 54 24 0C");
    WriteMemory<uint32_t>(pattern.get(0).get<uintptr_t>(2), nCoronasLimit, true);
    pattern = hook::pattern("81 FF ? ? ? ? 0F 8D ? ? ? ? 8B 44 24 1C D9 46 20");
    WriteMemory<uint32_t>(pattern.get(0).get<uintptr_t>(2), nCoronasLimit, true);
    pattern = hook::pattern("3D ? ? ? ? 72 ? 89 0D ? ? ? ? C3");
    WriteMemory<uint32_t>(pattern.get(0).get<uintptr_t>(1), nCoronasLimit * 64, true);
}

// The coronas' quads (CE 0xc1f170): for each corona of the frame's list the game made six calls to its vertex function
// (CE 0x4462e0, 0x24 bytes a vertex: the corner's position, the corona's centre in the normal, the colour with one
// corner in its alpha, u the other, v the brightness), about 7% of the render thread in a profile. The same quads are
// written here straight into the buffer the game's Begin locked, and with CoronaRenderer = 1 those wholly out of view
// (behind the camera or off the frame's sides) are left out of a pass from the game camera (the game drew them for the
// card to clip). Every address comes from the game's function at its first frame, checked against its opcodes, so the
// larger list IncreaseCoronaLimit made is read as it is then; a function that doesn't match is left to the game. Its
// start is TemporalAA's hook (the steady depth while it draws), which calls Draw in its place
namespace CoronaRender
{
    struct Vertex { float x, y, z, nx, ny, nz; uint32_t colour; float u, v; };
    static_assert(sizeof(Vertex) == 0x24);

    // Run before each batch is drawn, the technique set: a pass from another view puts its own depth for the
    // coronas' visibility test in (carmirrors.ixx)
    export void (*pBeforeDraw)() = nullptr;
    // A pass from another view than the game camera's, near it (a car's mirror, its reflected camera within half a metre
    // of the eye and looking back): none left out as behind or outside the view, which took the main view's. Counted:
    // the coronas listed, those behind the game camera, those drawn (carmirrors.ixx's log)
    export bool bOtherView = false;
    export uint32_t nOtherListed = 0, nOtherBehind = 0, nOtherDrawn = 0;
    // For a pass from another view: the part of the list made for it (carmirrors.ixx puts the coronas its mirror sees
    // after the game camera's), by the list's buffer; false draws none. Unset, the whole list.
    export bool (*pOtherRange)(uint32_t buffer, int32_t& first, int32_t& count) = nullptr;
    // True while the pass is one RenderSkip leaves out (its target never locked, its draws dropped): the water's
    // reflection while no water is seen still called this every frame, 14000 quads built for nothing (renderskip.ixx)
    export bool (*pPassDropped)() = nullptr;

    int32_t nMode = 1;                      // 0: the game's, 1: ours without what is out of view, 2: ours, all of them
    float* pCamera = nullptr;               // CE 0x128e340, the game camera's position, where it looks 8 floats before
    uint8_t* pFunction = nullptr;

    bool bReady = false, bFailed = false;
    uint32_t* (__cdecl* GetRenderFlags)() = nullptr;
    uint32_t* pBufferIndex = nullptr;
    int32_t* pCounts = nullptr;
    uint8_t* pShift = nullptr;              // the list's size exponent in the game's own code
    uint32_t* pFlagAddress = nullptr;       // the list + 0x1b, as the game's compare has it
    void** ppTexture = nullptr;
    void(__cdecl* PushStates)() = nullptr;
    void(__cdecl* SetState)(int, int) = nullptr;
    uint8_t** ppViewport = nullptr;
    void* pIdentity = nullptr;
    void(__fastcall* SetWorld)(void* viewport, void* edx, void* matrix) = nullptr;
    void(__cdecl* ApplyStates)() = nullptr;
    void(__cdecl* SetTexture)(void*) = nullptr;
    void(__cdecl* SetTechnique)(int, int) = nullptr;
    void(__cdecl* Begin)(int primitive, int vertices) = nullptr;
    void(__cdecl* End)() = nullptr;
    void(__cdecl* Finish)() = nullptr;
    void(__cdecl* SetFinal)(float) = nullptr;
    float* pReflectSize = nullptr;
    uint8_t* pReflectBase = nullptr;        // a float at + buffer * 0x210
    float* pReflectScale = nullptr;
    float* pReflectLimit = nullptr;
    float* pDimScale = nullptr;
    float* pOne = nullptr;
    uint32_t* pVertexLimit = nullptr;
    Vertex** ppVertex = nullptr;
    uint32_t* pVertexCount = nullptr;

    template <typename T> T Operand(uint8_t* at) { return *(T*)at; }
    template <typename T> T CallTarget(uint8_t* call) { return (T)(call + 5 + *(int32_t*)(call + 1)); }

    bool Read()
    {
        auto f = pFunction;
        auto is = [&](uint32_t offset, std::initializer_list<uint8_t> bytes)
        {
            uint32_t i = 0;
            for (auto b : bytes)
                if (f[offset + i++] != b)
                    return false;
            return true;
        };
        if (!is(0x28, { 0xE8 }) || !is(0x44, { 0x8B, 0x3D }) || !is(0x4D, { 0x8B, 0x34, 0xBD }) || !is(0x9C, { 0xC1, 0xE0 }) ||
            !is(0xA4, { 0x80, 0xB8 }) || !is(0xCE, { 0x8B, 0x35 }) || !is(0xD4, { 0xE8 }) || !is(0xDD, { 0xE8 }) ||
            !is(0xEB, { 0x8B, 0x0D }) || !is(0xF4, { 0x68 }) || !is(0xF9, { 0xE8 }) || !is(0xFE, { 0xE8 }) || !is(0x104, { 0xE8 }) ||
            !is(0x1B7, { 0xE8 }) || !is(0x1DC, { 0xE8 }) || !is(0x2E1, { 0xF3, 0x0F, 0x10, 0x05 }) || !is(0x2EF, { 0xF3, 0x0F, 0x10, 0x88 }) ||
            !is(0x2F7, { 0xF3, 0x0F, 0x59, 0x0D }) || !is(0x317, { 0xF3, 0x0F, 0x10, 0x05 }) || !is(0x379, { 0xF3, 0x0F, 0x10, 0x05 }) ||
            !is(0x409, { 0xF3, 0x0F, 0x10, 0x0D }) || !is(0x53D, { 0xE8 }) || !is(0x7D0, { 0xE8 }) || !is(0x7E3, { 0xE8 }) || !is(0x7F0, { 0xE8 }))
            return false;

        // The vertex function: cmp [limit], 0 / mov eax, [pointer] / inc [count]
        auto vertex = CallTarget<uint8_t*>(f + 0x53D);
        if (vertex[0] != 0x83 || vertex[1] != 0x3D || vertex[9] != 0xA1 || vertex[0x1D] != 0xFF || vertex[0x1E] != 0x05)
            return false;

        GetRenderFlags = CallTarget<decltype(GetRenderFlags)>(f + 0x28);
        pBufferIndex = Operand<uint32_t*>(f + 0x46);
        pCounts = Operand<int32_t*>(f + 0x50);
        pShift = f + 0x9E;
        pFlagAddress = (uint32_t*)(f + 0xA6);
        ppTexture = Operand<void**>(f + 0xD0);
        PushStates = CallTarget<decltype(PushStates)>(f + 0xD4);
        SetState = CallTarget<decltype(SetState)>(f + 0xDD);
        ppViewport = Operand<uint8_t**>(f + 0xED);
        pIdentity = Operand<void*>(f + 0xF5);
        SetWorld = CallTarget<decltype(SetWorld)>(f + 0xF9);
        ApplyStates = CallTarget<decltype(ApplyStates)>(f + 0xFE);
        SetTexture = CallTarget<decltype(SetTexture)>(f + 0x104);
        SetTechnique = CallTarget<decltype(SetTechnique)>(f + 0x1B7);
        Begin = CallTarget<decltype(Begin)>(f + 0x1DC);
        pReflectSize = Operand<float*>(f + 0x2E5);
        pReflectBase = Operand<uint8_t*>(f + 0x2F3);
        pReflectScale = Operand<float*>(f + 0x2FB);
        pReflectLimit = Operand<float*>(f + 0x31B);
        pDimScale = Operand<float*>(f + 0x37D);
        pOne = Operand<float*>(f + 0x40D);
        End = CallTarget<decltype(End)>(f + 0x7D0);
        Finish = CallTarget<decltype(Finish)>(f + 0x7E3);
        SetFinal = CallTarget<decltype(SetFinal)>(f + 0x7F0);
        pVertexLimit = Operand<uint32_t*>(vertex + 2);
        ppVertex = Operand<Vertex**>(vertex + 0xA);
        pVertexCount = Operand<uint32_t*>(vertex + 0x1F);
        return true;
    }

    // In place of CCoronas::Render; false when the game's own is to run
    export bool Draw()
    {
        if (!nMode || !pFunction)
            return false;
        if (pPassDropped && pPassDropped())
            return true;
        if (!bReady && !bFailed)
        {
            bReady = Read();
            bFailed = !bReady;
            FusionLog::Write("Coronas", "Renderer", bReady ? "the coronas' quads are written by Fusion Fix\n"
                                                           : "the game's corona function is not as expected, it draws them itself\n");
        }
        if (!bReady)
            return false;

        uint32_t flags = *GetRenderFlags();
        bool reflection = (flags >> 0x12) & 1;  // technique 0xf, and coronas with +0x1b set left out
        bool dim = (flags >> 0x11) & 1;
        uint32_t buffer = *pBufferIndex;
        int32_t first = 0;
        int32_t count = pCounts[buffer];
        if (bOtherView && pOtherRange)
        {
            int32_t otherCount = 0;
            if (!pOtherRange(buffer, first, otherCount))
                return true;
            count = first + otherCount;
        }
        if (count <= first)
            return true;
        auto list = (uint8_t*)(uintptr_t(*pFlagAddress) - 0x1B) + (size_t(buffer) * (3u << *pShift)) * 0x20;

        // Behind the camera, only in a pass from the game camera itself: the water's reflection and other views keep
        // them all (the paraboloid reflection has its own test below)
        auto viewport = *ppViewport;
        auto eye = (const float*)(viewport + 0x70);
        float forward[3] = {};
        bool skipBehind = false;
        if (nMode == 1 && !reflection && pCamera && !bOtherView)
        {
            auto right = (const float*)(viewport + 0x40), up = (const float*)(viewport + 0x50);
            const float* look = pCamera - 8;
            float dx = eye[0] - pCamera[0], dy = eye[1] - pCamera[1], dz = eye[2] - pCamera[2];
            float length = std::sqrt(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
            if (dx * dx + dy * dy + dz * dz < 0.25f && length > 0.5f && length < 2.0f)
            {
                for (int i = 0; i < 3; i++)
                    forward[i] = look[i] / length;
                // The view's own axes must agree that this is where it looks
                float cross[3] = { right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2], right[0] * up[1] - right[1] * up[0] };
                skipBehind = std::fabs(cross[0] * forward[0] + cross[1] * forward[1] + cross[2] * forward[2]) > 0.95f;
            }
        }

        // The paraboloid reflection (the reflection map, technique 0xf: paraboloid_corona, deferred_lighting VS13): its
        // eye 512 m under the game camera, the paraboloid's centre C 512 m above that eye. The shader gives a corona no
        // colour and no size unless its centre, moved toward the eye, is above C, and fades it out by the distance of
        // its corners from C, none left 64 m away (1 - L/64 times 16, saturated). Those are left out here (99.5% of
        // them in a night's log); the corners are within the size times the reflection's scale times 1.42 of the centre.
        bool skipParaboloid = false;
        float paraboloidCentre[3] = {};
        if (nMode == 1 && reflection && pCamera && !bOtherView)
        {
            // The view's z axis (grcViewport +0x60) straight down, as the shader's distance takes it: L is from the eye
            // plus 512 m along the view's z, C only so
            float dx = eye[0] - pCamera[0], dy = eye[1] - pCamera[1], dz = eye[2] + 512.0f - pCamera[2];
            if (dx * dx + dy * dy < 0.25f && dz * dz < 1.0f && ((const float*)(viewport + 0x60))[2] < -0.99f)
            {
                paraboloidCentre[0] = eye[0];
                paraboloidCentre[1] = eye[1];
                paraboloidCentre[2] = eye[2] + 512.0f;
                skipParaboloid = true;
            }
        }
        constexpr float ParaboloidFadeEnd = 64.0f;

        // Outside the view as well: a cone round where the camera looks, as wide as the projection's corner and a tenth
        // more (grcViewport +0x1c0, its x and y scales), each corona a ball 1.5 times its size (its quad's corners are
        // at most 1.42 times out); the shift toward the camera keeps a corona on its line of sight
        float coneCos = 0.0f, coneSin = 0.0f;
        bool skipOutside = false;
        if (skipBehind && viewport[0x2F0])
        {
            auto projection = (const float*)(viewport + 0x1C0);
            float scaleX = std::fabs(projection[0]), scaleY = std::fabs(projection[5]);
            if (scaleX > 0.05f && scaleY > 0.05f && scaleX < 100.0f && scaleY < 100.0f)
            {
                float tanCorner = std::sqrt(1.0f / (scaleX * scaleX) + 1.0f / (scaleY * scaleY)) * 1.1f;
                float hypotenuse = std::sqrt(1.0f + tanCorner * tanCorner);
                coneCos = 1.0f / hypotenuse;
                coneSin = tanCorner / hypotenuse;
                skipOutside = true;
            }
        }

        // Any other view from the game's own camera list (the water's reflection: its eye under the water, so none of
        // the tests above): the view's six frustum planes, as grcViewport::IsSphereVisible (CE 0x431e40) reads them at
        // +0x300 (normal, then distance; inside when n.p + d + r >= 0). That pass drew all 15000 coronas of a dawn, 1.2 ms
        // of the render thread. The ball takes the shift toward the camera (+0x14) in as well, which can bring a corona
        // over the far plane
        const float* frustum = nullptr;
        if (nMode == 1 && !reflection && !skipBehind && !bOtherView)
        {
            auto planes = (const float*)(viewport + 0x300);
            bool valid = true;
            for (int k = 0; k < 6 && valid; k++)
            {
                const float* p = planes + k * 4;
                float lengthSquared = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
                valid = lengthSquared > 0.81f && lengthSquared < 1.21f && std::isfinite(p[3]);
            }
            if (valid)
                frustum = planes;
        }

        static std::vector<uint32_t> drawn;
        drawn.clear();
        for (int32_t i = first; i < count; i++)
        {
            auto item = list + size_t(i) * 0x20;
            if (reflection && item[0x1B])
                continue;
            if (skipBehind)
            {
                auto pos = (const float*)item;
                float size = std::fabs(*(const float*)(item + 0xC));
                // No size, no pixels: its two triangles have none
                if (size == 0.0f)
                    continue;
                float d[3] = { pos[0] - eye[0], pos[1] - eye[1], pos[2] - eye[2] };
                float ahead = d[0] * forward[0] + d[1] * forward[1] + d[2] * forward[2];
                if (ahead < -2.0f * size - 1.0f)
                    continue;
                if (skipOutside)
                {
                    // The distance from the cone's surface, never more than the true one (also behind its tip):
                    // lateral * cos - ahead * sin > margin, compared squared, as the square root of each of thousands
                    // of coronas took 5% of the render thread in a profile
                    float lateralSquared = std::max(0.0f, d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - ahead * ahead);
                    float margin = 1.5f * size + 0.5f + ahead * coneSin;
                    if (margin < 0.0f || lateralSquared * coneCos * coneCos > margin * margin)
                        continue;
                }
            }
            else if (frustum)
            {
                auto pos = (const float*)item;
                float size = std::fabs(*(const float*)(item + 0xC));
                if (size == 0.0f)
                    continue;
                float radius = 1.5f * size + 0.5f + std::fabs(*(const float*)(item + 0x14));
                bool inside = true;
                for (int k = 0; k < 6 && inside; k++)
                {
                    const float* p = frustum + k * 4;
                    inside = p[0] * pos[0] + p[1] * pos[1] + p[2] * pos[2] + p[3] + radius >= 0.0f;
                }
                if (!inside)
                    continue;
            }
            else if (skipParaboloid)
            {
                // As the draw moves it toward the eye (the vertices' centre the shader reads)
                auto pos = (const float*)item;
                float toward = *(const float*)(item + 0x14);
                float d[3] = { pos[0] - eye[0], pos[1] - eye[1], pos[2] - eye[2] };
                float lengthSquared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                float inverse = lengthSquared == 0.0f ? 0.0f : *pOne / std::sqrt(lengthSquared);
                float P[3];
                for (int k = 0; k < 3; k++)
                    P[k] = pos[k] - d[k] * inverse * toward;
                if (P[2] - paraboloidCentre[2] <= 0.0f)
                    continue;
                float corner = std::fabs(*(const float*)(item + 0xC)) * std::fabs(*pReflectSize) * 1.5f;
                float c[3] = { P[0] - paraboloidCentre[0], P[1] - paraboloidCentre[1], P[2] - paraboloidCentre[2] };
                float reach = ParaboloidFadeEnd + corner + 0.5f;
                if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] > reach * reach)
                    continue;
            }
            drawn.push_back(uint32_t(i));
        }
        if (bOtherView)
        {
            nOtherListed += uint32_t(count - first);
            nOtherDrawn += uint32_t(drawn.size());
            if (pCamera)
            {
                const float* look = pCamera - 8;
                for (int32_t i = first; i < count; i++)
                {
                    auto pos = (const float*)(list + size_t(i) * 0x20);
                    if ((pos[0] - pCamera[0]) * look[0] + (pos[1] - pCamera[1]) * look[1] + (pos[2] - pCamera[2]) * look[2] < 0.0f)
                        ++nOtherBehind;
                }
            }
        }
        if (drawn.empty())
            return true;

        auto texture = *ppTexture;
        PushStates();
        SetState(2, 1);
        SetState(6, 0);
        SetWorld(*ppViewport, nullptr, pIdentity);
        ApplyStates();
        SetTexture(texture);

        viewport = *ppViewport;
        const float axisRight[3] = { ((float*)(viewport + 0x40))[0], ((float*)(viewport + 0x40))[1], ((float*)(viewport + 0x40))[2] };
        const float axisUp[3] = { -((float*)(viewport + 0x50))[0], -((float*)(viewport + 0x50))[1], -((float*)(viewport + 0x50))[2] };
        const float camera[3] = { ((float*)(viewport + 0x70))[0], ((float*)(viewport + 0x70))[1], ((float*)(viewport + 0x70))[2] };
        // Another view's pass takes the corona technique's second pass, as the dimmed ones do: tested against the depth
        // buffer (ZEnable 1), not against the main view's depth texture in the vertex shader (the first pass, depth off),
        // which had a car mirror show the city's lights through the buildings
        SetTechnique(reflection ? 0xF : 0xE, reflection ? 0 : ((dim || bOtherView) ? 1 : 0));

        for (size_t first = 0; first < drawn.size();)
        {
            size_t n = std::min<size_t>(drawn.size() - first, 0xAAA);
            Begin(3, int(n * 6));
            for (size_t k = first; k < first + n; k++)
            {
                auto item = list + size_t(drawn[k]) * 0x20;
                auto pos = (const float*)item;
                float size = *(const float*)(item + 0xC);
                float brightness = *(const float*)(item + 0x10);
                float toward = *(const float*)(item + 0x14);
                uint32_t colour = 0xFF000000u | uint32_t(item[0x18]) << 16 | uint32_t(item[0x19]) << 8 | uint32_t(item[0x1A]);

                float R[3], U[3];
                for (int i = 0; i < 3; i++)
                {
                    R[i] = axisRight[i] * size;
                    U[i] = axisUp[i] * size;
                }
                if (reflection)
                {
                    for (int i = 0; i < 3; i++)
                    {
                        R[i] *= *pReflectSize;
                        U[i] *= *pReflectSize;
                    }
                    float t = *(const float*)(pReflectBase + size_t(buffer) * 0x210) * *pReflectScale;
                    float limit = *pReflectLimit * *pReflectLimit;
                    brightness = t > limit ? limit : t;
                }
                else if (dim)
                    brightness *= *pDimScale;

                // Moved toward the camera by +0x14
                float d[3] = { pos[0] - camera[0], pos[1] - camera[1], pos[2] - camera[2] };
                float lengthSquared = d[1] * d[1] + d[0] * d[0] + d[2] * d[2];
                float inverse = lengthSquared == 0.0f ? 0.0f : *pOne / std::sqrt(lengthSquared);
                float P[3];
                for (int i = 0; i < 3; i++)
                    P[i] = pos[i] - d[i] * inverse * toward;

                *pVertexCount += 6;
                auto v = *ppVertex;
                if (!*pVertexLimit || !v)
                    continue;
                // The buffer is the card's write-combined memory: it is only written, in one go. Copying a vertex
                // within it (v[3] = v[2]) read it back uncached, and made the coronas far slower than the game's
                auto corner = [&](Vertex& out, bool plusRight, bool plusUp, uint32_t c, float u)
                {
                    float p[3];
                    for (int i = 0; i < 3; i++)
                    {
                        float side = plusRight ? P[i] + R[i] : P[i] - R[i];
                        p[i] = plusUp ? side + U[i] : side - U[i];
                    }
                    out = { p[0], p[1], p[2], P[0], P[1], P[2], c, u, brightness };
                };
                uint32_t clear = colour & 0xFFFFFFu;
                Vertex quad[6];
                corner(quad[0], false, true, colour, 0.0f);
                corner(quad[1], true, true, colour, 1.0f);
                corner(quad[2], true, false, clear, 1.0f);
                quad[3] = quad[2];
                corner(quad[4], false, false, clear, 0.0f);
                quad[5] = quad[0];
                memcpy(v, quad, sizeof(quad));
                *ppVertex = v + 6;
            }
            if (pBeforeDraw)
                pBeforeDraw();
            End();
            first += n;
        }

        Finish();
        SetFinal(1.0f);
        SetState(2, 0);
        SetState(6, 1);
        return true;
    }
}

class Coronas
{
public:
    Coronas()
    {
        FusionFix::onInitEventAsync() += []()
        {
            auto pattern = hook::pattern("81 FE ? ? ? ? 0F 8D ? ? ? ? 8B 44 24 08 8B 4C 24 1C F3 0F 10 44 24 ? C1 E2 06");
            if (!pattern.empty())
            {
                auto i = injector::ReadMemory<uint32_t>(pattern.get(0).get<uintptr_t>(2), true);
                if (i == 768)
                    IncreaseCoronaLimit();
            }
            else
            {
                pattern = hook::pattern("81 FE ? ? ? ? 0F 8D ? ? ? ? 8B 4C 24 08 8A 54 24 0C");
                auto i = injector::ReadMemory<uint32_t>(pattern.get(0).get<uintptr_t>(2), true);
                if (i == 768)
                    IncreaseCoronaLimitEFLC();
            }
        };

        FusionFix::onInitEvent() += []()
        {
            using namespace CoronaRender;
            SettingsReader reader;
            nMode = std::clamp(reader.ReadInteger("PROJECT2DFX", "CoronaRenderer", 1), 0, 2);
            if (!nMode)
                return;
            // The game camera, where the procedural generator measures from too (movss xmm2, [0x128e340] in CE 0xc09440)
            if (auto camera = hook::pattern("F3 0F 10 05 ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ? 56 8B 75 0C"); !camera.empty())
                pCamera = *camera.get_first<float*>(12);
            // Inside it: lea eax, [edi + edi*2] / shl eax, exponent / add eax, edx / shl eax, 5 / cmp byte [eax + list + 0x1b], 0
            auto inside = hook::pattern("8D 04 7F C1 E0 ? 03 C2 C1 E0 05 80 B8 ? ? ? ? 00 74");
            if (inside.empty())
                return;
            pFunction = inside.get_first<uint8_t>(-0x99);
        };
    }
} Coronas;