#pragma once

#include <string>
#include <vector>

namespace cutoff {

struct Check {
  std::string name;
  bool ok = false;
  bool required = true;
  std::string detail;
};

// Runs every environment check. Never throws.
std::vector<Check> run_doctor_checks();

// Prints the checklist and returns the process exit code (0 pass, 3 environment error).
int doctor_main();

}  // namespace cutoff
