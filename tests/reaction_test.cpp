// The market-impact model, in its simplest defensible form.
//
// A strictly impact-free replay assumes nobody notices your order. That is
// false, and its falseness grows with your size relative to the level. The
// smallest honest step is to model ONE mechanism: your resting order is
// visible, and a participant about to join behind it may not bother.
//
// NoReaction reproduces the previous behaviour exactly, so every existing
// result remains the baseline rather than being silently replaced.
#include "sim/reaction.hpp"

#include <gtest/gtest.h>

#include <cstdint>

#include "match/types.hpp"
#include "sim/types.hpp"

namespace sim {
namespace {

using match::Price;
using match::Side;

TEST(Reaction, NoReactionNeverCrowdsOut) {
    NoReaction model;
    for (int i = 0; i < 1000; ++i) {
        EXPECT_FALSE(model.crowds_out(Time{i}, Side::Buy, Price{100}, 500, 1000));
    }
    EXPECT_STREQ(model.name(), "none (impact-free)");
}

TEST(Reaction, CrowdOutIsRareForATinyOrder) {
    // One share against a thousand: nobody notices, and a model that says they
    // do is inventing impact rather than modelling it.
    CrowdOutReaction model{1.0, 42};
    std::size_t reactions = 0;
    for (int i = 0; i < 10000; ++i) {
        if (model.crowds_out(Time{i}, Side::Buy, Price{100}, 1, 1000)) ++reactions;
    }
    EXPECT_LT(reactions, 200u) << "a 0.1% order provoked a reaction more than 2% of the time";
}

TEST(Reaction, CrowdOutIsCommonForADominatingOrder) {
    // Half the level is conspicuous — the regime the size sweep already flags
    // as straining the impact-free assumption.
    CrowdOutReaction model{1.0, 42};
    std::size_t reactions = 0;
    for (int i = 0; i < 10000; ++i) {
        if (model.crowds_out(Time{i}, Side::Buy, Price{100}, 500, 1000)) ++reactions;
    }
    EXPECT_GT(reactions, 3000u);
    EXPECT_LT(reactions, 7000u);
}

TEST(Reaction, IntensityScalesTheEffect) {
    CrowdOutReaction gentle{0.2, 7};
    CrowdOutReaction fierce{1.0, 7};
    std::size_t gentle_count = 0;
    std::size_t fierce_count = 0;
    for (int i = 0; i < 10000; ++i) {
        if (gentle.crowds_out(Time{i}, Side::Buy, Price{100}, 300, 1000)) ++gentle_count;
        if (fierce.crowds_out(Time{i}, Side::Buy, Price{100}, 300, 1000)) ++fierce_count;
    }
    EXPECT_LT(gentle_count, fierce_count);
}

TEST(Reaction, IsSeededAndReproducible) {
    // A backtest whose answer moves between runs cannot be argued with.
    CrowdOutReaction a{0.5, 99};
    CrowdOutReaction b{0.5, 99};
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(a.crowds_out(Time{i}, Side::Buy, Price{100}, 200, 1000),
                  b.crowds_out(Time{i}, Side::Buy, Price{100}, 200, 1000));
    }
}

TEST(Reaction, AnEmptyLevelCannotBeCrowded) {
    // Guards a division by zero, and the semantics: there is nobody there to
    // react to you.
    CrowdOutReaction model{1.0, 1};
    EXPECT_FALSE(model.crowds_out(Time{0}, Side::Buy, Price{100}, 100, 0));
}

TEST(Reaction, ZeroIntensityMatchesNoReaction) {
    // The continuity check: the impact model at zero strength must BE the
    // impact-free model, or the sensitivity sweep has no valid baseline.
    CrowdOutReaction model{0.0, 5};
    for (int i = 0; i < 1000; ++i) {
        EXPECT_FALSE(model.crowds_out(Time{i}, Side::Buy, Price{100}, 900, 1000));
    }
}

}  // namespace
}  // namespace sim
