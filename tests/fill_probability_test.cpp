// Tests for the posted-order record and the fill-probability surface.
//
// A fill rate needs a denominator. The simulator's fills() vector is the
// numerator alone — an order posted and never filled leaves no trace in it —
// so without a record of every posted order there is no rate to compute, only
// a count.
#include "sim/fill_probability.hpp"

#include "sim/fill_model.hpp"
#include "sim/latency.hpp"
#include "sim/order_tracker.hpp"
#include "sim/simulator.hpp"
#include "sim/strategy.hpp"
#include "sim/types.hpp"

#include <cstdint>
#include <vector>

#include "match/types.hpp"

#include <gtest/gtest.h>

namespace sim {
namespace {

using match::Price;
using match::Quantity;
using match::Side;

class PostOnce : public Strategy {
  public:
    PostOnce(Side side, std::int64_t price, std::uint32_t qty)
        : side_(side), price_(price), qty_(qty) {}

    void on_book(const BookView& book, OrderGate& gate) override {
        if (posted_ || !book.two_sided()) return;
        posted_ = true;
        gate.submit(side_, Price{price_}, Quantity{qty_});
    }

    const char* name() const override { return "post-once"; }

  private:
    Side side_;
    std::int64_t price_;
    std::uint32_t qty_;
    bool posted_ = false;
};

MarketEvent add(std::int64_t t, std::uint64_t id, Side side, std::int64_t price,
                std::uint32_t qty) {
    return MarketEvent{Time{t},      MarketEventType::Add, match::OrderId{id},
                       Price{price}, Quantity{qty},        side};
}

MarketEvent execute(std::int64_t t, std::uint64_t maker_id, Side maker_side, std::int64_t price,
                    std::uint32_t qty) {
    return MarketEvent{
        Time{t},   MarketEventType::Execute, match::OrderId{maker_id}, Price{price}, Quantity{qty},
        maker_side};
}

TEST(OrderTracker, RecordsAnOrderThatNeverFills) {
    PostOnce strategy(Side::Buy, 90, 50);  // far from the touch
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 100, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 10));
    sim.on_market_event(add(2, 3, Side::Sell, 102, 10));
    sim.finish();

    ASSERT_EQ(sim.posted_orders().size(), 1u);
    const PostedOrder& posted = sim.posted_orders().front();
    EXPECT_EQ(posted.quantity, Quantity{50});
    EXPECT_EQ(posted.filled_quantity.units, 0u);
    EXPECT_FALSE(posted.was_filled());
    EXPECT_EQ(sim.fills().size(), 0u) << "the fill vector cannot see this order at all";
}

TEST(OrderTracker, RecordsPartialFillsAgainstThePostedQuantity) {
    PostOnce strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 10));
    sim.on_market_event(execute(2, 999, Side::Buy, 100, 20));
    sim.finish();

    ASSERT_EQ(sim.posted_orders().size(), 1u);
    const PostedOrder& posted = sim.posted_orders().front();
    EXPECT_EQ(posted.filled_quantity, Quantity{20});
    EXPECT_TRUE(posted.was_filled()) << "partially filled still counts as filled";
    EXPECT_FALSE(posted.fully_filled());
    EXPECT_EQ(posted.quantity, Quantity{50});
}

TEST(OrderTracker, CapturesTheOppositeSideDepthAtPostTime) {
    // The surface is indexed by both sides: the near-side queue AND the
    // opposite depth, because a thin opposite side is what pulls the price
    // through your order.
    PostOnce strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 77));
    sim.on_market_event(add(2, 3, Side::Sell, 102, 10));
    sim.finish();

    ASSERT_EQ(sim.posted_orders().size(), 1u);
    EXPECT_EQ(sim.posted_orders().front().opposite_depth, 77u);
}

// --- the surface --------------------------------------------------------------

PostedOrder posted(double qp, match::AggregateQuantity opposite, std::uint32_t filled) {
    PostedOrder order;
    order.quantity = Quantity{100};
    order.filled_quantity = Quantity{filled};
    order.opposite_depth = opposite;
    // Encode the requested queue position against a 1000-share level.
    order.queue_ahead = static_cast<match::AggregateQuantity>(qp * 1000.0);
    order.queue_behind = 1000 - order.queue_ahead;
    return order;
}

TEST(FillSurfaceTest, BinsByQueuePositionAndReportsAFillRate) {
    FillSurface surface;
    surface.add(posted(0.0, 1, 100));
    surface.add(posted(0.1, 1, 0));

    const FillSurface::Cell& cell = surface.cell(0, 0);
    EXPECT_EQ(cell.posted, 2u);
    EXPECT_EQ(cell.filled, 1u);
    EXPECT_DOUBLE_EQ(cell.fill_rate(), 0.5);
}

TEST(FillSurfaceTest, BackOfQueueLandsInTheLastBin) {
    FillSurface surface;
    surface.add(posted(0.99, 1, 0));
    EXPECT_EQ(surface.cell(FillSurface::kQueueBins - 1, 0).posted, 1u);
    EXPECT_EQ(surface.cell(0, 0).posted, 0u);
}

TEST(FillSurfaceTest, AQueuePositionOfExactlyOneDoesNotOverflowTheBins) {
    // qp == 1.0 is reachable: the back of a level with nothing behind it.
    // Naive binning by (int)(qp * bins) indexes one past the end.
    FillSurface surface;
    PostedOrder order;
    order.quantity = Quantity{100};
    order.filled_quantity = Quantity{0};
    order.queue_ahead = 500;
    order.queue_behind = 0;
    order.opposite_depth = 1;
    ASSERT_DOUBLE_EQ(order.normalised_queue_position(), 1.0);

    surface.add(order);
    EXPECT_EQ(surface.cell(FillSurface::kQueueBins - 1, 0).posted, 1u);
}

TEST(FillSurfaceTest, AnEmptyCellHasNoFillRateRatherThanZero) {
    // An untouched cell must be distinguishable from one where nothing filled.
    const FillSurface surface;
    EXPECT_EQ(surface.cell(0, 0).posted, 0u);
    EXPECT_FALSE(surface.cell(0, 0).has_data());
}

TEST(FillSurfaceTest, ThickerOppositeDepthLandsInAHigherBin) {
    FillSurface surface;
    surface.add(posted(0.0, 1, 0));     // thin
    surface.add(posted(0.0, 1000, 0));  // thick
    EXPECT_EQ(surface.cell(0, 0).posted, 1u);
    EXPECT_EQ(surface.cell(0, FillSurface::kDepthBins - 1).posted, 1u);
}

}  // namespace
}  // namespace sim
