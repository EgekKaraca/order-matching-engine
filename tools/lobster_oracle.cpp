// lobster_oracle.cpp — replay a LOBSTER message file through libmatch and diff
// the reconstructed book against LOBSTER's own published orderbook file.
//
// This is the simulator's external oracle, mirroring what the benchmark's
// consensus hash is for the engine. It is the difference between "I wrote a
// simulator" and "my simulator provably reconstructs the venue".
//
// The claim it earns is BOUNDED and must be stated that way: top-of-book exact
// for N consecutive events and X% across the session, with the divergences
// attributed. It is not "reconstructs NASDAQ exactly" — LOBSTER's message file
// only carries events inside the requested price range, so deep liquidity
// surfacing from outside the window is referenced by executions and cancels
// whose arrival was never captured. That is documented LOBSTER behaviour, not a
// replay bug, and it is why the level-50 sample is used rather than level-10.
//
// Two structural choices, both deliberate:
//
//   1. The book is SEEDED from orderbook row 0, then replay starts at message
//      row 1. The message file carries no initial state; from an empty book the
//      top of book never matches at all. Seeded levels have no per-order
//      identity, so they are held by one synthetic order each and drained at
//      level granularity.
//   2. A type-4 visible execution is applied as a targeted REDUCTION of the
//      named resting order, not as an aggressor crossing the book. LOBSTER's
//      message file already contains the outcome of the venue's matching, so
//      re-running the match here would double-count it. Phase 6 introduces the
//      aggressor form so that injected orders can win a queue race, and the
//      null-order-invariance check — replay with zero strategy orders must
//      reproduce this book byte for byte — is the gate between the two.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "adapters/lobster.hpp"
#include "match/map_book.hpp"
#include "match/report.hpp"
#include "match/types.hpp"

namespace {

constexpr std::size_t kLevels = 50;

struct NullSink {
    void operator()(const match::Report&) const {}
};

using Book = match::MapBook<NullSink>;

struct Stats {
    std::size_t rows = 0;
    std::size_t compared = 0;
    std::size_t ok1 = 0, ok5 = 0, ok10 = 0, ok_full = 0;
    std::size_t hidden = 0, halt = 0, cross = 0;
    std::size_t unknown_id = 0;
    std::size_t short_reduction = 0;
    std::size_t parse_failures = 0;
    std::size_t longest_ok1_run = 0;
    std::size_t current_ok1_run = 0;
    std::size_t first_bad1 = 0;
};

// Renders the reconstructed book in LOBSTER's own column order — per level:
// ask price, ask size, bid price, bid size — padding unoccupied levels exactly
// as LOBSTER does, so the two rows can be compared value for value.
void snapshot(const Book& book, std::vector<std::int64_t>& out) {
    static Book::LevelView asks[kLevels];
    static Book::LevelView bids[kLevels];
    const std::size_t n_ask = book.top_levels(match::Side::Sell, asks, kLevels);
    const std::size_t n_bid = book.top_levels(match::Side::Buy, bids, kLevels);

    out.clear();
    for (std::size_t i = 0; i < kLevels; ++i) {
        if (i < n_ask) {
            out.push_back(asks[i].price.ticks);
            out.push_back(static_cast<std::int64_t>(asks[i].quantity));
        } else {
            out.push_back(lobster::kEmptyAskPrice);
            out.push_back(0);
        }
        if (i < n_bid) {
            out.push_back(bids[i].price.ticks);
            out.push_back(static_cast<std::int64_t>(bids[i].quantity));
        } else {
            out.push_back(lobster::kEmptyBidPrice);
            out.push_back(0);
        }
    }
}

bool prefix_equal(const std::vector<std::int64_t>& a, const std::vector<std::int64_t>& b,
                  std::size_t levels) {
    const std::size_t n = 4 * levels;
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: lobster_oracle <message_50.csv> <orderbook_50.csv> [max_rows]\n";
        return 2;
    }
    const std::size_t max_rows = argc > 3 ? std::stoull(argv[3]) : 0;

    std::ifstream messages(argv[1]);
    std::ifstream orderbook(argv[2]);
    if (!messages || !orderbook) {
        std::cerr << "cannot open input files\n";
        return 1;
    }

    Book book{NullSink{}};
    Stats stats;

    // Synthetic ids for seeded liquidity, kept far above any real LOBSTER order
    // id so the two can never collide.
    std::uint64_t next_synthetic_id = 1ULL << 60;

    // --- seed from published row 0 ------------------------------------------
    {
        std::string row;
        if (!std::getline(orderbook, row)) {
            std::cerr << "empty orderbook file\n";
            return 1;
        }
        std::vector<std::int64_t> seed(4 * kLevels);
        if (!lobster::parse_orderbook_row(row, seed.data(), kLevels)) {
            std::cerr << "cannot parse orderbook row 0 as " << kLevels << " levels — is this the "
                      << "level-" << kLevels << " file?\n";
            return 1;
        }
        for (std::size_t i = 0; i < kLevels; ++i) {
            const std::int64_t ask_price = seed[4 * i];
            const std::int64_t ask_size = seed[4 * i + 1];
            const std::int64_t bid_price = seed[4 * i + 2];
            const std::int64_t bid_size = seed[4 * i + 3];

            if (ask_price != lobster::kEmptyAskPrice && ask_size > 0) {
                book.on_new_order(match::Order{match::OrderId{next_synthetic_id++}, match::SeqNum{0},
                                               match::Price{ask_price},
                                               match::Quantity{static_cast<std::uint32_t>(ask_size)},
                                               match::Side::Sell, match::TimeInForce::Gtc});
            }
            if (bid_price != lobster::kEmptyBidPrice && bid_size > 0) {
                book.on_new_order(match::Order{match::OrderId{next_synthetic_id++}, match::SeqNum{0},
                                               match::Price{bid_price},
                                               match::Quantity{static_cast<std::uint32_t>(bid_size)},
                                               match::Side::Buy, match::TimeInForce::Gtc});
            }
        }
        // The seeded snapshot can be crossed at the touch — it is a depth
        // photograph, not a matched book — so this is not asserted here.
        std::string discard;
        std::getline(messages, discard);  // message row 0 produced the seed state
    }

