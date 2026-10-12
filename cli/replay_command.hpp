#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace cutoff {
int replay_command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
}  // namespace cutoff
