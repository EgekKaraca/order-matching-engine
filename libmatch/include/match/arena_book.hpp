// arena_book.hpp — the optimised engine.
//
// Same contract as MapBook, same report stream, byte for byte. The two are
// swappable at the C ABI and both run under the same oracle, which is the only
// reason optimising is safe to attempt at all: every structural change is
// checked against the published consensus hash rather than against a hope.
//
// WHAT CHANGES, AND WHY THESE CHOICES
//
//   price levels   sorted vector per side, WORST price first so the best is at
//                  back(). Not a flat bitmap-indexed ladder: that is the
//                  intuitive choice and the benchmark data refutes it — across
//                  14 engines the flat array tops out at 5.60 M/s, BELOW a
//                  plain std::map's 7.94, because it is O(1) by tick but
//                  cache-thrashes across a wide price domain.
//
//                  The worst-first ordering is not cosmetic. Built best-first,
//                  this engine measured SLOWER than std::map, because all the
//                  churn is at the touch: each level emptied by a sweep was
//                  erased from the front, shifting the whole vector, and each
//                  new level near the touch was inserted at the front.
//                  Reversing it makes those pop_back() and push_back() and
//                  roughly doubled throughput on the canonical workload.
//   orders         intrusive doubly-linked list through the arena. O(1)
//                  append for time priority, O(1) unlink for cancel, and no
//                  pointer chasing outside one array.
//   allocation     pre-sized arena with an intrusive free list. Zero heap
//                  allocation on the matching path — a freed node's slot is
//                  reused rather than returned.
//   id -> node     open-addressing hash map, power-of-two capacity, linear
//                  probing. One cache line per probe instead of a pointer
//                  chase per bucket.
//
// A node stores its PRICE rather than an index into the level vector, because
// a sorted vector shifts its elements on every insert and erase — a stored
// level index would dangle constantly. Finding the level is a binary search
// over a few hundred contiguous entries, which is cheaper than it sounds and
// far cheaper than the bug class the alternative invites.
//
// Realistic expectation: the whole public field, including 52 engines by
// professional trading-industry engineers, sits under 8.19 M/s, and a good
// std::map engine already reaches 7.94. There is no "10x over STL" here and
// this file does not pretend otherwise.
#ifndef MATCH_ARENA_BOOK_HPP
#define MATCH_ARENA_BOOK_HPP

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "match/report.hpp"
#include "match/types.hpp"

namespace match {

namespace detail {

constexpr std::uint32_t kNil = 0xFFFFFFFFu;

// Open-addressing map from OrderId to a node index. Tombstoned erase, because
// backward-shift deletion would move entries the caller may be holding an
// index to mid-operation.
class IdIndex {
  public:
    explicit IdIndex(std::size_t capacity_hint = 1u << 12) {
        std::size_t capacity = 16;
        while (capacity < capacity_hint * 2) capacity <<= 1;
        slots_.assign(capacity, Slot{});
        mask_ = capacity - 1;
    }

    void insert(OrderId id, std::uint32_t node) {
        if ((used_ + tombstones_ + 1) * 4 >= slots_.size() * 3) grow();
        const std::uint64_t key = value_of(id);
        std::size_t i = hash(key) & mask_;
        std::size_t first_free = SIZE_MAX;
        while (true) {
            Slot& slot = slots_[i];
            if (slot.state == State::Empty) {
                const std::size_t target = first_free == SIZE_MAX ? i : first_free;
                if (slots_[target].state == State::Tombstone) --tombstones_;
                slots_[target] = Slot{key, node, State::Used};
                ++used_;
                return;
            }
            if (slot.state == State::Tombstone && first_free == SIZE_MAX) first_free = i;
            if (slot.state == State::Used && slot.key == key) {
                slot.node = node;  // reused id, previous one already retired
                return;
            }
            i = (i + 1) & mask_;
        }
    }

    std::uint32_t find(OrderId id) const {
        const std::uint64_t key = value_of(id);
        std::size_t i = hash(key) & mask_;
        while (true) {
            const Slot& slot = slots_[i];
            if (slot.state == State::Empty) return kNil;
            if (slot.state == State::Used && slot.key == key) return slot.node;
            i = (i + 1) & mask_;
        }
    }

