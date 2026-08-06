// fill_model.hpp — the three answers to "would my order actually have filled?"
//
// This is the axis the entire project measures. The same strategy, on the same
// data, with the same latency, produces different PnL depending only on which
// of these is used — and the difference is the result.
//
//   A  Naive        filled whenever the market trades at my price.
//                   What most portfolio backtesters do, explicitly or not.
//   B  L2 probabilistic   queue position ESTIMATED from depth changes,
//                   because L2 data cannot see where in the queue you are.
//   C  L3 exact     the real queue, the real FIFO, the real drainage.
//                   Only possible with per-order data and a matching engine.
//
// Models A and B never put the strategy's orders into the book: they are
// answering a counterfactual about a book they do not participate in, and
// inserting the orders would change the depth they read. Model C does insert
// them, because participating IS the model.
#ifndef SIM_FILL_MODEL_HPP
#define SIM_FILL_MODEL_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "match/types.hpp"
#include "sim/types.hpp"

namespace sim {

// Read-only access to the historical book, for models that need depth.
class BookProbe {
public:
    virtual ~BookProbe() = default;
    virtual match::AggregateQuantity depth_at(match::Price price, match::Side side) const = 0;
    virtual match::Price best_bid() const = 0;
    virtual match::Price best_ask() const = 0;
};

struct ModelFill {
    match::OrderId id;
    match::Quantity quantity;
    match::Price price;
};

class FillModel {
public:
    virtual ~FillModel() = default;

    // True when the strategy's orders are inserted into the real book and the
    // engine decides the fills. False for the counterfactual models.
    virtual bool uses_engine() const = 0;

    virtual void on_placed(match::OrderId id, match::Side side, match::Price price,
                           match::Quantity quantity, const BookProbe& book) = 0;
    virtual void on_removed(match::OrderId id) = 0;

    // Called after each historical event has been applied to the book. Appends
    // any fills the model says the strategy earned.
    virtual void on_event(const MarketEvent& event, const BookProbe& book,
                          std::vector<ModelFill>& out) = 0;

    // Size resting ahead of an order, as this model believes it. Model A
    // always answers zero, which is precisely its defect.
    virtual match::AggregateQuantity queue_ahead(match::OrderId id) const = 0;

    virtual const char* name() const = 0;
};

// --- C: the real thing -------------------------------------------------------

// A pass-through. The engine holds the orders and reports the fills; this
// exists so the three models present one interface to the simulator.
class L3ExactFill final : public FillModel {
public:
    bool uses_engine() const override { return true; }
    void on_placed(match::OrderId, match::Side, match::Price, match::Quantity,
                   const BookProbe&) override {}
    void on_removed(match::OrderId) override {}
    void on_event(const MarketEvent&, const BookProbe&, std::vector<ModelFill>&) override {}
    match::AggregateQuantity queue_ahead(match::OrderId) const override { return 0; }
    const char* name() const override { return "C: L3 exact (real queue)"; }
};

// --- A: the naive assumption -------------------------------------------------

// "The market traded at my price, so I was filled." No queue at all.
//
// One deliberate restraint: the fill is capped by the size that actually
// traded, rather than filling the order outright. That is the LESS generous
// naive variant, so the gap this study measures comes out as a lower bound.
// A backtester that fills the whole order on any trade at its price would show
// a larger gap, and the conservative choice is the defensible one.
class NaiveFill final : public FillModel {
public:
    bool uses_engine() const override { return false; }

    void on_placed(match::OrderId id, match::Side side, match::Price price,
                   match::Quantity quantity, const BookProbe&) override {
        orders_[id] = Resting{side, price, quantity};
    }

    void on_removed(match::OrderId id) override { orders_.erase(id); }

    void on_event(const MarketEvent& event, const BookProbe&,
                  std::vector<ModelFill>& out) override {
        if (event.type != MarketEventType::Execute) return;

        // event.side is the maker's side, so an order of the same side at a
        // price the trade reached would, under this assumption, have traded.
        std::uint32_t remaining_volume = event.quantity.units;
        for (auto& [id, order] : orders_) {
            if (remaining_volume == 0) break;
            if (order.side != event.side) continue;
            // A buy order fills if the market traded at or below its price.
            if (!match::crosses(order.price, event.price, order.side)) continue;

            const std::uint32_t fill = std::min(remaining_volume, order.quantity.units);
            if (fill == 0) continue;
            out.push_back(ModelFill{id, match::Quantity{fill}, order.price});
            order.quantity.units -= fill;
            remaining_volume -= fill;
        }
        erase_empty();
    }

    match::AggregateQuantity queue_ahead(match::OrderId) const override {
        return 0;  // the assumption, stated as a number
    }

    const char* name() const override { return "A: naive (filled at the touch)"; }

private:
    struct Resting {
        match::Side side;
        match::Price price;
        match::Quantity quantity;
    };

    void erase_empty() {
        for (auto it = orders_.begin(); it != orders_.end();) {
            it = it->second.quantity.units == 0 ? orders_.erase(it) : std::next(it);
        }
    }

