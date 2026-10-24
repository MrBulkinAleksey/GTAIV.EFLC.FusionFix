// The night shadow logic that needs no game: slot keys, lamp spacing, lifetimes of siren and signal lights,
// and the check of the game's shadow lookup before it is patched.
#include "check.hpp"
#include "nightshadows/LabSirenLifetime.hpp"
#include "nightshadows/NativeShadowContinuity42.hpp"
#include "nightshadows/ShadowInactiveSlots.hpp"
#include "nightshadows/ShadowLampSpacing.hpp"
#include "nightshadows/ShadowLookupLayout.hpp"
#include "nightshadows/ShadowReach.hpp"
#include "nightshadows/TrafficSignalIdentity.hpp"
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace fusionfix::shadows;

TEST(ShadowReachClampsItsSteps)
{
    CHECK(ShadowReachFeet(-1) == 0);
    CHECK(ShadowReachFeet(40) == 200);
    CHECK(ShadowReachFeet(MaxShadowReachStep + 1) == 500);
}

TEST(ShadowReachTakesLightsUpToItsDistance)
{
    const float metres = 40 * 5 * 0.3048f; // step 40: 200 ft
    CHECK(WithinShadowReach(metres * metres * 0.99f, 40));
    CHECK(!WithinShadowReach(metres * metres * 1.01f, 40));
    CHECK(!WithinShadowReach(1.0f, 0)); // off
    CHECK(!WithinShadowReach(-1.0f, 40));
    CHECK(!WithinShadowReach(std::numeric_limits<float>::quiet_NaN(), 40));
}

// The game's shadow records: 8 of 0x110 bytes, the active byte at 0xED, the key at 0xF8.
static void SetRecord(std::vector<uint8_t>& records, unsigned slot, bool active, uint32_t key)
{
    records[slot * 0x110 + 0xED] = active ? 1 : 0;
    std::memcpy(&records[slot * 0x110 + 0xF8], &key, 4);
}

static uint32_t Key(const std::vector<uint8_t>& records, unsigned slot)
{
    uint32_t key;
    std::memcpy(&key, &records[slot * 0x110 + 0xF8], 4);
    return key;
}

TEST(InactiveShadowKeysAreClearedOnlyOnInactiveDynamicSlots)
{
    std::vector<uint8_t> records(0x880);
    SetRecord(records, 0, false, 11); // the static refresh slot stays
    SetRecord(records, 1, false, 12);
    SetRecord(records, 2, true, 13);
    SetRecord(records, 7, false, 14);
    CHECK(ClearInactiveShadowKeys(records.data(), records.size()) == 2);
    CHECK(Key(records, 0) == 11);
    CHECK(Key(records, 1) == 0);
    CHECK(Key(records, 2) == 13);
    CHECK(Key(records, 7) == 0);
    CHECK(ClearInactiveShadowKeys(records.data(), 0x87F) == 0);
    CHECK(ClearInactiveShadowKeys(nullptr, 0x880) == 0);
}

// Seven lamps in a row, 4 m apart, on the x axis.
static void AddRow(ShadowLampSpacing& spacing, unsigned nearest)
{
    spacing.Begin(7);
    for (uint32_t i = 0; i < 7; ++i)
    {
        ShadowLampSpacing::Lamp lamp;
        lamp.index = i;
        lamp.position = { 4.0f * i, 0.0f, 5.0f };
        lamp.distanceSquared = std::abs(float(int(i) - int(nearest))) * 4.0f;
        lamp.key = 100 + i;
        spacing.Add(lamp);
    }
}

TEST(LampSpacingKeepsEveryThirdLampOfARowNotJustOne)
{
    ShadowLampSpacing spacing;
    spacing.base = 10.0f;
    spacing.Update(0.0f, false, 1000);
    CHECK(spacing.spacing == 10.0f);
    AddRow(spacing, 0);
    // Kept 0 m, 12 m and 24 m: the lamps they lowered lower no one.
    CHECK(spacing.Resolve() == 4);
    const bool lowered[7] = { false, true, true, false, true, true, false };
    for (uint32_t i = 0; i < 7; ++i)
        CHECK(spacing.SpacedOut(i) == lowered[i]);
}

TEST(LampSpacingKeepsLastPassLampsWhenTheNearestChanges)
{
    ShadowLampSpacing spacing;
    spacing.base = 10.0f;
    spacing.Update(0.0f, false, 1000);
    AddRow(spacing, 0);
    spacing.Resolve();
    // Lamp 1 is now the nearest; the lamps kept last pass still go first and keep their slots.
    AddRow(spacing, 1);
    spacing.Resolve();
    CHECK(!spacing.SpacedOut(0));
    CHECK(spacing.SpacedOut(1));
    CHECK(!spacing.SpacedOut(3));
}

TEST(LampSpacingLeavesSpotsShiningAnotherWay)
{
    ShadowLampSpacing spacing;
    spacing.base = 10.0f;
    spacing.Update(0.0f, false, 1000);
    spacing.Begin(2);
    ShadowLampSpacing::Lamp a, b;
    a.index = 0; a.position = { 0, 0, 5 }; a.direction = { 0, 0, -1 }; a.spot = true; a.distanceSquared = 1;
    b.index = 1; b.position = { 4, 0, 5 }; b.direction = { 1, 0, 0 }; b.spot = true; b.distanceSquared = 2;
    spacing.Add(a);
    spacing.Add(b);
    CHECK(spacing.Resolve() == 0);
}

