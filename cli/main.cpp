#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "doctor.hpp"
#include "run_command.hpp"

namespace {

void usage() {
  std::fputs(
      "cutoff 0.0.1\n"
      "usage: cutoff <command>\n"
      "commands available now:\n"
      "  doctor    check Verilator, compiler, cores and disk space\n"
      "  run       run random programs on the core and check them (see `cutoff run --help`)\n",
      stderr);
}

}  // namespace

namespace {

int dispatch(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 3;
  }
  const std::string_view cmd = argv[1];
  if (cmd == "doctor") return cutoff::doctor_main();
  if (cmd == "run") return cutoff::run_command(std::vector<std::string>(argv + 2, argv + argc), std::cout, std::cerr);
  std::fprintf(stderr, "cutoff: unknown or not-yet-implemented command '%s'\n\n", argv[1]);
  usage();
  return 3;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return dispatch(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "cutoff: unexpected error: %s\nThis is a bug in Cutoff; please keep the command line and the results folder.\n", e.what());
    return 3;
  }
}
