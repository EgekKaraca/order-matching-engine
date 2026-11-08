// Property-based / fuzz testing.
//
// The correctness hash is a strong oracle with one structural blind spot: it
// only sees reports. A book that is momentarily wrong but self-heals before any
// trade is priced produces a byte-perfect report stream. That is precisely the
// class the harness's random-point state audit exists to catch — and the audit
// is Linux-only, so this suite is the local substitute for it.
//
// Two things are checked after EVERY message, not just at the end:
//
//   1. The engine's own internal invariants (MapBook::validate) — level totals
//      equal the sum of their orders, no empty level survives to be visible at
//      best_bid/best_ask, the index and the lists agree, the book is uncrossed.
//   2. An independent ledger, rebuilt from the report stream alone, agrees with
//      the engine's live queries: same number of resting orders, same depth at
//      every price on both sides.
//
// The ledger is what catches a LOST ORDER — one silently dropped from the book
// while the reports still look plausible. It is a test-side model on purpose;
// the ABI adapter deliberately has no such shadow, because a shadow answering
// the audit queries would mask the engine rather than expose it.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "match/map_book.hpp"
#include "match/report.hpp"
#include "match/types.hpp"

namespace match {
namespace {

// Prices are deliberately packed into a narrow band so that crossing, deep
// FIFO queues, and level exhaustion all happen constantly. A wide band would
// produce a book that never trades and a fuzz run that proves nothing.
constexpr std::int64_t kMinPrice = 95;
constexpr std::int64_t kMaxPrice = 105;

struct Tracked {
    Side side{};
    Price price{};
    Quantity remaining{};
    std::uint64_t arrival{};  // bumped on every (re)insertion at a tail
    bool live = false;
};

class Ledger {
public:
    // --- what the reports say ------------------------------------------------

    void on_rested(OrderId id, Side side, Price price, Quantity qty) {
        orders_[id] = Tracked{side, price, qty, next_arrival_++, true};
    }

    void on_dead(OrderId id) {
        const auto it = orders_.find(id);
        if (it != orders_.end()) it->second.live = false;
    }

    bool is_live(OrderId id) const {
        const auto it = orders_.find(id);
        return it != orders_.end() && it->second.live;
    }

    const Tracked* find(OrderId id) const {
        const auto it = orders_.find(id);
        return it == orders_.end() ? nullptr : &it->second;
    }

    // Returns false if the maker was not actually available to fill.
    bool consume(OrderId maker, Quantity qty) {
        const auto it = orders_.find(maker);
        if (it == orders_.end() || !it->second.live) return false;
        if (it->second.remaining < qty) return false;
        it->second.remaining -= qty;
        if (!it->second.remaining) it->second.live = false;
        return true;
    }

    // --- what the engine should therefore look like --------------------------

    std::size_t live_count() const {
        std::size_t n = 0;
        for (const auto& [id, o] : orders_) n += o.live ? 1 : 0;
        return n;
    }

    std::map<std::pair<std::int64_t, std::uint8_t>, AggregateQuantity> depths() const {
        std::map<std::pair<std::int64_t, std::uint8_t>, AggregateQuantity> out;
        for (const auto& [id, o] : orders_) {
            if (!o.live) continue;
            out[{o.price.ticks, static_cast<std::uint8_t>(o.side)}] += o.remaining.units;
        }
        return out;
    }

    std::vector<OrderId> live_ids() const {
        std::vector<OrderId> out;
        for (const auto& [id, o] : orders_) {
            if (o.live) out.push_back(id);
        }
        return out;
    }

    std::vector<OrderId> dead_ids() const {
        std::vector<OrderId> out;
        for (const auto& [id, o] : orders_) {
            if (!o.live) out.push_back(id);
        }
        return out;
    }

private:
    std::unordered_map<OrderId, Tracked> orders_;
    std::uint64_t next_arrival_ = 0;
};

// Accumulates everything the run must conserve, so the closing identity can be
// asserted once at the end.
struct Totals {
    AggregateQuantity submitted = 0;
    AggregateQuantity filled = 0;
    AggregateQuantity withdrawn = 0;
};

class Fuzzer {
public:
    explicit Fuzzer(std::uint64_t seed) : rng_(seed) {}

