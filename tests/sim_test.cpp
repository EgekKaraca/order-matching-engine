// Tests for the injection layer. The properties worth pinning here are the
// ones that, if wrong, produce a plausible-looking backtest that is simply
// false: latency being real, fills being earned rather than assumed, and the
// strategy being unable to see the future.
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstdint>
#include <vector>

#include "match/types.hpp"
#include "sim/fill_model.hpp"
#include "sim/latency.hpp"
#include "sim/performance.hpp"
#include "sim/portfolio.hpp"
#include "sim/simulator.hpp"
#include "sim/strategies/fixed_offset_maker.hpp"
#include "sim/strategy.hpp"
#include "sim/types.hpp"

namespace sim {
namespace {

using match::Price;
using match::Quantity;
using match::Side;

// A strategy that posts one order the first time it is called and then does
// nothing, so a test can control exactly what is in the book.
class OneShot : public Strategy {
public:
    OneShot(Side side, std::int64_t price, std::uint32_t qty)
        : side_(side), price_(price), qty_(qty) {}

    void on_book(const BookView& book, OrderGate& gate) override {
        views.push_back(book);
        if (posted_) return;
        posted_ = true;
        id = gate.submit(side_, Price{price_}, Quantity{qty_});
    }

    void on_fill(const Fill& fill, const BookView&, OrderGate&) override {
        seen_fills.push_back(fill);
    }

    const char* name() const override { return "one-shot"; }

    match::OrderId id{};
    std::vector<BookView> views;
    std::vector<Fill> seen_fills;

private:
    Side side_;
    std::int64_t price_;
    std::uint32_t qty_;
    bool posted_ = false;
};

MarketEvent add(std::int64_t t, std::uint64_t id, Side side, std::int64_t price,
                std::uint32_t qty) {
    return MarketEvent{Time{t},        MarketEventType::Add, match::OrderId{id},
                       Price{price}, Quantity{qty},        side};
}

MarketEvent execute(std::int64_t t, std::uint64_t maker_id, Side maker_side, std::int64_t price,
                    std::uint32_t qty) {
    return MarketEvent{Time{t},      MarketEventType::Execute, match::OrderId{maker_id},
                       Price{price}, Quantity{qty},            maker_side};
}

MarketEvent cancel(std::int64_t t, std::uint64_t id, Side side, std::int64_t price,
                   std::uint32_t qty) {
    return MarketEvent{Time{t},      MarketEventType::Delete, match::OrderId{id},
                       Price{price}, Quantity{qty},           side};
}

// --- latency is real ---------------------------------------------------------

TEST(Latency, OrderDelayKeepsAnOrderOutOfTheBookUntilItArrives) {
    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ConstantDelay order{millis(10)};
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    // Two-sided book so the strategy is called at all.
    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 10));

    // An execution one millisecond later: the strategy's order was decided at
    // t=1 but does not arrive until t=1+10ms, so it cannot be filled here.
    sim.on_market_event(execute(millis(1).count, 1, Side::Buy, 99, 10));
    EXPECT_EQ(sim.stats().strategy_fills, 0u) << "filled before its order arrived";

    sim.on_market_event(add(millis(20).count, 3, Side::Sell, 101, 10));
    EXPECT_EQ(sim.stats().strategy_submits, 1u);
}

TEST(Latency, FeedDelayMakesTheStrategySeeAStaleBook) {
    OneShot strategy(Side::Buy, 100, 50);
    ConstantDelay feed{millis(5)};
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 10));
    // Nothing is visible yet: every snapshot is still in flight.
    EXPECT_TRUE(strategy.views.empty());

    sim.on_market_event(add(millis(6).count, 3, Side::Sell, 102, 10));
    ASSERT_FALSE(strategy.views.empty());

    // What it saw is older than the clock it saw it at — by construction.
    const BookView& view = strategy.views.front();
    EXPECT_LT(view.as_of(), view.now());
    EXPECT_GE(view.staleness().count, 0);
}

TEST(Latency, ZeroDelayIsNamedForWhatItIs) {
    ZeroDelay zero;
    EXPECT_STREQ(zero.name(), "zero (unphysical)");
    EXPECT_EQ(zero.sample(Time{0}).count, 0);
}

