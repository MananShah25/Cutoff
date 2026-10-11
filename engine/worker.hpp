#pragma once

#include "campaign_config.hpp"

namespace cutoff::engine {

// The body of a worker process: reads Tasks from `in_fd`, evaluates each program and writes the EventRecords
// to `out_fd`, until a Task with count 0 (or EOF) arrives. Never returns; ends with _exit.
[[noreturn]] void run_worker(const CampaignConfig& cfg, int in_fd, int out_fd);

}  // namespace cutoff::engine
