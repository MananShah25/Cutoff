# Cutoff

A CPU regression engine that estimates when more random testing is unlikely to find a new bug, and stops itself.
Work in progress: see `docs/decisions.md` and `docs/verification_log.md`. This README contains no results yet,
because none have been measured.

## Status

Step 6 of 12 (instrumented core): everything of Step 5 (seeded programs, the Verilated PicoRV32 and the reference model run
in parallel worker processes, checker C1, species and the Good-Turing stop rule, checkpoints and exact resume), plus a
generated copy of the core in which **201 planted bugs** (mutants, in the ALU, comparators, branch decisions, immediates,
register-file write and x0 handling, memory strobes and addresses, and PC update) can each be switched on at run time with
`--mutant N`. With no mutant selected the instrumented core is cycle-exactly the original. There is no drill yet: nothing
says how many of the planted bugs a given stop threshold would find; that is Step 7. The stop rule's defaults are
placeholders until the Step 11 benchmark.

Measured so far (Apple M3, 8 threads; commits and details in `docs/verification_log.md` V-27 to V-46):

| Run | Programs | Failures | Throughput |
| --- | --- | --- | --- |
| original core, seed 1, no species tracking (Step 4) | 100,000 | 0 | 22,220 programs/s (8 workers) |
| original core, seed 1, with species tracking | 100,000 | 0 | 12,839 programs/s (8 workers) |
| instrumented core, no mutant, seed 1 | 100,000 | 0 | 12,254 programs/s (8 workers) |
| original core, seed 1, stop rule epsilon 0.001 | stopped at 74,000 | 0 | exit code 0, same stop point for 1 and 8 workers |
| each of the 201 mutants, seed 1 (informational scan) | 500 each | 164 mutants fail at least once, 37 never | every killed mutant fails within its first 10 programs |

The event streams of the original core and of the instrumented core with no mutant selected are byte-identical
(`events.bin` SHA-256 `812caad7...`), for 1 and 8 workers. A 200,000-program run killed with SIGINT or SIGKILL and resumed ends
with byte-identical results. Reproduce with `scripts/baseline_gate.sh`, `scripts/step5_gate.sh` and `scripts/step6_gate.sh`.
Zero failures on the original core says nothing about bugs that uniform random programs rarely reach
(`docs/decisions.md` D-018); for example, two of the 37 mutants survive only because G0 never generates a backward jump.
The estimators are described in `docs/literature_check.md`; no claim of novelty is made.

## Run

```bash
./build/release/cli/cutoff run --max-programs 100000 --seed 1                    # fixed number of programs
./build/release/cli/cutoff run --max-programs 400000 --epsilon 0.001             # stop when the risk stays below 0.001
./build/release/cli/cutoff run --resume results/<run-id> --max-programs 800000   # continue a run, e.g. after Ctrl-C
./build/release/cli/cutoff build --drill --list                                   # the 201 planted bugs
./build/release/cli/cutoff run --mutant 90 --max-programs 100                     # run the core with planted bug 90
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
| `cli/` | The `cutoff` command (`doctor`, `run`, `build`) |
| `model/` | RV32I decoder, encoder and reference model |
| `gen/` | Seeded random-program generator (profile G0) |
| `drill/` | Mutant generator: lexer, expression parser, region table, mutation operators, instrumented-core emitter |
| `engine/` | Orchestrator, worker loop, checker C1, config, results files, checkpoints, risk monitor |
| `risk/` | Species, incremental tracker, estimators, stop rule |
| `worker/` | Verilated core and memory model |
| `rtl/` | `cutoff_core.v` wrapper; `picorv32/` is the pinned submodule |
| `tests/` | GoogleTest unit tests |
| `docs/` | Decisions log, verification log, literature check, archived baseline results |
| `cutoff.toml` | Default settings, documented |
