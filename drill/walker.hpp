#pragma once
// Walks the statements of one Verilog module and collects every expression a mutation could apply to: right-hand
// sides of assignments and `if` conditions in live code. "Live" means not provably dead for the configured parameters:
// the branch of an `if`, a conditional-generate, or a one-hot `case (1'b1)` item whose condition folds to a constant
// is dead, as is an `initial` block.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "expr.hpp"
#include "lexer.hpp"

namespace cutoff::drill {

struct Root {
  enum class Kind : unsigned char { Rhs, Cond } kind = Kind::Rhs;
  const Expr* expr = nullptr;
  std::string lhs;  // Rhs: the assigned signal when it is a plain identifier (no select), otherwise empty
  int line = 0;     // line of the first token of the expression
};

struct Walk {
  std::vector<std::unique_ptr<Expr>> owned;  // keeps every parsed tree alive
  std::vector<Root> roots;                   // live expressions, in source order
  std::vector<std::string> warnings;         // expressions in dead code that could not be parsed
  ParamMap params;                           // parameters and localparams with their evaluated values
  std::size_t module_begin = 0;              // token index of `module`
  std::size_t port_list_open = 0;            // token index of the '(' that opens the port list
  std::size_t live_statements = 0, dead_statements = 0;
  std::string error;                         // empty on success
};

// `overrides` replace the default values of parameters (the configuration of the wrapper that instantiates the module).
Walk walk_module(const std::vector<Token>& toks, std::string_view source, const std::string& module, const ParamMap& overrides);

}  // namespace cutoff::drill
