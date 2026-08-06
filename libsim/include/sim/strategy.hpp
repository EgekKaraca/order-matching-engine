// strategy.hpp — what a strategy can see, and what it can do.
//
// LEAKAGE DISCIPLINE, ENFORCED STRUCTURALLY.
//
// The rule is that a strategy must not be able to read the future. Enforcing
// that by care — "remember not to look at the next event" — fails eventually,
// silently, and in the direction that flatters the result. So it is enforced by
// the type system instead:
//
//   - BookView is a VALUE. It holds copies of a delayed snapshot, not a
//     pointer or reference to the live book. A strategy physically cannot
//     reach current book state through it, because the current state is not
//     in the object.
//   - The strategy never receives the event stream, the engine, or the
//     simulator. Its whole world is the arguments to on_book / on_fill.
//   - OrderGate is the only way to act, and it does not let the strategy
//     choose when its order arrives. The simulator applies order latency;
//     an order cannot be back-dated.
//
// If a future change hands the strategy a reference to the book "just for
// convenience", that is the moment this guarantee dies. It should be defended
// in review.
#ifndef SIM_STRATEGY_HPP
#define SIM_STRATEGY_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "match/types.hpp"
#include "sim/types.hpp"

namespace sim {

// The book as the strategy is entitled to know it: a snapshot that was true at
// `as_of`, delivered at `now`. The gap between the two is the feed latency, and
// it is visible deliberately — a strategy is allowed to know how stale its own
// data is, because a real one does.
class BookView {
public:
    BookView() = default;
    BookView(Time as_of, Time now, match::Price best_bid, match::AggregateQuantity bid_size,
             match::Price best_ask, match::AggregateQuantity ask_size)
        : as_of_(as_of),
          now_(now),
          best_bid_(best_bid),
          best_ask_(best_ask),
          bid_size_(bid_size),
          ask_size_(ask_size) {}

    Time as_of() const { return as_of_; }
    Time now() const { return now_; }
    Nanos staleness() const { return now_ - as_of_; }

    bool has_bid() const { return best_bid_ != match::Price::no_bid(); }
    bool has_ask() const { return best_ask_ != match::Price::no_ask(); }
    bool two_sided() const { return has_bid() && has_ask(); }

    match::Price best_bid() const { return best_bid_; }
    match::Price best_ask() const { return best_ask_; }
    match::AggregateQuantity bid_size() const { return bid_size_; }
    match::AggregateQuantity ask_size() const { return ask_size_; }

    // Twice the mid, so the mid of an odd spread stays exact. No floating
    // point anywhere near a price, here as in the engine.
    std::optional<std::int64_t> mid_x2() const {
        if (!two_sided()) return std::nullopt;
        return best_bid_.ticks + best_ask_.ticks;
    }

    std::optional<std::int64_t> spread() const {
        if (!two_sided()) return std::nullopt;
        return best_ask_.ticks - best_bid_.ticks;
    }

private:
    Time as_of_{};
    Time now_{};
    match::Price best_bid_ = match::Price::no_bid();
    match::Price best_ask_ = match::Price::no_ask();
    match::AggregateQuantity bid_size_ = 0;
    match::AggregateQuantity ask_size_ = 0;
};

// The strategy's only means of acting. It records intents; the simulator
// schedules them with order latency. Note what is absent: no way to specify an
// arrival time, no way to cancel retroactively, no way to ask what happened
// next.
class OrderGate {
public:
    struct Submit {
        match::Side side;
        match::Price price;
        match::Quantity quantity;
        match::OrderId id;
    };

    // Returns the id the order will have, so the strategy can track and cancel
    // it — but the order does not exist yet, and will not until it arrives.
    match::OrderId submit(match::Side side, match::Price price, match::Quantity quantity) {
        const match::OrderId id{next_id_++};
        submits_.push_back(Submit{side, price, quantity, id});
        return id;
    }

    void cancel(match::OrderId id) { cancels_.push_back(id); }

    // --- simulator side ---
    const std::vector<Submit>& submits() const { return submits_; }
    const std::vector<match::OrderId>& cancels() const { return cancels_; }
    void clear() {
        submits_.clear();
        cancels_.clear();
    }
    void set_next_id(std::uint64_t id) { next_id_ = id; }

private:
    std::vector<Submit> submits_;
    std::vector<match::OrderId> cancels_;
    std::uint64_t next_id_ = 0;
};

class Strategy {
public:
    virtual ~Strategy() = default;

    // Called when a book update becomes visible at the strategy's feed clock.
    virtual void on_book(const BookView& book, OrderGate& gate) = 0;

    // Called when one of the strategy's own orders fills — also delayed by
    // feed latency, because a fill notification is a message from the venue
    // like any other. A strategy that learns of its fills instantly is a
    // strategy with a private wire.
    virtual void on_fill(const Fill& fill, const BookView& book, OrderGate& gate) {
        (void)fill;
        (void)book;
        (void)gate;
    }

    // Called when one of the strategy's orders leaves the book for good, also
    // after feed latency.
    //
    // Without this a strategy cannot know its own exposure. It requotes, the
    // cancel takes an order-latency to land, and in that window the old order
    // is still live and can still fill — but the strategy has already
    // forgotten it. Position limits then never bind, and the backtest's PnL
    // becomes dominated by inventory mark-to-market rather than by execution,
    // which is a quiet way to measure the wrong thing entirely.
    virtual void on_order_gone(match::OrderId id, bool filled, OrderGate& gate) {
        (void)id;
        (void)filled;
        (void)gate;
    }

    virtual const char* name() const = 0;
};

}  // namespace sim

#endif  // SIM_STRATEGY_HPP
