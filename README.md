# Matching engine & L3 execution simulator

A single-threaded C++20 limit order book and matching engine, and an L3 execution simulator
built on top of it that replays real NASDAQ order-by-order data and injects orders into the
same timestamped stream.

**The deliverable is not the code — it is a measured number:** how much a naive "filled at the
touch" fill assumption inflates a passive market-making strategy's PnL, versus real L3 queue
position, once order latency and adverse selection are accounted for.

## Status

Phase 0 of 8. Nothing below is claimed yet — this section fills in from measured runs only.

| Claim | Status |
|---|---|
| Benchmark verdict (`flash1-dev/matching-engine-benchmark`, arXiv:2606.01183) | not yet run |
| Worst-case throughput, P50 / P99 | not yet measured |
| LOBSTER reconstruction bound | probed in Python, not yet in C++ |
| The fill-model gap | not yet measured |

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

The engine and its tests build and run on macOS. **The benchmark harness itself is Linux-only**
(it pins matcher and drainer threads to specific cores), so the verdict, the state audit, and
all throughput numbers come from a Linux box, and this README will say which.

## Probes

| Probe | What it settled |
|---|---|
| `probes/lobster_oracle_check.py` | LOBSTER's message + orderbook file pairing works as a reconstruction oracle — but only at level 50 (99.90% top-of-book across the hour; level 10 is worthless at 0.79%, because the message file only carries events inside the requested price range). |
| `probes/workload_parity_check.py` | The benchmark's workload generator produces a **bit-identical** workload on macOS/Apple libm as on the glibc/aarch64 machine the published reference came from — verified across all 1,996,097 messages and 5.93M field values. So the correctness hash is reachable off-Linux, and only the timed and audited runs need the Linux box. |
