#pragma once
// Owns the species trackers, the stop rule and the risk-curve file of a run.

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "campaign_config.hpp"
#include "checkpoint.hpp"
#include "estimators.hpp"
#include "species.hpp"
#include "stop_rule.hpp"
#include "tracker.hpp"

namespace cutoff::engine {

struct SetSummary {
  std::string name;
  std::size_t universe = 0;
  risk::Counts counts;
  double good_turing = 0, incidence = 0, coverage = 0, chao2 = 0, chao2_bias_corrected = 0;
  std::uint64_t programs_since_new = 0;
};

class RiskMonitor {
 public:
  explicit RiskMonitor(const CampaignConfig& cfg);

  // risk_curve.csv: created with a header, or (resume) cut back to `keep_bytes` and appended to.
  bool open_curve(const std::string& path, bool resume, std::uint64_t keep_bytes, std::string& error);

  bool tracking() const { return !trackers_.empty(); }
  void add_program(const risk::SpeciesIds& species);  // call once per delivered program, in order
  std::uint64_t programs() const { return n_; }
  bool at_checkpoint() const { return rule_.at_checkpoint(n_) && n_ > 0; }

  // Evaluates the risk at the current program count: appends one curve row per set, feeds the stop rule and the
  // plateau baseline. Returns the risk the rule used (the maximum over the sets).
  double evaluate();
  bool should_stop() const { return rule_.should_stop(); }
  const risk::StopRule& rule() const { return rule_; }
  std::uint64_t plateau_stop_at() const { return plateau_stop_at_; }

  std::uint64_t curve_bytes();  // flushes first
  std::vector<SetSummary> summary() const;
  double current_risk() const;  // same quantity as evaluate() returns, without side effects

  void export_state(Checkpoint& cp) const;
  bool import_state(const Checkpoint& cp, std::string& error);

 private:
  struct Tracked {
    risk::SpeciesSet set;
    risk::SpeciesTracker tracker;
  };
  CampaignConfig cfg_;
  risk::StopRule rule_;
  std::vector<Tracked> trackers_;
  std::uint64_t n_ = 0;
  std::uint64_t plateau_stop_at_ = 0;
  std::ofstream curve_;
};

}  // namespace cutoff::engine
