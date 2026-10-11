#!/usr/bin/env bash
# Step 7 gate on the release binary (refuses to run on a dirty tree; rebuilds and verifies the binary matches HEAD):
#  1. the golden side of a drill (the Verilated infect model) through the real pipeline: `cutoff drill --mutants 1`,
#     100,000 programs, seed 1 -> events.bin byte-identical to the original core's Step 4 gate (SHA-256 below), 8 and 1 workers;
#  2. brute-force soundness of the activation filter at scale: every mutant simulated on every program (--no-filter),
#     0 programs where a mutant changed the run without having been infected; and the filtered drill on the same programs
#     gives the same outcome for every mutant;
#  3. the obvious planted bug (ALU add -> subtract) is killed by program <= 3;
#  4. a drill with the stop rule observed: where it fired, how many of the planted bugs were found by then, how many by the end;
#  5. interrupt + resume of a drill at moderate scale: drill.tsv and events.bin identical to the uninterrupted run.
# events.bin and drill.tsv are parsed independently of the C++ code and cross-checked against summary.json.
# usage: scripts/step7_gate.sh [out-dir]       env: PROGRAMS_BF (default 5000), PROGRAMS_STOP (default 20000), EPS (default 0.05)
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/results/step7-$(date -u +%Y%m%d-%H%M%S)}"
cutoff="$root/build/release/cli/cutoff"
STEP4_SHA=812caad7a0edeea676aebb6675e9e8f4db64ff271370a3daf3f1d10fdeeab598
PROGRAMS_BF="${PROGRAMS_BF:-5000}"
PROGRAMS_STOP="${PROGRAMS_STOP:-20000}"
EPS="${EPS:-0.05}"

mkdir -p "$out"
if [ -n "$(git -C "$root" status --porcelain)" ] && [ "${ALLOW_DIRTY:-0}" != "1" ]; then
  echo "step7_gate: uncommitted changes; commit first (or ALLOW_DIRTY=1 for a throwaway run)." >&2; exit 3
fi
cmake --preset release > /dev/null && cmake --build --preset release > /dev/null || { echo "step7_gate: release build failed" >&2; exit 3; }
head_commit=$(git -C "$root" rev-parse --short=12 HEAD)
built_commit=$("$cutoff" run --max-programs 2 --workers 1 --results-dir "$out/probe" --run-id probe --no-progress > /dev/null 2>&1; grep -o '"git_commit": "[0-9a-f]*"' "$out/probe/probe/summary.json" | cut -d'"' -f4; rm -rf "$out/probe")
if [ "$built_commit" != "$head_commit" ]; then echo "step7_gate: binary was built from $built_commit but HEAD is $head_commit" >&2; exit 3; fi

fail=0
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
check() { if [ "$2" = "ok" ]; then echo "PASS  $1"; else echo "FAIL  $1"; fail=1; fi; }
now() { python3 -I -c 'import time; print(time.time())'; }
elapsed() { python3 -I -c "print('%.1f' % ($(now) - $1))"; }
json_num() { grep -o "\"$2\": [0-9.e+-]*" "$1/summary.json" | head -1 | grep -o '[0-9.e+-]*$'; }