TEST(Latency, LognormalIsSeededAndReproducible) {
    LognormalDelay a{micros(500), 0.5, 42};
    LognormalDelay b{micros(500), 0.5, 42};
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.sample(Time{i}).count, b.sample(Time{i}).count);
    }
}

TEST(Latency, LognormalRespectsItsFloor) {
    LognormalDelay model{micros(500), 2.0, 7, micros(100)};
    for (int i = 0; i < 1000; ++i) {
        EXPECT_GE(model.sample(Time{i}).count, micros(100).count);
    }
}

// --- fills are earned, not assumed -------------------------------------------

TEST(Injection, AnOrderBehindTheQueueDoesNotFillUntilTheQueueDrains) {
    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    // 200 shares are already resting at the strategy's price when it arrives.
    sim.seed_level(Side::Buy, Price{100}, Quantity{200});
    sim.on_market_event(add(0, 1, Side::Sell, 101, 10));
    sim.on_market_event(add(1, 2, Side::Buy, 99, 10));
    ASSERT_EQ(sim.stats().strategy_submits, 1u);

    // A seller takes 100 at the strategy's price. All of it goes to the queue
    // ahead — this is the entire thesis in one assertion. A naive fill model
    // would have filled the strategy here.
    sim.on_market_event(execute(2, 999, Side::Buy, 100, 100));
    EXPECT_EQ(sim.stats().strategy_fills, 0u);

    // Another 100 finishes draining the queue ahead...
    sim.on_market_event(execute(3, 999, Side::Buy, 100, 100));
    EXPECT_EQ(sim.stats().strategy_fills, 0u);

    // ...and only now does the strategy get filled.
    sim.on_market_event(execute(4, 999, Side::Buy, 100, 50));
    EXPECT_EQ(sim.stats().strategy_fills, 1u);
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_EQ(sim.fills().front().quantity, Quantity{50});
    EXPECT_EQ(sim.fills().front().queue_ahead_at_entry, 200u)
        << "the size ahead at entry is the number the naive model assumes is zero";
}

TEST(Injection, RecordsNormalisedQueuePositionAtEntry) {
    // 200 shares rest ahead when the strategy's 50-share order arrives, and
    // nothing joins behind it. Queue position is therefore 1.0 — the very back.
    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.seed_level(Side::Buy, Price{100}, Quantity{200});
    sim.on_market_event(add(0, 1, Side::Sell, 101, 10));
    sim.on_market_event(add(1, 2, Side::Buy, 99, 10));
    sim.on_market_event(execute(2, 999, Side::Buy, 100, 250));

    ASSERT_EQ(sim.fills().size(), 1u);
    const Fill& fill = sim.fills().front();
    EXPECT_EQ(fill.queue_ahead_at_entry, 200u);
    EXPECT_EQ(fill.queue_behind_at_entry, 0u);
    EXPECT_TRUE(fill.has_queue_position());
    EXPECT_DOUBLE_EQ(fill.normalised_queue_position(), 1.0);
}

TEST(Injection, NormalisedQueuePositionIsZeroAtTheFrontOfAnEmptyLevel) {
    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Sell, 101, 10));
    sim.on_market_event(add(1, 2, Side::Buy, 99, 10));
    sim.on_market_event(execute(2, 999, Side::Buy, 100, 50));

    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_EQ(sim.fills().front().queue_ahead_at_entry, 0u);
    EXPECT_DOUBLE_EQ(sim.fills().front().normalised_queue_position(), 0.0);
}

TEST(Injection, QueuePositionIsDestroyedByCancellationToo) {
    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 100, 200));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 10));
    ASSERT_EQ(sim.stats().strategy_submits, 1u);
    ASSERT_EQ(sim.fills().size(), 0u);

    // The order ahead cancels rather than trading. Queue position advances
    // without a single share changing hands — which is why a fill model built
    // on trades alone is optimistic.
    sim.on_market_event(cancel(2, 1, Side::Buy, 100, 200));
    sim.on_market_event(execute(3, 999, Side::Buy, 100, 50));

    EXPECT_EQ(sim.stats().strategy_fills, 1u);
}

