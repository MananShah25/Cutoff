#include "sim_infect.hpp"

#include "sim_run.hpp"

// Verilator's own headers are not clean under -Wall -Wextra; keep them contained here.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
#include "Vcutoff_infect.h"
#include "Vcutoff_infect___024root.h"
#include "verilated.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace cutoff {

namespace {

struct InfectTraits {
  std::vector<std::uint32_t> words;
  void select(Vcutoff_infect&, std::uint32_t) {}
  // The infected output is a wide port; collect every set bit after each evaluation (once set, a bit stays set).
  void sample(Vcutoff_infect& top) {
    if (words.empty()) words.assign(sizeof(top.infected) / sizeof(std::uint32_t), 0u);
    for (std::size_t w = 0; w < words.size(); ++w) words[w] |= top.infected[w];
  }
  std::uint32_t reg(Vcutoff_infect& top, std::size_t i) { return top.rootp->cutoff_core_infect__DOT__cpu__DOT__cpuregs[i]; }
};

}  // namespace

InfectedRun run_program_infected(std::span<const std::uint32_t> code, const RunConfig& cfg) {
  InfectTraits traits;
  InfectedRun out;
  out.run = detail::run_program_t<Vcutoff_infect, InfectTraits>(code, cfg, 0, traits);
  out.words = std::move(traits.words);
  return out;
}

}  // namespace cutoff
