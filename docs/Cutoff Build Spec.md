# Cutoff: A CPU Regression Engine That Knows When to Stop — Build Spec

Oct 10, 2026 · @Manan Amit Shah

## How to use this spec

This spec defines a software product to build end to end; implement it milestone by milestone (section 12) and let every claim in the README come from a measured run.

- **What it is:** a working C++ tool, working name Cutoff. It runs random-program regressions on a RISC-V core, checks them against a reference model, estimates the risk that more testing would find a new bug, and stops itself when that risk is low enough.
- **Read order:** sections 2 and 5 say what to build; 6 to 10 say how; 11 says how to prove it works; 12 gives the order of work; 14 gives the working rules.
- **Source tags:** (S12) points to the numbered Sources in section 17.
- **Confidence tags:** \[verified\] = found in a source excerpt during research (full papers not read; see section 16); \[knowledge\] = general knowledge, confirm before relying on it; \[decision\] = a design choice that may be revised with a written reason.
- **No deadline:** take the time the work needs. Each milestone has acceptance tests, and nothing moves on until they pass.
- **Where the research went:** the statistics behind the stop rule come from software-fuzzing research (S30 to S35) and are used here as engineering components with their own tests, not as a paper to write.

## The product in one page

**Cutoff is a regression engine for CPU verification that stops itself when more testing is unlikely to find a new bug.**

**Who it is for:** a verification or CPU-design engineer running random-instruction regressions on a RISC-V core, and anyone who has to justify how long a regression ran.

**The problem.** Random regressions run until someone decides they have run long enough. That decision is usually a guess, while simulation time is a real cost. Software fuzzing has statistical estimators for exactly this decision (S30 to S34), but a limited search found no ready-made tool that applies them to CPU verification, where bugs often depend on data values that code coverage cannot see (S36).

**What it does**

1. Generates random RV32I programs from seeded, reproducible profiles.
2. Runs each program on the RTL under Verilator (S23) and on a reference model, in parallel, and compares the results.
3. Tracks which behaviours each program exercised, including operand-aware cross coverage, and keeps a live estimate of the chance the next program shows something new.
4. Stops when that risk stays below a threshold you set, or when the budget runs out.
5. Runs a self-test, the fire drill: plants known bugs in the core, repeats the campaign, and reports how many were found by the time the stop rule fired.
6. Writes a sign-off report (Markdown, JSON, static HTML) with the risk curve, coverage saturation, drill results and compute saved.

**Headline demo.** One command runs a campaign on PicoRV32, stops at the chosen risk, and prints programs run, failures found, estimated residual risk, planted bugs found vs missed, and compute saved against a fixed budget.

**Non-goals** \[decision\]: a new coverage-guided fuzzer, UVM or SystemVerilog testbench support, formal verification, pipelined multi-core SoCs, a cluster scheduler, a web app with a backend. In scope: one machine with many cores, one core under test at a time, static reports.

**Definition of done:** the milestones in section 12 pass their acceptance tests, the validation suite in section 11 runs end to end, and the README shows measured results with the exact commands that reproduce them.

## Why it matters

Chip teams spend about as much effort verifying a design as building it, and have no principled rule for when simulation has run long enough.

- **Verification is a large effort.** A 2022 industry study found about a one-to-one ratio of peak verification to design engineers on average, and up to 5-to-1 in segments such as processors. Demand for verification engineers grew 6.2% a year between 2007 and 2022 against 2.7% for design engineers, and verification engineers spend more time debugging than on any other activity (S1).
- **Hardware fuzzing is not yet routine in industry.** Hardware fuzzing papers argue that verification time dominates implementation time (S37), and that hardware bugs rarely crash an RTL simulation, which weakens the crash signal software fuzzing relies on (S36).
- **Practitioners guess when to stop.** In software fuzzing, practitioners can only guess when residual risk falls below an acceptable threshold; the residual-risk literature exists to replace that guess (S31).
- **Simulation is expensive.** Exhaustive mutation testing multiplies mutants, tests and seeds; one patent's example of a million mutants, 50 tests, 4 seeds and an hour per simulation needs 200 million hours (S42). A reliable stop rule is therefore a compute-cost lever (an inference, not a measured claim).
- **Easy to explain to a verification team.** A stop rule with measured error bars answers a question verification leads face when planning regressions.

## Landscape and reuse decisions

Cutoff reuses existing open tools for simulation and checking, and builds only the missing piece: a stop decision with a built-in self-test.

| Existing piece | What it offers | Decision |
| --- | --- | --- |
| Verilator (S26, S27) | Compiles RTL to C++; line, toggle and expression coverage; merging of coverage files | Reuse as the simulator and an optional coverage source |
| PicoRV32 (S23, S24, S25) | Small RV32 core with parameters, a simple native memory interface, a Verilator testbench target, and RVFI bindings in riscv-formal | Reuse as the first core under test |
| Spike, riscv-dv, UVM flows (S14, S15) | Industry generator and ISS trace comparison; need a RISC-V toolchain and a UVM-capable simulator | Do not depend on them; ship a small RV32I model and generator; Spike stays an optional cross-check |
| MCY and Yosys mutate (S5, S6, S7, S29) | Netlist-level mutation with formal filtering of equivalent mutants | Optional cross-check for the drill; the drill itself mutates RTL source in one instrumented build |
| Certitude-style qualification (S2, S3, S4) | Commercial mutation-based qualification of verification environments | Inspiration for the drill's infected, propagated, detected split |
| Processor fuzzers (S9, S11, S12) | Coverage-guided generators; Cascade builds long valid programs and was evaluated on PicoRV32 | Not rebuilt; Cutoff's generators stay simple and stateless so the risk estimators stay valid |
| Residual-risk estimators (S30 to S35) | Good-Turing discovery probability and variants for software fuzzing | Implement from the papers, with unit tests on synthetic data |
| TestRIG (S13) | Random sequences with trace comparison and shrinking | Model for the retire-trace checker and for the shrinker |

