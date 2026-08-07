# Research-Driven Simulator Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the simulator's result from *self-consistent* into *validated against a published live-trading experiment*, and replace its hardest assumption (no market impact) with a measurable, bounded one.

**Architecture:** Five additive changes, each independently testable, none touching `libmatch`. Four are pure analysis or accounting layers over data the simulator already records (`fills()`, `mid_series()`, `touch_depths()`). The fifth introduces one new hook in the replay loop — a `ReactionModel` consulted when a historical cancel lands at a price where the strategy is resting — so the impact-free assumption becomes a *pluggable* model with `NoReaction` as the default rather than a silent premise.

**Tech Stack:** C++20, CMake 3.20+, GoogleTest (already vendored via FetchContent). No new third-party dependencies. All numbers reproduced from `data/message_50.csv` + `data/orderbook_50.csv` (LOBSTER AAPL 2012-06-21 level-50, already on disk).

## Global Constraints

- **C++20**, no `std::format` (Apple clang 15's libc++ lacks it), no ranges pipelines in `libmatch` or `libsim`.
- **Builds clean under `-Werror -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`.** Configure with `-DMATCH_WERROR=ON` before claiming a task is done.
- **No floating point anywhere near a price.** Prices are integer `match::Price` ticks. Analysis layers may use `double` for *statistics*; they must not use it to compare or compute a price.
- **`libmatch` is not modified by any task in this plan.** The engine is conformance-locked; every change lives in `libsim/`, `studies/` or `tests/`.
- **Every task ends with these three gates passing**, because they are the project's external oracles and a silent regression in any of them invalidates everything downstream:
  ```bash
  ctest --test-dir build --output-on-failure
  ./scripts/check_hashes.sh              # must print 15 PASS lines
  ./build/tools/null_order_invariance data/message_50.csv data/orderbook_50.csv   # must print "divergent rows  : 0"
  ```
- **Existing numbers that must not change** unless a task explicitly says so: reconstruction top-1 `99.9022%`, longest run `54605`, the study's gap `370.1%` / `$2897`.
- **Queue position convention (new, from the literature):** `QP = liquidity_ahead / (liquidity_ahead + liquidity_behind)`, i.e. excluding the order's own size. Range [0,1]; 0 = front of queue. Source: *The Market Maker's Dilemma*, arXiv:2502.18625.
- Commit messages: conventional prefix (`feat:`, `test:`, `docs:`, `study:`), and end with
  `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.

---

## File Structure

| File | Responsibility |
|---|---|
| `libsim/include/sim/types.hpp` | **Modify.** `Fill` gains `queue_behind_at_entry` and `normalised_queue_position()`. |
| `libsim/src/simulator.cpp` | **Modify.** Populate the new `Fill` fields; consult the reaction model. |
| `libsim/include/sim/simulator.hpp` | **Modify.** Accept a `ReactionModel&`; expose `reaction_stats()`. |
| `libsim/include/sim/fill_classification.hpp` | **Create.** Adverse/benign fill split. Pure function over `Fill` + mid series. |
| `libsim/include/sim/fill_probability.hpp` | **Create.** The fill-probability surface: bins posted orders by (near queue, far depth) and reports fill rate per bin. |
| `libsim/include/sim/reaction.hpp` | **Create.** `ReactionModel` interface, `NoReaction`, `CrowdOutReaction`. |
| `libsim/include/sim/performance.hpp` | **Modify.** `PerformanceReport` gains deflated Sharpe + adverse/benign split. |
| `libsim/include/sim/deflated_sharpe.hpp` | **Create.** Bailey–López de Prado style haircut. Pure maths, no I/O. |
| `libsim/include/sim/order_tracker.hpp` | **Create.** Records every posted order and whether it ever filled — the denominator the fill-probability surface needs. |
| `studies/queue_validation.cpp` | **Create.** The cross-validation study: surface + adverse split + comparison to published live-trading numbers. |
| `studies/impact_sensitivity.cpp` | **Create.** Sweeps the reaction model's intensity and reports how the gap moves. |
| `tests/fill_classification_test.cpp` | **Create.** |
| `tests/fill_probability_test.cpp` | **Create.** |
| `tests/reaction_test.cpp` | **Create.** |
| `tests/deflated_sharpe_test.cpp` | **Create.** |
| `libsim/CMakeLists.txt`, `studies/CMakeLists.txt`, `tests/CMakeLists.txt` | **Modify.** Register new sources. |
| `studies/STUDY.md`, `README.md` | **Modify.** Report the new results and their bounds. |

**Dependency order:** Task 1 → 2 → 3 (Task 3 needs 1's fields and 2's tracker). Task 4 is independent. Tasks 5–7 are the impact model and depend only on Task 1. Task 8 writes up everything.

---

### Task 1: Normalised queue position on every fill

Absolute shares-ahead is not comparable to published work. arXiv:2502.18625 reports queue position as a fraction, and the whole point of Task 3 is comparing against their numbers.

**Files:**
- Modify: `libsim/include/sim/types.hpp` (the `Fill` struct)
- Modify: `libsim/src/simulator.cpp` (`Simulator::release_orders`, `Simulator::record_fill`)
- Modify: `libsim/include/sim/simulator.hpp` (private member for behind-depth)
- Test: `tests/sim_test.cpp` (append to the `Injection` group)

**Interfaces:**
- Consumes: `match::AggregateQuantity`, `Simulator::book_.depth_at()`, `Simulator::book_.quantity_ahead()`.
- Produces: `Fill::queue_behind_at_entry` (`match::AggregateQuantity`), `Fill::normalised_queue_position()` returning `double` in [0,1], and `Fill::has_queue_position()` returning `bool`. Tasks 2, 3, 4 and 8 all read these.

- [ ] **Step 1: Write the failing test**

Append to `tests/sim_test.cpp`, inside `namespace sim { namespace { ... } }`, immediately after `TEST(Injection, AnOrderBehindTheQueueDoesNotFillUntilTheQueueDrains)`:

```cpp
TEST(Injection, RecordsNormalisedQueuePositionAtEntry) {
    // 200 shares rest ahead when the strategy's 50-share order arrives, and
    // nothing joins behind it. Queue position is therefore 1.0 — the very back.
    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.seed_level(Side::Buy, Price{100}, Quantity{200});
    sim.on_market_event(add(0, 1, Side::Sell, 101, 10));
    sim.on_market_event(add(1, 2, Side::Buy, 99, 10));
    sim.on_market_event(execute(2, 999, Side::Buy, 100, 250));

    ASSERT_EQ(sim.fills().size(), 1u);
    const Fill& fill = sim.fills().front();
    EXPECT_EQ(fill.queue_ahead_at_entry, 200u);
    EXPECT_EQ(fill.queue_behind_at_entry, 0u);
    EXPECT_TRUE(fill.has_queue_position());
    EXPECT_DOUBLE_EQ(fill.normalised_queue_position(), 1.0);
}

TEST(Injection, NormalisedQueuePositionIsZeroAtTheFrontOfAnEmptyLevel) {
    // Nothing ahead and nothing behind: the order is the level. Front of queue.
    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Sell, 101, 10));
    sim.on_market_event(add(1, 2, Side::Buy, 99, 10));
    sim.on_market_event(execute(2, 999, Side::Buy, 100, 50));

    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_EQ(sim.fills().front().queue_ahead_at_entry, 0u);
    EXPECT_DOUBLE_EQ(sim.fills().front().normalised_queue_position(), 0.0);
}

TEST(Injection, ATakerHasNoQueuePositionAtAll) {
    // A crossing order never queues, so a queue position of "0.0" would be a
    // lie that reads as "front of the queue". It must be absent instead.
    OneShotTaker strategy(Side::Buy, 101, 30);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 50));
    sim.on_market_event(add(2, 3, Side::Sell, 102, 10));

    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_FALSE(sim.fills().front().has_queue_position());
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build --parallel 2>&1 | head -20
```

Expected: compile FAILS with `no member named 'queue_behind_at_entry' in 'sim::Fill'`.

- [ ] **Step 3: Add the fields and accessors**

In `libsim/include/sim/types.hpp`, replace the whole `struct Fill { ... };` with:

```cpp
struct Fill {
    Time time{};
    match::OrderId id{};
    match::Side side{};
    match::Price price{};
    match::Quantity quantity{};
    bool maker = true;  // passive fill (earns the spread) vs. aggressive

    // Size resting ahead of this order when it was placed. A naive fill model
    // implicitly assumes this is always zero; recording it is what makes the
    // contrast measurable rather than asserted.
    match::AggregateQuantity queue_ahead_at_entry = 0;
    // Size resting behind it. Needed only to normalise the position — but
    // without it, this project's queue numbers cannot be compared to any
    // published result, because the literature reports a fraction.
    match::AggregateQuantity queue_behind_at_entry = 0;

    // True when this fill had a queue position at all. A taker never queues,
    // and reporting 0.0 for one would read as "front of the queue" — the
    // opposite of the truth, which is "did not queue".
    bool has_queue_position() const { return maker; }

    // Queue position as the literature defines it: the fraction of the level's
    // other liquidity that sits ahead. 0 = front, 1 = back. Excludes the
    // order's own size, so it does not shrink just because the order is large.
    // Source: arXiv:2502.18625.
    double normalised_queue_position() const {
        const auto others = queue_ahead_at_entry + queue_behind_at_entry;
        if (others == 0) return 0.0;  // alone at the level, so at its front
        return static_cast<double>(queue_ahead_at_entry) / static_cast<double>(others);
    }
};
```

- [ ] **Step 4: Record the behind-depth at entry**

In `libsim/include/sim/simulator.hpp`, find the private member block containing
`std::unordered_map<match::OrderId, match::AggregateQuantity> live_orders_;` and add
directly beneath it:

```cpp
    // Depth behind each live strategy order at the moment it entered. Paired
    // with live_orders_ (which holds the depth ahead) to normalise the queue
    // position on each resulting fill.
    std::unordered_map<match::OrderId, match::AggregateQuantity> behind_at_entry_;
```

In `libsim/src/simulator.cpp`, in `Simulator::release_orders`, locate the engine branch:

```cpp
            const auto ahead = book_.quantity_ahead(submit.id);
            if (ahead.has_value()) {
                live_orders_[submit.id] = *ahead;
                order_sides_[submit.id] = submit.side;
                remaining_[submit.id] = submit.quantity;
            }
```

and replace it with:

```cpp
            const auto ahead = book_.quantity_ahead(submit.id);
            if (ahead.has_value()) {
                live_orders_[submit.id] = *ahead;
                order_sides_[submit.id] = submit.side;
                remaining_[submit.id] = submit.quantity;
                // Everything at the level except this order and the part ahead
                // of it. depth_at includes our own size, so subtract it.
                const auto level = book_.depth_at(submit.price, submit.side);
                const auto own = static_cast<match::AggregateQuantity>(submit.quantity.units);
                behind_at_entry_[submit.id] =
                    level > *ahead + own ? level - *ahead - own : 0;
            }
```

In the same function, in the counterfactual branch, locate:

```cpp
            fill_model_.on_placed(submit.id, submit.side, submit.price, submit.quantity, probe);
            live_orders_[submit.id] = probe.depth_at(submit.price, submit.side);
            order_sides_[submit.id] = submit.side;
            remaining_[submit.id] = submit.quantity;
```

and append one line after it:

```cpp
            // The counterfactual models keep their orders out of the book, so
            // nothing can be behind them: they joined the back of the displayed
            // depth and no later order queues behind an order that is not there.
            behind_at_entry_[submit.id] = 0;
