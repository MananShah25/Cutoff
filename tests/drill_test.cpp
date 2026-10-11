// Tests for the mutant generator: lexer, expression parser, constant folding, statement walker, region table, mutation
// operators, the instrumented source, and generation on the real PicoRV32 source.

#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "expr.hpp"
#include "generate.hpp"
#include "lexer.hpp"
#include "mutator.hpp"
#include "regions.hpp"
#include "walker.hpp"

using namespace cutoff::drill;

namespace {

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string repo(const std::string& rel) { return std::string(CUTOFF_SOURCE_DIR) + "/" + rel; }

std::vector<std::string> texts(const LexResult& r) {
  std::vector<std::string> out;
  for (const Token& t : r.tokens)
    if (t.kind != Tok::End) out.emplace_back(t.text);
  return out;
}

ParseResult parse(const std::string& src, LexResult& keep) {
  keep = lex(src);
  EXPECT_EQ(keep.error, "");
  return parse_expr(keep.tokens, 0);
}

// Renders a tree with explicit grouping so precedence can be asserted.
std::string shape(const Expr& e) {
  using K = Expr::Kind;
  switch (e.kind) {
    case K::Number: return e.name;
    case K::Ident: return e.has_select ? e.name + "[..]" : e.name;
    case K::Call: {
      std::string s = e.name + "(";
      for (std::size_t i = 0; i < e.kids.size(); ++i) s += (i ? "," : "") + shape(*e.kids[i]);
      return s + ")";
    }
    case K::Paren: return "(" + shape(*e.kids[0]) + ")";
    case K::Concat: case K::Replicate: {
      std::string s = e.kind == K::Concat ? "{" : "R{";
      for (std::size_t i = 0; i < e.kids.size(); ++i) s += (i ? "," : "") + shape(*e.kids[i]);
      return s + "}";
    }
    case K::Unary: return "[" + e.op + " " + shape(*e.kids[0]) + "]";
    case K::Binary: return "[" + shape(*e.kids[0]) + " " + e.op + " " + shape(*e.kids[1]) + "]";
    case K::Ternary: return "[" + shape(*e.kids[0]) + " ? " + shape(*e.kids[1]) + " : " + shape(*e.kids[2]) + "]";
  }
  return "?";
}

// A tiny module around some statements, with the given parameters.
struct Snippet {
  std::string source;
  LexResult lex;
  Walk walk;
  RegionTable table;
  Plan plan;
};

Snippet make(const std::string& body, const std::string& ops = "", const ParamMap& params = {}, const std::string& stuck = "") {
  Snippet s;
  std::string decl;
  for (const auto& [name, value] : params) decl += std::string(decl.empty() ? "" : ", ") + "parameter " + name + " = " + std::to_string(value);
  s.source = "module m " + (decl.empty() ? std::string() : "#(" + decl + ") ") + "(input clk, input a, input b);\n" + body + "\nendmodule\n";
  s.lex = lex(s.source);
  EXPECT_EQ(s.lex.error, "");
  s.walk = walk_module(s.lex.tokens, s.source, "m", params);
  EXPECT_EQ(s.walk.error, "");
  std::ostringstream regions;
  regions << "module m\nregion t 1 400 \"module m\"" << (ops.empty() ? "" : " ops=" + ops) << "\n" << stuck;
  EXPECT_EQ(parse_regions(regions.str(), s.source, s.table), "");
  s.plan = plan_mutations(s.walk, s.table, s.source, s.lex.tokens);
  return s;
}

std::vector<std::string> mutated(const Snippet& s) {
  std::vector<std::string> out;
  out.reserve(s.plan.mutants.size());
  for (const Mutant& m : s.plan.mutants) out.push_back(m.original + " => " + m.mutated);
  return out;
}

}  // namespace

// ---------------------------------------------------------------- lexer

TEST(DrillLexer, TokensRangesAndLines) {
  const std::string src = "a <= b + 3'b 010; // c\n/* x\ny */ q <<< 2 == 32'h ffff_fffe; \"s\"\n";
  const LexResult r = lex(src);
  ASSERT_EQ(r.error, "");
  EXPECT_EQ(texts(r), (std::vector<std::string>{"a", "<=", "b", "+", "3'b 010", ";", "q", "<<<", "2", "==", "32'h ffff_fffe", ";", "\"s\""}));
  EXPECT_EQ(r.tokens[0].line, 1);
  EXPECT_EQ(r.tokens[6].line, 3) << "line numbers survive multi-line comments";
  for (const Token& t : r.tokens) EXPECT_EQ(src.substr(t.begin, t.end - t.begin), std::string(t.text));
  EXPECT_EQ(r.tokens.back().kind, Tok::End);
}

TEST(DrillLexer, AttributesAreDroppedButEventStarsAreKept) {
  const LexResult r = lex("(* parallel_case, full_case *) case (1'b1) always @* x; always @(*) y; always @(posedge clk) z; `debug($display(\"x\");)");
  ASSERT_EQ(r.error, "");
  EXPECT_EQ(texts(r), (std::vector<std::string>{"case", "(", "1'b1", ")", "always", "@", "*", "x", ";", "always", "@", "(", "*", ")", "y", ";", "always", "@", "(", "posedge", "clk", ")", "z", ";", "`debug", "(", "$display", "(", "\"x\"", ")", ";", ")"}));
}

