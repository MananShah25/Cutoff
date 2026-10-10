#include <cstdio>
#include <string_view>

#include "doctor.hpp"

namespace {

void usage() {
  std::fputs(
      "cutoff 0.0.1\n"
      "usage: cutoff <command>\n"
      "commands available now:\n"
      "  doctor    check Verilator, compiler, cores and disk space\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 3;
  }
  const std::string_view cmd = argv[1];
  if (cmd == "doctor") return cutoff::doctor_main();
  std::fprintf(stderr, "cutoff: unknown or not-yet-implemented command '%s'\n\n", argv[1]);
  usage();
  return 3;
}
