#pragma once
// Turns the live expressions of a module into numbered mutants and into the instrumented source in which every mutant
// is selectable at run time: each mutated expression E becomes (mut_sel == 16'dK ? <mutation K of E> : ... : E).
// mut_sel == 0 selects the original design. Mutation operators follow the Verilog operator list used in the literature
// (S20 of the spec) plus the domain additions: signed/unsigned comparison, shift-amount off-by-one and stuck-at bits.

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "regions.hpp"
#include "walker.hpp"

namespace cutoff::drill {

struct VariantSpec {
  enum class Kind : unsigned char { BinaryOp, ShiftPlus, ShiftMinus, SignedFlip, UnaryDrop, UnarySwap, Number, DropSigned, Stuck } kind = Kind::BinaryOp;
  int id = 0;
  std::string text;  // new operator, or the replacement literal
  int bit = 0, width = 0;
  bool one = false;  // Stuck: stuck-at-1 (else stuck-at-0)
};

struct Mutant {
  int id = 0;
  int site = 0;          // mutants of the same expression node share a site (for cluster resampling later)
  std::string category;  // region category
  std::string op_class;  // arith, eq, rel, logic, bitwise, not, const, shift, signed, stuck
  std::string kind;      // human-readable, e.g. "+ -> -"
  int line = 0;
  std::string original;  // source text of the mutated expression
  std::string mutated;   // what it becomes (children left as in the source)
};

struct Plan {
  std::vector<Mutant> mutants;
  std::unordered_map<const Expr*, std::vector<VariantSpec>> variants;
  std::vector<std::pair<const Root*, int>> unused;
};

Plan plan_mutations(const Walk& walk, const RegionTable& table, std::string_view source, const std::vector<Token>& toks);

// The module source with every root expression replaced by its selectable form.
std::string emit_instrumented(std::string_view source, const Walk& walk, const Plan& plan, const std::vector<Token>& toks, const std::string& module);

// The golden copy of the module with one "infected" bit per mutant (bit K of the new `infected` output; bit 0 is 0).
// Bit K is set when, at a point where the statement owning mutant K's expression really executes, that statement's
// expression would evaluate differently under mutant K than it does in the original. The shadow evaluations are inserted
// inside the original control flow (wrapping the statement, or resetting at the start of the always block), never change
// the behaviour of the module, and are compared in a 64-bit context so signedness and truncation cannot hide a difference.
// If no bit of mutant K is ever set during a run, a run of mutant K on the same program is identical to the golden run.
std::string emit_infected(std::string_view source, const Walk& walk, const Plan& plan, const std::vector<Token>& toks);

// manifest.tsv: id, site, category, op_class, kind, line, original, mutated.
std::string manifest_tsv(const Plan& plan);

}  // namespace cutoff::drill
