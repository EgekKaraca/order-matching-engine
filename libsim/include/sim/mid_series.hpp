// mid_series.hpp — one implementation of "what was the mid at time t".
//
// This lookup underpins every mark-out, every adverse/benign classification,
// every point on the equity curve and therefore the Sharpe. It had been
// implemented three separate times — in performance.hpp, in
// fill_classification.hpp, and again inside the study driver — which is three
// places for an off-by-one at the boundary to hide, and a guarantee that two of
// this project's numbers would eventually disagree for no visible reason.
//
// The series is (time, TWICE the mid). Doubling keeps a half-tick mid exact, so
// nothing rounds until the final division; callers halve once at the end.
#ifndef SIM_MID_SERIES_HPP
#define SIM_MID_SERIES_HPP

#include "sim/types.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace sim {

using MidSeries = std::vector<std::pair<Time, std::int64_t>>;

// Twice the mid as of `when`, from a time-sorted series.
//
// "As of" means the most recent sample at or before `when` — never a later
// one. A lookup that rounded to the nearest sample would let a mark-out peek
// microseconds into the future of its own horizon, which is a small leak in
// exactly the direction that flatters a result.
//
// Before the first sample it returns the first value rather than zero: a zero
// mid would register as a catastrophic price move and poison every statistic
// computed from it.
inline std::int64_t mid_x2_at(const MidSeries& series, Time when) {
    if (series.empty()) return 0;
    const auto it = std::upper_bound(
        series.begin(), series.end(), when,
        [](Time t, const std::pair<Time, std::int64_t>& entry) { return t < entry.first; });
    if (it == series.begin()) return series.front().second;
    return std::prev(it)->second;
}

}  // namespace sim

#endif  // SIM_MID_SERIES_HPP
