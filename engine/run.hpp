#pragma once
// One complete campaign run: results folder, orchestrator, risk monitor, stop rule, checkpoints, summary, exit code.

#include <cstdint>
#include <iosfwd>
#include <string>

#include "campaign_config.hpp"

namespace cutoff::engine {

// Exit codes (spec section 5):
//   0    stopped at the risk threshold (or, with the stop rule off, ran every program) with no failure
//   1    at least one failure
//   2    a budget (programs or hours) ended before the risk threshold was reached
//   3    setup or environment error
//   130  interrupted (Ctrl-C or SIGTERM); the run folder holds a checkpoint and can be continued with --resume
struct RunOutcome {
  int exit_code = 3;
  std::string run_dir;
  std::uint64_t programs = 0;
  std::uint64_t failures = 0;
  double wall_seconds = 0;
  std::string stop_reason;  // threshold | budget | program-count | time-budget | interrupted | error
};

// Starts a new run in <cfg.results_dir>/<run_id>, or, when `resume_dir` is non-empty, continues the run in that folder
// from its checkpoint (cfg then holds the saved settings plus any overrides).
RunOutcome execute_run(const CampaignConfig& cfg, const std::string& run_id, bool show_progress, std::ostream& out, std::ostream& err,
                       const std::string& resume_dir = {});

std::string default_run_id(const CampaignConfig& cfg);

}  // namespace cutoff::engine