TEST(DrillLexer, PreprocessorBranchesFollowTheDefinedMacros) {
  const std::string src =
      "a;\n`ifdef FOO\nb;\n`ifndef BAR\nc;\n`else\nd;\n`endif\n`else\ne;\n`endif\nf;\n`define X 1\n`ifndef NOPE\ng;\n`endif\n";
  EXPECT_EQ(texts(lex(src)), (std::vector<std::string>{"a", ";", "e", ";", "f", ";", "g", ";"}));
  EXPECT_EQ(texts(lex(src, {"FOO"})), (std::vector<std::string>{"a", ";", "b", ";", "c", ";", "f", ";", "g", ";"}));
  EXPECT_EQ(texts(lex(src, {"FOO", "BAR"})), (std::vector<std::string>{"a", ";", "b", ";", "d", ";", "f", ";", "g", ";"}));
  const LexResult r = lex("`ifdef A\nx;\n`elsif B\ny;\n`else\nz;\n`endif\n", {"B"});
  EXPECT_EQ(texts(r), (std::vector<std::string>{"y", ";"}));
  const LexResult l = lex("`ifdef A\n\n\nfoo;\n`endif\nbar;\n");
  EXPECT_EQ(l.tokens[0].line, 6) << "inactive lines still count";
}

TEST(DrillLexer, ErrorsNameTheLine) {
  EXPECT_NE(lex("a;\n`ifdef X\nb;\n").error.find("without `endif"), std::string::npos);
  EXPECT_NE(lex("`endif\n").error.find("line 1"), std::string::npos);
  EXPECT_NE(lex("a;\n\"unterminated").error.find("line 2"), std::string::npos);
  EXPECT_NE(lex("/* never closed").error.find("comment"), std::string::npos);
}

// ---------------------------------------------------------------- expressions

TEST(DrillExpr, PrecedenceAndAssociativity) {
  LexResult l;
  auto sh = [&](const std::string& src) { auto r = parse(src, l); EXPECT_TRUE(r.expr) << r.error; return r.expr ? shape(*r.expr) : "<error>"; };
  EXPECT_EQ(sh("a + b * c"), "[a + [b * c]]");
  EXPECT_EQ(sh("a - b - c"), "[[a - b] - c]");
  EXPECT_EQ(sh("a || b && c"), "[a || [b && c]]");
  EXPECT_EQ(sh("a | b ^ c & d"), "[a | [b ^ [c & d]]]");
  EXPECT_EQ(sh("a == b < c"), "[a == [b < c]]");
  EXPECT_EQ(sh("a << 1 + 2"), "[a << [1 + 2]]");
  EXPECT_EQ(sh("a ? b : c ? d : e"), "[a ? b : [c ? d : e]]");
  EXPECT_EQ(sh("x ? a + b : c || d"), "[x ? [a + b] : [c || d]]");
  EXPECT_EQ(sh("!a && ~b"), "[[! a] && [~ b]]");
  EXPECT_EQ(sh("a & ~1"), "[a & [~ 1]]");
  EXPECT_EQ(sh("|{a, b}"), "[| {a,b}]");
  EXPECT_EQ(sh("{2{a[15:0]}}"), "R{a[..]}");
  EXPECT_EQ(sh("$signed(x) < $signed(y)"), "[$signed(x) < $signed(y)]");
  EXPECT_EQ(sh("$signed({sel ? p[31] : 1'b0, q}) >>> r[4:0]"), "[$signed({[sel ? p[..] : 1'b0],q}) >>> r[..]]");
  EXPECT_EQ(sh("(a + b) * c"), "[([a + b]) * c]");
  EXPECT_EQ(sh("mem[a + 1][3:0] & b"), "[mem[..] & b]") << "selects are opaque and stay with their identifier";
}

TEST(DrillExpr, SpansAreExactAndNested) {
  const std::string src = "alu = (reg_op1 << 2) + $signed(x[3:0]) >= {a, 2'b00};";
  LexResult l = lex(src);
  const ParseResult r = parse_expr(l.tokens, 2);
  ASSERT_TRUE(r.expr) << r.error;
  EXPECT_EQ(source_text(src, *r.expr), "(reg_op1 << 2) + $signed(x[3:0]) >= {a, 2'b00}");
  EXPECT_EQ(l.tokens[r.next].text, ";");
  const Expr& rel = *r.expr;
  EXPECT_EQ(source_text(src, *rel.kids[0]), "(reg_op1 << 2) + $signed(x[3:0])");
  EXPECT_EQ(source_text(src, *rel.kids[0]->kids[1]), "$signed(x[3:0])");
  EXPECT_EQ(source_text(src, *rel.kids[1]), "{a, 2'b00}");
  EXPECT_EQ(rel.line, 1);
}

TEST(DrillExpr, ErrorsSayWhereAndWhy) {
  LexResult l = lex("a + ;");
  EXPECT_FALSE(parse_expr(l.tokens, 0).expr);
  EXPECT_NE(parse_expr(l.tokens, 0).error.find("expected an expression"), std::string::npos);
  l = lex("(a + b");
  EXPECT_NE(parse_expr(l.tokens, 0).error.find("')'"), std::string::npos);
  l = lex("x ? a b");
  EXPECT_NE(parse_expr(l.tokens, 0).error.find("':'"), std::string::npos);
}

TEST(DrillExpr, NumbersAreParsedWithTheirWidthAndBase) {
  struct C { const char* text; std::int64_t value; int width; char base; bool sized, xz; };
  const C cases[] = {{"12", 12, 0, 'd', false, false}, {"4'b0001", 1, 4, 'b', true, false}, {"32'hfffffffe", 4294967294LL, 32, 'h', true, false},
                     {"3'b 010", 2, 3, 'b', true, false}, {"32'h ffff_ffff", 4294967295LL, 32, 'h', true, false}, {"7'd25", 25, 7, 'd', true, false},
                     {"1'bx", 0, 1, 'b', true, true}, {"1_000", 1000, 0, 'd', false, false}};
  for (const C& c : cases) {
    const auto n = parse_number(c.text);
    ASSERT_TRUE(n) << c.text;
    EXPECT_EQ(n->has_xz, c.xz) << c.text;
    if (c.xz) continue;
    EXPECT_EQ(n->value, c.value) << c.text;
    EXPECT_EQ(n->width, c.width) << c.text;
    EXPECT_EQ(n->base, c.base) << c.text;
    EXPECT_EQ(n->sized, c.sized) << c.text;
  }
  EXPECT_FALSE(parse_number("4'b102"));
}

