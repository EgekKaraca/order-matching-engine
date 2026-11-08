// backtest_runner.cpp — run a strategy over L3 data and report on it.
//
// This is the general entry point: pick a strategy, pick a fill model, pick
// latencies, get a performance report. The fill-model study is a specialised
// use of the same machinery; this is the one you point at a new strategy.
//
// What it is honest about: one instrument, one session, no market impact. It
// is a single-instrument execution backtester, not a portfolio system.
#include "adapters/lobster.hpp"
#include "adapters/lobster_events.hpp"
#include "sim/fill_model.hpp"
#include "sim/latency.hpp"
#include "sim/performance.hpp"
#include "sim/simulator.hpp"
#include "sim/strategies/fixed_offset_maker.hpp"
#include "sim/strategies/momentum_taker.hpp"
#include "sim/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kLevels = 50;
constexpr double kUnitsPerDollar = 10000.0;

struct Options {
    std::string messages;
    std::string orderbook;
    std::string strategy = "maker";
    std::string fill_model = "l3";
    std::int64_t feed_latency_us = 100;
    std::int64_t order_latency_us = 500;
    std::uint32_t size = 100;
    std::int64_t maker_fee = -2;
    std::int64_t taker_fee = 3;
    std::string equity_csv;
};

void usage() {
    std::cerr <<
        R"(usage: backtest_runner <messages.csv> <orderbook.csv> [options]

  --strategy maker|taker      which strategy to run          (default maker)
  --fill-model l3|naive|prob  how fills are decided          (default l3)
  --feed-latency-us N         book updates are seen this late (default 100)
  --order-latency-us N        orders arrive this late         (default 500)
  --size N                    order size in shares            (default 100)
  --maker-fee N               per share, negative = rebate    (default -2)
  --taker-fee N               per share                       (default 3)
  --equity-csv PATH           write the equity curve there
)";
}

bool parse(int argc, char** argv, Options& out) {
    if (argc < 3) return false;
    out.messages = argv[1];
    out.orderbook = argv[2];

    for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };

        if (flag == "--strategy") {
            const char* v = next();
            if (v == nullptr) return false;
            out.strategy = v;
        } else if (flag == "--fill-model") {
            const char* v = next();
            if (v == nullptr) return false;
            out.fill_model = v;
        } else if (flag == "--feed-latency-us") {
            const char* v = next();
            if (v == nullptr) return false;
            out.feed_latency_us = std::stoll(v);
        } else if (flag == "--order-latency-us") {
            const char* v = next();
            if (v == nullptr) return false;
            out.order_latency_us = std::stoll(v);
        } else if (flag == "--size") {
            const char* v = next();
            if (v == nullptr) return false;
            out.size = static_cast<std::uint32_t>(std::stoul(v));
        } else if (flag == "--maker-fee") {
            const char* v = next();
            if (v == nullptr) return false;
            out.maker_fee = std::stoll(v);
        } else if (flag == "--taker-fee") {
            const char* v = next();
            if (v == nullptr) return false;
            out.taker_fee = std::stoll(v);
        } else if (flag == "--equity-csv") {
            const char* v = next();
            if (v == nullptr) return false;
            out.equity_csv = v;
        } else {
            std::cerr << "unknown option: " << flag << "\n";
            return false;
        }
    }
    return true;
}

std::unique_ptr<sim::Strategy> make_strategy(const Options& options) {
    if (options.strategy == "maker") {
        sim::FixedOffsetMakerConfig config;
        config.mode = sim::QuoteMode::JoinTouch;
        config.size = match::Quantity{options.size};
        config.max_position = 10 * static_cast<std::int64_t>(options.size);
        return std::make_unique<sim::FixedOffsetMaker>(config);
    }
    if (options.strategy == "taker") {
        sim::MomentumTakerConfig config;
        config.size = match::Quantity{options.size};
        config.max_position = 5 * static_cast<std::int64_t>(options.size);
        return std::make_unique<sim::MomentumTaker>(config);
    }
    return nullptr;
}