    std::unordered_map<match::OrderId, Resting> orders_;
};

// --- B: queue position estimated from L2 depth --------------------------------

// STATUS: EXPERIMENTAL — EXCLUDED FROM THE HEADLINE RESULT. Read this before
// quoting any number this class produces.
//
// A partial correction toward C ought to land between A and C. This one does
// not: it reports a HIGHER PnL than the naive model, and captures roughly 2.4x
// the spread per fill that L3 exact does, which no queue-position model should.
//
// Two genuine defects have already been found and fixed here, so the remaining
// anomaly is not obviously the same kind of thing:
//   1. it filled only on trades landing exactly on its own price, ignoring a
//      market that traded THROUGH the limit — a fivefold under-fill;
//   2. it then over-corrected, treating a trade through the level as an
//      ordinary queue advance rather than as the level being swept.
//
// It is left in the tree, unquoted, deliberately. The tempting next move is to
// keep adjusting it until it falls between A and C — and that would be fitting
// the model to the expected answer, which is a worse failure than not having
// the model at all. Whoever picks this up should find the mechanism first and
// let the number land where it lands.
//
// The project's own cut order puts model B first to go; A-vs-C is the whole
// argument and does not depend on this.
//
// A reimplementation of the ProbQueueModel family from hftbacktest, which
// exists precisely because L2 data cannot see queue position and must guess it.
//
// The order is placed at the back of the level, so the initial estimate of the
// size ahead is the whole displayed depth. Then:
//
//   - a TRADE at the level consumes from the front, so it advances the
//     estimate one-for-one;
//   - any other decrease in depth (cancellations) might have come from ahead
//     of the order or behind it. The probability it came from behind is
//     f(back) / (f(front) + f(back)) with f(x) = x^n — PowerProbQueueFunc.
//
// The expected value is applied rather than sampling from it. That is a
// deliberate choice: it makes the run reproducible and removes a source of
// variance that would otherwise have to be averaged away over many seeds,
// which would confound the very comparison being made.
class ProbQueueFill final : public FillModel {
public:
    explicit ProbQueueFill(double exponent = 1.0) : exponent_(exponent) {}

    bool uses_engine() const override { return false; }

    void on_placed(match::OrderId id, match::Side side, match::Price price,
                   match::Quantity quantity, const BookProbe& book) override {
        // Joining the back of the queue: everything displayed is ahead.
        const auto ahead = static_cast<double>(book.depth_at(price, side));
        orders_[id] = Resting{side, price, quantity, ahead};
    }

    void on_removed(match::OrderId id) override { orders_.erase(id); }

    void on_event(const MarketEvent& event, const BookProbe& book,
                  std::vector<ModelFill>& out) override {
        const bool is_trade = event.type == MarketEventType::Execute;
        const bool is_reduction = event.type == MarketEventType::Reduce ||
                                  event.type == MarketEventType::Delete;
        if (!is_trade && !is_reduction) return;

        for (auto& [id, order] : orders_) {
            if (order.side != event.side) continue;

            // Three cases, and conflating them is what made this model wrong
            // twice. Take a resting BUY at price P:
            //
            //   trade at P          consumes the queue at my own level, so it
            //                       advances my position one-for-one;
            //   trade BELOW P       a lower bid traded, which can only happen
            //                       once everything at P was exhausted — so my
            //                       level was swept on the way through and
            //                       nothing can still be ahead of me;
            //   trade ABOVE P       a better bid traded. My level was never
            //                       touched. It must NOT advance my queue.
            //
            // The mirror holds for a sell. Using crosses() alone merged the
            // first two cases and advanced the queue by the wrong amount; using
            // equality alone dropped the second case entirely and under-filled
            // this model fivefold.
            const bool at_my_level = order.price == event.price;
            const bool through_my_level =
                !at_my_level && match::crosses(order.price, event.price, order.side);

            if (is_trade && !at_my_level && !through_my_level) continue;
            if (!is_trade && !at_my_level) continue;

            if (is_trade) {
                if (through_my_level) {
                    // Swept. Everything ahead is gone and so am I.
                    order.ahead = 0.0;
                    out.push_back(ModelFill{id, order.quantity, order.price});
                    order.quantity.units = 0;
                    continue;
                }
                // Trades consume the front of the queue, so they advance the
                // estimate fully.
                const double consumed = static_cast<double>(event.quantity.units);
                const double overflow = consumed - order.ahead;
                order.ahead = std::max(0.0, order.ahead - consumed);
                if (overflow > 0.0) {
                    const auto fill = static_cast<std::uint32_t>(
                        std::min<double>(overflow, order.quantity.units));
                    if (fill > 0) {
                        out.push_back(ModelFill{id, match::Quantity{fill}, order.price});
                        order.quantity.units -= fill;
                    }
                }
            } else {
                // A cancellation: split it between ahead and behind by the
                // power rule. `behind` is what the displayed depth implies is
                // queued after this order.
                const auto depth = static_cast<double>(book.depth_at(order.price, order.side));
                const double behind = std::max(0.0, depth - order.ahead);
                const double f_front = weight(order.ahead);
                const double f_back = weight(behind);
                const double from_front =
                    (f_front + f_back) <= 0.0 ? 0.0 : f_front / (f_front + f_back);
                order.ahead =
                    std::max(0.0, order.ahead - static_cast<double>(event.quantity.units) *
                                                    from_front);
            }
        }
        erase_empty();
    }

    match::AggregateQuantity queue_ahead(match::OrderId id) const override {
        const auto it = orders_.find(id);
        return it == orders_.end() ? 0 : static_cast<match::AggregateQuantity>(it->second.ahead);
    }

    const char* name() const override {
        return "B: L2 probabilistic (EXPERIMENTAL, excluded)";
    }

private:
    struct Resting {
        match::Side side;
        match::Price price;
        match::Quantity quantity;
        double ahead;
    };

    double weight(double x) const { return std::pow(std::max(0.0, x), exponent_); }

    void erase_empty() {
        for (auto it = orders_.begin(); it != orders_.end();) {
            it = it->second.quantity.units == 0 ? orders_.erase(it) : std::next(it);
        }
    }

    double exponent_;
    std::unordered_map<match::OrderId, Resting> orders_;
};

}  // namespace sim

#endif  // SIM_FILL_MODEL_HPP