# events.bin of a run folder, parsed with the real record layout (struct EventRecord in engine/protocol.hpp: 7 x u64,
# 3 x u32, 4 x u8 = 72 bytes after a 40-byte header; verdict is the third of the final four bytes).
# Prints "<records> <failing records>".
events_stats() { python3 -I - "$1" <<'PY'
import struct, sys
data = open(sys.argv[1] + "/events.bin", "rb").read()
assert data[:8] == b"CUTOFFEV", "not an events.bin"
n = (len(data) - 40) // 72
bad = sum(1 for i in range(n) if struct.unpack_from("<7Q3I4B", data, 40 + 72 * i)[12] != 0)
print(n, bad)
PY
}
# drill.tsv of a run folder: "<rows> <killed> <never-infected> <infected-not-propagated> <propagated-not-detected> <found-by-stop>"
tsv_stats() { python3 -I - "$1" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1] + "/drill.tsv"), delimiter="\t"))
c = lambda o: sum(1 for r in rows if r["outcome"] == o)
print(len(rows), c("killed"), c("survivor:never-infected"), c("survivor:infected-not-propagated"), c("survivor:propagated-not-detected"), sum(1 for r in rows if r["found_by_stop"] == "yes"))
PY
}
# drill.tsv without the "ran" column (the only column the activation filter may change)
tsv_outcomes() { python3 -I - "$1" <<'PY'
import csv, sys
for r in csv.DictReader(open(sys.argv[1] + "/drill.tsv"), delimiter="\t"):
    r.pop("ran"); print("\t".join(r.values()))
PY
}
# cross-check a drill run folder: events.bin vs summary.json, drill.tsv vs summary.json
cross_check() {
  local dir="$1" label="$2" programs
  read -r nrec nbad < <(events_stats "$dir")
  programs=$(json_num "$dir" programs)
  read -r rows killed ni inp pnd fbs < <(tsv_stats "$dir")
  check "$label: events.bin holds $nrec records = summary.json programs ($(json_num "$dir" programs)); failures in events.bin: $nbad" "$([ "$nrec" = "$(grep -o '"programs": [0-9]*' "$dir/summary.json" | head -1 | grep -o '[0-9]*$')" ] && echo ok)"
  check "$label: drill.tsv killed $killed = summary.json killed ($(json_num "$dir" killed))" "$([ "$killed" = "$(json_num "$dir" killed)" ] && echo ok)"
  check "$label: survivor classes in drill.tsv ($ni / $inp / $pnd) = summary.json" "$([ "$ni" = "$(json_num "$dir" never_infected)" ] && [ "$inp" = "$(json_num "$dir" infected_not_propagated)" ] && [ "$pnd" = "$(json_num "$dir" propagated_not_detected)" ] && echo ok)"
  check "$label: killed + survivors = planted ($rows)" "$([ $((killed + ni + inp + pnd)) -eq "$rows" ] && echo ok)"
}

echo "== 1. the golden side of a drill vs the original core's Step 4 gate (commit $head_commit)"
for w in 8 1; do
  "$cutoff" drill --mutants 1 --max-programs 100000 --seed 1 --workers "$w" --results-dir "$out" --run-id "golden-${w}w" --no-progress > "$out/golden-${w}w.txt"; code=$?
  check "drill --mutants 1, $w worker(s): exit code 0 (got $code)" "$([ $code -eq 0 ] && echo ok)"
  check "drill --mutants 1, $w worker(s): events.bin == Step 4 gate ($(sha "$out/golden-${w}w/events.bin" | cut -c1-12))" "$([ "$(sha "$out/golden-${w}w/events.bin")" = "$STEP4_SHA" ] && echo ok)"
done

echo "== 2. activation-filter soundness by brute force: all 201 mutants x $PROGRAMS_BF programs, then the filtered drill on the same programs"
t=$(now)
"$cutoff" drill --no-filter --max-programs "$PROGRAMS_BF" --seed 1 --workers 8 --results-dir "$out" --run-id brute --no-progress > "$out/brute.txt"; code=$?
bf_seconds=$(elapsed "$t")
check "brute force: exit code 0 (got $code), $bf_seconds s" "$([ $code -eq 0 ] && echo ok)"
check "brute force: every pair was simulated ($(json_num "$out/brute" pairs_total) pairs, $(json_num "$out/brute" pairs_skipped) skipped)" "$([ "$(json_num "$out/brute" pairs_skipped)" = 0 ] && [ "$(json_num "$out/brute" pairs_total)" = $((201 * PROGRAMS_BF)) ] && echo ok)"
check "brute force: runs where a mutant changed the run without being infected: $(json_num "$out/brute" propagated_without_infection)" "$([ "$(json_num "$out/brute" propagated_without_infection)" = 0 ] && echo ok)"
check "brute force: detections without a changed run: $(json_num "$out/brute" detected_without_propagation)" "$([ "$(json_num "$out/brute" detected_without_propagation)" = 0 ] && echo ok)"
cross_check "$out/brute" "brute force"
t=$(now)
"$cutoff" drill --max-programs "$PROGRAMS_BF" --seed 1 --workers 8 --results-dir "$out" --run-id filtered --no-progress > "$out/filtered.txt"; code=$?
f_seconds=$(elapsed "$t")
check "filtered: exit code 0 (got $code), $f_seconds s (brute force $bf_seconds s)" "$([ $code -eq 0 ] && echo ok)"
check "filtered: skipped $(json_num "$out/filtered" pairs_skipped) of $(json_num "$out/filtered" pairs_total) mutant runs" "$([ "$(json_num "$out/filtered" pairs_skipped)" -gt 0 ] && echo ok)"
check "filtered: every column except 'ran' of drill.tsv equals the brute-force run's" "$([ "$(tsv_outcomes "$out/filtered")" = "$(tsv_outcomes "$out/brute")" ] && echo ok)"
check "filtered and brute force: identical events.bin" "$([ "$(sha "$out/filtered/events.bin")" = "$(sha "$out/brute/events.bin")" ] && echo ok)"
cross_check "$out/filtered" "filtered"

