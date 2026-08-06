// fill_model_study.cpp — the deliverable.
//
// The identical strategy, on identical data, with identical latency, under
// three fill models. Everything else in this repository is infrastructure for
// the number this produces:
//
//     gap = (PnL_A - PnL_C) / |PnL_A|
//
// how much a naive "filled at the touch" assumption inflates a passive market
// maker's PnL, once real queue position, order latency and adverse selection
// are accounted for.
//
// Metrics are defined before anything is run, because an ambiguous metric
// silently becomes a wrong headline number:
//
//   fill rate        filled qty / posted qty
//   spread capture   sum over fills of (fill_price - mid(t_fill)) * side_sign
//   mark-out @ h     (mid(t_fill+h) - fill_price) * side_sign * qty.
//                    NEGATIVE MEANS ADVERSE SELECTION — the cost the naive
//                    model cannot see, because it does not know which fills it
//                    would not have got.
//   net PnL          cash + inventory marked to the closing mid, net of fees
//   queue at fill    size resting ahead when the order was placed. Model A's is
//                    identically zero; that contrast is the whole intuition.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "adapters/lobster.hpp"
#include "adapters/lobster_events.hpp"
#include "sim/fill_model.hpp"
#include "sim/latency.hpp"
#include "sim/simulator.hpp"
#include "sim/strategies/fixed_offset_maker.hpp"
#include "sim/types.hpp"

namespace {

constexpr std::size_t kLevels = 50;
constexpr double kUnitsPerDollar = 10000.0;

struct Result {
    std::string model;
    std::size_t fills = 0;
    std::int64_t posted = 0;
    std::int64_t filled = 0;
    std::int64_t position = 0;
    std::int64_t fees = 0;
    double pnl = 0.0;
    double spread_capture = 0.0;
    double markout_1s = 0.0;
    double markout_10s = 0.0;
    double markout_60s = 0.0;
    std::int64_t queue_median = 0;
    std::int64_t queue_p90 = 0;
};

std::vector<sim::MarketEvent> load_events(const std::string& path) {
    std::vector<sim::MarketEvent> events;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);  // row 0 produced the seeding snapshot
    while (std::getline(in, line)) {
        lobster::Message m;
        if (!lobster::parse_message(line, m)) continue;
        if (const auto event = lobster::to_market_event(m)) events.push_back(*event);
    }
    return events;
}

std::vector<std::int64_t> load_seed(const std::string& path) {
    std::ifstream in(path);
    std::string row;
    std::vector<std::int64_t> seed(4 * kLevels);
    if (!std::getline(in, row) || !lobster::parse_orderbook_row(row, seed.data(), kLevels)) {
        seed.clear();
    }
    return seed;
}

// Twice the mid at or before `when`. The series is sorted, so this is a binary
// search. A mark-out deliberately reads the FUTURE of a fill — which the
// strategy must never do, but the analyst must.
std::int64_t mid_at(const std::vector<std::pair<sim::Time, std::int64_t>>& series,
                    sim::Time when) {
    if (series.empty()) return 0;
    const auto it = std::upper_bound(series.begin(), series.end(), when,
                                     [](sim::Time t, const auto& entry) { return t < entry.first; });
    if (it == series.begin()) return series.front().second;
    return std::prev(it)->second;
}

std::int64_t percentile(std::vector<std::int64_t> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(p * static_cast<double>(values.size() - 1))];
}

Result run(sim::FillModel& model, const std::vector<sim::MarketEvent>& events,
           const std::vector<std::int64_t>& seed, std::int64_t offset,
           std::int64_t order_latency_us) {
    sim::FixedOffsetMakerConfig strategy_config;
    strategy_config.offset = offset;
    strategy_config.size = match::Quantity{100};
    strategy_config.max_position = 1000;
    sim::FixedOffsetMaker strategy{strategy_config};

    sim::ConstantDelay feed_delay{sim::micros(100)};
    sim::ConstantDelay order_delay{sim::micros(order_latency_us)};

    sim::SimulatorConfig config;
    config.fees = sim::FeeSchedule{-2, 3};

    sim::Simulator simulator(strategy, feed_delay, order_delay, model, config);

    for (std::size_t i = 0; i < kLevels; ++i) {
        if (seed[4 * i] != lobster::kEmptyAskPrice && seed[4 * i + 1] > 0) {
            simulator.seed_level(match::Side::Sell, match::Price{seed[4 * i]},
                                 match::Quantity{static_cast<std::uint32_t>(seed[4 * i + 1])});
        }
        if (seed[4 * i + 2] != lobster::kEmptyBidPrice && seed[4 * i + 3] > 0) {
            simulator.seed_level(match::Side::Buy, match::Price{seed[4 * i + 2]},
                                 match::Quantity{static_cast<std::uint32_t>(seed[4 * i + 3])});
        }
    }

    for (const auto& event : events) simulator.on_market_event(event);
    simulator.finish();

    const auto& fills = simulator.fills();
    const auto& mids = simulator.mid_series();

    Result result;
    result.model = model.name();
    result.fills = fills.size();
    result.posted = static_cast<std::int64_t>(simulator.stats().strategy_submits) *
                    strategy_config.size.units;
    result.position = simulator.portfolio().position();
    result.fees = simulator.portfolio().fees_paid();
    result.pnl = static_cast<double>(
                     simulator.portfolio().pnl_x2(mids.empty() ? 0 : mids.back().second)) /
                 2.0 / kUnitsPerDollar;

    std::vector<std::int64_t> queue;
    queue.reserve(fills.size());
    for (const auto& fill : fills) {
        const auto qty = static_cast<double>(fill.quantity.units);
        const double sign = fill.side == match::Side::Buy ? 1.0 : -1.0;
        result.filled += fill.quantity.units;
        queue.push_back(static_cast<std::int64_t>(fill.queue_ahead_at_entry));

        // Spread capture: what the passive fill earned against the mid at the
        // moment it happened. A buy below the mid is positive.
        const double mid_now = static_cast<double>(mid_at(mids, fill.time)) / 2.0;
        result.spread_capture +=
            (mid_now - static_cast<double>(fill.price.ticks)) * sign * qty / kUnitsPerDollar;

        // Mark-outs: where the mid went AFTER the fill. Negative is adverse
        // selection — the fills that were granted were the ones worth granting
        // to somebody else.
        const auto markout = [&](sim::Nanos horizon) {
            const double later = static_cast<double>(mid_at(mids, fill.time + horizon)) / 2.0;
            return (later - static_cast<double>(fill.price.ticks)) * sign * qty / kUnitsPerDollar;
        };
        result.markout_1s += markout(sim::seconds(1));
        result.markout_10s += markout(sim::seconds(10));
        result.markout_60s += markout(sim::seconds(60));
    }
    result.queue_median = percentile(queue, 0.50);
    result.queue_p90 = percentile(queue, 0.90);
    return result;
}