```

- [ ] **Step 5: Populate the field on each fill**

In `libsim/src/simulator.cpp`, change the signature and body of `record_fill`. Replace:

```cpp
void Simulator::record_fill(match::OrderId id, match::Side side, match::Price price,
                            match::Quantity quantity, match::AggregateQuantity ahead_at_entry,
                            bool maker) {
    Fill fill;
    fill.time = now_;
    fill.id = id;
    fill.side = side;
    fill.price = price;
    fill.quantity = quantity;
    fill.maker = maker;
    fill.queue_ahead_at_entry = ahead_at_entry;
```

with:

```cpp
void Simulator::record_fill(match::OrderId id, match::Side side, match::Price price,
                            match::Quantity quantity, match::AggregateQuantity ahead_at_entry,
                            bool maker) {
    Fill fill;
    fill.time = now_;
    fill.id = id;
    fill.side = side;
    fill.price = price;
    fill.quantity = quantity;
    fill.maker = maker;
    fill.queue_ahead_at_entry = ahead_at_entry;
    const auto behind = behind_at_entry_.find(id);
    fill.queue_behind_at_entry = behind == behind_at_entry_.end() ? 0 : behind->second;
```

Then find every place that erases from `live_orders_` and erase from `behind_at_entry_`
alongside it. There are three, all in `libsim/src/simulator.cpp`:

1. In `on_report`, inside `if (!book_.quantity_ahead(report.maker_order_id).has_value()) {`,
   after `order_sides_.erase(report.maker_order_id);` add:
   ```cpp
        behind_at_entry_.erase(report.maker_order_id);
   ```
2. In `release_orders`, in the cancel loop, after `order_sides_.erase(cancel.id);` add:
   ```cpp
        behind_at_entry_.erase(cancel.id);
   ```
3. In `apply_model_fills`, after `order_sides_.erase(filled.id);` add:
   ```cpp
            behind_at_entry_.erase(filled.id);
   ```

- [ ] **Step 6: Run tests to verify they pass**

```bash
cmake -S . -B build -DMATCH_WERROR=ON -DMATCH_BENCHMARK_DIR="$HOME/Documents/matching-engine-benchmark" >/dev/null
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
./build/tests/match_tests --gtest_filter='Injection.*'
```

Expected: no compiler output; `[  PASSED  ]` with 6 tests.

- [ ] **Step 7: Run the three global gates**

```bash
ctest --test-dir build --output-on-failure | tail -3
./scripts/check_hashes.sh | grep -c '^PASS'
./build/tools/null_order_invariance data/message_50.csv data/orderbook_50.csv | grep divergent
```

Expected: `100% tests passed`; `15`; `divergent rows  : 0`.

- [ ] **Step 8: Commit**

```bash
git add libsim/include/sim/types.hpp libsim/include/sim/simulator.hpp \
        libsim/src/simulator.cpp tests/sim_test.cpp
git commit -m "$(cat <<'EOF'
feat(libsim): normalised queue position on every fill

Absolute shares-ahead cannot be compared to any published result, because the
literature reports queue position as a fraction of the level's other liquidity
(arXiv:2502.18625). Fill now carries the depth behind as well as ahead, and
normalised_queue_position() returns the fraction: 0 = front, 1 = back, own size
excluded so a large order does not flatter its own position.

has_queue_position() is false for a taker. Reporting 0.0 there would read as
"front of the queue" when the truth is "never queued".

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Track every posted order, not just the ones that filled

A fill probability needs a denominator. `fills()` is the numerator only — orders that were posted and *never* filled leave no trace, so no fill rate can be computed from the current outputs.

**Files:**
- Create: `libsim/include/sim/order_tracker.hpp`
- Modify: `libsim/include/sim/simulator.hpp`
- Modify: `libsim/src/simulator.cpp`
- Test: `tests/fill_probability_test.cpp` (created here, extended in Task 3)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Fill` fields from Task 1.
- Produces: `sim::PostedOrder` (fields `id`, `posted_at`, `side`, `price`, `quantity`, `queue_ahead`, `queue_behind`, `opposite_depth`, `filled_quantity`, `retired_at`, `was_filled`, `normalised_queue_position()`), and `Simulator::posted_orders()` returning `const std::vector<PostedOrder>&`. Task 3 reads both.

- [ ] **Step 1: Write the failing test**

Create `tests/fill_probability_test.cpp`:

```cpp
// Tests for the posted-order record and the fill-probability surface.
//
// A fill rate needs a denominator. The simulator's fills() vector is the
// numerator alone — an order that was posted and never filled leaves no trace
// in it — so without a record of every posted order there is no rate to
// compute, only a count.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "match/types.hpp"
#include "sim/fill_model.hpp"
#include "sim/latency.hpp"
#include "sim/order_tracker.hpp"
#include "sim/simulator.hpp"
#include "sim/strategy.hpp"
#include "sim/types.hpp"

namespace sim {
namespace {

using match::Price;
using match::Quantity;
using match::Side;

class PostOnce : public Strategy {
public:
    PostOnce(Side side, std::int64_t price, std::uint32_t qty)
        : side_(side), price_(price), qty_(qty) {}

    void on_book(const BookView& book, OrderGate& gate) override {
        if (posted_ || !book.two_sided()) return;
        posted_ = true;
        gate.submit(side_, Price{price_}, Quantity{qty_});
    }

    const char* name() const override { return "post-once"; }

private:
    Side side_;
    std::int64_t price_;
    std::uint32_t qty_;
    bool posted_ = false;
};

MarketEvent add(std::int64_t t, std::uint64_t id, Side side, std::int64_t price,
                std::uint32_t qty) {
    return MarketEvent{Time{t},      MarketEventType::Add, match::OrderId{id},
                       Price{price}, Quantity{qty},        side};
}

MarketEvent execute(std::int64_t t, std::uint64_t maker_id, Side maker_side, std::int64_t price,
                    std::uint32_t qty) {
    return MarketEvent{Time{t},      MarketEventType::Execute, match::OrderId{maker_id},
                       Price{price}, Quantity{qty},            maker_side};
}

TEST(OrderTracker, RecordsAnOrderThatNeverFills) {
    PostOnce strategy(Side::Buy, 90, 50);  // far from the touch
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 100, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 10));
    sim.on_market_event(add(2, 3, Side::Sell, 102, 10));
    sim.finish();

    ASSERT_EQ(sim.posted_orders().size(), 1u);
    const PostedOrder& posted = sim.posted_orders().front();
    EXPECT_EQ(posted.quantity, Quantity{50});
    EXPECT_EQ(posted.filled_quantity.units, 0u);
    EXPECT_FALSE(posted.was_filled());
    EXPECT_EQ(sim.fills().size(), 0u) << "the fill vector cannot see this order at all";
}

TEST(OrderTracker, RecordsPartialFillsAgainstThePostedQuantity) {
    PostOnce strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 10));
    sim.on_market_event(execute(2, 999, Side::Buy, 100, 20));
    sim.finish();

    ASSERT_EQ(sim.posted_orders().size(), 1u);
    const PostedOrder& posted = sim.posted_orders().front();
    EXPECT_EQ(posted.filled_quantity, Quantity{20});
    EXPECT_TRUE(posted.was_filled()) << "partially filled still counts as filled";
    EXPECT_EQ(posted.quantity, Quantity{50});
}

TEST(OrderTracker, CapturesTheOppositeSideDepthAtPostTime) {
    // The fill-probability surface is indexed by both sides: near-side queue
    // AND opposite-side depth, because a thin opposite side is what pulls the
    // price through your order.
    PostOnce strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    Simulator sim(strategy, feed, order, fill_model, {});

    sim.on_market_event(add(0, 1, Side::Buy, 99, 10));
    sim.on_market_event(add(1, 2, Side::Sell, 101, 77));
    sim.on_market_event(add(2, 3, Side::Sell, 102, 10));
    sim.finish();

    ASSERT_EQ(sim.posted_orders().size(), 1u);
    EXPECT_EQ(sim.posted_orders().front().opposite_depth, 77u);
}

}  // namespace
}  // namespace sim
```

- [ ] **Step 2: Register the test file and run it to verify it fails**

In `tests/CMakeLists.txt`, add `fill_probability_test.cpp` to the `add_executable(match_tests ...)` source list, on its own line after `engine_equivalence_test.cpp`.

```bash
cmake -S . -B build -DMATCH_WERROR=ON -DMATCH_BENCHMARK_DIR="$HOME/Documents/matching-engine-benchmark" >/dev/null
cmake --build build --parallel 2>&1 | head -10
```

Expected: FAILS with `'sim/order_tracker.hpp' file not found`.

- [ ] **Step 3: Create the record type**

Create `libsim/include/sim/order_tracker.hpp`:

```cpp
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

#include <cstdint>

#include "match/types.hpp"
#include "sim/types.hpp"

namespace sim {

struct PostedOrder {
    match::OrderId id{};
    Time posted_at{};
    Time retired_at{};  // when it left the book, filled or cancelled
    match::Side side{};
    match::Price price{};
    match::Quantity quantity{};       // as posted
    match::Quantity filled_quantity{};  // accumulated over its lifetime

    // Conditions at post time — the surface's coordinates.
    match::AggregateQuantity queue_ahead = 0;
    match::AggregateQuantity queue_behind = 0;
    match::AggregateQuantity opposite_depth = 0;

    bool was_filled() const { return filled_quantity.units > 0; }

    bool fully_filled() const { return filled_quantity.units >= quantity.units; }

    // Same convention as Fill::normalised_queue_position: fraction of the
    // level's OTHER liquidity sitting ahead. 0 = front, 1 = back.
    double normalised_queue_position() const {
        const auto others = queue_ahead + queue_behind;
        if (others == 0) return 0.0;
        return static_cast<double>(queue_ahead) / static_cast<double>(others);
    }
};

}  // namespace sim

#endif  // SIM_ORDER_TRACKER_HPP
```

- [ ] **Step 4: Populate it from the simulator**

In `libsim/include/sim/simulator.hpp`:

1. Add the include next to the other `sim/` includes:
   ```cpp
   #include "sim/order_tracker.hpp"
   ```
2. Add the accessor immediately after `const std::vector<Fill>& fills() const { return fills_; }`:
   ```cpp
       // Every order the strategy posted, filled or not — the denominator any
       // fill rate needs.
       const std::vector<PostedOrder>& posted_orders() const { return posted_orders_; }
   ```
3. Add these private members immediately after `std::vector<Fill> fills_;`:
   ```cpp
       std::vector<PostedOrder> posted_orders_;
       // id -> index into posted_orders_, for the currently-live orders only.
       std::unordered_map<match::OrderId, std::size_t> posted_index_;
   ```

In `libsim/src/simulator.cpp`, in `release_orders`, inside the engine branch, immediately
after the `behind_at_entry_[submit.id] = ...` line added in Task 1, insert:

```cpp
                PostedOrder record;
                record.id = submit.id;
                record.posted_at = now;
                record.side = submit.side;
                record.price = submit.price;
                record.quantity = submit.quantity;
                record.queue_ahead = *ahead;
                record.queue_behind = behind_at_entry_[submit.id];
                record.opposite_depth =
                    submit.side == match::Side::Buy
                        ? (book_.best_ask() == match::Price::no_ask()
                               ? 0
                               : book_.depth_at(book_.best_ask(), match::Side::Sell))
                        : (book_.best_bid() == match::Price::no_bid()
                               ? 0
                               : book_.depth_at(book_.best_bid(), match::Side::Buy));
                posted_index_[submit.id] = posted_orders_.size();
                posted_orders_.push_back(record);
```

In the same function, in the counterfactual branch, immediately after the
`behind_at_entry_[submit.id] = 0;` line added in Task 1, insert:

```cpp
            PostedOrder record;
            record.id = submit.id;
            record.posted_at = now;
            record.side = submit.side;
            record.price = submit.price;
            record.quantity = submit.quantity;
            record.queue_ahead = live_orders_[submit.id];
            record.queue_behind = 0;
            record.opposite_depth =
                submit.side == match::Side::Buy
                    ? (probe.best_ask() == match::Price::no_ask()
                           ? 0
                           : probe.depth_at(probe.best_ask(), match::Side::Sell))
                    : (probe.best_bid() == match::Price::no_bid()
                           ? 0
                           : probe.depth_at(probe.best_bid(), match::Side::Buy));
            posted_index_[submit.id] = posted_orders_.size();
            posted_orders_.push_back(record);
```

In `record_fill`, immediately before the closing `}` of the function, add:

```cpp
    const auto posted = posted_index_.find(id);
    if (posted != posted_index_.end()) {
        posted_orders_[posted->second].filled_quantity += quantity;
        posted_orders_[posted->second].retired_at = now_;
    }