**Positioning.** The new piece is the combination: a CPU-specific stop decision built on operand-aware species, validated by planted bugs inside the same tool. Describe what Cutoff does; do not claim it is the first of its kind.

**Dropped directions:** localizing the buggy RTL line from a failing test and ML triage of failures are crowded research areas (S38 to S41) and stay out of scope.

## Features and user workflows

Cutoff ships as one command-line tool with a subcommand per stage; every campaign is reproducible from its config file and seed.

| Command | Purpose | Key options | Output |
| --- | --- | --- | --- |
| `cutoff doctor` | Checks Verilator, compiler, core count and disk space; prints versions | none | Pass/fail checklist |
| `cutoff init <name>` | Creates a project and `cutoff.toml` for a core: RTL path, top module, memory adapter, ISA subset, profile, checker, risk threshold, budgets | `--core picorv32` | Project folder |
| `cutoff build` | Builds the Verilated worker and, on request, the instrumented drill build | `--drill` | Worker binaries |
| `cutoff run` | Runs a campaign until the stop rule fires or a budget ends | `--profile`, `--checker` (c1, c2 or c3), `--workers N`, `--epsilon`, `--confirm W`, `--min-programs`, `--max-programs`, `--max-hours`, `--resume` | Results folder and exit code |
| `cutoff drill` | Fire drill: plants bugs, repeats the campaign, reports found vs missed | `--mutants N`, `--seed`, `--operators FILE` | Drill results |
| `cutoff report <run>` | Regenerates Markdown, JSON and HTML reports | `--format` | Report files |
| `cutoff replay <program-id>` | Re-runs one program and prints the first divergence | `--trace` | Divergence report |
| `cutoff shrink <failure>` | Minimizes a failing program | `--max-steps` | Smaller failing program |
| `cutoff bench` | Runs the validation suite (section 11) | `--pool N` | Calibration tables and plots |

**Exit codes** \[decision\]: 0 = stopped at the risk threshold with no failures; 1 = at least one failure found; 2 = budget ended before the threshold was reached; 3 = setup or environment error. This makes Cutoff usable as a CI gate.

**Configuration** \[decision\]: one `cutoff.toml` holds every setting; the fully resolved config is copied into each results folder.

**Results folder:** `results/<run-id>/` containing `config.toml`, `events.bin` (compact per-program records), `summary.json`, `report.md`, `report.html`, and `failures/` (one folder per failure with seed, profile, divergence and replay command).

**Workflows**

1. First run: doctor, init, build, run, open the report.
2. Trust check: run the drill on the same core and profile before relying on a stop decision.
3. CI gate: run with a fixed budget and fail the job on exit code 1 or 2.
4. Failure triage: replay the failing id, shrink it, attach the divergence report.

**Generator profiles** (details in section 7): G0 uniform, G1 weighted mix, G2 boundary-biased operands, G3 dependency-heavy.

## Architecture

An orchestrator drives many worker processes, folds their results into the risk estimators in a fixed order, and decides when to stop.

| Component | Path | Responsibility | Language |
| --- | --- | --- | --- |
| CLI | `cli/` | Parses commands and config, prints progress, sets exit codes | C++20 |
| Orchestrator | `engine/` | Spawns workers, hands out seed ranges, re-orders results by program id, feeds the estimators, evaluates the stop rule, writes checkpoints | C++20 |
| Worker | `worker/` | Verilated core, memory model on PicoRV32's native valid/ready interface (S23), program loader, trace and species logging, cycle timeout; emits compact binary records | C++20 |
| Reference model | `model/` | RV32I instruction-set model that emits the same trace format | C++20 |
| Generators | `gen/` | Stateless, seeded program generators G0 to G3 | C++20 |
| Species and estimators | `risk/` | Species trackers, Good-Turing and Chao-type estimators, the stop rule; unit-tested on synthetic data | C++20 |
| Drill | `drill/` | Mutation-site enumeration, instrumented RTL generation, activation logic, drill runner | C++20 (Python allowed for RTL text rewriting) |
| Reports | `report/` | `summary.json`, Markdown, and static HTML with inline SVG charts | C++20 plus one HTML template |
| Bench | `bench/` | Pool runner, ground truth by resampling, calibration tables and plots | C++20 core, Python for plots |
| Core under test | `rtl/` | PicoRV32 pinned as a git submodule; the instrumented copy is generated, never hand-edited | Verilog |
| Docs and tests | `docs/`, `tests/` | Decisions log, verification log, user guide, unit and integration tests | Markdown, C++20 |

**Data flow:** seeds go to the generator; each worker runs the program on the golden core and the model and compares them; compact records return to the orchestrator; the estimators update; the stop rule decides; the report is written.

