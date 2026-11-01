// The deferred pair deadline: bounded p90 of measured handoff -> GPU-ready (not GPU execution),
// hysteresis, stale generated frame drop, and fail-open to the real frame.
#include "check.hpp"
#include "includes/frame_pacing.hpp"

using fusionfix::framepacing::Action;
using fusionfix::framepacing::PairDeadlines;
using fusionfix::framepacing::ReadinessBudget;

TEST(BudgetIgnoresInvalidAndOutlierSamples)
{
    ReadinessBudget budget;
    for (int i = 0; i < 8; ++i)
    {
        CHECK(!budget.Observe(-1.0));
        CHECK(!budget.Observe(0.0));
        CHECK(!budget.Observe(1e12));
        CHECK(!budget.Observe(-100.0));
        CHECK(budget.Observe(4.0 + i));
    }
    CHECK(budget.SampleCount() == 8);
    CHECK(budget.BudgetMs() > 0.0); // warmup complete after 8 valid samples
    CHECK(budget.BudgetMs() <= ReadinessBudget::MaximumBudgetMs);
    CHECK(!budget.Observe(ReadinessBudget::MaximumSampleMs + 1.0));
    CHECK(budget.SampleCount() == 8);
}

TEST(BudgetReachesP90WithFastUpSlowDown)
{
    ReadinessBudget budget;
    for (int i = 0; i < 7; ++i)
        budget.Observe(2.0);
    CHECK(budget.BudgetMs() == 0.0);
    budget.Observe(8.0); // warmup complete, p90 lands on the largest
    CHECK(budget.BudgetMs() >= 8.0 - 1e-9);

    budget.Observe(2.0); // p90 still the 8.0 while it is in the window
    CHECK(budget.BudgetMs() >= 8.0 - 1e-9);
    for (int i = 0; i < 7; ++i)
        budget.Observe(2.0); // below the band, but slow-down needs a run of 8
    CHECK(budget.BudgetMs() >= 8.0 - 1e-9);
    budget.Observe(2.0);
    CHECK(budget.BudgetMs() < 8.0);
    const double shrunk = budget.BudgetMs();
    CHECK(shrunk > 2.0); // one step, not straight to the target

    for (int i = 0; i < 8; ++i)
        budget.Observe(30.0);
    CHECK(budget.BudgetMs() >= 30.0 - 1e-9); // growth is immediate again
}

TEST(BudgetIsBoundedAndResettable)
{
    ReadinessBudget budget;
    for (int i = 0; i < 64; ++i)
        budget.Observe(100.0); // accepted sample, capped budget
    CHECK(budget.BudgetMs() <= ReadinessBudget::MaximumBudgetMs);
    budget.Reset();
    CHECK(budget.BudgetMs() == 0.0);
    CHECK(budget.SampleCount() == 0);
}

TEST(BudgetWrapsTheRingWithoutLoss)
{
    ReadinessBudget budget;
    for (int i = 0; i < 100; ++i)
        budget.Observe(1.0 + (i % 10));
    CHECK(budget.SampleCount() == ReadinessBudget::Capacity);
    CHECK(budget.BudgetMs() > 0.0);
}

TEST(DeadlineRejectsBadInputs)
{
    CHECK(!PairDeadlines::Make(0.0, 0.2, 0.7).valid);
    CHECK(!PairDeadlines::Make(-16.0, 0.2, 0.7).valid);
    CHECK(!PairDeadlines::Make(250.0, 0.2, 0.7).valid);
    CHECK(!PairDeadlines::Make(16.0, 0.8, 0.7).valid);   // aims inverted
    CHECK(!PairDeadlines::Make(16.0, 0.2, 1.2).valid);   // real aim above the frame
    CHECK(!PairDeadlines::Make(16.0, 0.2, 0.7, -1.0).valid);
    CHECK(!PairDeadlines::Make(16.0, 0.2, 0.7, std::nan("")).valid);
    CHECK(PairDeadlines::Make(16.0, 0.2, 0.7, 0.0).valid);
}

