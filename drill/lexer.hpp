#pragma once
// A small Verilog tokenizer with a minimal preprocessor, enough for the structure of a core like PicoRV32.
// Text in inactive `ifdef/`ifndef/`else/`elsif branches is skipped (line numbers stay correct); `define, `timescale
// and similar directive lines are skipped; attributes (* ... *) and comments produce no tokens. Every token keeps its
// byte range in the original text, so mutated output can be spliced into the untouched source.

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cutoff::drill {

enum class Tok : unsigned char {
  Ident,   // identifiers and keywords (begin, if, case, ...)
  Number,  // 12, 4'b0001, 32'h ffff_fffe, 'bx, 3'b 010
  Sys,     // $signed, $anyseq, ...
  Macro,   // `debug, `assert (a use of a text macro)
  String,
  Op,      // every operator and punctuation character, longest match
  End,
};

struct Token {
  Tok kind = Tok::End;
  std::string_view text;
  std::size_t begin = 0, end = 0;  // byte range in the source
  int line = 0;                    // 1-based
};

struct LexResult {
  std::vector<Token> tokens;  // ends with a Tok::End token
  std::string error;          // empty on success; otherwise "line N: what"
};

// `defines` are the macros considered defined for `ifdef (none for the RV32I build).
LexResult lex(std::string_view source, const std::set<std::string>& defines = {});

}  // namespace cutoff::drill