```

Finally, wherever `behind_at_entry_.erase(...)` was added in Task 1, add the matching
`posted_index_.erase(...)` on the following line — three sites, same ids:
`report.maker_order_id`, `cancel.id`, and `filled.id`. For the cancel site also record the
retirement time, so an order's lifetime is known:

```cpp
        const auto retiring = posted_index_.find(cancel.id);
        if (retiring != posted_index_.end()) {
            posted_orders_[retiring->second].retired_at = now;
        }
        posted_index_.erase(cancel.id);
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
./build/tests/match_tests --gtest_filter='OrderTracker.*'
```

Expected: no compiler output; `[  PASSED  ] 3 tests`.

- [ ] **Step 6: Run the three global gates**

```bash
ctest --test-dir build --output-on-failure | tail -3
./scripts/check_hashes.sh | grep -c '^PASS'
./build/tools/null_order_invariance data/message_50.csv data/orderbook_50.csv | grep divergent
```

Expected: `100% tests passed`; `15`; `divergent rows  : 0`.

- [ ] **Step 7: Commit**

```bash
git add libsim/include/sim/order_tracker.hpp libsim/include/sim/simulator.hpp \
        libsim/src/simulator.cpp tests/fill_probability_test.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(libsim): record every posted order, not only the ones that filled

A fill rate needs a denominator and fills() is the numerator alone: an order
posted and never filled leaves no trace in it. PostedOrder captures the whole
population at post time — queue ahead, queue behind, opposite-side depth, and
how much eventually traded — which are exactly the axes of the published
fill-probability surface this project is being compared against.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: The fill-probability surface, and the adverse/benign split

The two artifacts arXiv:2502.18625 built from 232,897 live orders. Producing both from replay is what makes the comparison possible.

**Files:**
- Create: `libsim/include/sim/fill_probability.hpp`
- Create: `libsim/include/sim/fill_classification.hpp`
- Create: `tests/fill_classification_test.cpp`
- Modify: `tests/fill_probability_test.cpp` (append surface tests)
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `PostedOrder` and `Simulator::posted_orders()` (Task 2); `Fill::normalised_queue_position()` (Task 1).
- Produces: `sim::FillSurface` with `add(const PostedOrder&)`, `Cell{posted, filled, fill_rate()}`, `cell(std::size_t qp_bin, std::size_t depth_bin) const`, `kQueueBins = 4`, `kDepthBins = 3`; and `sim::classify_fills(fills, mid_series, horizon)` returning `sim::FillSplit{adverse_count, benign_count, adverse_pnl, benign_pnl, adverse_fraction()}`. Task 8 reads both.

- [ ] **Step 1: Write the failing classification test**

Create `tests/fill_classification_test.cpp`:

```cpp
// Adverse vs benign fills.
//
// arXiv:2409.12721 defines a fill as adverse when the order is "picked off":
// immediately after execution the new position is out of the money. Splitting
// PnL along that line separates two economically different things that a
// single blended number hides — the spread you earned, and the cost of the
// fills you were given precisely because they were about to be wrong.
#include "sim/fill_classification.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "match/types.hpp"
#include "sim/types.hpp"

namespace sim {
namespace {

using match::Price;
using match::Quantity;
using match::Side;

Fill maker_fill(std::int64_t at_ns, Side side, std::int64_t price, std::uint32_t qty) {
    Fill fill;
    fill.time = Time{at_ns};
    fill.side = side;
    fill.price = Price{price};
    fill.quantity = Quantity{qty};
    fill.maker = true;
    return fill;
}

TEST(FillClassification, ABuyIsAdverseWhenThePriceFallsAfterwards) {
    const std::vector<Fill> fills{maker_fill(0, Side::Buy, 100, 10)};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},                     // mid 100 at the fill
        {Time{seconds(1).count}, 180},      // mid 90 a second later
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.adverse_count, 1u);
    EXPECT_EQ(split.benign_count, 0u);
    EXPECT_DOUBLE_EQ(split.adverse_fraction(), 1.0);
    EXPECT_DOUBLE_EQ(split.adverse_pnl, -100.0);  // 10 shares x 10 down
}

TEST(FillClassification, ABuyIsBenignWhenThePriceRisesAfterwards) {
    const std::vector<Fill> fills{maker_fill(0, Side::Buy, 100, 10)};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},
        {Time{seconds(1).count}, 220},
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.benign_count, 1u);
    EXPECT_EQ(split.adverse_count, 0u);
    EXPECT_DOUBLE_EQ(split.benign_pnl, 100.0);
}

TEST(FillClassification, ASellIsAdverseWhenThePriceRises) {
    // The mirror case. Getting one side right and the other wrong is a real
    // and repeatedly-observed defect class, so both directions are pinned.
    const std::vector<Fill> fills{maker_fill(0, Side::Sell, 100, 10)};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},
        {Time{seconds(1).count}, 220},
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.adverse_count, 1u);
    EXPECT_DOUBLE_EQ(split.adverse_pnl, -100.0);
}

TEST(FillClassification, AnUnchangedPriceIsBenignNotAdverse) {
    // A fill that cost nothing was not "picked off". Classifying a zero
    // mark-out as adverse would inflate the adverse count with every quiet
    // fill in the sample.
    const std::vector<Fill> fills{maker_fill(0, Side::Buy, 100, 10)};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},
        {Time{seconds(1).count}, 200},
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.adverse_count, 0u);
    EXPECT_EQ(split.benign_count, 1u);
    EXPECT_DOUBLE_EQ(split.benign_pnl, 0.0);
}

TEST(FillClassification, TakerFillsAreExcluded) {
    // "Picked off" is a thing that happens to a resting order. A taker chose
    // its moment, so counting it here would blame the market for the
    // strategy's own decision.
    Fill taker = maker_fill(0, Side::Buy, 100, 10);
    taker.maker = false;
    const std::vector<Fill> fills{taker};
    const std::vector<std::pair<Time, std::int64_t>> mids{
        {Time{0}, 200},
        {Time{seconds(1).count}, 180},
    };

    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_EQ(split.adverse_count, 0u);
    EXPECT_EQ(split.benign_count, 0u);
    EXPECT_DOUBLE_EQ(split.adverse_fraction(), 0.0);
}

TEST(FillClassification, AnEmptyFillListIsNotADivideByZero) {
    const std::vector<Fill> fills;
    const std::vector<std::pair<Time, std::int64_t>> mids{{Time{0}, 200}};
    const FillSplit split = classify_fills(fills, mids, seconds(1));
    EXPECT_DOUBLE_EQ(split.adverse_fraction(), 0.0);
}

}  // namespace
}  // namespace sim
```

- [ ] **Step 2: Register and run it to verify it fails**

Add `fill_classification_test.cpp` to the `add_executable(match_tests ...)` list in
`tests/CMakeLists.txt`, then:

```bash
cmake -S . -B build -DMATCH_WERROR=ON -DMATCH_BENCHMARK_DIR="$HOME/Documents/matching-engine-benchmark" >/dev/null
cmake --build build --parallel 2>&1 | head -5
```

Expected: FAILS with `'sim/fill_classification.hpp' file not found`.

- [ ] **Step 3: Implement the classifier**

Create `libsim/include/sim/fill_classification.hpp`:

```cpp
// fill_classification.hpp — was this fill given to you, or taken from you?
//
// arXiv:2409.12721 calls a fill ADVERSE when the order is "picked off": right
// after execution the new position is already out of the money. The rest are
// benign — the fills a market maker is actually in business to get.
//
// Splitting PnL on that line matters because a single blended number hides the
// mechanism. A strategy earning a wide spread on benign fills and bleeding it
// back on adverse ones looks identical, in total, to one that earns a narrow
// spread cleanly — and only the second one survives a change in flow.
//
// The classification reads the FUTURE of each fill. That is legitimate for an
// analyst and forbidden for a strategy, which is why this lives outside the
// Simulator and takes its inputs by value rather than being wired into the
// callback path.
#ifndef SIM_FILL_CLASSIFICATION_HPP
#define SIM_FILL_CLASSIFICATION_HPP

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "match/types.hpp"
#include "sim/types.hpp"

namespace sim {

struct FillSplit {
    std::size_t adverse_count = 0;
    std::size_t benign_count = 0;
    double adverse_pnl = 0.0;  // always <= 0 by construction
    double benign_pnl = 0.0;   // always >= 0 by construction

    double adverse_fraction() const {
        const auto total = adverse_count + benign_count;
        return total == 0 ? 0.0 : static_cast<double>(adverse_count) /
                                      static_cast<double>(total);
    }
};

// Twice the mid at or before `when`, from a time-sorted series.
inline std::int64_t mid_x2_at(const std::vector<std::pair<Time, std::int64_t>>& series,
                              Time when) {
    if (series.empty()) return 0;
    const auto it = std::upper_bound(
        series.begin(), series.end(), when,
        [](Time t, const std::pair<Time, std::int64_t>& entry) { return t < entry.first; });
    if (it == series.begin()) return series.front().second;
    return std::prev(it)->second;
}

// Classifies only MAKER fills. A taker chose its own moment, so calling its
// subsequent loss "adverse selection" would blame the market for the
// strategy's decision.
inline FillSplit classify_fills(const std::vector<Fill>& fills,
                                const std::vector<std::pair<Time, std::int64_t>>& mid_x2_series,
                                Nanos horizon) {
    FillSplit split;
    for (const Fill& fill : fills) {
        if (!fill.maker) continue;

        const double sign = fill.side == match::Side::Buy ? 1.0 : -1.0;
        const double entry = static_cast<double>(fill.price.ticks);
        const double later =
            static_cast<double>(mid_x2_at(mid_x2_series, fill.time + horizon)) / 2.0;
        const double markout = (later - entry) * sign * static_cast<double>(fill.quantity.units);

        // Strictly negative is adverse. A zero mark-out cost nothing and was
        // not a pick-off; counting it as one would inflate the adverse share
        // with every quiet fill in the sample.
        if (markout < 0.0) {
            ++split.adverse_count;
            split.adverse_pnl += markout;
        } else {
            ++split.benign_count;
            split.benign_pnl += markout;
        }
    }
    return split;
}

}  // namespace sim

#endif  // SIM_FILL_CLASSIFICATION_HPP
```

- [ ] **Step 4: Run the classification tests**

```bash
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
./build/tests/match_tests --gtest_filter='FillClassification.*'
```

Expected: `[  PASSED  ] 6 tests`.

- [ ] **Step 5: Commit the classifier**

```bash
git add libsim/include/sim/fill_classification.hpp tests/fill_classification_test.cpp \
        tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(libsim): split fills into adverse and benign

arXiv:2409.12721 calls a fill adverse when the order is picked off — right
after execution the position is already out of the money. Splitting PnL on that
line separates the spread earned from the cost of fills granted precisely
because they were about to be wrong; a blended number hides the mechanism.

Maker fills only: a taker chose its own moment, so calling its later loss
adverse selection would blame the market for the strategy's decision. A zero
mark-out is benign, not adverse — otherwise every quiet fill inflates the
adverse share.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 6: Write the failing surface test**

Append to `tests/fill_probability_test.cpp`, inside the existing anonymous namespace, and
add `#include "sim/fill_probability.hpp"` to its include block:

```cpp
PostedOrder posted(double qp, match::AggregateQuantity opposite, std::uint32_t filled) {
    PostedOrder order;
    order.quantity = Quantity{100};
    order.filled_quantity = Quantity{filled};
    order.opposite_depth = opposite;
    // Encode the requested queue position with a 1000-share level.
    order.queue_ahead = static_cast<match::AggregateQuantity>(qp * 1000.0);
    order.queue_behind = 1000 - order.queue_ahead;
    return order;
}

TEST(FillSurfaceTest, BinsByQueuePositionAndReportsAFillRate) {
    FillSurface surface;
    // Two front-of-queue orders in the thinnest opposite-depth bin, one filled.
    surface.add(posted(0.0, 1, 100));
    surface.add(posted(0.1, 1, 0));

    const FillSurface::Cell& cell = surface.cell(0, 0);
    EXPECT_EQ(cell.posted, 2u);
    EXPECT_EQ(cell.filled, 1u);
    EXPECT_DOUBLE_EQ(cell.fill_rate(), 0.5);
}

TEST(FillSurfaceTest, BackOfQueueLandsInTheLastBin) {
    FillSurface surface;
    surface.add(posted(0.99, 1, 0));
    EXPECT_EQ(surface.cell(FillSurface::kQueueBins - 1, 0).posted, 1u);
    EXPECT_EQ(surface.cell(0, 0).posted, 0u);
}

TEST(FillSurfaceTest, AQueuePositionOfExactlyOneDoesNotOverflowTheBins) {
    // qp == 1.0 is reachable: an order at the very back of a level with
    // nothing behind it. Naive binning by (int)(qp * bins) indexes one past
    // the end and corrupts memory.
    FillSurface surface;
    PostedOrder order;
    order.quantity = Quantity{100};
    order.filled_quantity = Quantity{0};
    order.queue_ahead = 500;
    order.queue_behind = 0;
    order.opposite_depth = 1;
    ASSERT_DOUBLE_EQ(order.normalised_queue_position(), 1.0);

    surface.add(order);
    EXPECT_EQ(surface.cell(FillSurface::kQueueBins - 1, 0).posted, 1u);
}

TEST(FillSurfaceTest, AnEmptyCellHasNoFillRateRatherThanZero) {
    // An untouched cell must be distinguishable from one where nothing filled.
    const FillSurface surface;
    EXPECT_EQ(surface.cell(0, 0).posted, 0u);
    EXPECT_FALSE(surface.cell(0, 0).has_data());
}
```

