#pragma once
// One complete campaign run: results folder, orchestrator, summary, exit code.

#include <cstdint>
#include <iosfwd>
#include <string>

#include "campaign_config.hpp"

namespace cutoff::engine {

// Exit codes (spec section 5): 0 finished with no failure, 1 at least one failure, 3 setup or environment error.
// Code 2 (budget ended before the risk threshold) arrives with the stop rule in Step 5.
struct RunOutcome {
  int exit_code = 3;
  std::string run_dir;
  std::uint64_t programs = 0;
  std::uint64_t failures = 0;
  double wall_seconds = 0;
};

RunOutcome execute_run(const CampaignConfig& cfg, const std::string& run_id, bool show_progress, std::ostream& out, std::ostream& err);

std::string default_run_id(const CampaignConfig& cfg);

}  // namespace cutoff::engine
