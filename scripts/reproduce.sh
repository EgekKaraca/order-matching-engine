#!/usr/bin/env bash
# reproduce.sh — every claim this project makes, from one command.
#
# The definition of done is that the README's three claims are reproducible
# together. This is that command. It runs everything that does NOT need Linux;
# the harness verdict is the one claim it cannot produce, and it says so.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=============================================================="
echo " 1. BUILD AND UNIT TESTS"
echo "=============================================================="
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      ${MATCH_BENCHMARK_DIR:+-DMATCH_BENCHMARK_DIR="$MATCH_BENCHMARK_DIR"} >/dev/null
cmake --build build --parallel >/dev/null
ctest --test-dir build --output-on-failure | tail -3

echo
echo "=============================================================="
echo " 2. ENGINE CORRECTNESS — the published consensus hash"
echo "=============================================================="
if [[ -n "${MATCH_BENCHMARK_DIR:-}" ]]; then
  scripts/check_hashes.sh "$MATCH_BENCHMARK_DIR"
else
  echo "SKIPPED: set MATCH_BENCHMARK_DIR to a checkout of"
  echo "  https://github.com/flash1-dev/matching-engine-benchmark"
fi

echo
echo "=============================================================="
echo " 3. RECONSTRUCTION ORACLE — replay vs the venue's published book"
echo "=============================================================="
if [[ -f data/message_50.csv ]]; then
  build/tools/lobster_oracle data/message_50.csv data/orderbook_50.csv
  echo
  echo "--- null-order invariance (the gate for injection) ---"
  build/tools/null_order_invariance data/message_50.csv data/orderbook_50.csv | tail -8
else
  echo "SKIPPED: no data/. Fetch the level-50 AAPL pair from"
  echo "  https://huggingface.co/datasets/totalorganfailure/lobster-data"
fi

echo
echo "=============================================================="
echo " 4. THE STUDY — three fill models, the gap, the sensitivities"
echo "=============================================================="
if [[ -f data/message_50.csv ]]; then
  build/studies/fill_model_study data/message_50.csv data/orderbook_50.csv
else
  echo "SKIPPED: no data/."
fi

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

echo
echo "=============================================================="
echo " NOT PRODUCED HERE: Verdict: VALID"
echo "=============================================================="
echo "The harness pins threads to cores and runs on Linux only. Section 2"
echo "above is Status: PASS — the report-stream half — and is NOT a verdict."
echo "For the verdict, run scripts/arch_session.sh on the Linux box."
