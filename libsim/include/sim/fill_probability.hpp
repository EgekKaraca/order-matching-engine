// fill_probability.hpp — the fill-probability surface.
//
// The headline artifact of arXiv:2502.18625: how often a posted order fills, as
// a joint function of where it sits in its own queue and how much depth is on
// the OTHER side. Both axes matter, for different reasons. The near-side queue
// decides how much must drain before you trade at all; the opposite side
// decides whether the price is about to come to you.
//
// That paper measured it with 232,897 live orders. Producing the same surface
// from replay is what allows the two to be compared — and a simulator that
// reproduces a live-money surface is validated in a way no self-consistency
// check can achieve.
#ifndef SIM_FILL_PROBABILITY_HPP
#define SIM_FILL_PROBABILITY_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "sim/order_tracker.hpp"

namespace sim {

class FillSurface {
public:
    // Four queue-position bins and three opposite-depth bins. Deliberately
    // coarse: the sample is one hour, and a finer grid would produce cells with
    // single-digit counts and a fill rate that is noise with a decimal point on
    // it.
    static constexpr std::size_t kQueueBins = 4;
    static constexpr std::size_t kDepthBins = 3;

    // Opposite-depth bin edges in shares, chosen around the ~109-share median
    // touch depth of the sample so the bins are actually populated.
    static constexpr match::AggregateQuantity kDepthEdges[kDepthBins - 1] = {60, 200};

    struct Cell {
        std::size_t posted = 0;
        std::size_t filled = 0;

        bool has_data() const { return posted > 0; }
        double fill_rate() const {
            return posted == 0 ? 0.0 : static_cast<double>(filled) / static_cast<double>(posted);
        }
    };

    void add(const PostedOrder& order) {
        Cell& target =
            cells_[queue_bin(order.normalised_queue_position())][depth_bin(order.opposite_depth)];
        ++target.posted;
        if (order.was_filled()) ++target.filled;
    }

    const Cell& cell(std::size_t queue_bin_index, std::size_t depth_bin_index) const {
        return cells_[queue_bin_index][depth_bin_index];
    }

    static std::size_t queue_bin(double normalised_position) {
        const auto scaled = static_cast<std::size_t>(std::max(0.0, normalised_position) *
                                                     static_cast<double>(kQueueBins));
        // A position of exactly 1.0 is reachable — the back of a level with
        // nothing behind it — and would otherwise index one past the end.
        return std::min(scaled, kQueueBins - 1);
    }

    static std::size_t depth_bin(match::AggregateQuantity opposite_depth) {
        for (std::size_t i = 0; i < kDepthBins - 1; ++i) {
            if (opposite_depth <= kDepthEdges[i]) return i;
        }
        return kDepthBins - 1;
    }

    static const char* queue_bin_label(std::size_t index) {
        static const char* kLabels[kQueueBins] = {"front  (0-25%)", "mid-lo (25-50%)",
                                                  "mid-hi (50-75%)", "back   (75-100%)"};
        return kLabels[index];
    }

private:
    std::array<std::array<Cell, kDepthBins>, kQueueBins> cells_{};
};

}  // namespace sim

#endif  // SIM_FILL_PROBABILITY_HPP
