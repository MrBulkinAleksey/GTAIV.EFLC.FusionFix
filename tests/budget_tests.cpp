// The shadow slot budget (PlayerShadowBudget): which of a pass's lights take the seven dynamic shadow maps, and
// how they keep them from one pass to the next.
#include "check.hpp"
#include "nightshadows/PlayerShadowBudget.hpp"
#include <memory>

using namespace fusionfix::shadows::budget;
using Budget = PlayerShadowBudget;

static Candidate Lamp(uint64_t key, float metres, bool onPlayer = false)
{
    Candidate c;
    c.key = key;
    c.index = uint32_t(key);
    c.kind = Kind::Lamp;
    c.distanceSquared = metres * metres;
    c.influencesPlayer = onPlayer;
    c.engineEligible = true;
    return c;
}

static Frame At(uint64_t serial, uint32_t ms, bool driving = false)
{
    return { serial, ms, 1, driving };
}

static bool Holds(const Budget::Selection& s, uint64_t key)
{
    for (size_t i = 0; i < Budget::SlotCount; ++i)
        if ((s.validMask & (1u << i)) && s.slots[i].key == key)
            return true;
    return false;
}

static int SlotOf(const Budget::Selection& s, uint64_t key)
{
    for (size_t i = 0; i < Budget::SlotCount; ++i)
        if ((s.validMask & (1u << i)) && s.slots[i].key == key)
            return int(i);
    return -1;
}

TEST(BudgetTakesTheSevenNearestLampsWithinReach)
{
    auto budget = std::make_unique<Budget>();
    budget->BeginPass(At(1, 1000));
    for (uint64_t i = 1; i <= 10; ++i)
        budget->Add(Lamp(i, 5.0f * i)); // 5 m to 50 m
    budget->Add(Lamp(99, 61.0f));       // past the 60 m a lamp may be
    const auto& s = budget->Finalize();
    CHECK(s.safeToApply);
    CHECK(s.count == 7);
    for (uint64_t i = 1; i <= 7; ++i)
        CHECK(Holds(s, i));
    CHECK(!Holds(s, 8));
    CHECK(!Holds(s, 99));
}

TEST(BudgetPutsLampsOnThePlayerFirst)
{
    auto budget = std::make_unique<Budget>();
    budget->BeginPass(At(1, 1000));
    for (uint64_t i = 1; i <= 8; ++i)
        budget->Add(Lamp(i, float(i)));
    budget->Add(Lamp(50, 40.0f, true));
    budget->Add(Lamp(51, 45.0f, true));
    const auto& s = budget->Finalize();
    CHECK(Holds(s, 50));
    CHECK(Holds(s, 51));
    CHECK(Holds(s, 5));
    CHECK(!Holds(s, 6)); // the two on the player took two of the nearer lamps' slots
}

TEST(BudgetKeepsThePlayersHeadlights)
{
    auto budget = std::make_unique<Budget>();
    budget->BeginPass(At(1, 1000));
    for (uint64_t i = 1; i <= 8; ++i)
        budget->Add(Lamp(i, float(i)));
    Candidate beam = Lamp(70, 20.0f);
    beam.kind = Kind::PlayerBeam;
    budget->Add(beam);
    CHECK(Holds(budget->Finalize(), 70));
}

TEST(BudgetLeavesAHeldLampItsSlotAndPlace)
{
    auto budget = std::make_unique<Budget>();
    budget->BeginPass(At(1, 1000));
    for (uint64_t i = 1; i <= 7; ++i)
        budget->Add(Lamp(i, 10.0f + i));
    const auto& first = budget->Finalize();
    const int place = SlotOf(first, 7); // 17 m, the farthest kept

    // A new lamp at 16 m is nearer than lamp 7, not by enough to take its slot.
    budget->BeginPass(At(2, 1600));
    budget->Add(Lamp(8, 16.0f));
    for (uint64_t i = 7; i >= 1; --i) // another order, other list indices
        budget->Add(Lamp(i, 10.0f + i));
    const auto& second = budget->Finalize();
    CHECK(Holds(second, 7));
    CHECK(!Holds(second, 8));
    CHECK(SlotOf(second, 7) == place);

    // Much nearer, it does.
    budget->BeginPass(At(3, 2200));
    budget->Add(Lamp(8, 2.0f));
    for (uint64_t i = 1; i <= 7; ++i)
        budget->Add(Lamp(i, 10.0f + i));
    const auto& third = budget->Finalize();
    CHECK(Holds(third, 8));
    CHECK(!Holds(third, 7));
}

TEST(BudgetForgetsItsSlotsAfterAPause)
{
    auto budget = std::make_unique<Budget>();
    budget->BeginPass(At(1, 1000));
    for (uint64_t i = 1; i <= 7; ++i)
        budget->Add(Lamp(i, 10.0f + i));
    budget->Finalize();
    // Three seconds later nothing is held: the nearer new lamp takes the farthest one's slot.
    budget->BeginPass(At(2, 4000));
    budget->Add(Lamp(8, 16.0f));
    for (uint64_t i = 1; i <= 7; ++i)
        budget->Add(Lamp(i, 10.0f + i));
    const auto& s = budget->Finalize();
    CHECK(Holds(s, 8));
    CHECK(!Holds(s, 7));
}

TEST(BudgetRefusesInputItCannotTrust)
{
    auto budget = std::make_unique<Budget>();

    budget->BeginPass(At(1, 1000));
    budget->Add(Lamp(1, 5.0f));
    Candidate sameKey = Lamp(1, 6.0f);
    sameKey.index = 2; // one light at two places in the list
    budget->Add(sameKey);
    CHECK(!budget->Finalize().safeToApply);

    budget->BeginPass(At(2, 1100));
    budget->Add(Lamp(0, 5.0f)); // no identity
    CHECK(budget->Finalize().unsupportedIdentity);

    budget->BeginPass(At(3, 1200));
    for (uint64_t i = 1; i <= Budget::InputCapacity + 1; ++i)
        budget->Add(Lamp(i, 1.0f + 0.01f * i));
    const auto& full = budget->Finalize();
    CHECK(full.inputOverflow);
    CHECK(!full.safeToApply);
}
