// cutoff-mutgen: generates the instrumented core and the mutant manifest.
//   cutoff-mutgen --rtl picorv32.v --wrapper cutoff_core.v --regions picorv32_regions.txt --out-rtl X.v --out-manifest X.tsv
//   cutoff-mutgen ... --list      print the manifest instead of writing files
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "generate.hpp"

namespace {

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// Rewrites a file only when its content changes, so an unchanged core does not trigger a rebuild.
bool write_if_changed(const std::string& path, const std::string& text) {
  std::string old;
  if (read_file(path, old) && old == text) return true;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << text;
  return static_cast<bool>(f);
}

}  // namespace

int main(int argc, char** argv) {
  std::string rtl, wrapper, regions, out_rtl, out_manifest, out_wrapper, out_embed;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& dst) {
      if (i + 1 >= argc) { std::fprintf(stderr, "cutoff-mutgen: %s needs a value\n", a.c_str()); return false; }
      dst = argv[++i];
      return true;
    };
    if (a == "--rtl") { if (!next(rtl)) return 3; }
    else if (a == "--wrapper") { if (!next(wrapper)) return 3; }
    else if (a == "--regions") { if (!next(regions)) return 3; }
    else if (a == "--out-rtl") { if (!next(out_rtl)) return 3; }
    else if (a == "--out-manifest") { if (!next(out_manifest)) return 3; }
    else if (a == "--out-wrapper") { if (!next(out_wrapper)) return 3; }
    else if (a == "--out-embed") { if (!next(out_embed)) return 3; }
    else if (a == "--list") list = true;
    else { std::fprintf(stderr, "cutoff-mutgen: unknown option '%s'\n", a.c_str()); return 3; }
  }
  if (rtl.empty() || wrapper.empty() || regions.empty() || (!list && (out_rtl.empty() || out_manifest.empty() || out_wrapper.empty() || out_embed.empty()))) {
    std::fprintf(stderr, "usage: cutoff-mutgen --rtl FILE --wrapper FILE --regions FILE (--out-rtl FILE --out-manifest FILE --out-wrapper FILE --out-embed FILE | --list)\n");
    return 3;
  }
  std::string rtl_text, wrapper_text, regions_text;
  for (const auto& [path, dst] : {std::pair<const std::string&, std::string&>{rtl, rtl_text}, {wrapper, wrapper_text}, {regions, regions_text}})
    if (!read_file(path, dst)) { std::fprintf(stderr, "cutoff-mutgen: cannot read %s\n", path.c_str()); return 3; }

  const cutoff::drill::GenerateResult r = cutoff::drill::generate_drill(rtl_text, wrapper_text, regions_text);
  if (!r.error.empty()) { std::fprintf(stderr, "cutoff-mutgen: %s\n", r.error.c_str()); return 3; }
  if (list) {
    std::fputs(r.manifest.c_str(), stdout);
    std::fputs(cutoff::drill::describe(r).c_str(), stderr);
    return 0;
  }
  if (!write_if_changed(out_rtl, r.instrumented) || !write_if_changed(out_manifest, r.manifest) || !write_if_changed(out_wrapper, r.wrapper) ||
      !write_if_changed(out_embed, cutoff::drill::embedded_source(r))) {
    std::fprintf(stderr, "cutoff-mutgen: cannot write the output files\n");
    return 3;
  }
  std::fputs(cutoff::drill::describe(r).c_str(), stdout);
  return 0;
}
