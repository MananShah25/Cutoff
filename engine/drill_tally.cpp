#include "drill_tally.hpp"

#include "drill_levels.hpp"

namespace cutoff::engine {

const char* to_string(Outcome o) {
  switch (o) {
    case Outcome::Killed: return "killed";
    case Outcome::NeverInfected: return "survivor:never-infected";
    case Outcome::InfectedNotPropagated: return "survivor:infected-not-propagated";
    case Outcome::PropagatedNotDetected: return "survivor:propagated-not-detected";
  }
  return "?";
}

DrillTally::DrillTally(std::vector<int> mutant_ids) : ids_(std::move(mutant_ids)), m_(ids_.size()) {}

void DrillTally::observe(std::uint64_t program_id, std::span<const std::uint8_t> levels) {
  ++programs_;
  for (std::size_t j = 0; j < m_.size() && j < levels.size(); ++j) {
    const std::uint8_t b = levels[j];
    MutantTally& t = m_[j];
    const bool infected = (b & kLevelInfected) != 0, ran = (b & kLevelRan) != 0, prop = (b & kLevelPropagated) != 0, det = (b & kLevelDetected) != 0;
    if (!ran) ++skipped_;
    if (ran) ++t.ran;
    if (infected) {
      ++t.infected;
      if (t.first_infected == kNever) t.first_infected = program_id;
    }
    if (prop) {
      ++t.propagated;
      if (t.first_propagated == kNever) t.first_propagated = program_id;
      if (!infected) ++violations_;
    }
    if (det) {
      ++t.detected;
      if (t.first_detected == kNever) t.first_detected = program_id;
      if (!prop) ++inconsistent_;
    }
  }
}

Outcome DrillTally::outcome(const MutantTally& t) {
  if (t.detected > 0) return Outcome::Killed;
  if (t.propagated > 0) return Outcome::PropagatedNotDetected;
  if (t.infected > 0) return Outcome::InfectedNotPropagated;
  return Outcome::NeverInfected;
}

std::vector<std::uint64_t> DrillTally::to_words() const {
  std::vector<std::uint64_t> w = {programs_, skipped_, violations_, inconsistent_};
  for (const MutantTally& t : m_)
    for (const std::uint64_t v : {t.ran, t.infected, t.propagated, t.detected, t.first_infected, t.first_propagated, t.first_detected}) w.push_back(v);
  return w;
}

DrillTally DrillTally::from_words(std::vector<int> ids, const std::vector<std::uint64_t>& words, bool& ok) {
  DrillTally t(std::move(ids));
  ok = words.size() == 4 + 7 * t.m_.size();
  if (!ok) return t;
  t.programs_ = words[0];
  t.skipped_ = words[1];
  t.violations_ = words[2];
  t.inconsistent_ = words[3];
  for (std::size_t j = 0; j < t.m_.size(); ++j) {
    const std::uint64_t* p = &words[4 + 7 * j];
    t.m_[j] = {p[0], p[1], p[2], p[3], p[4], p[5], p[6]};
  }
  return t;
}

}  // namespace cutoff::engine
