#include "tracker.hpp"

#include <algorithm>

namespace cutoff::risk {

bool SpeciesTracker::add_program(std::span<const std::uint16_t> ids) {
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] >= per_species_.size()) return false;
    if (i > 0 && ids[i] <= ids[i - 1]) return false;  // must be strictly ascending: sorted and unique
  }
  ++counts_.n;
  bool found_new = false;
  for (const std::uint16_t id : ids) {
    std::uint32_t& y = per_species_[id];
    ++y;
    ++counts_.v;
    if (y == 1) {
      ++counts_.s;
      ++counts_.q1;
      found_new = true;
    } else if (y == 2) {
      --counts_.q1;
      ++counts_.q2;
    } else if (y == 3) {
      --counts_.q2;
    }
  }
  if (found_new) last_new_n_ = counts_.n;
  return true;
}

SpeciesTracker SpeciesTracker::restore(std::vector<std::uint32_t> per_species, std::uint64_t n, std::uint64_t last_new_n) {
  SpeciesTracker t(0);
  t.per_species_ = std::move(per_species);
  t.counts_.n = n;
  t.last_new_n_ = last_new_n;
  for (const std::uint32_t y : t.per_species_) {
    if (y == 0) continue;
    ++t.counts_.s;
    t.counts_.v += y;
    if (y == 1) ++t.counts_.q1;
    if (y == 2) ++t.counts_.q2;
  }
  return t;
}

}  // namespace cutoff::risk
