// map_book.hpp — the obviously-correct engine.
//
// std::map of price levels, std::list of orders within a level, hash map from
// order id to list node. No arena, no intrusive containers, no cleverness of
// any kind. This is Phase 1: the engine whose only job is to be right, and to
// stay in the repo afterwards as the reference the optimised engine is diffed
// against under the same oracle.
//
// It is also not as slow as it looks. On the benchmark's own measurements a
// good std::map engine reaches 7.94 M msg/s against a field that tops out at
// 8.19 — so there is no "10x over the STL baseline" story to chase here, and
// correctness is the only thing worth spending Phase 1 on.
//
// The engine is templated on its report sink rather than taking a
// std::function: reports are emitted on the hot path, and a type-erased
// indirect call per report is exactly the cost the project claims not to pay.
#ifndef MATCH_MAP_BOOK_HPP
#define MATCH_MAP_BOOK_HPP

#include <algorithm>
#include <cassert>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <utility>

#include "match/report.hpp"
#include "match/types.hpp"

namespace match {

// A resting order carries only what its own node needs: its price and side are
// properties of the level it sits in, and duplicating them here is how they
// drift apart.
struct RestingOrder {
    OrderId id{};
    Quantity remaining{};
};

// One price level. `total` is maintained incrementally so depth_at is O(1);
// the invariant that it equals the sum of the list is checked by validate().
struct Level {
    std::list<RestingOrder> orders;
    AggregateQuantity total{0};
};

// Both sides keep the best price at begin(), so "walk the book best-first" is
// just iteration and there is no separate best-price cache to go stale.
using BidLevels = std::map<Price, Level, std::greater<Price>>;
using AskLevels = std::map<Price, Level, std::less<Price>>;

template <typename Sink>
class MapBook {
public:
    explicit MapBook(Sink sink) : sink_(std::move(sink)) {}

    void on_new_order(const Order& order) {
        // The ack comes first because the engine has accepted the order; the
        // canonical form sorts by (seq, type) anyway, so this is for readers
        // of a live stream, not for the hash.
        emit(Report::order_ack(order.seq, order.id, order.side, order.price, order.quantity));

        const Quantity residual = sweep(order.side, order.price, order.quantity, order.id,
                                        order.seq);
        if (!residual) return;

        if (order.tif == TimeInForce::Ioc) {
            // Dropped, never rested — and it is a CancelAck, not a reject.
            emit(Report::cancel_ack(order.seq, order.id, order.side, order.price, residual));
            return;
        }
        rest(order.side, order.price, order.id, residual);
    }

    void on_cancel(SeqNum seq, OrderId id) {
        const auto found = index_.find(id);
        if (found == index_.end()) {
            // Already filled, already cancelled, or never seen. All three are
            // the same answer: there is nothing resting to cancel.
            emit(Report::cancel_reject(seq, id));
            return;
        }
        const Locator loc = found->second;
        const Quantity withdrawn = loc.node->remaining;
        unlink(loc);
        index_.erase(found);
        emit(Report::cancel_ack(seq, id, loc.side, loc.price, withdrawn));
    }

    // Modify is cancel + reinsert, and the reinsert is a live aggressor: a buy
    // repriced up through the spread must trade, not rest crossed.
    void on_modify(SeqNum seq, OrderId id, Side side, Price new_price, Quantity new_qty) {
        const auto found = index_.find(id);
        if (found == index_.end()) {
            emit(Report::modify_reject(seq, id));
            return;
        }
        const Locator loc = found->second;
        assert(loc.side == side && "modify disagrees with the resting order's side");
        unlink(loc);
        index_.erase(found);

        const Quantity residual = sweep(side, new_price, new_qty, id, seq);
        if (residual) rest(side, new_price, id, residual);

        // The ack echoes what was REQUESTED — the new price and the new
        // quantity — even when the reinsert immediately filled part of it.
        // (Confirmed against the published canonical output, not inferred.)
        emit(Report::modify_ack(seq, id, side, new_price, new_qty));
    }

