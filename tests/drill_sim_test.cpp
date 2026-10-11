// Step 6 gates on the Verilated instrumented core: with no mutant selected it must be indistinguishable from the original
// core, mutants must really alter behaviour, and an obvious planted bug must be caught by checker C1 almost at once.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <tuple>
#include <sstream>
#include <string>
#include <vector>

#include "build_command.hpp"
#include "campaign_config.hpp"
#include "check.hpp"
#include "generator.hpp"
#include "model.hpp"
#include "rv32i.hpp"
#include "protocol.hpp"
#include "run.hpp"
#include "run_command.hpp"
#include "sim_core.hpp"
#include "sim_drill.hpp"

namespace fs = std::filesystem;
using namespace cutoff;
using namespace cutoff::engine;

namespace {

struct ManifestRow {
  int id = 0, site = 0, line = 0;
  std::string category, op_class, kind, original, mutated;
};

std::vector<ManifestRow> manifest() {
  std::vector<ManifestRow> rows;
  std::istringstream in{std::string(drill_manifest_tsv())};
  std::string line;
  std::getline(in, line);  // header
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    std::istringstream ls(line);
    std::string cell;
    while (std::getline(ls, cell, '\t')) f.push_back(cell);
    EXPECT_EQ(f.size(), 8u) << line;
    if (f.size() != 8) continue;
    rows.push_back({std::stoi(f[0]), std::stoi(f[1]), std::stoi(f[5]), f[2], f[3], f[4], f[6], f[7]});
  }
  return rows;
}

