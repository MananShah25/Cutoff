#include "toml_lite.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>

namespace cutoff::engine {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
  return s;
}

bool valid_key(std::string_view k) {
  if (k.empty()) return false;
  for (const char c : k)
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_' && c != '-') return false;
  return true;
}

// Parses a value starting at `rest` (already trimmed). On error returns false and sets `err`.
bool parse_value(std::string_view rest, TomlValue& out, std::string& err) {
  if (rest.empty()) {
    err = "missing value after '='";
    return false;
  }
  if (rest.front() == '"') {
    std::string s;
    std::size_t i = 1;
    for (; i < rest.size(); ++i) {
      const char c = rest[i];
      if (c == '"') break;
      if (c == '\\') {
        if (++i >= rest.size()) break;
        switch (rest[i]) {
          case 'n': s += '\n'; break;
          case 't': s += '\t'; break;
          case '"': s += '"'; break;
          case '\\': s += '\\'; break;
          default: err = std::string("unsupported escape '\\") + rest[i] + "' in string"; return false;
        }
      } else {
        s += c;
      }
    }
    if (i >= rest.size()) {
      err = "unterminated string (add the closing quote)";
      return false;
    }
    const std::string_view tail = trim(rest.substr(i + 1));
    if (!tail.empty() && tail.front() != '#') {
      err = "unexpected text after the string";
      return false;
    }
    out.type = TomlValue::Type::String;
    out.s = std::move(s);
    return true;
  }
  // Bare token up to a comment.
  std::size_t end = rest.find('#');
  std::string tok(trim(rest.substr(0, end)));
  if (tok == "true" || tok == "false") {
    out.type = TomlValue::Type::Bool;
    out.b = tok == "true";
    return true;
  }
  std::string digits;
  for (const char c : tok)
    if (c != '_') digits += c;
  if (digits.empty()) {
    err = "missing value after '='";
    return false;
  }
  const bool looks_float = digits.find_first_of(".eE") != std::string::npos;
  if (!looks_float) {
    std::int64_t v = 0;
    const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v);
    if (r.ec == std::errc() && r.ptr == digits.data() + digits.size()) {
      out.type = TomlValue::Type::Int;
      out.i = v;
      return true;
    }
  } else {
    char* endp = nullptr;
    const double d = std::strtod(digits.c_str(), &endp);
    if (endp != nullptr && *endp == '\0') {
      out.type = TomlValue::Type::Float;
      out.d = d;
      return true;
    }
  }
  err = "cannot read '" + tok + "' as a value (strings need double quotes)";
  return false;
}

}  // namespace

TomlParse parse_toml(std::string_view text, const std::string& name) {
  TomlParse result;
  std::string section;
  int line_no = 0;
  std::size_t pos = 0;
  auto fail = [&](const std::string& msg) {
    result.error = name + ":" + std::to_string(line_no) + ": " + msg;
    result.doc.clear();
  };
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    const std::string_view raw = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    ++line_no;
    const std::string_view line = trim(raw);
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '[') {
      const std::size_t close = line.find(']');
      if (close == std::string_view::npos) { fail("section header is missing ']'"); return result; }
      const std::string_view tail = trim(line.substr(close + 1));
      if (!tail.empty() && tail.front() != '#') { fail("unexpected text after the section header"); return result; }
      const std::string_view sec = trim(line.substr(1, close - 1));
      if (!valid_key(sec)) { fail("invalid section name '" + std::string(sec) + "'"); return result; }
      section = std::string(sec);
      continue;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) { fail("expected 'key = value' or a [section] header"); return result; }
    const std::string_view key = trim(line.substr(0, eq));
    if (!valid_key(key)) { fail("invalid key '" + std::string(key) + "'"); return result; }
    TomlValue v;
    v.line = line_no;
    std::string err;
    if (!parse_value(trim(line.substr(eq + 1)), v, err)) { fail(err); return result; }
    const std::string full = section.empty() ? std::string(key) : section + "." + std::string(key);
    if (result.doc.count(full) != 0) { fail("duplicate key '" + full + "'"); return result; }
    result.doc.emplace(full, std::move(v));
  }
  return result;
}

}  // namespace cutoff::engine