&#91;embedded content: pipeline architecture · 7 components\]

Workers run the golden core against the model; the drill swaps in an instrumented core with planted bugs; the orchestrator folds results in program order, so a run is identical whatever the worker timing.

**Process model** \[decision\]: one process per worker, talking to the orchestrator over pipes. This isolates Verilated model state and avoids thread-safety questions. The orchestrator re-orders results by program id before updating the estimators, so a campaign gives identical results whatever the worker scheduling.

**Checkpoints** \[decision\]: every K programs, write the estimator state and the next seed so that `--resume` continues exactly where the run stopped.

**Tooling** \[decision\]: C++20, CMake, GoogleTest, clang-tidy, and AddressSanitizer plus UBSan builds in CI. Python is used only for plotting in `bench/`.

**Determinism.** Every program is reproducible from (generator version, profile, seed). Store seeds, not program binaries, in result files. Fix initial values in the Verilated model \[knowledge: Verilator is two-state; check the initialization flags of the pinned version\].

## Core engines: reference model, generator and checkers

These three engines decide whether a mismatch is a real bug, so they get the most tests.

**Core under test.** PicoRV32 configured as plain RV32I \[decision\]. Its README lists parameters such as ENABLE\_COUNTERS, CATCH\_MISALIGN and CATCH\_ILLINSN (S23); read the pinned version's parameter defaults and record them \[knowledge: confirm that the multiply, divide, compressed and interrupt options are off\]. The core has a simple valid/ready memory interface that completes one transfer at a time (S23), so the memory model is small: a flat RAM with fixed latency \[decision\].

**Memory map and termination** \[decision\]: code at the core's reset address, a data region, and a magic store address that ends the program. Every program also has a cycle limit; hitting it is reported as a hang failure, separate from a mismatch.

**Reference model rules** (S28 unless noted)

- 32 registers, x0 hardwired to zero, writes to x0 ignored.
- The immediate's sign always comes from instruction bit 31.
- JALR sets the lowest bit of its target to zero; a historical PicoRV32 issue concerned exactly this (S22). Implement the spec, check the pinned core, and document any mismatch as a core finding rather than hiding it.
- Shift amounts use the low 5 bits \[knowledge\]. Signed and unsigned comparisons (SLT, SLTU, BLT, BLTU and the others) are tested separately \[knowledge\].
- A jump or branch to a target that is not 4-byte aligned traps in a core without compressed instructions \[knowledge\]; generators must avoid it.
- Misaligned loads and stores trap when CATCH\_MISALIGN is set (S23); generators emit aligned accesses only.
- Avoid illegal instructions, ECALL, EBREAK, CSR access and the cycle or instruction counters, whose behaviour differs between core and model by design (S23: counter instructions trap when ENABLE\_COUNTERS is 0).
- Align the model's reset address, memory map and initial register values with the testbench; fuzzing papers report false mismatches when reference and simulation setups differ (S12).

**Generator profiles** (all stateless and seeded)

| ID | Name | What it does | Purpose |
| --- | --- | --- | --- |
| G0 | Uniform | Opcodes, registers and immediates drawn uniformly from the legal set | Baseline |
| G1 | Weighted mix | Configurable weights for arithmetic, memory and branch groups | Realistic regression mix |
| G2 | Boundary-biased | Register initial values and immediates from boundary sets (0, 1, -1, INT\_MIN, INT\_MAX, alternating bit patterns); shift amounts 0, 1 and 31; odd and even jump-target offsets | Reaches data-dependent bugs |
| G3 | Dependency-heavy | High register reuse, rd equal to rs1 or rs2, short read-after-write chains, writes to x0 | Register-file and bypass logic |

**Program structure** \[decision\]: an initialization block loads registers with values from the profile's operand distribution; a body of L instructions in basic blocks with forward-only branches and jumps, so every program terminates; a terminating store. L is drawn from a configurable distribution.

**Generator invariants** (each has a unit test): always terminates; only aligned accesses; no illegal or excluded opcodes; identical output for identical (version, profile, seed); never adapts to past results.

**Checkers**

| Level | Compares | Needs |
| --- | --- | --- |
| C1 | Final registers and a hash of memory | Nothing extra |
| C2 | C1 plus the ordered store trace (address, data, strobe) | Memory-interface tap in the worker |
| C3 | C2 plus a per-instruction retire trace (PC, destination register and value, memory operation) | RVFI-style signals; riscv-formal has PicoRV32 bindings (S25) and the PicoRV32 Makefile has an RVFI monitor target (S24) \[knowledge: confirm how to enable RVFI in the pinned core, or fall back to hierarchical signal taps\] |

A divergence report names the first differing instruction, the expected and actual values, and the replay command.

## Drill engine: planted bugs and the self-test

The drill plants known bugs in the core and checks whether the campaign finds them, which tells the user how far to trust the stop decision.

**Mutation sites** \[decision\]: start with the ALU, comparators, branch-condition logic, immediate decode, register-file write enable and x0 handling, memory address and strobe logic, and the PC update. Aim for about 100 to 300 mutants at first.