    // Reduces a resting order in place, KEEPING its queue position, and
    // returns the quantity actually removed (0 if the order is not resting,
    // and clamped to what was left). Removes the order entirely if the
    // reduction consumes it.
    //
    // This is the production exchange semantic for a size decrease, and it is
    // what LOBSTER's type-2 partial cancels and type-4 visible executions do to
    // a resting order — an order that loses size does not go to the back of its
    // own queue. It is deliberately unreachable from the benchmark C ABI, whose
    // modify is always cancel + reinsert; docs/CONFORMANCE.md leaves the
    // decrease convention unspecified because engines legitimately differ, and
    // every modify in the canonical workload is a reprice or an increase. So
    // this cannot move the conformance hash, and check_hashes.sh confirms it.
    //
    // Emits no report on purpose: only the caller knows whether the reduction
    // was a cancellation or an execution, and labelling an execution as a
    // CancelAck would put a lie into the event stream.
    Quantity reduce(OrderId id, Quantity delta) {
        const auto found = index_.find(id);
        if (found == index_.end()) return Quantity{0};

        const Locator loc = found->second;
        const Quantity removed{std::min(delta.units, loc.node->remaining.units)};

        if (removed == loc.node->remaining) {
            unlink(loc);
            index_.erase(found);
            return removed;
        }
        if (loc.side == Side::Buy) {
            reduce_in(bids_, loc, removed);
        } else {
            reduce_in(asks_, loc, removed);
        }
        return removed;
    }

    // Consumes `qty` from the front of one price level, FIFO, without naming
    // an order. Returns the quantity actually removed.
    //
    // Replay needs this because a historical feed can reference liquidity whose
    // arrival was never captured — LOBSTER's message file only carries events
    // inside the requested price range, and a book seeded from a depth snapshot
    // has levels with no per-order identity at all. Taking from the front is
    // the right fallback precisely because those orders are the oldest ones
    // present, so FIFO puts them exactly where they belong.
    //
    // Like reduce(), this emits no report: the caller knows whether the
    // liquidity left as a cancel or as a fill.
    Quantity consume_at(Side side, Price price, Quantity qty) {
        return side == Side::Buy ? consume_in(bids_, price, qty) : consume_in(asks_, price, qty);
    }

    // --- audit queries -----------------------------------------------------

    struct LevelView {
        Price price{};
        AggregateQuantity quantity{};
    };

    // Fills `out` with up to `max` price levels, best first, and returns how
    // many were written. Used to diff the reconstructed book against a venue's
    // published depth snapshot.
    std::size_t top_levels(Side side, LevelView* out, std::size_t max) const {
        return side == Side::Buy ? copy_levels(bids_, out, max) : copy_levels(asks_, out, max);
    }
    // These read the live book, never a shadow. An empty level is erased the
    // moment it empties, so a dead best price can never be observed here.

    Price best_bid() const noexcept {
        return bids_.empty() ? Price::no_bid() : bids_.begin()->first;
    }

    Price best_ask() const noexcept {
        return asks_.empty() ? Price::no_ask() : asks_.begin()->first;
    }

    AggregateQuantity depth_at(Price price, Side side) const noexcept {
        if (side == Side::Buy) {
            const auto it = bids_.find(price);
            return it == bids_.end() ? 0 : it->second.total;
        }
        const auto it = asks_.find(price);
        return it == asks_.end() ? 0 : it->second.total;
    }

    bool empty() const noexcept { return bids_.empty() && asks_.empty(); }

    std::size_t resting_count() const noexcept { return index_.size(); }

    // Every invariant that must hold after any operation. Used by the unit
    // tests and by the property/fuzz suite, which calls it after every single
    // message — that is what catches internal corruption that self-heals
    // before it can reach the report stream.
    bool validate() const {
        return validate_side(bids_, Side::Buy) && validate_side(asks_, Side::Sell) &&
               !is_crossed() && index_consistent();
    }

private:
    struct Locator {
        Side side{};
        Price price{};
        std::list<RestingOrder>::iterator node{};
    };

    void emit(const Report& report) { sink_(report); }

    // Walks the opposite book best-price-first, FIFO within a level, and
    // returns the aggressor's unfilled remainder.
    Quantity sweep(Side aggressor_side, Price limit, Quantity qty, OrderId taker, SeqNum seq) {
        return aggressor_side == Side::Buy
                   ? sweep_side(asks_, aggressor_side, limit, qty, taker, seq)
                   : sweep_side(bids_, aggressor_side, limit, qty, taker, seq);
    }

