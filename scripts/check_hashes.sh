#!/usr/bin/env bash
# check_hashes.sh — the report-stream half of the benchmark verdict, locally.
#
# Generates each scenario's canonical workload with the benchmark's own
# generator, replays it through this engine, and compares the SHA-256 of the
# canonical report text against the published hash.
#
# This is `Status: PASS` — it is NOT `Verdict: VALID`. The verdict also requires
# the harness's random-point book-state audit, its anti-cheat checks, and a
# timed run, none of which run outside Linux. Do not quote a verdict from this
# script.
#
# Usage:  scripts/check_hashes.sh [benchmark_repo] [work_dir]
set -euo pipefail

BENCH="${1:-${BENCHMARK_DIR:-$HOME/Documents/matching-engine-benchmark}}"
WORK="${2:-${TMPDIR:-/tmp}/ome-hashcheck}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

REPLAY="$ROOT/build/tools/replay"
REFERENCE="$BENCH/reference/correctness_hash.txt"
SEED=23
COUNT=1000000
SCENARIOS=(static normal swing-25 swing-40 flash-crash)

[[ -x "$REPLAY" ]] || { echo "build the replay tool first: cmake --build build" >&2; exit 2; }
[[ -f "$REFERENCE" ]] || { echo "no reference hashes at $REFERENCE" >&2; exit 2; }

mkdir -p "$WORK"

# The generator is a standalone target with no Linux-only dependencies, so it
# builds anywhere the engine does. probes/workload_parity_check.py verified that
# the workload it produces here is bit-identical to the one the published
# hashes were computed against.
GEN="$WORK/generator"
if [[ ! -x "$GEN" || "$BENCH/workload/generator.cpp" -nt "$GEN" ]]; then
  echo "building the benchmark's workload generator..."
  ${CXX:-c++} -std=c++20 -O2 -I "$BENCH/api" "$BENCH/workload/generator.cpp" -o "$GEN"
fi

sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

failures=0
for scenario in "${SCENARIOS[@]}"; do
  workload="$WORK/$scenario.bin"
  output="$WORK/$scenario.txt"

  [[ -f "$workload" ]] || "$GEN" "$scenario" "$workload" "$COUNT" "$SEED" >/dev/null
  "$REPLAY" "$workload" "$output" >"$WORK/$scenario.log"

  want=$(grep "${scenario}_seed${SEED}_count${COUNT}_reports.txt" "$REFERENCE" | cut -d' ' -f1)
  got=$(sha256 "$output")

  if [[ "$want" == "$got" ]]; then
    printf 'PASS  %-12s %s\n' "$scenario" "$got"
  else
    printf 'FAIL  %-12s\n  expected %s\n  actual   %s\n' "$scenario" "$want" "$got"
    failures=$((failures + 1))
  fi
done

echo
if (( failures == 0 )); then
  echo "All ${#SCENARIOS[@]} scenarios reproduce the published byte-identical consensus at seed $SEED."
  echo "This is the report-stream half only. The verdict needs the Linux harness."
else
  echo "$failures scenario(s) diverged."
  echo "Localise with the benchmark's own tooling:"
  echo "  gunzip -k $BENCH/reference/canonical_output.txt.gz"
  echo "  diff <(head -n 200 $BENCH/reference/canonical_output.txt) <(head -n 200 $WORK/normal.txt)"
  exit 1
fi
