#!/usr/bin/env bash
# Runs clang-tidy on Cutoff's own sources (not on Verilator's generated model or third-party code).
# usage: scripts/tidy.sh [build-dir]   (default build/debug; configure it first)
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build/debug}"
tidy="${CLANG_TIDY:-$(command -v clang-tidy || echo /opt/homebrew/opt/llvm/bin/clang-tidy)}"
files=()
while IFS= read -r f; do files+=("$root/$f"); done < <(cd "$root" && git ls-files 'cli/*.cpp' 'worker/*.cpp' 'tests/*.cpp' 'model/*.cpp' 'gen/*.cpp' 'risk/*.cpp' 'engine/*.cpp')
out="$("$tidy" -p "$build" --quiet --header-filter="^$root/(cli|worker|model|gen|risk|engine)/" "${files[@]}" 2>&1)"
if echo "$out" | grep -qE "warning:|error:"; then
  echo "$out" | grep -E "warning:|error:" | head -40
  exit 1
fi
echo "clang-tidy: clean (${#files[@]} files)"