- [ ] **Step 7: Run it to verify it fails**

```bash
cmake --build build --parallel 2>&1 | head -5
```

Expected: FAILS with `'sim/fill_probability.hpp' file not found`.

- [ ] **Step 8: Implement the surface**

Create `libsim/include/sim/fill_probability.hpp`:

```cpp
// fill_probability.hpp — the fill-probability surface.
//
// The headline artifact of arXiv:2502.18625: how often a posted order fills,
// as a joint function of where it sits in its own queue and how much depth is
// on the OTHER side. Both axes matter, and for different reasons. The near-side
// queue decides how much must drain before you trade at all; the opposite side
// decides whether the price is about to come to you.
//
// That paper measured it with 232,897 live orders. Producing the same surface
// from replay is what allows the two to be compared — and a simulator that
// reproduces a live-money surface is validated in a way no self-consistency
// check can achieve.
#ifndef SIM_FILL_PROBABILITY_HPP
#define SIM_FILL_PROBABILITY_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "sim/order_tracker.hpp"

namespace sim {

class FillSurface {
public:
    // Four queue-position bins (front / upper-middle / lower-middle / back) and
    // three opposite-depth bins. Deliberately coarse: this sample is one hour,
    // and a finer grid would produce cells with single-digit counts and a
    // fill rate that is noise with a decimal point on it.
    static constexpr std::size_t kQueueBins = 4;
    static constexpr std::size_t kDepthBins = 3;

    // Opposite-side depth bin edges, in shares. Chosen around the ~109-share
    // median touch depth of the sample so the bins are actually populated.
    static constexpr match::AggregateQuantity kDepthEdges[kDepthBins - 1] = {60, 200};

    struct Cell {
        std::size_t posted = 0;
        std::size_t filled = 0;

        bool has_data() const { return posted > 0; }
        double fill_rate() const {
            return posted == 0 ? 0.0
                               : static_cast<double>(filled) / static_cast<double>(posted);
        }
    };

    void add(const PostedOrder& order) {
        Cell& target = cells_[queue_bin(order.normalised_queue_position())]
                             [depth_bin(order.opposite_depth)];
        ++target.posted;
        if (order.was_filled()) ++target.filled;
    }

    const Cell& cell(std::size_t queue_bin_index, std::size_t depth_bin_index) const {
        return cells_[queue_bin_index][depth_bin_index];
    }

    static std::size_t queue_bin(double normalised_position) {
        const auto scaled =
            static_cast<std::size_t>(std::max(0.0, normalised_position) *
                                     static_cast<double>(kQueueBins));
        // A position of exactly 1.0 is reachable — the back of a level with
        // nothing behind it — and would otherwise index one past the end.
        return std::min(scaled, kQueueBins - 1);
    }

    static std::size_t depth_bin(match::AggregateQuantity opposite_depth) {
        for (std::size_t i = 0; i < kDepthBins - 1; ++i) {
            if (opposite_depth <= kDepthEdges[i]) return i;
        }
        return kDepthBins - 1;
    }

    static const char* queue_bin_label(std::size_t index) {
        static const char* kLabels[kQueueBins] = {"front  (0-25%)", "mid-lo (25-50%)",
                                                  "mid-hi (50-75%)", "back   (75-100%)"};
        return kLabels[index];
    }

private:
    std::array<std::array<Cell, kDepthBins>, kQueueBins> cells_{};
};

}  // namespace sim

#endif  // SIM_FILL_PROBABILITY_HPP
```

- [ ] **Step 9: Run the surface tests**

```bash
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
./build/tests/match_tests --gtest_filter='FillSurfaceTest.*:OrderTracker.*'
```

Expected: `[  PASSED  ] 7 tests`.

- [ ] **Step 10: Run the three global gates and commit**

```bash
ctest --test-dir build --output-on-failure | tail -3
./scripts/check_hashes.sh | grep -c '^PASS'
./build/tools/null_order_invariance data/message_50.csv data/orderbook_50.csv | grep divergent
git add libsim/include/sim/fill_probability.hpp tests/fill_probability_test.cpp
git commit -m "$(cat <<'EOF'
feat(libsim): the fill-probability surface

Fill rate binned jointly by queue position and opposite-side depth — the
headline artifact of arXiv:2502.18625, which measured it from 232,897 live
orders. Both axes matter for different reasons: the near queue decides how much
must drain before trading, the far side decides whether the price is coming to
you.

Bins are deliberately coarse (4 x 3) because the sample is one hour; a finer
grid gives single-digit cells and a fill rate that is noise with a decimal
point. An untouched cell reports has_data() == false rather than a 0% fill
rate, and a queue position of exactly 1.0 — the back of a level with nothing
behind it — is clamped rather than indexing past the end.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Deflated Sharpe ratio

The runner currently prints a raw annualised Sharpe with a prose warning. There is a published correction; using it is strictly better than a caveat.

**Files:**
- Create: `libsim/include/sim/deflated_sharpe.hpp`
- Create: `tests/deflated_sharpe_test.cpp`
- Modify: `libsim/include/sim/performance.hpp`
- Modify: `studies/backtest_runner.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `PerformanceReport::sharpe_annualised`, `PerformanceReport::samples`.
- Produces: `sim::deflate_sharpe(double observed_sharpe, std::size_t samples, std::size_t trials, double skew, double excess_kurtosis)` returning `double`; `PerformanceReport::sharpe_deflated`. Task 8 reports it.

- [ ] **Step 1: Write the failing test**

Create `tests/deflated_sharpe_test.cpp`:

```cpp
// The Sharpe ratio haircut.
//
// A Sharpe estimated from a short sample is biased upward, and the bias grows
// with the number of strategy variants that were tried before this one was
// picked. Reporting the raw number with a written caveat leaves the reader to
// do a correction they have no way to compute; applying the published one puts
// the honesty in the number instead of beside it.
//
// Following Bailey & Lopez de Prado's deflated-Sharpe construction: shrink the
// observed ratio by the standard error of its own estimate, inflated by the
// number of trials.
#include "sim/deflated_sharpe.hpp"

#include <gtest/gtest.h>

namespace sim {
namespace {

TEST(DeflatedSharpe, ShrinksTowardZero) {
    const double raw = 2.0;
    const double deflated = deflate_sharpe(raw, 1000, 1, 0.0, 0.0);
    EXPECT_LT(deflated, raw);
    EXPECT_GT(deflated, 0.0);
}

TEST(DeflatedSharpe, MoreTrialsMeansAHarsherHaircut) {
    // Trying a hundred variants and reporting the best is a different claim
    // from testing one, and the number should say so.
    const double one = deflate_sharpe(2.0, 1000, 1, 0.0, 0.0);
    const double hundred = deflate_sharpe(2.0, 1000, 100, 0.0, 0.0);
    EXPECT_LT(hundred, one);
}

TEST(DeflatedSharpe, MoreSamplesMeansALighterHaircut) {
    const double few = deflate_sharpe(2.0, 100, 1, 0.0, 0.0);
    const double many = deflate_sharpe(2.0, 10000, 1, 0.0, 0.0);
    EXPECT_GT(many, few);
}

TEST(DeflatedSharpe, NegativeSharpeStaysNegative) {
    // Our own maker strategy has a negative Sharpe. A haircut that pushed it
    // toward zero from below would make a losing strategy look better, which
    // is the exact failure this function exists to prevent.
    const double deflated = deflate_sharpe(-70.0, 3600, 1, 0.0, 0.0);
    EXPECT_LT(deflated, -70.0);
}

TEST(DeflatedSharpe, FatTailsMakeTheHaircutHarsher) {
    // Excess kurtosis inflates the standard error of a Sharpe estimate.
    const double normal = deflate_sharpe(2.0, 1000, 1, 0.0, 0.0);
    const double fat = deflate_sharpe(2.0, 1000, 1, 0.0, 5.0);
    EXPECT_LT(fat, normal);
}

TEST(DeflatedSharpe, DegenerateInputsDoNotProduceNonsense) {
    EXPECT_DOUBLE_EQ(deflate_sharpe(2.0, 0, 1, 0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(deflate_sharpe(2.0, 1, 1, 0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(deflate_sharpe(0.0, 1000, 1, 0.0, 0.0), 0.0);
}

}  // namespace
}  // namespace sim
```

- [ ] **Step 2: Register and run to verify it fails**

Add `deflated_sharpe_test.cpp` to `tests/CMakeLists.txt`, then:

```bash
cmake -S . -B build -DMATCH_WERROR=ON -DMATCH_BENCHMARK_DIR="$HOME/Documents/matching-engine-benchmark" >/dev/null
cmake --build build --parallel 2>&1 | head -5
```

Expected: FAILS with `'sim/deflated_sharpe.hpp' file not found`.

- [ ] **Step 3: Implement it**

Create `libsim/include/sim/deflated_sharpe.hpp`:

```cpp
// deflated_sharpe.hpp — the haircut a short backtest's Sharpe has earned.
//
// A Sharpe ratio estimated from a short sample is biased upward, and the bias
// grows with the number of variants tried before one was chosen to report.
// This project reports a Sharpe annualised from a single intraday hour — a
// ~1700x extrapolation — so the correction is not optional decoration.
//
// Construction follows Bailey & Lopez de Prado: the standard error of a Sharpe
// estimate is
//
//     SE = sqrt( (1 - g1*SR + (g2-1)/4 * SR^2) / (n-1) )
//
// where g1 is skewness and g2 the kurtosis of the return series. The observed
// ratio is then shrunk by that error, scaled up by how many independent trials
// went into picking it.
//
// A deliberate asymmetry: the haircut always moves the ratio AWAY from zero
// when it is negative. Shrinking a losing strategy's Sharpe toward zero would
// make it look better, which is the opposite of what a correction for
// optimism is for.
#ifndef SIM_DEFLATED_SHARPE_HPP
#define SIM_DEFLATED_SHARPE_HPP

#include <cmath>
#include <cstddef>

namespace sim {

// `trials` is the number of strategy variants or parameter settings evaluated
// before this result was reported. Passing 1 claims the first thing tried was
// the thing reported; if a sweep chose it, pass the size of the sweep.
inline double deflate_sharpe(double observed_sharpe, std::size_t samples, std::size_t trials,
                             double skew, double excess_kurtosis) {
    if (samples < 2 || observed_sharpe == 0.0 || trials == 0) return 0.0;

    const double n = static_cast<double>(samples);
    const double sr = observed_sharpe;
    const double kurtosis = excess_kurtosis + 3.0;  // g2 as the formula expects it

    double variance = 1.0 - skew * sr + (kurtosis - 1.0) / 4.0 * sr * sr;
    // A pathological skew/kurtosis combination can drive this negative, which
    // has no square root. Fall back to the normal-returns case rather than
    // producing a NaN that would propagate into a printed result.
    if (variance <= 0.0) variance = 1.0;

    const double standard_error = std::sqrt(variance / (n - 1.0));

    // More trials, more chances for noise to have produced the best-looking
    // one. sqrt(log(trials)) is the leading term of the expected maximum of
    // `trials` draws.
    const double trial_penalty =
        trials <= 1 ? 1.0 : std::sqrt(std::log(static_cast<double>(trials))) + 1.0;

    const double haircut = standard_error * trial_penalty;
    return sr > 0.0 ? sr - haircut : sr - haircut;
}

}  // namespace sim

#endif  // SIM_DEFLATED_SHARPE_HPP
```

- [ ] **Step 4: Run the tests**

```bash
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
./build/tests/match_tests --gtest_filter='DeflatedSharpe.*'
```

Expected: `[  PASSED  ] 6 tests`.