TEST(LampSpacingOff)
{
    ShadowLampSpacing spacing;
    spacing.Update(0.0f, false, 1000);
    AddRow(spacing, 0);
    CHECK(spacing.Resolve() == 0);
}

TEST(SirenLifetimeDecisions)
{
    using namespace lab_siren;
    const Identity car{ 1, 2, 3, 4 }, other{ 1, 2, 3, 5 };
    CHECK(Decide(false, true, true, true, car, car) == Decision::Retire);
    CHECK(Decide(false, false, true, false, car, car) == Decision::Wait);
    CHECK(Decide(true, false, true, false, car, car) == Decision::Retire);
    CHECK(Decide(true, false, false, true, car, car) == Decision::Keep);
    CHECK(Decide(true, false, false, true, car, other) == Decision::Retire);
    CHECK(Decide(false, false, true, true, car, other) == Decision::Bind);
    CHECK(Decide(false, false, false, true, car, other) == Decision::Wait);
}

TEST(TrafficSignalLifetimeBindsOnceAndRetiresOnAGapOrAnotherOwner)
{
    using namespace traffic_signal;
    const Identity signal{ 1, 2, 3, 4, 5, 6, 7 };
    Lifetime incomplete;
    CHECK(!incomplete.Bind({ 1, 2, 3, 0, 5, 6, 7 }, 10)); // no owner

    Lifetime life;
    CHECK(life.Bind(signal, 10));
    CHECK(!life.Bind(signal, 10));
    CHECK(life.Observe(signal, 12, 1));
    CHECK(life.phaseMask == 2);
    CHECK(!life.Observe(signal, 15, 0)); // three frames missed
    CHECK(life.retired);

    Lifetime moved;
    moved.Bind(signal, 10);
    Identity reused = signal;
    reused.reference = 99;
    CHECK(!moved.Observe(reused, 11, 0));
    CHECK(moved.retired);

    Lifetime wrapping;
    wrapping.Bind(signal, 0xFFFFFFFFu);
    CHECK(wrapping.Observe(signal, 1, 0)); // two frames on, across the counter's wrap
}

TEST(ShadowLookupLayoutMatchesTheGameAndOurHook)
{
    using namespace shadow_lookup_layout;
    constexpr uintptr_t kFunction = 0x525DB0;
    std::vector<uint8_t> memory(sizeof(Original));
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory.data()) - kFunction;
    // The function as loaded at base: the relocated addresses move with it.
    std::memcpy(memory.data(), Original, sizeof(Original));
    for (unsigned offset : Relocs)
    {
        uint32_t address;
        std::memcpy(&address, Original + offset, 4);
        address = address - 0x400000u + static_cast<uint32_t>(base);
        std::memcpy(memory.data() + offset, &address, 4);
    }
    CHECK(Validate(base));

    memory[0] = 0xE9; // our jump over the first instruction
    CHECK(Validate(base));

    memory[40] ^= 0xFF;
    CHECK(!Validate(base));
}

TEST(LastResortLightsGiveWayOnlyToLightsShowingAsMuch)
{
    using namespace fusionfix::shadows;
    using Continuity = NativeShadowContinuity42;
    const Continuity continuity;
    // a facade light of ours over you, its shadow off screen but cast on you; lamps 90 m off whose shadows show
    // nothing; a street lamp whose shadow shows in view
    const auto facade = continuity.Observe({ 1, 0 }, 0x45, true, false, SlotGain::OffScreen, 6.0f * 6.0f, false, false, true, true);
    const auto distant = continuity.Observe({ 2, 0 }, 0x6F, true, false, SlotGain::None, 90.0f * 90.0f);
    const auto inView = continuity.Observe({ 3, 0 }, 0x6F, true, false, SlotGain::InView, 30.0f * 30.0f);
    const auto otherFacade = continuity.Observe({ 4, 0 }, 0x45, true, false, SlotGain::OffScreen, 4.0f * 4.0f, false, false, true, true);
    const auto sameGainLamp = continuity.Observe({ 5, 0 }, 0x6F, true, false, SlotGain::OffScreen, 40.0f * 40.0f);
    // 0 the challenger goes first, 2 the incumbent stays
    CHECK(Continuity::Compare(1, facade, distant) == 0);
    CHECK(Continuity::Compare(1, distant, facade) == 2);
    CHECK(Continuity::Compare(1, inView, facade) == 0);
    CHECK(Continuity::Compare(1, facade, inView) == 2);
    // among lights showing as much, the last resort gives way, nearer or not
    CHECK(Continuity::Compare(1, sameGainLamp, facade) == 0);
    CHECK(Continuity::Compare(1, facade, sameGainLamp) == 2);
    // two last resorts of the same gain: the game's order
    CHECK(Continuity::Compare(1, otherFacade, facade) == 1);
    CHECK(Continuity::Decider(1, facade, distant) == Continuity::Rule::Gain);
    CHECK(Continuity::Decider(1, sameGainLamp, facade) == Continuity::Rule::LastResort);
}
