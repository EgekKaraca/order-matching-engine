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
| LOBSTER reconstruction bound | ⏳ probed in Python, not yet in C++ |
| The fill-model gap | ⏳ not measured |

### What "report-stream correctness" does and does not mean

The engine reproduces `reference/correctness_hash.txt` for `static`, `normal`, `swing-25`,
`swing-40`, and `flash-crash` at seed 23 — the SHA-256 of the full ~2.2M-report canonical
stream, which the benchmark established as the byte-identical consensus of independent
conforming engines. That is the benchmark's `Status: PASS`.

It is **not** `Verdict: VALID`, and this README will not say so until the harness prints it.
The verdict additionally requires the random-point book-state audit, the anti-cheat checks,
and a timed run — all Linux-only. Only seed 23 is covered so far; the published hashes cover
no other seed.

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
