#!/usr/bin/env bash
# Step 4 gate: >= 100,000 random programs on the unmutated core with zero mismatches under C1, and
# byte-identical results for 1 and N workers. Refuses to run on a dirty tree so the commit recorded in
# summary.json identifies the code that produced the numbers.
# usage: scripts/baseline_gate.sh [programs] [seed] [out-dir]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
programs="${1:-100000}"
seed="${2:-1}"
out="${3:-$root/results/baseline-$(date -u +%Y%m%d-%H%M%S)}"
cutoff="$root/build/release/cli/cutoff"

if [ -n "$(git -C "$root" status --porcelain)" ] && [ "${ALLOW_DIRTY:-0}" != "1" ]; then
  echo "baseline_gate: the working tree has uncommitted changes; commit first (or set ALLOW_DIRTY=1 for a throwaway run)." >&2
  exit 3
fi
[ -x "$cutoff" ] || { echo "baseline_gate: build the release preset first: cmake --preset release && cmake --build --preset release" >&2; exit 3; }

mkdir -p "$out"
status=0
for w in 1 8; do
  echo "== $programs programs, seed $seed, $w worker(s)"
  "$cutoff" run --programs "$programs" --seed "$seed" --workers "$w" --results-dir "$out" --run-id "gate-${w}w" --no-progress || status=$?
  echo "   exit code $status"
done

a="$out/gate-1w/events.bin"; b="$out/gate-8w/events.bin"
sha_a=$(shasum -a 256 "$a" | cut -d' ' -f1); sha_b=$(shasum -a 256 "$b" | cut -d' ' -f1)
{
  echo "events.bin sha256, 1 worker : $sha_a"
  echo "events.bin sha256, 8 workers: $sha_b"
  if cmp -s "$a" "$b"; then echo "IDENTICAL"; else echo "DIFFERENT"; fi
} | tee "$out/determinism.txt"
cmp -s "$a" "$b" || status=1
echo "results in $out; gate exit status $status"
exit $status
