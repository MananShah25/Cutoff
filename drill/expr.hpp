#pragma once
// Verilog expression trees with exact source spans, plus a constant folder for parameter-only subexpressions.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lexer.hpp"

namespace cutoff::drill {

struct Expr {
  enum class Kind : unsigned char {
    Number,     // 4'b0001
    Ident,      // name, with any [...] selects folded in (the selects are opaque and never mutated)
    Call,       // $signed(x), $anyseq: `name` is the system function, kids are its arguments
    Paren,      // ( x )
    Concat,     // { a, b }
    Replicate,  // { n { a, b } }: the count is opaque, kids are the elements
    Unary,      // ! ~ - + and the reduction operators & | ^ ~& ~| ~^
    Binary,
    Ternary,    // cond ? a : b, kids = [cond, a, b]
  };
  Kind kind = Kind::Number;
  std::size_t tok_begin = 0, tok_end = 0;  // token range [begin, end)
  std::size_t src_begin = 0, src_end = 0;  // byte range in the source text
  int line = 0;                            // line of the first token (of the operator for Unary/Binary)
  std::string op;                          // operator text (Unary, Binary)
  std::string name;                        // identifier or system function name
  bool has_select = false;                 // Ident followed by [..]
  std::vector<std::unique_ptr<Expr>> kids;
};

struct ParseResult {
  std::unique_ptr<Expr> expr;
  std::size_t next = 0;  // index of the first token after the expression
  std::string error;     // empty on success
};

// Parses one expression starting at token `i`. The caller checks that `next` is where it expects the expression to end.
ParseResult parse_expr(const std::vector<Token>& toks, std::size_t i);

using ParamMap = std::map<std::string, std::int64_t>;

// Value of a parameter-only expression, or nullopt when it involves signals, unsupported operators or x/z.
std::optional<std::int64_t> fold_const(const Expr& e, const ParamMap& params);

// True if every identifier in the expression is a known parameter and at least one identifier occurs.
bool is_param_only(const Expr& e, const ParamMap& params);

// The exact source text of an expression.
std::string source_text(std::string_view source, const Expr& e);

// Numeric value of a literal such as 12, 4'b0001, 32'hfffffffe (nullopt for x/z); `width` is 0 when unsized.
struct NumberInfo {
  std::int64_t value = 0;
  int width = 0;
  char base = 'd';
  bool sized = false;
  bool has_xz = false;
};
std::optional<NumberInfo> parse_number(std::string_view text);

}  // namespace cutoff::drill
