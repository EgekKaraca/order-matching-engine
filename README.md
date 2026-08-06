# Matching engine & L3 execution simulator

A single-threaded C++20 limit order book and matching engine, and an L3 execution simulator
built on top of it that replays real NASDAQ order-by-order data and injects orders into the
same timestamped stream.

**The deliverable is not the code — it is a measured number:** how much a naive "filled at the
touch" fill assumption inflates a passive market-making strategy's PnL, versus real L3 queue
position, once order latency and adverse selection are accounted for.

## Status

Phase 1 of 8. This section fills in from measured runs only.

| Claim | Status |
|---|---|
| **Report-stream correctness** vs. the published consensus hash | ✅ **All five scenarios at the canonical seed 23**, byte-identical, through both the C++ engine and the dlopened C ABI adapter. Reproduce with `scripts/check_hashes.sh`. |
| Benchmark **verdict** (`flash1-dev/matching-engine-benchmark`, arXiv:2606.01183) | ⏳ not yet run — needs Linux |
| 34-case conformance gate | ⏳ not yet run — needs Linux |
| Worst-case throughput, P50 / P99 | ⏳ not measured |
| **LOBSTER reconstruction** vs. the venue's published book | ✅ **top-of-book exact for 54,605 consecutive events; 99.90% of the session.** See the bound below. |
| **The fill-model gap** | ⚠️ **first measurement taken — see below.** Directionally clear, but single-sample and with one model unvalidated. |

### What "report-stream correctness" does and does not mean

The engine reproduces `reference/correctness_hash.txt` for `static`, `normal`, `swing-25`,
`swing-40`, and `flash-crash` at seed 23 — the SHA-256 of the full ~2.2M-report canonical
stream, which the benchmark established as the byte-identical consensus of independent
conforming engines. That is the benchmark's `Status: PASS`.

It is **not** `Verdict: VALID`, and this README will not say so until the harness prints it.
The verdict additionally requires the random-point book-state audit, the anti-cheat checks,
and a timed run — all Linux-only. Only seed 23 is covered so far; the published hashes cover
no other seed.

### The result so far

Identical strategy (passive maker joining the touch, 100 shares), identical data, identical
latency, three fill models. AAPL 2012-06-21, 09:30–10:30, order latency 500 µs, feed latency
100 µs:

| Fill model | Fills | Spread capture | Mark-out @10s | Net PnL | Queue ahead (median) |
|---|--:|--:|--:|--:|--:|
| **A — naive** ("filled at the touch") | 1,118 | $467 | −$3,171 | **+$783** | 100 |
| B — L2 probabilistic (estimated queue) | 159 | $434 | +$404 | +$568 | 100 |
| **C — L3 exact** (real queue) | 908 | $1,075 | −$3,886 | **−$2,114** | 90 |

**The strategy is profitable under the naive fill model and loss-making under the real one.**
That is the single most useful sentence this project can produce, and it is a negative result
about the strategy rather than a positive one — which is the point.

`PnL_A > PnL_C` at every latency tested (0 to 100 ms).

#### But that table is at an order size the simulator cannot fully defend

Median displayed size at the touch on this data is **109 shares**. The 100-share order above is
therefore **92% of a typical level** — squarely in the regime where "my orders do not change
anyone else's behaviour" is doing more work than it can bear. Quoting the 370% without that
context would be overclaiming.

Sweeping order size, with the position limit scaled alongside it so the sweep measures size
rather than the limit:

| Order size | % of touch depth | PnL_A | PnL_C | Gap | Impact-free assumption |
|--:|--:|--:|--:|--:|---|
| 1 | 0.9% | $26 | $4 | 85% | defensible |
| **10** | **9.2%** | **+$217** | **−$46** | **121%** | **defensible** |
| 50 | 45.9% | $873 | −$936 | 207% | strained |
| 100 | 91.7% | $783 | −$2,114 | 370% | strained |
| 300 | 275% | −$2,233 | −$6,318 | 183% | strained |

**The sign flip survives at a defensible size.** At 10 shares — 9.2% of displayed depth, small
enough that the no-impact assumption is reasonable — the naive model still reports a profit
(+$217) and the L3 model still reports a loss (−$46). That, not the 370%, is the result this
work actually earns.

The gap also grows monotonically with size across the defensible range (85% → 121% → 207% →
370%), which is the mechanically expected direction: a larger order waits longer in the queue,
so the naive model's error compounds.

#### Caveats, none of them optional

1. **The ratio metric is fragile.** `(PnL_A − PnL_C) / |PnL_A|` blows up whenever `PnL_A` is
   near zero — at 100 µs latency both models lose money and the ratio stops meaning anything.
   Report the dollar gap alongside it, always.
2. **Model B is not validated.** It fills a fifth as often as C and shows *positive* mark-outs,
   which is not what a passive maker should show. Either mis-implemented or mis-parameterised;
   it should not be quoted until that is resolved. A-vs-C is the whole argument.
3. **One hour, one stock, one day.** The numbers are real. Their error bars are not estimated,
   and a single point estimate is not a finding.