TEST(DeadlineNeverExtendsTheRealOneWithAdaptiveBudget)
{
    const auto fixed = PairDeadlines::Make(16.0, 0.2, 0.7);
    const auto adaptive = PairDeadlines::Make(16.0, 0.2, 0.7, 2.0);
    CHECK(fixed.valid && adaptive.valid);
    CHECK(adaptive.generatedExpiryMs <= fixed.generatedExpiryMs + 1e-9);
    CHECK(adaptive.realMs == fixed.realMs);
    // Never below the generated aim plus the polling margin
    CHECK(adaptive.generatedExpiryMs >= adaptive.generatedMs);
}

TEST(AdaptiveBudgetNarrowsTheGeneratedWindow)
{
    const auto adaptive = PairDeadlines::Make(16.0, 0.2, 0.7, 2.0);
    // generated due at 3.2; the expiry floor keeps it eligible through 3.7
    CHECK(adaptive.Choose(3.2, true, true, false, true) == Action::Generated);
    // generated due but the GPU frame is not ready: wait, do not take on CPU time
    CHECK(adaptive.Choose(3.2, true, true, false, false) == Action::Wait);
}

TEST(StaleGeneratedDropsBeforeTheRealDeadline)
{
    const auto adaptive = PairDeadlines::Make(16.0, 0.2, 0.7, 2.0);
    CHECK(adaptive.generatedExpiryMs < adaptive.realMs);
    const double inside = adaptive.generatedExpiryMs + 0.1;
    CHECK(inside < adaptive.realMs);
    CHECK(adaptive.Choose(inside, true, true, false, true) == Action::DropGenerated);
}

TEST(FailsOpenToTheRealFrame)
{
    const auto d = PairDeadlines::Make(16.0, 0.2, 0.7);
    CHECK(d.Choose(11.3, true, true, false, true) == Action::Real);     // elapsed past the real deadline (11.2)
    CHECK(d.Choose(1.0, true, false, true, false) == Action::Real);     // real is due on its own signal
    CHECK(d.Choose(-5.0, true, true, false, true) == Action::Real);     // clock discontinuity
    CHECK(d.Choose(std::nan(""), true, true, false, true) == Action::Real);
    const PairDeadlines invalid;
    CHECK(invalid.Choose(1.0, true, true, false, true) == Action::Real);

    // No indefinite postponement: once the generated frame dropped, the next wait ends at the real deadline
    CHECK(d.Choose(d.realMs - 0.001, false, false, false, false) == Action::Wait);
    CHECK(d.Choose(d.realMs, false, false, false, false) == Action::Real);
}

TEST(WithoutBudgetTheGeneratedWindowIsTheWholeSafeInterval)
{
    const auto d = PairDeadlines::Make(16.0, 0.2, 0.7);
    CHECK(d.generatedExpiryMs > d.generatedMs);
    CHECK(d.Choose(d.generatedMs, true, true, false, true) == Action::Generated);
    CHECK(d.Choose(d.generatedExpiryMs - 0.01, true, true, false, true) == Action::Generated);
    CHECK(d.Choose(d.generatedExpiryMs, true, false, false, true) == Action::DropGenerated);
}

TEST(ReadinessRejectsNonFiniteSamplesWithoutChangingBudget)
{
    ReadinessBudget budget;
    for (int i = 0; i < 8; ++i)
        CHECK(budget.Observe(6.0));
    const auto before = budget.BudgetMs();
    CHECK(!budget.Observe(std::nan("")));
    CHECK(!budget.Observe(INFINITY));
    CHECK(!budget.Observe(-INFINITY));
    CHECK(budget.SampleCount() == 8);
    CHECK(budget.BudgetMs() == before);
}