    void erase(OrderId id) {
        const std::uint64_t key = value_of(id);
        std::size_t i = hash(key) & mask_;
        while (true) {
            Slot& slot = slots_[i];
            if (slot.state == State::Empty) return;
            if (slot.state == State::Used && slot.key == key) {
                slot.state = State::Tombstone;
                --used_;
                ++tombstones_;
                return;
            }
            i = (i + 1) & mask_;
        }
    }

    std::size_t size() const { return used_; }

  private:
    enum class State : std::uint8_t { Empty, Used, Tombstone };
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t node = kNil;
        State state = State::Empty;
    };

    // Order ids in the canonical workload are dense and sequential, so the
    // low bits alone would cluster badly under linear probing. This is
    // splitmix64's finaliser: cheap, and it spreads sequential keys.
    static std::uint64_t hash(std::uint64_t x) {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return x;
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.size() * 2, Slot{});
        mask_ = slots_.size() - 1;
        used_ = 0;
        tombstones_ = 0;
        for (const Slot& slot : old) {
            if (slot.state == State::Used) insert(OrderId{slot.key}, slot.node);
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t used_ = 0;
    std::size_t tombstones_ = 0;
};

}  // namespace detail

template <typename Sink>
class ArenaBook {
  public:
    explicit ArenaBook(Sink sink, std::size_t arena_hint = 1u << 16)
        : sink_(std::move(sink)), index_(arena_hint) {
        nodes_.reserve(arena_hint);
        bids_.reserve(1024);
        asks_.reserve(1024);
    }

    void on_new_order(const Order& order) {
        emit(Report::order_ack(order.seq, order.id, order.side, order.price, order.quantity));

        const Quantity residual =
            sweep(order.side, order.price, order.quantity, order.id, order.seq);
        if (!residual) return;

        if (order.tif == TimeInForce::Ioc) {
            emit(Report::cancel_ack(order.seq, order.id, order.side, order.price, residual));
            return;
        }
        rest(order.side, order.price, order.id, residual);
    }

    void on_cancel(SeqNum seq, OrderId id) {
        const std::uint32_t node = index_.find(id);
        if (node == detail::kNil) {
            emit(Report::cancel_reject(seq, id));
            return;
        }
        const Side side = nodes_[node].side;
        const Price price = nodes_[node].price;
        const Quantity withdrawn = nodes_[node].remaining;
        unlink(node);
        index_.erase(id);
        emit(Report::cancel_ack(seq, id, side, price, withdrawn));
    }

    void on_modify(SeqNum seq, OrderId id, Side side, Price new_price, Quantity new_qty) {
        const std::uint32_t node = index_.find(id);
        if (node == detail::kNil) {
            emit(Report::modify_reject(seq, id));
            return;
        }
        assert(nodes_[node].side == side && "modify disagrees with the resting order's side");
        unlink(node);
        index_.erase(id);

        const Quantity residual = sweep(side, new_price, new_qty, id, seq);
        if (residual) rest(side, new_price, id, residual);

        emit(Report::modify_ack(seq, id, side, new_price, new_qty));
    }

    Quantity reduce(OrderId id, Quantity delta) {
        const std::uint32_t node = index_.find(id);
        if (node == detail::kNil) return Quantity{0};

        const Quantity removed{std::min(delta.units, nodes_[node].remaining.units)};
        if (removed == nodes_[node].remaining) {
            unlink(node);
            index_.erase(id);
            return removed;
        }
        nodes_[node].remaining -= removed;
        level_of(nodes_[node].side, nodes_[node].price)->total -= removed.units;
        return removed;
    }

    Quantity consume_at(Side side, Price price, Quantity qty) {
        LevelEntry* level = level_of(side, price);
        if (level == nullptr) return Quantity{0};

        Quantity removed{0};
        while (qty && level->head != detail::kNil) {
            Node& front = nodes_[level->head];
            const Quantity take{std::min(qty.units, front.remaining.units)};

            qty -= take;
            removed += take;
            front.remaining -= take;
            level->total -= take.units;

            if (front.remaining) break;
            const OrderId id = front.id;
            const std::uint32_t node = level->head;
            unlink(node);
            index_.erase(id);
            // unlink may have erased the level entirely.
            level = level_of(side, price);
            if (level == nullptr) break;
        }
        return removed;
    }

    Price best_bid() const noexcept { return bids_.empty() ? Price::no_bid() : bids_.back().price; }

