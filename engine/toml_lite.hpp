#pragma once
// A deliberately small TOML subset: [sections], `key = value` with string, integer (underscores allowed),
// float and bool values, and # comments. Enough for cutoff.toml; arrays arrive when a setting needs them.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace cutoff::engine {

struct TomlValue {
  enum class Type : std::uint8_t { String, Int, Float, Bool } type = Type::String;
  std::string s;
  std::int64_t i = 0;
  double d = 0;
  bool b = false;
  int line = 0;
};

// Keys are "section.key" (or just "key" before any section header).
using TomlDoc = std::map<std::string, TomlValue>;

struct TomlParse {
  TomlDoc doc;
  std::string error;  // empty on success; otherwise "<name>:<line>: what is wrong"
};

TomlParse parse_toml(std::string_view text, const std::string& name);

}  // namespace cutoff::engine
