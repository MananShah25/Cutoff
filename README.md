# Cutoff

A CPU regression engine that estimates when more random testing is unlikely to find a new bug, and stops itself.
Work in progress: see `docs/decisions.md` and `docs/verification_log.md`. This README contains no results yet,
because none have been measured.

## Status

Step 5 of 12 (species, estimators, stop rule): `cutoff run` generates seeded random RV32I programs, runs each on the
Verilated PicoRV32 and on the reference model in parallel worker processes, checks final registers and memory (checker C1),
tracks which behaviours the programs exercised (SP-op: instruction kinds; SP-cross: kind x operand classes), estimates the
risk that the next program shows something new, and can stop itself when that risk stays below a threshold. Runs can be
interrupted (Ctrl-C or kill) and resumed exactly. There is no drill, so nothing yet says how many planted bugs a given
threshold would find; the stop rule's defaults are placeholders until the Step 11 benchmark.

Measured so far (Apple M3, 8 threads; details and commit hashes in `docs/verification_log.md` V-27 to V-38):

| Run | Programs | Failures | Throughput |
| --- | --- | --- | --- |
| Step 4 gate, seed 1, no species tracking | 100,000 | 0 | 22,220 programs/s (8 workers) |
| Step 4, seed 2, no species tracking | 1,000,000 | 0 | 22,226 programs/s (8 workers) |
| Step 5, seed 1, with species tracking | 100,000 | 0 | 12,839 programs/s (8 workers) |
| Step 5, seed 1, stop rule epsilon 0.001 | stopped at 74,000 | 0 | exit code 0, same stop point for 1 and 8 workers |

The event stream of the Step 4 and Step 5 seed-1 runs is byte-identical (`events.bin` SHA-256 `812caad7...`), for 1 and 8
workers. A 200,000-program run killed with SIGINT or SIGKILL and resumed ends with byte-identical results to an
uninterrupted run. Reproduce with `scripts/baseline_gate.sh` and `scripts/step5_gate.sh`.
Zero failures says nothing about bugs that uniform random programs rarely reach (`docs/decisions.md` D-018).
The estimators are the ones described in `docs/literature_check.md`; no claim of novelty is made.

## Run

```bash
./build/release/cli/cutoff run --max-programs 100000 --seed 1                    # fixed number of programs
./build/release/cli/cutoff run --max-programs 400000 --epsilon 0.001             # stop when the risk stays below 0.001
./build/release/cli/cutoff run --resume results/<run-id> --max-programs 800000   # continue a run, e.g. after Ctrl-C
```
Settings live in `./cutoff.toml`; `cutoff run --help` lists the options and exit codes (0 stopped at the threshold or ran
everything with no failure, 1 failure, 2 budget ended first, 3 error, 130 interrupted).

## Build (macOS)

```bash
brew install verilator cmake ninja llvm
git clone --recurse-submodules <repo-url> cutoff && cd cutoff
cmake --preset release && cmake --build --preset release
./build/release/cli/cutoff doctor
ctest --preset release
```

Sanitizers: `cmake --preset asan-macos && cmake --build --preset asan-macos && ctest --preset asan-macos`.
Static analysis: `scripts/tidy.sh build/debug`.

## Layout

| Path | Purpose |
| --- | --- |
| `cli/` | The `cutoff` command (`doctor`, `run`) |
| `model/` | RV32I decoder, encoder and reference model |
| `gen/` | Seeded random-program generator (profile G0) |
| `engine/` | Orchestrator, worker loop, checker C1, config, results files, checkpoints, risk monitor |
| `risk/` | Species, incremental tracker, estimators, stop rule |
| `worker/` | Verilated core and memory model |
| `rtl/` | `cutoff_core.v` wrapper; `picorv32/` is the pinned submodule |
| `tests/` | GoogleTest unit tests |
| `docs/` | Decisions log, verification log, literature check, archived baseline results |
| `cutoff.toml` | Default settings, documented |
