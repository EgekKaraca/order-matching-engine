// Differential testing: the optimised engine must be indistinguishable from
// the obviously-correct one.
//
// This is stronger than running the same unit tests against both. Unit tests
// check the cases somebody thought of; this drives both engines with the same
// randomly generated message sequences and asserts they agree on EVERYTHING
// observable — the canonical report stream byte for byte, and best bid, best
// ask and depth after every single message.
//
// It is also the practical safety net for optimising at all. The published
// consensus hash is the real oracle, but it takes a 2M-message replay to
// consult and it only reports that something differs. This narrows a
// divergence to one message on a sequence of a few hundred, which is the
// difference between a fix and an afternoon.
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "match/arena_book.hpp"
#include "match/canonical.hpp"
#include "match/map_book.hpp"
#include "match/report.hpp"
#include "match/types.hpp"

#include <gtest/gtest.h>

namespace match {
namespace {

struct Collector {
    std::vector<Report>* out;
    void operator()(const Report& r) const { out->push_back(r); }
};

// Deliberately narrow so crossing, deep queues and level exhaustion all happen
// constantly. A wide price band produces a book that never trades, and two
// engines agreeing about nothing happening proves nothing.
constexpr std::int64_t kMinPrice = 95;
constexpr std::int64_t kMaxPrice = 105;

class Differ {
  public:
    explicit Differ(std::uint64_t seed) : rng_(seed) {}

    void run(std::size_t messages) {
        for (std::size_t i = 0; i < messages; ++i) {
            reference_reports_.clear();
            candidate_reports_.clear();

            step();

            ASSERT_NO_FATAL_FAILURE(compare_reports(i));
            ASSERT_NO_FATAL_FAILURE(compare_state(i));
            ASSERT_TRUE(reference_.validate()) << "reference invalid at message " << i;
            ASSERT_TRUE(candidate_.validate()) << "candidate invalid at message " << i;
        }
        // A run that never traded would pass everything above vacuously.
        EXPECT_GT(trades_, 100u) << "the sequence barely traded; this proves little";
        EXPECT_GT(rejects_, 10u) << "the reject path went unexercised";
    }

  private:
    void step() {
        const std::int64_t roll = uniform(0, 99);
        const SeqNum seq{next_seq_++};

        if (roll < 45 || live_.empty()) {
            const OrderId id{next_id_++};
            const Side side = uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
            const Price price{uniform(kMinPrice, kMaxPrice)};
            const Quantity qty{static_cast<std::uint32_t>(uniform(1, 50))};
            const TimeInForce tif = uniform(0, 99) < 15 ? TimeInForce::Ioc : TimeInForce::Gtc;
            const Order order{id, seq, price, qty, side, tif};

            reference_.on_new_order(order);
            candidate_.on_new_order(order);
            sides_[id] = side;
            if (tif == TimeInForce::Gtc) live_.push_back(id);
        } else if (roll < 75) {
            const OrderId id = pick();
            reference_.on_cancel(seq, id);
            candidate_.on_cancel(seq, id);
        } else {
            const OrderId id = pick();
            // The side must match the resting order or the contract is
            // violated, so a modify of an unknown id uses either side — both
            // engines reject it before looking.
            const Side side = side_of(id);
            const Price price{uniform(kMinPrice, kMaxPrice)};
            const Quantity qty{static_cast<std::uint32_t>(uniform(1, 50))};
            reference_.on_modify(seq, id, side, price, qty);
            candidate_.on_modify(seq, id, side, price, qty);
            // A successful modify keeps the same side, so sides_ stays valid.
        }
    }

    // A modify must name the side the order actually rests on — that is part
    // of the ABI contract, and both engines assert it in debug builds. For an
    // id that was never issued the side is irrelevant: both reject before
    // looking at it.
    Side side_of(OrderId id) const {
        const auto it = sides_.find(id);
        return it == sides_.end() ? Side::Buy : it->second;
    }

    OrderId pick() {
        if (!live_.empty() && uniform(0, 99) < 70) {
            return live_[static_cast<std::size_t>(uniform(0, std::int64_t(live_.size()) - 1))];
        }
        return OrderId{static_cast<std::uint64_t>(uniform(0, std::int64_t(next_id_) + 5))};
    }

    void compare_reports(std::size_t message) {
        ASSERT_EQ(reference_reports_.size(), candidate_reports_.size())
            << "different number of reports at message " << message;
        for (std::size_t i = 0; i < reference_reports_.size(); ++i) {
            const std::string expected = canonical::line(reference_reports_[i]);
            const std::string actual = canonical::line(candidate_reports_[i]);
            ASSERT_EQ(expected, actual) << "report " << i << " differs at message " << message;
            if (reference_reports_[i].type == ReportType::Trade) ++trades_;
            if (reference_reports_[i].type == ReportType::CancelReject ||
                reference_reports_[i].type == ReportType::ModifyReject) {
                ++rejects_;
            }
        }
    }

    void compare_state(std::size_t message) {
        ASSERT_EQ(reference_.best_bid(), candidate_.best_bid()) << "best bid at " << message;
        ASSERT_EQ(reference_.best_ask(), candidate_.best_ask()) << "best ask at " << message;
        ASSERT_EQ(reference_.resting_count(), candidate_.resting_count())
            << "resting count at " << message;
        for (std::int64_t p = kMinPrice; p <= kMaxPrice; ++p) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                ASSERT_EQ(reference_.depth_at(Price{p}, side), candidate_.depth_at(Price{p}, side))
                    << "depth at " << p << " side " << int(side) << " at message " << message;
            }
        }
    }

    std::int64_t uniform(std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng_);
    }

    std::mt19937_64 rng_;
    std::vector<Report> reference_reports_;
    std::vector<Report> candidate_reports_;
    MapBook<Collector> reference_{Collector{&reference_reports_}};
    ArenaBook<Collector> candidate_{Collector{&candidate_reports_}};
    std::vector<OrderId> live_;
    std::unordered_map<OrderId, Side> sides_;
    std::uint64_t next_id_ = 0;
    std::uint64_t next_seq_ = 0;
    std::size_t trades_ = 0;
    std::size_t rejects_ = 0;
};

class EngineEquivalence : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(EngineEquivalence, ArenaBookMatchesMapBookExactly) {
    Differ differ(GetParam());
    differ.run(3000);
}

INSTANTIATE_TEST_SUITE_P(Seeds, EngineEquivalence,
                         ::testing::Values(1, 2, 3, 5, 8, 13, 21, 34, 55, 89),
                         [](const auto& seed) { return "seed" + std::to_string(seed.param); });

}  // namespace
}  // namespace match
