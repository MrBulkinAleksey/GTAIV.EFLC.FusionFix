#include "check.hpp"
#include "../source/VehicleMotion.hpp"
#include <array>
#include <limits>

namespace
{
    using Matrix = std::array<float, 16>;
    Matrix Identity()
    {
        return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    }
    Matrix Spin(int axle, float angle)
    {
        auto m = Identity();
        int a = (axle + 1) % 3, b = (axle + 2) % 3;
        m[a*4+a] = m[b*4+b] = std::cos(angle);
        m[a*4+b] = std::sin(angle);
        m[b*4+a] = -std::sin(angle);
        return m;
    }
    bool Near(float a, float b) { return std::abs(a-b) < 1e-5f; }
    void CheckBasis(const Matrix& a, const Matrix& b)
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                CHECK(Near(a[r*4+c], b[r*4+c]));
    }
}

TEST(DamageHistoryRejectsBothToggleDirectionsAndRebinding)
{
    int texture = 0, replacement = 0;
    using fusionfix::motion::DamageHistoryMatches;
    CHECK(DamageHistoryMatches(false, false, nullptr, nullptr, 0, 0));
    CHECK(DamageHistoryMatches(true, true, &texture, &texture, 2, 2));
    CHECK(!DamageHistoryMatches(true, false, &texture, nullptr, 2, 0));
    CHECK(!DamageHistoryMatches(false, true, nullptr, &texture, 0, 2));
    CHECK(!DamageHistoryMatches(true, true, &replacement, &texture, 2, 2));
    CHECK(!DamageHistoryMatches(true, true, &texture, &texture, 3, 2));
    CHECK(!DamageHistoryMatches(true, true, nullptr, nullptr, 2, 2));
    CHECK(!DamageHistoryMatches(true, true, &texture, &texture, 0, 0));
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(!DamageHistoryMatches(true, true, &texture, &texture, inf, inf));
}

TEST(WheelWorldUsesOneWheelTransformAndOnlyWorldTranslation)
{
    auto world = Identity(), wheel = Spin(2, 0.5f);
    world[0] = 0.00001f; world[1] = 42; world[5] = -9; // stale/locally overridden constants
    world[12] = 100; world[13] = 200; world[14] = 300;
    wheel[12] = 1; wheel[13] = -2; wheel[14] = 3;
    fusionfix::motion::WheelWorld(world.data(), wheel.data(), world.data());
    CheckBasis(world, wheel);
    CHECK(world[12] == 101 && world[13] == 198 && world[14] == 303);
    CHECK(wheel[12] == 1 && wheel[13] == -2 && wheel[14] == 3);
    CHECK(world[3] == 0 && world[7] == 0 && world[11] == 0 && world[15] == 1);
}