int find_mutant(int line, const std::string& kind) {
  for (const ManifestRow& r : manifest())
    if (r.line == line && r.kind == kind) return r.id;
  return -1;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

class TempDir {
 public:
  TempDir() {
    const std::string pattern = (fs::temp_directory_path() / "cutoff-test.XXXXXX").string();
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    path_ = mkdtemp(buf.data());
  }
  ~TempDir() {  // NOLINT(bugprone-exception-escape): best-effort cleanup
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::string& str() const { return path_; }

 private:
  std::string path_;
};

}  // namespace

TEST(DrillSim, TheManifestIsEmbeddedAndConsistent) {
  const auto rows = manifest();
  EXPECT_EQ(static_cast<int>(rows.size()), drill_mutant_count());
  EXPECT_EQ(drill_mutant_count(), 201) << "regression pin for the pinned core and region table (see the generator tests)";
  for (std::size_t i = 0; i < rows.size(); ++i) EXPECT_EQ(rows[i].id, static_cast<int>(i) + 1);
  EXPECT_NE(drill_summary().find("201 mutants"), std::string_view::npos);
}

TEST(DrillSim, WithNoMutantSelectedTheInstrumentedCoreIsCycleExactlyTheOriginal) {
  const gen::GenConfig cfg;
  int checked = 0;
  for (std::uint64_t i = 0; i < 1500; ++i) {
    const gen::Program p = gen::generate(cfg, gen::program_seed(60, i));
    const RunResult a = run_program(p.code);
    const RunResult b = run_program_mutant(p.code, 0);
    ASSERT_EQ(a.status, b.status) << i;
    ASSERT_EQ(a.exit_code, b.exit_code) << i;
    ASSERT_EQ(a.cycles, b.cycles) << i;
    ASSERT_EQ(a.trace_hash, b.trace_hash) << "the bus trace (cycle, address, data, strobes of every transfer) differs, program " << i;
    ASSERT_EQ(a.regs, b.regs) << i;
    ASSERT_EQ(a.mem_hash, b.mem_hash) << i;
    ++checked;
  }
  EXPECT_EQ(checked, 1500);
}

TEST(DrillSim, ThroughTheCheckerMutantZeroGivesTheSameRecordsAsTheOriginalCore) {
  CampaignConfig original, instrumented;
  instrumented.mutant = 0;
  for (std::uint64_t id = 0; id < 400; ++id) {
    const EventRecord a = evaluate_program(original, id);
    const EventRecord b = evaluate_program(instrumented, id);
    ASSERT_EQ(std::memcmp(&a, &b, sizeof a), 0) << id;
    ASSERT_EQ(a.verdict, static_cast<std::uint8_t>(Verdict::Pass));
  }
}

TEST(DrillSim, MostMutantsReallyChangeBehaviour) {
  // A mux that is wired wrongly (or a mutant in dead logic) never differs from mutant 0. Equivalent and hard mutants
  // legitimately exist, so this is a liveness check on the wiring, not a requirement that all 201 are exposed.
  const gen::GenConfig cfg;
  std::vector<gen::Program> programs;
  programs.reserve(25);
  for (std::uint64_t i = 0; i < 25; ++i) programs.push_back(gen::generate(cfg, gen::program_seed(61, i)));
  std::vector<RunResult> base;
  base.reserve(programs.size());
  for (const auto& p : programs) base.push_back(run_program_mutant(p.code, 0, {50000}));
  int live = 0;
  std::vector<int> quiet;
  for (int m = 1; m <= drill_mutant_count(); ++m) {
    bool differs = false;
    for (std::size_t i = 0; i < programs.size() && !differs; ++i) {
      const RunResult r = run_program_mutant(programs[i].code, static_cast<std::uint32_t>(m), {50000});
      differs = r.trace_hash != base[i].trace_hash || r.status != base[i].status || r.regs != base[i].regs || r.mem_hash != base[i].mem_hash;
    }
    if (differs) ++live;
    else quiet.push_back(m);
  }
  std::printf("[DrillSim] %d of %d mutants changed the bus trace, a register, memory or the status within 25 programs\n", live, drill_mutant_count());
  EXPECT_GE(live, 140);
}

TEST(DrillSim, AnObviousPlantedBugIsCaughtByC1WithinAFewPrograms) {
  // Spec section 11: "an ALU add turned into a subtract". Line 1240 holds `instr_sub ? a - b : a + b`.
  const int add_to_sub = find_mutant(1240, "+ -> -");
  const int sub_to_add = find_mutant(1240, "- -> +");
  ASSERT_GT(add_to_sub, 0);
  ASSERT_GT(sub_to_add, 0);
  for (const int m : {add_to_sub, sub_to_add}) {
    CampaignConfig cfg;
    cfg.mutant = m;
    int first_failure = -1;
    for (std::uint64_t id = 0; id < 10 && first_failure < 0; ++id)
      if (evaluate_program(cfg, id).verdict != static_cast<std::uint8_t>(Verdict::Pass)) first_failure = static_cast<int>(id);
    EXPECT_GE(first_failure, 0) << "mutant " << m << " was not caught in 10 programs";
    EXPECT_LE(first_failure, 3) << "mutant " << m << " should be caught within the first few programs";
  }
}

TEST(DrillSim, TheSignExtensionOfBranchAndJumpOffsetsOnlyShowsOnBackwardJumpsWhichG0NeverGenerates) {
  // In the 500-program scan the mutants that drop $signed from the J-immediate (line 880) and the B-immediate (line 1128)
  // survive: G0 emits only forward jumps and branches, so those offsets are never negative. Hand-written programs with a
  // backward jump and a backward branch expose them, which shows these are stimulus gaps, not equivalent mutants.
  const int j_imm = find_mutant(880, "drop $signed (zero- instead of sign-extend)");
  const int b_imm = find_mutant(1128, "drop $signed (zero- instead of sign-extend)");
  ASSERT_GT(j_imm, 0);
  ASSERT_GT(b_imm, 0);

  const std::vector<std::uint32_t> backward_jump = {
      rv::jal(0, 12),            // 0:  jump forward to 12
      rv::lui(4, 0x10000),       // 4:  <- the backward jump lands here
      rv::sw(1, 4, 0),           // 8:  exit with x1
      rv::addi(1, 0, 42),        // 12
      rv::jal(0, -12),           // 16: backward to 4
  };
  const std::vector<std::uint32_t> backward_branch = {
      rv::addi(1, 0, 3),         // 0
      rv::addi(2, 2, 7),         // 4:  <- loop body
      rv::addi(1, 1, -1),        // 8
      rv::bne(1, 0, -8),         // 12: backward to 4
      rv::lui(4, 0x10000),       // 16
      rv::sw(2, 4, 0),           // 20: exit with x2 = 21
  };
  for (const auto& [program, mutant, expected_exit] : {std::tuple{backward_jump, j_imm, 42u}, std::tuple{backward_branch, b_imm, 21u}}) {
    const ModelResult model = run_model(program, {1000, false});
    ASSERT_EQ(model.status, Status::Exited);
    ASSERT_EQ(model.exit_code, expected_exit);
    const RunResult none = run_program_mutant(program, 0, {20000});
    EXPECT_EQ(none.status, Status::Exited);
    EXPECT_EQ(none.exit_code, expected_exit) << "the unmutated instrumented core runs the program correctly";
    const RunResult bad = run_program_mutant(program, static_cast<std::uint32_t>(mutant), {20000});
    EXPECT_TRUE(bad.status != Status::Exited || bad.exit_code != expected_exit) << "mutant " << mutant << " must be exposed by a backward offset";
  }
}

TEST(DrillSim, CampaignSettingValidatesAndSeparatesStreams) {
  CampaignConfig c;
  c.mutant = 202;
  EXPECT_NE(validate(c).find("mutants 1 to 201"), std::string::npos) << validate(c);
  c.mutant = 201;
  EXPECT_EQ(validate(c), "");
  c.mutant = -1;
  EXPECT_EQ(validate(c), "");
  CampaignConfig a, b;
  b.mutant = 0;
  EXPECT_NE(stream_hash(a), stream_hash(b)) << "the original and the instrumented core are different streams for resume purposes";
  const auto parsed = parse_toml("[core]\nmutant = 7\n", "t");
  CampaignConfig d;
  ASSERT_EQ(apply_toml(d, parsed.doc, "t"), "");
  EXPECT_EQ(d.mutant, 7);
  EXPECT_NE(to_toml(d).find("mutant = 7"), std::string::npos);
  EXPECT_NE(apply_toml(d, parse_toml("[core]\nmutant = -2\n", "t").doc, "t").find("between -1 and 65535"), std::string::npos);
}

TEST(DrillSim, ACampaignOnAMutantReportsFailuresAndAFailureFolder) {
  TempDir tmp;
  const int m = find_mutant(1240, "+ -> -");
  ASSERT_GT(m, 0);
  std::ostringstream out, err;
  const int code = run_command({"--mutant", std::to_string(m), "--max-programs", "60", "--workers", "2", "--results-dir", tmp.str(), "--run-id", "bug", "--no-progress"}, out, err);
  EXPECT_EQ(code, 1) << err.str() << out.str();
  EXPECT_NE(out.str().find("failures:"), std::string::npos);
  EXPECT_EQ(out.str().find("failures:       0"), std::string::npos) << out.str();
  EXPECT_NE(slurp(fs::path(tmp.str()) / "bug" / "config.toml").find("mutant = " + std::to_string(m)), std::string::npos);
  int dirs = 0;
  for (const auto& e : fs::directory_iterator(fs::path(tmp.str()) / "bug" / "failures")) {
    ++dirs;
    const std::string text = slurp(e.path() / "failure.txt");
    EXPECT_NE(text.find("verdict:"), std::string::npos);
    EXPECT_EQ(text.find("verdict:           pass"), std::string::npos) << "a failure folder is only written for a failing program";
    EXPECT_NE(text.find("differing registers"), std::string::npos) << "the report shows what differs";
  }
  EXPECT_GT(dirs, 0);

  std::ostringstream out2, err2;
  EXPECT_EQ(run_command({"--mutant", "9999", "--results-dir", tmp.str(), "--run-id", "bad"}, out2, err2), 3);
  EXPECT_NE(err2.str().find("does not exist"), std::string::npos);
}

TEST(DrillSim, AResumeCannotSwitchTheCoreOrTheMutant) {
  TempDir tmp;
  CampaignConfig cfg;
  cfg.programs = 300;
  cfg.workers = 2;
  cfg.results_dir = tmp.str();
  cfg.stop.checkpoint = 100;
  cfg.mutant = 0;
  cfg.interrupt_after = 150;
  std::ostringstream out, err;
  const RunOutcome cut = execute_run(cfg, "m", false, out, err);
  ASSERT_EQ(cut.exit_code, 130);
  CampaignConfig other = cfg;
  other.interrupt_after = 0;
  other.mutant = 1;
  std::ostringstream err2;
  EXPECT_EQ(execute_run(other, "x", false, out, err2, cut.run_dir).exit_code, 3);
  EXPECT_NE(err2.str().find("differ from the saved run"), std::string::npos);
  other.mutant = 0;
  EXPECT_EQ(execute_run(other, "x", false, out, err2, cut.run_dir).exit_code, 0) << err2.str();
}

TEST(BuildCommand, ReportsTheBuiltInCoresAndListsEveryMutant) {
  std::ostringstream out, err;
  EXPECT_EQ(build_command({}, out, err), 0);
  EXPECT_NE(out.str().find("picorv32"), std::string::npos);
  EXPECT_NE(out.str().find("201 mutants"), std::string::npos);

  std::ostringstream d, e2;
  EXPECT_EQ(build_command({"--drill"}, d, e2), 0);
  EXPECT_NE(d.str().find("by category:"), std::string::npos);
  for (const char* cat : {"alu", "compare", "branch", "imm", "regfile", "memory", "pc"}) EXPECT_NE(d.str().find(cat), std::string::npos) << cat;

  std::ostringstream l, e3;
  EXPECT_EQ(build_command({"--drill", "--list"}, l, e3), 0);
  const std::string listing = l.str();
  EXPECT_EQ(listing.rfind("id\tsite\tcategory\top_class\tkind\tline\toriginal\tmutated\n", 0), 0u);
  EXPECT_EQ(std::count(listing.begin(), listing.end(), '\n'), 202);

  std::ostringstream x, e4;
  EXPECT_EQ(build_command({"--list"}, x, e4), 3);
  EXPECT_NE(e4.str().find("needs --drill"), std::string::npos);
  std::ostringstream y, e5;
  EXPECT_EQ(build_command({"--frob"}, y, e5), 3);
  EXPECT_NE(e5.str().find("unknown option"), std::string::npos);
  std::ostringstream h, e6;
  EXPECT_EQ(build_command({"--help"}, h, e6), 0);
  EXPECT_NE(h.str().find("usage: cutoff build"), std::string::npos);
}
