#pragma once
// The stop rule (spec section 9, [decision]):
//  1. no stop before min_programs;
//  2. every `checkpoint` programs the risk is evaluated;
//  3. stop when the risk has been below epsilon for `confirm` consecutive checkpoints at or after min_programs;
//  4. the caller stops anyway at its program or time budget.
// epsilon == 0 switches the rule off. The defaults are provisional until the benchmark (Step 11) chooses them.

#include <cstdint>

#include "estimators.hpp"

namespace cutoff::risk {

struct StopConfig {
  double epsilon = 0.0;            // 0 = rule off
  std::uint32_t confirm = 3;
  std::uint64_t min_programs = 1000;
  std::uint64_t checkpoint = 1000;
  Estimator estimator = Estimator::GoodTuring;
  std::uint64_t plateau_window = 5000;  // baseline only: "no new species in the last W programs"
};

class StopRule {
 public:
  explicit StopRule(const StopConfig& cfg) : cfg_(cfg) {}

  bool enabled() const { return cfg_.epsilon > 0.0; }
  bool at_checkpoint(std::uint64_t n) const { return cfg_.checkpoint > 0 && n % cfg_.checkpoint == 0; }

  // Feed the risk (the maximum over the enabled species sets) measured at checkpoint n.
  void on_checkpoint(std::uint64_t n, double risk);

  bool should_stop() const { return enabled() && consecutive_ >= cfg_.confirm; }
  std::uint32_t consecutive() const { return consecutive_; }
  void restore(std::uint32_t consecutive) { consecutive_ = consecutive; }
  const StopConfig& config() const { return cfg_; }

 private:
  StopConfig cfg_;
  std::uint32_t consecutive_ = 0;
};

}  // namespace cutoff::risk
