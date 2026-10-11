#pragma once
// The golden core with "infected" outputs: bit K of `infected` is set at a point of the run where the statement owning
// mutant K's expression executes and the expression would evaluate differently under mutant K (see drill/mutator.hpp).
// Its behaviour is exactly that of the original core; the run also reports which mutants the program infected.

#include <cstdint>
#include <span>
#include <vector>

#include "sim_core.hpp"

namespace cutoff {

struct InfectedRun {
  RunResult run;
  std::vector<std::uint32_t> words;  // bit K of the vector (word K/32, bit K%32) = mutant K was infected at least once; bit 0 unused
  bool infected(int mutant) const { return (words[static_cast<std::size_t>(mutant) / 32] >> (mutant % 32)) & 1u; }
};

InfectedRun run_program_infected(std::span<const std::uint32_t> code, const RunConfig& cfg = {});

}  // namespace cutoff
