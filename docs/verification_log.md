# Verification log

Outcome of checking each `[knowledge]` item in the spec, plus environment findings. Everything here was observed on 2026-10-10 on an Apple M3 (8 cores), macOS (Darwin 25.6.0), Verilator 5.052, Apple clang 16.0.0, Homebrew LLVM 23.1.3, CMake 4.4.4, PicoRV32 `ef203c2`.

| ID | Item | Result | Evidence |
| --- | --- | --- | --- |
| V-01 | PicoRV32 defaults for multiply, divide, compressed, interrupts | **Confirmed.** Defaults are `ENABLE_MUL=0`, `ENABLE_FAST_MUL=0`, `ENABLE_DIV=0`, `COMPRESSED_ISA=0`, `ENABLE_IRQ=0`. **Exception:** `ENABLE_COUNTERS` and `ENABLE_COUNTERS64` default to **1**, so the wrapper sets them to 0. `CATCH_MISALIGN=1`, `CATCH_ILLINSN=1` by default. | `rtl/picorv32/picorv32.v` lines 62-88; wrapper `rtl/cutoff_core.v` |
| V-02 | Is the historical JALR low-bit issue fixed in the pinned core? | **Yes for the case tested.** `jalr x2, 0(x1)` with `x1 = 0x11` lands at `0x10`, link register is 8, skipped instruction did not run. This tests one odd-target case on the RTL, not the model (the model does not exist yet). | `Smoke.JalrClearsLowBitOfTarget` |
| V-03 | How to enable RVFI on the pinned core | **Partly answered.** RVFI ports exist in `picorv32.v` under ``ifdef RISCV_FORMAL`` (starting at line 123), and the core's Makefile builds with `-D RISCV_FORMAL`. Not yet exercised; the signal semantics and riscv-formal bindings are still to be read before checker C3 (Step 8). | `rtl/picorv32/picorv32.v`, `rtl/picorv32/Makefile` |
| V-04 | Can the register file be read from the Verilated model? | **Yes.** With `--public-flat-rw`, `rootp->cutoff_core__DOT__cpu__DOT__cpuregs[i]` reads register `i`. Used for the final-register check needed by checker C1. | `worker/sim_core.cpp`; `Smoke.AddProgramExitsWithSum` |
| V-05 | Verilator two-state behaviour and initialisation flags | **Pinned, and tested.** Built with `--x-initial 0 --x-assign 0`. Asking the runtime for random reset (`randReset(2)`, seed 12345) leaves the trace hash, cycle count and registers unchanged. This shows the flags work for the smoke program only; it is not a proof for every program. | `Smoke.RandomResetRequestDoesNotChangeTrace` |
| V-06 | Run-to-run determinism | **Observed.** Two separate `cutoff doctor` processes print the same trace hash; two in-process runs match. | `Smoke.TwoRunsGiveIdenticalTraces`; doctor output |
| V-07 | Sanitizers on this machine | **Apple clang's ASan aborts at startup** (`sanitizer_malloc_mac.inc:189 asan_init_is_running`), even on a one-line hello-world. Homebrew LLVM clang's ASan works. The `asan-macos` preset uses it. Cause not investigated. | Reproduced with a trivial program; 9/9 tests pass under `asan-macos` |
| V-08 | Build warnings | Cutoff's own sources: **0 warnings** under `-Wall -Wextra` in debug, release and `asan-macos`. One warning remains in the `asan-macos` configuration, from compiling GoogleTest's own library with LLVM 23 (`gtest-printers.h:528`, `-Wcharacter-conversion`). It is third-party code and is not suppressed. | Build logs |
| V-09 | Bus transfers per smoke program | The smoke program takes 34 cycles end to end with the 1-wait-cycle memory slave (D-005). Not a performance measurement. | `cutoff doctor` |
| V-10 | clang-tidy | `scripts/tidy.sh` is clean on all 5 Cutoff source files with the checks in `.clang-tidy`. One waiver: `popen` in `cli/doctor.cpp` (fixed literal commands only). | `scripts/tidy.sh build/debug` |

## Still open from the spec's list

Alignment rule for jump targets without compressed instructions (Step 2/3); Chao-type and S32 estimator forms (Step 5 onward); activation-filter soundness (Step 7); SP-code coverage-dump cost (Step 12); the novelty literature check (before any claim of novelty; none is made).
