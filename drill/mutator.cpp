#include "mutator.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace cutoff::drill {

namespace {

using K = Expr::Kind;
using VK = VariantSpec::Kind;

std::string squeeze(const std::string& s) {
  std::string out;
  bool space = false;
  for (const char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = true; continue; }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

struct Planner {
  const Walk& walk;
  const RegionTable& table;
  std::string_view src;
  Plan plan;
  int next_id = 1;
  int next_site = 1;

  const Region* region = nullptr;

  bool allowed(const char* cls) const { return region != nullptr && region->allows(cls); }

  bool param_operand(const Expr& e) const { return is_param_only(e, walk.params); }

  int add(const Expr& node, VariantSpec v, const char* op_class, const std::string& kind, int site) {
    v.id = next_id++;
    plan.variants[&node].push_back(v);
    Mutant m;
    m.id = v.id;
    m.site = site;
    m.category = region->category;
    m.op_class = op_class;
    m.kind = kind;
    m.line = node.line;
    m.original = squeeze(source_text(src, node));
    plan.mutants.push_back(std::move(m));
    return v.id;
  }

  void visit(const Expr& e, const Expr* parent) {
    // A subtree that folds to a constant for these parameters has nothing to mutate.
    if (e.kind != K::Number && fold_const(e, walk.params)) return;
    int site = 0;
    auto site_id = [&] { if (site == 0) site = next_site++; return site; };

    switch (e.kind) {
      case K::Binary: {
        const Expr& l = *e.kids[0];
        const Expr& r = *e.kids[1];
        const bool skip = param_operand(l) || param_operand(r);
        if (!skip) binary_variants(e, site_id);
        break;
      }
      case K::Unary: {
        if (allowed("not") && !param_operand(*e.kids[0]) && (e.op == "!" || e.op == "~")) {
          VariantSpec drop;
          drop.kind = VK::UnaryDrop;
          add(e, drop, "not", "drop " + e.op, site_id());
          VariantSpec swap;
          swap.kind = VK::UnarySwap;
          swap.text = e.op == "!" ? "~" : "!";
          add(e, swap, "not", e.op + " -> " + swap.text, site_id());
        }
        break;
      }
      case K::Call: {
        const bool in_relational = parent != nullptr && parent->kind == K::Binary && (parent->op == "<" || parent->op == "<=" || parent->op == ">" || parent->op == ">=");
        if (allowed("signed") && e.name == "$signed" && e.kids.size() == 1 && !in_relational) {
          VariantSpec v;
          v.kind = VK::DropSigned;
          add(e, v, "signed", "drop $signed (zero- instead of sign-extend)", site_id());
        }
        break;
      }
      case K::Number: number_variants(e, parent, site_id); break;
      default: break;
    }

    // Dead arms of a conditional with a constant condition are not visited.
    if (e.kind == K::Ternary) {
      const auto c = fold_const(*e.kids[0], walk.params);
      if (c) {
        visit(*e.kids[*c != 0 ? 1 : 2], &e);
        return;
      }
    }
    if (e.kind == K::Replicate || e.kind == K::Ident) {
      for (const auto& k : e.kids) visit(*k, &e);
      return;
    }
    for (const auto& k : e.kids) visit(*k, &e);
  }

  void binary_variants(const Expr& e, const std::function<int()>& site_id) {
    const std::string& op = e.op;
    auto swap_to = [&](const char* cls, std::initializer_list<const char*> targets) {
      if (!allowed(cls)) return;
      for (const char* t : targets) {
        VariantSpec v;
        v.kind = VK::BinaryOp;
        v.text = t;
        add(e, v, cls, op + " -> " + t, site_id());
      }
    };
    if (op == "+") swap_to("arith", {"-"});
    else if (op == "-") swap_to("arith", {"+"});
    else if (op == "==") swap_to("eq", {"!="});
    else if (op == "!=") swap_to("eq", {"=="});
    else if (op == "<") swap_to("rel", {"<=", ">", ">="});
    else if (op == "<=") swap_to("rel", {"<", ">", ">="});
    else if (op == ">") swap_to("rel", {">=", "<", "<="});
    else if (op == ">=") swap_to("rel", {">", "<", "<="});
    else if (op == "&&") swap_to("logic", {"||"});
    else if (op == "||") swap_to("logic", {"&&"});
    else if (op == "&") swap_to("bitwise", {"|", "^"});
    else if (op == "|") swap_to("bitwise", {"&", "^"});
    else if (op == "^") swap_to("bitwise", {"|", "&"});
    else if ((op == "<<" || op == ">>" || op == ">>>" || op == "<<<") && allowed("shift") && e.kids[1]->kind != K::Number) {
      VariantSpec p;
      p.kind = VK::ShiftPlus;
      add(e, p, "shift", op + " amount + 1", site_id());
      VariantSpec m;
      m.kind = VK::ShiftMinus;
      add(e, m, "shift", op + " amount - 1", site_id());
    }
    if ((op == "<" || op == "<=" || op == ">" || op == ">=") && allowed("signed")) {
      const bool l_signed = e.kids[0]->kind == K::Call && e.kids[0]->name == "$signed";
      const bool r_signed = e.kids[1]->kind == K::Call && e.kids[1]->name == "$signed";
      if (l_signed && r_signed) {
        VariantSpec v;
        v.kind = VK::SignedFlip;
        v.one = false;  // strip: compare unsigned
        add(e, v, "signed", op + " signed -> unsigned", site_id());
      } else if (!l_signed && !r_signed) {
        VariantSpec v;
        v.kind = VK::SignedFlip;
        v.one = true;  // wrap: compare signed
        add(e, v, "signed", op + " unsigned -> signed", site_id());
      }
    }
  }

  void number_variants(const Expr& e, const Expr* parent, const std::function<int()>& site_id) {
    if (!allowed("const")) return;
    const auto n = parse_number(e.name);
    if (!n || n->has_xz) return;
    if (e.name.empty() || e.name[0] == '"') return;
    (void)parent;
    std::vector<std::pair<std::string, std::string>> out;  // literal, description
    const bool decimal = n->base == 'd';
    if (decimal) {
      const std::string prefix = n->sized ? std::to_string(n->width) + "'d" : "";
      std::set<std::int64_t> seen = {n->value};
      for (const std::int64_t v : {std::int64_t(0), std::int64_t(1), n->value + 1, n->value - 1}) {
        if (v < 0 || seen.count(v) != 0) continue;
        if (n->sized && n->width < 63 && v >= (std::int64_t(1) << n->width)) continue;
        seen.insert(v);
        out.push_back({prefix + std::to_string(v), std::to_string(n->value) + " -> " + std::to_string(v)});
      }
    } else if (n->sized && n->width >= 1 && n->width <= 64) {
      const std::string zeros = std::to_string(n->width) + "'b" + std::string(static_cast<std::size_t>(n->width), '0');
      const std::string ones = std::to_string(n->width) + "'b" + std::string(static_cast<std::size_t>(n->width), '1');
      const std::string text = squeeze(e.name);
      std::string digits;
      for (char c : e.name.substr(e.name.find('\'') + 2)) if (c != '_' && c != ' ') digits += c;
      const bool all_zero = digits.find_first_not_of('0') == std::string::npos;
      const bool all_one = (n->base == 'b' && digits.find_first_not_of('1') == std::string::npos) ||
                           (n->base == 'h' && digits.find_first_not_of('f') == std::string::npos && digits.find_first_not_of('F') == std::string::npos) ||
                           (n->base == 'h' && std::all_of(digits.begin(), digits.end(), [](char c) { return c == 'f' || c == 'F'; }));
      if (!all_zero) out.push_back({zeros, text + " -> all zeros"});
      if (!all_one) out.push_back({ones, text + " -> all ones"});
    }
    for (const auto& [lit, desc] : out) {
      VariantSpec v;
      v.kind = VK::Number;
      v.text = lit;
      add(e, v, "const", desc, site_id());
    }
  }

  void stuck_variants(const Root& r) {
    if (r.kind != Root::Kind::Rhs || r.lhs.empty() || !allowed("stuck")) return;
    if (r.expr->kind == K::Number) {  // assigning x/z (e.g. the unreachable `default: ... <= 1'bx`) has no behaviour to corrupt
      const auto n = parse_number(r.expr->name);
      if (!n || n->has_xz) return;
    }
    const StuckTarget* t = table.find_stuck(r.lhs);
    if (t == nullptr) return;
    const int site = next_site++;
    for (const int bit : t->bits)
      for (const bool one : {false, true}) {
        VariantSpec v;
        v.kind = VK::Stuck;
        v.bit = bit;
        v.width = t->width;
        v.one = one;
        add(*r.expr, v, "stuck", r.lhs + "[" + std::to_string(bit) + "] stuck-at-" + (one ? "1" : "0"), site);
      }
  }
};

std::string hex_mask(int width, int bit) {
  // sized hexadecimal literal with one bit set
  const int digits = (width + 3) / 4;
  std::string s(static_cast<std::size_t>(digits), '0');
  const int d = bit / 4;
  s[static_cast<std::size_t>(digits - 1 - d)] = "0123456789abcdef"[1 << (bit % 4)];
  return std::to_string(width) + "'h" + s;
}

// Verilog types an expression as signed or unsigned from its operands, and a ?: whose arms differ is unsigned as a whole.
// Mixing a signed original ($signed(x), a decimal literal) with an unsigned mutant arm would therefore turn the
// ORIGINAL's sign extension into zero extension even when no mutant is selected. Every mutant arm is made to agree with
// the original's signedness (see harmonise()).
bool expr_signed(const Expr& e) {
  switch (e.kind) {
    case K::Number: {
      const auto n = parse_number(e.name);
      if (!n) return false;
      if (!n->sized && n->base == 'd' && e.name.find('\'') == std::string::npos) return true;  // plain decimal: a signed integer
      return e.name.find("'s") != std::string::npos || e.name.find("'S") != std::string::npos;
    }
    case K::Call: return e.name == "$signed";
    case K::Paren: return expr_signed(*e.kids[0]);
    case K::Unary:
      if (e.op == "~" || e.op == "-" || e.op == "+") return expr_signed(*e.kids[0]);
      return false;  // ! and the reduction operators give one unsigned bit
    case K::Binary: {
      const std::string& op = e.op;
      if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" || op == "&" || op == "|" || op == "^" || op == "~^" || op == "^~")
        return expr_signed(*e.kids[0]) && expr_signed(*e.kids[1]);
      if (op == "<<" || op == ">>" || op == "<<<" || op == ">>>" || op == "**") return expr_signed(*e.kids[0]);
      return false;  // comparisons and logical operators
    }
    case K::Ternary: return expr_signed(*e.kids[1]) && expr_signed(*e.kids[2]);
    default: return false;  // identifiers (reg/wire are unsigned here), concatenations, selects
  }
}

bool variant_signed(const Expr& e, const VariantSpec& v) {
  switch (v.kind) {
    case VK::BinaryOp: {
      const std::string& op = v.text;
      if (op == "+" || op == "-" || op == "&" || op == "|" || op == "^") return expr_signed(*e.kids[0]) && expr_signed(*e.kids[1]);
      return false;
    }
    case VK::ShiftPlus:
    case VK::ShiftMinus: return expr_signed(*e.kids[0]);
    case VK::SignedFlip: return false;
    case VK::UnaryDrop: return expr_signed(*e.kids[0]);
    case VK::UnarySwap: return v.text == "~" ? expr_signed(*e.kids[0]) : false;
    case VK::Number: return v.text.find('\'') == std::string::npos;  // plain decimal literal
    case VK::DropSigned: return false;
    case VK::Stuck: return false;  // the mask literal is unsigned
  }
  return false;
}

// Makes a variant text agree with the original's signedness without changing its value.
std::string harmonise(const std::string& text, bool original_signed, bool variant_is_signed) {
  if (original_signed && !variant_is_signed) return "$signed({1'b0, " + text + "})";  // value-preserving: a non-negative signed number
  if (!original_signed && variant_is_signed) return "$unsigned(" + text + ")";
  return text;
}

// Text of a variant given the (already emitted) text of the node's children and of the node itself.
std::string compose(const Expr& e, const VariantSpec& v, const std::function<std::string(const Expr&)>& child, const std::string& self) {
  switch (v.kind) {
    case VK::BinaryOp: return "(" + child(*e.kids[0]) + ") " + v.text + " (" + child(*e.kids[1]) + ")";
    case VK::ShiftPlus: return "(" + child(*e.kids[0]) + ") " + e.op + " ((" + child(*e.kids[1]) + ") + 1)";
    case VK::ShiftMinus: return "(" + child(*e.kids[0]) + ") " + e.op + " ((" + child(*e.kids[1]) + ") - 1)";
    case VK::SignedFlip: {
      const Expr& l = *e.kids[0];
      const Expr& r = *e.kids[1];
      if (v.one) return "$signed(" + child(l) + ") " + e.op + " $signed(" + child(r) + ")";
      return "(" + child(*l.kids[0]) + ") " + e.op + " (" + child(*r.kids[0]) + ")";
    }
    case VK::UnaryDrop: return "(" + child(*e.kids[0]) + ")";
    case VK::UnarySwap: return v.text + "(" + child(*e.kids[0]) + ")";
    case VK::Number: return v.text;
    case VK::DropSigned: return "(" + child(*e.kids[0]) + ")";
    case VK::Stuck:
      if (v.one) return "((" + self + ") | " + hex_mask(v.width, v.bit) + ")";
      return "((" + self + ") & ~" + hex_mask(v.width, v.bit) + ")";
  }
  return self;
}

}  // namespace

Plan plan_mutations(const Walk& walk, const RegionTable& table, std::string_view source, const std::vector<Token>& toks) {
  (void)toks;
  Planner p{walk, table, source, {}, 1, 1, nullptr};
  for (const Root& r : walk.roots) {
    p.region = table.find(r.line);
    if (p.region == nullptr) continue;
    p.visit(*r.expr, nullptr);
    p.stuck_variants(r);
  }
  // Fill in the "mutated" text of every mutant (children as in the source).
  std::unordered_map<int, const std::pair<const Expr*, const VariantSpec*>*> unused;
  for (const auto& [node, list] : p.plan.variants) {
    for (const VariantSpec& v : list) {
      Mutant& m = p.plan.mutants[static_cast<std::size_t>(v.id - 1)];
      const std::string self = source_text(source, *node);
      m.mutated = squeeze(compose(*node, v, [&](const Expr& c) { return source_text(source, c); }, self));
    }
  }
  return std::move(p.plan);
}

std::string emit_instrumented(std::string_view source, const Walk& walk, const Plan& plan, const std::vector<Token>& toks, const std::string& module) {
  (void)module;
  // touched[node]: this subtree contains at least one variant
  std::unordered_map<const Expr*, bool> touched;
  std::function<bool(const Expr&)> mark = [&](const Expr& e) {
    bool t = plan.variants.count(&e) != 0;
    for (const auto& k : e.kids) t = mark(*k) || t;
    touched[&e] = t;
    return t;
  };
  for (const Root& r : walk.roots) mark(*r.expr);

  std::function<std::string(const Expr&)> emit = [&](const Expr& e) -> std::string {
    // Original text of the node with touched children replaced by their emitted form.
    std::string orig;
    std::size_t pos = e.src_begin;
    for (const auto& k : e.kids) {
      if (!touched[k.get()]) continue;
      orig += std::string(source.substr(pos, k->src_begin - pos));
      orig += emit(*k);
      pos = k->src_end;
    }
    orig += std::string(source.substr(pos, e.src_end - pos));
    const auto it = plan.variants.find(&e);
    if (it == plan.variants.end()) return orig;
    std::string out = "(";
    for (const VariantSpec& v : it->second) {
      const std::string arm = harmonise(compose(e, v, [&](const Expr& c) { return emit(c); }, orig), expr_signed(e), variant_signed(e, v));
      out += "mut_sel == 16'd" + std::to_string(v.id) + " ? (" + arm + ") : ";
    }
    out += "(" + orig + "))";
    return out;
  };

  struct Edit { std::size_t begin, end; std::string text; };
  std::vector<Edit> edits;
  for (const Root& r : walk.roots)
    if (touched[r.expr]) edits.push_back({r.expr->src_begin, r.expr->src_end, emit(*r.expr)});
  // the new port
  const std::size_t open = toks[walk.port_list_open].end;
  edits.push_back({open, open, "\n\tinput [15:0] mut_sel,"});
  std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin < b.begin; });
  std::string out;
  std::size_t pos = 0;
  for (const Edit& e : edits) {
    out += std::string(source.substr(pos, e.begin - pos));
    out += e.text;
    pos = e.end;
  }
  out += std::string(source.substr(pos));
  return out;
}