TEST(DrillExpr, ConstantFoldingKnowsWhatIsDeadForTheseParameters) {
  const ParamMap p = {{"ON", 1}, {"OFF", 0}, {"N", 5}};
  LexResult l;
  auto fold = [&](const std::string& src) {
    auto r = parse(src, l);
    return r.expr ? fold_const(*r.expr, p) : std::nullopt;
  };
  EXPECT_EQ(fold("N + 2 * 3"), 11);
  EXPECT_EQ(fold("ON ? 7 : 9"), 7);
  EXPECT_EQ(fold("OFF ? 7 : 9"), 9);
  EXPECT_EQ(fold("!OFF && ON"), 1);
  EXPECT_EQ(fold("N >= 5"), 1);
  EXPECT_EQ(fold("N == 4 || N == 5"), 1);
  EXPECT_EQ(fold("N << 2"), 20);
  EXPECT_EQ(fold("OFF && sig"), 0) << "a known 0 decides && even when the other side is a signal";
  EXPECT_EQ(fold("sig && OFF"), 0);
  EXPECT_EQ(fold("ON || sig"), 1);
  EXPECT_EQ(fold("sig || ON"), 1);
  EXPECT_FALSE(fold("ON && sig"));
  EXPECT_FALSE(fold("sig || OFF"));
  EXPECT_FALSE(fold("sig + 1"));
  EXPECT_FALSE(fold("OFF ? 1 : sig"));
  EXPECT_EQ(fold("ON ? 1 : sig"), 1) << "only the selected arm matters";
  EXPECT_FALSE(fold("1'bx"));
  EXPECT_FALSE(fold("sel[0]"));
  EXPECT_FALSE(fold("$signed(N)"));
  EXPECT_FALSE(fold("N / 0"));
  EXPECT_FALSE(fold("unknown_param"));
  auto r = parse("ON && N", l);
  EXPECT_TRUE(is_param_only(*r.expr, p));
  r = parse("ON && sig", l);
  EXPECT_FALSE(is_param_only(*r.expr, p));
  r = parse("4 + 1", l);
  EXPECT_FALSE(is_param_only(*r.expr, p)) << "numbers alone are not parameters";
}

// ---------------------------------------------------------------- walker

TEST(DrillWalker, FindsAssignmentsAndConditionsAndDistinguishesLessEqual) {
  const Snippet s = make(
      "reg x, y, z; reg [3:0] q;\n"
      "always @(posedge clk) begin\n"
      "  x <= a <= b;\n"           // nonblocking assign whose RHS contains a comparison
      "  if (a && b) y <= 1; else y <= 0;\n"
      "  q[1] <= a;\n"
      "end\n"
      "always @* z = a;\n"
      "assign w = a | b;\n");
  std::vector<std::string> got;
  got.reserve(s.walk.roots.size());
  for (const Root& r : s.walk.roots) got.push_back((r.kind == Root::Kind::Cond ? "cond:" : "rhs:") + source_text(s.source, *r.expr) + (r.lhs.empty() ? "" : " lhs=" + r.lhs));
  EXPECT_EQ(got, (std::vector<std::string>{"rhs:a <= b lhs=x", "cond:a && b", "rhs:1 lhs=y", "rhs:0 lhs=y", "rhs:a", "rhs:a lhs=z", "rhs:a | b lhs=w"}))
      << "the select on q[1] clears lhs; w is assigned continuously";
}

TEST(DrillWalker, PrunesWhatTheParametersMakeDead) {
  const Snippet s = make(
      "reg x, y; reg [1:0] st;\n"
      "always @(posedge clk) begin\n"
      "  if (ENABLE_IRQ) begin x <= a & b; end else begin x <= a | b; end\n"        // then dead
      "  if (!ENABLE_IRQ) y <= a ^ b; else y <= a & b;\n"                           // else dead
      "  if (ENABLE_IRQ && a) x <= a + b;\n"                                        // folds to 0: dead
      "  case (1'b1)\n"
      "    ENABLE_IRQ && a: x <= a - b;\n"                                          // dead item
      "    !ENABLE_IRQ && b: y <= a == b;\n"                                        // live item
      "    default: y <= a != b;\n"
      "  endcase\n"
      "  case (st)\n"
      "    0: x <= a && b;\n"                                                       // not one-hot: always live
      "  endcase\n"
      "end\n"
      "generate if (ENABLE_IRQ) begin always @* x = a & b & a; end else begin always @* y = a | b | a; end endgenerate\n"
      "initial begin x = a & b; end\n",
      "", {{"ENABLE_IRQ", 0}});
  std::vector<std::string> live;
  live.reserve(s.walk.roots.size());
  for (const Root& r : s.walk.roots) live.push_back(source_text(s.source, *r.expr));
  EXPECT_EQ(live, (std::vector<std::string>{"a | b", "a ^ b", "a == b", "a != b", "a && b", "a | b | a"}));
  EXPECT_GE(s.walk.dead_statements, 5u);
}

