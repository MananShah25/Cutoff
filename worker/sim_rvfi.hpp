#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "sim_core.hpp"

namespace cutoff {

struct RvfiRecord { std::uint32_t pc = 0, insn = 0; std::uint8_t rd = 0; std::uint32_t rd_value = 0; };
struct RvfiRun { RunResult run; std::vector<RvfiRecord> retired; };
RvfiRun run_program_rvfi(std::span<const std::uint32_t> code, const RunConfig& cfg = {});

}  // namespace cutoff
