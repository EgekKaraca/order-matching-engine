// order_tracker.hpp — every order the strategy posted, filled or not.
//
// The simulator's fills() vector is a record of successes. It cannot answer
// "what fraction of posted orders filled?", because the failures are invisible
// in it — and that fraction is the central quantity of the published
// fill-probability work this project is being compared against.
//
// So this records the whole population at post time: where the order sat in
// its queue, how thick the opposite side was, and how much of it eventually
// traded. Together those are the axes of the fill-probability surface.
#ifndef SIM_ORDER_TRACKER_HPP
#define SIM_ORDER_TRACKER_HPP

#include "sim/types.hpp"

#include <cstdint>

#include "match/types.hpp"

namespace sim {

struct PostedOrder {
    match::OrderId id{};
    Time posted_at{};
    Time retired_at{};  // when it left the book, filled or cancelled
    match::Side side{};
    match::Price price{};
    match::Quantity quantity{};         // as posted
    match::Quantity filled_quantity{};  // accumulated over its lifetime

    // Conditions at post time — the surface's coordinates.
    match::AggregateQuantity queue_ahead = 0;
    match::AggregateQuantity queue_behind = 0;
    match::AggregateQuantity opposite_depth = 0;

    bool was_filled() const { return filled_quantity.units > 0; }

    bool fully_filled() const { return filled_quantity.units >= quantity.units; }

    // Same convention as Fill::normalised_queue_position: the fraction of the
    // level's OTHER liquidity sitting ahead. 0 = front, 1 = back.
    double normalised_queue_position() const {
        const auto others = queue_ahead + queue_behind;
        if (others == 0) return 0.0;
        return static_cast<double>(queue_ahead) / static_cast<double>(others);
    }
};

}  // namespace sim

#endif  // SIM_ORDER_TRACKER_HPP
