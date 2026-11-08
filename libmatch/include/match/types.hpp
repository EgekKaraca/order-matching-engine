// types.hpp — the vocabulary of the matching engine.
//
// Everything here is a value type with no behaviour beyond what price-time
// priority needs. The engine itself lives elsewhere; this header is what both
// halves of the project (libmatch and libsim) agree to speak.
#ifndef MATCH_TYPES_HPP
#define MATCH_TYPES_HPP

#include <compare>
#include <cstdint>
#include <limits>

namespace match {

// A price is an integer tick INDEX, and deliberately dimensionless: this type
// never knows what a tick is worth. That is a boundary concern, because the
// two data sources this project consumes disagree —
//
//   the benchmark harness    1 tick = $0.005   (SEC sub-penny rule)
//   LOBSTER message files    1 unit = $0.0001
//
// so baking either scale in here would force a conversion at the wrong place.
// Adapters convert; the engine compares. Ticks are signed because the harness
// ABI passes int64_t and its two empty-book sentinels are the extremes of that
// range — a negative price is not meaningful, but it is representable, and the
// engine must round-trip whatever the ABI hands it.
struct Price {
    using rep = std::int64_t;

    rep ticks{};

    constexpr auto operator<=>(const Price&) const = default;

    // Sentinels for an empty side, matching the harness ABI exactly
    // (engine_query_best_bid / best_ask).
    static constexpr Price no_bid() noexcept { return Price{std::numeric_limits<rep>::min()}; }
    static constexpr Price no_ask() noexcept { return Price{std::numeric_limits<rep>::max()}; }
};

// Quantity is unsigned 32-bit to match the ABI's new_order_t::quantity. Summed
// depth across a level can exceed that, so aggregates use AggregateQuantity.
struct Quantity {
    using rep = std::uint32_t;

    rep units{};

    constexpr auto operator<=>(const Quantity&) const = default;

    constexpr Quantity& operator+=(Quantity rhs) noexcept {
        units += rhs.units;
        return *this;
    }
    // Precondition: rhs <= *this. Underflow here is the classic way a book
    // silently gains liquidity, so callers check first and tests pin it.
    constexpr Quantity& operator-=(Quantity rhs) noexcept {
        units -= rhs.units;
        return *this;
    }

    friend constexpr Quantity operator+(Quantity a, Quantity b) noexcept { return a += b; }
    friend constexpr Quantity operator-(Quantity a, Quantity b) noexcept { return a -= b; }

    constexpr explicit operator bool() const noexcept { return units != 0; }
};

using AggregateQuantity = std::uint64_t;  // engine_query_depth_at returns this

// Strong ids. Scoped enums give distinct types at zero cost, and std::hash
// already covers enumerations, so these drop straight into a hash map.
enum class OrderId : std::uint64_t {};
enum class SeqNum : std::uint64_t {};

constexpr std::uint64_t value_of(OrderId id) noexcept { return static_cast<std::uint64_t>(id); }
constexpr std::uint64_t value_of(SeqNum seq) noexcept { return static_cast<std::uint64_t>(seq); }

// Encoded to match the ABI's `uint8_t side` (0 = buy, 1 = sell) so the adapter
// is a cast, not a lookup table.
enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class TimeInForce : std::uint8_t {
    Gtc = 0,  // rests until filled or cancelled
    Ioc = 1,  // matches what it can; the remainder is dropped, never rested
};

// Price priority: which of two prices a resting order would rather have.
// A buy wants to be high (it gets filled first), a sell wants to be low.
constexpr bool is_better(Price a, Price b, Side side) noexcept {
    return side == Side::Buy ? a.ticks > b.ticks : a.ticks < b.ticks;
}

// Can an order of `side` at `limit` trade against a resting order at `resting`?
// Equal prices cross — an order at the touch fills against its own price.
constexpr bool crosses(Price limit, Price resting, Side side) noexcept {
    return side == Side::Buy ? limit.ticks >= resting.ticks : limit.ticks <= resting.ticks;
}

// An order as the engine receives it. What the engine stores internally is its
// own business; this is the message shape.
struct Order {
    OrderId id{};
    SeqNum seq{};
    Price price{};
    Quantity quantity{};
    Side side{};
    TimeInForce tif{};

    constexpr bool operator==(const Order&) const = default;
};

}  // namespace match

#endif  // MATCH_TYPES_HPP
