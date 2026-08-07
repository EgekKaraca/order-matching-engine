// The Sharpe ratio haircut.
//
// A Sharpe estimated from a short sample is biased upward, and the bias grows
// with the number of variants tried before this one was picked. Reporting the
// raw number with a written caveat leaves the reader to do a correction they
// have no way to compute; applying the published one puts the honesty in the
// number instead of beside it.
#include "sim/deflated_sharpe.hpp"

#include <gtest/gtest.h>

namespace sim {
namespace {

TEST(DeflatedSharpe, ShrinksTowardZero) {
    const double raw = 2.0;
    const double deflated = deflate_sharpe(raw, 1000, 1, 0.0, 0.0);
    EXPECT_LT(deflated, raw);
    EXPECT_GT(deflated, 0.0);
}

TEST(DeflatedSharpe, MoreTrialsMeansAHarsherHaircut) {
    // Trying a hundred variants and reporting the best is a different claim
    // from testing one, and the number should say so.
    const double one = deflate_sharpe(2.0, 1000, 1, 0.0, 0.0);
    const double hundred = deflate_sharpe(2.0, 1000, 100, 0.0, 0.0);
    EXPECT_LT(hundred, one);
}

TEST(DeflatedSharpe, MoreSamplesMeansALighterHaircut) {
    const double few = deflate_sharpe(2.0, 100, 1, 0.0, 0.0);
    const double many = deflate_sharpe(2.0, 10000, 1, 0.0, 0.0);
    EXPECT_GT(many, few);
}

TEST(DeflatedSharpe, NegativeSharpeGetsMoreNegative) {
    // Our own maker has a negative Sharpe. A haircut that pushed it toward
    // zero from below would make a losing strategy look better, which is the
    // exact failure this function exists to prevent.
    const double deflated = deflate_sharpe(-70.0, 3600, 1, 0.0, 0.0);
    EXPECT_LT(deflated, -70.0);
}

TEST(DeflatedSharpe, FatTailsMakeTheHaircutHarsher) {
    // Excess kurtosis inflates the standard error of a Sharpe estimate.
    const double normal = deflate_sharpe(2.0, 1000, 1, 0.0, 0.0);
    const double fat = deflate_sharpe(2.0, 1000, 1, 0.0, 5.0);
    EXPECT_LT(fat, normal);
}

TEST(DeflatedSharpe, DegenerateInputsDoNotProduceNonsense) {
    EXPECT_DOUBLE_EQ(deflate_sharpe(2.0, 0, 1, 0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(deflate_sharpe(2.0, 1, 1, 0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(deflate_sharpe(0.0, 1000, 1, 0.0, 0.0), 0.0);
}

}  // namespace
}  // namespace sim
