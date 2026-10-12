# Cutoff: Project Description

## Purpose

Cutoff is a deterministic, simulation-based regression engine for a small RISC-V CPU. It generates constrained random RV32I programs, executes every program on both a reference model and a Verilated RTL implementation, classifies any disagreement, and records reproducible evidence. Its longer-term objective is to decide when continued random testing is unlikely to discover a *new exercised behaviour*, rather than relying only on a fixed test-count budget.

The project currently targets [PicoRV32](https://github.com/YosysHQ/picorv32), configured as a plain RV32I core. It is deliberately scoped to one core and one local machine: it is not a general-purpose UVM environment, formal-verification system, coverage-guided fuzzer, multi-core SoC platform, or cloud scheduler.

The central claim is deliberately narrow: Cutoff can provide measured evidence about the generated-program distribution, the selected checker, and the selected RTL build. A clean campaign does **not** prove that the processor has no defects.

## What a campaign does

For program identifiers `0, 1, 2, …`, the generator derives a seed from the campaign seed and identifier. That makes every program independently addressable: a worker count, machine scheduling order, interruption, or resume does not change which program belongs to an identifier.

For each program, a worker performs this sequence:

1. Generate a legal RV32I program.
2. Run it in the in-process C++ reference model.
3. Run the same instruction words on a Verilated PicoRV32 wrapper with the same RAM, reset, memory latency, termination convention, and cycle limit.
4. Apply the selected checker.
5. Return a compact result record and, when enabled, species observations to the parent orchestrator.

The parent starts one local process per worker, communicates through binary pipes, reorders worker results by program ID, updates aggregate results and risk estimates, writes checkpoints, and decides whether to continue. Ordered delivery means the persisted event stream is independent of worker completion order.

## Core-under-test configuration

PicoRV32 is included as a pinned Git submodule at commit `ef203c2b0a3f`. Cutoff does not alter `picorv32.v`; its wrapper supplies the test configuration:

- RV32I instruction subset; counters are disabled.
- Registers initialize to zero.
- Verilator is configured for deterministic zeroed X-state handling.
- Memory is 64 KiB of byte-addressable RAM beginning at address zero.
- Code starts at address zero. Generated data accesses are confined to a separate window at `0x8000..0x8fff`.
- A store to `0x1000_0000` is a testbench exit operation; its written word is the program exit code.
- Other out-of-RAM accesses are memory faults. A core cycle limit prevents nontermination from hanging a campaign.

This setup is shared by the model and simulator because a differential checker is only useful if their environmental assumptions match.

## Program generation

The generator is a C++20 implementation with a shared RV32I encoder/decoder. It uses SplitMix64 to seed xoshiro256** and Lemire-style bounded integer sampling rather than implementation-dependent standard-library distributions. Therefore `(generator version, profile, campaign seed, program ID)` identifies exact instruction words.

Every generated program has three regions:

- An initialization block establishes random values for general-purpose registers and reserves `x31` as a stable data-base register.
- A generated body contains legal RV32I operations.
- An epilogue stores `x1` to the magic exit address.

Safety is built into the generator rather than inferred after simulation. Data accesses use the protected data base and aligned offsets; code cannot write itself; branches and jumps target valid instruction boundaries; and control-flow construction guarantees completion. This keeps a model timeout meaningful as a finding rather than a normal property of random code.

Four generator profiles are implemented:

| Profile | Intent |
| --- | --- |
| G0 | Uniform legal-opcode baseline, with forward-only control flow. |
| G1 | Weighted operation mix intended to increase useful architectural activity. |
| G2 | Boundary-biased register initialization, immediates, and shift values. |
| G3 | Higher register reuse to exercise data dependencies. |

G0 is intentionally weak in some dimensions: it has no loops or backward control flow, starts data memory at zero, and reaches boundary values only by chance. Those are known stimulus limitations, not evidence that the core lacks corresponding bugs.

## Reference model and checkers

The reference model implements the supported RV32I semantics directly in C++. It models x0, integer ALU operations, branches, JAL/JALR, loads and stores, alignment, endianness, faults, FENCE, unsupported instructions, architectural registers, RAM, and instruction retirement. It is unit-tested against hand-derived vectors, including signed edge cases and the JALR low-bit rule.

Three checker levels are available:

| Checker | Comparison | Purpose |
| --- | --- | --- |
| C1 | Final status, exit code, x1–x31, and final RAM hash | Fast architectural end-state differential check. |
| C2 | C1 plus the ordered non-terminating memory-store trace | Finds errors that happen to converge to the same final memory image. |
| C3 | C2 plus PicoRV32 RVFI retirement events compared against the model retirement trace | Finds instruction-by-instruction architectural divergence. |

RVFI is compiled only for C3. The terminating magic store is consumed by the harness before PicoRV32 reports an RVFI event, so C2 validates that store and C3 compares every earlier retirement. This is documented rather than silently ignored.

Failures are classified as generator faults, hangs, status mismatches, exit-code mismatches, register mismatches, memory mismatches, ordered-store mismatches, or RVFI-retirement mismatches. A failure folder saves `program.hex`, metadata, the observed verdict, and a replay command. `cutoff replay <results-folder> <program-id>` regenerates the program with the saved resolved configuration and re-runs its checker.

## Statistical stopping model

Cutoff tracks two kinds of *species* from the reference-model retire trace. A species is an exercised behavioural category, not a bug and not RTL coverage.

- **SP-op:** retired instruction kind.
- **SP-cross:** instruction kind crossed with coarse classes of source-operand values: zero, one, minus one, `INT_MIN`, `INT_MAX`, small positive, small negative, or other.

Species are counted as incidence data: a species contributes at most once per program. At checkpoints, the engine calculates Good–Turing `Q1 / n`, where `Q1` is the number of singleton species and `n` is the number of programs. This estimates the expected number of new species in the next generated program and is used as a conservative proxy for residual discovery opportunity. It is not a probability that the CPU is defect-free.

The stop rule requires the selected estimate to remain below epsilon for a configured number of consecutive checkpoints after a minimum test count. It also records a fixed-budget comparison and a simple no-new-species plateau baseline. Defaults remain provisional because the planned calibration benchmark has not yet measured how well species discovery predicts mutant discovery.

## Mutation testing and the drill

Cutoff can generate an instrumented PicoRV32 build containing 201 selectable RTL mutants across 103 expression sites. Mutation categories include ALU, branch, comparison, decode, immediate, memory, PC, register-file, and shift logic. The generated instrumented RTL preserves the original behaviour when no mutant is selected; this was checked both on short simulations and a 100,000-program baseline.

`cutoff drill` measures a three-level funnel for selected mutants:

1. **Infected:** the mutated expression changes for the program.
2. **Propagated:** the mutant changes the core run relative to the golden run.
3. **Detected:** the selected checker reports a failure.

The activation filter avoids simulating a mutant when its expression was not infected. A brute-force experiment checked 1,005,000 program–mutant pairs and found zero propagated-without-infection and zero detected-without-propagation cases; filtered and brute-force outcomes agreed. The drill distinguishes unexercised logic, masked changes, and checker gaps, but it does not establish formal equivalence of surviving mutants.

## Technology and architecture

| Area | Technology / approach |
| --- | --- |
| Implementation | C++20, compiled with Apple clang in the current development environment. |
| RTL simulation | Verilator 5.052 and generated C++ models. |
| RTL under test | PicoRV32, pinned Git submodule, wrapped but not modified. |
| Build | CMake presets and Ninja. |
| Tests | GoogleTest; release, debug, and AddressSanitizer configurations are supported. |
| Concurrency | Local Unix worker processes created with `fork()`, binary pipe protocol, deterministic ordered resequencing. |
| Persistence | Resolved TOML configuration, versioned binary event stream, FNV-checked checkpoint, CSV risk curve, JSON summary, and per-failure folders. |
| Mutation tooling | A project-local Verilog lexer, parser/walker, region selector, mutation emitter, and generated Verilated mutant core. |

The project uses a deliberately small TOML subset. Unknown, mistyped, and out-of-range settings fail with diagnostics. Command-line settings and config files use the same validation path, and a resolved config is written into every result directory.

## Determinism, interruption, and reproducibility

Determinism is a first-class design property:

- Generator seeds are stateless and random-accessible.
- Simulator X-state handling and initial register state are pinned.
- Worker results are committed in program-ID order.
- `events.bin` contains no timing-dependent fields.
- Checkpoints save stream identity, event/risk file positions, risk state, totals, and species counts.

On resume, Cutoff restores the checkpoint, truncates files to the saved byte positions, removes failure folders that must be regenerated, and continues the same stream. Tests and measured runs have shown that interrupted-and-resumed campaigns produce byte-identical events and risk curves to uninterrupted runs for the same stream.

## Measured results

All values below come from recorded runs; they are not projections.

| Measurement | Result |
| --- | --- |
| C1 baseline, G0, seed 1, 100,000 programs | 0 failures with both 1 and 8 workers; `events.bin` byte-identical across worker counts. |
| Larger C1 baseline, G0, seed 2, 1,000,000 programs | 0 failures. |
| Stop-rule demonstration, G0 | With epsilon 0.001, confirmation 3, and 1,000-program checkpoints, stopped at 74,000 programs identically with 1 and 8 workers. This is a demonstration setting, not a calibrated default. |
| Instrumented-core equivalence | With mutant selection 0, a 100,000-program C1 run had 0 failures and matched the original event-stream hash. |
| Drill activation-filter validation | 1,005,000 program–mutant pairs: 0 soundness violations; mutant 90 (ALU addition changed to subtraction) was detected on program 0. |
| Drill result | With the reported G0/C1 setup, 164 of 201 mutants were detected by the stop point at 3,500 programs; 166 were detected by 20,000 programs. |
| Current Mac benchmark, G2/C1, 8 workers | 100,000 programs, 0 failures, 22,419 programs/s, 4.46049 s. |
| Current Mac benchmark, G2/C3, 8 workers | 100,000 programs, 0 failures, 20,419 programs/s, 4.89735 s. |
| Current release test suite | 220 of 220 tests passed after C3 integration. |

The zero-mismatch results validate agreement between this model, this wrapper, and this constrained generator distribution. They do not validate unsupported ISA extensions, arbitrary firmware, security properties, timing closure, or bugs requiring stimuli absent from the profiles.

## Results and operational artifacts

Each run directory records the resolved configuration, event stream, checkpoint, risk curve, summary, and capped set of failure folders. Drill runs additionally write a mutant-level TSV funnel and drill summary block. These artifacts support reproducibility and post-run analysis without relying on console output.

The current `summary.json` format is explicitly provisional. Full Markdown and offline HTML reporting, a public final schema, calibration pool/kill matrices, and a `cutoff bench` command are planned but not yet complete.

## Current scope and remaining work

Implemented work includes the foundation, reference model, deterministic generator profiles, C1/C2/C3, multi-process campaign execution, species tracking and Good–Turing stopping, checkpoint/resume, mutation generation, instrumented simulation, activation-filter drill, failure capture, and replay.

The major unfinished deliverables are:

- Final report generator: stable `summary.json` schema, Markdown, self-contained offline HTML, and inline-SVG charts.
- Calibration benchmark: pool runner, species/kill matrices, half-pool ground truth, reliability analysis, epsilon/confirmation trade-off tables, and evidence-based defaults.
- Release hardening: program shrinker, SP-code species, Docker image, complete user guide/design note, generated README tables, and release tag.

These omissions are material. In particular, no final claim about compute savings, stop-rule calibration, or recommended stopping defaults should be made until the benchmark is implemented and run.

## How to interpret the project responsibly

Cutoff is best understood as a reproducible experimental framework for CPU random regression. It combines differential testing, trace checking, mutation testing, and a statistical stopping hypothesis. Its strongest evidence currently concerns deterministic execution, agreement between PicoRV32 and the model over measured program streams, and mutation-filter soundness. Its weakest—and intentionally unclaimed—area is whether a species-based stop signal predicts remaining bug-finding opportunity across profiles and checker levels. That is precisely the purpose of the remaining benchmark work.
