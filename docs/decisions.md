# Decisions log

Every deviation from, or choice not fixed by, the build spec. Newest last.

| ID | Date | Decision | Reason |
| --- | --- | --- | --- |
| D-001 | 2026-10-10 | Cutoff is its own git repository (`main` branch) inside `~/GithubRepos/Cutoff`. The user's home directory is itself a git repository with no commits; Cutoff is not part of it. | Spec asks for small meaningful commits on `main`. Setup note: early setup commands were run before this repo existed and so hit the home repository by mistake; the branch name, index, `.gitmodules` and submodule config there were restored. |
| D-002 | 2026-10-10 | Develop with Apple clang 16 (C++20). clang-tidy and sanitizer builds use Homebrew LLVM (`asan-macos` preset, `scripts/tidy.sh`). | Apple clang ships no clang-tidy, and its ASan runtime aborts at startup on this machine (verification log V-07). |
| D-003 | 2026-10-10 | GoogleTest 1.17.0 fetched by CMake with a pinned SHA-256. | Reproducible without a system install. |
| D-004 | 2026-10-10 | The RV32I configuration of PicoRV32 lives in our wrapper `rtl/cutoff_core.v`; `rtl/picorv32/picorv32.v` is the unmodified submodule, pinned at `ef203c2` (2026-09-07). Parameters changed from the core's defaults: `ENABLE_COUNTERS=0`, `ENABLE_COUNTERS64=0`, `REGS_INIT_ZERO=1`. All others equal the defaults listed in V-01. | Spec section 7: plain RV32I, counters trap by design. The wrapper is not a change to the core under test. |
| D-005 | 2026-10-10 | Memory map for the testbench: 64 KiB flat RAM at address 0 (the core's reset address); a store to `0x1000_0000` ends the program and its data word is the exit code; any other access outside RAM is reported as a memory fault; default cycle limit 100,000. The memory slave answers every transfer after one wait cycle (about two cycles per transfer). | Spec section 7 leaves exact values to us. Must be mirrored by the reference model in Step 2. |
| D-006 | 2026-10-10 | `cutoff doctor` requires at least 5 GiB of free disk where it is run; a missing CMake is only a warning. | Result folders and pools grow in later steps; CMake is only needed to rebuild. |
| D-007 | 2026-10-10 | Initial state is pinned at compile time (`--x-initial 0 --x-assign 0`) and by `REGS_INIT_ZERO=1`. | Determinism (spec rule 4); proven by test V-05. |
| D-008 | 2026-10-10 | `-Wall -Wextra` apply to Cutoff's own sources only, not to Verilator's generated model or GoogleTest. | The generated code is third-party. Our code must stay warning-free under all three configurations. |
| D-009 | 2026-10-10 | The CI workflow is written but has not been run on GitHub (there is no remote). | Spec lists CI with sanitizers; it cannot be validated locally. Treat as untested until a remote exists. |
