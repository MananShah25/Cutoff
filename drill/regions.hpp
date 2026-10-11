#pragma once
// The region table: which lines of a core's source may be mutated, grouped by the categories of the spec
// (ALU, comparators, branch decision, immediate decode, register-file write / x0, memory address and strobes, PC update),
// plus the signals that get stuck-at mutants. It is pinned to one version of the core source: every region carries
// an anchor text that must appear on its first line, so a different source version fails loudly instead of silently
// mutating the wrong logic.
//
// File format (one directive per line, # starts a comment):
//   module   picorv32
//   region   <category> <first-line> <last-line> "<anchor text>" [ops=arith,eq,rel,logic,bitwise,not,const,shift,signed,stuck]
//   stuck    <signal> <width> <bit,bit,...>

#include <set>
#include <string>
#include <vector>

namespace cutoff::drill {

struct Region {
  std::string category;
  int first_line = 0, last_line = 0;
  std::string anchor;
  std::set<std::string> ops;  // operator classes enabled in this region
  bool allows(const std::string& cls) const { return ops.count(cls) != 0; }
};

struct StuckTarget {
  std::string signal;
  int width = 0;
  std::vector<int> bits;
};

struct RegionTable {
  std::string module;
  std::vector<Region> regions;
  std::vector<StuckTarget> stuck;
  const Region* find(int line) const;
  const StuckTarget* find_stuck(const std::string& signal) const;
};

inline const std::set<std::string>& all_operator_classes() {
  static const std::set<std::string> all = {"arith", "eq", "rel", "logic", "bitwise", "not", "const", "shift", "signed", "stuck"};
  return all;
}

// Returns an empty error on success. `source` is used to check the anchors.
std::string parse_regions(const std::string& text, const std::string& source, RegionTable& table);

}  // namespace cutoff::drill
