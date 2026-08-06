#include "adapters/lobster.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "match/types.hpp"

namespace lobster {
namespace {

TEST(ParseMessage, ARealNewLimitOrderLine) {
    // A line in the shape LOBSTER actually ships: seconds-after-midnight with
    // nanosecond precision, price in dollars x 10000.
    Message m;
    ASSERT_TRUE(parse_message("34200.017459617,1,11885113,21,5853300,-1", m));

    EXPECT_EQ(m.time_ns, 34200017459617);
    EXPECT_EQ(m.type, EventType::NewLimitOrder);
    EXPECT_EQ(m.order_id, 11885113u);
    EXPECT_EQ(m.size, 21u);
    EXPECT_EQ(m.price, 5853300);  // $585.33
    EXPECT_EQ(m.side, match::Side::Sell);
    EXPECT_TRUE(m.touches_visible_book());
}

TEST(ParseMessage, BuyDirectionIsPlusOne) {
    Message m;
    ASSERT_TRUE(parse_message("34200.189607670,1,11885975,20,5852900,1", m));
    EXPECT_EQ(m.side, match::Side::Buy);
}

TEST(ParseMessage, TimeIsExactAtNanosecondPrecision) {
    // The whole trading day in nanoseconds needs 46 bits. It fits in a double
    // today, which is exactly why parsing it as one is a trap worth avoiding.
    Message m;
    ASSERT_TRUE(parse_message("57599.999999999,1,1,1,1,1", m));
    EXPECT_EQ(m.time_ns, 57599999999999);
}

TEST(ParseMessage, ShortAndAbsentFractionsArePadded) {
    Message m;
    ASSERT_TRUE(parse_message("34200.5,1,1,1,1,1", m));
    EXPECT_EQ(m.time_ns, 34200500000000);

    ASSERT_TRUE(parse_message("34200,1,1,1,1,1", m));
    EXPECT_EQ(m.time_ns, 34200000000000);
}

TEST(ParseMessage, ClassifiesEveryEventType) {
    const struct {
        int code;
        EventType type;
        bool visible;
    } cases[] = {
        {1, EventType::NewLimitOrder, true},   {2, EventType::PartialCancel, true},
        {3, EventType::Delete, true},          {4, EventType::VisibleExecution, true},
        {5, EventType::HiddenExecution, false}, {6, EventType::CrossTrade, false},
        {7, EventType::Halt, false},
    };
    for (const auto& c : cases) {
        Message m;
        const std::string line = "34200.0," + std::to_string(c.code) + ",1,1,1,1";
        ASSERT_TRUE(parse_message(line, m)) << line;
        EXPECT_EQ(m.type, c.type);
        // Hidden executions, crosses and halts leave the displayed book alone.
        // Applying a hidden execution would double-count liquidity that was
        // never shown.
        EXPECT_EQ(m.touches_visible_book(), c.visible) << "type " << c.code;
    }
}

TEST(ParseMessage, RejectsMalformedLines) {
    Message m;
    EXPECT_FALSE(parse_message("", m));
    EXPECT_FALSE(parse_message("34200.0,1,1,1,1", m)) << "five columns, not six";
    EXPECT_FALSE(parse_message("34200.0,9,1,1,1,1", m)) << "type out of range";
    EXPECT_FALSE(parse_message("34200.0,1,1,1,1,0", m)) << "direction must be +/-1";
    EXPECT_FALSE(parse_message("not-a-time,1,1,1,1,1", m));
    EXPECT_FALSE(parse_message("34200.0,1,1,1,12.5,1", m)) << "price is an integer count";
}

TEST(ParseMessage, ToleratesTrailingWhitespaceAndCarriageReturns) {
    Message m;
    ASSERT_TRUE(parse_message("34200.0,1,7,20,5853300,-1\r\n", m));
    EXPECT_EQ(m.order_id, 7u);
    EXPECT_EQ(m.size, 20u);
}

TEST(ParseOrderbookRow, ReadsLevelsInAskBidOrder) {
    // Column order per level is: ask price, ask size, bid price, bid size.
    std::int64_t out[8]{};
    ASSERT_TRUE(parse_orderbook_row("5853300,21,5852900,20,5853400,50,5852800,100", out, 2));

    EXPECT_EQ(out[0], 5853300);  // level 1 ask price
    EXPECT_EQ(out[1], 21);       // level 1 ask size
    EXPECT_EQ(out[2], 5852900);  // level 1 bid price
    EXPECT_EQ(out[3], 20);       // level 1 bid size
    EXPECT_EQ(out[4], 5853400);
    EXPECT_EQ(out[7], 100);
}

TEST(ParseOrderbookRow, ReadsThePaddingForAnUnoccupiedLevel) {
    // LOBSTER pads a level that does not exist rather than shortening the row.
    std::int64_t out[4]{};
    ASSERT_TRUE(parse_orderbook_row("9999999999,0,-9999999999,0", out, 1));
    EXPECT_EQ(out[0], kEmptyAskPrice);
    EXPECT_EQ(out[1], 0);
    EXPECT_EQ(out[2], kEmptyBidPrice);
    EXPECT_EQ(out[3], 0);
}

TEST(ParseOrderbookRow, RejectsAMismatchedLevelCount) {
    // Pairing a level-10 orderbook file with a level-50 message file is a
    // documented way to get a silently wrong answer, so arity is checked.
    std::int64_t out[8]{};
    EXPECT_FALSE(parse_orderbook_row("1,2,3,4,5,6,7,8", out, 1)) << "too many columns";
    EXPECT_FALSE(parse_orderbook_row("1,2,3,4", out, 2)) << "too few columns";
}

}  // namespace
}  // namespace lobster
