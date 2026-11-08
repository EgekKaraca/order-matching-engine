// Unit tests for the reference engine.
//
// The named cases mirror the benchmark's 34-case conformance battery
// (docs/CONFORMANCE.md), which was reverse-engineered from latent bugs found
// across a 247-engine survey. Every one of them is a hard invariant that any
// correct price-time-priority book must satisfy, so they are worth pinning
// locally even though the gate itself only runs on Linux — a failure here is a
// failure there, discovered days earlier.
#include "match/map_book.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "match/canonical.hpp"
#include "match/report.hpp"
#include "match/types.hpp"

#include <gtest/gtest.h>

namespace match {
namespace {

class BookTest : public ::testing::Test {
  protected:
    struct Sink {
        std::vector<Report>* out;
        void operator()(const Report& r) const { out->push_back(r); }
    };

    std::vector<Report> reports_;
    MapBook<Sink> book_{Sink{&reports_}};
    std::uint64_t next_seq_ = 0;

    // Every helper validates afterwards, so any test that drives the book also
    // asserts that no operation left it internally inconsistent.
    SeqNum add(std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty,
               TimeInForce tif = TimeInForce::Gtc) {
        const SeqNum seq{next_seq_++};
        book_.on_new_order(Order{OrderId{id}, seq, Price{price}, Quantity{qty}, side, tif});
        EXPECT_TRUE(book_.validate()) << "book invalid after new order " << id;
        return seq;
    }

    SeqNum cancel(std::uint64_t id) {
        const SeqNum seq{next_seq_++};
        book_.on_cancel(seq, OrderId{id});
        EXPECT_TRUE(book_.validate()) << "book invalid after cancel " << id;
        return seq;
    }

    SeqNum modify(std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty) {
        const SeqNum seq{next_seq_++};
        book_.on_modify(seq, OrderId{id}, side, Price{price}, Quantity{qty});
        EXPECT_TRUE(book_.validate()) << "book invalid after modify " << id;
        return seq;
    }

    // The reports since the last clear, as canonical lines — comparing the
    // serialised form keeps the assertions readable and tests the same bytes
    // the oracle hashes.
    std::vector<std::string> lines() {
        std::vector<std::string> out;
        out.reserve(reports_.size());
        for (const auto& r : reports_) out.push_back(canonical::line(r));
        return out;
    }

    void clear() { reports_.clear(); }
};

using Lines = std::vector<std::string>;

// --- resting and the empty book ---------------------------------------------

TEST_F(BookTest, EmptyBookReportsSentinels) {
    EXPECT_EQ(book_.best_bid(), Price::no_bid());
    EXPECT_EQ(book_.best_ask(), Price::no_ask());
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 0u);
    EXPECT_TRUE(book_.empty());
}

TEST_F(BookTest, AnUncrossedOrderJustRests) {
    add(1, Side::Buy, 100, 50);
    EXPECT_EQ(lines(), Lines{"0,0,0,1,100,50"});
    EXPECT_EQ(book_.best_bid(), Price{100});
    EXPECT_EQ(book_.best_ask(), Price::no_ask());
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 50u);
}

TEST_F(BookTest, DepthAggregatesAcrossOrdersAtALevel) {
    add(1, Side::Buy, 100, 50);
    add(2, Side::Buy, 100, 30);
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 80u);
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Sell), 0u);  // wrong side, not a match
}

// --- matching ----------------------------------------------------------------

TEST_F(BookTest, ExactFillLeavesNothingResting) {
    add(1, Side::Buy, 100, 50);
    clear();
    add(2, Side::Sell, 100, 50);

    EXPECT_EQ(lines(), (Lines{"0,1,1,2,100,50", "1,1,100,50,1,2"}));
    EXPECT_TRUE(book_.empty());
    EXPECT_EQ(book_.resting_count(), 0u);
}

