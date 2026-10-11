# Cutoff

A CPU regression engine that estimates when more random testing is unlikely to find a new bug, and stops itself.
Work in progress: see `docs/decisions.md` and `docs/verification_log.md`. This README contains no results yet,
because none have been measured.

## Status

Step 3 of 12 (generator G0): seeded, reproducible random RV32I programs that terminate by construction, on top of the
RV32I reference model (tested against hand-computed vectors and the Verilated PicoRV32). No orchestrator, checker,
estimator or report yet, so there are no results to report.

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
| `cli/` | The `cutoff` command |
| `model/` | RV32I decoder, encoder and reference model |
| `gen/` | Seeded random-program generator (profile G0) |
| `worker/` | Verilated core and memory model |
| `rtl/` | `cutoff_core.v` wrapper; `picorv32/` is the pinned submodule |
| `tests/` | GoogleTest unit tests |
| `docs/` | Decisions log and verification log |
