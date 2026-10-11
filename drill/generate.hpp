#pragma once
// End-to-end generation of the instrumented core and its mutant manifest.

#include <map>
#include <string>

#include "mutator.hpp"

namespace cutoff::drill {

struct GenerateResult {
  std::string instrumented;  // the whole source file with the module replaced by its selectable form
  std::string manifest;      // TSV, see manifest_tsv()
  std::string wrapper;       // the wrapper module with the extra mut_sel input, instantiating the instrumented core
  std::vector<Mutant> mutants;
  std::map<std::string, int> by_category;
  std::map<std::string, int> by_class;
  int sites = 0;
  std::size_t live_roots = 0, dead_statements = 0;
  std::string source_hash;   // FNV-1a of the original source, 16 hex digits
  std::string error;         // empty on success
};

// `wrapper_text` is the Verilog that instantiates the module; its `.NAME(number)` connections give the parameter values.
GenerateResult generate_drill(const std::string& rtl_text, const std::string& wrapper_text, const std::string& regions_text);

std::string describe(const GenerateResult& r);  // human-readable summary

// A C++ source that embeds the manifest and the summary in the binary (see worker/sim_drill.hpp).
std::string embedded_source(const GenerateResult& r);

}  // namespace cutoff::drill
