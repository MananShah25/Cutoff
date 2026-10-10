#include "doctor.hpp"

#include <cstdio>
#include <filesystem>
#include <thread>

#include "sim_core.hpp"
#include "smoke_programs.hpp"

namespace cutoff {

namespace {

constexpr std::uintmax_t kMinFreeBytes = 5ULL << 30U;  // 5 GiB, see docs/decisions.md D-006

// First line of a command's combined output, or empty if it could not be run.
std::string first_line(const char* cmd) {
  std::string out;
  // Only ever called with fixed literal commands (tool version queries), never with user input.
  if (FILE* p = popen(cmd, "r")) {  // NOLINT(bugprone-command-processor)
    char buf[256];
    if (fgets(buf, sizeof buf, p)) out = buf;
    pclose(p);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

std::string hex64(std::uint64_t v) {
  char buf[19];
  std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(v));
  return buf;
}

Check check_compiler() {
  Check c{"C++ compiler", true, true, {}};
#ifdef __VERSION__
  c.detail = __VERSION__;
#else
  c.detail = "unknown";
#endif
  c.detail += " (C++" + std::to_string(__cplusplus / 100 % 100U) + ")";
  return c;
}

Check check_verilator() {
  Check c{"Verilator", false, true, {}};
  const std::string v = first_line("verilator --version 2>&1");
  c.detail = v.empty() ? "not found on PATH; install it (brew install verilator) and retry" : v;
  c.ok = v.rfind("Verilator 5", 0) == 0;
  if (!v.empty() && !c.ok) c.detail += "  (Cutoff needs Verilator 5.x)";
  return c;
}

Check check_cmake() {
  Check c{"CMake", false, false, {}};
  const std::string v = first_line("cmake --version 2>&1");
  c.ok = !v.empty();
  c.detail = c.ok ? v : "not found on PATH (only needed to rebuild Cutoff)";
  return c;
}

Check check_cores() {
  const unsigned n = std::thread::hardware_concurrency();
  return {"CPU cores", n > 0, true, n > 0 ? std::to_string(n) + " hardware threads" : "could not detect core count"};
}

Check check_disk() {
  Check c{"Disk space", false, true, {}};
  std::error_code ec;
  const auto info = std::filesystem::space(std::filesystem::current_path(), ec);
  if (ec) {
    c.detail = "could not query free space: " + ec.message();
  } else {
    c.ok = info.available >= kMinFreeBytes;
    c.detail = std::to_string(info.available >> 30U) + " GiB free here";
    if (!c.ok) c.detail += "; need at least 5 GiB, free some space or run from another disk";
  }
  return c;
}

Check check_core_selftest() {
  Check c{"Verilated core", false, true, {}};
  const RunResult r = run_program(smoke::kAdd);
  c.ok = r.status == Status::Exited && r.exit_code == 12;
  c.detail = "built with Verilator " + verilator_version() + "; smoke program " + to_string(r.status) + " with exit code " +
             std::to_string(r.exit_code) + " in " + std::to_string(r.cycles) + " cycles, trace hash " + hex64(r.trace_hash);
  if (!c.ok) c.detail += "  (expected exited / 12; rebuild with a clean build directory)";
  return c;
}

}  // namespace

std::vector<Check> run_doctor_checks() {
  return {check_compiler(), check_verilator(), check_cmake(), check_cores(), check_disk(), check_core_selftest()};
}

int doctor_main() {
  const auto checks = run_doctor_checks();
  bool all_ok = true;
  for (const auto& c : checks) {
    const char* tag = c.ok ? " ok " : (c.required ? "FAIL" : "warn");
    std::printf("[%s] %-15s %s\n", tag, c.name.c_str(), c.detail.c_str());
    if (!c.ok && c.required) all_ok = false;
  }
  std::printf("\n%s\n", all_ok ? "All required checks passed." : "Some required checks failed; fix the FAIL lines above and rerun `cutoff doctor`.");
  return all_ok ? 0 : 3;
}

}  // namespace cutoff
