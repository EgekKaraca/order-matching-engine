// simulator.hpp — the merged, timestamp-ordered replay loop.
//
//         historical L3 events ──┐
//                                ├──► merge by timestamp ──► libmatch ──► reports
//         strategy orders ───────┘         (+ latency)                      │
//               ▲                                                           │
//               └──────────────── strategy callback ◄──────────────────────┘
//
// The strategy's orders enter the same book as everyone else's, take a real
// place in a real FIFO queue, and fill only when the size ahead of them
// actually drains — via the historical cancels and executions being replayed.
// Queue position is mostly destroyed by cancellation, not by trading, which is
// why a fill model built on trades alone is optimistic.
//
// THE ONE ASSUMPTION THIS CANNOT ESCAPE: replaying historical flow assumes the
// strategy's orders do not change anyone else's behaviour. The counterparties
// it trades against would have acted differently had it really been there. So
// this is a queue-realistic, IMPACT-FREE simulator: valid for small passive
// size, and invalid for large or aggressive size. The study quantifies where
// that starts to bite rather than pretending it does not.
#ifndef SIM_SIMULATOR_HPP
#define SIM_SIMULATOR_HPP

#include <cstdint>
#include <deque>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "match/map_book.hpp"
#include "match/report.hpp"
#include "match/types.hpp"
#include "sim/fill_model.hpp"
#include "sim/latency.hpp"
#include "sim/portfolio.hpp"
#include "sim/strategy.hpp"
#include "sim/types.hpp"

namespace sim {

struct SimulatorConfig {
    FeeSchedule fees = FeeSchedule::none();
    // Strategy order ids live above this so they can never collide with a
    // historical order id or a synthetic seeding id.
    std::uint64_t strategy_id_base = 1ULL << 62;
    std::uint64_t aggressor_id = (1ULL << 61) + 1;
};

struct SimulatorStats {
    std::size_t events = 0;
    std::size_t strategy_submits = 0;
    std::size_t strategy_cancels = 0;
    std::size_t strategy_fills = 0;
    std::size_t maker_fills = 0;
    std::size_t taker_fills = 0;
    std::size_t rejected_cancels = 0;
    std::size_t orders_retired = 0;
    std::size_t callbacks = 0;
};

class Simulator {
public:
    // `fill_model` decides whether a strategy order fills. Pass L3ExactFill to
    // let the engine decide; the other models answer a counterfactual and
    // deliberately keep the strategy's orders out of the book.
    Simulator(Strategy& strategy, DelayModel& feed_delay, DelayModel& order_delay,
              FillModel& fill_model, SimulatorConfig config = {});

    // Seeds the book with resting liquidity that has no per-order identity —
    // a depth snapshot at the start of the window.
    void seed_level(match::Side side, match::Price price, match::Quantity quantity);

    // Applies one historical event, then lets the clock advance to it. Events
    // must arrive in non-decreasing time order.
    void on_market_event(const MarketEvent& event);

    // Flushes any strategy orders still in flight at the end of the window.
    void finish();

    const Portfolio& portfolio() const { return portfolio_; }
    const SimulatorStats& stats() const { return stats_; }
    const std::vector<Fill>& fills() const { return fills_; }

    // (time, 2x mid) sampled at every event, for mark-out computation after the
    // run. Kept as a time series rather than folded in live, because a mark-out
    // at horizon h needs a price from the FUTURE of the fill — which the
    // strategy must never see, but the analyst may.
    const std::vector<std::pair<Time, std::int64_t>>& mid_series() const { return mid_series_; }

    // Displayed size at the touch, sampled at every event. Lets an order size
    // be expressed as a FRACTION OF LEVEL DEPTH — which is the axis along which
    // the impact-free assumption stops being defensible, so it has to be
    // measured rather than asserted.
    const std::vector<match::AggregateQuantity>& touch_depths() const { return touch_depths_; }

private:
    struct PendingSubmit {
        Time arrival;
        match::Side side;
        match::Price price;
        match::Quantity quantity;
        match::OrderId id;
        match::TimeInForce tif;
    };
    struct PendingCancel {
        Time arrival;
        match::OrderId id;
    };
    struct VisibleUpdate {
        Time visible_at;
        Time as_of;
        match::Price best_bid;
        match::AggregateQuantity bid_size;
        match::Price best_ask;
        match::AggregateQuantity ask_size;
    };
    struct PendingFill {
        Time visible_at;
        Fill fill;
    };
    struct PendingGone {
        Time visible_at;
        match::OrderId id;
        bool filled;
    };

    struct TradeSink {
        Simulator* owner;
        void operator()(const match::Report& report) const { owner->on_report(report); }
    };
    friend struct TradeSink;

    using Book = match::MapBook<TradeSink>;

    void on_report(const match::Report& report);
    void record_fill(match::OrderId id, match::Side side, match::Price price,
                     match::Quantity quantity, match::AggregateQuantity ahead_at_entry,
                     bool maker);
    void apply_model_fills(const MarketEvent& event);
    void deliver_due(Time now);
    void release_orders(Time now);
    void publish_snapshot(Time as_of);
    // Takes whatever the strategy just asked for and schedules it with order
    // latency. The strategy cannot do this itself — that is the point.
    void schedule_from_gate(Time decided_at);
    BookView current_view(Time now) const;
    void run_callbacks(Time now);

    Strategy& strategy_;
    FillModel& fill_model_;
    DelayModel& feed_delay_;
    DelayModel& order_delay_;
    SimulatorConfig config_;

    Book book_;
    Portfolio portfolio_;
    OrderGate gate_;
    SimulatorStats stats_;

    Time now_{};
    // The strategy order currently being submitted, if any. A marketable order
    // trades during on_new_order, before it could be registered as live, so
    // its taker-side fills are recognised through this.
    match::OrderId in_flight_{};
    bool in_flight_active_ = false;
    std::uint64_t next_seed_id_ = 1ULL << 60;

    std::deque<PendingSubmit> pending_submits_;
    std::deque<PendingCancel> pending_cancels_;
    std::deque<VisibleUpdate> feed_queue_;
    std::deque<PendingFill> fill_queue_;
    std::deque<PendingGone> gone_queue_;

    // Live strategy orders, and the queue depth ahead of each at the moment it
    // entered the book.
    std::unordered_map<match::OrderId, match::AggregateQuantity> live_orders_;
    std::unordered_map<match::OrderId, match::Side> order_sides_;
    std::unordered_map<match::OrderId, match::Quantity> remaining_;
    std::vector<ModelFill> model_fills_;

    // Read-only window onto the historical book for the counterfactual models.
    struct Probe;
    friend struct Probe;

    BookView last_view_;
    std::vector<Fill> fills_;
    std::vector<std::pair<Time, std::int64_t>> mid_series_;
    std::vector<match::AggregateQuantity> touch_depths_;
};

}  // namespace sim

#endif  // SIM_SIMULATOR_HPP
