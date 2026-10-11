#include "system_info.hpp"

#include "sim_core.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace cutoff::engine {

std::string cpu_model() {
#ifdef __APPLE__
  char buf[256];
  std::size_t len = sizeof buf;
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0 && len > 0) return std::string(buf);
#else
  std::ifstream f("/proc/cpuinfo");
  for (std::string line; std::getline(f, line);)
    if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) return line.substr(line.find(':') + 2);
#endif
  return "unknown";
}

unsigned logical_cores() {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1 : n;
}

std::string verilator_version_string() { return verilator_version(); }

std::string compiler_version() {
#ifdef __VERSION__
  return __VERSION__;
#else
  return "unknown";
#endif
}

std::string utc_timestamp() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

}  // namespace cutoff::engine
