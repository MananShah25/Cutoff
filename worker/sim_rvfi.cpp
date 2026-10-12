#include "sim_rvfi.hpp"
#include "sim_run.hpp"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
#include "Vcutoff_rvfi.h"
#include "Vcutoff_rvfi___024root.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
namespace cutoff { namespace {
struct Traits {
  std::vector<RvfiRecord> retired;
  // run_program_t samples after both edges.  RVFI state is committed at the
  // rising edge, so only observe that half of each simulated clock cycle.
  bool after_falling_edge = true;
  void select(Vcutoff_rvfi&, std::uint32_t) {}
  void sample(Vcutoff_rvfi& top) {
    if (after_falling_edge) {
      after_falling_edge = false;
      return;
    }
    after_falling_edge = true;
    if (top.rvfi_valid) {
      retired.push_back({top.rvfi_pc_rdata, top.rvfi_insn, top.rvfi_rd_addr, top.rvfi_rd_wdata});
    }
  }
  std::uint32_t reg(Vcutoff_rvfi& top, std::size_t i) { return top.rootp->cutoff_core__DOT__cpu__DOT__cpuregs[i]; }
}; }
RvfiRun run_program_rvfi(std::span<const std::uint32_t> code, const RunConfig& cfg) { Traits t; RvfiRun r; r.run = detail::run_program_t<Vcutoff_rvfi, Traits>(code, cfg, 0, t); r.retired = std::move(t.retired); return r; }
}  // namespace cutoff