**Operator set** (from published Verilog mutation operators, S20): exchange + and -; exchange == and !=; exchange <, <=, > and >=; exchange boolean and bitwise operators; drop or exchange ! and \~; replace integer constants with 0, 1, c+1 and c-1; replace bit-vector constants with all zeros or all ones. Domain additions \[decision\]: signed vs unsigned comparison, shift-amount off-by-one, stuck-at-0 or stuck-at-1 on selected bits. Optional: reintroduce real historical bugs from the core's git log, in the spirit of deriving operators from real bugs (S21).

**Instrumented build.** One generated Verilog file where every site k reads `(mut_sel == k) ? mutated : original`; mut\_sel = 0 means the original design. The core is compiled once and each mutant is selected at run time, which avoids one Verilator build per mutant. This is the same idea as instrumenting a design with multiplexors (S8) and Yosys's control-signal option for mutations (S7).

**Baseline gate.** With mut\_sel = 0 the instrumented build must behave identically to the original core and match the model.

**Activation filter** \[decision; validate before trusting\]. Each site also drives an infected bit, set when the mutated expression differs from the original while the site is evaluated. A mutant can only be exposed by a program whose golden run set that bit; if the bit never fires, the mutant run would be identical to the golden run. So mutant runs execute only for programs that infect the mutant. Validate by brute force on a sample: run every mutant on every program in a small set and confirm that no kill occurs where the filter skipped the run.

**Three levels per mutant and program** (activation, propagation and detection, as in functional qualification, S2): infected; propagated (architectural state differs from the golden run); detected (the checker flags it). A mutant never infected is a stimulus gap; infected but never propagated suggests masking or equivalence; propagated but never detected is a checker gap.

**Outputs:** a manifest (id, file, line, operator, original and mutated text); per-mutant counts at each level; a list of survivors with their level; and the found-by-stop-time summary used by the report.

**Equivalent mutants.** A mutant never killed may be equivalent or just hard. Report raw and survivor-adjusted counts, with the mutation score defined as killed divided by (generated minus equivalent) (S19). Formal triage of survivors (Yosys with SBY or EQY, or MCY; S5, S25) is a later milestone.

**Cost.** Runs needed = programs times the mutants each program infects, with early exit on the first mismatch. Measure throughput in M2 and size the default mutant count from it.

## Stop-rule engine: species, estimators and the stop decision

The engine turns what each program exercised into a running estimate of the chance that the next program shows something new, and stops when that estimate stays low.

**Definitions**

- A *species* is an observable behaviour a program can exercise (table below).
- After n programs, Q\_j(n) is the number of species exercised by exactly j of them.
- The generator is stateless, so programs are independent draws. This keeps the estimators valid; adaptive, coverage-guided generators bias them (S31).

**Good-Turing discovery probability** (S30), the main estimate:

```latex
\hat{U}(n) = \frac{Q_1(n)}{n}
```

It is the number of species exercised by exactly one program, divided by the number of programs (S30), and approximates the chance that the next program exercises a species not yet seen.

**Species sets** (each is switched on or off in config)

| ID | Species | Rationale |
| --- | --- | --- |
| SP-op | Opcode (about 40 RV32I kinds) | Saturates fast; the weak baseline |
| SP-cross | Opcode x rs1 class x rs2 class; classes: zero, one, minus one, INT\_MIN, INT\_MAX, small positive, small negative, other | Data-aware cells aimed at value-dependent bugs |
| SP-cross+ | SP-cross plus immediate class, shift-amount boundary (0, 1, 31), branch taken or not, result class (zero, negative, wrap) | Richer cells |
| SP-dep | Register dependency patterns: rd equals rs1 or rs2, read-after-write distance buckets, writes to x0 | Register-file behaviour |
| SP-code | Verilator coverage points: line, branch, expression, toggle (S26, S27) | Optional; costs a coverage dump per program |

&#91;decision\] The default stop rule uses the maximum risk across the enabled species sets.

**Other estimators** (all \[knowledge\]; verify against the papers before implementing)

- Chao-type richness: observed species plus a correction built from Q\_1 and Q\_2, in classic and bias-corrected forms.
- Incidence-based sample coverage in the style of Chao and Jost, giving a coverage-based risk estimate.
- The dependency-aware discovery-probability estimator from the ICSE 2026 paper (S32), as a stretch.

Report every estimator; the stop rule uses the one the validation suite selects (section 11).

**Stop rule** \[decision\]

1. Do not allow a stop before `min_programs`.
2. Every `checkpoint` programs, compute the risk estimate.
3. Stop when the estimate has stayed below `epsilon` for `confirm` consecutive checkpoints.
4. Stop anyway at `max_programs` or `max_hours`, with exit code 2.
5. Baselines kept for comparison: a fixed budget, and a plateau rule (no new species in the last W programs); saturation-based criteria are the same family as in S35.

Defaults for `epsilon`, `confirm` and `min_programs` come from the validation suite, not from guesses.

**Edge cases.** An estimate of zero after few programs is not evidence of safety, which is why `min_programs` exists. Very sparse species sets inflate Q\_1, so report set sizes next to the estimate. Never enable adaptive generation without revisiting the estimators.

**What the number means.** The estimate is a proxy for the chance of seeing new behaviour; the fuzzing literature uses discovery probability as an upper bound on residual risk (S31). It is not a probability that the core is bug-free. The drill is the check on how far the proxy can be trusted for a given core.

## Reports and dashboard

Every run ends with a sign-off report a reviewer can read without running anything: one Markdown file, one JSON file and one offline HTML page.

