#pragma once
// results/<run-id>/: config.toml, events.bin, summary.json and failures/.

#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "campaign.hpp"
#include "campaign_config.hpp"

namespace cutoff::engine {

struct Totals {
  std::uint64_t programs = 0;
  std::uint64_t failures = 0;
  std::array<std::uint64_t, 7> by_verdict{};  // indexed by Verdict
  std::uint64_t core_cycles = 0;
  std::uint64_t model_instructions = 0;
  std::uint32_t failure_dirs_written = 0;
};

class ResultsWriter : public RecordSink {
 public:
  // Creates `run_dir` (which must not already exist), writes config.toml and the events header.
  static std::unique_ptr<ResultsWriter> create(const CampaignConfig& cfg, const std::string& run_dir, std::string& error);

  void on_record(const EventRecord& rec) override;

  struct Timing {
    std::string started_utc;
    double wall_seconds = 0;
  };
  // Flushes events.bin and writes summary.json. `exit_code` and `stop_reason` are recorded in the summary.
  bool finish(const std::string& run_id, const Timing& timing, std::uint32_t workers, int exit_code,
              const std::string& stop_reason, std::string& error);

  const Totals& totals() const { return totals_; }

 private:
  ResultsWriter(const CampaignConfig& cfg, std::string run_dir) : cfg_(cfg), dir_(std::move(run_dir)) {}
  void write_failure_dir(const EventRecord& rec);

  CampaignConfig cfg_;
  std::string dir_;
  std::ofstream events_;
  Totals totals_;
};

// FNV-1a of the resolved config text, shown in summary.json so two runs can be told to have the same settings.
std::uint64_t config_hash(const CampaignConfig& cfg);

}  // namespace cutoff::engine
