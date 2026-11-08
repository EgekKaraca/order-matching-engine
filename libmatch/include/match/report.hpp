// report.hpp — the engine's output event stream.
//
// The engine's product is an ordered stream of reports, not just a mutated
// book. Identical input must give a byte-identical stream every time; that one
// property is what the benchmark's correctness oracle hashes AND what makes a
// backtest on top of this engine reproducible.
//
// These are exactly the six report types of the benchmark C ABI
// (api/matching_engine_api.h), in the same order, with the same field meanings.
// That is deliberate: the C ABI is the seam this project claims has no
// translation layer, so the engine emits the seam's vocabulary natively rather
// than emitting its own and converting in the adapter.
//
// Two consequences worth stating, because a reader will expect otherwise:
//   - There is no OrderReject. The ABI has no way to refuse a new order, and
//     the canonical workload never sends one that should be refused.
//   - There is no BookUpdate. Book state is queried (best bid / best ask /
//     depth at), not published. A market-data view belongs to libsim.
#ifndef MATCH_REPORT_HPP
#define MATCH_REPORT_HPP

#include <cstdint>

#include "match/types.hpp"

namespace match {

// Values match me_report_type_t. The canonical text sorts on this, so the
// numbering is part of the contract, not an implementation detail.
enum class ReportType : std::uint8_t {
    OrderAck = 0,
    Trade = 1,
    CancelAck = 2,
    ModifyAck = 3,
    CancelReject = 4,
    ModifyReject = 5,
};

// One flat report covering all six types; unused fields stay zero. Flat rather
// than a variant because this is what crosses the ABI, and because the
// canonical serialiser wants to sort a contiguous array of them.
struct Report {
    ReportType type{};
    Side side{};         // the originating order's side, where meaningful
    SeqNum seq{};        // the aggressive / originating order's sequence number
    OrderId order_id{};  // the order this report concerns
    Price price{};       // Trade: the MAKER's resting price
    Quantity quantity{};
    OrderId maker_order_id{};  // Trade only
    OrderId taker_order_id{};  // Trade only

    constexpr bool operator==(const Report&) const = default;

    // Named constructors — each takes exactly the fields its report type
    // carries, so a caller cannot forget one or set one that does not apply.

    static constexpr Report order_ack(SeqNum seq, OrderId id, Side side, Price price,
                                      Quantity qty) noexcept {
        return {ReportType::OrderAck, side, seq, id, price, qty, {}, {}};
    }

    // `price` is the maker's resting price and `seq` the aggressor's — the two
    // fields engines most often get wrong (a taker-priced trade is a real,
    // repeatedly-observed defect class).
    static constexpr Report trade(SeqNum aggressor_seq, Price maker_price, Quantity qty,
                                  OrderId maker, OrderId taker) noexcept {
        return {ReportType::Trade, {}, aggressor_seq, {}, maker_price, qty, maker, taker};
    }

    // Emitted both for a successful cancel and for the dropped residual of a
    // partially-filled IOC order. `qty` is the quantity withdrawn: the ABI
    // struct carries it and the reference adapters populate it, but the
    // canonical line omits it, so it is invisible to the correctness hash.
    static constexpr Report cancel_ack(SeqNum seq, OrderId id, Side side, Price price,
                                       Quantity qty = {}) noexcept {
        return {ReportType::CancelAck, side, seq, id, price, qty, {}, {}};
    }

    static constexpr Report modify_ack(SeqNum seq, OrderId id, Side side, Price new_price,
                                       Quantity new_qty) noexcept {
        return {ReportType::ModifyAck, side, seq, id, new_price, new_qty, {}, {}};
    }

    // The order is not resting: already filled, already cancelled, or never
    // seen. A reject carries identity only — the order it names is gone.
    static constexpr Report cancel_reject(SeqNum seq, OrderId id) noexcept {
        return {ReportType::CancelReject, {}, seq, id, {}, {}, {}, {}};
    }

    static constexpr Report modify_reject(SeqNum seq, OrderId id) noexcept {
        return {ReportType::ModifyReject, {}, seq, id, {}, {}, {}, {}};
    }
};

}  // namespace match

#endif  // MATCH_REPORT_HPP
