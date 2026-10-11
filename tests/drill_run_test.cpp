// Step 7: the fire drill. Unit tests for mutant selection, the funnel tally, the report and the wire frame; simulation tests
// for the infected-bit model (cycle-exact against the original core, sound, and sensitive); campaign tests for `cutoff drill`
// (events.bin untouched, worker-count independence, filter on/off, stop rule observed, exact resume, refusals).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "campaign_config.hpp"
#include "check.hpp"
#include "drill_levels.hpp"
#include "drill_report.hpp"
#include "drill_select.hpp"
#include "drill_tally.hpp"
#include "generator.hpp"
#include "protocol.hpp"
#include "run.hpp"
#include "run_command.hpp"
#include "rv32i.hpp"
#include "sim_core.hpp"
#include "sim_drill.hpp"
#include "sim_infect.hpp"

namespace fs = std::filesystem;
using namespace cutoff;
using namespace cutoff::engine;

namespace {

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
  fs::path file(const std::string& name) const { return fs::path(path_) / name; }

 private:
  std::string path_;
};

int find_mutant(int line, const std::string& kind) {
  for (const MutantInfo& m : all_mutants())
    if (m.line == line && m.kind == kind) return m.id;
  return -1;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string cell;
  while (std::getline(in, cell, sep)) out.push_back(cell);
  return out;
}

// The rows of drill.tsv, optionally without some columns (by name).
std::vector<std::string> tsv_rows(const std::string& tsv, const std::vector<std::string>& drop = {}) {
  const std::vector<std::string> lines = split(tsv, '\n');
  std::vector<std::string> out;
  if (lines.empty()) return out;
  const std::vector<std::string> header = split(lines[0], '\t');
  for (const std::string& line : lines) {
    const std::vector<std::string> f = split(line, '\t');
    std::string row;
    for (std::size_t i = 0; i < f.size(); ++i) {
      if (i < header.size() && std::find(drop.begin(), drop.end(), header[i]) != drop.end()) continue;
      row += f[i] + "\t";
    }
    out.push_back(row);
  }
  return out;
}

// Column `name` of every row (header excluded).
std::vector<std::string> tsv_column(const std::string& tsv, const std::string& name) {
  const std::vector<std::string> lines = split(tsv, '\n');
  const std::vector<std::string> header = split(lines.at(0), '\t');
  const auto it = std::find(header.begin(), header.end(), name);
  std::vector<std::string> out;
  if (it == header.end()) return out;
  const std::size_t col = static_cast<std::size_t>(it - header.begin());
  for (std::size_t i = 1; i < lines.size(); ++i) out.push_back(split(lines[i], '\t').at(col));
  return out;
}

CampaignConfig small_drill(const TempDir& tmp, std::uint64_t programs, std::uint32_t mutants, std::uint32_t workers) {
  CampaignConfig cfg;
  cfg.drill = true;
  cfg.drill_mutants = mutants;
  cfg.programs = programs;
  cfg.workers = workers;
  cfg.results_dir = tmp.str();
  cfg.stop.checkpoint = 50;
  return cfg;
}

struct Quiet {
  std::ostringstream out, err;
};

}  // namespace

// ---------------------------------------------------------------- selection

TEST(DrillSelect, NoRulesSelectsEveryMutantInOrder) {
  std::string error;
  const std::vector<int> ids = select_mutants("", 0, 1, error);
  EXPECT_EQ(error, "");
  ASSERT_EQ(ids.size(), 201u);
  for (std::size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], static_cast<int>(i) + 1);
}

TEST(DrillSelect, ManifestRowsAreComplete) {
  const std::vector<MutantInfo> all = all_mutants();
  ASSERT_EQ(all.size(), 201u);
  for (const MutantInfo& m : all) {
    EXPECT_FALSE(m.category.empty()) << m.id;
    EXPECT_FALSE(m.op_class.empty()) << m.id;
    EXPECT_GT(m.line, 0) << m.id;
    EXPECT_NE(m.original, m.mutated) << m.id;
  }
}

TEST(DrillSelect, IncludeAndExcludeRulesAndComments) {
  const std::vector<MutantInfo> all = all_mutants();
  std::string error;
  const std::vector<int> mem = select_mutants("category:memory  # only the memory interface\n\n", 0, 1, error);
  EXPECT_EQ(error, "");
  EXPECT_EQ(mem.size(), static_cast<std::size_t>(std::count_if(all.begin(), all.end(), [](const MutantInfo& m) { return m.category == "memory"; })));
  for (const int id : mem) EXPECT_EQ(all[static_cast<std::size_t>(id - 1)].category, "memory");

  const std::vector<int> not_shift = select_mutants("-category:shift\n", 0, 1, error);
  EXPECT_EQ(error, "");
  for (const int id : not_shift) EXPECT_NE(all[static_cast<std::size_t>(id - 1)].category, "shift");
  EXPECT_EQ(not_shift.size() + static_cast<std::size_t>(std::count_if(all.begin(), all.end(), [](const MutantInfo& m) { return m.category == "shift"; })), all.size());

  // Excludes always win over includes.
  const std::vector<int> both = select_mutants("category:alu\n-class:" + all_mutants()[0].op_class + "\n", 0, 1, error);
  EXPECT_EQ(error, "");
  for (const int id : both) {
    EXPECT_EQ(all[static_cast<std::size_t>(id - 1)].category, "alu");
    EXPECT_NE(all[static_cast<std::size_t>(id - 1)].op_class, all[0].op_class);
  }
}