std::unique_ptr<sim::FillModel> make_fill_model(const std::string& name) {
    if (name == "l3") return std::make_unique<sim::L3ExactFill>();
    if (name == "naive") return std::make_unique<sim::NaiveFill>();
    if (name == "prob") return std::make_unique<sim::ProbQueueFill>();
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        usage();
        return 2;
    }

    auto strategy = make_strategy(options);
    if (strategy == nullptr) {
        std::cerr << "unknown strategy: " << options.strategy << "\n";
        return 2;
    }
    auto fill_model = make_fill_model(options.fill_model);
    if (fill_model == nullptr) {
        std::cerr << "unknown fill model: " << options.fill_model << "\n";
        return 2;
    }

    std::ifstream orderbook(options.orderbook);
    std::vector<std::int64_t> seed(4 * kLevels);
    std::string row;
    if (!orderbook || !std::getline(orderbook, row) ||
        !lobster::parse_orderbook_row(row, seed.data(), kLevels)) {
        std::cerr << "cannot read the seeding orderbook row from " << options.orderbook << "\n";
        return 1;
    }

    sim::ConstantDelay feed_delay{sim::micros(options.feed_latency_us)};
    sim::ConstantDelay order_delay{sim::micros(options.order_latency_us)};
    sim::SimulatorConfig config;
    config.fees = sim::FeeSchedule{options.maker_fee, options.taker_fee};

    sim::Simulator simulator(*strategy, feed_delay, order_delay, *fill_model, config);
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

    std::ifstream messages(options.messages);
    if (!messages) {
        std::cerr << "cannot open " << options.messages << "\n";
        return 1;
    }
    std::string line;
    std::getline(messages, line);  // row 0 produced the seeding snapshot
    std::size_t events = 0;
    while (std::getline(messages, line)) {
        lobster::Message m;
        if (!lobster::parse_message(line, m)) continue;
        if (const auto event = lobster::to_market_event(m)) {
            simulator.on_market_event(*event);
            ++events;
        }
    }
    simulator.finish();

    // One configuration, run once, as specified on the command line. Nothing
    // was searched over, so the multiple-trials penalty is genuinely 1.
    const sim::PerformanceAnalyzer analyzer(simulator.fills(), simulator.mid_series(),
                                            kUnitsPerDollar, /*trials=*/1);
    const auto report = analyzer.analyse(sim::seconds(1), sim::seconds(10));
    const auto& stats = simulator.stats();

    std::printf("strategy       : %s\n", strategy->name());
    std::printf("fill model     : %s\n", fill_model->name());
    std::printf("latency        : feed %lld us, order %lld us\n",
                static_cast<long long>(options.feed_latency_us),
                static_cast<long long>(options.order_latency_us));
    std::printf("fees per share : maker %lld, taker %lld\n",
                static_cast<long long>(options.maker_fee),
                static_cast<long long>(options.taker_fee));
    std::printf("events         : %zu\n\n", events);

    std::printf("--- activity ---\n");
    std::printf("orders sent          : %zu (%zu cancels, %zu too late)\n", stats.strategy_submits,
                stats.strategy_cancels, stats.rejected_cancels);
    std::printf("fills                : %zu  (%zu passive, %zu aggressive)\n", stats.strategy_fills,
                stats.maker_fills, stats.taker_fills);
    std::printf("shares traded        : %lld\n", static_cast<long long>(report.shares_traded));
    std::printf("round trips          : %lld\n", static_cast<long long>(report.round_trips));
    std::printf("turnover             : $%.0f\n", report.turnover);
    std::printf("max abs position     : %lld\n", static_cast<long long>(report.max_abs_position));
    std::printf("time in market       : %.1f%%\n", 100.0 * report.time_in_market);

    std::printf("\n--- result ---\n");
    std::printf("net PnL              : $%.2f\n", report.net_pnl);
    std::printf("max drawdown         : $%.2f\n", report.max_drawdown);
    std::printf("fees paid            : $%.2f  (negative = rebate earned)\n",
                static_cast<double>(simulator.portfolio().fees_paid()) / kUnitsPerDollar);
    std::printf("hit rate (10s)       : %.1f%%\n", 100.0 * report.hit_rate);
    std::printf("avg mark-out (10s)   : $%.4f per fill  (negative = adverse selection)\n",
                report.avg_markout);
    std::printf("stdev of 1s PnL      : $%.2f\n", report.return_stdev);
    std::printf("Sharpe (annualised)  : %.2f  raw\n", report.sharpe_annualised);
    std::printf("Sharpe (deflated)    : %.2f  <- quote this one\n", report.sharpe_deflated);
    std::printf("  skew %.2f, excess kurtosis %.2f, %zu one-second samples.\n", report.return_skew,
                report.return_excess_kurtosis, report.samples);
    std::printf("  The deflated figure applies the Bailey / Lopez de Prado haircut for a\n");
    std::printf("  short sample and the number of variants tried. Annualising one\n");
    std::printf("  intraday hour to 252 days is still a ~1700x extrapolation.\n");

    if (!options.equity_csv.empty()) {
        std::ofstream out(options.equity_csv);
        out << "time_ns,equity,position\n";
        for (const auto& point : analyzer.equity_curve(sim::seconds(1))) {
            out << point.time.ns << ',' << point.equity << ',' << point.position << '\n';
        }
        std::printf("\nwrote equity curve to %s\n", options.equity_csv.c_str());
    }
    return 0;
}
