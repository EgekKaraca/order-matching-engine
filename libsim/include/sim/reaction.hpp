// reaction.hpp — the one assumption this project could not previously express.
//
// Replaying historical flow assumes the strategy's orders do not change anyone
// else's behaviour. That is the deepest limitation of the whole simulator, and
// until now it was a PREMISE rather than a parameter: there was no way to turn
// it off, so no way to measure what it was worth.
//
// This makes it pluggable, with the impact-free case as the explicit default.
// One mechanism is modelled, chosen for being the most direct and the easiest
// to defend: a resting order is VISIBLE, so a participant who was about to join
// the level behind it may instead not bother, seeing the queue as already too
// long to be worth joining. The bigger the strategy's share of the level, the
// more often that happens.
//
// WHAT THIS IS NOT: a price-impact model. It does not move the mid, does not
// model informed traders reacting to the strategy's flow, and does not model
// anyone racing it to the touch. It captures crowding-out and nothing else.
// The honest use is to sweep its intensity and report how much the headline
// depends on it — a sensitivity, not a correction.
//
// The real answers, for the record: agent-based simulation
// (jpmorganchase/abides-jpmc-public) replaces replay with agents that genuinely
// respond; queue-reactive generative models (arXiv:2603.24137) learn the
// conditional dynamics. Both are larger than this file, and both are what to
// reach for if the impact term turns out to dominate.
#ifndef SIM_REACTION_HPP
#define SIM_REACTION_HPP

#include "sim/types.hpp"

#include <cstdint>
#include <random>

#include "match/types.hpp"

namespace sim {

class ReactionModel {
  public:
    virtual ~ReactionModel() = default;

    // Called when a historical participant is about to add liquidity at a price
    // where the strategy is already resting. Returning true means that
    // participant would NOT have joined, had the strategy's order been there.
    //
    // `strategy_size` is the strategy's resting quantity at that level;
    // `level_depth` is the whole level including it.
    virtual bool crowds_out(Time when, match::Side side, match::Price price,
                            match::AggregateQuantity strategy_size,
                            match::AggregateQuantity level_depth) = 0;

    virtual const char* name() const = 0;
};

// The current behaviour, named. Every result produced before this file existed
// was produced under this model, so it remains the baseline.
class NoReaction final : public ReactionModel {
  public:
    bool crowds_out(Time, match::Side, match::Price, match::AggregateQuantity,
                    match::AggregateQuantity) override {
        return false;
    }
    const char* name() const override { return "none (impact-free)"; }
};

// Probability of crowding out equal to `intensity` times the strategy's share
// of the level. Linear because nothing in one hour of one stock justifies a
// fitted curve, and a fitted curve on that sample would be decoration.
class CrowdOutReaction final : public ReactionModel {
  public:
    CrowdOutReaction(double intensity, std::uint64_t seed) : intensity_(intensity), rng_(seed) {}

    bool crowds_out(Time, match::Side, match::Price, match::AggregateQuantity strategy_size,
                    match::AggregateQuantity level_depth) override {
        if (level_depth == 0 || intensity_ <= 0.0) return false;

        const double share = static_cast<double>(strategy_size) / static_cast<double>(level_depth);
        const double probability = intensity_ * share;
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < probability;
    }

    const char* name() const override { return "crowd-out (linear in size share)"; }

    double intensity() const { return intensity_; }

  private:
    double intensity_;
    std::mt19937_64 rng_;
};

}  // namespace sim

#endif  // SIM_REACTION_HPP