TEST_F(BookTest, TradePrintsAtTheMakersPriceWhenTheBuyerLifts) {
    // A buy limit of 105 lifting an ask resting at 100 must print at 100, not
    // at its own more aggressive limit. Several real engines mis-price this.
    add(1, Side::Sell, 100, 50);
    clear();
    add(2, Side::Buy, 105, 50);

    EXPECT_EQ(lines(), (Lines{"0,1,0,2,105,50", "1,1,100,50,1,2"}));
}

TEST_F(BookTest, TradePrintsAtTheMakersPriceWhenTheSellerHits) {
    // The other direction: several engines get exactly one side right.
    add(1, Side::Buy, 100, 50);
    clear();
    add(2, Side::Sell, 95, 50);

    EXPECT_EQ(lines(), (Lines{"0,1,1,2,95,50", "1,1,100,50,1,2"}));
}

TEST_F(BookTest, FifoWithinAPriceLevel) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 100, 10);
    add(3, Side::Buy, 100, 10);
    clear();
    add(4, Side::Sell, 100, 25);

    // Earliest first, and the third order is left with 5.
    EXPECT_EQ(lines(),
              (Lines{"0,3,1,4,100,25", "1,3,100,10,1,4", "1,3,100,10,2,4", "1,3,100,5,3,4"}));
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 5u);
}

TEST_F(BookTest, SweepsBestPriceFirstAcrossLevels) {
    add(1, Side::Sell, 102, 10);
    add(2, Side::Sell, 100, 10);
    add(3, Side::Sell, 101, 10);
    clear();
    add(4, Side::Buy, 102, 30);

    // 100, then 101, then 102 — cheapest liquidity first, regardless of when
    // each order arrived.
    EXPECT_EQ(lines(),
              (Lines{"0,3,0,4,102,30", "1,3,100,10,2,4", "1,3,101,10,3,4", "1,3,102,10,1,4"}));
    EXPECT_TRUE(book_.empty());
}

TEST_F(BookTest, MarketableLimitStopsAtItsOwnPrice) {
    add(1, Side::Sell, 100, 10);
    add(2, Side::Sell, 101, 10);
    clear();
    add(3, Side::Buy, 100, 30);

    // Fills the 100 and rests 20 at 100 — it does not walk through its own
    // limit to reach the 101.
    EXPECT_EQ(lines(), (Lines{"0,2,0,3,100,30", "1,2,100,10,1,3"}));
    EXPECT_EQ(book_.best_bid(), Price{100});
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 20u);
    EXPECT_EQ(book_.best_ask(), Price{101});
}

TEST_F(BookTest, SweepResidualBecomesTheTouchAndIsThenFillable) {
    add(1, Side::Sell, 100, 10);
    add(2, Side::Buy, 100, 30);  // fills 10, rests 20
    ASSERT_EQ(book_.best_bid(), Price{100});
    clear();
    add(3, Side::Sell, 100, 20);

    EXPECT_EQ(lines(), (Lines{"0,2,1,3,100,20", "1,2,100,20,2,3"}));
    EXPECT_TRUE(book_.empty());
}

TEST_F(BookTest, APartiallyFilledMakerKeepsFrontOfQueueAcrossCalls) {
    add(1, Side::Buy, 100, 100);
    add(2, Side::Buy, 100, 100);
    add(3, Side::Sell, 100, 40);  // eats 40 of order 1
    clear();
    add(4, Side::Sell, 100, 60);

    // Order 1 still leads with its remaining 60 — a partial fill must not send
    // it to the back of its own queue.
    EXPECT_EQ(lines(), (Lines{"0,3,1,4,100,60", "1,3,100,60,1,4"}));
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 100u);
}

// --- IOC ---------------------------------------------------------------------

TEST_F(BookTest, IocResidualIsCancelledNotRested) {
    add(1, Side::Sell, 100, 10);
    clear();
    add(2, Side::Buy, 100, 30, TimeInForce::Ioc);

    // OrderAck, one fill, and a CancelAck for the 20 that could not fill.
    EXPECT_EQ(lines(), (Lines{"0,1,0,2,100,30", "1,1,100,10,1,2", "2,1,0,2,100"}));
    EXPECT_EQ(book_.best_bid(), Price::no_bid());
    EXPECT_TRUE(book_.empty());
}

