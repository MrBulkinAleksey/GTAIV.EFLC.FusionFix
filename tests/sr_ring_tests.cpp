#include "check.hpp"
#include "../source/includes/sr_ring.hpp"

// The protocol has a Windows ABI. Linux protocol coverage uses -fshort-wchar;
// the slot ownership tests also run in the ordinary portable suite.
#if defined(_WIN32) || (defined(__SIZEOF_WCHAR_T__) && __SIZEOF_WCHAR_T__ == 2)
#include "../source/includes/upscaler_protocol.hpp"
#include <cstring>

TEST(SrRingTextureMapping)
{
    namespace P = UpscalerProtocol;
    for (uint32_t slot = 0; slot < P::SlotCount; ++slot)
        for (uint32_t i = 0; i < P::InputCount + 1; ++i)
        {
            auto base = static_cast<P::Texture>(i);
            auto texture = P::SlotTexture(base, slot);
            CHECK(static_cast<uint32_t>(texture) < static_cast<uint32_t>(P::Texture::Count));
            CHECK(P::BaseTexture(texture) == base);
            CHECK(P::IsOutputSize(texture) == (base == P::Texture::Output));
            CHECK(!P::IsFrameGenerationTexture(texture));
        }
    CHECK(P::SlotTexture(P::Texture::Output, P::SlotCount) == P::Texture::Count);
    CHECK(P::BaseTexture(P::Texture::Count) == P::Texture::Count);
}

TEST(SrRingImmutableMetadataLayout)
{
    namespace P = UpscalerProtocol;
    P::Shared shared{};
    shared.WaitValue = 71;
    shared.SignalValue = 72;
    shared.CameraForward[2] = 3;
    shared.FrameId = 8;
    shared.HudLess = 1;
    std::memcpy(&shared.Slots[1].Metadata, &shared.WaitValue, sizeof(P::FrameMetadata));
    shared.FrameId = 9; // the next request must not mutate retained slot metadata
    const auto& metadata = shared.Slots[1].Metadata;
    CHECK(metadata.WaitValue == 71 && metadata.SignalValue == 72);
    CHECK(metadata.FrameId == 8 && metadata.CameraForward[2] == 3 && metadata.HudLess == 1);
}
#endif

TEST(SrRingOwnershipAndEpoch)
{
    UpscalerRing::State ring;
    CHECK(!ring.HasPrevious() && ring.NeedsReset());
    auto epoch = ring.Epoch();
    for (uint64_t id = 1; id <= 12; ++id)
    {
        auto slot = ring.Next();
        ring.Push(slot, id, id * 2);
        CHECK(ring.Previous().id == id && ring.Previous().value == id * 2);
        CHECK(ring.Next() != slot && !ring.NeedsReset());
    }
    ring.Reset();
    CHECK(!ring.HasPrevious() && ring.Epoch() != epoch && ring.NeedsReset());
    ring.Push(ring.Next(), 13, 26);
    CHECK(ring.Previous().id == 13);
}