    template <typename Levels>
    Quantity sweep_side(Levels& levels, Side aggressor_side, Price limit, Quantity qty,
                        OrderId taker, SeqNum seq) {
        while (qty && !levels.empty()) {
            const auto level_it = levels.begin();
            const Price maker_price = level_it->first;

            // Stop at the aggressor's own limit: a marketable limit rests its
            // remainder here rather than walking through its own price.
            if (!crosses(limit, maker_price, aggressor_side)) break;

            Level& level = level_it->second;
            while (qty && !level.orders.empty()) {
                RestingOrder& maker = level.orders.front();
                const Quantity fill{std::min(qty.units, maker.remaining.units)};

                // The fill prints at the MAKER's resting price, never the
                // aggressor's limit, in both directions.
                emit(Report::trade(seq, maker_price, fill, maker.id, taker));

                qty -= fill;
                maker.remaining -= fill;
                level.total -= fill.units;

                if (maker.remaining) break;  // partially filled: keeps its place at the front
                index_.erase(maker.id);
                level.orders.pop_front();
            }
            if (level.orders.empty()) levels.erase(level_it);
        }
        return qty;
    }

    void rest(Side side, Price price, OrderId id, Quantity qty) {
        assert(index_.find(id) == index_.end() && "resting an id that is already in the book");
        if (side == Side::Buy) {
            rest_in(bids_, side, price, id, qty);
        } else {
            rest_in(asks_, side, price, id, qty);
        }
    }

    template <typename Levels>
    void rest_in(Levels& levels, Side side, Price price, OrderId id, Quantity qty) {
        Level& level = levels[price];
        level.orders.push_back(RestingOrder{id, qty});  // the tail: last in, last filled
        level.total += qty.units;
        index_.emplace(id, Locator{side, price, std::prev(level.orders.end())});
    }

    void unlink(const Locator& loc) {
        if (loc.side == Side::Buy) {
            unlink_from(bids_, loc);
        } else {
            unlink_from(asks_, loc);
        }
    }

    // Erases one node without touching its neighbours — the orders behind it
    // in the queue keep their places, and a same-price sibling survives.
    template <typename Levels>
    void unlink_from(Levels& levels, const Locator& loc) {
        const auto level_it = levels.find(loc.price);
        assert(level_it != levels.end() && "locator points at a level that is gone");
        Level& level = level_it->second;
        level.total -= loc.node->remaining.units;
        level.orders.erase(loc.node);
        if (level.orders.empty()) levels.erase(level_it);
    }

    template <typename Levels>
    Quantity consume_in(Levels& levels, Price price, Quantity qty) {
        const auto level_it = levels.find(price);
        if (level_it == levels.end()) return Quantity{0};

        Level& level = level_it->second;
        Quantity removed{0};
        while (qty && !level.orders.empty()) {
            RestingOrder& front = level.orders.front();
            const Quantity take{std::min(qty.units, front.remaining.units)};

            qty -= take;
            removed += take;
            front.remaining -= take;
            level.total -= take.units;

            if (front.remaining) break;
            index_.erase(front.id);
            level.orders.pop_front();
        }
        if (level.orders.empty()) levels.erase(level_it);
        return removed;
    }

    template <typename Levels>
    static std::size_t copy_levels(const Levels& levels, LevelView* out, std::size_t max) {
        std::size_t n = 0;
        for (const auto& [price, level] : levels) {
            if (n == max) break;
            out[n++] = LevelView{price, level.total};
        }
        return n;
    }

    // Decrements one node and its level total together — the two must never
    // move independently, which is what validate() checks.
    template <typename Levels>
    void reduce_in(Levels& levels, const Locator& loc, Quantity removed) {
        const auto level_it = levels.find(loc.price);
        assert(level_it != levels.end() && "locator points at a level that is gone");
        level_it->second.total -= removed.units;
        loc.node->remaining -= removed;
    }

    template <typename Levels>
    bool validate_side(const Levels& levels, Side side) const {
        for (const auto& [price, level] : levels) {
            if (level.orders.empty()) return false;  // a phantom level is visible to best_*
            AggregateQuantity sum = 0;
            for (const auto& order : level.orders) {
                if (!order.remaining) return false;  // a zero-quantity node should be gone
                sum += order.remaining.units;
                const auto found = index_.find(order.id);
                if (found == index_.end()) return false;
                if (found->second.side != side || found->second.price != price) return false;
            }
            if (sum != level.total) return false;
        }
        return true;
    }

    bool is_crossed() const noexcept {
        if (bids_.empty() || asks_.empty()) return false;
        return best_bid().ticks >= best_ask().ticks;
    }

    bool index_consistent() const {
        std::size_t nodes = 0;
        for (const auto& [price, level] : bids_) nodes += level.orders.size();
        for (const auto& [price, level] : asks_) nodes += level.orders.size();
        return nodes == index_.size();
    }

    Sink sink_;
    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace match

#endif  // MATCH_MAP_BOOK_HPP