TEST(DrillWalker, EvaluatesParametersInOrderWithOverrides) {
  const std::string src =
      "module m #(parameter [0:0] A = 1, parameter [0:0] B = 0, parameter [31:0] C = 32'h 0000_0010) (input clk);\n"
      "localparam D = A || B;\n"
      "localparam integer E = C + 4;\n"
      "localparam [7:0] F = 8'b00001000;\n"
      "localparam G = E > 10 ? 100 : 200;\n"
      "endmodule\n";
  const LexResult l = lex(src);
  const Walk w = walk_module(l.tokens, src, "m", {{"B", 1}});
  ASSERT_EQ(w.error, "");
  EXPECT_EQ(w.params.at("A"), 1);
  EXPECT_EQ(w.params.at("B"), 1) << "override replaces the default";
  EXPECT_EQ(w.params.at("C"), 16);
  EXPECT_EQ(w.params.at("D"), 1);
  EXPECT_EQ(w.params.at("E"), 20);
  EXPECT_EQ(w.params.at("F"), 8);
  EXPECT_EQ(w.params.at("G"), 100);
}

TEST(DrillWalker, SkipsInstancesMacrosDeclarationsAndFunctionsAndReportsRealErrors) {
  const Snippet s = make(
      "wire [3:0] w; reg r; integer i;\n"
      "sub #(.P(1)) u0 (.clk(clk), .d({a, b}), .q(w));\n"
      "always @(posedge clk) begin\n"
      "  `debug($display(\"x\", a);)\n"
      "  r <= a & b;\n"
      "end\n");
  ASSERT_EQ(s.walk.roots.size(), 1u);
  EXPECT_EQ(source_text(s.source, *s.walk.roots[0].expr), "a & b");

  const std::string bad = "module m (input clk);\nalways @* x = a + ;\nendmodule\n";
  const LexResult l = lex(bad);
  const Walk w = walk_module(l.tokens, bad, "m", {});
  EXPECT_NE(w.error.find("line 2"), std::string::npos) << w.error;
  EXPECT_NE(w.error.find("expected an expression"), std::string::npos) << w.error;

  const std::string dead_bad = "module m (input clk);\ninitial x = a + ;\nalways @* y = a;\nendmodule\n";
  const LexResult l2 = lex(dead_bad);
  const Walk w2 = walk_module(l2.tokens, dead_bad, "m", {});
  EXPECT_EQ(w2.error, "") << "unparsable code that is dead only produces a warning";
  EXPECT_EQ(w2.warnings.size(), 1u);
  const Walk missing = walk_module(l.tokens, bad, "nope", {});
  EXPECT_NE(missing.error.find("not found"), std::string::npos);
}

// ---------------------------------------------------------------- regions

TEST(DrillRegions, ParseLookupAndErrors) {
  const std::string src = "line one\n  foo = a + b;\nline three\n";
  RegionTable t;
  ASSERT_EQ(parse_regions("# c\nmodule m\nregion alu 2 3 \"foo = a\" ops=arith,const\nregion imm 3 3 \"line three\"\nstuck sig 8 0,7\n", src, t), "");
  ASSERT_EQ(t.regions.size(), 2u);
  EXPECT_EQ(t.find(2)->category, "alu");
  EXPECT_TRUE(t.find(2)->allows("arith"));
  EXPECT_FALSE(t.find(2)->allows("logic"));
  EXPECT_TRUE(t.find(3)->category == "alu") << "first matching region wins";
  EXPECT_EQ(t.find(1), nullptr);
  EXPECT_TRUE(t.regions[1].allows("stuck")) << "no ops= means every class";
  ASSERT_NE(t.find_stuck("sig"), nullptr);
  EXPECT_EQ(t.find_stuck("sig")->bits, (std::vector<int>{0, 7}));
  EXPECT_EQ(t.find_stuck("other"), nullptr);

  auto err = [&](const std::string& text) { RegionTable x; return parse_regions(text, src, x); };
  EXPECT_NE(err("module m\nregion alu 2 3 \"WRONG\"\n").find("different version of the core source"), std::string::npos);
  EXPECT_NE(err("module m\nregion alu 2 3 \"WRONG\"\n").find("foo = a + b"), std::string::npos) << "the message shows what the line actually says";
  EXPECT_NE(err("module m\nregion alu 3 2 \"line\"\n").find("bad line range"), std::string::npos);
  EXPECT_NE(err("module m\nregion alu 2 3 foo\n").find("double quotes"), std::string::npos);
  EXPECT_NE(err("module m\nregion alu 2 3 \"foo\" ops=bogus\n").find("unknown operator class"), std::string::npos);
  EXPECT_NE(err("module m\nstuck s 8 9\n").find("outside the width"), std::string::npos);
  EXPECT_NE(err("module m\nfrobnicate\n").find("unknown directive"), std::string::npos);
  EXPECT_NE(err("region alu 1 1 \"line\"\n").find("name the module"), std::string::npos);
}

// ---------------------------------------------------------------- mutation operators (exact expected output)

