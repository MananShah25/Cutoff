#pragma once
// results/<run-id>/: config.toml, events.bin, risk_curve.csv, checkpoint.bin, summary.json and failures/.

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "campaign_config.hpp"
#include "checkpoint.hpp"
#include "protocol.hpp"
#include "risk_monitor.hpp"
#include "totals.hpp"

namespace cutoff::engine {

// Everything summary.json reports beyond the totals.
struct SummaryInput {
  std::string run_id;
  std::string started_utc;
  double wall_seconds = 0;          // all sessions together
  std::uint32_t workers = 0;
  std::uint32_t sessions = 1;       // 1 + number of resumes
  int exit_code = 0;
  std::string stop_reason;          // threshold | budget | program-count | time-budget | error
  std::uint64_t programs_at_stop = 0;
  std::vector<SetSummary> sets;
  double risk_used = 0;             // the value the stop rule compares with epsilon
  std::uint64_t plateau_stop_at = 0;
  std::uint32_t stop_consecutive = 0;
  std::string drill_json;           // the "drill" object for summary.json (empty outside a drill)
};

class ResultsWriter {
 public:
  // New run: creates `run_dir` (which must not exist), writes config.toml and the events header.
  static std::unique_ptr<ResultsWriter> create(const CampaignConfig& cfg, const std::string& run_dir, std::string& error);
  // Continues a run: cuts events.bin back to the checkpoint, removes failure folders of programs that will be re-run,
  // restores the totals and rewrites config.toml with the current settings.
  static std::unique_ptr<ResultsWriter> resume(const CampaignConfig& cfg, const std::string& run_dir, const Checkpoint& cp, std::string& error);

  void append(const EventRecord& rec);  // events.bin, totals, and (for the first max_failure_dirs failures) a failure folder
  bool flush(std::string& error);
  std::uint64_t events_bytes();         // after flush
  const Totals& totals() const { return totals_; }
  const std::string& dir() const { return dir_; }

  bool write_summary(const SummaryInput& in, std::string& error);
  // A file next to summary.json (e.g. drill.tsv).
  bool write_aux(const std::string& name, const std::string& text, std::string& error);

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
