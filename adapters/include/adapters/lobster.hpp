// lobster.hpp — LOBSTER message-file parsing.
//
// LOBSTER ships a *message* file and a matching *orderbook* file for the same
// window. The message file is the L3 event stream; the orderbook file is the
// venue's own published book state after each of those events. That pairing is
// what makes it an external oracle for the replay path: reconstruct from the
// messages, compare against the published book.
//
// Message file format — six comma-separated columns, one event per line:
//
//   time        seconds after midnight, nanosecond precision
//   type        1 new limit order    2 partial cancel   3 full delete
//               4 visible execution  5 hidden execution 6 cross/auction trade
//               7 trading halt
//   order id    stable within the file; 0 for events with no order identity
//   size        shares
//   price       DOLLARS x 10000  (so $58.54 is 585400)
//   direction   1 buy, -1 sell
//
// Two things about this format that matter downstream:
//
//   - The price scale is $0.0001 per unit, NOT the benchmark's $0.005 tick.
//     Price in this project is a dimensionless tick index precisely so that
//     both can be carried without a conversion in the engine; the scale is
//     recorded here, at the boundary, and nowhere else.
//   - A type-5 hidden execution does NOT touch the visible book, so it must be
//     parsed and then deliberately ignored by the reconstruction. Treating it
//     as a normal execution double-counts liquidity that was never displayed.
#ifndef ADAPTERS_LOBSTER_HPP
#define ADAPTERS_LOBSTER_HPP

#include <cstdint>
#include <string_view>

#include "match/types.hpp"

namespace lobster {

// One LOBSTER unit is $0.0001. Recorded for the adapter boundary and for
// anything that has to render a price back into dollars; the engine never
// needs it.
constexpr std::int64_t kUnitsPerDollar = 10000;

// The padding LOBSTER writes for an unoccupied level in the orderbook file.
constexpr std::int64_t kEmptyAskPrice = 9999999999;
constexpr std::int64_t kEmptyBidPrice = -9999999999;

enum class EventType : std::uint8_t {
    NewLimitOrder = 1,
    PartialCancel = 2,     // a reduction in size; the order keeps its place
    Delete = 3,            // the whole remaining quantity leaves the book
    VisibleExecution = 4,  // a displayed order was hit or lifted
    HiddenExecution = 5,   // invisible to the book — see the note above
    CrossTrade = 6,        // auction / cross
    Halt = 7,              // trading halt indicator, not a book event
};

struct Message {
    std::int64_t time_ns = 0;  // nanoseconds after midnight
    EventType type{};
    std::uint64_t order_id = 0;
    std::uint32_t size = 0;
    std::int64_t price = 0;  // dollars x 10000
    match::Side side{};

    // True for the events that change displayed liquidity. Hidden executions,
    // halts and cross trades are parsed but must not be applied to the book.
    bool touches_visible_book() const {
        return type == EventType::NewLimitOrder || type == EventType::PartialCancel ||
               type == EventType::Delete || type == EventType::VisibleExecution;
    }
};

// Parses one line. Returns false on a malformed line rather than throwing:
// these files run to hundreds of thousands of rows and a parse failure should
// name its row, not unwind.
bool parse_message(std::string_view line, Message& out);

// Parses one orderbook-file row into `levels` groups of
// (ask price, ask size, bid price, bid size). `out` must have room for
// 4 * levels values. Returns false if the row has the wrong arity.
bool parse_orderbook_row(std::string_view line, std::int64_t* out, std::size_t levels);

}  // namespace lobster

#endif  // ADAPTERS_LOBSTER_HPP
