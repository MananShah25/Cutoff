#include "check.hpp"

#include <algorithm>

#include "drill_levels.hpp"
#include "generator.hpp"
#include "sim_drill.hpp"

namespace cutoff::engine {

std::uint64_t hash_regs(const std::array<std::uint32_t, 32>& regs) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const std::uint32_t r : regs)
    for (unsigned b = 0; b < 4; ++b) {
      h ^= (r >> (8 * b)) & 0xffu;
      h *= 0x100000001b3ULL;
    }
  return h;
}

CheckOutcome check_c1(const ModelResult& model, const RunResult& core) {
  if (model.status != Status::Exited) return {Verdict::GeneratorFault, 0};
  if (core.status == Status::Timeout) return {Verdict::Hang, 0};
  if (core.status != model.status) return {Verdict::StatusMismatch, 0};
  if (core.exit_code != model.exit_code) return {Verdict::ExitCodeMismatch, 0};
  for (std::uint8_t i = 1; i < 32; ++i)
    if (core.regs[i] != model.regs[i]) return {Verdict::RegMismatch, i};
  if (core.mem_hash != model.mem_hash) return {Verdict::MemMismatch, 0};
  return {};
}

Execution execute_program(const CampaignConfig& cfg, std::uint64_t program_id) {
  Execution e;
  const std::uint64_t seed = gen::program_seed(cfg.seed, program_id);
  e.program = gen::generate(cfg.gen, seed);
  const bool want_species = cfg.species.any();
  e.model = run_model(e.program.code, {cfg.max_instructions, want_species});
  if (want_species && e.model.status == Status::Exited) e.species = risk::extract_species(e.model.retired, cfg.species);
  if (cfg.drill) {  // the golden run of a drill is the infect model, which behaves exactly like the original core
    InfectedRun g = run_program_infected(e.program.code, {cfg.max_cycles});
    e.core = g.run;
    e.infected = std::move(g.words);
  } else {
    e.core = cfg.mutant < 0 ? run_program(e.program.code, {cfg.max_cycles})
                            : run_program_mutant(e.program.code, static_cast<std::uint32_t>(cfg.mutant), {cfg.max_cycles});
  }

  // Test hook (debug.corrupt_core_every): fake a core bug so the failure path can be exercised end to end.
  if (cfg.corrupt_core_every != 0 && (program_id + 1) % cfg.corrupt_core_every == 0) e.core.regs[5] ^= 1u;
  return e;
}

// One level byte per selected mutant for this program (see drill_levels.hpp).
std::vector<std::uint8_t> drill_levels(const CampaignConfig& cfg, const Execution& golden) {
  std::vector<std::uint8_t> levels(cfg.drill_ids.size(), 0);
  // A mutant that runs far past the golden run's length is hung; the cycle limit keeps such runs cheap.
  const std::uint64_t limit = std::min<std::uint64_t>(cfg.max_cycles, 8 * golden.core.cycles + 2000);
  for (std::size_t j = 0; j < cfg.drill_ids.size(); ++j) {
    const int id = cfg.drill_ids[j];
    const bool infected = ((golden.infected[static_cast<std::size_t>(id) / 32] >> (id % 32)) & 1u) != 0;
    std::uint8_t b = infected ? kLevelInfected : 0;
    if (infected || !cfg.drill_filter) {
      b |= kLevelRan;
      const RunResult m = run_program_mutant(golden.program.code, static_cast<std::uint32_t>(id), {limit});
      const RunResult& g = golden.core;
      if (m.status != g.status || m.exit_code != g.exit_code || m.regs != g.regs || m.mem_hash != g.mem_hash || m.bus_hash != g.bus_hash) b |= kLevelPropagated;
      if (golden.model.status == Status::Exited && check_c1(golden.model, m).verdict != Verdict::Pass) b |= kLevelDetected;
    }
    levels[j] = b;
  }
  return levels;
}

EventRecord evaluate_program(const CampaignConfig& cfg, std::uint64_t program_id) { return evaluate(cfg, program_id).rec; }

ProgramResult evaluate(const CampaignConfig& cfg, std::uint64_t program_id) {
  Execution e = execute_program(cfg, program_id);
  const ModelResult& model = e.model;
  const RunResult& core = e.core;
  const CheckOutcome outcome = check_c1(model, core);
  EventRecord rec;
  rec.program_id = program_id;
  rec.seed = e.program.seed;
  rec.program_hash = gen::program_hash(e.program);
  rec.model_reg_hash = hash_regs(model.regs);
  rec.model_mem_hash = model.mem_hash;
  rec.core_mem_hash = core.mem_hash;
  rec.core_cycles = core.cycles;
  rec.model_instret = static_cast<std::uint32_t>(std::min<std::uint64_t>(model.instret, UINT32_MAX));
  rec.model_exit_code = model.exit_code;
  rec.core_exit_code = core.exit_code;
  rec.model_status = static_cast<std::uint8_t>(model.status);
  rec.core_status = static_cast<std::uint8_t>(core.status);
  rec.verdict = static_cast<std::uint8_t>(outcome.verdict);
  rec.detail = outcome.detail;
  ProgramResult result;
  result.rec = rec;
  result.species = std::move(e.species);
  if (cfg.drill) result.drill = drill_levels(cfg, e);
  return result;
}

}  // namespace cutoff::engine