void print_table(const std::vector<Result>& results) {
    std::printf("%-38s %8s %8s %9s %9s %10s %10s %10s %8s %8s\n", "fill model", "fills",
                "fill%", "spread$", "mo@1s", "mo@10s", "mo@60s", "netPnL$", "qMed", "qP90");
    for (const auto& r : results) {
        const double fill_pct =
            r.posted == 0 ? 0.0 : 100.0 * static_cast<double>(r.filled) /
                                      static_cast<double>(r.posted);
        std::printf("%-38s %8zu %7.2f%% %9.0f %9.0f %10.0f %10.0f %10.0f %8lld %8lld\n",
                    r.model.c_str(), r.fills, fill_pct, r.spread_capture, r.markout_1s,
                    r.markout_10s, r.markout_60s, r.pnl,
                    static_cast<long long>(r.queue_median),
                    static_cast<long long>(r.queue_p90));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: fill_model_study <message_50.csv> <orderbook_50.csv> "
                     "[offset_units]\n";
        return 2;
    }
    const std::int64_t offset = argc > 3 ? std::stoll(argv[3]) : 3000;

    const auto seed = load_seed(argv[2]);
    if (seed.empty()) {
        std::cerr << "cannot read the seeding orderbook row\n";
        return 1;
    }
    const auto events = load_events(argv[1]);
    std::printf("events: %zu, offset: %lld units, feed latency 100us\n\n", events.size(),
                static_cast<long long>(offset));

    // --- the three models at a fixed latency --------------------------------
    std::printf("=== the comparison (order latency 500us) ===\n");
    std::vector<Result> results;
    {
        sim::NaiveFill a;
        sim::ProbQueueFill b{1.0};
        sim::L3ExactFill c;
        results.push_back(run(a, events, seed, offset, 500));
        results.push_back(run(b, events, seed, offset, 500));
        results.push_back(run(c, events, seed, offset, 500));
    }
    print_table(results);

    const double pnl_a = results[0].pnl;
    const double pnl_b = results[1].pnl;
    const double pnl_c = results[2].pnl;
    const double gap = pnl_a == 0.0 ? 0.0 : (pnl_a - pnl_c) / std::abs(pnl_a);

    std::printf("\nTHE GAP  (PnL_A - PnL_C) / |PnL_A|  =  %.1f%%\n", 100.0 * gap);
    std::printf("  A naive: $%.0f    B estimated: $%.0f    C L3 exact: $%.0f\n", pnl_a, pnl_b,
                pnl_c);

    // --- sensitivity to order latency ---------------------------------------
    std::printf("\n=== how the gap varies with order latency ===\n");
    std::printf("%12s %12s %12s %12s %10s\n", "latency(us)", "PnL_A", "PnL_B", "PnL_C", "gap");
    for (const std::int64_t latency : {0, 100, 500, 1000, 5000, 20000, 100000}) {
        sim::NaiveFill a;
        sim::ProbQueueFill b{1.0};
        sim::L3ExactFill c;
        const auto ra = run(a, events, seed, offset, latency);
        const auto rb = run(b, events, seed, offset, latency);
        const auto rc = run(c, events, seed, offset, latency);
        const double g = ra.pnl == 0.0 ? 0.0 : (ra.pnl - rc.pnl) / std::abs(ra.pnl);
        std::printf("%12lld %12.0f %12.0f %12.0f %9.1f%%\n", static_cast<long long>(latency),
                    ra.pnl, rb.pnl, rc.pnl, 100.0 * g);
    }

    std::printf("\nOne hour of one stock on one day. The numbers are real; their error\n");
    std::printf("bars are not estimated, and a single point estimate is not a finding.\n");
    return 0;
}
