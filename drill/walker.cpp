#include "walker.hpp"

#include <algorithm>
#include <set>

namespace cutoff::drill {

namespace {

struct W {
  const std::vector<Token>& t;
  std::string_view src;
  Walk& out;
  std::string error;

  bool is(std::size_t i, std::string_view s) const { return i < t.size() && (t[i].kind == Tok::Op || t[i].kind == Tok::Ident) && t[i].text == s; }
  bool failed() const { return !error.empty(); }
  void fail(std::size_t i, const std::string& what) {
    if (error.empty()) error = "line " + std::to_string(t[std::min(i, t.size() - 1)].line) + ": " + what + " near '" + std::string(t[std::min(i, t.size() - 1)].text) + "'";
  }

  // Index just past the group opened by the bracket at i.
  std::size_t skip_group(std::size_t i) {
    const std::string_view open = t[i].text;
    const std::string_view close = open == "[" ? "]" : open == "(" ? ")" : "}";
    int depth = 0;
    for (std::size_t j = i; j < t.size(); ++j) {
      if (t[j].kind != Tok::Op) continue;
      if (t[j].text == open) ++depth;
      else if (t[j].text == close && --depth == 0) return j + 1;
    }
    fail(i, "unbalanced bracket");
    return t.size() - 1;
  }

  // Skips to just past the next ';' at bracket depth 0.
  std::size_t skip_to_semicolon(std::size_t i) {
    while (i < t.size() - 1) {
      if (t[i].kind == Tok::Op && (t[i].text == "(" || t[i].text == "[" || t[i].text == "{")) { i = skip_group(i); continue; }
      if (is(i, ";")) return i + 1;
      ++i;
    }
    fail(i, "missing ';'");
    return i;
  }

  // Parses an expression at i that must end at token `closer`; records it when live.
  const Expr* parse_until(std::size_t& i, std::string_view closer, bool live) {
    ParseResult r = parse_expr(t, i);
    if (!r.expr || !is(r.next, closer)) {
      const std::string msg = r.error.empty() ? "expression does not end at '" + std::string(closer) + "'" : r.error;
      if (live) fail(i, msg);
      else out.warnings.push_back(msg);
      i = skip_to_closer(i, closer);
      return nullptr;
    }
    i = r.next;
    out.owned.push_back(std::move(r.expr));
    return out.owned.back().get();
  }

  std::size_t skip_to_closer(std::size_t i, std::string_view closer) {
    while (i < t.size() - 1) {
      if (t[i].kind == Tok::Op && (t[i].text == "(" || t[i].text == "[" || t[i].text == "{")) { i = skip_group(i); continue; }
      if (is(i, closer)) return i;
      ++i;
    }
    return i;
  }

  int current_block = -1;

  std::size_t assignment(std::size_t i, bool live, std::size_t stmt_begin, bool continuous) {
    // lvalue: ident with selects, or a concatenation
    std::string lhs;
    std::size_t j = i;
    if (is(j, "{")) {
      j = skip_group(j);
    } else {
      lhs = std::string(t[j].text);
      ++j;
      while (is(j, "[")) { j = skip_group(j); lhs.clear(); }
    }
    if (failed()) return j;
    if (!(is(j, "=") || is(j, "<="))) {  // not an assignment: an instance or something we do not model
      return skip_to_semicolon(i);
    }
    ++j;
    if (t[j].kind == Tok::Op && t[j].text == "#") j += 2;  // intra-assignment delay
    const std::size_t rhs_begin = j;
    const Expr* e = parse_until(j, ";", live);
    if (e != nullptr && live) {
      Root r;
      r.kind = Root::Kind::Rhs;
      r.expr = e;
      r.lhs = lhs;
      r.line = t[rhs_begin].line;
      r.stmt_begin = stmt_begin;
      r.stmt_end = j + 1;
      r.continuous = continuous;
      r.block = continuous ? -1 : current_block;
      out.roots.push_back(r);
    }
    ++out.live_statements;
    if (!live) { --out.live_statements; ++out.dead_statements; }
    return j + 1;
  }

