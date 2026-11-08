// latency.hpp — the two delays that separate a backtest from a fantasy.
//
// This is the single largest source of fake alpha in backtesting, and it is
// two distinct effects, not one:
//
//   FEED latency   the strategy sees a book update at t_exchange + d_feed.
//                  Without it, the strategy trades on information it could
//                  not have had.
//   ORDER latency  the strategy's order reaches the book at t_decision +
//                  d_order. Without it, the strategy reacts instantly to that
//                  information.
//
// Both are needed. Modelling only one leaves a strategy that either sees the
// future or acts on it infinitely fast.
//
// The interface is pluggable from the start rather than starting constant and
// being retrofitted, because order latency is the axis the study SWEEPS — "at
// what order latency does the strategy stop working at all?" is one of the
// headline sensitivities, so it has to be a parameter of the design, not a
// constant someone later has to prise out.
//
// Virtual dispatch is fine here: this is consulted once per strategy action,
// not once per message, and it is nowhere near the matching loop.
#ifndef SIM_LATENCY_HPP
#define SIM_LATENCY_HPP

#include "sim/types.hpp"

#include <algorithm>
#include <cstdint>
#include <random>

namespace sim {

class DelayModel {
  public:
    virtual ~DelayModel() = default;

    // The delay applying to something that happens at `when`. Taking the time
    // as an argument is what lets a model vary by time of day — one of the
    // dimensions the study reports the gap across.
    virtual Nanos sample(Time when) = 0;

    virtual const char* name() const = 0;
};

// The starting point: one number, no distribution. Honest and boring, and the
// right thing to sweep first because the result is attributable to exactly one
// parameter.
class ConstantDelay final : public DelayModel {
  public:
    explicit ConstantDelay(Nanos delay) : delay_(delay) {}

    Nanos sample(Time) override { return delay_; }
    const char* name() const override { return "constant"; }

    Nanos value() const { return delay_; }

  private:
    Nanos delay_;
};

// Real latency is right-skewed: a floor set by physics, a typical value near
// it, and a long tail of queueing and retransmission. A lognormal captures
// that shape with two parameters.
//
// Seeded and reproducible, because a backtest whose answer moves between runs
// cannot be argued with.
class LognormalDelay final : public DelayModel {
  public:
    LognormalDelay(Nanos median, double sigma, std::uint64_t seed, Nanos floor = Nanos{0})
        : median_(median), sigma_(sigma), floor_(floor), rng_(seed) {}

    Nanos sample(Time) override {
        // A lognormal's median is exp(mu), so mu = log(median) and the median
        // parameter means what it says.
        const double mu = std::log(static_cast<double>(median_.count));
        std::lognormal_distribution<double> dist(mu, sigma_);
        const auto drawn = static_cast<std::int64_t>(dist(rng_));
        return Nanos{std::max(drawn, floor_.count)};
    }

    const char* name() const override { return "lognormal"; }

  private:
    Nanos median_;
    double sigma_;
    Nanos floor_;
    std::mt19937_64 rng_;
};

// For the null case and for tests: everything is instantaneous. Using this for
// a real run is exactly the assumption the study exists to refute, so it is
// named to make that obvious in a config.
class ZeroDelay final : public DelayModel {
  public:
    Nanos sample(Time) override { return Nanos{0}; }
    const char* name() const override { return "zero (unphysical)"; }
};

}  // namespace sim

#endif  // SIM_LATENCY_HPP