TEST(DrillSelect, MalformedRulesAndEmptySelectionsAreExplained) {
  std::string error;
  EXPECT_TRUE(select_mutants("frobnicate\n", 0, 1, error).empty());
  EXPECT_NE(error.find("line 1"), std::string::npos) << error;
  error.clear();
  EXPECT_TRUE(select_mutants("# fine\nclass:no_such_class\n", 0, 1, error).empty());
  EXPECT_NE(error.find("line 2"), std::string::npos) << error;
  error.clear();
  EXPECT_TRUE(select_mutants("category:memory\n-category:memory\n", 0, 1, error).empty());
  EXPECT_NE(error.find("leaves no mutants"), std::string::npos) << error;
}

TEST(DrillSelect, SamplesAreDeterministicSortedUniqueAndSeedDependent) {
  std::string error;
  const std::vector<int> a = select_mutants("", 20, 7, error), b = select_mutants("", 20, 7, error), c = select_mutants("", 20, 8, error);
  EXPECT_EQ(error, "");
  EXPECT_EQ(a.size(), 20u);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_TRUE(std::is_sorted(a.begin(), a.end()));
  EXPECT_EQ(std::adjacent_find(a.begin(), a.end()), a.end());
  EXPECT_EQ(select_mutants("", 5000, 1, error).size(), 201u) << "a sample larger than the pool is the whole pool";
  EXPECT_EQ(select_mutants("category:alu", 3, 1, error).size(), 3u);
}

// ---------------------------------------------------------------- tally

TEST(DrillTally, FunnelOutcomesAndFirstProgramIds) {
  DrillTally t({10, 20, 30, 40});
  using namespace cutoff::engine;
  const std::uint8_t ran = kLevelRan, inf = kLevelInfected | kLevelRan, prop = kLevelInfected | kLevelRan | kLevelPropagated,
                     det = kLevelInfected | kLevelRan | kLevelPropagated | kLevelDetected;
  t.observe(0, std::vector<std::uint8_t>{0, 0, 0, 0});            // filter skipped everything
  t.observe(1, std::vector<std::uint8_t>{ran, inf, prop, ran});
  t.observe(2, std::vector<std::uint8_t>{det, inf, prop, ran});
  t.observe(3, std::vector<std::uint8_t>{det, inf, det, ran});
  EXPECT_EQ(t.programs(), 4u);
  EXPECT_EQ(t.pairs_total(), 16u);
  EXPECT_EQ(t.pairs_skipped(), 4u);
  EXPECT_EQ(t.propagated_without_infection(), 0u);
  EXPECT_EQ(t.detected_without_propagation(), 0u);
  const auto& m = t.mutants();
  EXPECT_EQ(DrillTally::outcome(m[0]), Outcome::Killed);
  EXPECT_EQ(DrillTally::outcome(m[1]), Outcome::InfectedNotPropagated);
  EXPECT_EQ(DrillTally::outcome(m[2]), Outcome::Killed);
  EXPECT_EQ(DrillTally::outcome(m[3]), Outcome::NeverInfected);
  EXPECT_EQ(m[0].first_detected, 2u);
  EXPECT_EQ(m[2].first_propagated, 1u);
  EXPECT_EQ(m[2].first_detected, 3u);
  EXPECT_EQ(m[1].first_infected, 1u);
  EXPECT_EQ(m[3].first_infected, kNever);
  EXPECT_EQ(m[0].ran, 3u);
  EXPECT_EQ(m[0].detected, 2u);

  DrillTally p({1});
  p.observe(0, std::vector<std::uint8_t>{kLevelRan | kLevelInfected | kLevelPropagated});
  EXPECT_EQ(DrillTally::outcome(p.mutants()[0]), Outcome::PropagatedNotDetected);
  EXPECT_STREQ(to_string(Outcome::PropagatedNotDetected), "survivor:propagated-not-detected");
}

TEST(DrillTally, SoundnessViolationsAreCounted) {
  DrillTally t({1, 2});
  t.observe(0, std::vector<std::uint8_t>{kLevelRan | kLevelPropagated, kLevelRan | kLevelInfected | kLevelDetected});
  EXPECT_EQ(t.propagated_without_infection(), 1u) << "propagated although the infected bit was clear";
  EXPECT_EQ(t.detected_without_propagation(), 1u) << "detected although the run did not differ";
}