std::string emit_infected(std::string_view source, const Walk& walk, const Plan& plan, const std::vector<Token>& toks) {
  struct Shadow {
    int id;
    std::string root_k;  // the root expression text with mutant `id` applied
  };
  struct RootInfo {
    const Root* root;
    std::vector<Shadow> shadows;
  };
  std::vector<RootInfo> roots;
  for (const Root& r : walk.roots) {
    RootInfo info{&r, {}};
    const std::string root_src = source_text(source, *r.expr);
    std::function<void(const Expr&)> collect = [&](const Expr& e) {
      const auto it = plan.variants.find(&e);
      if (it != plan.variants.end()) {
        for (const VariantSpec& v : it->second) {
          const std::string self = source_text(source, e);
          const std::string arm = harmonise(compose(e, v, [&](const Expr& c) { return source_text(source, c); }, self), expr_signed(e), variant_signed(e, v));
          const std::size_t b = e.src_begin - r.expr->src_begin, len = e.src_end - e.src_begin;
          info.shadows.push_back({v.id, root_src.substr(0, b) + arm + root_src.substr(b + len)});
        }
      }
      for (const auto& k : e.kids) collect(*k);
    };
    collect(*r.expr);
    if (!info.shadows.empty()) roots.push_back(std::move(info));
  }

  struct Insertion {
    std::size_t offset;
    int depth;
    bool is_end;
    std::string text;
  };
  std::vector<Insertion> ins;
  std::map<int, std::vector<int>> block_ids;  // always block -> mutant ids it hosts
  std::string decls;
  std::vector<bool> is_wire(plan.mutants.size() + 1, false);
  int serial = 0;
  for (const RootInfo& info : roots) {
    const Root& r = *info.root;
    const std::string orig = source_text(source, *r.expr);
    const bool cond = r.kind == Root::Kind::Cond;
    auto as_value = [&](const std::string& text) { return cond ? "(" + text + ") ? 64'd1 : 64'd0" : text; };
    const int n = serial++;
    if (r.continuous) {
      std::string text = "\n/* verilator lint_off UNSIGNED */\nwire [63:0] inf_a" + std::to_string(n) + " = " + as_value(orig) + ";";
      for (const Shadow& s : info.shadows) {
        is_wire[static_cast<std::size_t>(s.id)] = true;
        text += "\nwire [63:0] inf_b" + std::to_string(n) + "_" + std::to_string(s.id) + " = " + as_value(s.root_k) + ";";
        text += "\nassign inf_" + std::to_string(s.id) + " = (inf_a" + std::to_string(n) + " != inf_b" + std::to_string(n) + "_" + std::to_string(s.id) + ");";
      }
      text += "\n/* verilator lint_on UNSIGNED */";
      ins.push_back({toks[r.stmt_end - 1].end, 1, true, text});
    } else {
      // Shadow evaluations of a mutated constant can be always-true/false comparisons; that is the point of the mutant.
      std::string text = "begin : inf_blk" + std::to_string(n) + "\n  reg [63:0] inf_a, inf_b;\n  /* verilator lint_off UNSIGNED */\n  inf_a = " + as_value(orig) + ";";
      for (const Shadow& s : info.shadows) {
        text += "\n  inf_b = " + as_value(s.root_k) + "; inf_" + std::to_string(s.id) + " = (inf_a != inf_b);";
        block_ids[r.block].push_back(s.id);
      }
      text += "\n  /* verilator lint_on UNSIGNED */\n";
      ins.push_back({toks[r.stmt_begin].begin, 1, false, text});
      ins.push_back({toks[r.stmt_end - 1].end, 1, true, "\nend"});
    }
  }
  for (const auto& [b, ids] : block_ids) {
    std::string text = "begin\n";
    for (const int id : ids) text += "  inf_" + std::to_string(id) + " = 1'b0;\n";
    const Block& blk = walk.blocks[static_cast<std::size_t>(b)];
    ins.push_back({toks[blk.begin].begin, 0, false, text});
    ins.push_back({toks[blk.end - 1].end, 0, true, "\nend"});
  }
  // declarations right after the module header, the vector at the end of the module
  const int count = static_cast<int>(plan.mutants.size());
  for (int id = 1; id <= count; ++id) decls += std::string("\n") + (is_wire[static_cast<std::size_t>(id)] ? "wire" : "reg") + " inf_" + std::to_string(id) + ";";
  ins.push_back({toks[walk.header_end - 1].end, 0, true, decls});
  std::string vec = "\nassign infected = {";
  for (int id = count; id >= 1; --id) vec += "inf_" + std::to_string(id) + ", ";
  vec += "1'b0};\n";
  ins.push_back({toks[walk.endmodule].begin, 0, false, vec});
  ins.push_back({toks[walk.port_list_open].end, 0, false, "\n\toutput [" + std::to_string(count) + ":0] infected,"});

  std::stable_sort(ins.begin(), ins.end(), [](const Insertion& a, const Insertion& b) {
    if (a.offset != b.offset) return a.offset < b.offset;
    if (a.is_end != b.is_end) return a.is_end;  // close earlier constructs before opening new ones
    return a.is_end ? a.depth > b.depth : a.depth < b.depth;
  });
  std::string out;
  std::size_t pos = 0;
  for (const Insertion& i : ins) {
    out += std::string(source.substr(pos, i.offset - pos));
    out += i.text;
    pos = i.offset;
  }
  out += std::string(source.substr(pos));
  return out;
}

std::string manifest_tsv(const Plan& plan) {
  std::ostringstream os;
  os << "id\tsite\tcategory\top_class\tkind\tline\toriginal\tmutated\n";
  for (const Mutant& m : plan.mutants)
    os << m.id << '\t' << m.site << '\t' << m.category << '\t' << m.op_class << '\t' << m.kind << '\t' << m.line << '\t' << m.original << '\t' << m.mutated << '\n';
  return os.str();
}

}  // namespace cutoff::drill