**`summary.json`** \[decision\] holds the machine-readable result:

| Field group | Contents |
| --- | --- |
| run | Run id, date, git commit, tool version, Verilator and compiler versions, CPU model and core count, config hash |
| core | Core name and commit, parameters, ISA subset, checker level |
| campaign | Programs run, programs per second, wall time, stop reason (threshold, budget or failure), exit code |
| failures | Count; per failure: program id, profile, seed, first divergence, replay command |
| risk | Per species set: size, species seen, Q1 and Q2, risk estimate at each checkpoint, the estimator the stop rule used |
| drill | Planted mutants; infected, propagated and detected counts; found by stop time; survivors by level |
| compute | Programs and hours used, compared with a fixed budget and with the plateau rule |

**Report sections** \[decision\]: verdict line (stopped, risk, failures); risk curve; species saturation; failures with replay commands; drill results and what they mean; compute saved; reproducibility block (exact commands, versions, seeds).

**HTML requirements:** one self-contained file; no network access; charts drawn as inline SVG; readable on a phone; no external JavaScript dependencies.

**Charts**

| Chart | Data | Notes |
| --- | --- | --- |
| Risk curve | Risk estimate vs programs, log scale, one line per species set | Epsilon as a dashed line; stop point marked |
| Species saturation | Species seen vs programs, per set | Shows which sets are still growing |
| Drill funnel | Planted, infected, propagated, detected counts | Survivors annotated by level |
| Compute comparison | Programs used by the stop rule vs a fixed budget vs the plateau rule | Include planted bugs missed by each |
| Failures | Table of failures | Replay command copyable |

**Honesty rules for the report** \[decision\]: print the drill result next to every stop decision; label estimates as estimates; print how many planted bugs the drill could not expose; never print a verdict without the numbers behind it.

## Validation and acceptance tests

The product is only credible if its parts are tested and its stop decision is checked against known answers; this section lists those tests.

**Unit tests**

- **Reference model:** each RV32I instruction against hand-computed vectors, including boundary values (INT\_MIN, -1, shifts of 0, 1 and 31, JALR with odd targets). Optional cross-check against Spike if installed.
- **Generators:** the invariants in section 7, plus identical output for identical (version, profile, seed).
- **Estimators:** simulate species counts from a known occupancy model and confirm the estimators match theory on large samples; confirm no estimator reads outcomes of future programs.
- **Orchestrator:** results are identical for 1 and N workers; `--resume` reproduces an uninterrupted run exactly.

**Integration gates**

- **Baseline gate:** at least 100,000 random programs on the unmutated core with zero mismatches under C1. Any real mismatch is investigated and documented, never suppressed.
- **Instrumented-build gate:** mut\_sel = 0 matches the original core and the model.
- **Activation-filter gate:** no kill is lost by the filter on a brute-force sample.
- **Drill sanity:** a deliberately obvious bug (for example an ALU add turned into a subtract) is found within a few programs under every checker.
- **Report gate:** `summary.json` validates against its schema and the HTML opens offline.

**Calibration benchmark** (`cutoff bench`). This tests the stop rule itself, using the drill as ground truth.

1. For each generator profile, run a large pool of programs (start at 10,000; scale up as compute allows) through the golden core and all infected mutants, storing species and kill matrices. Split the pool in two halves: campaigns draw from half A, ground truth from half B.
2. Measure the true residual discovery probability on half B. The generator is stateless, so this is exact up to sampling error:

```latex
R_n(G,c) = \Pr_{P \sim G}\left[ \exists\, k \notin K_n : D_c(P,k) = 1 \mid K_n \right]
```

Here K\_n is the set of mutants killed by the first n programs of a campaign, and D\_c(P,k) = 1 when program P exposes mutant k under checker c.

3. Compute the calibration error at each checkpoint (positive = over-estimate, negative = under-estimate):

```latex
\rho(n) = \log_{10} \frac{\hat{U}(n)}{R_n(G,c)}
```

4. Draw at least 200 campaigns per profile by permuting half A.
5. Report: the median and spread of rho; the share of checkpoints where the estimate is below the truth (a stop-too-early risk); a reliability diagram (binned estimate vs observed new-kill frequency); and for each epsilon, programs at stop, fraction of killable mutants found, mutants missed, and programs saved vs a fixed budget and vs the plateau rule.
6. Statistics: bootstrap confidence intervals over campaigns; cluster resampling by mutation site, because mutants are correlated; hold out some mutation operators when choosing defaults.

**Design questions the benchmark answers** (they set the defaults and the wording of the README; any outcome is acceptable): do opcode-only species under-estimate residual risk on data-dependent bugs? Do operand-aware cross species reduce the error? Which epsilon and confirm values give a good trade-off between programs saved and bugs missed?

**Acceptance for the validation suite:** `cutoff bench` runs end to end from a clean checkout; every table and plot in the README is produced by it; and the defaults in `cutoff.toml` cite the benchmark run that chose them.

## Milestones and acceptance tests

Build in eight gated milestones; a milestone starts only when the previous gate passes. Sizes are relative effort, not time (S small, M medium, L large).

&#91;embedded content: roadmap · 8 milestones, 8 gates\]

Work runs left to right along the top row, then continues on the bottom row; the table below gives each gate in full.

