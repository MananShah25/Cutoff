#include "regions.hpp"

#include <sstream>

namespace cutoff::drill {

const Region* RegionTable::find(int line) const {
  for (const Region& r : regions)
    if (line >= r.first_line && line <= r.last_line) return &r;
  return nullptr;
}

const StuckTarget* RegionTable::find_stuck(const std::string& signal) const {
  for (const StuckTarget& s : stuck)
    if (s.signal == signal) return &s;
  return nullptr;
}

namespace {

std::string source_line(const std::string& source, int line) {
  int cur = 1;
  std::size_t b = 0;
  for (std::size_t i = 0; i < source.size() && cur < line; ++i)
    if (source[i] == '\n') { ++cur; b = i + 1; }
  if (cur != line) return {};
  const std::size_t e = source.find('\n', b);
  return source.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

}  // namespace

std::string parse_regions(const std::string& text, const std::string& source, RegionTable& table) {
  std::istringstream in(text);
  std::string raw;
  int lineno = 0;
  while (std::getline(in, raw)) {
    ++lineno;
    const std::size_t hash = raw.find('#');
    if (hash != std::string::npos) raw.resize(hash);
    std::istringstream ls(raw);
    std::string word;
    if (!(ls >> word)) continue;
    const std::string where = "regions line " + std::to_string(lineno) + ": ";
    if (word == "module") {
      if (!(ls >> table.module)) return where + "module needs a name";
    } else if (word == "region") {
      Region r;
      if (!(ls >> r.category >> r.first_line >> r.last_line)) return where + "expected: region <category> <first-line> <last-line> \"<anchor>\" [ops=...]";
      std::string rest;
      std::getline(ls, rest);
      const std::size_t q1 = rest.find('"');
      const std::size_t q2 = q1 == std::string::npos ? q1 : rest.find('"', q1 + 1);
      if (q2 == std::string::npos) return where + "the anchor text must be in double quotes";
      r.anchor = rest.substr(q1 + 1, q2 - q1 - 1);
      std::string tail = rest.substr(q2 + 1);
      r.ops = all_operator_classes();
      const std::size_t eq = tail.find("ops=");
      if (eq != std::string::npos) {
        r.ops.clear();
        std::string list = tail.substr(eq + 4);
        std::istringstream ops(list);
        std::string item;
        std::string token;
        ops >> token;
        std::istringstream csv(token);
        while (std::getline(csv, item, ',')) {
          if (all_operator_classes().count(item) == 0) return where + "unknown operator class '" + item + "'";
          r.ops.insert(item);
        }
      }
      if (r.first_line < 1 || r.last_line < r.first_line) return where + "bad line range";
      const std::string first = source_line(source, r.first_line);
      if (first.find(r.anchor) == std::string::npos)
        return where + "the source line " + std::to_string(r.first_line) + " does not contain the anchor \"" + r.anchor + "\" (it reads: \"" + first +
               "\"). This table was written for a different version of the core source.";
      table.regions.push_back(std::move(r));
    } else if (word == "stuck") {
      StuckTarget s;
      std::string bits;
      if (!(ls >> s.signal >> s.width >> bits)) return where + "expected: stuck <signal> <width> <bit,bit,...>";
      std::istringstream csv(bits);
      std::string item;
      while (std::getline(csv, item, ',')) {
        const int b = std::stoi(item);
        if (b < 0 || b >= s.width) return where + "bit " + item + " is outside the width " + std::to_string(s.width);
        s.bits.push_back(b);
      }
      table.stuck.push_back(std::move(s));
    } else {
      return where + "unknown directive '" + word + "'";
    }
  }
  if (table.module.empty()) return "the regions file must name the module (module <name>)";
  return {};
}

}  // namespace cutoff::drill
