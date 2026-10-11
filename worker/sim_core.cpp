#include "sim_core.hpp"
#include "sim_run.hpp"

#include <memory>
#include <vector>

// Verilator's own headers are not clean under -Wall -Wextra; keep them contained here.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
#include "Vcutoff_core.h"
#include "Vcutoff_core___024root.h"
#include "verilated.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace cutoff {

namespace {

struct OriginalTraits {
  static void select(Vcutoff_core&, std::uint32_t) {}
  static std::uint32_t reg(Vcutoff_core& top, std::size_t i) { return top.rootp->cutoff_core__DOT__cpu__DOT__cpuregs[i]; }
};

}  // namespace

void set_runtime_random_reset(int mode, unsigned seed) {
  Verilated::randReset(mode);
  Verilated::randSeed(static_cast<int>(seed));
}

std::string verilator_version() { return Verilated::productVersion(); }

RunResult run_program(std::span<const std::uint32_t> code, const RunConfig& cfg) {
  return detail::run_program_t<Vcutoff_core, OriginalTraits>(code, cfg, 0);
}

}  // namespace cutoff