TEST_F(BookTest, IocThatFillsCompletelyEmitsNoCancel) {
    add(1, Side::Sell, 100, 30);
    clear();
    add(2, Side::Buy, 100, 30, TimeInForce::Ioc);

    EXPECT_EQ(lines(), (Lines{"0,1,0,2,100,30", "1,1,100,30,1,2"}));
}

TEST_F(BookTest, IocAgainstAnEmptyBookCancelsInFull) {
    add(1, Side::Buy, 100, 30, TimeInForce::Ioc);
    EXPECT_EQ(lines(), (Lines{"0,0,0,1,100,30", "2,0,0,1,100"}));
    EXPECT_TRUE(book_.empty());
}

// --- cancel ------------------------------------------------------------------

TEST_F(BookTest, CancelHeadThenSweepKeepsTheOrdersBehind) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 100, 10);
    add(3, Side::Buy, 100, 10);
    clear();
    cancel(1);
    add(4, Side::Sell, 100, 20);

    EXPECT_EQ(lines(),
              (Lines{"2,3,0,1,100", "0,4,1,4,100,20", "1,4,100,10,2,4", "1,4,100,10,3,4"}));
    EXPECT_TRUE(book_.empty());
}

TEST_F(BookTest, CancelMiddleThenSweepKeepsTheOrdersBehind) {
    // The classic corruption: unlinking a middle node and losing the tail.
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 100, 10);
    add(3, Side::Buy, 100, 10);
    clear();
    cancel(2);
    add(4, Side::Sell, 100, 20);

    EXPECT_EQ(lines(),
              (Lines{"2,3,0,2,100", "0,4,1,4,100,20", "1,4,100,10,1,4", "1,4,100,10,3,4"}));
}

TEST_F(BookTest, DeepFifoSurvivesScatteredCancels) {
    for (std::uint64_t i = 1; i <= 10; ++i) add(i, Side::Buy, 100, 10);
    cancel(2);
    cancel(5);
    cancel(6);
    cancel(9);
    clear();
    add(99, Side::Sell, 100, 60);

    // The six survivors fill in their original arrival order.
    EXPECT_EQ(lines(),
              (Lines{"0,14,1,99,100,60", "1,14,100,10,1,99", "1,14,100,10,3,99", "1,14,100,10,4,99",
                     "1,14,100,10,7,99", "1,14,100,10,8,99", "1,14,100,10,10,99"}));
    EXPECT_TRUE(book_.empty());
}

TEST_F(BookTest, CancellingTheBestLevelDoesNotLeaveAPhantomPrice) {
    // No follow-on sweep to let a stale best-price cache self-heal: a direct
    // query at this instant is what catches it.
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 99, 10);
    cancel(1);

    EXPECT_EQ(book_.best_bid(), Price{99});
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 0u);
}

TEST_F(BookTest, CancelOfANeverIssuedIdIsRejected) {
    clear();
    cancel(42);
    EXPECT_EQ(lines(), Lines{"4,0,42"});
}

TEST_F(BookTest, CancelAfterAFullFillIsRejected) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Sell, 100, 10);
    clear();
    cancel(1);
    EXPECT_EQ(lines(), Lines{"4,2,1"});
}

TEST_F(BookTest, DuplicateCancelIsRejected) {
    // ~2% of the canonical workload's cancels are duplicates.
    add(1, Side::Buy, 100, 10);
    cancel(1);
    clear();
    cancel(1);
    EXPECT_EQ(lines(), Lines{"4,2,1"});
}

TEST_F(BookTest, PartialFillResidualRemainsCancellable) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Sell, 100, 4);
    clear();
    cancel(1);
    EXPECT_EQ(lines(), Lines{"2,2,0,1,100"});
    EXPECT_TRUE(book_.empty());
}

// --- id reuse ----------------------------------------------------------------

