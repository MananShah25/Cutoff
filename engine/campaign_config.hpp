#pragma once
// Every setting of a campaign. cutoff.toml, command-line overrides and the resolved copy written into each
// results folder are all views of this one struct.

#include <cstdint>
#include <string>

#include "generator.hpp"
#include "toml_lite.hpp"

namespace cutoff::engine {

struct CampaignConfig {
  // [core]
  std::string core_name = "picorv32";
  // [campaign]
  std::uint64_t seed = 1;
  std::uint64_t programs = 1000;
  std::uint32_t workers = 0;     // 0 = one per hardware thread
  std::uint32_t batch_size = 64;
  // [generator]
  gen::GenConfig gen;
  // [checker]
  std::string checker = "c1";
  std::uint64_t max_cycles = 200'000;       // core cycle limit; hitting it is a Hang
  std::uint64_t max_instructions = 100'000; // model instruction limit
  // [output]
  std::string results_dir = "results";
  std::uint32_t max_failure_dirs = 20;
  // [debug]  test hooks, off by default
  std::uint32_t corrupt_core_every = 0;     // flip a bit of x5 in the core result of every n-th program
  std::int64_t crash_worker_at = -1;        // the worker that gets this program id exits abruptly
};

// Applies the settings found in `doc` onto `cfg`. Returns an error message (empty on success) that names the
// offending key and, for unknown keys, lists the valid ones.
std::string apply_toml(CampaignConfig& cfg, const TomlDoc& doc, const std::string& source_name);

// Empty if usable, otherwise what to change.
std::string validate(const CampaignConfig& cfg);

// Canonical text of the full resolved configuration (this is what results/<run>/config.toml contains).
std::string to_toml(const CampaignConfig& cfg);

std::uint32_t resolved_workers(const CampaignConfig& cfg);

}  // namespace cutoff::engine
