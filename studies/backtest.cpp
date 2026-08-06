// backtest.cpp — run the passive market maker over real L3 data.
//
// One strategy, one dataset, one fill model (L3 exact). This is the harness the
// Phase 7 study will run three ways; for now it establishes that a strategy can
// be injected into replayed flow, earn its fills through the queue, and be
// accounted for.
//
// Nothing here is a result yet. A single strategy under a single fill model
// produces a PnL, and a PnL on its own is exactly the kind of number this
// project exists to be suspicious of. The result is the DIFFERENCE between fill
// models, which needs Phase 7.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "adapters/lobster.hpp"
#include "adapters/lobster_events.hpp"
#include "sim/latency.hpp"
#include "sim/simulator.hpp"
#include "sim/strategies/fixed_offset_maker.hpp"
#include "sim/types.hpp"

namespace {

constexpr std::size_t kLevels = 50;

// LOBSTER prices are in units of $0.0001, so a PnL in (price unit x share)
// divides by this to reach dollars.
constexpr double kUnitsPerDollar = 10000.0;

std::int64_t percentile(std::vector<std::int64_t> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    return values[index];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: backtest <message_50.csv> <orderbook_50.csv> "
                     "[offset_units] [order_latency_us]\n";
        return 2;
    }
    const std::int64_t offset = argc > 3 ? std::stoll(argv[3]) : 100;
    const std::int64_t order_latency_us = argc > 4 ? std::stoll(argv[4]) : 500;

    std::ifstream messages(argv[1]);
    std::ifstream orderbook(argv[2]);
    if (!messages || !orderbook) {
        std::cerr << "cannot open input files\n";
        return 1;
    }

    sim::FixedOffsetMakerConfig strategy_config;
    strategy_config.offset = offset;
    strategy_config.size = match::Quantity{100};
    strategy_config.max_position = 1000;
    sim::FixedOffsetMaker strategy{strategy_config};

    // Both delays constant to start with, so the result is attributable to one
    // parameter at a time. Phase 7 sweeps the order latency.
    sim::ConstantDelay feed_delay{sim::micros(100)};
    sim::ConstantDelay order_delay{sim::micros(order_latency_us)};

    sim::SimulatorConfig config;
    // A representative US equity maker-taker schedule, in price units per
    // share: a rebate for providing, a fee for taking. The strategy is
    // passive, so it should be collecting the first.
    config.fees = sim::FeeSchedule{-2, 3};

    sim::Simulator simulator(strategy, feed_delay, order_delay, config);

    // Seed from published row 0, exactly as the reconstruction oracle does.
    {
        std::string row;
        std::vector<std::int64_t> seed(4 * kLevels);
        if (!std::getline(orderbook, row) ||
            !lobster::parse_orderbook_row(row, seed.data(), kLevels)) {
            std::cerr << "cannot read the seeding orderbook row\n";
            return 1;
        }
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
        std::string discard;
        std::getline(messages, discard);
    }

    std::string line;
    std::size_t skipped = 0;
    while (std::getline(messages, line)) {
        lobster::Message m;
        if (!lobster::parse_message(line, m)) continue;
        const auto event = lobster::to_market_event(m);
        // Hidden executions, crosses and halts are dropped by the adapter —
        // they leave the displayed book untouched.
        if (event.has_value()) {
            simulator.on_market_event(*event);
        } else {
            ++skipped;
        }
    }
    simulator.finish();

    const auto& stats = simulator.stats();
    const auto& portfolio = simulator.portfolio();
    const auto& fills = simulator.fills();
    const auto& mids = simulator.mid_series();

    const std::int64_t final_mid_x2 = mids.empty() ? 0 : mids.back().second;
    const double pnl_dollars =
        static_cast<double>(portfolio.pnl_x2(final_mid_x2)) / 2.0 / kUnitsPerDollar;

    std::printf("strategy             : %s (offset %lld units, %u shares)\n", strategy.name(),
                static_cast<long long>(offset), strategy_config.size.units);
    std::printf("feed latency         : %lld us\n", 100LL);
    std::printf("order latency        : %lld us\n", static_cast<long long>(order_latency_us));
    std::printf("\n");
    std::printf("market events        : %zu\n", stats.events);
    std::printf("events skipped       : %zu  (hidden / cross / halt)\n", skipped);
    std::printf("strategy callbacks   : %zu\n", stats.callbacks);
    std::printf("orders submitted     : %zu\n", stats.strategy_submits);
    std::printf("orders cancelled     : %zu\n", stats.strategy_cancels);
    std::printf("cancels too late     : %zu  (order latency lost the race)\n",
                stats.rejected_cancels);
    std::printf("fills                : %zu\n", stats.strategy_fills);
    std::printf("\n");
    std::printf("shares bought        : %lld\n", static_cast<long long>(portfolio.bought()));
    std::printf("shares sold          : %lld\n", static_cast<long long>(portfolio.sold()));
    std::printf("closing position     : %lld\n", static_cast<long long>(portfolio.position()));
    std::printf("  strategy believes   : %lld  (must agree, or the strategy is flying blind)\n",
                static_cast<long long>(strategy.position()));
    std::printf("fees paid            : $%.2f  (negative = rebate earned)\n",
                static_cast<double>(portfolio.fees_paid()) / kUnitsPerDollar);
    std::printf("net PnL              : $%.2f  (inventory marked to mid)\n", pnl_dollars);

    if (!fills.empty()) {
        std::vector<std::int64_t> ahead;
        ahead.reserve(fills.size());
        for (const auto& fill : fills) {
            ahead.push_back(static_cast<std::int64_t>(fill.queue_ahead_at_entry));
        }
        std::printf("\nqueue depth ahead at entry, over filled orders\n");
        std::printf("  median : %lld shares\n",
                    static_cast<long long>(percentile(ahead, 0.50)));
        std::printf("  p90    : %lld shares\n", static_cast<long long>(percentile(ahead, 0.90)));
        std::printf("  max    : %lld shares\n", static_cast<long long>(percentile(ahead, 1.00)));
        std::printf("\nA naive 'filled at the touch' model assumes every one of these is 0.\n");
        std::printf("That assumption is the thing the Phase 7 study prices.\n");
    }
    return 0;
}
