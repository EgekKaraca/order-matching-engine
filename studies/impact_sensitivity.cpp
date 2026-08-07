// impact_sensitivity.cpp — how much does the result depend on nobody noticing?
//
// The simulator is queue-realistic and impact-free. That second half is its
// deepest assumption, and the useful question is not "can we remove it" —
// removing it properly needs agent-based simulation — but "how much of the
// answer rests on it".
//
// So: sweep the crowd-out intensity from 0 (the published baseline) upward and
// watch the gap. If the gap is stable, the headline is robust to the
// assumption. If it collapses, the headline was an artefact of it. Either
// answer is worth having and only one of them is comfortable.
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
#include "sim/reaction.hpp"
#include "sim/simulator.hpp"
#include "sim/strategies/fixed_offset_maker.hpp"
#include "sim/types.hpp"

namespace {

constexpr std::size_t kLevels = 50;
constexpr double kUnitsPerDollar = 10000.0;

struct Outcome {
    double pnl = 0.0;
    std::size_t fills = 0;
    std::size_t crowded_out = 0;
};

std::vector<std::int64_t> load_seed(const std::string& path) {
    std::ifstream in(path);
    std::string row;
    std::vector<std::int64_t> seed(4 * kLevels);
    if (!std::getline(in, row) || !lobster::parse_orderbook_row(row, seed.data(), kLevels)) {
        seed.clear();
    }
    return seed;
}

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

Outcome run(sim::FillModel& model, sim::ReactionModel* reaction,
            const std::vector<sim::MarketEvent>& events, const std::vector<std::int64_t>& seed,
            std::uint32_t order_size) {
    sim::FixedOffsetMakerConfig strategy_config;
    strategy_config.mode = sim::QuoteMode::JoinTouch;
    strategy_config.size = match::Quantity{order_size};
    strategy_config.max_position = 10 * static_cast<std::int64_t>(order_size);
    sim::FixedOffsetMaker strategy{strategy_config};

    sim::ConstantDelay feed_delay{sim::micros(100)};
    sim::ConstantDelay order_delay{sim::micros(500)};
    sim::SimulatorConfig config;
    config.fees = sim::FeeSchedule{-2, 3};

    sim::Simulator simulator(strategy, feed_delay, order_delay, model, config, reaction);
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

    const auto& mids = simulator.mid_series();
    Outcome outcome;
    outcome.pnl =
        static_cast<double>(simulator.portfolio().pnl_x2(mids.empty() ? 0 : mids.back().second)) /
        2.0 / kUnitsPerDollar;
    outcome.fills = simulator.stats().strategy_fills;
    outcome.crowded_out = simulator.stats().crowded_out;
    return outcome;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: impact_sensitivity <message_50.csv> <orderbook_50.csv> "
                     "[order_size] [csv_out]\n";
        return 2;
    }
    const auto order_size = static_cast<std::uint32_t>(argc > 3 ? std::stoul(argv[3]) : 10);
    const std::string csv_out = argc > 4 ? argv[4] : "";

    const auto seed = load_seed(argv[2]);
    if (seed.empty()) {
        std::cerr << "cannot read the seeding orderbook row\n";
        return 1;
    }
    const auto events = load_events(argv[1]);

    std::printf("Order size %u shares. Intensity 0 is the published baseline: a strictly\n",
                order_size);
    std::printf("impact-free replay. Higher values mean historical participants\n");
    std::printf("increasingly decline to queue behind the strategy.\n\n");
    std::printf("%10s %12s %12s %10s %10s %12s\n", "intensity", "PnL_A", "PnL_C", "gap",
                "C fills", "crowded out");

    std::vector<std::string> rows;
    for (const double intensity : {0.0, 0.25, 0.5, 1.0, 2.0}) {
        sim::NaiveFill naive;
        sim::L3ExactFill exact;
        // The same seed across intensities, so the comparison isolates
        // intensity rather than luck.
        sim::CrowdOutReaction reaction_a{intensity, 20260807};
        sim::CrowdOutReaction reaction_c{intensity, 20260807};

        const Outcome a = run(naive, &reaction_a, events, seed, order_size);
        const Outcome c = run(exact, &reaction_c, events, seed, order_size);
        const double gap = a.pnl == 0.0 ? 0.0 : (a.pnl - c.pnl) / std::abs(a.pnl);

        std::printf("%10.2f %12.0f %12.0f %9.1f%% %10zu %12zu\n", intensity, a.pnl, c.pnl,
                    100.0 * gap, c.fills, c.crowded_out);
        rows.push_back(std::to_string(intensity) + "," + std::to_string(a.pnl) + "," +
                       std::to_string(c.pnl) + "," + std::to_string(gap) + "," +
                       std::to_string(c.crowded_out));
    }

    std::printf("\nRead this as a SENSITIVITY, not a correction. The crowd-out model\n");
    std::printf("captures one mechanism — participants declining to queue behind a\n");
    std::printf("visible order — and does not move the mid, model informed flow, or\n");
    std::printf("model anyone racing to the touch. If the gap holds across the sweep the\n");
    std::printf("headline does not rest on the impact-free assumption. If it collapses,\n");
    std::printf("it did.\n");

    if (!csv_out.empty()) {
        std::ofstream out(csv_out);
        out << "intensity,pnl_a,pnl_c,gap,crowded_out\n";
        for (const auto& row : rows) out << row << '\n';
        std::printf("\nwrote %s\n", csv_out.c_str());
    }
    return 0;
}
