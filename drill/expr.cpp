#include "expr.hpp"

#include <algorithm>
#include <cctype>

namespace cutoff::drill {

namespace {

struct Parser {
  const std::vector<Token>& t;
  std::string error;

  bool is_op(std::size_t i, std::string_view s) const { return i < t.size() && t[i].kind == Tok::Op && t[i].text == s; }

  std::unique_ptr<Expr> make(Expr::Kind k, std::size_t b) const {
    auto e = std::make_unique<Expr>();
    e->kind = k;
    e->tok_begin = b;
    e->src_begin = t[b].begin;
    e->line = t[b].line;
    return e;
  }
  void finish(Expr& e, std::size_t end) const {
    e.tok_end = end;
    e.src_end = t[end - 1].end;
  }
  void fail(std::size_t i, const std::string& what) {
    if (error.empty()) error = "line " + std::to_string(t[std::min(i, t.size() - 1)].line) + ": " + what + " near '" + std::string(t[std::min(i, t.size() - 1)].text) + "'";
  }

  // Index just past the bracket group that opens at i ('[', '(' or '{').
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

  static int binary_prec(std::string_view op) {
    if (op == "**") return 12;
    if (op == "*" || op == "/" || op == "%") return 11;
    if (op == "+" || op == "-") return 10;
    if (op == "<<" || op == ">>" || op == "<<<" || op == ">>>") return 9;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 8;
    if (op == "==" || op == "!=" || op == "===" || op == "!==") return 7;
    if (op == "&") return 6;
    if (op == "^" || op == "~^" || op == "^~") return 5;
    if (op == "|") return 4;
    if (op == "&&") return 3;
    if (op == "||") return 2;
    return -1;
  }

  std::unique_ptr<Expr> primary(std::size_t& i) {
    const Token& k = t[i];
    const std::size_t b = i;
    if (k.kind == Tok::Number || k.kind == Tok::String) {
      auto e = make(Expr::Kind::Number, b);
      e->name = std::string(k.text);
      ++i;
      finish(*e, i);
      return e;
    }
    if (k.kind == Tok::Sys) {
      auto e = make(Expr::Kind::Call, b);
      e->name = std::string(k.text);
      ++i;
      if (is_op(i, "(")) {
        ++i;
        if (!is_op(i, ")")) {
          for (;;) {
            auto a = expr(i, 0);
            if (!a) return nullptr;
            e->kids.push_back(std::move(a));
            if (is_op(i, ",")) { ++i; continue; }
            break;
          }
        }
        if (!is_op(i, ")")) { fail(i, "expected ')' closing the call"); return nullptr; }
        ++i;
      }
      finish(*e, i);
      return e;
    }
    if (k.kind == Tok::Ident) {
      auto e = make(Expr::Kind::Ident, b);
      e->name = std::string(k.text);
      ++i;
      while (is_op(i, "[")) {
        e->has_select = true;
        i = skip_group(i);
        if (!error.empty()) return nullptr;
      }
      finish(*e, i);
      return e;
    }
    if (k.kind == Tok::Op && k.text == "(") {
      auto e = make(Expr::Kind::Paren, b);
      ++i;
      auto inner = expr(i, 0);
      if (!inner) return nullptr;
      if (!is_op(i, ")")) { fail(i, "expected ')'"); return nullptr; }
      ++i;
      e->kids.push_back(std::move(inner));
      finish(*e, i);
      return e;
    }
    if (k.kind == Tok::Op && k.text == "{") {
      ++i;
      // {count{...}} (replication) or {a, b, ...} (concatenation)
      const std::size_t save = i;
      auto first = expr(i, 0);
      if (!first) return nullptr;
      if (is_op(i, "{")) {  // replication: `first` was the count
        auto e = make(Expr::Kind::Replicate, b);
        ++i;
        for (;;) {
          auto a = expr(i, 0);
          if (!a) return nullptr;
          e->kids.push_back(std::move(a));
          if (is_op(i, ",")) { ++i; continue; }
          break;
        }
        if (!is_op(i, "}")) { fail(i, "expected '}' closing the replication body"); return nullptr; }
        ++i;
        if (!is_op(i, "}")) { fail(i, "expected '}' closing the replication"); return nullptr; }
        ++i;
        finish(*e, i);
        return e;
      }
      (void)save;
      auto e = make(Expr::Kind::Concat, b);
      e->kids.push_back(std::move(first));
      while (is_op(i, ",")) {
        ++i;
        auto a = expr(i, 0);
        if (!a) return nullptr;
        e->kids.push_back(std::move(a));
      }
      if (!is_op(i, "}")) { fail(i, "expected '}'"); return nullptr; }
      ++i;
      finish(*e, i);
      return e;
    }
    fail(i, "expected an expression");
    return nullptr;
  }

