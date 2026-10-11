#pragma once
// The testbench memory map (docs/decisions.md D-005), shared by the reference model and the
// Verilated worker so the two cannot drift apart.

#include <cstdint>
#include <span>

namespace cutoff {

inline constexpr std::uint32_t kRamBytes = 64 * 1024;   // flat RAM at address 0, which is also the reset address
inline constexpr std::uint32_t kMagicAddr = 0x1000'0000;  // a store to this word ends the program

enum class Status : std::uint8_t {
  Running,   // model only: the program has not finished
  Exited,    // magic store reached; exit_code holds its data word
  Trap,      // illegal instruction, misaligned access or misaligned jump target
  Timeout,   // instruction or cycle limit reached
  MemFault,  // access outside RAM that is not the magic store
};

constexpr const char* to_string(Status s) {
  switch (s) {
    case Status::Running: return "running";
    case Status::Exited: return "exited";
    case Status::Trap: return "trap";
    case Status::Timeout: return "timeout";
    case Status::MemFault: return "memfault";
  }
  return "?";
}

// 64-bit FNV-1a over the RAM bytes: the "hash of memory" compared by checker C1.
constexpr std::uint64_t hash_ram(std::span<const std::uint8_t> ram) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const std::uint8_t b : ram) {
    h ^= b;
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace cutoff
