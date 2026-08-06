// momentum_taker.hpp — a directional strategy that crosses the spread.
//
// It exists for two reasons, neither of which is that it makes money.
//
// First, it proves the strategy API generalises. Everything else in this
// repository was built around a passive market maker, and a framework shaped
// entirely by one strategy is a framework with one strategy's assumptions
// baked into it. This one is its opposite in every respect: it takes liquidity
// instead of providing it, holds directional risk instead of flat inventory,
// pays the spread instead of earning it, and pays the taker fee instead of
// collecting the rebate.
//
// Second, it is the natural counterpart to the fill-model study. A passive
// strategy is destroyed by an optimistic FILL assumption; an aggressive one is
// destroyed by an optimistic COST assumption. A backtester that fills takers at
// the mid, or ignores the taker fee, will show this class of strategy working
// when it does not.
//
// The signal is deliberately trivial — mid versus its own exponential moving
// average — because a good signal would make this a trading strategy rather
// than an instrument for testing the machinery.
#ifndef SIM_STRATEGIES_MOMENTUM_TAKER_HPP
#define SIM_STRATEGIES_MOMENTUM_TAKER_HPP

#include <cstdint>

#include "match/types.hpp"
#include "sim/strategy.hpp"
#include "sim/types.hpp"

namespace sim {

struct MomentumTakerConfig {
    // EMA smoothing, as a reciprocal: the average moves by 1/alpha_inverse of
    // the gap each update. Larger is slower.
    std::int64_t alpha_inverse = 64;
    // How far the mid must sit from its average, in price units, before acting.
    // Below this the strategy would be paying the spread to trade noise.
    std::int64_t threshold = 200;
    match::Quantity size{100};
    std::int64_t max_position = 500;
    // Minimum gap between trades. Without it the strategy fires on every event
    // that clears the threshold and the result measures the event rate rather
    // than the signal.
    Nanos cooldown = millis(100);
};

class MomentumTaker final : public Strategy {
public:
    explicit MomentumTaker(MomentumTakerConfig config = {}) : config_(config) {}

    void on_book(const BookView& book, OrderGate& gate) override {
        const auto mid_x2 = book.mid_x2();
        if (!mid_x2.has_value()) return;

        if (!seeded_) {
            ema_x2_ = *mid_x2;
            seeded_ = true;
            return;
        }
        // Integer EMA: no floating point anywhere near a price, here as in the
        // engine. The division truncates toward zero, which biases the average
        // by less than one unit and never accumulates.
        ema_x2_ += (*mid_x2 - ema_x2_) / config_.alpha_inverse;

        if (book.now() - last_trade_ < config_.cooldown) return;

        const std::int64_t deviation_x2 = *mid_x2 - ema_x2_;

        // Crossing costs the spread, so the threshold has to clear it to be
        // worth doing at all. Comparing doubled quantities throughout keeps
        // the half-tick mid exact.
        if (deviation_x2 > 2 * config_.threshold && position_ < config_.max_position) {
            gate.take(match::Side::Buy, book.best_ask(), config_.size);
            last_trade_ = book.now();
        } else if (deviation_x2 < -2 * config_.threshold && position_ > -config_.max_position) {
            gate.take(match::Side::Sell, book.best_bid(), config_.size);
            last_trade_ = book.now();
        }
    }

    void on_fill(const Fill& fill, const BookView&, OrderGate&) override {
        const auto qty = static_cast<std::int64_t>(fill.quantity.units);
        position_ += fill.side == match::Side::Buy ? qty : -qty;
    }

    const char* name() const override { return "momentum taker"; }

    std::int64_t position() const { return position_; }

private:
    MomentumTakerConfig config_;
    std::int64_t ema_x2_ = 0;
    bool seeded_ = false;
    std::int64_t position_ = 0;
    Time last_trade_{};
};

}  // namespace sim

#endif  // SIM_STRATEGIES_MOMENTUM_TAKER_HPP
