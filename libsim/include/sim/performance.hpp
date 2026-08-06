// performance.hpp — the numbers a strategy is actually judged on.
//
// PnL alone says almost nothing. A strategy that made $3,000 by holding a
// terrifying position for an hour and one that made $3,000 from four thousand
// small round trips are not the same strategy, and only one of them is
// tradeable. These are the statistics that tell them apart.
//
// Two deliberate choices worth defending:
//
//   - Returns are sampled on a fixed CLOCK, not per trade. Per-trade returns
//     make a strategy look better the more it trades, because each round trip
//     is small and the variance of the series collapses. Clock sampling is
//     what a risk desk would compute.
//   - The Sharpe ratio here is annualised from an intraday sample and is
//     therefore the single most over-claimed number in backtesting. An hour of
//     one day annualised to 252 days is an extrapolation of roughly 1,700x,
//     and it inherits every bit of that leverage in its error. It is reported
//     because it is expected, and labelled for what it is.
#ifndef SIM_PERFORMANCE_HPP
#define SIM_PERFORMANCE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sim/types.hpp"

namespace sim {

struct PerformanceReport {
    // Money, in the price units the run used.
    double net_pnl = 0.0;
    double max_drawdown = 0.0;   // largest peak-to-trough fall in mark-to-market equity
    double peak_equity = 0.0;
    double trough_equity = 0.0;

    // Activity.
    std::size_t fills = 0;
    std::int64_t shares_traded = 0;
    std::int64_t round_trips = 0;   // completed buy+sell pairs, by matched shares
    double turnover = 0.0;          // total traded notional
    std::int64_t max_abs_position = 0;
    double time_in_market = 0.0;    // fraction of samples with a non-zero position

    // Risk-adjusted. See the header note before quoting the Sharpe.
    double return_stdev = 0.0;
    double sharpe_annualised = 0.0;
    std::size_t samples = 0;

    // Per-fill economics.
    double hit_rate = 0.0;      // fraction of fills with a positive mark-out
    double avg_markout = 0.0;   // mean mark-out per fill at the chosen horizon
};

// One point on the equity curve: mark-to-market wealth at a sampled instant.
struct EquityPoint {
    Time time;
    double equity;
    std::int64_t position;
};

// Samples the equity curve on a fixed interval from the fill history and a mid
// series. Kept separate from the Simulator because it reads the FUTURE of every
// fill — legitimate for an analyst, forbidden for a strategy.
class PerformanceAnalyzer {
public:
    PerformanceAnalyzer(const std::vector<Fill>& fills,
                        const std::vector<std::pair<Time, std::int64_t>>& mid_x2_series,
                        double units_per_currency = 1.0)
        : fills_(fills), mids_(mid_x2_series), scale_(units_per_currency) {}

    std::vector<EquityPoint> equity_curve(Nanos interval) const {
        std::vector<EquityPoint> curve;
        if (mids_.empty()) return curve;

        std::size_t next_fill = 0;
        std::int64_t position = 0;
        double cash = 0.0;

        for (Time t = mids_.front().first; t <= mids_.back().first; t = t + interval) {
            while (next_fill < fills_.size() && fills_[next_fill].time <= t) {
                const Fill& fill = fills_[next_fill++];
                const auto qty = static_cast<std::int64_t>(fill.quantity.units);
                const std::int64_t signed_qty = fill.side == match::Side::Buy ? qty : -qty;
                position += signed_qty;
                cash -= static_cast<double>(signed_qty) * static_cast<double>(fill.price.ticks);
            }
            const double mid = static_cast<double>(mid_at(t)) / 2.0;
            curve.push_back(
                EquityPoint{t, (cash + static_cast<double>(position) * mid) / scale_, position});
        }
        return curve;
    }

    PerformanceReport analyse(Nanos sample_interval, Nanos markout_horizon) const {
        PerformanceReport report;
        const auto curve = equity_curve(sample_interval);
        report.samples = curve.size();
        if (curve.empty()) return report;

        report.net_pnl = curve.back().equity;

        // Drawdown on the mark-to-market curve, which is the one that would
        // have triggered a risk limit — not on realised PnL, which conveniently
        // ignores the position you were sitting on at the time.
        double peak = curve.front().equity;
        for (const auto& point : curve) {
            peak = std::max(peak, point.equity);
            report.max_drawdown = std::max(report.max_drawdown, peak - point.equity);
            report.peak_equity = std::max(report.peak_equity, point.equity);
            report.trough_equity = std::min(report.trough_equity, point.equity);
            report.max_abs_position = std::max(report.max_abs_position, std::abs(point.position));
            if (point.position != 0) report.time_in_market += 1.0;
        }
        report.time_in_market /= static_cast<double>(curve.size());

        // Clock-sampled returns, in currency per interval. Left as absolute
        // changes rather than percentages because the strategy has no capital
        // base defined — a percentage return would need one invented, and an
        // invented denominator is how a Sharpe ratio becomes whatever you want.
        std::vector<double> deltas;
        deltas.reserve(curve.size());
        for (std::size_t i = 1; i < curve.size(); ++i) {
            deltas.push_back(curve[i].equity - curve[i - 1].equity);
        }
        if (deltas.size() > 1) {
            double mean = 0.0;
            for (const double d : deltas) mean += d;
            mean /= static_cast<double>(deltas.size());

            double variance = 0.0;
            for (const double d : deltas) variance += (d - mean) * (d - mean);
            variance /= static_cast<double>(deltas.size() - 1);
            report.return_stdev = std::sqrt(variance);

            if (report.return_stdev > 0.0) {
                const double per_interval = mean / report.return_stdev;
                // Intervals in a 252-day year of 6.5-hour sessions.
                const double intervals_per_year =
                    252.0 * 6.5 * 3600.0 * 1e9 / static_cast<double>(sample_interval.count);
                report.sharpe_annualised = per_interval * std::sqrt(intervals_per_year);
            }
        }

        std::int64_t bought = 0;
        std::int64_t sold = 0;
        double markout_total = 0.0;
        std::size_t positive = 0;
        for (const Fill& fill : fills_) {
            const auto qty = static_cast<std::int64_t>(fill.quantity.units);
            report.shares_traded += qty;
            report.turnover +=
                static_cast<double>(qty) * static_cast<double>(fill.price.ticks) / scale_;
            (fill.side == match::Side::Buy ? bought : sold) += qty;

            const double sign = fill.side == match::Side::Buy ? 1.0 : -1.0;
            const double later = static_cast<double>(mid_at(fill.time + markout_horizon)) / 2.0;
            const double markout =
                (later - static_cast<double>(fill.price.ticks)) * sign * static_cast<double>(qty) /
                scale_;
            markout_total += markout;
            if (markout > 0.0) ++positive;
        }
        report.fills = fills_.size();
        report.round_trips = std::min(bought, sold);
        if (!fills_.empty()) {
            report.hit_rate = static_cast<double>(positive) / static_cast<double>(fills_.size());
            report.avg_markout = markout_total / static_cast<double>(fills_.size());
        }
        return report;
    }

private:
    std::int64_t mid_at(Time when) const {
        if (mids_.empty()) return 0;
        const auto it = std::upper_bound(
            mids_.begin(), mids_.end(), when,
            [](Time t, const auto& entry) { return t < entry.first; });
        if (it == mids_.begin()) return mids_.front().second;
        return std::prev(it)->second;
    }

    const std::vector<Fill>& fills_;
    const std::vector<std::pair<Time, std::int64_t>>& mids_;
    double scale_;
};

}  // namespace sim

#endif  // SIM_PERFORMANCE_HPP
