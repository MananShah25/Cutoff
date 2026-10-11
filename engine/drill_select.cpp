#include "drill_select.hpp"

#include <algorithm>
#include <set>
#include <sstream>

#include "rng.hpp"
#include "sim_drill.hpp"

namespace cutoff::engine {

std::vector<MutantInfo> all_mutants() {
  std::vector<MutantInfo> out;
  std::istringstream in{std::string(cutoff::drill_manifest_tsv())};
  std::string line;
  std::getline(in, line);  // header
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    std::istringstream ls(line);
    std::string cell;
    while (std::getline(ls, cell, '\t')) f.push_back(cell);
    if (f.size() != 8) continue;
    MutantInfo m;
    m.id = std::stoi(f[0]);
    m.site = std::stoi(f[1]);
    m.category = f[2];
    m.op_class = f[3];
    m.kind = f[4];
    m.line = std::stoi(f[5]);
    m.original = f[6];
    m.mutated = f[7];
    out.push_back(std::move(m));
  }
  return out;
}

std::vector<int> select_mutants(const std::string& operators_text, std::size_t count, std::uint64_t sample_seed, std::string& error) {
  const std::vector<MutantInfo> all = all_mutants();
  std::set<std::string> include_class, include_category, exclude_class, exclude_category;
  std::istringstream in(operators_text);
  std::string raw;
  int lineno = 0;
  while (std::getline(in, raw)) {
    ++lineno;
    if (const auto hash = raw.find('#'); hash != std::string::npos) raw.resize(hash);
    std::istringstream ls(raw);
    std::string rule;
    if (!(ls >> rule)) continue;
    const bool exclude = rule[0] == '-';
    if (exclude) rule.erase(0, 1);
    const auto colon = rule.find(':');
    const std::string kind = colon == std::string::npos ? "" : rule.substr(0, colon), name = colon == std::string::npos ? "" : rule.substr(colon + 1);
    const bool known_class = std::any_of(all.begin(), all.end(), [&](const MutantInfo& m) { return m.op_class == name; });
    const bool known_category = std::any_of(all.begin(), all.end(), [&](const MutantInfo& m) { return m.category == name; });
    if ((kind != "class" || !known_class) && (kind != "category" || !known_category)) {
      error = "operators file line " + std::to_string(lineno) + ": expected [-]class:<operator class> or [-]category:<category> naming something that exists, found '" + rule + "'";
      return {};
    }
    (kind == "class" ? (exclude ? exclude_class : include_class) : (exclude ? exclude_category : include_category)).insert(name);
  }
  const bool has_include = !include_class.empty() || !include_category.empty();
  std::vector<int> eligible;
  for (const MutantInfo& m : all) {
    const bool inc = !has_include || include_class.count(m.op_class) != 0 || include_category.count(m.category) != 0;
    const bool exc = exclude_class.count(m.op_class) != 0 || exclude_category.count(m.category) != 0;
    if (inc && !exc) eligible.push_back(m.id);
  }
  if (eligible.empty()) {
    error = "the operators file leaves no mutants; check the include and exclude rules (`cutoff build --drill --list` shows the classes and categories).";
    return {};
  }
  if (count > 0 && count < eligible.size()) {
    gen::Rng rng(sample_seed);
    for (std::size_t i = 0; i < count; ++i) {  // partial Fisher-Yates: the first `count` entries are a uniform sample
      const std::size_t j = i + rng.below(static_cast<std::uint32_t>(eligible.size() - i));
      std::swap(eligible[i], eligible[j]);
    }
    eligible.resize(count);
    std::sort(eligible.begin(), eligible.end());
  }
  return eligible;
}

}  // namespace cutoff::engine
