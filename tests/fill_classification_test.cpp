// Adverse vs benign fills.
//
// arXiv:2409.12721 defines a fill as adverse when the order is "picked off":
// immediately after execution the new position is out of the money. Splitting
// PnL along that line separates two economically different things a single
// blended number hides — the spread you earned, and the cost of the fills you
// were given precisely because they were about to be wrong.
#include "sim/fill_classification.hpp"

#include "sim/types.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include "match/types.hpp"

#include <gtest/gtest.h>

namespace sim {
namespace {

using match::Price;
using match::Quantity;
using match::Side;

Fill maker_fill(std::int64_t at_ns, Side side, std::int64_t price, std::uint32_t qty) {
    Fill fill;
    fill.time = Time{at_ns};
    fill.side = side;
    fill.price = Price{price};
    fill.quantity = Quantity{qty};
    fill.maker = true;
    return fill;
}

TEST(FillClassification, ABuyIsAdverseWhenThePriceFallsAfterwards) {
    const std::vector<Fill> fills{maker_fill(0, Side::Buy, 100, 10)};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},                 // mid 100 at the fill
        {Time{seconds(1).count}, 180},  // mid 90 a second later
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.adverse_count, 1u);
    EXPECT_EQ(split.benign_count, 0u);
    EXPECT_DOUBLE_EQ(split.adverse_fraction(), 1.0);
    EXPECT_DOUBLE_EQ(split.adverse_pnl, -100.0);  // 10 shares x 10 down
}

TEST(FillClassification, ABuyIsBenignWhenThePriceRisesAfterwards) {
    const std::vector<Fill> fills{maker_fill(0, Side::Buy, 100, 10)};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},
        {Time{seconds(1).count}, 220},
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.benign_count, 1u);
    EXPECT_EQ(split.adverse_count, 0u);
    EXPECT_DOUBLE_EQ(split.benign_pnl, 100.0);
}

TEST(FillClassification, ASellIsAdverseWhenThePriceRises) {
    // The mirror case. Getting one side right and the other wrong is a real
    // and repeatedly-observed defect class, so both directions are pinned.
    const std::vector<Fill> fills{maker_fill(0, Side::Sell, 100, 10)};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},
        {Time{seconds(1).count}, 220},
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.adverse_count, 1u);
    EXPECT_DOUBLE_EQ(split.adverse_pnl, -100.0);
}

TEST(FillClassification, AnUnchangedPriceIsBenignNotAdverse) {
    // A fill that cost nothing was not picked off. Classifying a zero mark-out
    // as adverse would inflate the adverse count with every quiet fill.
    const std::vector<Fill> fills{maker_fill(0, Side::Buy, 100, 10)};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},
        {Time{seconds(1).count}, 200},
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.adverse_count, 0u);
    EXPECT_EQ(split.benign_count, 1u);
    EXPECT_DOUBLE_EQ(split.benign_pnl, 0.0);
}

TEST(FillClassification, TakerFillsAreExcluded) {
    // Being picked off is something that happens to a resting order. A taker
    // chose its moment, so counting it here would blame the market for the
    // strategy's own decision.
    Fill taker = maker_fill(0, Side::Buy, 100, 10);
    taker.maker = false;
    const std::vector<Fill> fills{taker};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},
        {Time{seconds(1).count}, 180},
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.adverse_count, 0u);
    EXPECT_EQ(split.benign_count, 0u);
    EXPECT_DOUBLE_EQ(split.adverse_fraction(), 0.0);
}

TEST(FillClassification, AnEmptyFillListIsNotADivideByZero) {
    const std::vector<Fill> fills;
    const std::vector<std::pair<Time, std::int64_t>> mids{{Time{0}, 200}};
    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_DOUBLE_EQ(split.adverse_fraction(), 0.0);
}

}  // namespace
}  // namespace sim