TEST(DrillTally, WordsRoundTripAndRejectTheWrongShape) {
  DrillTally t({3, 5, 9});
  for (std::uint64_t i = 0; i < 20; ++i) {
    const std::uint8_t a = (i % 3 == 0) ? kLevelRan | kLevelInfected : 0;
    const std::uint8_t b = (i % 5 == 0) ? kLevelRan | kLevelInfected | kLevelPropagated | kLevelDetected : kLevelRan;
    t.observe(i, std::vector<std::uint8_t>{a, b, 0});
  }
  const std::vector<std::uint64_t> w = t.to_words();
  bool ok = false;
  const DrillTally back = DrillTally::from_words({3, 5, 9}, w, ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(back.to_words(), w);
  EXPECT_EQ(back.programs(), 20u);
  auto shorter = w;
  shorter.pop_back();
  (void)DrillTally::from_words({3, 5, 9}, shorter, ok);
  EXPECT_FALSE(ok);
  (void)DrillTally::from_words({3, 5}, w, ok);
  EXPECT_FALSE(ok) << "a different selection cannot adopt the saved counters";
}

// ---------------------------------------------------------------- report

TEST(DrillReport, NumbersTsvAndJsonAgreeWithTheTally) {
  const std::vector<MutantInfo> infos = all_mutants();
  DrillTally t({1, 2, 3, 4, 5});
  const std::uint8_t det = kLevelInfected | kLevelRan | kLevelPropagated | kLevelDetected, inf = kLevelInfected | kLevelRan,
                     prop = kLevelInfected | kLevelRan | kLevelPropagated;
  t.observe(0, std::vector<std::uint8_t>{det, 0, inf, prop, 0});
  t.observe(1, std::vector<std::uint8_t>{det, 0, inf, prop, 0});
  t.observe(2, std::vector<std::uint8_t>{det, 0, inf, prop, 0});
  // mutant 1 killed at 0; 2 never infected; 3 infected-not-propagated; 4 propagated-not-detected; 5 never infected.
  const DrillReport r = make_drill_report(t, infos, 0);
  EXPECT_EQ(r.planted, 5u);
  EXPECT_EQ(r.killed, 1u);
  EXPECT_EQ(r.never_infected, 2u);
  EXPECT_EQ(r.infected_not_propagated, 1u);
  EXPECT_EQ(r.propagated_not_detected, 1u);
  EXPECT_EQ(r.infected_any, 3u);
  EXPECT_EQ(r.propagated_any, 2u);
  EXPECT_DOUBLE_EQ(r.score_raw, 0.2);
  EXPECT_DOUBLE_EQ(r.score_upper, 0.25) << "killed / (planted - infected-not-propagated)";
  EXPECT_NE(drill_text(r).find("the stop rule did not fire"), std::string::npos);
  const std::string json = drill_json(r);
  EXPECT_NE(json.find("\"planted\": 5"), std::string::npos);
  EXPECT_NE(json.find("\"stop_at\": null"), std::string::npos);
  EXPECT_NE(json.find("\"never_infected\": 2"), std::string::npos);
  EXPECT_NE(json.find("\"propagated_not_detected\": 1"), std::string::npos);

  DrillReport long_scores;
  long_scores.score_raw = 0.825871;
  long_scores.score_upper = 0.897297;
  const std::string long_score_json = drill_json(long_scores);
  EXPECT_NE(long_score_json.find("\"mutation_score_upper_bound\": 0.897297"), std::string::npos);

  // With a stop point: mutant 1 was first detected at program 0, before the stop at 2, so it is found by the stop.
  const DrillReport s = make_drill_report(t, infos, 2);
  EXPECT_EQ(s.killed_by_stop, 1u);
  EXPECT_NE(drill_text(s).find("found by stop"), std::string::npos);
  const DrillReport late = make_drill_report(t, infos, 1);  // detected at 0 < 1 still counts
  EXPECT_EQ(late.killed_by_stop, 1u);
  EXPECT_NE(drill_json(s).find("\"stop_at\": 2"), std::string::npos);

  const std::vector<std::string> rows = split(drill_tsv(t, infos, 2), '\n');
  ASSERT_EQ(rows.size(), 6u);
  EXPECT_EQ(split(rows[0], '\t').size(), 14u);
  for (std::size_t i = 1; i < rows.size(); ++i) EXPECT_EQ(split(rows[i], '\t').size(), 14u) << rows[i];
  EXPECT_NE(rows[1].find("killed"), std::string::npos);
  EXPECT_EQ(split(rows[1], '\t').back(), "yes");
  EXPECT_EQ(split(rows[2], '\t').back(), "no");
  EXPECT_EQ(split(drill_tsv(t, infos, 0), '\n')[1].substr(rows[1].rfind('\t')), "\t-");
}

// ---------------------------------------------------------------- wire frame

TEST(DrillFrames, DrillBytesRoundTripAndPartialFramesAreIncomplete) {
  ProgramResult a;
  a.rec.program_id = 9;
  a.species[0] = {4, 5};
  a.drill = {0, 1, 3, 7, 15, 2};
  ProgramResult b;
  b.rec.program_id = 10;
  std::vector<char> wire;
  encode_frame(a, wire);
  const std::size_t first = wire.size();
  encode_frame(b, wire);

  ProgramResult got;
  ASSERT_EQ(decode_frame(wire.data(), wire.size(), got), first);
  EXPECT_EQ(got.drill, a.drill);
  EXPECT_EQ(got.species, a.species);
  ASSERT_EQ(decode_frame(wire.data() + first, wire.size() - first, got), wire.size() - first);
  EXPECT_TRUE(got.drill.empty());
  for (std::size_t cut = 0; cut < first; ++cut) ASSERT_EQ(decode_frame(wire.data(), cut, got), 0u) << "cut at " << cut;
}

TEST(DrillFrames, AFullPoolOfMutantsFitsInOneFrame) {
  ProgramResult a;
  a.drill.assign(201, kLevelRan | kLevelInfected);
  std::vector<char> wire;
  encode_frame(a, wire);
  ProgramResult got;
  ASSERT_EQ(decode_frame(wire.data(), wire.size(), got), wire.size());
  EXPECT_EQ(got.drill.size(), 201u);
}

// ---------------------------------------------------------------- the infected-bit model

TEST(DrillInfectSim, TheInfectedCoreIsCycleExactlyTheOriginalIncludingTheUntimedBusSequence) {
  const gen::GenConfig cfg;
  for (std::uint64_t i = 0; i < 1500; ++i) {
    const gen::Program p = gen::generate(cfg, gen::program_seed(70, i));
    const RunResult a = run_program(p.code);
    const InfectedRun b = run_program_infected(p.code);
    ASSERT_EQ(a.status, b.run.status) << i;
    ASSERT_EQ(a.exit_code, b.run.exit_code) << i;
    ASSERT_EQ(a.cycles, b.run.cycles) << i;
    ASSERT_EQ(a.trace_hash, b.run.trace_hash) << i;
    ASSERT_EQ(a.bus_hash, b.run.bus_hash) << i;
    ASSERT_EQ(a.regs, b.run.regs) << i;
    ASSERT_EQ(a.mem_hash, b.run.mem_hash) << i;
  }
}

TEST(DrillInfectSim, TheInstrumentedCoreWithMutantZeroHasTheSameUntimedBusHashAsTheOriginal) {
  const gen::GenConfig cfg;
  for (std::uint64_t i = 0; i < 300; ++i) {
    const gen::Program p = gen::generate(cfg, gen::program_seed(71, i));
    ASSERT_EQ(run_program(p.code).bus_hash, run_program_mutant(p.code, 0).bus_hash) << i;
  }
}

TEST(DrillInfectSim, ABugInTheAdderIsInfectedByEveryProgramThatAdds) {
  const int m = find_mutant(1240, "+ -> -");
  ASSERT_GT(m, 0);
  const gen::GenConfig cfg;
  int infected = 0;
  for (std::uint64_t i = 0; i < 150; ++i) {
    const gen::Program p = gen::generate(cfg, gen::program_seed(72, i));
    infected += run_program_infected(p.code).infected(m) ? 1 : 0;
  }
  EXPECT_EQ(infected, 150);
}

TEST(DrillInfectSim, SignExtensionMutantsAreInfectedEverywhereButOnlyExposedByBackwardOffsets) {
  // PicoRV32 decodes the J- and B-immediates of every instruction, jump or not, so a negative I-immediate "infects" the
  // mutant that zero-extends them: the expression value differs, and the effect is masked because the instruction is not a
  // jump. The difference between infected and exposed is exactly what the funnel is for. G0 emits only forward jumps and
  // branches, so on G0 programs these mutants never change a run; hand-written backward offsets do expose them.
  const int j_imm = find_mutant(880, "drop $signed (zero- instead of sign-extend)");
  const int b_imm = find_mutant(1128, "drop $signed (zero- instead of sign-extend)");
  ASSERT_GT(j_imm, 0);
  ASSERT_GT(b_imm, 0);
  const gen::GenConfig cfg;
  int infected_j = 0;
  for (std::uint64_t i = 0; i < 60; ++i) {
    const gen::Program p = gen::generate(cfg, gen::program_seed(73, i));
    const InfectedRun golden = run_program_infected(p.code);
    infected_j += golden.infected(j_imm) ? 1 : 0;
    for (const int m : {j_imm, b_imm}) {
      const RunResult r = run_program_mutant(p.code, static_cast<std::uint32_t>(m));
      ASSERT_EQ(r.status, golden.run.status) << "mutant " << m << " program " << i;
      ASSERT_EQ(r.exit_code, golden.run.exit_code) << "mutant " << m << " program " << i;
      ASSERT_EQ(r.regs, golden.run.regs) << "mutant " << m << " program " << i;
      ASSERT_EQ(r.mem_hash, golden.run.mem_hash) << "mutant " << m << " program " << i;
      ASSERT_EQ(r.bus_hash, golden.run.bus_hash) << "mutant " << m << " program " << i;
    }
  }
  EXPECT_GT(infected_j, 0) << "negative immediates of other instructions reach the J-immediate decoder";

  const std::vector<std::uint32_t> backward_jump = {rv::jal(0, 12), rv::lui(4, 0x10000), rv::sw(1, 4, 0), rv::addi(1, 0, 42), rv::jal(0, -12)};
  const std::vector<std::uint32_t> backward_branch = {rv::addi(1, 0, 3), rv::addi(2, 2, 7), rv::addi(1, 1, -1), rv::bne(1, 0, -8), rv::lui(4, 0x10000), rv::sw(2, 4, 0)};
  for (const auto& [program, m] : {std::pair{backward_jump, j_imm}, std::pair{backward_branch, b_imm}}) {
    const InfectedRun golden = run_program_infected(program);
    EXPECT_TRUE(golden.infected(m));
    const RunResult bad = run_program_mutant(program, static_cast<std::uint32_t>(m), {20000});
    EXPECT_TRUE(bad.status != golden.run.status || bad.exit_code != golden.run.exit_code) << "mutant " << m;
  }
}

TEST(DrillInfectSim, NoMutantChangesARunWithoutHavingBeenInfected) {
  // The soundness claim behind the activation filter, checked by brute force: every mutant on every program, and whenever
  // the mutant run differs from the golden run in anything observable, the golden run must have reported it infected.
  const gen::GenConfig cfg;
  const int n = drill_mutant_count();
  long differing = 0, pairs = 0;
  for (std::uint64_t i = 0; i < 24; ++i) {
    const gen::Program p = gen::generate(cfg, gen::program_seed(74, i));
    const InfectedRun golden = run_program_infected(p.code);
    const RunConfig limited{std::min<std::uint64_t>(100'000, 8 * golden.run.cycles + 2000)};
    for (int m = 1; m <= n; ++m) {
      const RunResult r = run_program_mutant(p.code, static_cast<std::uint32_t>(m), limited);
      const bool differs = r.status != golden.run.status || r.exit_code != golden.run.exit_code || r.regs != golden.run.regs || r.mem_hash != golden.run.mem_hash ||
                           r.bus_hash != golden.run.bus_hash;
      ++pairs;
      if (differs) {
        ++differing;
        ASSERT_TRUE(golden.infected(m)) << "mutant " << m << " changed program " << i << " but was not reported infected";
      }
    }
  }
  std::printf("[DrillInfectSim] %ld of %ld (program, mutant) pairs differ from the golden run; every one was reported infected\n", differing, pairs);
  EXPECT_GT(differing, 100) << "the check must actually see changed runs";
}

// ---------------------------------------------------------------- campaigns

TEST(DrillRun, EventsBinIsExactlyThePlainCampaignsAndTheFilesExist) {
  TempDir tmp;
  Quiet q;
  CampaignConfig plain;
  plain.programs = 120;
  plain.workers = 2;
  plain.results_dir = tmp.str();
  ASSERT_EQ(execute_run(plain, "plain", false, q.out, q.err).exit_code, 0) << q.err.str();

  CampaignConfig drill = small_drill(tmp, 120, 12, 2);
  const RunOutcome o = execute_run(drill, "drill", false, q.out, q.err);
  ASSERT_EQ(o.exit_code, 0) << q.err.str();
  EXPECT_EQ(slurp(tmp.file("plain") / "events.bin"), slurp(tmp.file("drill") / "events.bin")) << "the drill must not change what the golden campaign records";
  const std::string tsv = slurp(tmp.file("drill") / "drill.tsv");
  EXPECT_EQ(split(tsv, '\n').size(), 13u);
  EXPECT_NE(slurp(tmp.file("drill") / "summary.json").find("\"drill\""), std::string::npos);
  EXPECT_NE(q.out.str().find("funnel:"), std::string::npos);
  EXPECT_NE(q.out.str().find("mutation score:"), std::string::npos);
  EXPECT_FALSE(fs::exists(tmp.file("plain") / "drill.tsv"));
}

TEST(DrillRun, TheObviousBugIsKilledByTheFirstProgram) {
  TempDir tmp;
  Quiet q;
  const int m = find_mutant(1240, "+ -> -");
  ASSERT_GT(m, 0);
  const fs::path ops = tmp.file("ops.txt");
  std::ofstream(ops) << "# just the adder\nclass:" << all_mutants()[static_cast<std::size_t>(m - 1)].op_class << "\ncategory:" << all_mutants()[static_cast<std::size_t>(m - 1)].category << "\n";
  CampaignConfig cfg = small_drill(tmp, 40, 0, 2);
  cfg.drill_operators = ops.string();
  ASSERT_EQ(execute_run(cfg, "adder", false, q.out, q.err).exit_code, 0) << q.err.str();
  const std::string tsv = slurp(tmp.file("adder") / "drill.tsv");
  const std::vector<std::string> ids = tsv_column(tsv, "id"), first = tsv_column(tsv, "first_detected"), outcome = tsv_column(tsv, "outcome");
  const auto it = std::find(ids.begin(), ids.end(), std::to_string(m));
  ASSERT_NE(it, ids.end());
  const std::size_t row = static_cast<std::size_t>(it - ids.begin());
  EXPECT_EQ(outcome[row], "killed");
  EXPECT_EQ(first[row], "0");
}

TEST(DrillRun, WorkerCountDoesNotChangeAnyResult) {
  TempDir tmp;
  Quiet q;
  ASSERT_EQ(execute_run(small_drill(tmp, 150, 15, 1), "w1", false, q.out, q.err).exit_code, 0) << q.err.str();
  ASSERT_EQ(execute_run(small_drill(tmp, 150, 15, 4), "w4", false, q.out, q.err).exit_code, 0) << q.err.str();
  EXPECT_EQ(slurp(tmp.file("w1") / "drill.tsv"), slurp(tmp.file("w4") / "drill.tsv"));
  EXPECT_EQ(slurp(tmp.file("w1") / "events.bin"), slurp(tmp.file("w4") / "events.bin"));
}

TEST(DrillRun, TheActivationFilterOnlySkipsWorkAndNeverChangesTheOutcome) {
  TempDir tmp;
  Quiet q;
  CampaignConfig on = small_drill(tmp, 100, 30, 3), off = on;
  off.drill_filter = false;
  ASSERT_EQ(execute_run(on, "on", false, q.out, q.err).exit_code, 0) << q.err.str();
  ASSERT_EQ(execute_run(off, "off", false, q.out, q.err).exit_code, 0) << q.err.str();
  const std::string a = slurp(tmp.file("on") / "drill.tsv"), b = slurp(tmp.file("off") / "drill.tsv");
  EXPECT_EQ(tsv_rows(a, {"ran"}), tsv_rows(b, {"ran"})) << "everything but the number of simulated runs must agree";
  long ran_on = 0, ran_off = 0;
  for (const std::string& v : tsv_column(a, "ran")) ran_on += std::stol(v);
  for (const std::string& v : tsv_column(b, "ran")) ran_off += std::stol(v);
  EXPECT_EQ(ran_off, 100L * 30);
  EXPECT_LT(ran_on, ran_off);
  const std::string summary = slurp(tmp.file("off") / "summary.json");
  EXPECT_NE(summary.find("\"propagated_without_infection\": 0"), std::string::npos) << "brute force found no soundness violation";
  EXPECT_NE(summary.find("\"pairs_skipped\": 0"), std::string::npos);
}

TEST(DrillRun, TheStopRuleIsObservedButTheDrillRunsOnToTheBudget) {
  TempDir tmp;
  Quiet q;
  CampaignConfig cfg = small_drill(tmp, 400, 10, 2);
  cfg.stop.epsilon = 0.5;  // fires at the first checkpoint that satisfies min_programs and confirm
  cfg.stop.confirm = 2;
  cfg.stop.min_programs = 50;
  cfg.stop.checkpoint = 50;
  const RunOutcome o = execute_run(cfg, "stop", false, q.out, q.err);
  ASSERT_EQ(o.exit_code, 0) << q.err.str();
  EXPECT_EQ(o.programs, 400u) << "the drill continues after the stop rule fires";
  const std::string summary = slurp(tmp.file("stop") / "summary.json");
  const auto pos = summary.find("\"stop_at\": ");
  ASSERT_NE(pos, std::string::npos);
  const unsigned long stop_at = std::stoul(summary.substr(pos + 11));
  EXPECT_GE(stop_at, 100u);
  EXPECT_EQ(stop_at % 50, 0u) << "the stop rule is evaluated at checkpoints";
  EXPECT_LT(stop_at, 400u);
  EXPECT_NE(q.out.str().find("found by stop:"), std::string::npos);
  const std::vector<std::string> by_stop = tsv_column(slurp(tmp.file("stop") / "drill.tsv"), "found_by_stop");
  EXPECT_TRUE(std::all_of(by_stop.begin(), by_stop.end(), [](const std::string& s) { return s == "yes" || s == "no"; }));
}

TEST(DrillRun, AnInterruptedDrillResumesToExactlyTheUninterruptedResult) {
  TempDir tmp;
  Quiet q;
  CampaignConfig whole = small_drill(tmp, 200, 20, 3);
  ASSERT_EQ(execute_run(whole, "whole", false, q.out, q.err).exit_code, 0) << q.err.str();

  CampaignConfig cut = whole;
  cut.interrupt_after = 100;
  const RunOutcome first = execute_run(cut, "cut", false, q.out, q.err);
  ASSERT_EQ(first.exit_code, 130);
  CampaignConfig rest = whole;
  rest.workers = 2;  // a different worker count on the way back
  const RunOutcome second = execute_run(rest, "cut", false, q.out, q.err, first.run_dir);
  ASSERT_EQ(second.exit_code, 0) << q.err.str();
  EXPECT_EQ(slurp(tmp.file("whole") / "drill.tsv"), slurp(tmp.file("cut") / "drill.tsv"));
  EXPECT_EQ(slurp(tmp.file("whole") / "events.bin"), slurp(tmp.file("cut") / "events.bin"));
}

TEST(DrillRun, AResumeMustKeepTheDrillSettings) {
  TempDir tmp;
  Quiet q;
  CampaignConfig cfg = small_drill(tmp, 150, 8, 2);
  cfg.interrupt_after = 100;
  const RunOutcome cut = execute_run(cfg, "d", false, q.out, q.err);
  ASSERT_EQ(cut.exit_code, 130);

  CampaignConfig other = cfg;
  other.interrupt_after = 0;
  other.drill_mutants = 9;
  std::ostringstream e1;
  EXPECT_EQ(execute_run(other, "d", false, q.out, e1, cut.run_dir).exit_code, 3);
  EXPECT_NE(e1.str().find("cannot resume"), std::string::npos) << e1.str();

  other = cfg;
  other.interrupt_after = 0;
  other.drill = false;
  std::ostringstream e2;
  EXPECT_EQ(execute_run(other, "d", false, q.out, e2, cut.run_dir).exit_code, 3);
  EXPECT_NE(e2.str().find("cannot resume"), std::string::npos) << e2.str();

  other = cfg;
  other.interrupt_after = 0;
  std::ostringstream e3;
  EXPECT_EQ(execute_run(other, "d", false, q.out, e3, cut.run_dir).exit_code, 0) << e3.str();

  // The operators file is named in the settings but read at start-up: editing it between sessions changes the selection,
  // which the checkpoint's own list of mutants catches.
  const fs::path ops = tmp.file("ops.txt");
  std::ofstream(ops) << "category:shift\n";
  CampaignConfig sel = small_drill(tmp, 150, 0, 2);
  sel.drill_operators = ops.string();
  sel.interrupt_after = 100;
  const RunOutcome scut = execute_run(sel, "s", false, q.out, q.err);
  ASSERT_EQ(scut.exit_code, 130) << q.err.str();
  std::ofstream(ops) << "category:alu\n";
  sel.interrupt_after = 0;
  std::ostringstream e5;
  EXPECT_EQ(execute_run(sel, "s", false, q.out, e5, scut.run_dir).exit_code, 3);
  EXPECT_NE(e5.str().find("different selection of mutants"), std::string::npos) << e5.str();

  // And a plain run cannot be resumed as a drill.
  CampaignConfig plain;
  plain.programs = 150;
  plain.workers = 2;
  plain.results_dir = tmp.str();
  plain.stop.checkpoint = 50;
  plain.interrupt_after = 100;
  const RunOutcome pcut = execute_run(plain, "p", false, q.out, q.err);
  ASSERT_EQ(pcut.exit_code, 130);
  CampaignConfig as_drill = small_drill(tmp, 150, 8, 2);
  std::ostringstream e4;
  EXPECT_EQ(execute_run(as_drill, "p", false, q.out, e4, pcut.run_dir).exit_code, 3);
  EXPECT_NE(e4.str().find("cannot resume"), std::string::npos) << e4.str();
}

TEST(DrillRun, AGoldenCoreFailureIsReportedAndStillCountsTowardsExitCodeOne) {
  TempDir tmp;
  Quiet q;
  CampaignConfig cfg = small_drill(tmp, 60, 5, 2);
  cfg.corrupt_core_every = 7;
  EXPECT_EQ(execute_run(cfg, "bad", false, q.out, q.err).exit_code, 1) << q.err.str();
  EXPECT_NE(q.out.str().find("failures:"), std::string::npos);
}

TEST(DrillCli, OptionsSelectAndSampleTheMutants) {
  TempDir tmp;
  {
    Quiet q;
    EXPECT_EQ(drill_command({"--programs", "30", "--mutants", "6", "--sample-seed", "5", "--workers", "2", "--results-dir", tmp.str(), "--run-id", "a", "--no-progress"}, q.out, q.err), 0) << q.err.str();
    EXPECT_EQ(split(slurp(tmp.file("a") / "drill.tsv"), '\n').size(), 7u);
  }
  {
    Quiet q;
    EXPECT_EQ(drill_command({"--programs", "30", "--mutants", "6", "--sample-seed", "5", "--workers", "1", "--results-dir", tmp.str(), "--run-id", "b", "--no-progress"}, q.out, q.err), 0);
    EXPECT_EQ(tsv_column(slurp(tmp.file("a") / "drill.tsv"), "id"), tsv_column(slurp(tmp.file("b") / "drill.tsv"), "id")) << "same sample seed, same mutants";
  }
  {
    Quiet q;
    EXPECT_EQ(drill_command({"--programs", "30", "--mutants", "6", "--sample-seed", "6", "--workers", "1", "--results-dir", tmp.str(), "--run-id", "c", "--no-progress"}, q.out, q.err), 0);
    EXPECT_NE(tsv_column(slurp(tmp.file("a") / "drill.tsv"), "id"), tsv_column(slurp(tmp.file("c") / "drill.tsv"), "id"));
  }
  {
    const fs::path ops = tmp.file("shift.txt");
    std::ofstream(ops) << "category:shift\n";
    Quiet q;
    EXPECT_EQ(drill_command({"--programs", "20", "--operators", ops.string(), "--workers", "2", "--results-dir", tmp.str(), "--run-id", "d", "--no-progress"}, q.out, q.err), 0) << q.err.str();
    const std::vector<std::string> cats = tsv_column(slurp(tmp.file("d") / "drill.tsv"), "category");
    ASSERT_FALSE(cats.empty());
    EXPECT_TRUE(std::all_of(cats.begin(), cats.end(), [](const std::string& c) { return c == "shift"; }));
  }
}

TEST(DrillCli, BadInputGetsAnExplanationAndExitCodeThree) {
  TempDir tmp;
  const fs::path bad = tmp.file("bad.txt");
  std::ofstream(bad) << "class:nonsense\n";
  {
    Quiet q;
    EXPECT_EQ(drill_command({"--operators", bad.string(), "--results-dir", tmp.str(), "--run-id", "x"}, q.out, q.err), 3);
    EXPECT_NE(q.err.str().find("operators file line 1"), std::string::npos) << q.err.str();
  }
  {
    Quiet q;
    EXPECT_EQ(drill_command({"--operators", tmp.file("missing.txt").string(), "--results-dir", tmp.str(), "--run-id", "y"}, q.out, q.err), 3);
    EXPECT_NE(q.err.str().find("cannot read the operators file"), std::string::npos) << q.err.str();
  }
  {
    Quiet q;
    EXPECT_EQ(drill_command({"--mutants", "many"}, q.out, q.err), 3);
    EXPECT_NE(q.err.str().find("needs a whole number"), std::string::npos);
  }
  {
    Quiet q;
    EXPECT_EQ(run_command({"--mutants", "5"}, q.out, q.err), 3) << "--mutants belongs to `cutoff drill`";
    EXPECT_NE(q.err.str().find("unknown option"), std::string::npos);
  }
}

TEST(DrillCli, HelpDescribesTheDrill) {
  Quiet q;
  EXPECT_EQ(drill_command({"--help"}, q.out, q.err), 0);
  for (const char* word : {"usage: cutoff drill", "--mutants", "--operators", "--sample-seed", "--no-filter", "drill.tsv"}) EXPECT_NE(q.out.str().find(word), std::string::npos) << word;
}

TEST(DrillConfig, DrillSettingsRoundTripAndSeparateTheStream) {
  CampaignConfig c;
  c.drill = true;
  c.drill_mutants = 25;
  c.drill_sample_seed = 11;
  c.drill_filter = false;
  const std::string text = to_toml(c);
  EXPECT_NE(text.find("[drill]"), std::string::npos);
  CampaignConfig back;
  const auto parsed = parse_toml(text, "t");
  ASSERT_EQ(parsed.error, "");
  ASSERT_EQ(apply_toml(back, parsed.doc, "t"), "");
  EXPECT_TRUE(back.drill);
  EXPECT_EQ(back.drill_mutants, 25u);
  EXPECT_EQ(back.drill_sample_seed, 11);
  EXPECT_FALSE(back.drill_filter);
  CampaignConfig plain;
  EXPECT_NE(stream_hash(c), stream_hash(plain)) << "a drill and a plain run must not be resumed into each other";
  EXPECT_NE(apply_toml(back, parse_toml("[drill]\nbogus = 1\n", "t").doc, "t"), "");
}