TEST(ReadinessBudgetUsesHandoffTimeRatherThanLateness)
{
    ReadinessBudget budget;
    for (int i = 0; i < 8; ++i)
        budget.Observe(7.0); // GPU completion observed seven ms after handoff
    const auto plan = PairDeadlines::Make(20.0, 0.2, 0.8, budget.BudgetMs());
    CHECK(plan.generatedMs == 4.0);
    CHECK(plan.generatedExpiryMs == 7.5);
    CHECK(plan.Choose(7.0, true, true, false, true) == Action::Generated);
    CHECK(plan.Choose(7.5, true, true, false, true) == Action::DropGenerated);
    CHECK(plan.realMs == 16.0);
}

TEST(RealDeadlineSurvivesEveryGeneratedOutcome)
{
    const auto plan = PairDeadlines::Make(20.0, 0.2, 0.8, 6.0);
    for (const bool ready : {false, true})
    {
        CHECK(plan.Choose(6.5, true, true, false, ready) == Action::DropGenerated);
        CHECK(plan.Choose(15.999, false, false, false, ready) == Action::Wait);
        CHECK(plan.Choose(16.0, false, false, false, ready) == Action::Real);
        CHECK(plan.Choose(16.0, true, true, false, ready) == Action::Real);
    }
    CHECK(plan.realMs == 16.0);
}

TEST(DeadlineBoundsHoldAcrossSupportedFrameIntervals)
{
    for (double frame : {0.1, 5.0, 16.667, 33.333, 100.0, 249.0})
        for (double budget : {0.0, 0.25, 2.0, 50.0, 100.0})
        {
            const auto plan = PairDeadlines::Make(frame, 0.2, 0.7, budget);
            CHECK(plan.valid);
            CHECK(plan.generatedExpiryMs >= 0.0);
            CHECK(plan.generatedExpiryMs < plan.realMs);
            CHECK(plan.realMs <= 50.0);
            CHECK(plan.Choose(plan.realMs, true, true, false, true) == Action::Real);
        }
}

TEST(HudLessMustBeCapturedForTheSameFrameOnEveryPresentationPath)
{
    using fusionfix::framepacing::HasFreshHudLess;
    CHECK(HasFreshHudLess(true, true, 42, 42));
    CHECK(!HasFreshHudLess(false, true, 42, 42));
    CHECK(!HasFreshHudLess(true, false, 42, 42));
    CHECK(!HasFreshHudLess(true, true, 41, 42));
    CHECK(!HasFreshHudLess(true, true, 43, 42));
    CHECK(HasFreshHudLess(true, true, 0, 0)); // frame counter wrap
    CHECK(!HasFreshHudLess(false, true, 0, 0));
}

TEST(P90UsesNearestRankRatherThanTheMaximumOrMean)
{
    ReadinessBudget budget;
    for (int i = 1; i <= 10; ++i)
        CHECK(budget.Observe(double(i)));
    CHECK(budget.SampleCount() == 10);
    CHECK(budget.BudgetMs() == 9.0);
    for (double invalid : {0.0, -1.0, 100.001, 1e12, double(INFINITY), double(-INFINITY), std::nan("")})
        CHECK(!budget.Observe(invalid));
    CHECK(budget.SampleCount() == 10);
    CHECK(budget.BudgetMs() == 9.0);
}

TEST(UnreadyGeneratedNeverWinsAtAnyEligibleTime)
{
    const auto plan = PairDeadlines::Make(20.0, 0.2, 0.8, 7.0);
    for (double elapsed : {4.0, 4.5, 6.0, 7.499})
    {
        CHECK(plan.Choose(elapsed, true, true, false, false) == Action::Wait);
        CHECK(plan.Choose(elapsed, true, true, false, true) == Action::Generated);
    }
    CHECK(plan.Choose(7.5, true, true, false, false) == Action::DropGenerated);
    CHECK(plan.Choose(16.0, true, true, false, false) == Action::Real);
}
