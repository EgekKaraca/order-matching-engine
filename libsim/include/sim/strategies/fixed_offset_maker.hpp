// fixed_offset_maker.hpp — one deliberately simple passive market maker.
//
// It quotes a fixed offset from the mid on both sides, refreshes when the
// market moves away from its quotes, and does nothing else. No signal, no
// inventory skew, no adverse-selection avoidance.
//
// That is the point. The strategy is INSTRUMENTATION for measuring the
// simulator, not an attempt to make money. A fixed-offset passive maker is the
// most queue-position-sensitive thing there is: every fill it gets is a fill it
// had to wait in line for, so the difference between "filled because the market
// traded at my price" and "filled because the queue ahead of me actually
// drained" shows up at full strength. A cleverer strategy would muddy the
// measurement with its own decisions.
//
// Scope creep here is the named risk in the project plan. One strategy.
#ifndef SIM_STRATEGIES_FIXED_OFFSET_MAKER_HPP
#define SIM_STRATEGIES_FIXED_OFFSET_MAKER_HPP

#include <cstdint>
#include <algorithm>
#include <optional>
#include <unordered_map>

#include "match/types.hpp"
#include "sim/strategy.hpp"
#include "sim/types.hpp"

namespace sim {

struct FixedOffsetMakerConfig {
    // How far from the mid to quote, in price units. Larger means a better
    // price if filled and a longer wait to be filled — which is exactly the
    // trade-off the fill model governs.
    std::int64_t offset = 100;
    match::Quantity size{100};
    // Stop quoting a side once inventory passes this, so a one-sided market
    // does not accumulate an unbounded position that dominates the PnL through
    // mark-to-market rather than through execution.
    std::int64_t max_position = 1000;
    // Requote only when the target price has actually moved. Without this the
    // strategy cancels and resubmits on every tick, throwing away queue
    // position continuously — which would make the measurement about churn
    // rather than about queueing.
    std::int64_t requote_threshold = 1;
};

class FixedOffsetMaker final : public Strategy {
public:
    explicit FixedOffsetMaker(FixedOffsetMakerConfig config = {}) : config_(config) {}

    void on_book(const BookView& book, OrderGate& gate) override {
        const auto mid_x2 = book.mid_x2();
        if (!mid_x2.has_value()) return;  // one-sided book: nothing to quote around

        // Work in doubled prices to keep the mid exact, then halve once. A
        // half-unit mid rounds toward the passive side on each quote, which is
        // conservative: it never quotes more aggressively than intended.
        const std::int64_t bid_target = (*mid_x2 - 2 * config_.offset) / 2;
        const std::int64_t ask_target = (*mid_x2 + 2 * config_.offset + 1) / 2;

        // The limit is on total EXPOSURE, not just on filled position. An order
        // that has been cancelled but whose cancel has not yet landed is still
        // live and can still fill, so it still counts. Limiting only the filled
        // position lets exposure grow without bound in exactly the window that
        // order latency creates.
        const std::int64_t long_exposure = position_ + outstanding(match::Side::Buy);
        const std::int64_t short_exposure = position_ - outstanding(match::Side::Sell);

        maintain(gate, match::Side::Buy, bid_target, long_exposure >= config_.max_position);
        maintain(gate, match::Side::Sell, ask_target, short_exposure <= -config_.max_position);
    }

    void on_fill(const Fill& fill, const BookView&, OrderGate&) override {
        const auto qty = static_cast<std::int64_t>(fill.quantity.units);
        position_ += fill.side == match::Side::Buy ? qty : -qty;

        // Reduce the outstanding exposure this order still represents.
        const auto live = live_.find(fill.id);
        if (live != live_.end()) {
            live->second.remaining -= std::min(live->second.remaining, qty);
        }

        if (bid_.has_value() && bid_->id == fill.id) bid_.reset();
        if (ask_.has_value() && ask_->id == fill.id) ask_.reset();
    }

    void on_order_gone(match::OrderId id, bool, OrderGate&) override {
        live_.erase(id);
        if (bid_.has_value() && bid_->id == id) bid_.reset();
        if (ask_.has_value() && ask_->id == id) ask_.reset();
    }

    const char* name() const override { return "fixed-offset passive maker"; }

    std::int64_t position() const { return position_; }

private:
    struct Quote {
        match::OrderId id;
        std::int64_t price;
    };

    struct Outstanding {
        match::Side side;
        std::int64_t remaining;
    };

    void maintain(OrderGate& gate, match::Side side, std::int64_t target, bool suppressed) {
        auto& quote = side == match::Side::Buy ? bid_ : ask_;

        if (suppressed) {
            if (quote.has_value()) {
                gate.cancel(quote->id);
                quote.reset();
            }
            return;
        }

        if (quote.has_value()) {
            const std::int64_t drift = quote->price > target ? quote->price - target
                                                             : target - quote->price;
            if (drift < config_.requote_threshold) return;  // still good enough; keep the queue slot
            gate.cancel(quote->id);
        }

        const match::OrderId id = gate.submit(side, match::Price{target}, config_.size);
        quote = Quote{id, target};
        live_[id] = Outstanding{side, static_cast<std::int64_t>(config_.size.units)};
    }

    // Shares still at risk on one side: everything submitted and not yet known
    // to be gone.
    std::int64_t outstanding(match::Side side) const {
        std::int64_t total = 0;
        for (const auto& [id, order] : live_) {
            if (order.side == side) total += order.remaining;
        }
        return total;
    }

    FixedOffsetMakerConfig config_;
    std::optional<Quote> bid_;
    std::optional<Quote> ask_;
    // Every order submitted and not yet confirmed gone — including ones we
    // have asked to cancel, because the cancel has not landed yet.
    std::unordered_map<match::OrderId, Outstanding> live_;
    std::int64_t position_ = 0;
};

}  // namespace sim

#endif  // SIM_STRATEGIES_FIXED_OFFSET_MAKER_HPP