TEST_F(BookTest, AnIdIsReusableAfterCancel) {
    add(1, Side::Buy, 100, 10);
    cancel(1);
    clear();
    add(1, Side::Buy, 101, 20);
    EXPECT_EQ(lines(), Lines{"0,2,0,1,101,20"});
    EXPECT_EQ(book_.best_bid(), Price{101});
}

TEST_F(BookTest, AnIdIsReusableAfterAFullFill) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Sell, 100, 10);
    clear();
    add(1, Side::Buy, 99, 5);
    EXPECT_EQ(lines(), Lines{"0,2,0,1,99,5"});
    EXPECT_EQ(book_.depth_at(Price{99}, Side::Buy), 5u);
}

// --- modify ------------------------------------------------------------------

TEST_F(BookTest, ModifyRepricesAndLosesPriority) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 100, 10);
    clear();
    modify(1, Side::Buy, 101, 10);

    EXPECT_EQ(lines(), Lines{"3,2,0,1,101,10"});
    EXPECT_EQ(book_.best_bid(), Price{101});
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 10u);  // the sibling survives
}

TEST_F(BookTest, ModifyDoesNotLoseASameLevelSibling) {
    // A reprice that double-decrements the shared level orphans the sibling.
    // The report stream can look fine while the book is wrong, which is why
    // this is checked through the queries too.
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 100, 40);
    modify(1, Side::Buy, 99, 10);

    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 40u);
    EXPECT_EQ(book_.depth_at(Price{99}, Side::Buy), 10u);
    EXPECT_EQ(book_.resting_count(), 2u);
}

TEST_F(BookTest, ModifyIntoCrossMatchesRatherThanRestingCrossed) {
    add(1, Side::Sell, 101, 10);
    add(2, Side::Buy, 99, 10);
    clear();
    modify(2, Side::Buy, 101, 10);

    EXPECT_EQ(lines(), (Lines{"1,2,101,10,1,2", "3,2,0,2,101,10"}));
    EXPECT_TRUE(book_.empty());
}

TEST_F(BookTest, CrossingModifyStillFillsAtTheMakersPrice) {
    // The modify reprices PAST the maker, so a fill at its own limit would be
    // visible — that is the point of repricing past rather than onto it.
    add(1, Side::Sell, 100, 10);
    add(2, Side::Buy, 90, 10);
    clear();
    modify(2, Side::Buy, 105, 10);

    EXPECT_EQ(lines(), (Lines{"1,2,100,10,1,2", "3,2,0,2,105,10"}));
}

TEST_F(BookTest, CrossingModifyFillsAtTheMakersPriceOnTheSellSide) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Sell, 110, 10);
    clear();
    modify(2, Side::Sell, 95, 10);

    EXPECT_EQ(lines(), (Lines{"1,2,100,10,1,2", "3,2,1,2,95,10"}));
}

TEST_F(BookTest, ModifyOfAPartiallyFilledResidualRepricesTheRemainder) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Sell, 100, 6);  // leaves 4 resting
    clear();
    modify(1, Side::Buy, 99, 4);

    EXPECT_EQ(lines(), Lines{"3,2,0,1,99,4"});
    EXPECT_EQ(book_.depth_at(Price{99}, Side::Buy), 4u);
}

TEST_F(BookTest, ModifyAckEchoesTheRequestEvenWhenTheReinsertFills) {
    // The ack carries the requested new price and quantity, not what is left
    // resting afterwards. Verified against the published canonical output.
    add(1, Side::Sell, 100, 4);
    add(2, Side::Buy, 90, 10);
    clear();
    modify(2, Side::Buy, 100, 10);

    EXPECT_EQ(lines(), (Lines{"1,2,100,4,1,2", "3,2,0,2,100,10"}));
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 6u);
}

TEST_F(BookTest, ModifyOfANeverIssuedIdIsRejected) {
    clear();
    modify(42, Side::Buy, 100, 10);
    EXPECT_EQ(lines(), Lines{"5,0,42"});
}

TEST_F(BookTest, ModifyAfterAFullFillIsRejected) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Sell, 100, 10);
    clear();
    modify(1, Side::Buy, 99, 10);
    EXPECT_EQ(lines(), Lines{"5,2,1"});
}

