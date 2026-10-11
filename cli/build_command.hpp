#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace cutoff {

// `cutoff build`. In this version the Verilated cores are compiled into the binary by CMake, so the command reports what
// was built in; `--drill` describes the instrumented core and `--drill --list` prints every mutant.
int build_command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace cutoff