  std::unique_ptr<Expr> unary(std::size_t& i) {
    const Token& k = t[i];
    if (k.kind == Tok::Op && (k.text == "!" || k.text == "~" || k.text == "-" || k.text == "+" || k.text == "&" || k.text == "|" ||
                              k.text == "^" || k.text == "~&" || k.text == "~|" || k.text == "~^" || k.text == "^~")) {
      auto e = make(Expr::Kind::Unary, i);
      e->op = std::string(k.text);
      const std::size_t b = i;
      ++i;
      auto operand = unary(i);
      if (!operand) return nullptr;
      e->kids.push_back(std::move(operand));
      (void)b;
      finish(*e, i);
      return e;
    }
    return primary(i);
  }

  // Precedence climbing; min_prec 0 also accepts the ternary operator.
  std::unique_ptr<Expr> expr(std::size_t& i, int min_prec) {
    auto lhs = unary(i);
    if (!lhs) return nullptr;
    for (;;) {
      if (t[i].kind != Tok::Op) break;
      const int p = binary_prec(t[i].text);
      if (p < 0 || p < min_prec) break;
      const std::size_t op_tok = i;
      const std::string op(t[i].text);
      ++i;
      auto rhs = expr(i, p + 1);  // all binary operators here are left-associative
      if (!rhs) return nullptr;
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::Binary;
      e->tok_begin = lhs->tok_begin;
      e->src_begin = lhs->src_begin;
      e->line = t[op_tok].line;
      e->op = op;
      e->kids.push_back(std::move(lhs));
      e->kids.push_back(std::move(rhs));
      finish(*e, i);
      lhs = std::move(e);
    }
    if (min_prec == 0 && is_op(i, "?")) {
      ++i;
      auto a = expr(i, 0);
      if (!a) return nullptr;
      if (!is_op(i, ":")) { fail(i, "expected ':' in the conditional expression"); return nullptr; }
      ++i;
      auto b = expr(i, 0);
      if (!b) return nullptr;
      auto e = std::make_unique<Expr>();
      e->kind = Expr::Kind::Ternary;
      e->tok_begin = lhs->tok_begin;
      e->src_begin = lhs->src_begin;
      e->line = lhs->line;
      e->kids.push_back(std::move(lhs));
      e->kids.push_back(std::move(a));
      e->kids.push_back(std::move(b));
      finish(*e, i);
      return e;
    }
    return lhs;
  }
};

}  // namespace

ParseResult parse_expr(const std::vector<Token>& toks, std::size_t i) {
  Parser p{toks, {}};
  ParseResult r;
  r.expr = p.expr(i, 0);
  r.next = i;
  r.error = p.error;
  if (!r.expr && r.error.empty()) r.error = "cannot parse expression";
  return r;
}

std::optional<NumberInfo> parse_number(std::string_view text) {
  NumberInfo n;
  std::string s;
  for (const char c : text)
    if (c != '_' && c != ' ' && c != '\t') s += c;
  const std::size_t q = s.find('\'');
  auto digits_value = [&](const std::string& d, int base) -> std::optional<std::int64_t> {
    if (d.empty()) return std::nullopt;
    std::int64_t v = 0;
    for (const char c : d) {
      int x;
      if (c >= '0' && c <= '9') x = c - '0';
      else if (c >= 'a' && c <= 'f') x = 10 + c - 'a';
      else if (c >= 'A' && c <= 'F') x = 10 + c - 'A';
      else return std::nullopt;
      if (x >= base) return std::nullopt;
      v = v * base + x;
    }
    return v;
  };
  if (q == std::string::npos) {
    const auto v = digits_value(s, 10);
    if (!v) return std::nullopt;
    n.value = *v;
    return n;
  }
  n.sized = q > 0;
  if (q > 0) {
    const auto w = digits_value(s.substr(0, q), 10);
    if (!w) return std::nullopt;
    n.width = static_cast<int>(*w);
  }
  std::size_t k = q + 1;
  if (k < s.size() && (s[k] == 's' || s[k] == 'S')) ++k;
  if (k >= s.size()) return std::nullopt;
  n.base = static_cast<char>(std::tolower(static_cast<unsigned char>(s[k])));
  ++k;
  const std::string d = s.substr(k);
  if (d.find_first_of("xXzZ?") != std::string::npos) {
    n.has_xz = true;
    return n;
  }
  const int base = n.base == 'b' ? 2 : n.base == 'o' ? 8 : n.base == 'h' ? 16 : 10;
  const auto v = digits_value(d, base);
  if (!v) return std::nullopt;
  n.value = *v;
  return n;
}

std::string source_text(std::string_view source, const Expr& e) { return std::string(source.substr(e.src_begin, e.src_end - e.src_begin)); }

namespace {

bool bool_of(std::int64_t v) { return v != 0; }

}  // namespace

std::optional<std::int64_t> fold_const(const Expr& e, const ParamMap& params) {
  using K = Expr::Kind;
  switch (e.kind) {
    case K::Number: {
      const auto n = parse_number(e.name);
      if (!n || n->has_xz) return std::nullopt;
      return n->value;
    }
    case K::Ident: {
      if (e.has_select) return std::nullopt;
      const auto it = params.find(e.name);
      if (it == params.end()) return std::nullopt;
      return it->second;
    }
    case K::Paren: return fold_const(*e.kids[0], params);
    case K::Unary: {
      const auto v = fold_const(*e.kids[0], params);
      if (!v) return std::nullopt;
      if (e.op == "!") return bool_of(*v) ? 0 : 1;
      if (e.op == "-") return -*v;
      if (e.op == "+") return *v;
      return std::nullopt;
    }
    case K::Ternary: {
      const auto c = fold_const(*e.kids[0], params);
      if (!c) return std::nullopt;
      return fold_const(*e.kids[bool_of(*c) ? 1 : 2], params);
    }
    case K::Binary: {
      const auto l = fold_const(*e.kids[0], params);
      const auto r = fold_const(*e.kids[1], params);
      // Short circuits: a known 0 decides &&, a known non-zero decides ||, even if the other side is a signal.
      if (e.op == "&&") {
        if ((l && !bool_of(*l)) || (r && !bool_of(*r))) return 0;
        if (l && r) return 1;
        return std::nullopt;
      }
      if (e.op == "||") {
        if ((l && bool_of(*l)) || (r && bool_of(*r))) return 1;
        if (l && r) return 0;
        return std::nullopt;
      }
      if (!l || !r) return std::nullopt;
      const std::int64_t a = *l, b = *r;
      if (e.op == "+") return a + b;
      if (e.op == "-") return a - b;
      if (e.op == "*") return a * b;
      if (e.op == "/") return b == 0 ? std::nullopt : std::optional<std::int64_t>(a / b);
      if (e.op == "%") return b == 0 ? std::nullopt : std::optional<std::int64_t>(a % b);
      if (e.op == "==") return a == b ? 1 : 0;
      if (e.op == "!=") return a != b ? 1 : 0;
      if (e.op == "<") return a < b ? 1 : 0;
      if (e.op == "<=") return a <= b ? 1 : 0;
      if (e.op == ">") return a > b ? 1 : 0;
      if (e.op == ">=") return a >= b ? 1 : 0;
      if (e.op == "&") return a & b;
      if (e.op == "|") return a | b;
      if (e.op == "^") return a ^ b;
      if (e.op == "<<" && b >= 0 && b < 62) return a << b;
      if (e.op == ">>" && b >= 0 && b < 62) return a >> b;
      return std::nullopt;
    }
    case K::Call:
    case K::Concat:
    case K::Replicate: return std::nullopt;
  }
  return std::nullopt;
}

namespace {

// -1: contains a signal or something unknown; 0: numbers only; 1: parameters (and numbers) with at least one parameter
int param_class(const Expr& e, const ParamMap& params) {
  using K = Expr::Kind;
  if (e.kind == K::Number) return 0;
  if (e.kind == K::Ident) return (!e.has_select && params.count(e.name) != 0) ? 1 : -1;
  if (e.kind == K::Call || e.kind == K::Concat || e.kind == K::Replicate) return -1;
  int cls = 0;
  for (const auto& k : e.kids) {
    const int c = param_class(*k, params);
    if (c < 0) return -1;
    cls = std::max(cls, c);
  }
  return cls;
}

}  // namespace

bool is_param_only(const Expr& e, const ParamMap& params) { return param_class(e, params) == 1; }

}  // namespace cutoff::drill
