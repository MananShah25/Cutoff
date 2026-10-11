# Cutoff

A CPU regression engine that estimates when more random testing is unlikely to find a new bug, and stops itself.
Work in progress: see `docs/decisions.md` and `docs/verification_log.md`. This README contains no results yet,
because none have been measured.

## Status

Step 4 of 12 (campaign runner): `cutoff run` generates seeded random RV32I programs, runs each on the Verilated
PicoRV32 and on the reference model in parallel worker processes, and checks final registers and memory (checker C1).
There is no stop rule, drill, or report yet; a run executes a fixed number of programs.

Measured so far (Apple M3, 8 threads; full details and commit hashes in `docs/verification_log.md` V-27 to V-30):

| Run | Programs | Failures | Throughput |
| --- | --- | --- | --- |
| seed 1, 1 worker | 100,000 | 0 | 3,820 programs/s |
| seed 1, 8 workers | 100,000 | 0 | 22,220 programs/s |
| seed 2, 8 workers | 1,000,000 | 0 | 22,226 programs/s |

The two 100,000-program runs produce byte-identical `events.bin` files. Reproduce with `scripts/baseline_gate.sh`.
Zero failures says nothing about bugs that uniform random programs rarely reach (see `docs/decisions.md` D-018).

## Run

```bash
./build/release/cli/cutoff run --programs 100000 --seed 1     # settings: ./cutoff.toml, overrides: --help
```

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
| `engine/` | Orchestrator, worker loop, checker C1, config and results files |
| `worker/` | Verilated core and memory model |
| `rtl/` | `cutoff_core.v` wrapper; `picorv32/` is the pinned submodule |
| `tests/` | GoogleTest unit tests |
| `docs/` | Decisions log, verification log, archived baseline results |
| `cutoff.toml` | Default settings, documented |
