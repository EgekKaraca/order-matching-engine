#include "match/types.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace match {
namespace {

TEST(Price, OrdersByTick) {
    EXPECT_LT(Price{10}, Price{11});
    EXPECT_EQ(Price{10}, Price{10});
    EXPECT_GT(Price{-1}, Price{-2});
}

TEST(Price, EmptySideSentinelsMatchTheAbi) {
    // The harness ABI specifies these exact values for an empty side, and the
    // state audit compares against them literally.
    EXPECT_EQ(Price::no_bid().ticks, std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(Price::no_ask().ticks, std::numeric_limits<std::int64_t>::max());

    // Any real price beats an empty book on its own side, which is what makes
    // the sentinels safe to compare against directly.
    EXPECT_TRUE(is_better(Price{0}, Price::no_bid(), Side::Buy));
    EXPECT_TRUE(is_better(Price{0}, Price::no_ask(), Side::Sell));
}

TEST(Side, OppositeIsAnInvolution) {
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
    EXPECT_EQ(opposite(Side::Sell), Side::Buy);
    EXPECT_EQ(opposite(opposite(Side::Buy)), Side::Buy);
}

TEST(Side, EncodingMatchesTheAbi) {
    // 0 = buy, 1 = sell, so the adapter can cast rather than translate.
    EXPECT_EQ(static_cast<std::uint8_t>(Side::Buy), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(Side::Sell), 1);
}

TEST(PricePriority, BuyWantsHighSellWantsLow) {
    EXPECT_TRUE(is_better(Price{11}, Price{10}, Side::Buy));
    EXPECT_FALSE(is_better(Price{10}, Price{11}, Side::Buy));

    EXPECT_TRUE(is_better(Price{10}, Price{11}, Side::Sell));
    EXPECT_FALSE(is_better(Price{11}, Price{10}, Side::Sell));

    // Equal prices are not better than each other — that is what leaves time
    // priority to break the tie.
    EXPECT_FALSE(is_better(Price{10}, Price{10}, Side::Buy));
    EXPECT_FALSE(is_better(Price{10}, Price{10}, Side::Sell));
}

TEST(Crossing, AtTheTouchCrosses) {
    // A buy at 10 against a resting ask at 10 trades: equal prices cross.
    EXPECT_TRUE(crosses(Price{10}, Price{10}, Side::Buy));
    EXPECT_TRUE(crosses(Price{10}, Price{10}, Side::Sell));
}

TEST(Crossing, RespectsTheLimit) {
    // A buy limit of 10 will not pay 11.
    EXPECT_TRUE(crosses(Price{10}, Price{9}, Side::Buy));
    EXPECT_FALSE(crosses(Price{10}, Price{11}, Side::Buy));

    // A sell limit of 10 will not accept 9.
    EXPECT_TRUE(crosses(Price{10}, Price{11}, Side::Sell));
    EXPECT_FALSE(crosses(Price{10}, Price{9}, Side::Sell));
}

TEST(Quantity, Arithmetic) {
    EXPECT_EQ(Quantity{3} + Quantity{4}, Quantity{7});
    EXPECT_EQ(Quantity{7} - Quantity{4}, Quantity{3});
    EXPECT_LT(Quantity{3}, Quantity{4});
}

TEST(Quantity, ZeroIsFalsey) {
    EXPECT_FALSE(static_cast<bool>(Quantity{0}));
    EXPECT_TRUE(static_cast<bool>(Quantity{1}));
}

TEST(Ids, AreDistinctTypesAndRoundTrip) {
    static_assert(!std::is_same_v<OrderId, SeqNum>);
    EXPECT_EQ(value_of(OrderId{42}), 42u);
    EXPECT_EQ(value_of(SeqNum{7}), 7u);
}

TEST(Order, ComparesFieldwise) {
    const Order a{OrderId{1}, SeqNum{2}, Price{3}, Quantity{4}, Side::Buy, TimeInForce::Gtc};
    Order b = a;
    EXPECT_EQ(a, b);
    b.tif = TimeInForce::Ioc;
    EXPECT_NE(a, b);
}

}  // namespace
}  // namespace match