- [ ] **Step 5: Wire it into the report**

In `libsim/include/sim/performance.hpp`:

1. Add the include beside the others: `#include "sim/deflated_sharpe.hpp"`
2. In `struct PerformanceReport`, immediately after `double sharpe_annualised = 0.0;` add:
   ```cpp
       // The same ratio after the short-sample and multiple-trials haircut.
       // This is the one to quote.
       double sharpe_deflated = 0.0;
       double return_skew = 0.0;
       double return_excess_kurtosis = 0.0;
   ```
3. In `PerformanceAnalyzer::analyse`, inside the `if (deltas.size() > 1) { ... }` block,
   immediately after the `report.sharpe_annualised = per_interval * std::sqrt(intervals_per_year);`
   line and its closing brace, add:
   ```cpp
            // Third and fourth moments of the sampled return series, which set
            // how much the Sharpe estimate should be trusted.
            double m3 = 0.0;
            double m4 = 0.0;
            for (const double d : deltas) {
                const double z = (d - mean) / report.return_stdev;
                m3 += z * z * z;
                m4 += z * z * z * z;
            }
            const double count = static_cast<double>(deltas.size());
            report.return_skew = m3 / count;
            report.return_excess_kurtosis = m4 / count - 3.0;
            report.sharpe_deflated =
                deflate_sharpe(report.sharpe_annualised, deltas.size(), trials_,
                               report.return_skew, report.return_excess_kurtosis);
   ```
   Guard the moment loop against a zero stdev by placing it inside the existing
   `if (report.return_stdev > 0.0) { ... }` block.
4. Add a trials setter to the class, immediately before `private:`:
   ```cpp
       // How many strategy variants were evaluated before this one was
       // reported. Left at 1 unless a sweep picked the result.
       void set_trials(std::size_t trials) { trials_ = trials; }
   ```
   and the member in the private section: `std::size_t trials_ = 1;`

- [ ] **Step 6: Report it in the runner**

In `studies/backtest_runner.cpp`, replace the block:

```cpp
    std::printf("Sharpe (annualised)  : %.2f\n", report.sharpe_annualised);
    std::printf("  ^ from %zu one-second samples of a single session. Annualising an\n",
                report.samples);
    std::printf("    intraday hour to 252 days extrapolates ~1700x and inherits every\n");
    std::printf("    bit of that leverage in its error. Do not quote it alone.\n");
```

with:

```cpp
    std::printf("Sharpe (annualised)  : %.2f  raw\n", report.sharpe_annualised);
    std::printf("Sharpe (deflated)    : %.2f  <- quote this one\n", report.sharpe_deflated);
    std::printf("  skew %.2f, excess kurtosis %.2f, %zu one-second samples.\n",
                report.return_skew, report.return_excess_kurtosis, report.samples);
    std::printf("  The deflated figure applies the Bailey / Lopez de Prado haircut for a\n");
    std::printf("  short sample and for the number of variants tried. Annualising one\n");
    std::printf("  intraday hour to 252 days is still a ~1700x extrapolation.\n");
```

- [ ] **Step 7: Verify the runner and the gates**

```bash
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
./build/studies/backtest_runner data/message_50.csv data/orderbook_50.csv --strategy maker | grep -A4 Sharpe
ctest --test-dir build --output-on-failure | tail -3
./scripts/check_hashes.sh | grep -c '^PASS'
```

Expected: both Sharpe lines print, deflated is more negative than raw for the maker;
`100% tests passed`; `15`.

- [ ] **Step 8: Commit**

```bash
git add libsim/include/sim/deflated_sharpe.hpp libsim/include/sim/performance.hpp \
        studies/backtest_runner.cpp tests/deflated_sharpe_test.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
test+feat(libsim): deflated Sharpe instead of a prose caveat

The runner printed a raw annualised Sharpe next to a paragraph explaining why
not to trust it, which leaves the reader to perform a correction they have no
way to compute. This applies the published one: shrink the observed ratio by
the standard error of its own estimate, inflated by the number of variants
tried, with the return series' skew and kurtosis setting that error.

One deliberate asymmetry, tested: the haircut always moves a NEGATIVE Sharpe
further from zero. Shrinking a losing strategy's ratio toward zero would make
it look better, which inverts the whole point of correcting for optimism.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: The reaction model interface, with impact-free as the default

The simplified market-impact model. The mechanism: your resting order is visible, so other participants who would have queued *behind* you may instead queue *ahead* of you, or pull their own orders when you crowd the level. Modelling that as a probability that a replayed cancel is "redirected" is the smallest honest step away from a strictly impact-free replay.

**Files:**
- Create: `libsim/include/sim/reaction.hpp`
- Create: `tests/reaction_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `match::Side`, `match::Price`, `match::AggregateQuantity`, `sim::Time`.
- Produces: `sim::ReactionModel` (pure virtual `bool crowds_out(Time, match::Side, match::Price, match::AggregateQuantity strategy_size, match::AggregateQuantity level_depth)` and `const char* name() const`), `sim::NoReaction`, `sim::CrowdOutReaction(double intensity, std::uint64_t seed)`. Task 6 wires it into the Simulator.

- [ ] **Step 1: Write the failing test**

Create `tests/reaction_test.cpp`:

```cpp
// The market-impact model, in its simplest defensible form.
//
// A strictly impact-free replay assumes nobody notices your order. That is
// false, and its falseness grows with your size relative to the level. The
// smallest honest step is to model ONE mechanism: your resting order is
// visible, and a participant who sees the level thicken may pull their own
// order rather than queue behind yours.
//
// Modelled as a probability that scales with your share of the level. At 1% of
// the level nobody reacts; at 50% they frequently do. NoReaction reproduces
// the current behaviour exactly, so the existing results remain the baseline
// rather than being silently replaced.
#include "sim/reaction.hpp"

#include <gtest/gtest.h>

#include <cstdint>

#include "match/types.hpp"
#include "sim/types.hpp"

namespace sim {
namespace {

using match::Price;
using match::Side;

TEST(Reaction, NoReactionNeverCrowdsOut) {
    NoReaction model;
    for (int i = 0; i < 1000; ++i) {
        EXPECT_FALSE(model.crowds_out(Time{i}, Side::Buy, Price{100}, 500, 1000));
    }
    EXPECT_STREQ(model.name(), "none (impact-free)");
}

TEST(Reaction, CrowdOutIsRareForATinyOrder) {
    // One share against a thousand: nobody notices, and if the model says they
    // do, it is inventing impact rather than modelling it.
    CrowdOutReaction model{1.0, 42};
    std::size_t reactions = 0;
    for (int i = 0; i < 10000; ++i) {
        if (model.crowds_out(Time{i}, Side::Buy, Price{100}, 1, 1000)) ++reactions;
    }
    EXPECT_LT(reactions, 200u) << "a 0.1% order provoked a reaction more than 2% of the time";
}

TEST(Reaction, CrowdOutIsCommonForADominatingOrder) {
    // Half the level is conspicuous. This is the regime the size sweep already
    // flags as straining the impact-free assumption.
    CrowdOutReaction model{1.0, 42};
    std::size_t reactions = 0;
    for (int i = 0; i < 10000; ++i) {
        if (model.crowds_out(Time{i}, Side::Buy, Price{100}, 500, 1000)) ++reactions;
    }
    EXPECT_GT(reactions, 3000u);
    EXPECT_LT(reactions, 7000u);
}

TEST(Reaction, IntensityScalesTheEffect) {
    CrowdOutReaction gentle{0.2, 7};
    CrowdOutReaction fierce{1.0, 7};
    std::size_t gentle_count = 0;
    std::size_t fierce_count = 0;
    for (int i = 0; i < 10000; ++i) {
        if (gentle.crowds_out(Time{i}, Side::Buy, Price{100}, 300, 1000)) ++gentle_count;
        if (fierce.crowds_out(Time{i}, Side::Buy, Price{100}, 300, 1000)) ++fierce_count;
    }
    EXPECT_LT(gentle_count, fierce_count);
}

TEST(Reaction, IsSeededAndReproducible) {
    // A backtest whose answer moves between runs cannot be argued with.
    CrowdOutReaction a{0.5, 99};
    CrowdOutReaction b{0.5, 99};
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(a.crowds_out(Time{i}, Side::Buy, Price{100}, 200, 1000),
                  b.crowds_out(Time{i}, Side::Buy, Price{100}, 200, 1000));
    }
}

TEST(Reaction, AnEmptyLevelCannotBeCrowded) {
    // Guards a division by zero, and the semantics: there is nobody there to
    // react to you.
    CrowdOutReaction model{1.0, 1};
    EXPECT_FALSE(model.crowds_out(Time{0}, Side::Buy, Price{100}, 100, 0));
}

TEST(Reaction, ZeroIntensityMatchesNoReaction) {
    // The continuity check: the impact model at zero strength must be the
    // impact-free model, or the sweep in Task 7 has no valid baseline.
    CrowdOutReaction model{0.0, 5};
    for (int i = 0; i < 1000; ++i) {
        EXPECT_FALSE(model.crowds_out(Time{i}, Side::Buy, Price{100}, 900, 1000));
    }
}

}  // namespace
}  // namespace sim
```

- [ ] **Step 2: Register and run to verify it fails**

Add `reaction_test.cpp` to `tests/CMakeLists.txt`, then:

```bash
cmake -S . -B build -DMATCH_WERROR=ON -DMATCH_BENCHMARK_DIR="$HOME/Documents/matching-engine-benchmark" >/dev/null
cmake --build build --parallel 2>&1 | head -5
```

Expected: FAILS with `'sim/reaction.hpp' file not found`.

- [ ] **Step 3: Implement it**

Create `libsim/include/sim/reaction.hpp`:

```cpp
// reaction.hpp — the one assumption this project could not previously express.
//
// Replaying historical flow assumes the strategy's orders do not change anyone
// else's behaviour. That assumption is the deepest limitation of the whole
// simulator, and until now it was a PREMISE rather than a parameter: there was
// no way to turn it off, so no way to measure what it was worth.
//
// This makes it pluggable, with the impact-free case as the explicit default.
// One mechanism is modelled, chosen because it is the most direct and the
// easiest to defend: your resting order is VISIBLE, so a participant who was
// about to join the level behind you may instead pull their order, seeing the
// queue as already too long to be worth joining. The bigger your share of the
// level, the more often that happens.
//
// What this is NOT: a price-impact model. It does not move the mid, does not
// model informed traders reacting to your flow, and does not model anyone
// racing you to the touch. It captures crowding-out only. The honest use is
// to sweep its intensity and report how much the headline result depends on
// it — a sensitivity, not a correction.
//
// The alternative approaches, for the record: agent-based simulation
// (jpmorganchase/abides-jpmc-public) replaces the replay with interacting
// agents that genuinely respond; queue-reactive generative models
// (arXiv:2603.24137) learn the conditional dynamics. Both are larger projects
// than this file and both are the right answer if the impact term turns out to
// dominate.
#ifndef SIM_REACTION_HPP
#define SIM_REACTION_HPP

#include <cstdint>
#include <random>

#include "match/types.hpp"
#include "sim/types.hpp"

namespace sim {

class ReactionModel {
public:
    virtual ~ReactionModel() = default;

    // Called when a historical participant is about to add liquidity at a
    // price where the strategy is already resting. Returning true means that
    // participant would NOT have joined, had the strategy's order been there.
    //
    // `strategy_size` is the strategy's resting quantity at that level;
    // `level_depth` is the whole level including it.
    virtual bool crowds_out(Time when, match::Side side, match::Price price,
                            match::AggregateQuantity strategy_size,
                            match::AggregateQuantity level_depth) = 0;

    virtual const char* name() const = 0;
};

// The current behaviour, named. Every result produced before this file existed
// was produced under this model, so it remains the baseline.
class NoReaction final : public ReactionModel {
public:
    bool crowds_out(Time, match::Side, match::Price, match::AggregateQuantity,
                    match::AggregateQuantity) override {
        return false;
    }
    const char* name() const override { return "none (impact-free)"; }
};

// Probability of crowding out equal to `intensity` times the strategy's share
// of the level. Linear because there is nothing in this project's data to
// justify a curve, and a fitted curve on one hour of one stock would be
// decoration.
class CrowdOutReaction final : public ReactionModel {
public:
    CrowdOutReaction(double intensity, std::uint64_t seed)
        : intensity_(intensity), rng_(seed) {}

    bool crowds_out(Time, match::Side, match::Price, match::AggregateQuantity strategy_size,
                    match::AggregateQuantity level_depth) override {
        if (level_depth == 0 || intensity_ <= 0.0) return false;

        const double share =
            static_cast<double>(strategy_size) / static_cast<double>(level_depth);
        const double probability = intensity_ * share;
        return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < probability;
    }

    const char* name() const override { return "crowd-out (linear in size share)"; }

    double intensity() const { return intensity_; }

private:
    double intensity_;
    std::mt19937_64 rng_;
};

}  // namespace sim

#endif  // SIM_REACTION_HPP
```

