#include <gtest/gtest.h>

#include <array>

#include "sim_core.hpp"
#include "smoke_programs.hpp"

using namespace cutoff;

TEST(Smoke, AddProgramExitsWithSum) {
  const RunResult r = run_program(smoke::kAdd);
  EXPECT_EQ(r.status, Status::Exited);
  EXPECT_EQ(r.exit_code, 12u);
  EXPECT_EQ(r.regs[0], 0u);
  EXPECT_EQ(r.regs[1], 5u);
  EXPECT_EQ(r.regs[2], 7u);
  EXPECT_EQ(r.regs[3], 12u);
  EXPECT_EQ(r.regs[4], 0x1000'0000u);
}

TEST(Smoke, TwoRunsGiveIdenticalTraces) {
  const RunResult a = run_program(smoke::kAdd);
  const RunResult b = run_program(smoke::kAdd);
  EXPECT_EQ(a.trace_hash, b.trace_hash);
  EXPECT_EQ(a.cycles, b.cycles);
  EXPECT_EQ(a.regs, b.regs);
}

TEST(Smoke, DifferentProgramsGiveDifferentTraces) {
  EXPECT_NE(run_program(smoke::kAdd).trace_hash, run_program(smoke::kJalrOdd).trace_hash);
}

// Verification-log item: is the historical PicoRV32 JALR low-bit issue present in the pinned core?
TEST(Smoke, JalrClearsLowBitOfTarget) {
  const RunResult r = run_program(smoke::kJalrOdd);
  EXPECT_EQ(r.status, Status::Exited);
  EXPECT_EQ(r.exit_code, 8u);  // link register = address after the jalr
  EXPECT_EQ(r.regs[5], 0u);    // skipped instruction did not run
}

TEST(Smoke, InfiniteLoopTimesOut) {
  const RunResult r = run_program(std::array<std::uint32_t, 1>{0x0000006f}, RunConfig{2000});
  EXPECT_EQ(r.status, Status::Timeout);
}

TEST(Smoke, IllegalInstructionTraps) {
  const RunResult r = run_program(std::array<std::uint32_t, 1>{0xffffffff});
  EXPECT_EQ(r.status, Status::Trap);
}

TEST(Smoke, WildAccessIsMemFault) {
  // lui x1, 0x20000 ; lw x2, 0(x1)
  const RunResult r = run_program(std::array<std::uint32_t, 2>{0x200000b7, 0x0000a103});
  EXPECT_EQ(r.status, Status::MemFault);
}

// Verification-log item: Verilator is two-state; the build pins initial values with
// --x-initial 0 / --x-assign 0, so asking the runtime for random initialisation must change nothing.
TEST(Smoke, RandomResetRequestDoesNotChangeTrace) {
  const RunResult base = run_program(smoke::kAdd);
  set_runtime_random_reset(2, 12345);
  const RunResult randomized = run_program(smoke::kAdd);
  set_runtime_random_reset(0, 0);
  EXPECT_EQ(base.trace_hash, randomized.trace_hash);
  EXPECT_EQ(base.cycles, randomized.cycles);
  EXPECT_EQ(base.regs, randomized.regs);
}

// The core raises a bus request before its trap output for a misaligned fetch or store, so a run that
// ends on a bus event must wait briefly for a trap (RunConfig::trap_settle_cycles). Measured on 2026-10-10:
// jal to 0xfffffffe reports MemFault with no wait and Trap from one cycle on.
TEST(Smoke, TrapPreemptsABusEventEndingTheRun) {
  const std::array<std::uint32_t, 1> jal_misaligned{0xfffff2ef};  // jal x5, -2
  RunConfig no_wait;
  no_wait.trap_settle_cycles = 0;
  EXPECT_EQ(run_program(jal_misaligned, no_wait).status, Status::MemFault);
  RunConfig one_cycle;
  one_cycle.trap_settle_cycles = 1;
  EXPECT_EQ(run_program(jal_misaligned, one_cycle).status, Status::Trap);
  EXPECT_EQ(run_program(jal_misaligned).status, Status::Trap);
}

TEST(Smoke, LongSettleWindowDoesNotTurnALegitimateExitIntoATrap) {
  // sw to the magic address is followed by an all-zero (illegal) word. The testbench stops answering the core
  // once the store is seen, so the core never reaches that word however long the window is.
  RunConfig cfg;
  cfg.trap_settle_cycles = 64;
  const RunResult r = run_program(smoke::kAdd, cfg);
  EXPECT_EQ(r.status, Status::Exited);
  EXPECT_EQ(r.exit_code, 12u);
}