TEST_F(BookTest, ModifyQuantityIncreaseGoesToTheBackOfTheQueue) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 100, 10);
    modify(1, Side::Buy, 100, 20);
    clear();
    add(3, Side::Sell, 100, 30);

    // Order 2 now leads. (Every modify in the canonical workload is a reprice
    // or a quantity increase, and both lose priority under every convention
    // engines use, so this is not one of the untested convention cases.)
    EXPECT_EQ(lines(), (Lines{"0,3,1,3,100,30", "1,3,100,10,2,3", "1,3,100,20,1,3"}));
}

// --- in-place size reduction -------------------------------------------------
// Not reachable from the benchmark ABI; this is the production semantic that
// LOBSTER's partial cancels and executions need.

TEST_F(BookTest, ReduceKeepsQueuePosition) {
    add(1, Side::Buy, 100, 50);
    add(2, Side::Buy, 100, 50);
    EXPECT_EQ(book_.reduce(OrderId{1}, Quantity{30}), Quantity{30});
    EXPECT_TRUE(book_.validate());
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 70u);
    clear();

    // Order 1 still leads with its remaining 20 — a decrease does not send an
    // order to the back of its own queue. This is the whole reason reduce()
    // exists rather than reusing modify.
    add(3, Side::Sell, 100, 20);
    EXPECT_EQ(lines(), (Lines{"0,2,1,3,100,20", "1,2,100,20,1,3"}));
}

TEST_F(BookTest, ReduceToZeroRemovesTheOrder) {
    add(1, Side::Buy, 100, 50);
    EXPECT_EQ(book_.reduce(OrderId{1}, Quantity{50}), Quantity{50});
    EXPECT_TRUE(book_.validate());
    EXPECT_TRUE(book_.empty());
    EXPECT_EQ(book_.best_bid(), Price::no_bid());  // no phantom level left behind
}

TEST_F(BookTest, ReduceClampsToWhatIsResting) {
    add(1, Side::Buy, 100, 50);
    EXPECT_EQ(book_.reduce(OrderId{1}, Quantity{999}), Quantity{50});
    EXPECT_TRUE(book_.empty());
}

TEST_F(BookTest, ReduceOfANonRestingOrderRemovesNothing) {
    clear();
    EXPECT_EQ(book_.reduce(OrderId{42}, Quantity{10}), Quantity{0});
    EXPECT_TRUE(book_.validate());
    EXPECT_TRUE(lines().empty()) << "reduce() emits no report — the caller reports";
}

// --- level-granularity consumption -------------------------------------------
// The fallback for replaying liquidity whose arrival was never captured.

TEST_F(BookTest, ConsumeAtTakesFromTheFrontOfTheQueue) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 100, 10);
    add(3, Side::Buy, 100, 10);

    EXPECT_EQ(book_.consume_at(Side::Buy, Price{100}, Quantity{15}), Quantity{15});
    EXPECT_TRUE(book_.validate());
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 15u);
    clear();

    // Order 1 is gone entirely, order 2 is left with 5 and still leads.
    add(4, Side::Sell, 100, 5);
    EXPECT_EQ(lines(), (Lines{"0,3,1,4,100,5", "1,3,100,5,2,4"}));
}

TEST_F(BookTest, ConsumeAtClampsToTheLevelAndEmptiesIt) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 99, 10);

    EXPECT_EQ(book_.consume_at(Side::Buy, Price{100}, Quantity{999}), Quantity{10});
    EXPECT_TRUE(book_.validate());
    EXPECT_EQ(book_.best_bid(), Price{99}) << "the emptied level must not linger";
}

TEST_F(BookTest, ConsumeAtAnAbsentLevelTakesNothing) {
    add(1, Side::Buy, 100, 10);
    EXPECT_EQ(book_.consume_at(Side::Buy, Price{50}, Quantity{10}), Quantity{0});
    EXPECT_EQ(book_.consume_at(Side::Sell, Price{100}, Quantity{10}), Quantity{0})
        << "the wrong side is not the same level";
    EXPECT_EQ(book_.depth_at(Price{100}, Side::Buy), 10u);
}

