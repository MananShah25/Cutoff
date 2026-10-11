#pragma once
// Checker C1 (final registers + hash of memory + status + exit code) and the per-program evaluation
// that workers run.

#include <cstdint>

#include "campaign_config.hpp"
#include "generator.hpp"
#include "model.hpp"
#include "protocol.hpp"
#include "sim_core.hpp"

namespace cutoff::engine {

struct CheckOutcome {
  Verdict verdict = Verdict::Pass;
  std::uint8_t detail = 0;  // register index for RegMismatch
};

// Precedence: a model that did not exit cleanly is a GeneratorFault (nothing can be concluded about the core);
// then a core Timeout is a Hang; then status, exit code, registers, memory.
CheckOutcome check_c1(const ModelResult& model, const RunResult& core);

// Everything one program produces; failure reports need the full results, not just the compact record.
struct Execution {
  gen::Program program;
  ModelResult model;
  RunResult core;
  risk::SpeciesIds species;  // empty unless the campaign tracks species
};

// Generates program `program_id` of the campaign and runs it on the model and on the core.
Execution execute_program(const CampaignConfig& cfg, std::uint64_t program_id);

// execute_program + check_c1: the compact record for events.bin plus the species for the estimators.
ProgramResult evaluate(const CampaignConfig& cfg, std::uint64_t program_id);

// The record alone (convenience for tests and tools).
EventRecord evaluate_program(const CampaignConfig& cfg, std::uint64_t program_id);

std::uint64_t hash_regs(const std::array<std::uint32_t, 32>& regs);

}  // namespace cutoff::engine