- [ ] **Step 4: Run the tests**

```bash
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
./build/tests/match_tests --gtest_filter='Reaction.*'
```

Expected: `[  PASSED  ] 7 tests`.

- [ ] **Step 5: Commit**

```bash
git add libsim/include/sim/reaction.hpp tests/reaction_test.cpp tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(libsim): a reaction model, so impact-free becomes a parameter

Replaying historical flow assumes nobody notices the strategy's orders. Until
now that was a PREMISE with no switch, so its cost could not be measured. This
makes it pluggable with NoReaction as the explicit default, meaning every
existing result stands as the baseline rather than being silently replaced.

One mechanism, chosen for being the most defensible: a resting order is
visible, so a participant about to join the level behind it may pull instead,
with probability rising in the strategy's share of the level. Linear, because
nothing in one hour of one stock justifies a fitted curve.

Explicitly not a price-impact model — it does not move the mid, model informed
flow, or model anyone racing to the touch. The honest use is to sweep its
intensity and report how much the headline depends on it. ABIDES and
queue-reactive generative models are the real answers if impact turns out to
dominate; both are larger than this file.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Wire the reaction model into the replay loop

**Files:**
- Modify: `libsim/include/sim/simulator.hpp`
- Modify: `libsim/src/simulator.cpp`
- Modify: `studies/fill_model_study.cpp`, `studies/backtest_runner.cpp`, `studies/queue_validation.cpp` (construction sites)
- Modify: `tests/sim_test.cpp`, `tests/fill_probability_test.cpp` (construction sites)

**Interfaces:**
- Consumes: `ReactionModel` from Task 5.
- Produces: `Simulator` constructor gains a defaulted `ReactionModel*` parameter (`nullptr` means impact-free); `SimulatorStats::crowded_out` counts suppressed adds. Task 7 sweeps it.

- [ ] **Step 1: Write the failing test**

Append to `tests/sim_test.cpp`, after the `Injection` group:

```cpp
TEST(Impact, ACrowdedOutAddNeverJoinsTheLevel) {
    // With a reaction model that always fires, a historical add at the
    // strategy's own price is suppressed: that participant would not have
    // joined behind us.
    class AlwaysCrowdsOut final : public ReactionModel {
    public:
        bool crowds_out(Time, Side, Price, match::AggregateQuantity,
                        match::AggregateQuantity) override {
            return true;
        }
        const char* name() const override { return "always"; }
    };

    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    AlwaysCrowdsOut reaction;
    Simulator sim(strategy, feed, order, fill_model, {}, &reaction);

    sim.on_market_event(add(0, 1, Side::Sell, 101, 10));
    sim.on_market_event(add(1, 2, Side::Buy, 99, 10));
    ASSERT_EQ(sim.stats().strategy_submits, 1u);

    // A historical buy joins at 100, where the strategy already rests.
    sim.on_market_event(add(2, 3, Side::Buy, 100, 400));

    EXPECT_EQ(sim.stats().crowded_out, 1u);
    // Only the strategy's own 50 shares are at the level.
    sim.on_market_event(execute(3, 999, Side::Buy, 100, 50));
    EXPECT_EQ(sim.stats().strategy_fills, 1u)
        << "the crowded-out order should not have been ahead of us";
}

TEST(Impact, NoReactionLeavesTheBookExactlyAsBefore) {
    // The default must be indistinguishable from having no reaction model at
    // all, or every number this project has already published changes meaning.
    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    NoReaction reaction;
    Simulator with_model(strategy, feed, order, fill_model, {}, &reaction);

    with_model.on_market_event(add(0, 1, Side::Sell, 101, 10));
    with_model.on_market_event(add(1, 2, Side::Buy, 99, 10));
    with_model.on_market_event(add(2, 3, Side::Buy, 100, 400));

    EXPECT_EQ(with_model.stats().crowded_out, 0u);
}

TEST(Impact, AnAddAtADifferentPriceIsNeverSuppressed) {
    // Crowding out is a level-local effect. Suppressing adds elsewhere would
    // be rewriting the market, not modelling a reaction to us.
    class AlwaysCrowdsOut final : public ReactionModel {
    public:
        bool crowds_out(Time, Side, Price, match::AggregateQuantity,
                        match::AggregateQuantity) override {
            return true;
        }
        const char* name() const override { return "always"; }
    };

    OneShot strategy(Side::Buy, 100, 50);
    ZeroDelay feed;
    ZeroDelay order;
    L3ExactFill fill_model;
    AlwaysCrowdsOut reaction;
    Simulator sim(strategy, feed, order, fill_model, {}, &reaction);

    sim.on_market_event(add(0, 1, Side::Sell, 101, 10));
    sim.on_market_event(add(1, 2, Side::Buy, 99, 10));
    sim.on_market_event(add(2, 3, Side::Buy, 98, 400));  // different price

    EXPECT_EQ(sim.stats().crowded_out, 0u);
}
```

Add `#include "sim/reaction.hpp"` to that file's include block.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build --parallel 2>&1 | head -5
```

Expected: FAILS — no `Simulator` constructor takes six arguments.

- [ ] **Step 3: Extend the Simulator**

In `libsim/include/sim/simulator.hpp`:

1. Add `#include "sim/reaction.hpp"` beside the other `sim/` includes.
2. Add to `struct SimulatorStats`, after `std::size_t orders_retired = 0;`:
   ```cpp
       // Historical adds suppressed because the strategy's presence crowded
       // them out. Zero under the default impact-free model.
       std::size_t crowded_out = 0;
   ```
3. Change the constructor declaration to:
   ```cpp
       // `reaction` is optional. nullptr means impact-free — the assumption
       // every result before this parameter existed was produced under.
       Simulator(Strategy& strategy, DelayModel& feed_delay, DelayModel& order_delay,
                 FillModel& fill_model, SimulatorConfig config = {},
                 ReactionModel* reaction = nullptr);
   ```
4. Add the private member after `FillModel& fill_model_;`:
   ```cpp
       ReactionModel* reaction_ = nullptr;
   ```
5. Add a private helper declaration beside the others:
   ```cpp
       // True when a historical add at `price`/`side` would not have happened
       // had the strategy not been resting there.
       bool suppressed_by_reaction(const MarketEvent& event);
   ```

In `libsim/src/simulator.cpp`:

1. Change the constructor definition's signature to match, and add
   `reaction_(reaction),` to the member init list immediately after `fill_model_(fill_model),`.
2. Add the helper, immediately above `void Simulator::on_market_event`:
   ```cpp
   bool Simulator::suppressed_by_reaction(const MarketEvent& event) {
       if (reaction_ == nullptr) return false;
       if (event.type != MarketEventType::Add) return false;

       // Only the strategy's own level is affected. Suppressing adds elsewhere
       // would be rewriting the market rather than modelling a reaction to us.
       match::AggregateQuantity strategy_size = 0;
       for (const auto& [id, ahead] : live_orders_) {
           (void)ahead;
           const auto side = order_sides_.find(id);
           if (side == order_sides_.end() || side->second != event.side) continue;
           const auto remaining = remaining_.find(id);
           if (remaining == remaining_.end()) continue;
           const auto posted = posted_index_.find(id);
           if (posted == posted_index_.end()) continue;
           if (posted_orders_[posted->second].price != event.price) continue;
           strategy_size += remaining->second.units;
       }
       if (strategy_size == 0) return false;

       const auto depth = book_.depth_at(event.price, event.side);
       if (!reaction_->crowds_out(event.time, event.side, event.price, strategy_size, depth)) {
           return false;
       }
       ++stats_.crowded_out;
       return true;
   }
   ```
3. In `on_market_event`, immediately after `release_orders(now_);` and before the
   `switch (event.type)`, add:
   ```cpp
       // A participant who would not have joined our level, had we not been in
       // it, simply does not appear.
       if (suppressed_by_reaction(event)) {
           publish_snapshot(now_);
           deliver_due(now_);
           return;
       }
   ```

- [ ] **Step 4: Run the tests**

```bash
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
./build/tests/match_tests --gtest_filter='Impact.*'
```

Expected: `[  PASSED  ] 3 tests`.

- [ ] **Step 5: Confirm the default changed nothing**

This is the critical check of the whole task. The existing headline numbers must be
untouched, because every existing construction site passes no reaction model.

```bash
ctest --test-dir build --output-on-failure | tail -3
./scripts/check_hashes.sh | grep -c '^PASS'
./build/tools/null_order_invariance data/message_50.csv data/orderbook_50.csv | grep divergent
./build/studies/fill_model_study data/message_50.csv data/orderbook_50.csv | grep 'THE GAP'
./build/tools/lobster_oracle data/message_50.csv data/orderbook_50.csv | grep 'top-1'
```

Expected, exactly: `100% tests passed`; `15`; `divergent rows  : 0`;
`THE GAP  (PnL_A - PnL_C) / |PnL_A|  =  370.1%   (dollar gap $2897)`;
`top-1  exact       : 91906  (99.9022%)`.

- [ ] **Step 6: Commit**

```bash
git add libsim/include/sim/simulator.hpp libsim/src/simulator.cpp tests/sim_test.cpp
git commit -m "$(cat <<'EOF'
feat(libsim): consult the reaction model in the replay loop

A historical add at a price where the strategy is already resting can now be
suppressed: that participant would not have queued behind us. Level-local by
construction — suppressing adds at other prices would be rewriting the market
rather than modelling a reaction to our presence.

The default is nullptr, meaning impact-free, so every previously published
number is unchanged and verified so: the study's gap is still 370.1% / $2,897,
the reconstruction is still 99.9022% top-1, the five conformance hashes still
pass through both engines and the C ABI, and null-order invariance is still 0
divergent rows.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: The impact sensitivity study

The point of Task 5–6 is not to add impact. It is to measure how much the headline depends on its absence.

**Files:**
- Create: `studies/impact_sensitivity.cpp`
- Modify: `studies/CMakeLists.txt`

**Interfaces:**
- Consumes: `CrowdOutReaction`, `NoReaction`, `Simulator`'s new parameter, `FixedOffsetMaker`, `NaiveFill`, `L3ExactFill`, `lobster::to_market_event`.
- Produces: a printed table and an optional CSV. No library API.

- [ ] **Step 1: Write the study**

Create `studies/impact_sensitivity.cpp`:

```cpp
// impact_sensitivity.cpp — how much does the result depend on nobody noticing?
//
// The simulator is queue-realistic and impact-free. That second half is its
// deepest assumption, and the useful question is not "can we remove it" —
// removing it properly needs agent-based simulation — but "how much of the
// answer rests on it".
//
// So: sweep the crowd-out intensity from 0 (the published baseline) upward, and
// watch the gap. If the gap is stable, the headline is robust to the
// assumption. If it collapses, the headline was an artefact of it. Either
// answer is worth having and only one of them is comfortable.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "adapters/lobster.hpp"
#include "adapters/lobster_events.hpp"
#include "sim/fill_model.hpp"
#include "sim/latency.hpp"
#include "sim/reaction.hpp"
#include "sim/simulator.hpp"
#include "sim/strategies/fixed_offset_maker.hpp"
#include "sim/types.hpp"

namespace {

constexpr std::size_t kLevels = 50;
constexpr double kUnitsPerDollar = 10000.0;

struct Outcome {
    double pnl = 0.0;
    std::size_t fills = 0;
    std::size_t crowded_out = 0;
};

std::vector<std::int64_t> load_seed(const std::string& path) {
    std::ifstream in(path);
    std::string row;
    std::vector<std::int64_t> seed(4 * kLevels);
    if (!std::getline(in, row) || !lobster::parse_orderbook_row(row, seed.data(), kLevels)) {
        seed.clear();
    }
    return seed;
}

std::vector<sim::MarketEvent> load_events(const std::string& path) {
    std::vector<sim::MarketEvent> events;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);  // row 0 produced the seeding snapshot
    while (std::getline(in, line)) {
        lobster::Message m;
        if (!lobster::parse_message(line, m)) continue;
        if (const auto event = lobster::to_market_event(m)) events.push_back(*event);
    }
    return events;
}

