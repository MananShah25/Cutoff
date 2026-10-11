#pragma once
// Every setting of a campaign. cutoff.toml, command-line overrides and the resolved copy written into each
// results folder are all views of this one struct.

#include <cstdint>
#include <string>
#include <vector>

#include "generator.hpp"
#include "species.hpp"
#include "stop_rule.hpp"
#include "toml_lite.hpp"

namespace cutoff::engine {

struct CampaignConfig {
  // [core]
  std::string core_name = "picorv32";
  // -1: the original core. 0: the instrumented core with no mutation selected (must behave exactly like the original).
  // N >= 1: the instrumented core with mutant N selected (see `cutoff build --drill --list`).
  std::int64_t mutant = -1;
  // [campaign]
  std::uint64_t seed = 1;
  std::uint64_t programs = 1000;      // budget: at most this many programs (ids 0 .. programs-1)
  std::uint32_t workers = 0;     // 0 = one per hardware thread
  std::uint32_t batch_size = 64;
  // [generator]
  gen::GenConfig gen;
  // [checker]
  std::string checker = "c1";
  std::uint64_t max_cycles = 200'000;       // core cycle limit; hitting it is a Hang
  std::uint64_t max_instructions = 100'000; // model instruction limit
  // [species]  which species sets the estimators track
  risk::SpeciesConfig species;
  // [stop]  rule off while epsilon == 0; the other defaults are provisional until the benchmark (Step 11) chooses them
  risk::StopConfig stop;
  double max_hours = 0.0;                   // wall-clock budget across all sessions of a run; 0 = none
  // [drill]  `cutoff drill` also runs planted bugs (mutants) of the instrumented core against every program
  bool drill = false;
  std::uint32_t drill_mutants = 0;       // 0 = every eligible mutant, otherwise a random sample of this size
  std::int64_t drill_sample_seed = -1;   // seed of that sample; -1 = the campaign seed
  std::string drill_operators;           // path of an operators file (see engine/drill_select.hpp); empty = every operator
  bool drill_filter = true;              // activation filter: simulate a mutant only on programs that infect it
  std::vector<int> drill_ids;            // the resolved selection (filled by execute_run; not a setting)
  // [output]
  std::string results_dir = "results";
  std::uint32_t max_failure_dirs = 20;
  // [debug]  test hooks, off by default
  std::uint32_t corrupt_core_every = 0;     // flip a bit of x5 in the core result of every n-th program
  std::int64_t crash_worker_at = -1;        // the worker that gets this program id exits abruptly
  std::uint64_t interrupt_after = 0;        // behave as if Ctrl-C arrived once this many programs are done (0 = never)
};

// Applies the settings found in `doc` onto `cfg`. Returns an error message (empty on success) that names the
// offending key and, for unknown keys, lists the valid ones.
std::string apply_toml(CampaignConfig& cfg, const TomlDoc& doc, const std::string& source_name);

// Empty if usable, otherwise what to change.
std::string validate(const CampaignConfig& cfg);

// Canonical text of the full resolved configuration (this is what results/<run>/config.toml contains).
std::string to_toml(const CampaignConfig& cfg);

std::uint32_t resolved_workers(const CampaignConfig& cfg);

// Hash of the settings that decide what each program's result and species are (core, seed, generator, checker
// limits, species sets, debug corruption). Budgets, stop-rule settings and worker counts are excluded, so a run can be
// resumed with a larger budget or another threshold, but never with a different program stream.
std::uint64_t stream_hash(const CampaignConfig& cfg);

}  // namespace cutoff::engine
