#pragma once
// Verilated PicoRV32 plus a flat-RAM memory model on its native valid/ready bus.

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace cutoff {

inline constexpr std::uint32_t kRamBytes = 64 * 1024;
inline constexpr std::uint32_t kMagicAddr = 0x1000'0000;  // a store here ends the program

struct RunConfig {
  std::uint64_t max_cycles = 100'000;
};

enum class Status : std::uint8_t { Exited, Trap, Timeout, MemFault };

struct RunResult {
  Status status = Status::Timeout;
  std::uint32_t exit_code = 0;   // data of the magic store
  std::uint64_t cycles = 0;
  std::uint64_t trace_hash = 0;  // FNV-1a over every accepted bus transfer
  std::array<std::uint32_t, 32> regs{};
};

const char* to_string(Status s);

// Runs `code` (loaded at address 0, the core's reset address) to completion.
RunResult run_program(std::span<const std::uint32_t> code, const RunConfig& cfg = {});

// Asks the Verilator runtime for random initial values (mode 0 = off, 1 = zero, 2 = random).
// The build pins initial state at compile time, so this must not change any result; tests use it to prove that.
void set_runtime_random_reset(int mode, unsigned seed);

// Version string of the Verilator that built the core, e.g. "5.052".
std::string verilator_version();

}  // namespace cutoff
