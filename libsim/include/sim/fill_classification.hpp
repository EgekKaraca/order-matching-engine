// fill_classification.hpp — was this fill given to you, or taken from you?
//
// arXiv:2409.12721 calls a fill ADVERSE when the order is "picked off": right
// after execution the new position is already out of the money. The rest are
// benign — the fills a market maker is actually in business to get.
//
// Splitting PnL on that line matters because a single blended number hides the
// mechanism. A strategy earning a wide spread on benign fills and bleeding it
// back on adverse ones looks identical, in total, to one earning a narrow
// spread cleanly — and only the second survives a change in flow.
//
// The classification reads the FUTURE of each fill. That is legitimate for an
// analyst and forbidden for a strategy, which is why this lives outside the
// Simulator and takes its inputs by value rather than being wired into the
// callback path.
#ifndef SIM_FILL_CLASSIFICATION_HPP
#define SIM_FILL_CLASSIFICATION_HPP

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "match/types.hpp"
#include "sim/types.hpp"

namespace sim {

struct FillSplit {
    std::size_t adverse_count = 0;
    std::size_t benign_count = 0;
    double adverse_pnl = 0.0;  // <= 0 by construction
    double benign_pnl = 0.0;   // >= 0 by construction

    double adverse_fraction() const {
        const auto total = adverse_count + benign_count;
        return total == 0 ? 0.0 : static_cast<double>(adverse_count) / static_cast<double>(total);
    }
};

// Twice the mid at or before `when`, from a time-sorted series.
inline std::int64_t mid_x2_at(const std::vector<std::pair<Time, std::int64_t>>& series,
                              Time when) {
    if (series.empty()) return 0;
    const auto it = std::upper_bound(
        series.begin(), series.end(), when,
        [](Time t, const std::pair<Time, std::int64_t>& entry) { return t < entry.first; });
    if (it == series.begin()) return series.front().second;
    return std::prev(it)->second;
}

// Classifies MAKER fills only. A taker chose its own moment, so calling its
// subsequent loss "adverse selection" would blame the market for the strategy's
// decision.
inline FillSplit classify_fills(const std::vector<Fill>& fills,
                                const std::vector<std::pair<Time, std::int64_t>>& mid_x2_series,
                                Nanos horizon) {
    FillSplit split;
    for (const Fill& fill : fills) {
        if (!fill.maker) continue;

        const double sign = fill.side == match::Side::Buy ? 1.0 : -1.0;
        const double entry = static_cast<double>(fill.price.ticks);
        const double later =
            static_cast<double>(mid_x2_at(mid_x2_series, fill.time + horizon)) / 2.0;
        const double markout = (later - entry) * sign * static_cast<double>(fill.quantity.units);

        // Strictly negative is adverse. A zero mark-out cost nothing and was
        // not a pick-off; counting it as one would inflate the adverse share
        // with every quiet fill in the sample.
        if (markout < 0.0) {
            ++split.adverse_count;
            split.adverse_pnl += markout;
        } else {
            ++split.benign_count;
            split.benign_pnl += markout;
        }
    }
    return split;
}

}  // namespace sim

#endif  // SIM_FILL_CLASSIFICATION_HPP