    Price best_ask() const noexcept { return asks_.empty() ? Price::no_ask() : asks_.back().price; }

    AggregateQuantity depth_at(Price price, Side side) const noexcept {
        const LevelEntry* level = const_cast<ArenaBook*>(this)->level_of(side, price);
        return level == nullptr ? 0 : level->total;
    }

    std::optional<AggregateQuantity> quantity_ahead(OrderId id) const {
        const std::uint32_t node = index_.find(id);
        if (node == detail::kNil) return std::nullopt;

        AggregateQuantity ahead = 0;
        for (std::uint32_t i = nodes_[node].prev; i != detail::kNil; i = nodes_[i].prev) {
            ahead += nodes_[i].remaining.units;
        }
        return ahead;
    }

    struct LevelView {
        Price price{};
        AggregateQuantity quantity{};
    };

    std::size_t top_levels(Side side, LevelView* out, std::size_t max) const {
        const auto& levels = side == Side::Buy ? bids_ : asks_;
        const std::size_t n = std::min(max, levels.size());
        // Stored worst-first, so walk backwards to emit best-first.
        for (std::size_t i = 0; i < n; ++i) {
            const LevelEntry& level = levels[levels.size() - 1 - i];
            out[i] = LevelView{level.price, level.total};
        }
        return n;
    }

    bool empty() const noexcept { return bids_.empty() && asks_.empty(); }
    std::size_t resting_count() const noexcept { return index_.size(); }

    bool validate() const {
        if (!validate_side(bids_, Side::Buy)) return false;
        if (!validate_side(asks_, Side::Sell)) return false;
        if (!bids_.empty() && !asks_.empty() &&
            bids_.back().price.ticks >= asks_.back().price.ticks) {
            return false;
        }
        std::size_t counted = 0;
        for (const auto& level : bids_) counted += count_nodes(level);
        for (const auto& level : asks_) counted += count_nodes(level);
        return counted == index_.size();
    }

  private:
    // 32 bytes, so two nodes share a cache line and a queue walk touches half
    // as many lines as it otherwise would.
    struct Node {
        OrderId id{};
        Quantity remaining{};
        Price price{};
        std::uint32_t next = detail::kNil;
        std::uint32_t prev = detail::kNil;
        Side side{};
    };

    struct LevelEntry {
        Price price{};
        AggregateQuantity total = 0;
        std::uint32_t head = detail::kNil;
        std::uint32_t tail = detail::kNil;
    };

    void emit(const Report& report) { sink_(report); }

    // Levels are sorted WORST price first, so the best is at back().
    //
    // The obvious layout is the other way round — best at index 0, matching how
    // one reads a book. It is measurably worse, and the reason is that all the
    // churn is at the touch: every level that empties during a sweep is erased
    // from the front, shifting the entire vector each time, and every new level
    // near the touch is inserted at the front. Putting the hot end at back()
    // turns those into pop_back() and push_back().
    static bool level_before(Price a, Price b, Side side) {
        return side == Side::Buy ? a.ticks < b.ticks : a.ticks > b.ticks;
    }

