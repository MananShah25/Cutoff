#include "build_command.hpp"

#include <map>
#include <ostream>
#include <sstream>

#include "sim_core.hpp"
#include "sim_drill.hpp"

namespace cutoff {

int build_command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  bool drill = false, list = false;
  for (const std::string& a : args) {
    if (a == "--drill") drill = true;
    else if (a == "--list") list = true;
    else if (a == "--help" || a == "-h") {
      out << "usage: cutoff build [--drill [--list]]\n"
             "  The Verilated cores are compiled into this binary when Cutoff is built with CMake.\n"
             "  (no option)    report the cores built in\n"
             "  --drill        describe the instrumented core: how many mutants, in which parts of the design\n"
             "  --list         with --drill, print every mutant (id, site, category, operator class, kind, line, original, mutated)\n";
      return 0;
    } else {
      err << "cutoff: unknown option '" << a << "'. See `cutoff build --help`.\n";
      return 3;
    }
  }
  if (list && !drill) {
    err << "cutoff: --list needs --drill.\n";
    return 3;
  }
  if (!drill) {
    out << "Verilated cores built in (Verilator " << verilator_version() << "): picorv32 (RV32I), and its instrumented copy with "
        << drill_mutant_count() << " mutants (see --drill).\n";
    return 0;
  }
  if (list) {
    out << drill_manifest_tsv();
    return 0;
  }
  out << "Instrumented core: " << drill_summary();
  return 0;
}

}  // namespace cutoff
