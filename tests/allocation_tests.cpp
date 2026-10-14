// The shadow allocation pass (ShadowAllocationPass): the transaction that writes the seven shadow slot indices
// into the game's pass, only when everything it saw is still as it was.
#include "check.hpp"
#include "nightshadows/ShadowAllocationPass.hpp"
#include <memory>
#include <vector>

using namespace fusionfix::shadows::budget;
using Pass = ShadowAllocationPass;

// A light list of n lights of 0x80 bytes, each with its own bytes.
static std::vector<uint8_t> Lights(uint32_t n)
{
    std::vector<uint8_t> list(n * Pass::LightBytes);
    for (size_t i = 0; i < list.size(); ++i)
        list[i] = uint8_t(i * 7 + 3);
    return list;
}

static Candidate Lamp(uint32_t index, float metres)
{
    Candidate c;
    c.key = 1000 + index;
    c.index = index;
    c.kind = Kind::Lamp;
    c.distanceSquared = metres * metres;
    c.engineEligible = true;
    return c;
}

static const void* At(const std::vector<uint8_t>& list, uint32_t index)
{
    return list.data() + index * Pass::LightBytes;
}

// Ten lamps, the nearest at the end of the list; the game's own choice the first seven.
struct Setup
{
    std::vector<uint8_t> list = Lights(10);
    std::unique_ptr<Pass> pass = std::make_unique<Pass>();
    int32_t seven[7] = { 0, 1, 2, 3, 4, 5, 6 };

    void Observe()
    {
        pass->Begin({ 1, 1000, 1, false }, list.data(), 10);
        for (uint32_t i = 0; i < 10; ++i)
            pass->Observe(Lamp(i, 50.0f - 4.0f * i), At(list, i));
    }
};

static bool HasIndex(const int32_t (&seven)[7], int32_t index)
{
    for (int32_t i : seven)
        if (i == index)
            return true;
    return false;
}

TEST(AllocationWritesTheBudgetsSevenLamps)
{
    Setup s;
    s.Observe();
    CHECK(s.pass->Commit(s.list.data(), 10, s.seven));
    for (int32_t i = 3; i < 10; ++i) // the seven nearest
        CHECK(HasIndex(s.seven, i));
    CHECK(!HasIndex(s.seven, 0));
}

TEST(AllocationLeavesTheGamesChoiceWhenALightChangedMeanwhile)
{
    Setup s;
    s.Observe();
    s.list[5 * Pass::LightBytes + 0x10] ^= 1;
    CHECK(!s.pass->Commit(s.list.data(), 10, s.seven));
    for (int32_t i = 0; i < 7; ++i)
        CHECK(s.seven[i] == i);
}

TEST(AllocationRefusesALightNotWhereItsIndexSays)
{
    Setup s;
    s.pass->Begin({ 1, 1000, 1, false }, s.list.data(), 10);
    s.pass->Observe(Lamp(2, 5.0f), At(s.list, 3));
    CHECK(!s.pass->Active());
    CHECK(!s.pass->Commit(s.list.data(), 10, s.seven));
    CHECK(s.seven[0] == 0);
}

TEST(AllocationRefusesOneIndexOrKeyTwice)
{
    Setup s;
    s.pass->Begin({ 1, 1000, 1, false }, s.list.data(), 10);
    s.pass->Observe(Lamp(2, 5.0f), At(s.list, 2));
    s.pass->Observe(Lamp(2, 6.0f), At(s.list, 2));
    CHECK(s.pass->FailureCode() == 3);

    s.pass->Begin({ 2, 1100, 1, false }, s.list.data(), 10);
    Candidate other = Lamp(4, 6.0f);
    other.key = 1000 + 2; // light 2's key on light 4
    s.pass->Observe(Lamp(2, 5.0f), At(s.list, 2));
    s.pass->Observe(other, At(s.list, 4));
    CHECK(s.pass->FailureCode() == 3);
}

TEST(AllocationRefusesAnotherListOrABadGameChoice)
{
    Setup s;
    s.Observe();
    auto copy = s.list;
    CHECK(!s.pass->Commit(copy.data(), 10, s.seven)); // same bytes, another list

    s.Observe();
    int32_t twice[7] = { 0, 0, 2, 3, 4, 5, 6 };
    CHECK(!s.pass->Commit(s.list.data(), 10, twice));
    CHECK(twice[1] == 0);
}