TEST(Injection, StrategyOrdersDoNotDisturbTheHistoricalBookTheyDoNotTouch) {
    OneShot strategy(Side::Buy, 90, 50);  // far from the touch, never fills
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 100, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 10));
    sim.on_market_event(execute(2, 1, Side::Buy, 100, 10));

    EXPECT_EQ(sim.stats().strategy_fills, 0u);
    EXPECT_EQ(sim.portfolio().position(), 0);
}

// --- taking liquidity --------------------------------------------------------

// A strategy that crosses the spread once, so the taker path can be driven
// deterministically.
class OneShotTaker : public Strategy {
public:
    OneShotTaker(Side side, std::int64_t limit, std::uint32_t qty)
        : side_(side), limit_(limit), qty_(qty) {}

    void on_book(const BookView& book, OrderGate& gate) override {
        // Wait for a two-sided book. Crossing into an empty opposite side is
        // not a test of anything — the order simply expires.
        if (done_ || !book.two_sided()) return;
        done_ = true;
        id = gate.take(side_, Price{limit_}, Quantity{qty_});
    }

    const char* name() const override { return "one-shot taker"; }
    match::OrderId id{};

private:
    Side side_;
    std::int64_t limit_;
    std::uint32_t qty_;
    bool done_ = false;
};

TEST(Taker, CrossesTheSpreadAndFillsImmediately) {
    OneShotTaker strategy(Side::Buy, 101, 30);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 50));

    // The order was decided on the second event and arrives on the third,
    // where it lifts the resting offer without ever queueing.
    sim.on_market_event(add(2, 3, Side::Sell, 102, 10));

    EXPECT_EQ(sim.stats().taker_fills, 1u);
    EXPECT_EQ(sim.stats().maker_fills, 0u);
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_FALSE(sim.fills().front().maker) << "a crossing order provides no liquidity";
    EXPECT_EQ(sim.fills().front().price, Price{101}) << "pays the resting maker's price";
    EXPECT_EQ(sim.fills().front().quantity, Quantity{30});
    EXPECT_EQ(sim.fills().front().queue_ahead_at_entry, 0u) << "a taker never queues";
    EXPECT_EQ(sim.portfolio().position(), 30);
}

TEST(Taker, HasNoQueuePositionAtAll) {
    // A crossing order never queues, so a queue position of "0.0" would be a
    // lie that reads as "front of the queue". It must be absent instead.
    OneShotTaker strategy(Side::Buy, 101, 30);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 50));
    sim.on_market_event(add(2, 3, Side::Sell, 102, 10));

    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_FALSE(sim.fills().front().has_queue_position());
}

TEST(Taker, PaysTheTakerFeeRatherThanEarningTheRebate) {
    OneShotTaker strategy(Side::Buy, 101, 10);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    SimulatorConfig config;
    config.fees = FeeSchedule{-2, 3};  // rebate for making, fee for taking
    Simulator sim(strategy, feed, order, fill_model, config);

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 50));
    sim.on_market_event(add(2, 3, Side::Sell, 102, 10));

    ASSERT_EQ(sim.stats().taker_fills, 1u);
    EXPECT_EQ(sim.portfolio().fees_paid(), 30) << "10 shares at 3 per share, paid not earned";
}

TEST(Taker, DoesNotRestItsUnfilledRemainder) {
    // Wants 100 but only 20 is offered inside its limit. The rest is dropped:
    // a taker that quietly rested its remainder would become a maker, and
    // would start earning a spread it never waited for.
    OneShotTaker strategy(Side::Buy, 101, 100);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 20));
    sim.on_market_event(add(2, 3, Side::Sell, 105, 10));

    EXPECT_EQ(sim.portfolio().position(), 20);
    // Nothing of the strategy's is resting: the book's best bid is still the
    // historical one.
    sim.on_market_event(execute(3, 1, Side::Buy, 99, 10));
    EXPECT_EQ(sim.stats().maker_fills, 0u);
}

TEST(Taker, RespectsItsLimitPrice) {
    // Only offers above the limit exist, so nothing should fill.
    OneShotTaker strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 105, 50));
    sim.on_market_event(add(2, 3, Side::Sell, 106, 10));

    EXPECT_EQ(sim.stats().taker_fills, 0u);
    EXPECT_EQ(sim.portfolio().position(), 0);
}

