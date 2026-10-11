#pragma once
// Aggregates the per-program drill bytes into the funnel of every selected mutant.

#include <cstdint>
#include <span>
#include <vector>

namespace cutoff::engine {

inline constexpr std::uint64_t kNever = UINT64_MAX;

struct MutantTally {
  std::uint64_t ran = 0, infected = 0, propagated = 0, detected = 0;                       // programs at each level
  std::uint64_t first_infected = kNever, first_propagated = kNever, first_detected = kNever;  // program ids
};

enum class Outcome : unsigned char {
  Killed,                 // detected by the checker on at least one program
  NeverInfected,          // no program reached and changed the mutated expression: a stimulus gap (or dead logic)
  InfectedNotPropagated,  // reached and changed, but the effect never left the expression: masking, or an equivalent mutant
  PropagatedNotDetected,  // changed the run, but checker C1 did not flag it: a checker gap
};
const char* to_string(Outcome o);

class DrillTally {
 public:
  DrillTally() = default;
  explicit DrillTally(std::vector<int> mutant_ids);

  // `levels[j]` is the byte for mutant_ids()[j] on program `program_id` (see drill_levels.hpp).
  void observe(std::uint64_t program_id, std::span<const std::uint8_t> levels);

  const std::vector<int>& ids() const { return ids_; }
  const std::vector<MutantTally>& mutants() const { return m_; }
  std::uint64_t programs() const { return programs_; }
  std::uint64_t pairs_skipped() const { return skipped_; }        // (program, mutant) pairs the activation filter did not simulate
  std::uint64_t pairs_total() const { return programs_ * ids_.size(); }
  // Soundness counters; both must stay 0. They can only be non-zero when the filter is off (every pair simulated).
  std::uint64_t propagated_without_infection() const { return violations_; }
  std::uint64_t detected_without_propagation() const { return inconsistent_; }

  static Outcome outcome(const MutantTally& t);

  std::vector<std::uint64_t> to_words() const;
  static DrillTally from_words(std::vector<int> ids, const std::vector<std::uint64_t>& words, bool& ok);

 private:
  std::vector<int> ids_;
  std::vector<MutantTally> m_;
  std::uint64_t programs_ = 0, skipped_ = 0, violations_ = 0, inconsistent_ = 0;
};

}  // namespace cutoff::engine
