#include "lexer.hpp"

#include <cctype>
#include <cstring>

namespace cutoff::drill {

namespace {

struct Cond {
  bool parent_active;
  bool active;
  bool taken;
};

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$'; }

constexpr const char* kOps[] = {"<<<", ">>>", "===", "!==", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "**", "~&", "~|", "~^", "^~", "->"};

}  // namespace

LexResult lex(std::string_view s, const std::set<std::string>& defines) {
  LexResult out;
  std::vector<Cond> conds;
  auto active = [&] { return conds.empty() || conds.back().active; };
  std::size_t i = 0;
  int line = 1;
  bool line_start = true;
  auto fail = [&](const std::string& what) {
    out.error = "line " + std::to_string(line) + ": " + what;
    out.tokens.clear();
  };

  while (i < s.size()) {
    const char c = s[i];
    if (c == '\n') { ++line; ++i; line_start = true; continue; }
    if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }

    // Preprocessor directives occupy a whole line.
    if (c == '`' && line_start) {
      std::size_t j = i + 1;
      while (j < s.size() && is_ident_char(s[j])) ++j;
      const std::string_view dir = s.substr(i + 1, j - i - 1);
      std::size_t eol = j;
      while (eol < s.size() && s[eol] != '\n') {
        if (s[eol] == '\\' && eol + 1 < s.size() && s[eol + 1] == '\n') ++eol, ++line;  // continued line
        ++eol;
      }
      auto arg = [&] {
        std::size_t a = j;
        while (a < eol && (s[a] == ' ' || s[a] == '\t')) ++a;
        std::size_t b = a;
        while (b < eol && is_ident_char(s[b])) ++b;
        return std::string(s.substr(a, b - a));
      };
      if (dir == "ifdef" || dir == "ifndef") {
        const bool def = defines.count(arg()) != 0;
        const bool want = dir == "ifdef" ? def : !def;
        const bool parent = active();
        conds.push_back({parent, parent && want, want});
      } else if (dir == "elsif") {
        if (conds.empty()) { fail("`elsif without `ifdef"); return out; }
        Cond& k = conds.back();
        const bool want = defines.count(arg()) != 0;
        k.active = k.parent_active && !k.taken && want;
        k.taken = k.taken || want;
      } else if (dir == "else") {
        if (conds.empty()) { fail("`else without `ifdef"); return out; }
        Cond& k = conds.back();
        k.active = k.parent_active && !k.taken;
        k.taken = true;
      } else if (dir == "endif") {
        if (conds.empty()) { fail("`endif without `ifdef"); return out; }
        conds.pop_back();
      } else if (dir == "define" || dir == "undef" || dir == "timescale" || dir == "include" || dir == "default_nettype" ||
                 dir == "resetall" || dir == "celldefine" || dir == "endcelldefine") {
        // skipped (the macros of interest are only ever used as calls we do not expand)
      } else {
        // a macro use at the start of a line: fall through to normal tokenising
        goto normal;
      }
      i = eol;
      continue;
    }
  normal:
    line_start = false;

    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) {
        if (s[i] == '\n') ++line;
        ++i;
      }
      if (i + 1 >= s.size()) { fail("unterminated /* comment"); return out; }
      i += 2;
      continue;
    }
    if (!active()) {
      ++i;
      continue;
    }
    // Attribute (* ... *), but not @(*) or (*)
    if (c == '(' && i + 1 < s.size() && s[i + 1] == '*' && !(i + 2 < s.size() && s[i + 2] == ')')) {
      i += 2;
      while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == ')')) {
        if (s[i] == '\n') ++line;
        ++i;
      }
      if (i + 1 >= s.size()) { fail("unterminated attribute (* ... *)"); return out; }
      i += 2;
      continue;
    }

    Token t;
    t.begin = i;
    t.line = line;
    if (c == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') ++i;
        if (i < s.size() && s[i] == '\n') ++line;
        ++i;
      }
      if (i >= s.size()) { fail("unterminated string"); return out; }
      ++i;
      t.kind = Tok::String;
    } else if (c == '`') {
      ++i;
      while (i < s.size() && is_ident_char(s[i])) ++i;
      t.kind = Tok::Macro;
    } else if (c == '$') {
      ++i;
      while (i < s.size() && is_ident_char(s[i])) ++i;
      t.kind = Tok::Sys;
    } else if (is_ident_start(c)) {
      while (i < s.size() && is_ident_char(s[i])) ++i;
      t.kind = Tok::Ident;
    } else if (std::isdigit(static_cast<unsigned char>(c)) != 0 || (c == '\'' && i + 1 < s.size() && std::strchr("bBoOdDhHsS", s[i + 1]) != nullptr)) {
      // size, then optionally 'base and digits (spaces allowed after the base letter)
      while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) != 0 || s[i] == '_')) ++i;
      if (i < s.size() && s[i] == '\'') {
        ++i;
        if (i < s.size() && (s[i] == 's' || s[i] == 'S')) ++i;
        if (i < s.size() && std::strchr("bBoOdDhH", s[i]) != nullptr) ++i;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        while (i < s.size() && (std::isxdigit(static_cast<unsigned char>(s[i])) != 0 || s[i] == '_' || s[i] == 'x' || s[i] == 'X' || s[i] == 'z' || s[i] == 'Z' || s[i] == '?')) ++i;
      }
      t.kind = Tok::Number;
    } else {
      t.kind = Tok::Op;
      std::size_t len = 1;
      for (const char* op : kOps) {
        const std::size_t n = std::strlen(op);
        if (s.compare(i, n, op) == 0) { len = n; break; }
      }
      i += len;
    }
    t.end = i;
    t.text = s.substr(t.begin, t.end - t.begin);
    out.tokens.push_back(t);
  }
  if (!conds.empty()) { fail("`ifdef without `endif"); return out; }
  Token end;
  end.kind = Tok::End;
  end.begin = end.end = s.size();
  end.line = line;
  out.tokens.push_back(end);
  return out;
}

}  // namespace cutoff::drill