| Milestone | Builds | Size | Acceptance gate |
| --- | --- | --- | --- |
| M0 Foundations | Repo, CMake, CI with ASan and UBSan, PicoRV32 submodule pinned, Verilated core builds, hand-assembled smoke program runs and terminates, `cutoff doctor` | S | Clean build from scratch; two runs of the smoke program give identical traces; doctor prints tool versions |
| M1 Model, generator, checker | RV32I model, profile G0, checker C1, worker process, `cutoff run` with a fixed program count | L | All unit tests pass; baseline gate (at least 100,000 programs, zero mismatches, or every mismatch documented); results identical for 1 and N workers |
| M2 Species and stop rule | Species SP-op and SP-cross, Good-Turing estimator, stop rule, checkpoint and resume, live progress, throughput measurement | M | Estimator tests on synthetic data pass; a run stops by the rule with the right exit code; resume reproduces an uninterrupted run exactly |
| M3 Drill | Mutator, instrumented build, activation filter, `cutoff drill`, three-level funnel | L | Instrumented-build gate; activation-filter gate; drill sanity bug found; manifest and funnel produced |
| M4 Profiles, checkers, failures | Profiles G1 to G3, checkers C2 and C3, failure capture, `cutoff replay`, profile config files | M | Invariant tests per profile; each failure folder reproduces its divergence by replay; seeded test bugs are caught at the expected checker levels |
| M5 Reports | `summary.json`, Markdown and HTML reports, compute comparison against fixed budget and plateau rule | M | Report gate; HTML opens offline; the headline demo runs with one command |
| M6 Benchmark | `cutoff bench`: pool runner, ground truth, calibration tables and plots, defaults chosen and recorded, README results | L | Bench runs from a clean checkout; README tables are generated; defaults cite the bench run that chose them |
| M7 Hardening and release | Shrinker, SP-code species, Docker image, user guide, demo recording, tagged v0.1 | M | Fresh-machine install from the README works in the Docker image; all gates re-run green; the demo is reproducible |

**Stretch after M7:** formal triage of surviving mutants (S5, S25); a second core adapter; reintroduced historical bugs; an adaptive generator, which requires reworking the estimators first (S31).

## Risks and technical pitfalls

The biggest risks are a wrong reference model and over-trusting the stop estimate; both have gates in the milestones.

| Risk | Why it matters | Mitigation |
| --- | --- | --- |
| False mismatches between model and core | Differential testing reports false failures when setups differ (S12) | Align reset address, memory map and initial state; baseline gate; document real core quirks |
| Real quirks or bugs in PicoRV32 | A historical JALR issue was reported (S22); a fuzzer found new bugs in five of six cores it tested (S10) | Treat mismatches as findings, record them, keep generators inside the common subset |
| Equivalent or hard mutants | Unkillable mutants distort drill results (S19, S5) | Report raw and adjusted counts; formal triage as a later milestone |
| Planted bugs are not real bugs | Software studies find weak links between mutation score and later bug fixes (S18) | State the limit in every report; add historical real bugs; hold out operators |
| Estimate is a proxy | Discovery probability is an upper-bound proxy, not a bug probability (S31) | Run the drill beside every stop decision; use operand-aware species; report calibration |
| Results depend on the generator | The estimators assume a stateless generator | Defaults per profile; document the assumption; no adaptive generation without rework |
| One simple core | PicoRV32 is optimized for size, not performance (S23) | State it as a limitation; second core adapter as stretch |
| Compute cost | Mutants, tests and seeds multiply (S42) | Activation filter, early exit, early throughput measurement, pools sized to the budget |
| Simulator semantics | Verilator is two-state and initial values matter \[knowledge\] | Fix initialization; determinism tests |
| Wrong reference model | A model bug corrupts every result | Hand-computed vectors; optional Spike cross-check; baseline gate |
| Scope creep | The non-goals list is long for a reason | Milestone gates; decisions log for every deviation |
| Overclaiming | Results and novelty can be overstated | Rules in section 14: claims only from measured outputs |

## Working rules for Claude Code

These rules apply to every session on this project.

1. **Measured results only.** Never invent or estimate a number. Every figure in the README, reports or commit messages comes from a script's output stored under `results/`, with the commit hash and environment.
2. **Gates are real.** Do not start a milestone until the previous gate passes. If a gate fails, fix it, or write the failure and the reason in `docs/decisions.md`; never loosen a gate silently.
3. **Test the risky parts first.** Write the reference-model, generator and estimator tests before or alongside the code.
4. **Deterministic by construction.** Fixed seeds, recorded versions, and no dependence on wall-clock time or hash order in results.
5. **Verify before relying.** Every item tagged \[knowledge\] gets checked against the pinned tool or the paper; record the outcome in `docs/verification_log.md`.
6. **Report contradictions.** If a result contradicts this spec's assumptions (for example the activation filter loses kills, or an estimator is badly miscalibrated), say so and propose a change; do not tune until the numbers look right.
7. **Ask before** changing the core under test, dropping a milestone or non-goal, adding adaptive generation, or making any claim of novelty.
8. **Record decisions.** Every deviation from this spec goes in `docs/decisions.md` with a reason and a date.
9. **Say what was not run.** In every summary, list what was tested, what was not, and what failed.
10. **Keep it shippable.** Each milestone leaves `main` building, tested and documented; commit small, meaningful steps.
11. **Public material only.** Use public cores and public tools; no secrets or private data in the repo.
12. **Quality bar.** C++20, no warnings under -Wall -Wextra, clang-tidy clean on new code, sanitizers green in CI, and user-facing errors that say what to do next.