// --- leakage discipline ------------------------------------------------------

TEST(Leakage, TheViewIsAValueNotAWindowOntoTheLiveBook) {
    // BookView holds copies. Once handed out it cannot change, so a strategy
    // that stores one cannot use it to observe the future. This is the
    // structural guarantee, asserted rather than assumed.
    static_assert(std::is_copy_constructible_v<BookView>);

    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 10));
    ASSERT_FALSE(strategy.views.empty());
    const BookView held = strategy.views.front();
    const auto bid_when_seen = held.best_bid();

    // Move the market decisively.
    sim.on_market_event(add(2, 3, Side::Buy, 100, 500));
    sim.on_market_event(add(3, 4, Side::Buy, 100, 500));

    EXPECT_EQ(held.best_bid(), bid_when_seen) << "a held view must not track the live book";
}

// --- accounting --------------------------------------------------------------

TEST(Portfolio, BuyingSpendsCashAndSellingRaisesIt) {
    Portfolio p;
    p.apply(Fill{Time{0}, match::OrderId{1}, Side::Buy, Price{100}, Quantity{10}, true, 0});
    EXPECT_EQ(p.position(), 10);
    EXPECT_EQ(p.cash(), -1000);

    p.apply(Fill{Time{1}, match::OrderId{2}, Side::Sell, Price{110}, Quantity{10}, true, 0});
    EXPECT_EQ(p.position(), 0);
    EXPECT_EQ(p.cash(), 100);  // bought at 100, sold at 110, ten shares
}

TEST(Portfolio, MakerRebateIsCredited) {
    // Negative maker fee is a rebate: the venue pays for the liquidity.
    Portfolio p{FeeSchedule{-2, 3}};
    p.apply(Fill{Time{0}, match::OrderId{1}, Side::Buy, Price{100}, Quantity{10}, true, 0});
    EXPECT_EQ(p.fees_paid(), -20);
    EXPECT_EQ(p.cash(), -1000 + 20);
}

TEST(Portfolio, TakerFeeIsCharged) {
    Portfolio p{FeeSchedule{-2, 3}};
    p.apply(Fill{Time{0}, match::OrderId{1}, Side::Buy, Price{100}, Quantity{10}, false, 0});
    EXPECT_EQ(p.fees_paid(), 30);
    EXPECT_EQ(p.cash(), -1030);
}

TEST(Portfolio, InventoryIsMarkedToTheMidNotTheTouch) {
    // Marking unclosed inventory at the touch would hand the strategy a free
    // half-spread on every open position.
    Portfolio p;
    p.apply(Fill{Time{0}, match::OrderId{1}, Side::Buy, Price{100}, Quantity{10}, true, 0});

    // Mid of 105 (doubled: 210). Bought 10 at 100, so 50 of unrealised gain.
    EXPECT_EQ(p.pnl_x2(210), 2 * -1000 + 10 * 210);
    EXPECT_EQ(p.pnl_x2(210) / 2, 50);
}

// --- performance statistics --------------------------------------------------

namespace {
std::vector<std::pair<Time, std::int64_t>> flat_mids(std::int64_t mid_x2, int count) {
    std::vector<std::pair<Time, std::int64_t>> mids;
    for (int i = 0; i < count; ++i) mids.emplace_back(Time{seconds(i).count}, mid_x2);
    return mids;
}
}  // namespace

TEST(Performance, EquityTracksMarkToMarketNotRealisedPnl) {
    // Buy 10 at 100 and hold. With the mid at 110 the position is worth 100
    // more than it cost, and that shows up in equity immediately — a curve
    // built on realised PnL only would sit flat at zero and hide the risk
    // being carried.
    const std::vector<Fill> fills{
        Fill{Time{0}, match::OrderId{1}, Side::Buy, Price{100}, Quantity{10}, true, 0}};
    const auto mids = flat_mids(220, 5);  // mid 110

    const PerformanceAnalyzer analyzer(fills, mids);
    const auto curve = analyzer.equity_curve(seconds(1));

    ASSERT_FALSE(curve.empty());
    EXPECT_EQ(curve.back().position, 10);
    EXPECT_DOUBLE_EQ(curve.back().equity, 100.0);  // 10 * (110 - 100)
}