    // --- replay --------------------------------------------------------------
    std::unordered_map<std::uint64_t, char> unused;  // kept for symmetry; ids live in the book
    std::vector<std::int64_t> mine;
    std::vector<std::int64_t> published(4 * kLevels);
    std::string message_row;
    std::string book_row;

    while (std::getline(messages, message_row) && std::getline(orderbook, book_row)) {
        ++stats.rows;

        lobster::Message m;
        if (!lobster::parse_message(message_row, m)) {
            ++stats.parse_failures;
            continue;
        }

        switch (m.type) {
            case lobster::EventType::NewLimitOrder:
                // Resting directly rather than through the matching path: the
                // message file is the venue's post-match record, so an order
                // recorded as "new" did not trade on arrival.
                book.on_new_order(match::Order{match::OrderId{m.order_id}, match::SeqNum{0},
                                               match::Price{m.price}, match::Quantity{m.size},
                                               m.side, match::TimeInForce::Gtc});
                break;

            case lobster::EventType::PartialCancel:
            case lobster::EventType::VisibleExecution:
            case lobster::EventType::Delete: {
                const match::Quantity want{m.size};
                const match::Quantity got = book.reduce(match::OrderId{m.order_id}, want);
                if (got < want) {
                    // Two distinct situations, counted separately because they
                    // mean different things about the data:
                    //   - nothing removed: the order was never submitted inside
                    //     the captured price range, which is the documented
                    //     LOBSTER filtering effect;
                    //   - something removed but not enough: the order WAS
                    //     tracked and was already short, which would point at a
                    //     replay bug rather than at the data.
                    if (!got) {
                        ++stats.unknown_id;
                    } else {
                        ++stats.short_reduction;
                    }
                    book.consume_at(m.side, match::Price{m.price}, want - got);
                }
                break;
            }

            case lobster::EventType::HiddenExecution:
                ++stats.hidden;  // never displayed, so the visible book is unchanged
                break;
            case lobster::EventType::CrossTrade:
                ++stats.cross;
                break;
            case lobster::EventType::Halt:
                ++stats.halt;  // the published row duplicates the preceding state
                break;
        }

        if (!lobster::parse_orderbook_row(book_row, published.data(), kLevels)) {
            std::cerr << "cannot parse orderbook row " << stats.rows << '\n';
            return 1;
        }
        snapshot(book, mine);
        ++stats.compared;

        const bool ok1 = prefix_equal(mine, published, 1);
        stats.ok1 += ok1 ? 1 : 0;
        stats.ok5 += prefix_equal(mine, published, 5) ? 1 : 0;
        stats.ok10 += prefix_equal(mine, published, 10) ? 1 : 0;
        // The strictest comparison available: all 50 published levels. Nothing
        // in the project claims this depth — it is reported so the bound of the
        // claim that IS made is visible rather than assumed.
        stats.ok_full += prefix_equal(mine, published, kLevels) ? 1 : 0;

        if (ok1) {
            ++stats.current_ok1_run;
            if (stats.current_ok1_run > stats.longest_ok1_run) {
                stats.longest_ok1_run = stats.current_ok1_run;
            }
        } else {
            if (stats.first_bad1 == 0) stats.first_bad1 = stats.rows;
            stats.current_ok1_run = 0;
        }

        if (max_rows != 0 && stats.rows >= max_rows) break;
    }

    const auto pct = [&](std::size_t n) {
        return stats.compared == 0 ? 0.0
                                   : 100.0 * static_cast<double>(n) /
                                         static_cast<double>(stats.compared);
    };

    std::printf("rows replayed        : %zu\n", stats.rows);
    std::printf("rows compared        : %zu\n", stats.compared);
    std::printf("  top-1  exact       : %zu  (%.4f%%)\n", stats.ok1, pct(stats.ok1));
    std::printf("  top-5  exact       : %zu  (%.4f%%)\n", stats.ok5, pct(stats.ok5));
    std::printf("  top-10 exact       : %zu  (%.4f%%)\n", stats.ok10, pct(stats.ok10));
    std::printf("  top-%zu exact       : %zu  (%.4f%%)\n", kLevels, stats.ok_full,
                pct(stats.ok_full));
    std::printf("longest top-1 run    : %zu consecutive events\n", stats.longest_ok1_run);
    std::printf("first top-1 mismatch : row %zu\n", stats.first_bad1);
    std::printf("hidden executions    : %zu  (skipped — invisible to the book)\n", stats.hidden);
    std::printf("cross trades         : %zu\n", stats.cross);
    std::printf("halts                : %zu\n", stats.halt);
    std::printf("unknown order ids    : %zu  (submitted outside the captured price range)\n",
                stats.unknown_id);
    std::printf("short reductions     : %zu  (order tracked but already short)\n",
                stats.short_reduction);
    std::printf("parse failures       : %zu\n", stats.parse_failures);
    std::printf("book valid at end    : %s\n", book.validate() ? "yes" : "NO");
    return 0;
}
