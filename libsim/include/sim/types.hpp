// types.hpp — the simulator's vocabulary.
//
// libsim owns everything the engine must never know about: time, latency,
// position, fees, and the strategy. libmatch does not include this header.
#ifndef SIM_TYPES_HPP
#define SIM_TYPES_HPP

#include <compare>
#include <cstdint>

#include "match/types.hpp"

namespace sim {

// A duration, in nanoseconds. Distinct from Time so that adding two instants
// or comparing a latency to a timestamp does not compile.
struct Nanos {
    std::int64_t count{};
    constexpr auto operator<=>(const Nanos&) const = default;
};

constexpr Nanos micros(std::int64_t n) { return Nanos{n * 1'000}; }
constexpr Nanos millis(std::int64_t n) { return Nanos{n * 1'000'000}; }
constexpr Nanos seconds(std::int64_t n) { return Nanos{n * 1'000'000'000}; }

// An instant, in nanoseconds after midnight — the same base LOBSTER uses.
struct Time {
    std::int64_t ns{};
    constexpr auto operator<=>(const Time&) const = default;

    constexpr Time operator+(Nanos d) const { return Time{ns + d.count}; }
    constexpr Time operator-(Nanos d) const { return Time{ns - d.count}; }
    constexpr Nanos operator-(Time other) const { return Nanos{ns - other.ns}; }
};

// One historical market event, normalised away from any particular venue's
// encoding. Adapters produce these; the simulator consumes them.
enum class MarketEventType : std::uint8_t {
    Add,      // a new order joins the book
    Reduce,   // a resting order shrinks but keeps its place
    Delete,   // a resting order leaves entirely
    Execute,  // a resting order was hit or lifted by an incoming aggressor
};

struct MarketEvent {
    Time time{};
    MarketEventType type{};
    match::OrderId id{};
    match::Price price{};
    match::Quantity quantity{};
    // For Execute this is the MAKER's side — the side of the order that was
    // executed. The aggressor was on the other side. Getting this backwards
    // silently inverts every fill in the study.
    match::Side side{};
};

// A fill of one of the strategy's own orders.
struct Fill {
    Time time{};
    match::OrderId id{};
    match::Side side{};
    match::Price price{};
    match::Quantity quantity{};
    bool maker = true;  // passive fill (earns the spread) vs. aggressive
    // Size that was resting ahead of this order when it was placed. A naive
    // fill model implicitly assumes this is always zero; recording it is what
    // makes the contrast measurable rather than asserted.
    match::AggregateQuantity queue_ahead_at_entry = 0;
    // Size resting behind it. Needed only to normalise the position — but
    // without it this project's queue numbers cannot be compared to any
    // published result, because the literature reports a fraction.
    match::AggregateQuantity queue_behind_at_entry = 0;

    // True when this fill had a queue position at all. A taker never queues,
    // and reporting 0.0 for one would read as "front of the queue" — the
    // opposite of the truth, which is "did not queue".
    bool has_queue_position() const { return maker; }

    // Queue position as the literature defines it: the fraction of the level's
    // OTHER liquidity sitting ahead. 0 = front, 1 = back. Excludes the order's
    // own size, so a large order does not flatter its own position.
    // Source: arXiv:2502.18625.
    double normalised_queue_position() const {
        const auto others = queue_ahead_at_entry + queue_behind_at_entry;
        if (others == 0) return 0.0;  // alone at the level, so at its front
        return static_cast<double>(queue_ahead_at_entry) / static_cast<double>(others);
    }
};

}  // namespace sim

#endif  // SIM_TYPES_HPP