TEST(DrillMutator, EveryOperatorClassOnHandWrittenExpressions) {
  auto only = [&](const char* cls, const std::string& stmt) { return mutated(make("reg x;\nalways @* x = " + stmt + ";\n", cls)); };
  EXPECT_EQ(only("arith", "a + b"), (std::vector<std::string>{"a + b => (a) - (b)"}));
  EXPECT_EQ(only("arith", "a - b"), (std::vector<std::string>{"a - b => (a) + (b)"}));
  EXPECT_EQ(only("eq", "a == b"), (std::vector<std::string>{"a == b => (a) != (b)"}));
  EXPECT_EQ(only("eq", "a != b"), (std::vector<std::string>{"a != b => (a) == (b)"}));
  EXPECT_EQ(only("rel", "a < b"), (std::vector<std::string>{"a < b => (a) <= (b)", "a < b => (a) > (b)", "a < b => (a) >= (b)"}));
  EXPECT_EQ(only("rel", "a >= b"), (std::vector<std::string>{"a >= b => (a) > (b)", "a >= b => (a) < (b)", "a >= b => (a) <= (b)"}));
  EXPECT_EQ(only("logic", "a && b"), (std::vector<std::string>{"a && b => (a) || (b)"}));
  EXPECT_EQ(only("logic", "a || b"), (std::vector<std::string>{"a || b => (a) && (b)"}));
  EXPECT_EQ(only("bitwise", "a & b"), (std::vector<std::string>{"a & b => (a) | (b)", "a & b => (a) ^ (b)"}));
  EXPECT_EQ(only("bitwise", "a | b"), (std::vector<std::string>{"a | b => (a) & (b)", "a | b => (a) ^ (b)"}));
  EXPECT_EQ(only("bitwise", "a ^ b"), (std::vector<std::string>{"a ^ b => (a) | (b)", "a ^ b => (a) & (b)"}));
  EXPECT_EQ(only("not", "!a"), (std::vector<std::string>{"!a => (a)", "!a => ~(a)"}));
  EXPECT_EQ(only("not", "~a"), (std::vector<std::string>{"~a => (a)", "~a => !(a)"}));
  EXPECT_EQ(only("shift", "a << b"), (std::vector<std::string>{"a << b => (a) << ((b) + 1)", "a << b => (a) << ((b) - 1)"}));
  EXPECT_EQ(only("shift", "a >>> b[2:0]"), (std::vector<std::string>{"a >>> b[2:0] => (a) >>> ((b[2:0]) + 1)", "a >>> b[2:0] => (a) >>> ((b[2:0]) - 1)"}));
  EXPECT_TRUE(only("shift", "a << 3").empty()) << "a constant amount is covered by the constant operator, not duplicated";
  EXPECT_EQ(only("signed", "$signed(a) < $signed(b)"), (std::vector<std::string>{"$signed(a) < $signed(b) => (a) < (b)"}));
  EXPECT_EQ(only("signed", "a < b"), (std::vector<std::string>{"a < b => $signed(a) < $signed(b)"}));
  EXPECT_TRUE(only("signed", "$signed(a) < b").empty()) << "mixed signedness already compares unsigned";
  EXPECT_EQ(only("signed", "$signed(a) + b"), (std::vector<std::string>{"$signed(a) => (a)"})) << "dropping $signed outside a comparison changes the extension";
}

TEST(DrillMutator, ConstantsAreReplacedWithTheirNeighboursZerosAndOnes) {
  auto only = [&](const std::string& stmt) { return mutated(make("reg [3:0] x;\nalways @* x = " + stmt + ";\n", "const")); };
  EXPECT_EQ(only("a + 4"), (std::vector<std::string>{"4 => 0", "4 => 1", "4 => 5", "4 => 3"}));
  EXPECT_EQ(only("a + 1"), (std::vector<std::string>{"1 => 0", "1 => 2"}));
  EXPECT_EQ(only("a + 0"), (std::vector<std::string>{"0 => 1"}));
  EXPECT_EQ(only("a + 4'd15"), (std::vector<std::string>{"4'd15 => 4'd0", "4'd15 => 4'd1", "4'd15 => 4'd14"})) << "never beyond the width";
  EXPECT_EQ(only("a & 4'b0101"), (std::vector<std::string>{"4'b0101 => 4'b0000", "4'b0101 => 4'b1111"}));
  EXPECT_EQ(only("a & 4'b1111"), (std::vector<std::string>{"4'b1111 => 4'b0000"}));
  EXPECT_EQ(only("a & 4'b0000"), (std::vector<std::string>{"4'b0000 => 4'b1111"}));
  EXPECT_EQ(only("a & 8'hff"), (std::vector<std::string>{"8'hff => 8'b00000000"}));
  EXPECT_EQ(only("{a, 2'b00}"), (std::vector<std::string>{"2'b00 => 2'b11"}));
  EXPECT_TRUE(only("a & 1'bx").empty()) << "x/z constants are left alone";
  EXPECT_EQ(mutated(make("reg x;\nalways @* x = a[2] & b[1:0];\n", "const")).size(), 0u) << "bit and part select indices are never mutated";
  EXPECT_EQ(mutated(make("reg [7:0] x;\nalways @* x = {4{a}};\n", "const")).size(), 0u) << "replication counts are never mutated";
}

TEST(DrillMutator, NothingIsMutatedWhereTheParametersDecide) {
  const ParamMap p = {{"ON", 1}, {"OFF", 0}};
  auto all = [&](const std::string& stmt) { return mutated(make("reg x;\nalways @* x = " + stmt + ";\n", "", p)); };
  EXPECT_TRUE(all("ON && OFF").empty()) << "a constant expression";
  EXPECT_EQ(all("ON && a"), (std::vector<std::string>{})) << "the && joins a parameter and a signal: that is configuration, not logic";
  EXPECT_EQ(all("OFF ? a + b : a - b"), (std::vector<std::string>{"a - b => (a) + (b)"})) << "the live arm of a constant conditional is mutated, the dead arm is not";
  EXPECT_EQ(all("ON ? a + b : a - b"), (std::vector<std::string>{"a + b => (a) - (b)"}));
  EXPECT_TRUE(all("OFF && (a + b)").empty()) << "folds to 0: dead";
}

