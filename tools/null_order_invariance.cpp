// null_order_invariance.cpp — the gate between reconstruction and simulation.
//
// Phase 5 replays a LOBSTER execution as a targeted reduction of the named
// resting order. That reproduces the venue's book, but the engine never
// matches, so an injected order could never fill — nothing ever aggresses
// against the book.
//
// Phase 6 replays the same execution as an incoming aggressor, letting the
// engine's FIFO decide who fills. That is what allows an injected order to win
// a queue race, and therefore what the study needs. It is also strictly
// stronger, and strictly riskier: the aggressor consumes from the FRONT of the
// level, so it only reproduces history if the reconstructed queue ORDER matches
// the venue's real one.
//
// This tool is the check that they agree. It runs both models over the same
// message file in lockstep and compares the FULL 50-level book after every
// single message — not summary statistics, which can coincide while the books
// differ. With no strategy orders injected, the two must be identical at every
// row. Any divergence is a bug in the aggressor path, and it must be fixed
// before anything is injected: after injection there is no longer a reference
// to compare against, so this is the last moment the question can be settled.
#include "adapters/lobster.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "match/map_book.hpp"
#include "match/report.hpp"
#include "match/types.hpp"

namespace {

constexpr std::size_t kLevels = 50;
constexpr std::uint64_t kAggressorId = (1ULL << 61) + 1;

struct NullSink {
    void operator()(const match::Report&) const {}
};

using Book = match::MapBook<NullSink>;

void snapshot(const Book& book, std::vector<std::int64_t>& out) {
    static Book::LevelView asks[kLevels];
    static Book::LevelView bids[kLevels];
    const std::size_t n_ask = book.top_levels(match::Side::Sell, asks, kLevels);
    const std::size_t n_bid = book.top_levels(match::Side::Buy, bids, kLevels);

    out.clear();
    for (std::size_t i = 0; i < kLevels; ++i) {
        out.push_back(i < n_ask ? asks[i].price.ticks : lobster::kEmptyAskPrice);
        out.push_back(i < n_ask ? static_cast<std::int64_t>(asks[i].quantity) : 0);
        out.push_back(i < n_bid ? bids[i].price.ticks : lobster::kEmptyBidPrice);
        out.push_back(i < n_bid ? static_cast<std::int64_t>(bids[i].quantity) : 0);
    }
}

void seed_from(Book& book, const std::vector<std::int64_t>& row, std::uint64_t& next_id) {
    for (std::size_t i = 0; i < kLevels; ++i) {
        const std::int64_t ask_price = row[4 * i];
        const std::int64_t ask_size = row[4 * i + 1];
        const std::int64_t bid_price = row[4 * i + 2];
        const std::int64_t bid_size = row[4 * i + 3];

        if (ask_price != lobster::kEmptyAskPrice && ask_size > 0) {
            book.on_new_order(match::Order{match::OrderId{next_id++}, match::SeqNum{0},
                                           match::Price{ask_price},
                                           match::Quantity{static_cast<std::uint32_t>(ask_size)},
                                           match::Side::Sell, match::TimeInForce::Gtc});
        }
        if (bid_price != lobster::kEmptyBidPrice && bid_size > 0) {
            book.on_new_order(match::Order{match::OrderId{next_id++}, match::SeqNum{0},
                                           match::Price{bid_price},
                                           match::Quantity{static_cast<std::uint32_t>(bid_size)},
                                           match::Side::Buy, match::TimeInForce::Gtc});
        }
    }
}

void apply_reduction(Book& book, const lobster::Message& m) {
    switch (m.type) {
        case lobster::EventType::NewLimitOrder:
            book.on_new_order(match::Order{match::OrderId{m.order_id}, match::SeqNum{0},
                                           match::Price{m.price}, match::Quantity{m.size}, m.side,
                                           match::TimeInForce::Gtc});
            break;
        case lobster::EventType::PartialCancel:
        case lobster::EventType::VisibleExecution:
        case lobster::EventType::Delete: {
            const match::Quantity want{m.size};
            const match::Quantity got = book.reduce(match::OrderId{m.order_id}, want);
            if (got < want) book.consume_at(m.side, match::Price{m.price}, want - got);
            break;
        }
        default:
            break;  // hidden execution, cross, halt — no visible book change
    }
}

void apply_aggressor(Book& book, const lobster::Message& m) {
    if (m.type != lobster::EventType::VisibleExecution) {
        apply_reduction(book, m);
        return;
    }
    // The direction on an execution is the MAKER's side, so the aggressor is
    // the other one. IOC, because an execution consumes what is there and
    // leaves nothing behind — a resting residual would invent liquidity.
    book.on_new_order(match::Order{match::OrderId{kAggressorId}, match::SeqNum{0},
                                   match::Price{m.price}, match::Quantity{m.size},
                                   match::opposite(m.side), match::TimeInForce::Ioc});
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: null_order_invariance <message_50.csv> <orderbook_50.csv>\n";
        return 2;
    }

