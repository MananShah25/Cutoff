// A first look at generated programs on the real core. This is a smoke check of the generator, NOT the
// 100,000-program baseline gate of Step 4.

#include <gtest/gtest.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "generator.hpp"
#include "model.hpp"
#include "sim_core.hpp"

using namespace cutoff;

TEST(GenVsCore, FirstGeneratedProgramsAgreeBetweenModelAndCore) {
  const gen::GenConfig cfg;
  const int n = 500;
  std::vector<std::string> bad;
  for (int i = 0; i < n; ++i) {
    const std::uint64_t seed = gen::program_seed(100, i);
    const gen::Program p = gen::generate(cfg, seed);
    const ModelResult m = run_model(p.code, {20'000, false});
    const RunResult r = run_program(p.code);
    std::ostringstream err;
    if (m.status != Status::Exited || r.status != Status::Exited) {
      err << "status model=" << to_string(m.status) << " core=" << to_string(r.status);
    } else {
      if (m.exit_code != r.exit_code) err << "exit_code model=0x" << std::hex << m.exit_code << " core=0x" << r.exit_code;
      for (unsigned k = 1; k < 32 && err.str().empty(); ++k)
        if (m.regs[k] != r.regs[k]) err << "x" << std::dec << k << " model=0x" << std::hex << m.regs[k] << " core=0x" << r.regs[k];
    }
    if (!err.str().empty() && bad.size() < 8) bad.push_back("seed " + std::to_string(seed) + ": " + err.str());
  }
  std::printf("[GenVsCore] compared %d generated programs, %zu differ\n", n, bad.size());
  std::ostringstream os;
  for (const auto& b : bad) os << "\n  " << b;
  EXPECT_TRUE(bad.empty()) << os.str();
}
