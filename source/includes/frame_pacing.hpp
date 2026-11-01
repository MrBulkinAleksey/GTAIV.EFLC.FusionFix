#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fusionfix::framepacing
{
    // Bounded p90 of observed handoff -> GPU-ready intervals, NOT GPU execution time.
    // Callers must observe a completed GPU fence/readback, not a CPU submission acknowledgement.
    // Polling makes each observation an upper bound. Missing/failed observations are not samples.
    // A run of misses cannot grow the budget: expiry uses only actually observed completions.
    class ReadinessBudget
    {
    public:
        static constexpr std::size_t Capacity = 64;
        static constexpr std::size_t Warmup = 8;
        static constexpr double MaximumSampleMs = 100.0;
        static constexpr double MaximumBudgetMs = 50.0;

        void Reset() { *this = {}; }

        bool Observe(double ms)
        {
            if (!std::isfinite(ms) || ms <= 0.0 || ms > MaximumSampleMs)
                return false;
            samples_[next_] = ms;
            next_ = (next_ + 1) % Capacity;
            count_ = std::min(count_ + 1, Capacity);
            if (count_ < Warmup)
                return true;

            auto sorted = samples_;
            std::sort(sorted.begin(), sorted.begin() + count_);
            const auto rank = (count_ * 9 + 9) / 10; // nearest-rank p90
            const double target = std::clamp(sorted[rank - 1], 0.25, MaximumBudgetMs);
            const double hysteresis = std::max(0.25, budgetMs_ * 0.1);
            if (budgetMs_ == 0.0 || target > budgetMs_ + hysteresis)
            {
                budgetMs_ = target;
                lowerSamples_ = 0;
            }
            else if (target < budgetMs_ - hysteresis)
            {
                // A single quiet frame must not remove the reserve; grow quickly, shrink slowly.
                if (++lowerSamples_ >= Warmup)
                {
                    budgetMs_ = std::max(target, budgetMs_ - std::max(0.25, budgetMs_ * 0.1));
                    lowerSamples_ = 0;
                }
            }
            else
                lowerSamples_ = 0;
            return true;
        }

        double BudgetMs() const { return budgetMs_; } // 0 until enough actual observations
        std::size_t SampleCount() const { return count_; }

    private:
        std::array<double, Capacity> samples_{};
        std::size_t next_ = 0, count_ = 0, lowerSamples_ = 0;
        double budgetMs_ = 0.0;
    };

    // A texture left over from an earlier frame is not a valid HUD mask.
    constexpr bool HasFreshHudLess(bool captured, bool targetAvailable, std::uint32_t capturedFrame, std::uint32_t frame)
    {
        return captured && targetAvailable && capturedFrame == frame;
    }

    enum class Action { Wait, Generated, DropGenerated, Real };

    struct PairDeadlines
    {
        double generatedMs = 0.0;
        double generatedExpiryMs = 0.0;
        double realMs = 0.0;
        bool valid = false;

        // Construct once at handoff, never rebase on generated readiness/presentation.
        // Without a measured budget the optional frame may use the entire safe interval.
        // Adaptive pacing narrows that interval to p90 + polling margin, never extends the real deadline.
        static PairDeadlines Make(double frameMs, double generatedAim, double realAim, double readinessBudgetMs = 0.0)
        {
            PairDeadlines result;
            if (!std::isfinite(frameMs) || frameMs <= 0.0 || frameMs >= 250.0 ||
                !std::isfinite(generatedAim) || !std::isfinite(realAim) ||
                generatedAim < 0.0 || realAim <= generatedAim || realAim > 1.0 ||
                !std::isfinite(readinessBudgetMs) || readinessBudgetMs < 0.0)
                return result;
            result.generatedMs = std::clamp(frameMs * generatedAim, 0.0, 50.0);
            result.realMs = std::clamp(frameMs * realAim, 0.0, 50.0);
            const double minimumGapMs = std::clamp(frameMs * 0.1, 1.0, 5.0);
            const double latest = std::max(0.0, result.realMs - minimumGapMs);
            result.generatedExpiryMs = readinessBudgetMs > 0.0 ?
                std::min(latest, std::max(result.generatedMs + 0.5, readinessBudgetMs + 0.5)) : latest;
            result.valid = true;
            return result;
        }

        Action Choose(double elapsedMs, bool generatedPending, bool generatedDue, bool realDue, bool gpuReady) const
        {
            // A clock discontinuity/invalid plan fails open to the real frame, never an unbounded wait.
            if (!valid || !std::isfinite(elapsedMs) || elapsedMs < 0.0 || realDue || elapsedMs >= realMs)
                return Action::Real;
            if (!generatedPending)
                return Action::Wait;
            if (elapsedMs >= generatedExpiryMs)
                return Action::DropGenerated;
            return generatedDue && gpuReady ? Action::Generated : Action::Wait;
        }
    };
}