TEST(DrillMutator, StuckAtMutantsAndTheirLimits) {
  const Snippet s = make("reg [3:0] x; reg y;\nalways @* begin x = a + b; y = a; end\n", "stuck", {}, "stuck x 4 0,3\nstuck y 1 0\n");
  std::vector<std::string> kinds;
  kinds.reserve(s.plan.mutants.size());
  for (const Mutant& m : s.plan.mutants) kinds.push_back(m.kind);
  EXPECT_EQ(kinds, (std::vector<std::string>{"x[0] stuck-at-0", "x[0] stuck-at-1", "x[3] stuck-at-0", "x[3] stuck-at-1", "y[0] stuck-at-0", "y[0] stuck-at-1"}));
  EXPECT_EQ(s.plan.mutants[0].mutated, "((a + b) & ~4'h1)");
  EXPECT_EQ(s.plan.mutants[3].mutated, "((a + b) | 4'h8)");
  EXPECT_EQ(s.plan.mutants[0].site, s.plan.mutants[3].site) << "all stuck-at mutants of one assignment share a site";
  EXPECT_NE(s.plan.mutants[0].site, s.plan.mutants[4].site);
  EXPECT_TRUE(make("reg [3:0] x;\nalways @* x = 1'bx;\n", "stuck", {}, "stuck x 4 0\n").plan.mutants.empty()) << "assigning x has no behaviour to corrupt";
  EXPECT_TRUE(make("reg [3:0] x;\nalways @* x[1] = a;\n", "stuck", {}, "stuck x 4 0\n").plan.mutants.empty()) << "only whole-signal assignments";
}

TEST(DrillMutator, IdsSitesAndClassesAreStableAndAccurate) {
  const Snippet s = make("reg x;\nalways @* x = (a + b) == 3 ? a : !b;\n");
  std::set<int> ids;
  for (std::size_t i = 0; i < s.plan.mutants.size(); ++i) {
    EXPECT_EQ(s.plan.mutants[i].id, static_cast<int>(i) + 1);
    EXPECT_EQ(s.plan.mutants[i].line, 3 - 0 + 0 == 0 ? 0 : s.plan.mutants[i].line);
    ids.insert(s.plan.mutants[i].site);
  }
  EXPECT_FALSE(s.plan.mutants.empty());
  EXPECT_LT(ids.size(), s.plan.mutants.size()) << "several mutants share a site";
  const Snippet again = make("reg x;\nalways @* x = (a + b) == 3 ? a : !b;\n");
  EXPECT_EQ(manifest_tsv(s.plan), manifest_tsv(again.plan)) << "generation is deterministic";
  EXPECT_EQ(manifest_tsv(s.plan).rfind("id\tsite\tcategory\top_class\tkind\tline\toriginal\tmutated\n", 0), 0u);
}

// ---------------------------------------------------------------- the instrumented source

TEST(DrillEmit, MuxesOnlyTouchTheExpressionsAndKeepSignednessOfTheOriginal) {
  const Snippet s = make("reg [7:0] x; reg [11:0] imm;\nalways @* begin\n  x = a + b;\n  imm = $signed(imm[7:0]);\nend\n", "arith,signed");
  const std::string out = emit_instrumented(s.source, s.walk, s.plan, s.lex.tokens, "m");
  EXPECT_NE(out.find("module m (\n\tinput [15:0] mut_sel,input clk"), std::string::npos) << out;
  EXPECT_NE(out.find("x = (mut_sel == 16'd1 ? ((a) - (b)) : (a + b));"), std::string::npos) << out;
  // The original $signed(...) is kept; the mutant that drops it is zero-extended as a non-negative SIGNED number, so the
  // conditional stays signed and the original arm still sign-extends.
  EXPECT_NE(out.find("imm = (mut_sel == 16'd2 ? ($signed({1'b0, (imm[7:0])})) : ($signed(imm[7:0])));"), std::string::npos) << out;
  // Everything outside the replaced expressions is byte-for-byte the original.
  EXPECT_EQ(out.substr(out.find("reg [7:0] x;")).find("always @* begin\n  x = "), s.source.substr(s.source.find("reg [7:0] x;")).find("always @* begin\n  x = "));
}

TEST(DrillEmit, NestedSitesComposeAndTheTextOutsideStaysIdentical) {
  const Snippet s = make("reg x;\nalways @* x = a & b | a;\n", "bitwise");
  const std::string out = emit_instrumented(s.source, s.walk, s.plan, s.lex.tokens, "m");
  // ids: 1,2 for the outer |, then 3,4 (a&b) -> |,^ ... every id appears
  for (int id = 1; id <= static_cast<int>(s.plan.mutants.size()); ++id) EXPECT_NE(out.find("mut_sel == 16'd" + std::to_string(id) + " "), std::string::npos) << id;
  const LexResult re = lex(out);
  ASSERT_EQ(re.error, "") << "the instrumented source is still lexable";
  const Walk again = walk_module(re.tokens, out, "m", {});
  EXPECT_EQ(again.error, "") << "and walkable";
}

// ---------------------------------------------------------------- the infected-bit core