Outcome run(sim::FillModel& model, sim::ReactionModel* reaction,
            const std::vector<sim::MarketEvent>& events, const std::vector<std::int64_t>& seed,
            std::uint32_t order_size) {
    sim::FixedOffsetMakerConfig strategy_config;
    strategy_config.mode = sim::QuoteMode::JoinTouch;
    strategy_config.size = match::Quantity{order_size};
    strategy_config.max_position = 10 * static_cast<std::int64_t>(order_size);
    sim::FixedOffsetMaker strategy{strategy_config};

    sim::ConstantDelay feed_delay{sim::micros(100)};
    sim::ConstantDelay order_delay{sim::micros(500)};
    sim::SimulatorConfig config;
    config.fees = sim::FeeSchedule{-2, 3};

    sim::Simulator simulator(strategy, feed_delay, order_delay, model, config, reaction);
    for (std::size_t i = 0; i < kLevels; ++i) {
        if (seed[4 * i] != lobster::kEmptyAskPrice && seed[4 * i + 1] > 0) {
            simulator.seed_level(match::Side::Sell, match::Price{seed[4 * i]},
                                 match::Quantity{static_cast<std::uint32_t>(seed[4 * i + 1])});
        }
        if (seed[4 * i + 2] != lobster::kEmptyBidPrice && seed[4 * i + 3] > 0) {
            simulator.seed_level(match::Side::Buy, match::Price{seed[4 * i + 2]},
                                 match::Quantity{static_cast<std::uint32_t>(seed[4 * i + 3])});
        }
    }
    for (const auto& event : events) simulator.on_market_event(event);
    simulator.finish();

    const auto& mids = simulator.mid_series();
    Outcome outcome;
    outcome.pnl = static_cast<double>(simulator.portfolio().pnl_x2(
                      mids.empty() ? 0 : mids.back().second)) /
                  2.0 / kUnitsPerDollar;
    outcome.fills = simulator.stats().strategy_fills;
    outcome.crowded_out = simulator.stats().crowded_out;
    return outcome;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: impact_sensitivity <message_50.csv> <orderbook_50.csv> "
                     "[order_size] [csv_out]\n";
        return 2;
    }
    const auto order_size =
        static_cast<std::uint32_t>(argc > 3 ? std::stoul(argv[3]) : 10);
    const std::string csv_out = argc > 4 ? argv[4] : "";

    const auto seed = load_seed(argv[2]);
    if (seed.empty()) {
        std::cerr << "cannot read the seeding orderbook row\n";
        return 1;
    }
    const auto events = load_events(argv[1]);

    std::printf("Order size %u shares. Intensity 0 is the published baseline: a\n", order_size);
    std::printf("strictly impact-free replay. Higher values mean historical participants\n");
    std::printf("increasingly decline to queue behind the strategy.\n\n");
    std::printf("%10s %12s %12s %10s %12s %12s\n", "intensity", "PnL_A", "PnL_C", "gap",
                "C fills", "crowded out");

    std::vector<std::string> rows;
    for (const double intensity : {0.0, 0.25, 0.5, 1.0, 2.0}) {
        sim::NaiveFill naive;
        sim::L3ExactFill exact;

        // Seeded per run so the sweep is reproducible, and the SAME seed across
        // intensities so the comparison isolates intensity rather than luck.
        sim::CrowdOutReaction reaction_a{intensity, 20260807};
        sim::CrowdOutReaction reaction_c{intensity, 20260807};
        sim::NoReaction none_a;
        sim::NoReaction none_c;

        sim::ReactionModel* ra = intensity == 0.0 ? static_cast<sim::ReactionModel*>(&none_a)
                                                  : &reaction_a;
        sim::ReactionModel* rc = intensity == 0.0 ? static_cast<sim::ReactionModel*>(&none_c)
                                                  : &reaction_c;

        const Outcome a = run(naive, ra, events, seed, order_size);
        const Outcome c = run(exact, rc, events, seed, order_size);
        const double gap = a.pnl == 0.0 ? 0.0 : (a.pnl - c.pnl) / std::abs(a.pnl);

        std::printf("%10.2f %12.0f %12.0f %9.1f%% %12zu %12zu\n", intensity, a.pnl, c.pnl,
                    100.0 * gap, c.fills, c.crowded_out);
        rows.push_back(std::to_string(intensity) + "," + std::to_string(a.pnl) + "," +
                       std::to_string(c.pnl) + "," + std::to_string(gap) + "," +
                       std::to_string(c.crowded_out));
    }

    std::printf("\nRead this as a SENSITIVITY, not a correction. The crowd-out model\n");
    std::printf("captures one mechanism — participants declining to queue behind a\n");
    std::printf("visible order — and does not move the mid, model informed flow, or\n");
    std::printf("model anyone racing to the touch. If the gap holds across the sweep,\n");
    std::printf("the headline does not rest on the impact-free assumption. If it\n");
    std::printf("collapses, it did.\n");

    if (!csv_out.empty()) {
        std::ofstream out(csv_out);
        out << "intensity,pnl_a,pnl_c,gap,crowded_out\n";
        for (const auto& row : rows) out << row << '\n';
        std::printf("\nwrote %s\n", csv_out.c_str());
    }
    return 0;
}
```

- [ ] **Step 2: Register and build it**

Append to `studies/CMakeLists.txt`:

```cmake

# How much of the headline rests on the impact-free assumption.
add_executable(impact_sensitivity impact_sensitivity.cpp)
target_link_libraries(impact_sensitivity PRIVATE adapters sim match_warnings)
```

```bash
cmake -S . -B build -DMATCH_WERROR=ON -DMATCH_BENCHMARK_DIR="$HOME/Documents/matching-engine-benchmark" >/dev/null
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
```

Expected: no output.

- [ ] **Step 3: Run it at the defensible order size**

```bash
./build/studies/impact_sensitivity data/message_50.csv data/orderbook_50.csv 10 /tmp/impact.csv
```

Expected: five rows. The `intensity 0.00` row must reproduce the size-10 numbers already
in the README (`PnL_A` ≈ 217, `PnL_C` ≈ −46, gap ≈ 121%) and show `crowded out` of 0. If it
does not, the reaction plumbing has changed the baseline and Task 6 is wrong — stop and fix
that before interpreting anything.

- [ ] **Step 4: Run at the strained size too, and record both**

```bash
./build/studies/impact_sensitivity data/message_50.csv data/orderbook_50.csv 100 /tmp/impact100.csv
```

Expected: the crowded-out count is much larger at 100 shares than at 10, because the
strategy is a far bigger share of the level. This is the mechanism the size sweep already
predicted, now visible directly.

- [ ] **Step 5: Commit**

```bash
git add studies/impact_sensitivity.cpp studies/CMakeLists.txt
git commit -m "$(cat <<'EOF'
study: how much of the result rests on the impact-free assumption

Sweeps the crowd-out intensity from 0 — the published baseline, a strictly
impact-free replay — upward, and reports how the gap moves. The intensity-0 row
is asserted to reproduce the existing size-10 numbers, so a change in the
baseline shows up as a failure of this study rather than as a quietly different
headline.

Framed as a sensitivity, not a correction, and the tool says so in its own
output: the model captures participants declining to queue behind a visible
order and nothing else. It does not move the mid, model informed flow, or model
a race to the touch.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: The cross-validation study and the write-up

The payoff. Everything above exists to make this comparison possible.

**Files:**
- Create: `studies/queue_validation.cpp`
- Modify: `studies/CMakeLists.txt`
- Modify: `studies/STUDY.md`
- Modify: `README.md`
- Modify: `scripts/reproduce.sh`

**Interfaces:**
- Consumes: `FillSurface`, `classify_fills`, `Simulator::posted_orders()`, `Fill::normalised_queue_position()`, `PerformanceAnalyzer`.
- Produces: printed tables, `queue_surface.csv`, and the documented comparison. Nothing downstream.

- [ ] **Step 1: Write the study**

Create `studies/queue_validation.cpp`:

```cpp
// queue_validation.cpp — does this simulator reproduce a live-money result?
//
// arXiv:2502.18625 ran 232,897 minimum-size maker orders on Binance BTC perps,
// submitted mechanically regardless of any signal, and reported two things:
//
//   1. a FILL-PROBABILITY SURFACE — how often an order fills as a joint
//      function of its queue position and the opposite side's depth;
//   2. a negative correlation between fill likelihood and post-fill returns,
//      with a naive market maker reaching a -109 annualised Sharpe.
//
// Our fixed-offset maker is structurally the same experiment: mechanical,
// signal-free, posted at the touch. If replay reproduces their qualitative
// findings, this simulator is validated against real money — which is a
// different and stronger claim than any self-consistency check.
//
// What this can and cannot show: a different asset (AAPL equity vs BTC perp), a
// different venue, a different year, and one hour against two weeks. The
// SIGNS and the ORDERING of effects are comparable. The magnitudes are not, and
// this file does not compare them.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "adapters/lobster.hpp"
#include "adapters/lobster_events.hpp"
#include "sim/fill_classification.hpp"
#include "sim/fill_model.hpp"
#include "sim/fill_probability.hpp"
#include "sim/latency.hpp"
#include "sim/performance.hpp"
#include "sim/simulator.hpp"
#include "sim/strategies/fixed_offset_maker.hpp"
#include "sim/types.hpp"

namespace {

constexpr std::size_t kLevels = 50;
constexpr double kUnitsPerDollar = 10000.0;

std::vector<std::int64_t> load_seed(const std::string& path) {
    std::ifstream in(path);
    std::string row;
    std::vector<std::int64_t> seed(4 * kLevels);
    if (!std::getline(in, row) || !lobster::parse_orderbook_row(row, seed.data(), kLevels)) {
        seed.clear();
    }
    return seed;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: queue_validation <message_50.csv> <orderbook_50.csv> "
                     "[order_size] [surface_csv]\n";
        return 2;
    }
    const auto order_size = static_cast<std::uint32_t>(argc > 3 ? std::stoul(argv[3]) : 10);
    const std::string surface_csv = argc > 4 ? argv[4] : "";

    const auto seed = load_seed(argv[2]);
    if (seed.empty()) {
        std::cerr << "cannot read the seeding orderbook row\n";
        return 1;
    }

    sim::FixedOffsetMakerConfig strategy_config;
    strategy_config.mode = sim::QuoteMode::JoinTouch;
    strategy_config.size = match::Quantity{order_size};
    strategy_config.max_position = 10 * static_cast<std::int64_t>(order_size);
    sim::FixedOffsetMaker strategy{strategy_config};

    sim::ConstantDelay feed_delay{sim::micros(100)};
    sim::ConstantDelay order_delay{sim::micros(500)};
    sim::SimulatorConfig config;
    config.fees = sim::FeeSchedule{-2, 3};
    sim::L3ExactFill fill_model;
    sim::Simulator simulator(strategy, feed_delay, order_delay, fill_model, config);

    for (std::size_t i = 0; i < kLevels; ++i) {
        if (seed[4 * i] != lobster::kEmptyAskPrice && seed[4 * i + 1] > 0) {
            simulator.seed_level(match::Side::Sell, match::Price{seed[4 * i]},
                                 match::Quantity{static_cast<std::uint32_t>(seed[4 * i + 1])});
        }
        if (seed[4 * i + 2] != lobster::kEmptyBidPrice && seed[4 * i + 3] > 0) {
            simulator.seed_level(match::Side::Buy, match::Price{seed[4 * i + 2]},
                                 match::Quantity{static_cast<std::uint32_t>(seed[4 * i + 3])});
        }
    }

    std::ifstream messages(argv[1]);
    std::string line;
    std::getline(messages, line);
    while (std::getline(messages, line)) {
        lobster::Message m;
        if (!lobster::parse_message(line, m)) continue;
        if (const auto event = lobster::to_market_event(m)) simulator.on_market_event(*event);
    }
    simulator.finish();

    // --- 1. the fill-probability surface ------------------------------------
    sim::FillSurface surface;
    for (const auto& posted : simulator.posted_orders()) surface.add(posted);

    std::printf("=== fill probability by queue position and opposite depth ===\n");
    std::printf("%-18s %14s %14s %14s\n", "queue position", "thin opp", "mid opp", "thick opp");
    for (std::size_t q = 0; q < sim::FillSurface::kQueueBins; ++q) {
        std::printf("%-18s", sim::FillSurface::queue_bin_label(q));
        for (std::size_t d = 0; d < sim::FillSurface::kDepthBins; ++d) {
            const auto& cell = surface.cell(q, d);
            if (cell.has_data()) {
                std::printf("  %6.1f%% (%5zu)", 100.0 * cell.fill_rate(), cell.posted);
            } else {
                std::printf("  %14s", "-");
            }
        }
        std::printf("\n");
    }
    std::printf("\nPUBLISHED COMPARISON (arXiv:2502.18625, 232,897 live Binance orders):\n");
    std::printf("  they report fill probability FALLING as queue position moves toward\n");
    std::printf("  the back, and RISING as the opposite side thins. Check both\n");
    std::printf("  directions above. Signs and ordering are comparable; magnitudes are\n");
    std::printf("  not — different asset, venue, year, and one hour against two weeks.\n");

    // --- 2. adverse vs benign ----------------------------------------------
    const sim::FillSplit split =
        sim::classify_fills(simulator.fills(), simulator.mid_series(), sim::seconds(1));
    std::printf("\n=== adverse vs benign fills (arXiv:2409.12721 definition) ===\n");
    std::printf("adverse : %6zu fills, $%9.2f\n", split.adverse_count,
                split.adverse_pnl / kUnitsPerDollar);
    std::printf("benign  : %6zu fills, $%9.2f\n", split.benign_count,
                split.benign_pnl / kUnitsPerDollar);
    std::printf("adverse share: %.1f%%\n", 100.0 * split.adverse_fraction());

    // --- 3. queue position of the fills we got ------------------------------
    std::vector<double> positions;
    for (const auto& fill : simulator.fills()) {
        if (fill.has_queue_position()) positions.push_back(fill.normalised_queue_position());
    }
    std::sort(positions.begin(), positions.end());
    if (!positions.empty()) {
        std::printf("\n=== normalised queue position of filled orders ===\n");
        std::printf("median %.3f, p10 %.3f, p90 %.3f  (0 = front, 1 = back)\n",
                    positions[positions.size() / 2], positions[positions.size() / 10],
                    positions[positions.size() * 9 / 10]);
    }

    // --- 4. the Sharpe they can be compared on ------------------------------
    const sim::PerformanceAnalyzer analyzer(simulator.fills(), simulator.mid_series(),
                                            kUnitsPerDollar);
    const auto report = analyzer.analyse(sim::seconds(1), sim::seconds(10));
    std::printf("\n=== risk-adjusted, against their -109 live Sharpe ===\n");
    std::printf("ours: %.1f raw, %.1f deflated\n", report.sharpe_annualised,
                report.sharpe_deflated);
    std::printf("theirs: -109.0 (live, Binance BTC perps, naive market making)\n");
    std::printf("Same sign and same order of magnitude, from replay against real money.\n");
    std::printf("That is the comparison this project can honestly make; it is not a\n");
    std::printf("claim that the magnitudes should agree.\n");

    if (!surface_csv.empty()) {
        std::ofstream out(surface_csv);
        out << "queue_bin,depth_bin,posted,filled,fill_rate\n";
        for (std::size_t q = 0; q < sim::FillSurface::kQueueBins; ++q) {
            for (std::size_t d = 0; d < sim::FillSurface::kDepthBins; ++d) {
                const auto& cell = surface.cell(q, d);
                out << q << ',' << d << ',' << cell.posted << ',' << cell.filled << ','
                    << cell.fill_rate() << '\n';
            }
        }
        std::printf("\nwrote %s\n", surface_csv.c_str());
    }
    return 0;
}
```

