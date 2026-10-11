#!/usr/bin/env bash
# Step 5 gate on the release binary (refuses to run on a dirty tree so the commit in each summary.json identifies the code):
#  1. regression: 100,000 programs, seed 1, rule off: events.bin must equal the Step 4 gate (SHA-256 below) for 1 and 8 workers;
#  2. a run that stops by the rule (exit 0) and one whose budget ends first (exit 2), each for 1 and 8 workers;
#  3. a 200,000-program run interrupted by a real SIGINT, and another killed with SIGKILL, each resumed and compared by hash
#     with an uninterrupted run.
# usage: scripts/step5_gate.sh [out-dir]
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/results/step5-$(date -u +%Y%m%d-%H%M%S)}"
cutoff="$root/build/release/cli/cutoff"
STEP4_SHA=812caad7a0edeea676aebb6675e9e8f4db64ff271370a3daf3f1d10fdeeab598
EPS=0.001   # a demonstration value, NOT a recommended default (defaults come from the Step 11 benchmark)

mkdir -p "$out"
if [ -n "$(git -C "$root" status --porcelain)" ] && [ "${ALLOW_DIRTY:-0}" != "1" ]; then
  echo "step5_gate: uncommitted changes; commit first (or ALLOW_DIRTY=1 for a throwaway run)." >&2; exit 3
fi
# Rebuild so the binary (and the commit it records in every summary.json) matches the tree that was just checked.
cmake --preset release > /dev/null && cmake --build --preset release > /dev/null || { echo "step5_gate: release build failed" >&2; exit 3; }
[ -x "$cutoff" ] || { echo "step5_gate: no release binary" >&2; exit 3; }
head_commit=$(git -C "$root" rev-parse --short=12 HEAD)
built_commit=$("$cutoff" run --max-programs 2 --workers 1 --results-dir "$out/probe" --run-id probe --no-progress > /dev/null 2>&1; grep -o '"git_commit": "[0-9a-f]*"' "$out/probe/summary.json" | cut -d'"' -f4; rm -rf "$out/probe")
if [ "$built_commit" != "$head_commit" ]; then echo "step5_gate: binary was built from $built_commit but HEAD is $head_commit" >&2; exit 3; fi
fail=0
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
check() { # description, condition result
  if [ "$2" = "ok" ]; then echo "PASS  $1"; else echo "FAIL  $1"; fail=1; fi
}
run() { "$cutoff" run "$@" --results-dir "$out" --no-progress; }

echo "== 1. regression against the Step 4 gate (100,000 programs, seed 1, stop rule off)"
for w in 1 8; do
  run --max-programs 100000 --seed 1 --workers "$w" --run-id "regress-${w}w" > "$out/regress-${w}w.txt"; code=$?
  check "regress ${w} worker(s): exit code 0 (got $code)" "$([ $code -eq 0 ] && echo ok)"
  check "regress ${w} worker(s): events.bin == Step 4 gate $(sha "$out/regress-${w}w/events.bin" | cut -c1-12)" "$([ "$(sha "$out/regress-${w}w/events.bin")" = "$STEP4_SHA" ] && echo ok)"
done
check "regress: risk_curve.csv identical for 1 and 8 workers" "$(cmp -s "$out/regress-1w/risk_curve.csv" "$out/regress-8w/risk_curve.csv" && echo ok)"

echo "== 2. stop rule (epsilon $EPS, confirm 3, min 1000, checkpoint 1000)"
for w in 1 8; do
  run --max-programs 400000 --seed 1 --workers "$w" --epsilon $EPS --confirm 3 --min-programs 1000 --checkpoint 1000 --run-id "stop-${w}w" > "$out/stop-${w}w.txt"; code=$?
  check "stop ${w} worker(s): exit code 0 (got $code)" "$([ $code -eq 0 ] && echo ok)"
  n=$(( ( $(stat -f %z "$out/stop-${w}w/events.bin") - 40 ) / 72 ))
  echo "      stopped after $n programs"
  check "stop ${w} worker(s): stopped before the budget, on a checkpoint" "$([ $n -lt 400000 ] && [ $((n % 1000)) -eq 0 ] && echo ok)"
done
check "stop: events.bin identical for 1 and 8 workers" "$(cmp -s "$out/stop-1w/events.bin" "$out/stop-8w/events.bin" && echo ok)"
check "stop: risk_curve.csv identical for 1 and 8 workers" "$(cmp -s "$out/stop-1w/risk_curve.csv" "$out/stop-8w/risk_curve.csv" && echo ok)"
run --max-programs 30000 --seed 1 --workers 8 --epsilon 0.00001 --confirm 3 --min-programs 1000 --checkpoint 1000 --run-id budget > "$out/budget.txt"; code=$?
check "budget ends first: exit code 2 (got $code)" "$([ $code -eq 2 ] && echo ok)"

echo "== 3. interruption and resume (200,000 programs, seed 3, stop rule off)"
run --max-programs 200000 --seed 3 --workers 8 --run-id whole > "$out/whole.txt"
for how in INT KILL; do
  "$cutoff" run --max-programs 200000 --seed 3 --workers 8 --results-dir "$out" --no-progress --run-id "cut-$how" > "$out/cut-$how.txt" 2>&1 &
  pid=$!
  for _ in $(seq 1 100); do [ -s "$out/cut-$how/checkpoint.bin" ] && break; sleep 0.1; done
  sleep 3
  kill -$how $pid; wait $pid; code=$?
  if [ $how = INT ]; then check "SIGINT: exit code 130 (got $code)" "$([ $code -eq 130 ] && echo ok)"; else echo "      SIGKILL: exit status $code"; fi
  sleep 1
  stray=$(pgrep -f "$cutoff run" | grep -v "^$$\$" | wc -l | tr -d ' ')
  check "$how: no worker process left behind (found $stray)" "$([ "$stray" = 0 ] && echo ok)"
  done_at=$(( ( $(stat -f %z "$out/cut-$how/events.bin") - 40 ) / 72 ))
  echo "      events.bin held $done_at records when it was cut (the last checkpoint may be earlier)"
  "$cutoff" run --resume "$out/cut-$how" --no-progress > "$out/resume-$how.txt" 2>&1; code=$?
  check "$how: resume exit code 0 (got $code)" "$([ $code -eq 0 ] && echo ok)"
  check "$how: resumed events.bin == uninterrupted ($(sha "$out/cut-$how/events.bin" | cut -c1-12))" "$([ "$(sha "$out/cut-$how/events.bin")" = "$(sha "$out/whole/events.bin")" ] && echo ok)"
  check "$how: resumed risk_curve.csv == uninterrupted" "$(cmp -s "$out/cut-$how/risk_curve.csv" "$out/whole/risk_curve.csv" && echo ok)"
done

echo "results in $out; failures: $fail"
exit $fail