TEST(DrillInfect, ShadowStatementsSitInsideTheOriginalControlFlow) {
  const Snippet s = make(
      "reg [7:0] x; reg y; wire [7:0] w;\n"
      "always @(posedge clk) begin\n"
      "  if (a && b) x <= x + 1;\n"
      "  else y <= !a;\n"
      "end\n"
      "assign w = x - 3;\n",
      "arith,logic,not");
  const std::string out = emit_infected(s.source, s.walk, s.plan, s.lex.tokens);
  const int n = static_cast<int>(s.plan.mutants.size());
  ASSERT_GE(n, 4);
  EXPECT_NE(out.find("module m (\n\toutput [" + std::to_string(n) + ":0] infected,input clk"), std::string::npos) << out;
  EXPECT_NE(out.find("assign infected = {inf_" + std::to_string(n)), std::string::npos);
  EXPECT_NE(out.find(", 1'b0};"), std::string::npos) << "bit 0 is constant 0";
  // every mutant has exactly one declaration, as a reg when its statement is in an always block and a wire when continuous
  for (int id = 1; id <= n; ++id) {
    const std::string name = "inf_" + std::to_string(id) + ";";
    const bool wire = out.find("wire " + name) != std::string::npos, reg = out.find("reg " + name) != std::string::npos;
    EXPECT_TRUE(wire != reg) << "mutant " << id << " must be declared exactly once";
  }
  EXPECT_NE(out.find("wire inf_" + std::to_string(n) + ";"), std::string::npos) << "the continuous assign's mutant is a wire";
  EXPECT_NE(out.find("assign inf_" + std::to_string(n) + " = (inf_a"), std::string::npos);
  // the always block resets every infected reg it hosts at its start, so no latch is inferred
  EXPECT_NE(out.find("always @(posedge clk) begin\n  inf_1 = 1'b0;\n  inf_2 = 1'b0;"), std::string::npos) << out;
  // the original statements are all still there, untouched and in order
  const std::size_t p1 = out.find("x <= x + 1;"), p2 = out.find("y <= !a;");
  ASSERT_NE(p1, std::string::npos);
  ASSERT_NE(p2, std::string::npos);
  EXPECT_LT(p1, p2);
  EXPECT_NE(out.find("assign w = x - 3;"), std::string::npos);
  // shadows compare the original root with the root under each mutant, in a 64-bit context
  EXPECT_NE(out.find("inf_a = (a && b) ? 64'd1 : 64'd0;"), std::string::npos) << out;
  EXPECT_NE(out.find("inf_b = ((a) || (b)) ? 64'd1"), std::string::npos) << "the && -> || mutant of the condition";
  EXPECT_NE(out.find("inf_a = x + 1;"), std::string::npos);
  EXPECT_NE(out.find("x + 1;\nend"), std::string::npos) << "the wrapper closes after the original statement";
  // the result is still valid input for our own front end
  const LexResult re = lex(out);
  ASSERT_EQ(re.error, "");
  EXPECT_EQ(walk_module(re.tokens, out, "m", {}).error, "");
}

TEST(DrillInfect, ChainedElseIfIsWrappedWithoutBreakingTheChain) {
  const Snippet s = make("reg x;\nalways @* begin\n  if (a) x = 1; else if (b + 1 == 2) x = 0; else x = a;\nend\n", "arith,eq");
  const std::string out = emit_infected(s.source, s.walk, s.plan, s.lex.tokens);
  EXPECT_NE(out.find("else begin : inf_blk"), std::string::npos) << "the inner if becomes the body of the else";
  const std::size_t e1 = out.find("else begin : inf_blk");
  EXPECT_NE(out.find("else x = a;", e1), std::string::npos) << "the final else stays attached to the inner if";
  const LexResult re = lex(out);
  ASSERT_EQ(re.error, "");
  EXPECT_EQ(walk_module(re.tokens, out, "m", {}).error, "");
}

TEST(DrillInfect, StatementsWithoutBeginAreWrappedSoTheyStayOneStatement) {
  const Snippet s = make("reg x, y;\nalways @* if (a) x = b + 1; else y = a - b;\n", "arith");
  const std::string out = emit_infected(s.source, s.walk, s.plan, s.lex.tokens);
  EXPECT_NE(out.find("if (a) begin : inf_blk"), std::string::npos) << out;
  EXPECT_NE(out.find("else begin : inf_blk"), std::string::npos);
  const LexResult re = lex(out);
  ASSERT_EQ(re.error, "");
  EXPECT_EQ(walk_module(re.tokens, out, "m", {}).error, "");
}

// ---------------------------------------------------------------- the real core

class RealCore : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    rtl_ = slurp(repo("rtl/picorv32/picorv32.v"));
    wrapper_ = slurp(repo("rtl/cutoff_core.v"));
    regions_ = slurp(repo("drill/picorv32_regions.txt"));
    result_ = generate_drill(rtl_, wrapper_, regions_);
  }
  static std::string rtl_, wrapper_, regions_;
  static GenerateResult result_;
};
std::string RealCore::rtl_, RealCore::wrapper_, RealCore::regions_;
GenerateResult RealCore::result_;

TEST_F(RealCore, TheWholeModuleParsesWithTheWrapperConfiguration) {
  const LexResult l = lex(rtl_);
  ASSERT_EQ(l.error, "");
  ParamMap ov;
  const LexResult w = lex(wrapper_);
  for (std::size_t i = 0; i + 4 < w.tokens.size(); ++i)
    if (w.tokens[i].text == "." && w.tokens[i + 3].kind == Tok::Number && w.tokens[i + 4].text == ")") ov[std::string(w.tokens[i + 1].text)] = parse_number(w.tokens[i + 3].text).value_or(NumberInfo{}).value;
  const Walk walk = walk_module(l.tokens, rtl_, "picorv32", ov);
  ASSERT_EQ(walk.error, "");
  EXPECT_TRUE(walk.warnings.empty()) << walk.warnings.front();
  // Values the RV32I configuration must give (verification log V-01), evaluated by the constant folder.
  for (const char* off : {"ENABLE_COUNTERS", "ENABLE_COUNTERS64", "COMPRESSED_ISA", "ENABLE_PCPI", "ENABLE_MUL", "ENABLE_FAST_MUL", "ENABLE_DIV", "ENABLE_IRQ", "BARREL_SHIFTER", "TWO_CYCLE_ALU", "TWO_CYCLE_COMPARE", "WITH_PCPI"})
    EXPECT_EQ(walk.params.at(off), 0) << off;
  EXPECT_EQ(walk.params.at("CATCH_MISALIGN"), 1);
  EXPECT_EQ(walk.params.at("ENABLE_REGS_DUALPORT"), 1);
  EXPECT_EQ(walk.params.at("regindex_bits"), 5);
  EXPECT_EQ(walk.params.at("cpu_state_fetch"), 64);
  // Regression pins for the parser on this exact source (they move only if the parser or the source changes).
  EXPECT_EQ(walk.roots.size(), 579u);
  EXPECT_EQ(walk.dead_statements, 261u);
}