    void run(std::size_t messages) {
        for (std::size_t i = 0; i < messages; ++i) {
            step();
            ASSERT_TRUE(book_.validate()) << "engine invariants broken at message " << i;
            ASSERT_NO_FATAL_FAILURE(check_against_ledger()) << "at message " << i;
        }
        check_conservation();
        check_coverage();
    }

private:
    // --- message generation --------------------------------------------------

    void step() {
        reports_.clear();
        const std::int64_t roll = uniform(0, 99);
        if (roll < 45 || ledger_.live_count() == 0) {
            do_new_order();
        } else if (roll < 80) {
            do_cancel();
        } else {
            do_modify();
        }
    }

    void do_new_order() {
        // Mostly fresh ids, occasionally a dead one — reusing the id of a
        // cancelled or fully-filled order is legal and is its own conformance
        // case, so the fuzzer must exercise it.
        OrderId id{next_id_++};
        if (uniform(0, 9) == 0) {
            const auto dead = ledger_.dead_ids();
            if (!dead.empty()) id = dead[static_cast<std::size_t>(uniform(0, int(dead.size()) - 1))];
        }

        const Side side = uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
        const Price price{uniform(kMinPrice, kMaxPrice)};
        const Quantity qty{static_cast<std::uint32_t>(uniform(1, 50))};
        const TimeInForce tif = uniform(0, 99) < 15 ? TimeInForce::Ioc : TimeInForce::Gtc;

        const SeqNum seq{next_seq_++};
        totals_.submitted += qty.units;
        book_.on_new_order(Order{id, seq, price, qty, side, tif});

        // --- the reports this message must have produced ---
        ASSERT_EQ(count_of(ReportType::OrderAck), 1u) << "exactly one ack per new order";
        const Quantity filled = audit_fills(id, side, price, seq);

        ASSERT_LE(filled.units, qty.units) << "filled more than was submitted";
        const Quantity residual = qty - filled;

        if (tif == TimeInForce::Ioc) {
            ASSERT_EQ(count_of(ReportType::CancelAck), residual ? 1u : 0u)
                << "an IOC residual is cancelled, and only a residual is";
            if (residual) totals_.withdrawn += residual.units;
            ledger_.on_dead(id);
        } else if (residual) {
            ledger_.on_rested(id, side, price, residual);
        } else {
            ledger_.on_dead(id);
        }
    }

    void do_cancel() {
        const OrderId id = pick_id();
        const bool was_live = ledger_.is_live(id);
        const Quantity outstanding = was_live ? ledger_.find(id)->remaining : Quantity{};

        book_.on_cancel(SeqNum{next_seq_++}, id);

        if (was_live) {
            ASSERT_EQ(count_of(ReportType::CancelAck), 1u) << "a resting order cancels";
            ASSERT_EQ(count_of(ReportType::CancelReject), 0u);
            totals_.withdrawn += outstanding.units;
            ledger_.on_dead(id);
        } else {
            // Already filled, already cancelled, or never seen — all rejects.
            ASSERT_EQ(count_of(ReportType::CancelReject), 1u)
                << "cancelling a non-resting order must reject, not ack";
            ASSERT_EQ(count_of(ReportType::CancelAck), 0u);
        }
    }

    void do_modify() {
        const OrderId id = pick_id();
        const bool was_live = ledger_.is_live(id);
        const Side side = was_live ? ledger_.find(id)->side
                                   : (uniform(0, 1) == 0 ? Side::Buy : Side::Sell);
        const Price price{uniform(kMinPrice, kMaxPrice)};
        const Quantity qty{static_cast<std::uint32_t>(uniform(1, 50))};

        if (!was_live) {
            book_.on_modify(SeqNum{next_seq_++}, id, side, price, qty);
            ASSERT_EQ(count_of(ReportType::ModifyReject), 1u)
                << "modifying a non-resting order must reject";
            ASSERT_EQ(count_of(ReportType::ModifyAck), 0u);
            return;
        }

        // A modify is cancel + reinsert. The old quantity leaves the book and
        // the new quantity enters it, so conservation sees a withdrawal of the
        // old outstanding amount and a fresh submission of the new one.
        const Quantity outstanding = ledger_.find(id)->remaining;
        totals_.withdrawn += outstanding.units;
        totals_.submitted += qty.units;
        ledger_.on_dead(id);

        const SeqNum seq{next_seq_++};
        book_.on_modify(seq, id, side, price, qty);

        ASSERT_EQ(count_of(ReportType::ModifyAck), 1u);
        const Quantity filled = audit_fills(id, side, price, seq);
        ASSERT_LE(filled.units, qty.units);

        const Quantity residual = qty - filled;
        if (residual) ledger_.on_rested(id, side, price, residual);
    }