TEST(Performance, DrawdownIsMeasuredOnMarkToMarket) {
    // Bought at 100; the mid then falls to 90 before recovering. The trough is
    // a real drawdown even though nothing was ever sold.
    const std::vector<Fill> fills{
        Fill{Time{0}, match::OrderId{1}, Side::Buy, Price{100}, Quantity{10}, true, 0}};
    std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{seconds(0).count}, 200},  // mid 100, equity 0
        {Time{seconds(1).count}, 180},  // mid  90, equity -100
        {Time{seconds(2).count}, 200},  // mid 100, equity 0
    };

    const PerformanceAnalyzer analyzer(fills, mids);
    const auto report = analyzer.analyse(seconds(1), seconds(1));
    EXPECT_DOUBLE_EQ(report.max_drawdown, 100.0);
    EXPECT_DOUBLE_EQ(report.net_pnl, 0.0) << "ends flat, but the drawdown happened";
}

TEST(Performance, TurnoverAndRoundTripsCountBothSides) {
    const std::vector<Fill> fills{
        Fill{Time{0}, match::OrderId{1}, Side::Buy, Price{100}, Quantity{10}, true, 0},
        Fill{Time{seconds(1).count}, match::OrderId{2}, Side::Sell, Price{100}, Quantity{4}, true,
             0}};
    const auto mids = flat_mids(200, 3);

    const PerformanceAnalyzer analyzer(fills, mids);
    const auto report = analyzer.analyse(seconds(1), seconds(1));

    EXPECT_EQ(report.shares_traded, 14);
    EXPECT_EQ(report.round_trips, 4) << "only the matched shares are a round trip";
    EXPECT_DOUBLE_EQ(report.turnover, 1400.0);
}

TEST(Performance, HitRateCountsFillsThatWereRightAfterwards) {
    // A buy at 100 with the mid at 110 a second later is a good fill; a sell
    // at the same price into the same rise is a bad one.
    const std::vector<Fill> fills{
        Fill{Time{0}, match::OrderId{1}, Side::Buy, Price{100}, Quantity{1}, true, 0},
        Fill{Time{0}, match::OrderId{2}, Side::Sell, Price{100}, Quantity{1}, true, 0}};
    std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},
        {Time{seconds(1).count}, 220},
        {Time{seconds(2).count}, 220},
    };

    const PerformanceAnalyzer analyzer(fills, mids);
    const auto report = analyzer.analyse(seconds(1), seconds(1));
    EXPECT_DOUBLE_EQ(report.hit_rate, 0.5);
    EXPECT_DOUBLE_EQ(report.avg_markout, 0.0) << "the two cancel exactly";
}

TEST(Performance, TimeInMarketIgnoresFlatSamples) {
    const std::vector<Fill> fills{
        Fill{Time{seconds(2).count}, match::OrderId{1}, Side::Buy, Price{100}, Quantity{10}, true,
             0}};
    const auto mids = flat_mids(200, 4);

    const PerformanceAnalyzer analyzer(fills, mids);
    const auto report = analyzer.analyse(seconds(1), seconds(1));
    // Flat for the first two samples of four.
    EXPECT_DOUBLE_EQ(report.time_in_market, 0.5);
}

TEST(Performance, AConstantPnlStreamHasNoVarianceAndNoSharpe) {
    // Guards against a divide-by-zero producing an infinite Sharpe, which is
    // the most embarrassing number a backtester can print.
    const std::vector<Fill> fills;
    const auto mids = flat_mids(200, 10);
    const PerformanceAnalyzer analyzer(fills, mids);
    const auto report = analyzer.analyse(seconds(1), seconds(1));
    EXPECT_DOUBLE_EQ(report.return_stdev, 0.0);
    EXPECT_DOUBLE_EQ(report.sharpe_annualised, 0.0);
}

// --- the strategy ------------------------------------------------------------

TEST(FixedOffsetMaker, QuotesBothSidesAroundTheMid) {
    FixedOffsetMakerConfig config;
    config.offset = 10;
    config.size = Quantity{25};
    FixedOffsetMaker strategy{config};

    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 100, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 120, 10));

    // Mid is 110, so it should be bidding 100 and offering 120.
    EXPECT_GE(sim.stats().strategy_submits, 2u);
}

