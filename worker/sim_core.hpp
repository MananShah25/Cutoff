#pragma once
// Verilated PicoRV32 plus a flat-RAM memory model on its native valid/ready bus.

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "memory_map.hpp"

namespace cutoff {

struct RunConfig {
  std::uint64_t max_cycles = 100'000;
  // The core raises a bus request one or two cycles before its `trap` output when the access or fetch is
  // misaligned, so a run that ends on a bus event (magic store, out-of-range access) keeps clocking for this
  // many cycles and reports Trap if the core traps meanwhile. Measured in tests/model_vs_core_test.cpp.
  unsigned trap_settle_cycles = 4;
};

struct RunResult {
  Status status = Status::Timeout;
  std::uint32_t exit_code = 0;   // data of the magic store
  std::uint64_t cycles = 0;
  std::uint64_t trace_hash = 0;  // FNV-1a over every accepted bus transfer
  std::array<std::uint32_t, 32> regs{};
};

// Runs `code` (loaded at address 0, the core's reset address) to completion.
RunResult run_program(std::span<const std::uint32_t> code, const RunConfig& cfg = {});

// Asks the Verilator runtime for random initial values (mode 0 = off, 1 = zero, 2 = random).
// The build pins initial state at compile time, so this must not change any result; tests use it to prove that.
void set_runtime_random_reset(int mode, unsigned seed);

// Version string of the Verilator that built the core, e.g. "5.052".
std::string verilator_version();

}  // namespace cutoff
