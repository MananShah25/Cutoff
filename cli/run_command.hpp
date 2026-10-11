#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace cutoff {

// `cutoff run`. `args` are the arguments after the command name. Returns the process exit code.
int run_command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

// `cutoff drill`: the same campaign, with the planted bugs of the instrumented core run against every program too.
int drill_command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace cutoff