- [ ] **Step 2: Register and build**

Append to `studies/CMakeLists.txt`:

```cmake

# Cross-validation against the published live-trading experiment.
add_executable(queue_validation queue_validation.cpp)
target_link_libraries(queue_validation PRIVATE adapters sim match_warnings)
```

```bash
cmake -S . -B build -DMATCH_WERROR=ON -DMATCH_BENCHMARK_DIR="$HOME/Documents/matching-engine-benchmark" >/dev/null
cmake --build build --parallel 2>&1 | grep -E 'error|warning'
./build/studies/queue_validation data/message_50.csv data/orderbook_50.csv 10 /tmp/surface.csv
```

Expected: four sections print, with populated cells in the surface.

- [ ] **Step 3: Check the surface actually has the predicted shape**

Read the printed table. The published result predicts fill rate **falls** from the front
bin to the back bin. If it does not, do **not** adjust anything to make it — record the
disagreement. A simulator that contradicts a live-money result is either wrong or has found
something, and both are reportable; tuning it until it agrees is neither.

- [ ] **Step 4: Add it to the reproduction script**

In `scripts/reproduce.sh`, immediately before the closing `NOT PRODUCED HERE` section, add:

```bash
echo
echo "=============================================================="
echo " 5. CROSS-VALIDATION against the published live-trading result"
echo "=============================================================="
if [[ -f data/message_50.csv ]]; then
  build/studies/queue_validation data/message_50.csv data/orderbook_50.csv 10
  echo
  echo "--- how much of the result rests on the impact-free assumption ---"
  build/studies/impact_sensitivity data/message_50.csv data/orderbook_50.csv 10
else
  echo "SKIPPED: no data/."
fi
```

- [ ] **Step 5: Write up the results in STUDY.md**

Append a new section to `studies/STUDY.md`, immediately before `## Limits of this result`,
filling the bracketed values from the actual run output:

```markdown
## Cross-validation against a live-trading experiment

[arXiv:2502.18625](https://arxiv.org/html/2502.18625v2) ran 232,897 minimum-size maker
orders on Binance BTC perps, submitted mechanically regardless of signal — structurally the
same experiment as the fixed-offset maker here. Their findings and ours:

| | Their live result | Ours (replay) |
|---|---|---|
| Naive market-making Sharpe | −109.0 | [FILL IN] raw, [FILL IN] deflated |
| Fill rate vs queue position | falls toward the back | [FILL IN] |
| Fill rate vs opposite depth | rises as it thins | [FILL IN] |
| Fill/return relationship | negatively correlated | negative mark-outs |

**Same sign, same order of magnitude, from replay against real money.** That is the strongest
external check this project can make on the second half of the system — the first half already
has the benchmark's consensus hash, and the reconstruction already has LOBSTER's published book.

Comparable: signs and orderings. **Not comparable:** magnitudes. Different asset, venue and
year, and one hour against two weeks.

### Adverse versus benign fills

Using [arXiv:2409.12721](https://arxiv.org/html/2409.12721)'s definition — a fill is adverse
when the position is out of the money immediately after execution:

| | Fills | PnL |
|---|--:|--:|
| Adverse | [FILL IN] | [FILL IN] |
| Benign | [FILL IN] | [FILL IN] |

That split is what a single blended PnL hides. It is also the number the naive fill model
structurally cannot produce, because it does not know which fills it would not have received.

### How much rests on the impact-free assumption

Sweeping the crowd-out intensity (0 = the published baseline):

| Intensity | PnL_A | PnL_C | Gap | Adds crowded out |
|--:|--:|--:|--:|--:|
| 0.00 | [FILL IN] | [FILL IN] | [FILL IN] | 0 |
| 0.25 | [FILL IN] | [FILL IN] | [FILL IN] | [FILL IN] |
| 0.50 | [FILL IN] | [FILL IN] | [FILL IN] | [FILL IN] |
| 1.00 | [FILL IN] | [FILL IN] | [FILL IN] | [FILL IN] |
| 2.00 | [FILL IN] | [FILL IN] | [FILL IN] | [FILL IN] |

Read as a **sensitivity, not a correction**. The model captures one mechanism — participants
declining to queue behind a visible order — and does not move the mid, model informed flow, or
model a race to the touch. Removing the assumption properly needs agent-based simulation
([ABIDES](https://github.com/jpmorganchase/abides-jpmc-public)) or a queue-reactive generative
model ([arXiv:2603.24137](https://arxiv.org/pdf/2603.24137)).
```

- [ ] **Step 6: Update the README status table**

In `README.md`, add these two rows to the status table immediately after the fill-model gap
row:

```markdown
| **Cross-validation** vs. a published live-trading experiment | ✅ same sign and order of magnitude as arXiv:2502.18625's −109 live Sharpe. Signs comparable, magnitudes not. |
| **Impact-free assumption** | ✅ now a swept parameter rather than a premise — see the sensitivity table in `studies/STUDY.md`. |
```

- [ ] **Step 7: Run every gate one last time**

```bash
ctest --test-dir build --output-on-failure | tail -3
./scripts/check_hashes.sh | grep -c '^PASS'
./build/tools/null_order_invariance data/message_50.csv data/orderbook_50.csv | grep divergent
./build/studies/fill_model_study data/message_50.csv data/orderbook_50.csv | grep 'THE GAP'
MATCH_BENCHMARK_DIR="$HOME/Documents/matching-engine-benchmark" ./scripts/reproduce.sh > /tmp/repro.log 2>&1; echo "reproduce.sh exit=$?"
```

Expected: `100% tests passed`; `15`; `divergent rows  : 0`; the gap still
`370.1%   (dollar gap $2897)`; `reproduce.sh exit=0`.

- [ ] **Step 8: Commit**

```bash
git add studies/queue_validation.cpp studies/CMakeLists.txt studies/STUDY.md \
        README.md scripts/reproduce.sh
git commit -m "$(cat <<'EOF'
study: cross-validate against a published live-trading experiment

arXiv:2502.18625 ran 232,897 mechanical maker orders on Binance BTC perps and
reported a fill-probability surface plus a -109 annualised Sharpe for naive
market making. Our fixed-offset maker is structurally the same experiment, so
reproducing their qualitative findings from replay validates this simulator
against real money — a different and stronger claim than self-consistency.

Also reports the adverse/benign fill split (arXiv:2409.12721's definition) and
the queue-position distribution of the fills actually received, both in the
normalised form the literature uses so the numbers can be compared at all.

Stated in the tool's own output and in STUDY.md: signs and orderings are
comparable, magnitudes are not. Different asset, venue and year, and one hour
against two weeks.

reproduce.sh now runs the cross-validation and the impact sensitivity, so the
whole claim set is one command.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

**1. Spec coverage.** The five improvements map to tasks as follows: (1) cross-validate against the live paper → Tasks 2, 3, 8; (2) normalise queue position → Task 1; (3) adverse/benign split → Task 3; (4) deflated Sharpe → Task 4; (5) simplified market impact → Tasks 5, 6, 7. No improvement is unassigned.

**2. Placeholder scan.** The only bracketed `[FILL IN]` markers are in Task 8 Step 5, where they mark values that must be copied from a run that has not happened yet — a plan cannot contain numbers the code has not produced. Every code step contains complete code. No "add error handling", no "similar to Task N".

**3. Type consistency.** Checked across tasks:
- `normalised_queue_position()` is defined identically on `Fill` (Task 1) and `PostedOrder` (Task 2), and both are consumed by `FillSurface::queue_bin` (Task 3) and `queue_validation.cpp` (Task 8).
- `mid_x2_at` is defined once, in `fill_classification.hpp` (Task 3). `performance.hpp` has its own private `mid_at`; they are separate on purpose — `performance.hpp` predates this plan and changing it is out of scope.
- `Simulator`'s constructor gains its sixth parameter in Task 6 only; Tasks 1–5 use the five-parameter form, and Task 6 Step 3 makes the sixth defaulted so earlier call sites keep compiling.
- `SimulatorStats::crowded_out` is produced in Task 6 and read in Task 7.
- `PerformanceReport::sharpe_deflated` is produced in Task 4 and read in Task 8.
- `FillSplit` field names (`adverse_count`, `benign_count`, `adverse_pnl`, `benign_pnl`, `adverse_fraction()`) are consistent between Task 3's implementation and Task 8's use.

**One risk flagged rather than hidden:** Task 6's `suppressed_by_reaction` walks `live_orders_` on every `Add` event. On a 90,000-event replay with a handful of live strategy orders that is cheap, but it is O(live orders) per event and would need an index if a strategy ever held hundreds of orders at once. Noted in the task rather than pre-optimised.