TEST_F(BookTest, TopLevelsReturnsBestFirstAndAggregates) {
    add(1, Side::Buy, 100, 10);
    add(2, Side::Buy, 102, 20);
    add(3, Side::Buy, 101, 30);
    add(4, Side::Buy, 102, 5);  // same level as order 2

    MapBook<Sink>::LevelView levels[4]{};
    ASSERT_EQ(book_.top_levels(Side::Buy, levels, 4), 3u);

    EXPECT_EQ(levels[0].price, Price{102});
    EXPECT_EQ(levels[0].quantity, 25u);  // 20 + 5
    EXPECT_EQ(levels[1].price, Price{101});
    EXPECT_EQ(levels[2].price, Price{100});
}

TEST_F(BookTest, TopLevelsStopsAtTheRequestedDepth) {
    for (std::uint64_t i = 0; i < 10; ++i) {
        add(i + 1, Side::Sell, 100 + std::int64_t(i), 10);
    }
    MapBook<Sink>::LevelView levels[3]{};
    ASSERT_EQ(book_.top_levels(Side::Sell, levels, 3), 3u);
    EXPECT_EQ(levels[0].price, Price{100});  // asks: lowest first
    EXPECT_EQ(levels[2].price, Price{102});
}

// --- price domain ------------------------------------------------------------

TEST_F(BookTest, HandlesPricesNearTheRepresentationalCeiling) {
    constexpr std::int64_t high = (std::int64_t{1} << 62);
    add(1, Side::Sell, high, 10);
    clear();
    add(2, Side::Buy, high, 10);

    EXPECT_EQ(lines(), (Lines{"0,1,0,2,4611686018427387904,10", "1,1,4611686018427387904,10,1,2"}));
    EXPECT_TRUE(book_.empty());
}

TEST_F(BookTest, HandlesNegativeTicks) {
    add(1, Side::Buy, -50, 10);
    EXPECT_EQ(book_.best_bid(), Price{-50});
    EXPECT_EQ(book_.depth_at(Price{-50}, Side::Buy), 10u);
}

// --- conservation ------------------------------------------------------------

TEST_F(BookTest, QuantityIsConservedAcrossAMixedSequence) {
    // The conservation law, stated precisely:
    //
    //     submitted == 2 * filled + resting + withdrawn
    //
    // The factor of two is the part that is easy to get wrong, and the spec
    // this project was written from states it without one. A trade of size Q
    // is a single report, but it retires Q from the maker AND Q from the
    // taker — two orders' worth of submitted quantity per fill. Writing the
    // identity without the factor makes it fail on any book that actually
    // trades, which is how this test found its own bug.
    add(1, Side::Buy, 100, 100);
    add(2, Side::Buy, 99, 100);
    add(3, Side::Sell, 101, 100);
    add(4, Side::Sell, 100, 150);  // fills 100 against order 1, rests 50
    cancel(2);
    modify(3, Side::Sell, 99, 100);  // crosses nothing (order 2 is gone), rests at 99

    AggregateQuantity filled = 0;
    AggregateQuantity withdrawn = 0;
    for (const auto& r : reports_) {
        if (r.type == ReportType::Trade) filled += r.quantity.units;
        if (r.type == ReportType::CancelAck) withdrawn += r.quantity.units;
    }
    const AggregateQuantity resting =
        book_.depth_at(Price{100}, Side::Sell) + book_.depth_at(Price{99}, Side::Sell) +
        book_.depth_at(Price{101}, Side::Sell) + book_.depth_at(Price{100}, Side::Buy) +
        book_.depth_at(Price{99}, Side::Buy);

    EXPECT_EQ(filled, 100u);
    EXPECT_EQ(withdrawn, 100u);
    EXPECT_EQ(resting, 150u);

    constexpr AggregateQuantity submitted = 100 + 100 + 100 + 150;
    EXPECT_EQ(2 * filled + withdrawn + resting, submitted);
}

}  // namespace
}  // namespace match