## Deliverables, demo and resume claims

**Deliverables**

- A public repository with the layout in section 6, CI badges and a tagged v0.1 release.
- A README with: what it is, a 60-second quickstart, the headline demo, the benchmark results (generated), the limitations, and how to reproduce every number.
- A user guide in `docs/`: installation, config reference, command reference, adding a core, reading the report, FAQ.
- A Docker image that runs the quickstart.
- A recorded demo of one full run: doctor, build, run to the stop point, open the report, run the drill.
- Released benchmark data (kill and species matrices) so others can re-analyse it.
- A short design note (2 to 4 pages) explaining the stop rule and its limits in plain language.

**Demo script** \[decision\]: `cutoff doctor`, `cutoff build --drill`, `cutoff run --profile g2 --epsilon <chosen>`, open `report.html`, `cutoff drill`. It must run unattended and print the verdict.

**Resume-claim discipline.** Write resume bullets only from numbers the tool produced, and keep a claims table that maps each bullet to the results file that proves it. Template (fill every blank from measured output, and drop any claim whose blank you cannot fill):

- Built a C++20 CPU regression engine (Verilator, RISC-V) that runs random-program regressions across \_\_ worker processes and stops automatically using Good-Turing risk estimates.
- Validated the stop rule with \_\_ planted RTL bugs: found \_\_% by the stop point while using \_\_% fewer programs than a fixed budget.
- If the benchmark supports it: operand-aware species cut the risk-estimation error by \_\_ (median log10 ratio) versus opcode-only species.

Do not write "novel" or "first". Be ready to explain the reference model, the mutation operators, the estimator formula and the limits (one simple core; planted bugs are not real bugs).

## Verification status and open items

**How "verified" was decided.** Items tagged \[verified\] were found in the excerpts that search returned for the sources in section 17; the full papers and pages were not read. Read the full source before implementing anything from it, especially S30 to S32.

**Found in source excerpts \[verified\]**

- Verification-to-design engineer ratios and growth rates, and that verification engineers spend most of their time debugging (S1).
- Mutation-based qualification concepts and tools: Certitude (S2 to S4), MCY (S5 to S7), instrumenting designs with multiplexors (S8), the Yosys mutate control signal (S7, S29).
- Processor fuzzers and their evaluation on PicoRV32 and other cores (S9 to S12); TestRIG (S13); riscv-dv and Ibex flows (S14, S15).
- Residual-risk theory: Good-Turing as singletons over samples (S30), adaptive bias in greybox fuzzing (S31), dependency-aware estimation (S32), stopping criteria (S33 to S35).
- PicoRV32 parameters, simple memory interface and Verilator testbench target (S23, S24); riscv-formal PicoRV32 bindings (S25).
- Verilator coverage options and merging (S26, S27); the RV32I register model with x0 hardwired to zero (S28); the historical JALR low-bit issue report (S22).
- The Verilog mutation operator list (S20) and the mutation score formula (S19).

**Not yet verified \[knowledge\]: check each one and log the result in `docs/verification_log.md`**

| Item | Why it matters | How to check |
| --- | --- | --- |
| PicoRV32 defaults for multiply, divide, compressed and interrupt options | Defines the RV32I configuration | Read the pinned `picorv32.v` |
| How to enable RVFI on the pinned core | Needed for checker C3 | Read the Makefile target and the riscv-formal bindings; fall back to hierarchical taps |
| Whether the JALR low-bit issue is fixed in the pinned core | Affects the baseline gate | Run a targeted unit test |
| Exact forms of the Chao-type and sample-coverage estimators, and the S32 estimator | Needed for the secondary estimators | Read the papers; test on synthetic data |
| Verilator two-state behaviour and initialization flags | Determinism | Read the docs for the pinned version; run determinism tests |
| Soundness of the activation filter under Verilator semantics | Drill cost and correctness | Brute-force comparison on a sample |
| Alignment rule for jump targets without compressed instructions | Generator correctness | Check the unprivileged spec and the core |
| Cost of a per-program coverage dump | Whether SP-code is practical | Measure in M7 |
| Novelty of the approach | Wording of any claim | Repeat the literature check below and report to the user |

**Literature check to repeat** (report close prior work to the user before continuing): residual risk or Good-Turing with hardware fuzzing, RTL or processor verification; stopping criterion or coverage-closure prediction with constrained-random verification; discovery probability with RISC-V; citations of S31, S32 and S36 for hardware follow-ups.

**Open questions for the user**

1. How many cores and how many hours of compute are available? This sets the pool sizes in M6.
2. Is a second core worth adding after M7, and which one?
3. Should the repository be public on GitHub from the start?
4. Is a short written design note wanted in addition to the README?

## Sources

Gathered by search during research on 2026-10-10; excerpts were read, not full pages.

