#pragma once
// The instrumented core (picorv32 with every mutation selectable through mut_sel) behind the same bus and memory model
// as the original. Mutant 0 is the original design; mutants 1..N are listed in the generated manifest.

#include <cstdint>
#include <span>
#include <string_view>

#include "sim_core.hpp"

namespace cutoff {

// Number of mutants (ids 1..N) in the instrumented core, its manifest (TSV: id, site, category, ...) and a summary.
int drill_mutant_count();
std::string_view drill_manifest_tsv();
std::string_view drill_summary();

RunResult run_program_mutant(std::span<const std::uint32_t> code, std::uint32_t mutant, const RunConfig& cfg = {});

}  // namespace cutoff
