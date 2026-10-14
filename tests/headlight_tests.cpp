// The headlight shadow selector (StableHeadlightSelector): which two headlights get a shadow, a frame after it
// sees them, and how long it keeps them.
#include "check.hpp"
#include "nightshadows/StableHeadlightSelector.hpp"

using namespace fusionfix::shadows;

static HeadlightCandidate Beam(std::uintptr_t id, float metres, bool player = false)
{
    HeadlightCandidate c;
    c.identity = id;
    c.geometry.distanceSquared = metres * metres;
    c.playerHeadlight = player;
    return c;
}

static FrameContext Frame(uint64_t frame, uint32_t ms, std::uintptr_t session = 1)
{
    return { frame, ms, session, false, 0 };
}

static bool Active(const StableHeadlightSelector& s, std::uintptr_t id)
{
    const auto ids = s.ActiveIdentities();
    return ids[0] == id || ids[1] == id;
}

TEST(HeadlightGeometryTellsABeamAimedAtThePlayer)
{
    const Vec3 player{ 0, 10, 0 }, light{ 0, 0, 0 };
    const Vec3 ahead{ 0, 1, 0 }, aside{ 1, 0.5f, 0 }, behind{ 0, -1, 0 };
    CHECK(EvaluateGeometry(player, light, &ahead).aimedAtPlayer);
    CHECK(!EvaluateGeometry(player, light, &aside).aimedAtPlayer); // 63 degrees off, past the 45 degree cone
    CHECK(!EvaluateGeometry(player, light, &behind).aimedAtPlayer);
    const auto unknown = EvaluateGeometry(player, light);
    CHECK(!unknown.directionKnown);
    CHECK(unknown.distanceSquared == 100.0f);
}

TEST(HeadlightsChosenTheFrameAfterAndNearestFirst)
{
    StableHeadlightSelector s;
    s.BeginFrame(Frame(1, 0));
    s.Consider(Beam(1, 10.0f));
    s.Consider(Beam(2, 20.0f));
    s.Consider(Beam(3, 5.0f));
    CHECK(!Active(s, 3)); // nothing yet in the frame they are seen
    s.BeginFrame(Frame(2, 16));
    CHECK(Active(s, 3));
    CHECK(Active(s, 1));
    CHECK(!Active(s, 2));
}

TEST(HeadlightsOfThePlayerGoFirst)
{
    StableHeadlightSelector s;
    s.BeginFrame(Frame(1, 0));
    s.Consider(Beam(1, 5.0f));
    s.Consider(Beam(2, 6.0f));
    s.Consider(Beam(9, 30.0f, true));
    s.BeginFrame(Frame(2, 16));
    CHECK(Active(s, 9));
    CHECK(Active(s, 1));
    CHECK(!Active(s, 2));
}

TEST(HeadlightHeldHalfASecondThenGivenToAMuchNearerOne)
{
    StableHeadlightSelector s;
    s.BeginFrame(Frame(1, 0));
    s.Consider(Beam(1, 10.0f));
    s.Consider(Beam(2, 12.0f));
    s.BeginFrame(Frame(2, 16));
    uint64_t frame = 2;
    uint32_t ms = 16;
    auto step = [&](bool withNear)
    {
        s.Consider(Beam(1, 10.0f));
        s.Consider(Beam(2, 12.0f));
        if (withNear)
            s.Consider(Beam(3, 1.0f));
        ms += 16;
        s.BeginFrame(Frame(++frame, ms));
    };
    step(true); // 32 ms: both held
    CHECK(Active(s, 1) && Active(s, 2) && !Active(s, 3));
    while (ms < 600)
        step(true);
    CHECK(Active(s, 3)); // past the hold, it takes the farther one's slot
    CHECK(Active(s, 1));
    CHECK(!Active(s, 2));
}

TEST(HeadlightGoneForMoreThanTwoFramesIsDropped)
{
    StableHeadlightSelector s;
    s.BeginFrame(Frame(1, 0));
    s.Consider(Beam(1, 10.0f));
    s.BeginFrame(Frame(2, 16));
    CHECK(Active(s, 1));
    s.BeginFrame(Frame(3, 32)); // not seen in frame 2
    CHECK(Active(s, 1));
    s.BeginFrame(Frame(4, 48));
    CHECK(Active(s, 1));
    s.BeginFrame(Frame(5, 64));
    CHECK(!Active(s, 1));
}

TEST(HeadlightsStartOverOnANewSession)
{
    StableHeadlightSelector s;
    s.BeginFrame(Frame(1, 0));
    s.Consider(Beam(1, 10.0f));
    s.BeginFrame(Frame(2, 16));
    CHECK(Active(s, 1));
    s.Consider(Beam(1, 10.0f));
    s.BeginFrame(Frame(3, 32, 2)); // a game loaded
    CHECK(!Active(s, 1));
}