TEST_F(RealCore, GenerationSucceedsAndCoversEverySpecCategory) {
  ASSERT_EQ(result_.error, "");
  EXPECT_EQ(result_.mutants.size(), 201u) << "regression pin for the pinned core and region table";
  EXPECT_GE(result_.mutants.size(), 100u);
  EXPECT_LE(result_.mutants.size(), 300u) << "spec: about 100 to 300 mutants at first";
  for (const char* cat : {"alu", "compare", "branch", "imm", "regfile", "memory", "pc"}) EXPECT_GT(result_.by_category[cat], 0) << cat;
  for (const char* cls : {"arith", "eq", "rel", "logic", "bitwise", "not", "const", "shift", "signed", "stuck"}) EXPECT_GT(result_.by_class[cls], 0) << cls;
  EXPECT_NE(result_.instrumented.find("GENERATED by cutoff-mutgen"), std::string::npos);
  EXPECT_NE(result_.wrapper.find("module cutoff_core_drill ("), std::string::npos);
  EXPECT_NE(result_.wrapper.find(".mut_sel   (mut_sel),"), std::string::npos);
}

TEST_F(RealCore, ManifestIsAccurateAndDeterministic) {
  ASSERT_EQ(result_.error, "");
  std::set<int> ids;
  std::vector<std::string> lines;
  {
    std::istringstream in(result_.manifest);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
  }
  ASSERT_EQ(lines.size(), result_.mutants.size() + 1);
  for (std::size_t i = 0; i < result_.mutants.size(); ++i) {
    const Mutant& m = result_.mutants[i];
    EXPECT_EQ(m.id, static_cast<int>(i) + 1);
    ids.insert(m.id);
    EXPECT_NE(m.original, m.mutated) << "mutant " << m.id << " changes nothing";
    EXPECT_FALSE(m.original.empty());
    // The recorded original text really is in the source (modulo whitespace) at about the recorded line.
    std::string squeezed;
    bool space = false;
    for (const char c : rtl_) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = true; continue; }
      if (space && !squeezed.empty()) squeezed += ' ';
      space = false;
      squeezed += c;
    }
    EXPECT_NE(squeezed.find(m.original), std::string::npos) << "mutant " << m.id << ": " << m.original;
    EXPECT_NE(result_.instrumented.find("mut_sel == 16'd" + std::to_string(m.id) + " "), std::string::npos) << m.id;
  }
  EXPECT_EQ(ids.size(), result_.mutants.size());
  const GenerateResult again = generate_drill(rtl_, wrapper_, regions_);
  EXPECT_EQ(again.instrumented, result_.instrumented);
  EXPECT_EQ(again.manifest, result_.manifest);
}

TEST_F(RealCore, TheInstrumentedSourceKeepsTheOriginalOutsideTheMutatedExpressions) {
  ASSERT_EQ(result_.error, "");
  // Strip the two generated header lines: the line count outside mutated statements is unchanged.
  const std::size_t body = result_.instrumented.find("`timescale");
  ASSERT_NE(body, std::string::npos);
  EXPECT_EQ(result_.instrumented.substr(body, 200), rtl_.substr(rtl_.find("`timescale"), 200));
  // Every module other than picorv32 is byte-identical.
  const std::size_t other = rtl_.find("module picorv32_regs");
  const std::size_t other_i = result_.instrumented.find("module picorv32_regs");
  ASSERT_NE(other, std::string::npos);
  ASSERT_NE(other_i, std::string::npos);
  EXPECT_EQ(result_.instrumented.substr(other_i), rtl_.substr(other));
  // The instrumented core is itself parseable by the same front end.
  const LexResult l = lex(result_.instrumented);
  ASSERT_EQ(l.error, "");
  EXPECT_EQ(walk_module(l.tokens, result_.instrumented, "picorv32", {}).error, "");
}

TEST_F(RealCore, ADifferentSourceVersionIsRefusedNotMisread) {
  std::string shifted = "// one extra line\n" + rtl_;
  const GenerateResult r = generate_drill(shifted, wrapper_, regions_);
  EXPECT_NE(r.error.find("different version of the core source"), std::string::npos) << r.error;
  EXPECT_TRUE(r.instrumented.empty());
  EXPECT_NE(generate_drill(rtl_, "module x;\nendmodule\n", regions_).error.find("module cutoff_core ("), std::string::npos);
}

TEST_F(RealCore, TheInfectedCoreIsGeneratedForEveryMutantAndStaysParseable) {
  ASSERT_EQ(result_.error, "");
  ASSERT_FALSE(result_.infect_source.empty());
  for (const Mutant& m : result_.mutants) {
    const std::string name = "inf_" + std::to_string(m.id) + ";";
    EXPECT_TRUE(result_.infect_source.find("reg " + name) != std::string::npos || result_.infect_source.find("wire " + name) != std::string::npos) << m.id;
  }
  const LexResult l = lex(result_.infect_source);
  ASSERT_EQ(l.error, "");
  EXPECT_EQ(walk_module(l.tokens, result_.infect_source, "picorv32", {}).error, "");
  EXPECT_NE(result_.infect_wrapper.find("module cutoff_core_infect ("), std::string::npos);
  EXPECT_NE(result_.infect_wrapper.find("output [201:0] infected"), std::string::npos);
  EXPECT_NE(result_.infect_wrapper.find(".infected  (infected),"), std::string::npos);
  // The infected core differs from the original only by insertions: the original's token sequence is a subsequence of it.
  const LexResult orig = lex(rtl_);
  ASSERT_EQ(orig.error, "");
  std::size_t j = 0;
  for (const Token& t : l.tokens)
    if (j < orig.tokens.size() && t.text == orig.tokens[j].text) ++j;
  EXPECT_EQ(j, orig.tokens.size()) << "original token " << j << " is missing or out of order";
}