TEST(FixedOffsetMaker, DoesNotChurnItsQueuePositionWhenTheMidIsUnchanged) {
    FixedOffsetMakerConfig config;
    config.offset = 10;
    FixedOffsetMaker strategy{config};

    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 100, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 120, 10));
    const auto after_first = sim.stats().strategy_submits;

    // More events, same touch: requoting here would throw away queue position
    // for nothing, and would make the study a measurement of churn.
    sim.on_market_event(add(2, 3, Side::Buy, 90, 10));
    sim.on_market_event(add(3, 4, Side::Sell, 130, 10));

    EXPECT_EQ(sim.stats().strategy_submits, after_first) << "requoted despite an unchanged mid";
}

TEST(FixedOffsetMaker, ExposureLimitBindsDespiteOrderLatency) {
    // Regression. The strategy used to track only its most recent quote per
    // side. When it requoted, the cancel took an order-latency to land, and in
    // that window the forgotten order was still live and could still fill — so
    // the position limit never bound and the position ran to eighteen times it.
    // A backtest in that state measures inventory mark-to-market, not
    // execution.
    FixedOffsetMakerConfig config;
    config.offset = 10;
    config.size = Quantity{100};
    config.max_position = 300;
    FixedOffsetMaker strategy{config};

    ZeroDelay feed;
    ConstantDelay order{millis(1)};  // long enough for cancels to lag badly
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.seed_level(Side::Sell, Price{200}, Quantity{100000});
    sim.on_market_event(add(0, 1, Side::Buy, 100, 100));
    sim.on_market_event(add(1, 2, Side::Sell, 120, 100));

    // Walk the mid around so the strategy requotes constantly, then hit its
    // bids repeatedly. Every fill is on the buy side, so an unbounded strategy
    // accumulates without limit.
    std::int64_t t = millis(2).count;
    for (int i = 0; i < 200; ++i) {
        t += micros(200).count;
        sim.on_market_event(add(t, 1000 + std::uint64_t(i), Side::Buy, 100 + (i % 5), 100));
        t += micros(200).count;
        sim.on_market_event(execute(t, 1, Side::Buy, 100 + (i % 5), 100));

        EXPECT_LE(std::abs(sim.portfolio().position()), config.max_position + 200)
            << "exposure ran away at iteration " << i;
    }
}

TEST(FixedOffsetMaker, LearnsWhenAnOrderIsGone) {
    FixedOffsetMaker strategy{};
    OrderGate gate;
    BookView view{Time{0}, Time{0}, Price{100}, 10, Price{120}, 10};

    strategy.on_book(view, gate);
    ASSERT_FALSE(gate.submits().empty());
    const auto id = gate.submits().front().id;

    // Once told the order is gone, the strategy must be willing to requote it
    // rather than believing it still has a live quote there.
    strategy.on_order_gone(id, false, gate);
    gate.clear();
    strategy.on_book(view, gate);
    EXPECT_FALSE(gate.submits().empty()) << "did not replace an order it was told was gone";
}

TEST(FixedOffsetMaker, StopsQuotingASideAtItsPositionLimit) {
    FixedOffsetMakerConfig config;
    config.offset = 10;
    config.max_position = 5;
    config.size = Quantity{10};
    FixedOffsetMaker strategy{config};
    EXPECT_EQ(strategy.position(), 0);

    OrderGate gate;
    BookView view{Time{0}, Time{0}, Price{100}, 10, Price{120}, 10};

    strategy.on_book(view, gate);
    EXPECT_EQ(gate.submits().size(), 2u) << "both sides at a flat position";

    // Fill the buy side past the limit, then the strategy should stop bidding.
    strategy.on_fill(Fill{Time{1}, gate.submits()[0].id, Side::Buy, Price{100}, Quantity{10}, true,
                          0},
                     view, gate);
    ASSERT_EQ(strategy.position(), 10);

    gate.clear();
    strategy.on_book(view, gate);
    for (const auto& submit : gate.submits()) {
        EXPECT_EQ(submit.side, Side::Sell) << "still bidding while long past the limit";
    }
}

}  // namespace
}  // namespace sim
