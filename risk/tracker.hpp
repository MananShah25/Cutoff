#pragma once
// Incremental incidence counts for one species set: how many programs exercised each species, and the derived
// frequency-of-frequencies Q1, Q2 and V, maintained in O(species per program) per program.

#include <cstdint>
#include <span>
#include <vector>

#include "estimators.hpp"

namespace cutoff::risk {

class SpeciesTracker {
 public:
  explicit SpeciesTracker(std::size_t universe) : per_species_(universe, 0) {}

  // `ids` are the species one program exercised: unique, each below the universe size. Out-of-range or repeated
  // ids are a caller bug and are rejected (returns false, nothing changes).
  bool add_program(std::span<const std::uint16_t> ids);

  const Counts& counts() const { return counts_; }
  // Number of programs since a new species last appeared (0 right after a program that found one).
  std::uint64_t programs_since_new() const { return counts_.n - last_new_n_; }
  std::uint64_t last_new_n() const { return last_new_n_; }
  const std::vector<std::uint32_t>& per_species() const { return per_species_; }

  // Rebuilds a tracker from saved per-species counts (checkpoint resume).
  static SpeciesTracker restore(std::vector<std::uint32_t> per_species, std::uint64_t n, std::uint64_t last_new_n);

 private:
  std::vector<std::uint32_t> per_species_;
  Counts counts_;
  std::uint64_t last_new_n_ = 0;
};

}  // namespace cutoff::risk