- **S1** [The 2022 Wilson Research Group Functional Verification Study, Part 8 (Siemens)](https://blogs.sw.siemens.com/verificationhorizons/2022/12/12/part-8-the-2022-wilson-research-group-functional-verification-study/)
- **S2** [The principles of functional qualification (Tech Design Forums)](https://www.techdesignforums.com/practice/?p=2269)
- **S3** [Improve functional verification quality with mutation-based code coverage (Embedded)](https://www.embedded.com/?p=4008851)
- **S4** [On the functional qualification of a platform model (Fummi et al.)](https://iris.univr.it/handle/11562/334965)
- **S5** [MCY documentation](https://mcy.readthedocs.io/)
- **S6** [MCY repository](https://github.com/YosysHQ/mcy)
- **S7** [MCY mutate command](https://mcy.readthedocs.io/en/stable/_sources/mutate.rst.txt)
- **S8** [Infineon: mutation coverage collection with formal property checking (DVCon)](https://dvcon-proceedings.org/wp-content/uploads/75412-1.pdf)
- **S9** [Cascade: CPU fuzzing via intricate program generation (USENIX Security 2024)](https://usenix.org/system/files/usenixsecurity24-solt.pdf)
- **S10** [Cascade news coverage (The Register)](https://www.theregister.com/2023/10/24/cascade_fuzzer_zurich/)
- **S11** [ProcessorFuzz (arXiv)](https://arxiv.org/pdf/2209.01789)
- **S12** [Hardware fuzzing methods comparison (arXiv)](https://arxiv.org/pdf/2410.22561)
- **S13** [TestRIG: randomized testing of RISC-V CPUs](https://riscv.org/blog/2025/02/testrig-randomized-testing-of-risc-v-cpus/)
- **S14** [Ibex verification flow](https://ibex-core.readthedocs.io/en/latest/03_reference/verification.html)
- **S15** [riscv-dv README (fork)](https://github.com/taoliug/riscv-dv)
- **S16** [Mutation analysis with coverage discounting (DATE)](https://past.date-conference.com/date17/files/proceedings/2013/pdffiles/IP1_02.PDF)
- **S17** [Mind the Gap: coverage vs mutation score (arXiv)](https://arxiv.org/pdf/2309.02395)
- **S18** [Can testedness be effectively measured?](https://ics.uci.edu/~iftekha/publication/can-testedness-be-effectively-measured)
- **S19** [Mutation sampling technique, mutation score definition (arXiv)](https://arxiv.org/pdf/0710.4802)
- **S20** [Mutation testing with hyperproperties, Verilog operator list (arXiv)](https://arxiv.org/pdf/1907.07368)
- **S21** [GateTruth, mentions Mantra (arXiv)](https://arxiv.org/pdf/2608.12635)
- **S22** [riscv-torture issue on PicoRV32 JALR](https://github.com/ucb-bar/riscv-torture/issues/11)
- **S23** [PicoRV32 README (fork)](https://github.com/fpgs/picorv32)
- **S24** [PicoRV32 Makefile](https://github.com/YosysHQ/picorv32/blob/main/Makefile)
- **S25** [riscv-formal](https://github.com/olofk/riscv-formal)
- **S26** [Verilator coverage guide](https://veripool.org/guide/latest/simulating.html)
- **S27** [verilator\_coverage](https://veripool.org/guide/latest/exe_verilator_coverage.html)
- **S28** [RV32I base integer instruction set](https://docs.riscv.org/reference/isa/unpriv/rv32.html)
- **S29** [Yosys mutate command](https://yosyshq.readthedocs.io/projects/yosys/en/0.40/cmd/mutate.html)
- **S30** [STADS: software testing as species discovery](https://arxiv.org/pdf/1803.02130)
- **S31** [Estimating residual risk in greybox fuzzing (FSE 2021)](https://2021.esec-fse.org/details/fse-2021-papers/39/Estimating-Residual-Risk-in-Greybox-Fuzzing)
- **S32** [Dependency-aware residual risk analysis (ICSE 2026)](https://conf.researchr.org/details/icse-2026/icse-2026-research-track/138/Dependency-aware-Residual-Risk-Analysis)
- **S33** [Assurances in software testing: a roadmap](https://arxiv.org/pdf/1807.10255)
- **S34** [When to stop fuzzing (The Fuzzing Book)](https://www.fuzzingbook.org/html/WhenToStopFuzzing.html)
- **S35** [Green Fuzzing: a saturation-based stopping criterion (ISSTA)](https://www.cs.cit.tum.de/sse/aktuelles/article/new-paper-at-issta-green-fuzzing-a-saturation-based-stopping-criterion-using-vulnerability-prediction/)
- **S36** [SoK: oracles, guidance and input generation in hardware fuzzing](https://arxiv.org/pdf/2609.27300)
- **S37** [Fuzzing hardware like software](https://arxiv.org/pdf/2102.02308)
- **S38** [Wit-HW: bug localization via witness test generation](https://arxiv.org/pdf/2508.14414)
- **S39** [BluesFL: LLM-based fault localization for processors](https://www.alphaxiv.org/abs/2605.17290)
- **S40** [Clustering-based failure triage for RTL regression debugging (IEEE)](https://ieeexplore.ieee.org/document/7035339)
- **S41** [VCDiag: classifying erroneous waveforms for failure triage](https://arxiv.org/pdf/2506.03590v4)
- **S42** [Ranking combinations of mutants, test cases and random seeds (patent)](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9965580)
- **S43** [Mutation analysis for SystemC designs at TLM (Univ. Verona)](https://iris.univr.it/handle/11562/347753)