    std::ifstream messages(argv[1]);
    std::ifstream orderbook(argv[2]);
    if (!messages || !orderbook) {
        std::cerr << "cannot open input files\n";
        return 1;
    }

    Book reduction_book{NullSink{}};
    Book aggressor_book{NullSink{}};

    {
        std::string row;
        std::vector<std::int64_t> seed(4 * kLevels);
        if (!std::getline(orderbook, row) ||
            !lobster::parse_orderbook_row(row, seed.data(), kLevels)) {
            std::cerr << "cannot read the seeding orderbook row\n";
            return 1;
        }
        // Both books are seeded identically, from the same snapshot, so any
        // later difference is attributable to the execution model alone.
        std::uint64_t a = 1ULL << 60;
        std::uint64_t b = 1ULL << 60;
        seed_from(reduction_book, seed, a);
        seed_from(aggressor_book, seed, b);

        std::string discard;
        std::getline(messages, discard);
    }

    std::vector<std::int64_t> left;
    std::vector<std::int64_t> right;
    std::string line;
    std::size_t rows = 0;
    std::size_t divergent = 0;
    std::size_t first_divergence = 0;

    while (std::getline(messages, line)) {
        ++rows;
        lobster::Message m;
        if (!lobster::parse_message(line, m)) continue;

        apply_reduction(reduction_book, m);
        apply_aggressor(aggressor_book, m);

        snapshot(reduction_book, left);
        snapshot(aggressor_book, right);

        if (left != right) {
            ++divergent;
            if (first_divergence == 0) {
                first_divergence = rows;
                std::printf("first divergence at row %zu (message type %d, order %llu)\n", rows,
                            static_cast<int>(m.type), static_cast<unsigned long long>(m.order_id));
                for (std::size_t i = 0; i < 4; ++i) {
                    std::printf("  level 1 col %zu: reduction=%lld aggressor=%lld\n", i,
                                static_cast<long long>(left[i]), static_cast<long long>(right[i]));
                }
            }
        }
    }

    std::printf("rows replayed   : %zu\n", rows);
    std::printf("divergent rows  : %zu\n", divergent);

    if (divergent == 0) {
        std::printf("\nNULL-ORDER INVARIANCE HOLDS: with no strategy orders injected, replaying\n");
        std::printf("executions as aggressors reproduces the targeted-reduction book exactly,\n");
        std::printf("at all %zu levels, after every message.\n", kLevels);
        std::printf("\nWhat this does NOT prove: a published book is a DEPTH view, so it is\n");
        std::printf("blind to which order at a level was consumed. Two engines that pick\n");
        std::printf("different orders from the same level produce identical depth. Queue\n");
        std::printf("ORDER is measured separately, by lobster_oracle's queue-position\n");
        std::printf("agreement statistic, and that one is not 100%%.\n");
        return 0;
    }
    std::printf("\nNULL-ORDER INVARIANCE VIOLATED on %zu of %zu rows.\n", divergent, rows);
    std::printf("The aggressor path must be fixed before anything is injected: after\n");
    std::printf("injection there is no reference left to compare against.\n");
    return 1;
}
