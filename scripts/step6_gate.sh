#!/usr/bin/env bash
# Step 6 gate on the release binary (refuses to run on a dirty tree; rebuilds and verifies the binary matches HEAD):
#  1. instrumented core, no mutant selected: 100,000 programs, seed 1, must give an events.bin byte-identical to the
#     original core's Step 4 gate (SHA-256 below), for 8 and for 1 worker;
#  2. the embedded manifest is complete and consistent;
#  3. an obvious planted bug (ALU add turned into subtract) is caught almost at once under C1;
#  4. informational scan: every mutant is run on the first 500 programs of the stream; records which ones produce a
#     failure and how early. It is NOT the drill (no activation filter, no funnel, no equivalence analysis: Step 7).
# usage: scripts/step6_gate.sh [out-dir]
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/results/step6-$(date -u +%Y%m%d-%H%M%S)}"
cutoff="$root/build/release/cli/cutoff"
STEP4_SHA=812caad7a0edeea676aebb6675e9e8f4db64ff271370a3daf3f1d10fdeeab598

mkdir -p "$out"
if [ -n "$(git -C "$root" status --porcelain)" ] && [ "${ALLOW_DIRTY:-0}" != "1" ]; then
  echo "step6_gate: uncommitted changes; commit first (or ALLOW_DIRTY=1 for a throwaway run)." >&2; exit 3
fi
cmake --preset release > /dev/null && cmake --build --preset release > /dev/null || { echo "step6_gate: release build failed" >&2; exit 3; }
head_commit=$(git -C "$root" rev-parse --short=12 HEAD)
built_commit=$("$cutoff" run --max-programs 2 --workers 1 --results-dir "$out/probe" --run-id probe --no-progress > /dev/null 2>&1; grep -o '"git_commit": "[0-9a-f]*"' "$out/probe/probe/summary.json" | cut -d'"' -f4; rm -rf "$out/probe")
if [ "$built_commit" != "$head_commit" ]; then echo "step6_gate: binary was built from $built_commit but HEAD is $head_commit" >&2; exit 3; fi

fail=0
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
check() { if [ "$2" = "ok" ]; then echo "PASS  $1"; else echo "FAIL  $1"; fail=1; fi; }
# first failing program id of a run folder (or -1)
first_failure() { python3 -I - "$1" <<'PY'
import struct, sys
data = open(sys.argv[1] + "/events.bin", "rb").read()
n = (len(data) - 40) // 72
for i in range(n):
    if data[40 + 72 * i + 62] != 0:
        print(i); break
else:
    print(-1)
PY
}

echo "== 1. instrumented core, nothing mutated, against the original core's Step 4 gate (commit $head_commit)"
for w in 8 1; do
  "$cutoff" run --max-programs 100000 --seed 1 --mutant 0 --workers "$w" --results-dir "$out" --run-id "m0-${w}w" --no-progress > "$out/m0-${w}w.txt"; code=$?
  check "mutant 0, $w worker(s): exit code 0 (got $code)" "$([ $code -eq 0 ] && echo ok)"
  check "mutant 0, $w worker(s): events.bin == Step 4 gate ($(sha "$out/m0-${w}w/events.bin" | cut -c1-12))" "$([ "$(sha "$out/m0-${w}w/events.bin")" = "$STEP4_SHA" ] && echo ok)"
done

echo "== 2. embedded manifest"
"$cutoff" build --drill --list > "$out/manifest.tsv"
rows=$(($(wc -l < "$out/manifest.tsv") - 1))
contiguous=$(awk -F'\t' 'NR>1 && $1 != NR-1 {bad=1} END {print bad ? "bad" : "ok"}' "$out/manifest.tsv")
check "manifest has $rows mutants with ids 1..$rows" "$contiguous"
"$cutoff" build --drill > "$out/summary.txt"

echo "== 3. obvious planted bug (ALU add -> subtract, line 1240)"
bug=$(awk -F'\t' '$6==1240 && $5=="+ -> -" {print $1}' "$out/manifest.tsv")
"$cutoff" run --mutant "$bug" --max-programs 20 --workers 2 --results-dir "$out" --run-id obvious --no-progress > "$out/obvious.txt"; code=$?
ff=$(first_failure "$out/obvious")
check "mutant $bug: exit code 1 (got $code)" "$([ $code -eq 1 ] && echo ok)"
check "mutant $bug: first failing program is $ff (<= 3)" "$([ "$ff" -ge 0 ] && [ "$ff" -le 3 ] && echo ok)"

echo "== 4. informational scan: every mutant on the first 500 programs (not the drill)"
printf '[output]\nmax_failure_dirs = 0\n' > "$out/scan.toml"
printf 'id\tcategory\top_class\tkind\tline\tfailures\tfirst_failure\n' > "$out/scan.tsv"
tail -n +2 "$out/manifest.tsv" | while IFS=$'\t' read -r id site cat cls kind line orig mut; do
  "$cutoff" run --config "$out/scan.toml" --mutant "$id" --max-programs 500 --seed 1 --workers 8 --results-dir "$out/scan" --run-id "m$id" --no-progress > /dev/null 2>&1
  failures=$(grep -o '"count": [0-9]*' "$out/scan/m$id/summary.json" | head -1 | grep -o '[0-9]*$')
  ff=$(first_failure "$out/scan/m$id")
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$id" "$cat" "$cls" "$kind" "$line" "$failures" "$ff" >> "$out/scan.tsv"
  rm -rf "$out/scan/m$id"
done
awk -F'\t' 'NR>1 {n++; if ($6>0) k++; c[$2]++; if ($6>0) ck[$2]++} END {printf "      %d of %d mutants produced at least one failure in 500 programs\n", k, n; for (x in c) printf "      %-8s %d of %d\n", x, ck[x]+0, c[x]}' "$out/scan.tsv" | sort

echo "results in $out; failures: $fail"
exit $fail