echo "== 3. the obvious planted bug (ALU add -> subtract, line 1240) in the filtered drill"
"$cutoff" build --drill --list > "$out/manifest.tsv"
bug=$(awk -F'\t' '$6==1240 && $5=="+ -> -" {print $1}' "$out/manifest.tsv")
row=$(awk -F'\t' -v id="$bug" '$1==id {print $13, $12}' "$out/filtered/drill.tsv")
set -- $row
check "mutant $bug: outcome $1, first detected at program $2 (killed, <= 3)" "$([ "$1" = killed ] && [ "$2" != - ] && [ "$2" -le 3 ] && echo ok)"

echo "== 4. stop rule observed: epsilon $EPS, confirm 3, min 1000, checkpoint 500, budget $PROGRAMS_STOP"
t=$(now)
"$cutoff" drill --epsilon "$EPS" --confirm 3 --min-programs 1000 --checkpoint 500 --max-programs "$PROGRAMS_STOP" --seed 1 --workers 8 --results-dir "$out" --run-id stop --no-progress > "$out/stop.txt"; code=$?
check "stop-rule drill: exit code 0 or 2 (got $code), $(elapsed "$t") s" "$([ $code -eq 0 ] || [ $code -eq 2 ] && echo ok)"
cross_check "$out/stop" "stop-rule drill"
stop_at=$(grep -o '"stop_at": [0-9a-z]*' "$out/stop/summary.json" | head -1 | grep -o '[0-9a-z]*$')
check "stop rule fired at $stop_at programs (null = it did not fire within the budget)" "$([ "$stop_at" != null ] && echo ok)"

echo "== 5. interrupt and resume of a drill ($PROGRAMS_BF programs, interrupted at half)"
printf '[debug]\ninterrupt_after = %d\n' $((PROGRAMS_BF / 2)) > "$out/interrupt.toml"
"$cutoff" drill --config "$out/interrupt.toml" --max-programs "$PROGRAMS_BF" --seed 1 --workers 8 --checkpoint 500 --results-dir "$out" --run-id resumed --no-progress > "$out/resumed-1.txt"; code=$?
check "first session: interrupted, exit code 130 (got $code)" "$([ $code -eq 130 ] && echo ok)"
"$cutoff" drill --resume "$out/resumed" --workers 3 --no-progress > "$out/resumed-2.txt"; code=$?
check "second session (3 workers): exit code 0 (got $code)" "$([ $code -eq 0 ] && echo ok)"
check "resumed events.bin == uninterrupted ($(sha "$out/resumed/events.bin" | cut -c1-12))" "$([ "$(sha "$out/resumed/events.bin")" = "$(sha "$out/filtered/events.bin")" ] && echo ok)"
check "resumed drill.tsv == uninterrupted drill.tsv" "$([ "$(cat "$out/resumed/drill.tsv")" = "$(cat "$out/filtered/drill.tsv")" ] && echo ok)"

echo "results in $out; failures: $fail"
exit $fail
