#include "sim_drill.hpp"

#include "sim_run.hpp"

// Verilator's own headers are not clean under -Wall -Wextra; keep them contained here.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
#include "Vcutoff_drill.h"
#include "Vcutoff_drill___024root.h"
#include "verilated.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace cutoff {

namespace detail {
extern const std::string_view kDrillManifest;
extern const std::string_view kDrillSummary;
extern const int kDrillMutants;
}  // namespace detail

int drill_mutant_count() { return detail::kDrillMutants; }
std::string_view drill_manifest_tsv() { return detail::kDrillManifest; }
std::string_view drill_summary() { return detail::kDrillSummary; }

namespace {

struct DrillTraits {
  static void select(Vcutoff_drill& top, std::uint32_t mutant) { top.mut_sel = static_cast<std::uint16_t>(mutant); }
  static std::uint32_t reg(Vcutoff_drill& top, std::size_t i) { return top.rootp->cutoff_core_drill__DOT__cpu__DOT__cpuregs[i]; }
};

}  // namespace

RunResult run_program_mutant(std::span<const std::uint32_t> code, std::uint32_t mutant, const RunConfig& cfg) {
  return detail::run_program_t<Vcutoff_drill, DrillTraits>(code, cfg, mutant);
}

}  // namespace cutoff
