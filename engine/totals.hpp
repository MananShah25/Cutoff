#pragma once

#include <array>
#include <cstdint>

namespace cutoff::engine {

struct Totals {
  std::uint64_t programs = 0;
  std::uint64_t failures = 0;
  std::array<std::uint64_t, 9> by_verdict{};  // indexed by Verdict
  std::uint64_t core_cycles = 0;
  std::uint64_t model_instructions = 0;
  std::uint32_t failure_dirs_written = 0;
};

}  // namespace cutoff::engine