    std::vector<LevelEntry>& levels_for(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const std::vector<LevelEntry>& levels_for(Side side) const {
        return side == Side::Buy ? bids_ : asks_;
    }

    LevelEntry* level_of(Side side, Price price) {
        auto& levels = levels_for(side);
        const auto it = std::lower_bound(levels.begin(), levels.end(), price,
                                         [side](const LevelEntry& entry, Price p) {
                                             return level_before(entry.price, p, side);
                                         });
        if (it == levels.end() || it->price != price) return nullptr;
        return &*it;
    }

    std::uint32_t allocate() {
        if (free_head_ != detail::kNil) {
            const std::uint32_t node = free_head_;
            free_head_ = nodes_[node].next;
            nodes_[node] = Node{};
            return node;
        }
        nodes_.push_back(Node{});
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    }

    void release(std::uint32_t node) {
        nodes_[node].next = free_head_;
        free_head_ = node;
    }

    void rest(Side side, Price price, OrderId id, Quantity qty) {
        auto& levels = levels_for(side);
        auto it = std::lower_bound(levels.begin(), levels.end(), price,
                                   [side](const LevelEntry& entry, Price p) {
                                       return level_before(entry.price, p, side);
                                   });
        if (it == levels.end() || it->price != price) {
            it = levels.insert(it, LevelEntry{price, 0, detail::kNil, detail::kNil});
        }

        const std::uint32_t node = allocate();
        nodes_[node].id = id;
        nodes_[node].remaining = qty;
        nodes_[node].price = price;
        nodes_[node].side = side;
        nodes_[node].next = detail::kNil;
        nodes_[node].prev = it->tail;

        if (it->tail != detail::kNil) {
            nodes_[it->tail].next = node;
        } else {
            it->head = node;
        }
        it->tail = node;
        it->total += qty.units;

        index_.insert(id, node);
    }

    void unlink(std::uint32_t node) {
        const Side side = nodes_[node].side;
        const Price price = nodes_[node].price;
        auto& levels = levels_for(side);
        const auto it = std::lower_bound(levels.begin(), levels.end(), price,
                                         [side](const LevelEntry& entry, Price p) {
                                             return level_before(entry.price, p, side);
                                         });
        assert(it != levels.end() && it->price == price && "unlinking from a level that is gone");

        const std::uint32_t prev = nodes_[node].prev;
        const std::uint32_t next = nodes_[node].next;
        if (prev != detail::kNil) {
            nodes_[prev].next = next;
        } else {
            it->head = next;
        }
        if (next != detail::kNil) {
            nodes_[next].prev = prev;
        } else {
            it->tail = prev;
        }
        it->total -= nodes_[node].remaining.units;
        release(node);

        if (it->head == detail::kNil) {
            if (it + 1 == levels.end()) {
                levels.pop_back();
            } else {
                levels.erase(it);
            }
        }
    }

    Quantity sweep(Side aggressor_side, Price limit, Quantity qty, OrderId taker, SeqNum seq) {
        auto& levels = levels_for(opposite(aggressor_side));
        while (qty && !levels.empty()) {
            LevelEntry& level = levels.back();
            const Price maker_price = level.price;
            if (!crosses(limit, maker_price, aggressor_side)) break;

            while (qty && level.head != detail::kNil) {
                Node& maker = nodes_[level.head];
                const Quantity fill{std::min(qty.units, maker.remaining.units)};

                emit(Report::trade(seq, maker_price, fill, maker.id, taker));

                qty -= fill;
                maker.remaining -= fill;
                level.total -= fill.units;

                if (maker.remaining) break;
                const OrderId id = maker.id;
                const std::uint32_t node = level.head;
                const std::uint32_t next = maker.next;
                level.head = next;
                if (next != detail::kNil) {
                    nodes_[next].prev = detail::kNil;
                } else {
                    level.tail = detail::kNil;
                }
                release(node);
                index_.erase(id);
            }
            // pop_back(), not erase(begin()) — this is the whole point of the
            // worst-first layout.
            if (level.head == detail::kNil) levels.pop_back();
        }
        return qty;
    }

    std::size_t count_nodes(const LevelEntry& level) const {
        std::size_t n = 0;
        for (std::uint32_t i = level.head; i != detail::kNil; i = nodes_[i].next) ++n;
        return n;
    }

    bool validate_side(const std::vector<LevelEntry>& levels, Side side) const {
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const LevelEntry& level = levels[i];
            if (level.head == detail::kNil) return false;  // a phantom level
            if (i > 0 && !level_before(levels[i - 1].price, level.price, side)) return false;

            AggregateQuantity sum = 0;
            std::uint32_t previous = detail::kNil;
            for (std::uint32_t node = level.head; node != detail::kNil; node = nodes_[node].next) {
                if (!nodes_[node].remaining) return false;
                if (nodes_[node].prev != previous) return false;
                if (nodes_[node].price != level.price || nodes_[node].side != side) return false;
                if (index_.find(nodes_[node].id) != node) return false;
                sum += nodes_[node].remaining.units;
                previous = node;
            }
            if (previous != level.tail) return false;
            if (sum != level.total) return false;
        }
        return true;
    }

    Sink sink_;
    std::vector<Node> nodes_;
    std::uint32_t free_head_ = detail::kNil;
    std::vector<LevelEntry> bids_;
    std::vector<LevelEntry> asks_;
    detail::IdIndex index_;
};

}  // namespace match

#endif  // MATCH_ARENA_BOOK_HPP