  std::size_t stmt(std::size_t i, bool live) {
    if (failed()) return i;
    const Token& k = t[i];
    if (k.kind == Tok::End) { fail(i, "unexpected end of file"); return i; }
    if (k.kind == Tok::Macro) {  // `debug(...), `assert(...): expand to nothing in the RV32I build
      ++i;
      if (is(i, "(")) i = skip_group(i);
      if (is(i, ";")) ++i;
      return i;
    }
    if (k.kind == Tok::Op) {
      if (k.text == ";") return i + 1;
      if (k.text == "{") return assignment(i, live, i, false);
      if (k.text == "#") return i + 2;
      fail(i, "unexpected token at statement start");
      return i;
    }
    if (k.kind != Tok::Ident) { fail(i, "unexpected token at statement start"); return i; }
    const std::string_view w = k.text;

    if (w == "begin") {
      ++i;
      if (is(i, ":")) i += 2;
      while (!is(i, "end") && !failed()) {
        if (t[i].kind == Tok::End) { fail(i, "missing 'end'"); return i; }
        i = stmt(i, live);
      }
      return i + 1;
    }
    if (w == "if") {
      const std::size_t if_begin = i;
      ++i;
      if (!is(i, "(")) { fail(i, "expected '(' after if"); return i; }
      ++i;
      const Expr* cond = parse_until(i, ")", live);
      if (failed()) return i;
      ++i;  // ')'
      std::optional<std::int64_t> v;
      if (cond != nullptr) v = fold_const(*cond, out.params);
      const bool then_live = live && !(v && *v == 0);
      const bool else_live = live && !(v && *v != 0);
      std::optional<std::size_t> root_index;
      if (live && cond != nullptr && !v) {
        Root r;
        r.kind = Root::Kind::Cond;
        r.expr = cond;
        r.line = t[cond->tok_begin].line;
        r.stmt_begin = if_begin;
        r.block = current_block;
        out.roots.push_back(r);
        root_index = out.roots.size() - 1;
      }
      i = stmt(i, then_live);
      if (is(i, "else")) i = stmt(i + 1, else_live);
      if (root_index) out.roots[*root_index].stmt_end = i;
      return i;
    }
    if (w == "case" || w == "casez" || w == "casex") {
      ++i;
      if (!is(i, "(")) { fail(i, "expected '(' after case"); return i; }
      const std::size_t sel_open = i;
      i = skip_group(i);
      const bool onehot = i - sel_open == 3 && t[sel_open + 1].kind == Tok::Number && t[sel_open + 1].text == "1'b1";
      while (!is(i, "endcase") && !failed()) {
        if (t[i].kind == Tok::End) { fail(i, "missing 'endcase'"); return i; }
        bool item_live = live;
        if (is(i, "default")) {
          ++i;
          if (is(i, ":")) ++i;
        } else {
          bool any_live_label = false, all_known = true;
          for (;;) {
            ParseResult r = parse_expr(t, i);
            if (!r.expr) { fail(i, r.error); return i; }
            i = r.next;
            std::optional<std::int64_t> v;
            if (onehot) v = fold_const(*r.expr, out.params);
            if (!v || *v != 0) any_live_label = true;
            if (!v) all_known = false;
            out.owned.push_back(std::move(r.expr));
            if (is(i, ",")) { ++i; continue; }
            break;
          }
          (void)all_known;
          if (!is(i, ":")) { fail(i, "expected ':' after case label"); return i; }
          ++i;
          if (onehot && !any_live_label) item_live = false;
        }
        i = stmt(i, item_live);
      }
      return i + 1;
    }
    if (w == "for") {
      ++i;
      if (is(i, "(")) i = skip_group(i);
      return stmt(i, false);  // loops only appear in initialisation here
    }
    if (w == "always") {
      ++i;
      if (is(i, "@")) {
        ++i;
        if (is(i, "(")) i = skip_group(i);
        else ++i;  // @*
      }
      const int saved_block = current_block;
      if (live) {
        out.blocks.push_back({i, 0});
        current_block = static_cast<int>(out.blocks.size()) - 1;
      }
      i = stmt(i, live);
      if (live) out.blocks[static_cast<std::size_t>(current_block)].end = i;
      current_block = saved_block;
      return i;
    }
    if (w == "initial") return stmt(i + 1, false);
    if (w == "assign") return assignment(i + 1, live, i, true);
    if (w == "generate" || w == "endgenerate") return i + 1;
    if (w == "function" || w == "task") {
      const std::string_view end_word = w == "function" ? "endfunction" : "endtask";
      while (!is(i, end_word) && t[i].kind != Tok::End) ++i;
      return i + 1;
    }
    static const std::set<std::string_view> decls = {"reg", "wire", "integer", "parameter", "localparam", "input", "output", "inout",
                                                     "genvar", "real", "time", "tri", "supply0", "supply1", "wand", "wor"};
    if (decls.count(w) != 0) return skip_to_semicolon(i);
    return assignment(i, live, i, false);  // an assignment, or an instance (skipped to its ';')
  }
};

// Reads `parameter`/`localparam` declarations in [b, e) in order, evaluating each (with overrides applied).
void collect_params(const std::vector<Token>& t, std::size_t b, std::size_t e, const ParamMap& overrides, ParamMap& params) {
  auto is = [&](std::size_t i, std::string_view s) { return i < t.size() && t[i].text == s && (t[i].kind == Tok::Op || t[i].kind == Tok::Ident); };
  for (std::size_t i = b; i < e; ++i) {
    if (!(is(i, "parameter") || is(i, "localparam"))) continue;
    std::size_t j = i + 1;
    while (j < e && !(t[j].kind == Tok::Ident && is(j + 1, "=")) ) {
      if (is(j, "[")) {  // width: skip the group
        int depth = 0;
        for (; j < e; ++j) {
          if (is(j, "[")) ++depth;
          if (is(j, "]") && --depth == 0) { ++j; break; }
        }
        continue;
      }
      ++j;
    }
    if (j + 1 >= e) break;
    const std::string name(t[j].text);
    ParseResult r = parse_expr(t, j + 2);
    if (!r.expr) continue;
    auto ov = overrides.find(name);
    const auto v = ov != overrides.end() ? std::optional<std::int64_t>(ov->second) : fold_const(*r.expr, params);
    if (v) params[name] = *v;
    i = std::max(i, r.next - 1);
  }
}

}  // namespace

Walk walk_module(const std::vector<Token>& toks, std::string_view source, const std::string& module, const ParamMap& overrides) {
  Walk out;
  std::size_t m = 0;
  for (; m + 1 < toks.size(); ++m)
    if (toks[m].kind == Tok::Ident && toks[m].text == "module" && toks[m + 1].text == module) break;
  if (m + 1 >= toks.size()) { out.error = "module '" + module + "' not found"; return out; }
  out.module_begin = m;
  std::size_t end = m;
  while (end < toks.size() && !(toks[end].kind == Tok::Ident && toks[end].text == "endmodule")) ++end;
  if (end >= toks.size()) { out.error = "'endmodule' not found for module '" + module + "'"; return out; }

  collect_params(toks, m, end, overrides, out.params);

  W w{toks, source, out, {}};
  std::size_t i = m + 2;
  if (w.is(i, "#")) i = w.skip_group(i + 1);
  if (!w.is(i, "(")) { out.error = "line " + std::to_string(toks[i].line) + ": expected the port list of module '" + module + "'"; return out; }
  out.port_list_open = i;
  i = w.skip_group(i);
  if (w.is(i, ";")) ++i;
  out.header_end = i;
  out.endmodule = end;
  while (i < end && !w.failed()) i = w.stmt(i, true);
  if (w.failed()) out.error = w.error;
  return out;
}

}  // namespace cutoff::drill
