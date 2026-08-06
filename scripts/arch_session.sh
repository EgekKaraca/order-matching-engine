#!/usr/bin/env bash
# arch_session.sh — the Linux session, in order, so nothing is rediscovered.
#
# Everything in this project that cannot run on macOS is here. The ORDER
# matters and is not arbitrary: the harness is validated against a known-good
# baseline BEFORE this engine is put under it, so that a failure can be
# attributed. Skipping that step is how an afternoon gets spent debugging an
# engine that was fine.
#
# Prerequisites: GCC 14+ or Clang 16+, CMake 3.16+, Boost headers, Python 3.
set -euo pipefail

BENCH="${BENCHMARK_DIR:-$HOME/matching-engine-benchmark}"
ENGINE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "### 0. Build this engine and its tests"
cmake -S "$ENGINE" -B "$ENGINE/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DMATCH_BENCHMARK_DIR="$BENCH"
cmake --build "$ENGINE/build" --parallel
ctest --test-dir "$ENGINE/build" --output-on-failure

echo
echo "### 1. Build the harness"
cd "$BENCH" && make

echo
echo "### 2. Prove the HARNESS is healthy, using a known-good baseline."
echo "###    Do this before blaming your own engine for anything."
scripts/build_baselines.sh liquibook
./harness --baseline liquibook --scenario normal --mode perf
./harness --baseline liquibook --scenario normal --mode audit
echo ">>> Expected: Status: PASS, State audit: PASS, Verdict: VALID."
echo ">>> If not, the environment is wrong. Stop and fix that first."

echo
echo "### 3. The 34-case conformance gate on this engine"
python3 scripts/conformance_check.py "$ENGINE/build/adapters/ome_adapter.so"

echo
echo "### 4. The verdict — the one claim macOS cannot produce"
./harness --engine "$ENGINE/build/adapters/ome_adapter.so" --scenario normal --mode perf
./harness --engine "$ENGINE/build/adapters/ome_adapter.so" --scenario normal --mode audit

echo
echo "### 5. All five scenarios, plus the baseline comparison"
scripts/run_challenge.py --engine "$ENGINE/build/adapters/ome_adapter.so" --all-scenarios
scripts/build_baselines.sh all
scripts/run_challenge.py --compare ome_adapter liquibook quantcup exchange_core

echo
echo "### If a hash diverges, localise it rather than guessing:"
echo "  ./harness --engine <so> --scenario normal --mode perf \\"
echo "      --write-canonical-output /tmp/candidate.txt"
echo "  scripts/explain_divergence.py reference/canonical_output.txt.gz \\"
echo "      /tmp/candidate.txt --json-output /tmp/divergence.json"
echo
echo "### Note: this box is x86, the published figures are Graviton4 (ARM"
echo "### Neoverse-V2). State the hardware next to any throughput number"
echo "### rather than implying comparability."
