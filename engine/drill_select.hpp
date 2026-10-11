#pragma once
// Which of the instrumented core's mutants a drill uses.

#include <string>
#include <vector>

namespace cutoff::engine {

struct MutantInfo {
  int id = 0;
  int site = 0;
  int line = 0;
  std::string category, op_class, kind, original, mutated;
};

// Every mutant of the built-in instrumented core, from the embedded manifest.
std::vector<MutantInfo> all_mutants();

// Applies an operator filter and a random sample.
//   operators_file  one rule per line, '#' starts a comment: `class:<op_class>` or `category:<category>` includes,
//                   a leading '-' excludes. With no include rule every mutant is eligible; excludes always apply.
//   count           0 = all eligible mutants, otherwise a uniformly random sample of that many, chosen with `sample_seed`
// Returns the selected ids in ascending order. `error` is set when the filter is malformed or leaves nothing.
std::vector<int> select_mutants(const std::string& operators_text, std::size_t count, std::uint64_t sample_seed, std::string& error);

}  // namespace cutoff::engine