    // Most of the time an order that is genuinely resting, sometimes anything
    // ever issued (usually dead by now), sometimes an id never issued at all.
    // Weighted toward live because picking uniformly from every id ever seen
    // makes almost every cancel and modify a reject — the accept paths, which
    // include the crossing modify, then go barely exercised.
    OrderId pick_id() {
        const auto live = ledger_.live_ids();
        if (!live.empty() && uniform(0, 99) < 60) {
            return live[static_cast<std::size_t>(uniform(0, std::int64_t(live.size()) - 1))];
        }
        if (next_id_ > 0 && uniform(0, 3) != 0) {
            return OrderId{static_cast<std::uint64_t>(uniform(0, std::int64_t(next_id_) - 1))};
        }
        return OrderId{static_cast<std::uint64_t>(uniform(0, std::int64_t(next_id_) + 5))};
    }

    // --- per-message property checks ----------------------------------------

    // Validates every trade this message produced and returns the total filled.
    Quantity audit_fills(OrderId taker, Side side, Price limit, SeqNum seq) {
        Quantity filled{0};
        bool first = true;
        Price previous{};
        std::uint64_t previous_arrival = 0;

        for (const Report& r : reports_) {
            if (r.type != ReportType::Trade) continue;

            EXPECT_EQ(value_of(r.taker_order_id), value_of(taker));
            EXPECT_EQ(value_of(r.seq), value_of(seq)) << "a trade carries the aggressor's seq";
            EXPECT_GT(r.quantity.units, 0u) << "a zero-quantity fill is not a fill";
            EXPECT_NE(value_of(r.maker_order_id), value_of(taker)) << "an order traded with itself";

            // No fill at a price worse than the aggressor's own limit.
            EXPECT_TRUE(crosses(limit, r.price, side))
                << "filled at " << r.price.ticks << " through a limit of " << limit.ticks;

            const Tracked* maker = ledger_.find(r.maker_order_id);
            // Not ASSERT_*: this helper returns a value, so it cannot use the
            // fatal macros. Record the failure and skip the checks that would
            // dereference a null maker.
            if (maker == nullptr) {
                ADD_FAILURE() << "traded against an order that was never resting";
                continue;
            }
            EXPECT_EQ(maker->price, r.price) << "a trade must print at the maker's resting price";
            EXPECT_EQ(maker->side, opposite(side));

            if (!first) {
                // Price priority: liquidity is consumed best-price-first, so
                // each fill is at a price no better than the one before it.
                //
                // "Better" is judged from the MAKER's side, not the
                // aggressor's — a buyer sweeps asks upward, taking the lowest
                // (best-for-a-seller) first. Written with the aggressor's own
                // side this assertion is exactly inverted, which is how it
                // failed on its first run against a correct engine.
                EXPECT_FALSE(is_better(r.price, previous, opposite(side)))
                    << "swept " << previous.ticks << " before the better " << r.price.ticks;
                // Time priority: within one price level, earlier arrivals fill
                // first. Checked only across fills at the same price, since a
                // new level restarts the queue.
                if (r.price == previous) {
                    EXPECT_GT(maker->arrival, previous_arrival)
                        << "filled a later arrival before an earlier one at " << r.price.ticks;
                }
            }

            EXPECT_TRUE(ledger_.consume(r.maker_order_id, r.quantity))
                << "filled more from a maker than it had resting";

            filled += r.quantity;
            previous = r.price;
            previous_arrival = maker->arrival;
            first = false;
        }
        return filled;
    }

