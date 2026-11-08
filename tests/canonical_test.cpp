#include "match/canonical.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "match/report.hpp"
#include "match/types.hpp"

#include <gtest/gtest.h>

namespace match {
namespace {

std::string text_of(std::vector<Report> reports) {
    std::ostringstream out;
    canonical::write(out, reports);
    return out.str();
}

// The six line formats, verbatim from the benchmark's docs/METHODOLOGY.md.
// These are the bytes the published SHA-256 is taken over, so each one is
// pinned literally rather than round-tripped through a parser.

TEST(CanonicalLine, OrderAck) {
    EXPECT_EQ(canonical::line(Report::order_ack(SeqNum{12}, OrderId{99}, Side::Sell, Price{33500},
                                                Quantity{100})),
              "0,12,1,99,33500,100");
}

TEST(CanonicalLine, TradeCarriesMakerPriceAndAggressorSeq) {
    EXPECT_EQ(canonical::line(
                  Report::trade(SeqNum{12}, Price{33500}, Quantity{40}, OrderId{7}, OrderId{99})),
              "1,12,33500,40,7,99");
}

TEST(CanonicalLine, CancelAckOmitsQuantity) {
    EXPECT_EQ(canonical::line(Report::cancel_ack(SeqNum{5}, OrderId{7}, Side::Buy, Price{33499})),
              "2,5,0,7,33499");
}

TEST(CanonicalLine, ModifyAck) {
    EXPECT_EQ(canonical::line(Report::modify_ack(SeqNum{5}, OrderId{7}, Side::Buy, Price{33501},
                                                 Quantity{250})),
              "3,5,0,7,33501,250");
}

TEST(CanonicalLine, RejectsCarryIdentityOnly) {
    EXPECT_EQ(canonical::line(Report::cancel_reject(SeqNum{5}, OrderId{7})), "4,5,7");
    EXPECT_EQ(canonical::line(Report::modify_reject(SeqNum{5}, OrderId{7})), "5,5,7");
}

TEST(CanonicalLine, HandlesNegativeAndExtremePrices) {
    // Ticks are signed, and the conformance battery includes a price near the
    // representational ceiling.
    EXPECT_EQ(canonical::line(
                  Report::order_ack(SeqNum{0}, OrderId{1}, Side::Buy, Price{-250}, Quantity{1})),
              "0,0,0,1,-250,1");

    const auto huge = std::numeric_limits<std::int64_t>::max();
    EXPECT_EQ(canonical::line(
                  Report::order_ack(SeqNum{0}, OrderId{1}, Side::Buy, Price{huge}, Quantity{1})),
              "0,0,0,1,9223372036854775807,1");
}

TEST(CanonicalText, SortsBySeqThenType) {
    std::vector<Report> reports{
        Report::cancel_reject(SeqNum{2}, OrderId{9}),
        Report::trade(SeqNum{1}, Price{10}, Quantity{5}, OrderId{1}, OrderId{2}),
        Report::order_ack(SeqNum{1}, OrderId{2}, Side::Buy, Price{10}, Quantity{5}),
    };
    EXPECT_EQ(text_of(reports),
              "0,1,0,2,10,5\n"
              "1,1,10,5,1,2\n"
              "4,2,9");
}

TEST(CanonicalText, IsIndependentOfEmissionOrderWithinAMessage) {
    // The whole point of the sort: an engine that emits its ModifyAck before
    // the modify's trades and one that emits it after must serialise the same.
    const auto ack = Report::modify_ack(SeqNum{4}, OrderId{3}, Side::Buy, Price{20}, Quantity{8});
    const auto fill = Report::trade(SeqNum{4}, Price{20}, Quantity{8}, OrderId{1}, OrderId{3});

    EXPECT_EQ(text_of({ack, fill}), text_of({fill, ack}));
}

TEST(CanonicalText, PreservesTradeOrderWithinAMessage) {
    // The sort is STABLE, so fills of one aggressor keep the order the engine
    // emitted them in. That order is the engine's price-time priority claim —
    // if the sort were not stable, walking the book in the wrong sequence
    // would serialise identically and the oracle would miss it.
    std::vector<Report> reports{
        Report::trade(SeqNum{9}, Price{10}, Quantity{1}, OrderId{100}, OrderId{5}),
        Report::trade(SeqNum{9}, Price{11}, Quantity{2}, OrderId{200}, OrderId{5}),
        Report::trade(SeqNum{9}, Price{12}, Quantity{3}, OrderId{300}, OrderId{5}),
    };
    EXPECT_EQ(text_of(reports),
              "1,9,10,1,100,5\n"
              "1,9,11,2,200,5\n"
              "1,9,12,3,300,5");
}

TEST(CanonicalText, HasNoTrailingNewline) {
    // A trailing newline changes the hash and nothing else — exactly the kind
    // of defect that costs a day.
    const auto text = text_of({Report::cancel_reject(SeqNum{1}, OrderId{1})});
    EXPECT_EQ(text, "4,1,1");
    EXPECT_NE(text.back(), '\n');
}

TEST(CanonicalText, EmptyStreamIsEmpty) { EXPECT_EQ(text_of({}), ""); }

TEST(CanonicalText, SurvivesABufferFlushBoundary) {
    // The writer flushes in 32 KiB chunks; a line straddling that boundary
    // must not gain or lose a separator.
    std::vector<Report> reports;
    reports.reserve(20000);
    for (std::uint64_t i = 0; i < 20000; ++i) {
        reports.push_back(Report::cancel_reject(SeqNum{i}, OrderId{i}));
    }
    const auto text = text_of(reports);

    const auto newlines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    EXPECT_EQ(newlines, reports.size() - 1);
    EXPECT_TRUE(text.starts_with("4,0,0\n4,1,1\n"));
    EXPECT_TRUE(text.ends_with("\n4,19999,19999"));
}

}  // namespace
}  // namespace match
