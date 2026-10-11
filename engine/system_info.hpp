#pragma once

#include <string>

namespace cutoff::engine {

std::string cpu_model();         // e.g. "Apple M3"; "unknown" if it cannot be read
unsigned logical_cores();
std::string verilator_version_string();  // the Verilator that built the core
std::string compiler_version();  // the compiler that built Cutoff
std::string utc_timestamp();     // "2026-10-10T19:05:00Z"

}  // namespace cutoff::engine