    // The engine's live queries must agree with a ledger built only from its
    // own reports. A disagreement means an order was lost, duplicated, or
    // silently moved.
    void check_against_ledger() {
        ASSERT_EQ(book_.resting_count(), ledger_.live_count()) << "resting order count diverged";

        const auto expected = ledger_.depths();
        for (std::int64_t p = kMinPrice; p <= kMaxPrice; ++p) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                const auto key = std::make_pair(p, static_cast<std::uint8_t>(side));
                const auto it = expected.find(key);
                const AggregateQuantity want = it == expected.end() ? 0 : it->second;
                ASSERT_EQ(book_.depth_at(Price{p}, side), want)
                    << "depth diverged at price " << p << " side " << int(side);
            }
        }

        // The touch must be a level that actually has depth — the phantom
        // price-level check, made after every single message.
        if (book_.best_bid() != Price::no_bid()) {
            ASSERT_GT(book_.depth_at(book_.best_bid(), Side::Buy), 0u) << "phantom best bid";
        }
        if (book_.best_ask() != Price::no_ask()) {
            ASSERT_GT(book_.depth_at(book_.best_ask(), Side::Sell), 0u) << "phantom best ask";
        }
    }

    // A fuzz run that never trades, never rejects, and never drops an IOC
    // residual passes every invariant vacuously. These floors are the guard
    // against a generator that quietly stops exercising the engine — if a
    // future change to the message mix breaks coverage, this fails loudly
    // rather than reporting a green suite that tests nothing.
    void check_coverage() {
        EXPECT_GT(seen_[std::size_t(ReportType::Trade)], 200u) << "hardly any trading happened";
        EXPECT_GT(seen_[std::size_t(ReportType::CancelAck)], 100u);
        EXPECT_GT(seen_[std::size_t(ReportType::ModifyAck)], 100u);
        EXPECT_GT(seen_[std::size_t(ReportType::CancelReject)], 20u) << "reject path unexercised";
        EXPECT_GT(seen_[std::size_t(ReportType::ModifyReject)], 20u) << "reject path unexercised";
    }

    void check_conservation() {
        AggregateQuantity resting = 0;
        for (const auto& [key, qty] : ledger_.depths()) resting += qty;

        // One trade of size Q retires Q from the maker and Q from the taker.
        EXPECT_EQ(2 * totals_.filled + totals_.withdrawn + resting, totals_.submitted)
            << "quantity was created or destroyed";
    }

    std::size_t count_of(ReportType type) const {
        std::size_t n = 0;
        for (const Report& r : reports_) n += (r.type == type) ? 1 : 0;
        return n;
    }

    std::int64_t uniform(std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng_);
    }

    struct Sink {
        std::vector<Report>* out;
        Totals* totals;
        std::array<std::size_t, 6>* seen;
        void operator()(const Report& r) const {
            out->push_back(r);
            ++(*seen)[static_cast<std::size_t>(r.type)];
            if (r.type == ReportType::Trade) totals->filled += r.quantity.units;
        }
    };

    std::mt19937_64 rng_;
    std::vector<Report> reports_;
    Totals totals_;
    std::array<std::size_t, 6> seen_{};
    MapBook<Sink> book_{Sink{&reports_, &totals_, &seen_}};
    Ledger ledger_;
    std::uint64_t next_id_ = 0;
    std::uint64_t next_seq_ = 0;
};

class PropertyTest : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(PropertyTest, InvariantsHoldAfterEveryMessage) {
    Fuzzer fuzzer(GetParam());
    fuzzer.run(4000);
}

// Fixed seeds rather than a random one per run: a failure has to be
// reproducible from the test name alone, and a suite that fails only on
// somebody else's machine is worse than no suite.
INSTANTIATE_TEST_SUITE_P(Seeds, PropertyTest,
                         ::testing::Values(1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610,
                                           987, 1597),
                         [](const auto& seed) { return "seed" + std::to_string(seed.param); });

}  // namespace
}  // namespace match