An experiment-design trap worth recording, because the first run fell into it: quoting a fixed
offset from the mid **measured nothing at all** (gap ≈ 0). AAPL in 2012 routinely ran a $0.61
spread, so a $0.30 offset lands *inside* the spread where no queue exists — and with no queue,
every fill model agrees by construction. The strategy has to **join the touch** for queue
position to be a thing that can be modelled. A study that skipped this check would have
reported "no gap" and been wrong.

Reproduce with `build/studies/fill_model_study data/message_50.csv data/orderbook_50.csv`.

### The reconstruction claim, and its bound

Replaying LOBSTER's AAPL 2012-06-21 level-50 message file through this engine and diffing the
reconstructed book against LOBSTER's own published orderbook file, **every row**, 91,996 events:

| Depth compared | Exact match |
|---|--:|
| Top of book | **99.90%** (91,906 / 91,996) |
| Top 5 levels | 98.84% |
| Top 10 levels | 97.49% |
| **All 50 levels** | **2.84%** |

Longest unbroken run of exact top-of-book agreement: **54,605 consecutive events**, roughly 36
minutes of trading. First divergence at row 54,606.

**So the claim is a top-of-book oracle, not a full-depth one**, and the 2.84% row is left in
this table deliberately: it is the number that shows why. LOBSTER's message file carries only
events inside the requested price range, so liquidity surfacing from outside the window is
referenced by executions and cancels whose arrival was never captured — 91 such events here.
That is documented LOBSTER behaviour rather than a replay defect, and it is why the level-50
sample is used: the level-10 file diverges by row 439 and matches on 0.79% of rows.

Reproduce with `build/tools/lobster_oracle data/message_50.csv data/orderbook_50.csv`.

Cross-check: an independent Python implementation (`probes/lobster_oracle_check.py`, a
level-aggregate dict book rather than a per-order L3 book through the engine) reproduces the
same top-1/5/10 figures.

It classifies 85 events as unknown-id where the C++ counts 91, and the six-event difference is
now **fully accounted for**: exactly 6 of the C++'s 91 name an order it had previously tracked.
When an unknown-id event drains a level, the C++ takes from the front of the queue and so
removes real tracked orders; a later event naming one of them then finds it gone. The Python
fallback decrements a level aggregate and never removes a tracked order, so it does not. That
is a difference in fallback policy for liquidity the data cannot attribute — the message file
does not say whose order left — and not a defect in either. Neither can be right, because the
information is absent from the source.

## The caveat, stated up front

Replaying historical order flow assumes **my orders do not change anyone else's behaviour**.
The counterparties traded against would have acted differently had I really been there. So this
is a **queue-realistic, impact-free** simulator: it is valid for small, passive size, and it
breaks completely for large or aggressive size.

Where that starts to bite is measured, not asserted — the study reports how the result varies
with order size as a fraction of level depth. The paradigm that removes the assumption entirely
is agent-based simulation ([ABIDES](https://github.com/jpmorganchase/abides-jpmc-public)).

## Layout

```
libmatch/     the matching engine. Knows nothing about backtesting.
              Emits the benchmark C ABI's report vocabulary natively.
libsim/       replay, injection, latency, accounting. Talks to libmatch
              through that same C ABI.
adapters/     LOBSTER / ITCH / Databento -> NormalizedEvent, and the
              benchmark harness adapter.
studies/      the fill-model comparison; produces the plots and the number.
probes/       throwaway scripts that de-risk an assumption before it is
              built on. Kept, not deleted — two of them have already
              overturned a design assumption.
tests/        unit + property/fuzz.
```

**Why the C ABI is the seam** and not an internal C++ interface: it is the same boundary the
benchmark harness uses, so the engine that was benchmarked is byte-for-byte the engine that is
backtested. There is no "research build" that diverges from the "fast build" — which is the
quiet way this kind of project becomes dishonest.

## Building

```bash
cmake -S . -B build && cmake --build build --parallel && ctest --test-dir build
```

Requires CMake 3.20+ and a C++20 compiler. GoogleTest is fetched at configure time.

To also build the harness adapter and the ABI replay tool, point at a checkout of the
benchmark repo:

```bash
cmake -S . -B build -DMATCH_BENCHMARK_DIR=/path/to/matching-engine-benchmark
```

Then check the engine against the published hashes — no harness required:

```bash
scripts/check_hashes.sh
```

The engine, its tests, and the hash check all run on macOS. **The harness itself is Linux-only**
(it pins matcher and drainer threads to specific cores), so the verdict, the state audit, the
34-case conformance gate, and every throughput number come from a Linux box, and this README
will say which.

## Probes

| Probe | What it settled |
|---|---|
| `probes/lobster_oracle_check.py` | LOBSTER's message + orderbook file pairing works as a reconstruction oracle — but only at level 50 (99.90% top-of-book across the hour; level 10 is worthless at 0.79%, because the message file only carries events inside the requested price range). |
| `probes/workload_parity_check.py` | The benchmark's workload generator produces a **bit-identical** workload on macOS/Apple libm as on the glibc/aarch64 machine the published reference came from — verified across all 1,996,097 messages and 5.93M field values. So the correctness hash is reachable off-Linux, and only the timed and audited runs need the Linux box. |
