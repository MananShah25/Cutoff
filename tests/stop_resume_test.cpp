// Step 5 tests: wire frames, new settings, checkpoint files, the risk monitor, the stop rule inside a campaign, exit
// codes, and checkpoint/resume (including a real Ctrl-C).

#include <gtest/gtest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "campaign.hpp"
#include "campaign_config.hpp"
#include "check.hpp"
#include "checkpoint.hpp"
#include "protocol.hpp"
#include "results.hpp"
#include "risk_monitor.hpp"
#include "run.hpp"
#include "run_command.hpp"

namespace fs = std::filesystem;
using namespace cutoff;
using namespace cutoff::engine;

namespace {

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

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A small, fast campaign. Species default to both sets; the stop rule is off unless a test enables it.
CampaignConfig base_config(const std::string& dir, std::uint64_t programs) {
  CampaignConfig c;
  c.programs = programs;
  c.workers = 2;
  c.results_dir = dir;
  c.stop.checkpoint = 100;
  c.stop.min_programs = 100;
  return c;
}

// SP-op saturates within a few programs (see V-33), so a stop rule on it fires deterministically and early.
CampaignConfig op_only_stop(const std::string& dir, std::uint64_t programs) {
  CampaignConfig c = base_config(dir, programs);
  c.species.cross = false;
  c.stop.epsilon = 0.01;
  c.stop.confirm = 3;
  c.stop.min_programs = 300;
  c.stop.checkpoint = 100;
  return c;
}

RunOutcome run(const CampaignConfig& cfg, const std::string& id, std::string* err_text = nullptr, std::string* out_text = nullptr,
               const std::string& resume = {}) {
  std::ostringstream out, err;
  const RunOutcome r = execute_run(cfg, id, false, out, err, resume);
  if (err_text) *err_text = err.str();
  if (out_text) *out_text = out.str();
  return r;
}

std::string line_with(const std::string& text, const std::string& key) {
  const std::size_t p = text.find(key);
  return p == std::string::npos ? std::string() : text.substr(p, text.find('\n', p) - p);
}

// The deterministic parts of summary.json (everything except timing, run id/sessions, worker count).
std::string stable_summary(const fs::path& dir) {
  const std::string s = slurp(dir / "summary.json");
  std::string out;
  for (const char* key : {"\"campaign\"", "\"stop\"", "\"risk\"", "\"failures\"", "\"totals\""}) out += line_with(s, key) + "\n";
  return out;
}

struct Collect : RecordSink {
  std::vector<ProgramResult> results;
  bool on_record(const ProgramResult& r) override { results.push_back(r); return true; }
};

}  // namespace

// ---------------------------------------------------------------- wire frames

TEST(Frames, RoundTripWithAndWithoutSpecies) {
  ProgramResult a;
  a.rec.program_id = 42;
  a.rec.seed = 0x1122334455667788ULL;
  a.rec.verdict = 5;
  a.species[0] = {1, 5, 37};
  a.species[1] = {0, 64, 1000, 2623};
  ProgramResult b;  // no species at all
  b.rec.program_id = 43;
  std::vector<char> wire;
  encode_frame(a, wire);
  const std::size_t first = wire.size();
  encode_frame(b, wire);

  ProgramResult got;
  ASSERT_EQ(decode_frame(wire.data(), wire.size(), got), first);
  EXPECT_EQ(std::memcmp(&got.rec, &a.rec, sizeof a.rec), 0);
  EXPECT_EQ(got.species, a.species);
  ASSERT_EQ(decode_frame(wire.data() + first, wire.size() - first, got), wire.size() - first);
  EXPECT_EQ(got.rec.program_id, 43u);
  for (const auto& ids : got.species) EXPECT_TRUE(ids.empty());
}

TEST(Frames, PartialFramesAreReportedAsIncompleteAtEveryCutPoint) {
  ProgramResult a;
  a.species[0] = {1, 2, 3};
  a.species[1] = {10, 20};
  std::vector<char> wire;
  encode_frame(a, wire);
  ProgramResult got;
  for (std::size_t cut = 0; cut < wire.size(); ++cut) ASSERT_EQ(decode_frame(wire.data(), cut, got), 0u) << "cut at " << cut;
  EXPECT_EQ(decode_frame(wire.data(), wire.size(), got), wire.size());
}

TEST(Frames, WorkersReturnSortedUniqueSpeciesInsideTheUniverse) {
  CampaignConfig cfg = base_config("unused", 10);
  for (std::uint64_t id = 0; id < 40; ++id) {
    const ProgramResult r = evaluate(cfg, id);
    for (const risk::SpeciesSet s : {risk::SpeciesSet::Op, risk::SpeciesSet::Cross}) {
      const auto& ids = r.species[static_cast<std::size_t>(s)];
      ASSERT_FALSE(ids.empty());
      EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
      EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
      EXPECT_LT(ids.back(), risk::universe_size(s));
    }
    EXPECT_TRUE(r.species[2].empty() && r.species[3].empty());
  }
  cfg.species.cross = false;
  EXPECT_TRUE(evaluate(cfg, 0).species[1].empty()) << "a disabled set is not computed";
  cfg.species.op = false;
  EXPECT_TRUE(evaluate(cfg, 0).species[0].empty());
}

TEST(Frames, SpeciesStreamDoesNotDependOnTheWorkerCount) {
  CampaignConfig cfg = base_config("unused", 300);
  Collect one, many;
  cfg.workers = 1;
  ASSERT_TRUE(run_campaign(cfg, one).ok);
  cfg.workers = 5;
  cfg.batch_size = 7;
  ASSERT_TRUE(run_campaign(cfg, many).ok);
  ASSERT_EQ(one.results.size(), many.results.size());
  for (std::size_t i = 0; i < one.results.size(); ++i) ASSERT_EQ(one.results[i].species, many.results[i].species) << i;
}

// ---------------------------------------------------------------- settings

TEST(StopSettings, ParseValidateAndRoundTrip) {
  CampaignConfig c;
  const auto parsed = parse_toml("[stop]\nepsilon = 0.002\nconfirm = 5\nmin_programs = 2_000\ncheckpoint = 250\nestimator = \"coverage\"\n"
                                 "plateau_window = 777\nmax_hours = 1.5\n[species]\nop = false\ncross = true\n", "s");
  ASSERT_EQ(parsed.error, "");
  ASSERT_EQ(apply_toml(c, parsed.doc, "s"), "");
  EXPECT_DOUBLE_EQ(c.stop.epsilon, 0.002);
  EXPECT_EQ(c.stop.confirm, 5u);
  EXPECT_EQ(c.stop.min_programs, 2000u);
  EXPECT_EQ(c.stop.checkpoint, 250u);
  EXPECT_EQ(c.stop.estimator, risk::Estimator::CoverageRisk);
  EXPECT_EQ(c.stop.plateau_window, 777u);
  EXPECT_DOUBLE_EQ(c.max_hours, 1.5);
  EXPECT_FALSE(c.species.op);
  EXPECT_TRUE(c.species.cross);
  const std::string text = to_toml(c);
  CampaignConfig d;
  const auto again = parse_toml(text, "rt");
  ASSERT_EQ(again.error, "");
  ASSERT_EQ(apply_toml(d, again.doc, "rt"), "");
  EXPECT_EQ(to_toml(d), text);
  EXPECT_DOUBLE_EQ(d.stop.epsilon, 0.002) << "epsilon survives the text round trip exactly";
  CampaignConfig i;
  ASSERT_EQ(apply_toml(i, parse_toml("[stop]\nepsilon = 1\n", "i").doc, "i"), "") << "an integer is accepted where a number is expected";
  EXPECT_DOUBLE_EQ(i.stop.epsilon, 1.0);
}

TEST(StopSettings, BadValuesSaySoAndValidationCatchesUselessRules) {
  auto apply = [](const std::string& text) {
    CampaignConfig c;
    const auto p = parse_toml(text, "f");
    return p.error.empty() ? apply_toml(c, p.doc, "f") : p.error;
  };
  EXPECT_NE(apply("[stop]\nepsilon = -0.1\n").find("non-negative"), std::string::npos);
  EXPECT_NE(apply("[stop]\nepsilon = \"small\"\n").find("must be a number"), std::string::npos);
  EXPECT_NE(apply("[stop]\nconfirm = 0\n").find("between 1"), std::string::npos);
  EXPECT_NE(apply("[stop]\nestimator = \"magic\"\n").find("good-turing"), std::string::npos);
  EXPECT_NE(apply("[species]\nop = 1\n").find("true or false"), std::string::npos);
  CampaignConfig c;
  c.stop.epsilon = 1.0;
  EXPECT_NE(validate(c).find("(0, 1)"), std::string::npos);
  c.stop.epsilon = 0.01;
  c.species.op = c.species.cross = false;
  EXPECT_NE(validate(c).find("species"), std::string::npos);
  c.species.op = true;
  c.programs = 50;
  c.stop.checkpoint = 100;
  EXPECT_NE(validate(c).find("never be evaluated"), std::string::npos);
  c.stop.epsilon = 0.0;
  EXPECT_EQ(validate(c), "") << "with the rule off a large checkpoint interval is harmless";
}

TEST(StopSettings, StreamHashIgnoresBudgetsAndStopSettingsButNotWhatDefinesThePrograms) {
  CampaignConfig a;
  CampaignConfig b = a;
  b.programs = 12345;
  b.workers = 3;
  b.stop.epsilon = 0.5;
  b.stop.confirm = 9;
  b.stop.checkpoint = 7;
  b.max_hours = 2;
  b.interrupt_after = 55;
  b.results_dir = "x";
  EXPECT_EQ(stream_hash(a), stream_hash(b));
  for (auto edit : std::vector<void (*)(CampaignConfig&)>{
           [](CampaignConfig& c) { c.seed = 2; }, [](CampaignConfig& c) { c.gen.max_len = 99; }, [](CampaignConfig& c) { c.species.cross = false; },
           [](CampaignConfig& c) { c.max_cycles = 300000; }, [](CampaignConfig& c) { c.corrupt_core_every = 10; }}) {
    CampaignConfig d = a;
    edit(d);
    EXPECT_NE(stream_hash(a), stream_hash(d));
  }
}

// ---------------------------------------------------------------- checkpoint file

TEST(CheckpointFile, RoundTripDetectsDamageAndLeavesNoTempFile) {
  TempDir tmp;
  const std::string path = tmp.str() + "/checkpoint.bin";
  Checkpoint cp;
  cp.stream_hash = 0xabcdef;
  cp.next_program_id = 777;
  cp.events_bytes = 40 + 777 * 72;
  cp.curve_bytes = 999;
  cp.stop_consecutive = 2;
  cp.plateau_stop_at = 600;
  cp.elapsed_seconds = 12.5;
  cp.sessions = 3;
  cp.totals.programs = 777;
  cp.totals.failures = 4;
  cp.totals.by_verdict[5] = 4;
  cp.totals.core_cycles = 123456789012ULL;
  cp.totals.failure_dirs_written = 3;
  cp.sets.push_back({0, 11, {1, 0, 3, 7}});
  cp.sets.push_back({1, 700, std::vector<std::uint32_t>(2624, 2)});
  std::string err;
  ASSERT_TRUE(write_checkpoint(path, cp, err)) << err;
  EXPECT_FALSE(fs::exists(path + ".tmp"));

  Checkpoint got;
  ASSERT_TRUE(read_checkpoint(path, got, err)) << err;
  EXPECT_EQ(got.stream_hash, cp.stream_hash);
  EXPECT_EQ(got.next_program_id, 777u);
  EXPECT_EQ(got.events_bytes, cp.events_bytes);
  EXPECT_EQ(got.curve_bytes, 999u);
  EXPECT_EQ(got.stop_consecutive, 2u);
  EXPECT_EQ(got.plateau_stop_at, 600u);
  EXPECT_DOUBLE_EQ(got.elapsed_seconds, 12.5);
  EXPECT_EQ(got.sessions, 3u);
  EXPECT_EQ(got.totals.core_cycles, 123456789012ULL);
  EXPECT_EQ(got.totals.by_verdict[5], 4u);
  ASSERT_EQ(got.sets.size(), 2u);
  EXPECT_EQ(got.sets[0].per_species, (std::vector<std::uint32_t>{1, 0, 3, 7}));
  EXPECT_EQ(got.sets[1].per_species.size(), 2624u);

  const std::string good = slurp(path);
  auto write_bytes = [&](const std::string& bytes) { std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes; };
  for (std::size_t flip : {std::size_t(9), good.size() / 2, good.size() - 3}) {  // version, middle of the body, checksum
    std::string bad = good;
    bad[flip] = static_cast<char>(static_cast<unsigned char>(bad[flip]) ^ 0x40u);
    write_bytes(bad);
    EXPECT_FALSE(read_checkpoint(path, got, err)) << "flipped byte " << flip;
    EXPECT_NE(err.find("damaged"), std::string::npos) << err;
  }
  write_bytes(good.substr(0, good.size() / 2));
  EXPECT_FALSE(read_checkpoint(path, got, err));
  write_bytes("NOTACHECKPOINT-NOTACHECKPOINT");
  EXPECT_FALSE(read_checkpoint(path, got, err));
  EXPECT_NE(err.find("not a Cutoff checkpoint"), std::string::npos);
  fs::remove(path);
  EXPECT_FALSE(read_checkpoint(path, got, err));
  EXPECT_NE(err.find("no checkpoint"), std::string::npos);
}

// ---------------------------------------------------------------- risk monitor

TEST(RiskMonitorTest, WritesCurveRowsAndStateSurvivesExportAndImport) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 400);
  cfg.stop.checkpoint = 50;
  cfg.stop.epsilon = 0.0;
  std::vector<ProgramResult> stream;
  stream.reserve(400);
  for (std::uint64_t id = 0; id < 400; ++id) stream.push_back(evaluate(cfg, id));

  RiskMonitor whole(cfg);
  std::string err;
  ASSERT_TRUE(whole.open_curve(tmp.str() + "/whole.csv", false, 0, err)) << err;
  for (const auto& r : stream) {
    whole.add_program(r.species);
    if (whole.at_checkpoint()) whole.evaluate();
  }
  whole.curve_bytes();  // flushes the file
  const std::string whole_csv = slurp(tmp.str() + "/whole.csv");
  EXPECT_EQ(std::count(whole_csv.begin(), whole_csv.end(), '\n'), 1 + 8 * 2) << "header + 8 checkpoints x 2 sets";

  // Same stream, interrupted at 237 and continued from the exported state in a fresh monitor.
  RiskMonitor first(cfg);
  ASSERT_TRUE(first.open_curve(tmp.str() + "/split.csv", false, 0, err));
  for (std::size_t i = 0; i < 237; ++i) {
    first.add_program(stream[i].species);
    if (first.at_checkpoint()) first.evaluate();
  }
  Checkpoint cp;
  cp.next_program_id = 237;
  cp.curve_bytes = first.curve_bytes();
  first.export_state(cp);
  RiskMonitor second(cfg);
  // Junk after the checkpointed length must be discarded when the curve is reopened for resume.
  { std::ofstream(tmp.str() + "/split.csv", std::ios::app) << "junk,junk\n"; }
  ASSERT_TRUE(second.open_curve(tmp.str() + "/split.csv", true, cp.curve_bytes, err)) << err;
  ASSERT_TRUE(second.import_state(cp, err)) << err;
  for (std::size_t i = 237; i < stream.size(); ++i) {
    second.add_program(stream[i].species);
    if (second.at_checkpoint()) second.evaluate();
  }
  second.curve_bytes();
  EXPECT_EQ(slurp(tmp.str() + "/split.csv"), whole_csv);
  ASSERT_EQ(second.summary().size(), whole.summary().size());
  for (std::size_t i = 0; i < whole.summary().size(); ++i) {
    EXPECT_EQ(second.summary()[i].counts.q1, whole.summary()[i].counts.q1);
    EXPECT_EQ(second.summary()[i].counts.v, whole.summary()[i].counts.v);
    EXPECT_EQ(second.summary()[i].programs_since_new, whole.summary()[i].programs_since_new);
  }
}

TEST(RiskMonitorTest, CurveRowsAreSelfConsistent) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 300);
  cfg.stop.checkpoint = 100;
  const RunOutcome r = run(cfg, "curve");
  ASSERT_EQ(r.exit_code, 0);
  std::istringstream in(slurp(fs::path(r.run_dir) / "risk_curve.csv"));
  std::string line;
  std::getline(in, line);
  EXPECT_EQ(line, "n,set,species_seen,q1,q2,v,good_turing,incidence,coverage,chao2,chao2_bias_corrected,programs_since_new");
  int rows = 0;
  unsigned long long last_n = 0, last_seen_op = 0, last_seen_cross = 0;
  while (std::getline(in, line)) {
    ++rows;
    unsigned long long n, seen, q1, q2, v, since;
    double gt, inc, cov, c2, c2bc;
    char set[16];
    ASSERT_EQ(std::sscanf(line.c_str(),  // NOLINT(bugprone-unchecked-string-to-number-conversion): the return value (field count) is checked
 "%llu,%15[^,],%llu,%llu,%llu,%llu,%lf,%lf,%lf,%lf,%lf,%llu", &n, set, &seen, &q1, &q2, &v, &gt, &inc, &cov, &c2, &c2bc, &since), 12) << line;
    EXPECT_EQ(n % 100, 0u);
    EXPECT_GE(n, last_n);
    last_n = n;
    EXPECT_NEAR(gt, static_cast<double>(q1) / static_cast<double>(n), 1e-8 * std::max(1.0, gt));
    EXPECT_NEAR(inc, static_cast<double>(q1) / static_cast<double>(v), 1e-8 * std::max(1.0, inc));
    EXPECT_LE(cov, inc + 1e-12);
    EXPECT_GE(c2, static_cast<double>(seen) - 1e-9);
    EXPECT_LE(since, n);
    unsigned long long& prev = std::string(set) == "op" ? last_seen_op : last_seen_cross;
    EXPECT_GE(seen, prev) << "species seen never decreases";
    prev = seen;
  }
  EXPECT_EQ(rows, 3 * 2);
}

// ---------------------------------------------------------------- stop rule inside a campaign

TEST(StopInCampaign, StopsAtTheThresholdOnACheckpointAndExitsZero) {
  TempDir tmp;
  CampaignConfig cfg = op_only_stop(tmp.str(), 5000);
  std::string out;
  const RunOutcome r = run(cfg, "t", nullptr, &out);
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.stop_reason, "threshold");
  EXPECT_EQ(r.programs, 500u) << "min_programs 300: checkpoints 300, 400, 500 are the three confirmations";
  EXPECT_NE(out.find("risk threshold reached"), std::string::npos) << out;
  EXPECT_EQ(slurp(fs::path(r.run_dir) / "events.bin").size(), sizeof(EventsHeader) + 500 * sizeof(EventRecord));
  const std::string summary = slurp(fs::path(r.run_dir) / "summary.json");
  EXPECT_NE(summary.find("\"stop_reason\": \"threshold\""), std::string::npos);
  EXPECT_NE(summary.find("\"programs_at_stop\": 500"), std::string::npos);
  EXPECT_NE(summary.find("\"fixed_budget\": 5000"), std::string::npos);
  const std::string curve = slurp(fs::path(r.run_dir) / "risk_curve.csv");
  EXPECT_EQ(std::count(curve.begin(), curve.end(), '\n'), 1 + 5) << "checkpoints 100..500";
}

TEST(StopInCampaign, SameStopPointForAnyWorkerCount) {
  TempDir tmp;
  std::string events[2];
  std::string curves[2];
  std::uint64_t at[2];
  int k = 0;
  for (const std::uint32_t workers : {1u, 6u}) {
    CampaignConfig cfg = op_only_stop(tmp.str(), 5000);
    cfg.workers = workers;
    cfg.batch_size = workers == 1 ? 64 : 5;
    const RunOutcome r = run(cfg, "w" + std::to_string(workers));
    ASSERT_EQ(r.exit_code, 0);
    at[k] = r.programs;
    events[k] = slurp(fs::path(r.run_dir) / "events.bin");
    curves[k] = slurp(fs::path(r.run_dir) / "risk_curve.csv");
    ++k;
  }
  EXPECT_EQ(at[0], at[1]);
  EXPECT_EQ(events[0], events[1]);
  EXPECT_EQ(curves[0], curves[1]);
}

TEST(StopInCampaign, NoStopBeforeMinProgramsEvenWhenTheRiskIsAlreadyZero) {
  TempDir tmp;
  CampaignConfig cfg = op_only_stop(tmp.str(), 5000);
  cfg.stop.min_programs = 1000;
  cfg.stop.epsilon = 0.99;
  const RunOutcome r = run(cfg, "min");
  EXPECT_EQ(r.programs, 1200u) << "confirmations at 1000, 1100, 1200";
}

TEST(StopInCampaign, BudgetEndingFirstGivesExitCodeTwo) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 600);
  cfg.stop.epsilon = 1e-12;  // unreachable: SP-cross keeps finding new cells
  std::string out;
  const RunOutcome r = run(cfg, "b", nullptr, &out);
  EXPECT_EQ(r.exit_code, 2);
  EXPECT_EQ(r.stop_reason, "budget");
  EXPECT_EQ(r.programs, 600u);
  EXPECT_NE(out.find("program budget (600) used up"), std::string::npos) << out;
  EXPECT_NE(slurp(fs::path(r.run_dir) / "summary.json").find("\"exit_code\": 2"), std::string::npos);
}

TEST(StopInCampaign, FailuresOutrankTheStopReason) {
  TempDir tmp;
  CampaignConfig cfg = op_only_stop(tmp.str(), 5000);
  cfg.corrupt_core_every = 100;
  const RunOutcome r = run(cfg, "f");
  EXPECT_EQ(r.stop_reason, "threshold");
  EXPECT_EQ(r.exit_code, 1) << "a failure was found, so the run is not a pass even though the threshold was reached";
  EXPECT_EQ(r.failures, 5u);
}

TEST(StopInCampaign, TimeBudgetStopsAtTheNextCheckpointWithExitCodeTwo) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 100000);
  cfg.max_hours = 1e-9;
  cfg.stop.epsilon = 1e-12;
  const RunOutcome r = run(cfg, "h");
  EXPECT_EQ(r.stop_reason, "time-budget");
  EXPECT_EQ(r.exit_code, 2);
  EXPECT_EQ(r.programs, 100u) << "the first checkpoint";
}

TEST(StopInCampaign, WithTheRuleOffAllProgramsRunAndExitIsZero) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 450);
  const RunOutcome r = run(cfg, "off");
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.stop_reason, "program-count");
  EXPECT_EQ(r.programs, 450u);
  const std::string curve = slurp(fs::path(r.run_dir) / "risk_curve.csv");
  EXPECT_EQ(std::count(curve.begin(), curve.end(), '\n'), 1 + 4 * 2) << "the curve is recorded even when the rule is off";
}

TEST(StopInCampaign, PlateauBaselineIsReportedAlongside) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 600);
  cfg.species.cross = false;      // SP-op: no new species after the first few programs
  cfg.stop.plateau_window = 150;
  const RunOutcome r = run(cfg, "p");
  const std::string summary = slurp(fs::path(r.run_dir) / "summary.json");
  EXPECT_NE(summary.find("\"plateau_window\": 150"), std::string::npos);
  EXPECT_NE(summary.find("\"plateau_rule_would_have_stopped_at\": 200"), std::string::npos) << summary;
}

TEST(StopInCampaign, CliFlagsReachTheRule) {
  TempDir tmp;
  std::ostringstream out, err;
  // SP-cross is still finding cells at 5,000 programs (risk about 0.02), so this threshold is met well before the budget.
  const int code = run_command({"--max-programs", "5000", "--epsilon", "0.25", "--confirm", "2", "--min-programs", "200", "--checkpoint", "100",
                                "--estimator", "good-turing", "--workers", "2", "--results-dir", tmp.str(), "--run-id", "c", "--no-progress"}, out, err);
  EXPECT_EQ(code, 0) << err.str();
  EXPECT_NE(out.str().find("risk threshold reached"), std::string::npos) << out.str();
  const std::string events = slurp(fs::path(tmp.str()) / "c" / "events.bin");
  const std::size_t stopped_at = (events.size() - sizeof(EventsHeader)) / sizeof(EventRecord);
  EXPECT_GE(stopped_at, 400u) << "two confirmations at or after min_programs 200 take at least checkpoints 200 and 300";
  EXPECT_LT(stopped_at, 5000u);
  EXPECT_EQ(stopped_at % 100, 0u) << "a stop always lands on a checkpoint";
  EXPECT_NE(slurp(fs::path(tmp.str()) / "c" / "config.toml").find("confirm = 2"), std::string::npos);
  EXPECT_EQ(run_command({"--epsilon", "tiny"}, out, err), 3);
  EXPECT_NE(err.str().find("needs a number"), std::string::npos);
  std::ostringstream err2;
  EXPECT_EQ(run_command({"--max-programs", "300", "--epsilon", "1e-12", "--checkpoint", "100", "--results-dir", tmp.str(), "--run-id", "two", "--no-progress"}, out, err2), 2);
}

// ---------------------------------------------------------------- resume

TEST(Resume, AnInterruptedRunContinuesToExactlyTheSameFilesAsAnUninterruptedOne) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 1500);
  cfg.stop.checkpoint = 100;
  const RunOutcome whole = run(cfg, "whole");
  ASSERT_EQ(whole.exit_code, 0);

  CampaignConfig first = cfg;
  first.interrupt_after = 737;  // not on a checkpoint boundary
  std::string err;
  const RunOutcome cut = run(first, "split", &err);
  EXPECT_EQ(cut.exit_code, 130);
  EXPECT_EQ(cut.stop_reason, "interrupted");
  EXPECT_EQ(cut.programs, 737u);
  EXPECT_NE(err.find("--resume"), std::string::npos) << err;
  EXPECT_TRUE(fs::exists(fs::path(cut.run_dir) / "checkpoint.bin"));
  EXPECT_FALSE(fs::exists(fs::path(cut.run_dir) / "summary.json")) << "an interrupted run has no summary yet";

  CampaignConfig rest = cfg;  // interrupt_after back to 0
  std::string out;
  const RunOutcome done = run(rest, "ignored", &err, &out, cut.run_dir);
  ASSERT_EQ(done.exit_code, 0) << err;
  EXPECT_EQ(done.run_dir, cut.run_dir);
  EXPECT_EQ(done.programs, 1500u);
  EXPECT_EQ(slurp(fs::path(done.run_dir) / "events.bin"), slurp(fs::path(whole.run_dir) / "events.bin"));
  EXPECT_EQ(slurp(fs::path(done.run_dir) / "risk_curve.csv"), slurp(fs::path(whole.run_dir) / "risk_curve.csv"));
  EXPECT_EQ(stable_summary(done.run_dir), stable_summary(whole.run_dir));
  EXPECT_NE(slurp(fs::path(done.run_dir) / "summary.json").find("\"sessions\": 2"), std::string::npos);
  EXPECT_NE(out.find("(this session)"), std::string::npos);
}

TEST(Resume, ThreeInterruptionsStillGiveTheSameResult) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 900);
  const RunOutcome whole = run(cfg, "whole");
  std::string dir;
  for (const std::uint64_t cut_at : {111u, 400u, 401u}) {
    CampaignConfig step = cfg;
    step.interrupt_after = cut_at;
    const RunOutcome r = run(step, "multi", nullptr, nullptr, dir);
    ASSERT_EQ(r.exit_code, 130) << cut_at;
    dir = r.run_dir;
  }
  const RunOutcome done = run(cfg, "multi", nullptr, nullptr, dir);
  ASSERT_EQ(done.exit_code, 0);
  EXPECT_EQ(slurp(fs::path(dir) / "events.bin"), slurp(fs::path(whole.run_dir) / "events.bin"));
  EXPECT_EQ(slurp(fs::path(dir) / "risk_curve.csv"), slurp(fs::path(whole.run_dir) / "risk_curve.csv"));
  EXPECT_EQ(stable_summary(dir), stable_summary(whole.run_dir));
  EXPECT_NE(slurp(fs::path(dir) / "summary.json").find("\"sessions\": 4"), std::string::npos);
}

TEST(Resume, StopRuleDecisionAfterTheResumePointIsTheSame) {
  TempDir tmp;
  CampaignConfig cfg = op_only_stop(tmp.str(), 5000);
  const RunOutcome whole = run(cfg, "whole");
  ASSERT_EQ(whole.programs, 500u);
  for (const std::uint64_t cut_at : {130u, 300u, 399u, 450u}) {  // before, at and inside the confirmation window
    CampaignConfig first = cfg;
    first.interrupt_after = cut_at;
    const RunOutcome cut = run(first, "cut" + std::to_string(cut_at));
    ASSERT_EQ(cut.exit_code, 130) << cut_at;
    const RunOutcome done = run(cfg, "x", nullptr, nullptr, cut.run_dir);
    ASSERT_EQ(done.exit_code, 0) << cut_at;
    EXPECT_EQ(done.programs, 500u) << "cut at " << cut_at;
    EXPECT_EQ(slurp(fs::path(done.run_dir) / "events.bin"), slurp(fs::path(whole.run_dir) / "events.bin")) << cut_at;
    EXPECT_EQ(slurp(fs::path(done.run_dir) / "risk_curve.csv"), slurp(fs::path(whole.run_dir) / "risk_curve.csv")) << cut_at;
  }
}

TEST(Resume, ABudgetThatRanOutCanBeExtended) {
  TempDir tmp;
  CampaignConfig small = base_config(tmp.str(), 300);
  small.stop.epsilon = 1e-12;
  const RunOutcome first = run(small, "ext");
  ASSERT_EQ(first.exit_code, 2);
  CampaignConfig bigger = small;
  bigger.programs = 700;
  const RunOutcome second = run(bigger, "ext", nullptr, nullptr, first.run_dir);
  EXPECT_EQ(second.exit_code, 2);
  EXPECT_EQ(second.programs, 700u);
  CampaignConfig fresh = bigger;
  const RunOutcome reference = run(fresh, "fresh700");
  EXPECT_EQ(slurp(fs::path(second.run_dir) / "events.bin"), slurp(fs::path(reference.run_dir) / "events.bin"));
  EXPECT_EQ(slurp(fs::path(second.run_dir) / "risk_curve.csv"), slurp(fs::path(reference.run_dir) / "risk_curve.csv"));
}

TEST(Resume, ANewThresholdCanBeAppliedToAFinishedRun) {
  TempDir tmp;
  CampaignConfig cfg = op_only_stop(tmp.str(), 5000);
  cfg.stop.epsilon = 1e-12;
  cfg.programs = 400;
  const RunOutcome a = run(cfg, "re");
  ASSERT_EQ(a.exit_code, 2);
  CampaignConfig loosened = cfg;
  loosened.programs = 5000;
  loosened.stop.epsilon = 0.01;  // the saved state (risk 0 at every checkpoint so far) satisfies this after more checkpoints
  const RunOutcome b = run(loosened, "re", nullptr, nullptr, a.run_dir);
  EXPECT_EQ(b.stop_reason, "threshold");
  EXPECT_EQ(b.exit_code, 0);
  EXPECT_EQ(b.programs, 500u);
}

TEST(Resume, ResumingAFinishedThresholdRunChangesNothing) {
  TempDir tmp;
  CampaignConfig cfg = op_only_stop(tmp.str(), 5000);
  const RunOutcome a = run(cfg, "fin");
  ASSERT_EQ(a.programs, 500u);
  const std::string events = slurp(fs::path(a.run_dir) / "events.bin");
  const RunOutcome b = run(cfg, "fin", nullptr, nullptr, a.run_dir);
  EXPECT_EQ(b.exit_code, 0);
  EXPECT_EQ(b.stop_reason, "threshold");
  EXPECT_EQ(b.programs, 500u);
  EXPECT_EQ(slurp(fs::path(b.run_dir) / "events.bin"), events);
  EXPECT_NE(slurp(fs::path(b.run_dir) / "summary.json").find("\"sessions\": 2"), std::string::npos);
}

TEST(Resume, RefusesToMixDifferentProgramStreams) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 300);
  cfg.interrupt_after = 150;
  const RunOutcome cut = run(cfg, "mix");
  ASSERT_EQ(cut.exit_code, 130);
  std::string err;
  CampaignConfig other_seed = base_config(tmp.str(), 300);
  other_seed.seed = 99;
  EXPECT_EQ(run(other_seed, "x", &err, nullptr, cut.run_dir).exit_code, 3);
  EXPECT_NE(err.find("differ from the saved run"), std::string::npos) << err;
  CampaignConfig other_species = base_config(tmp.str(), 300);
  other_species.species.cross = false;
  EXPECT_EQ(run(other_species, "x", &err, nullptr, cut.run_dir).exit_code, 3);
  CampaignConfig other_gen = base_config(tmp.str(), 300);
  other_gen.gen.max_len = 100;
  EXPECT_EQ(run(other_gen, "x", &err, nullptr, cut.run_dir).exit_code, 3);
  // The run is still intact and can be finished with the right settings.
  CampaignConfig right = base_config(tmp.str(), 300);
  EXPECT_EQ(run(right, "x", &err, nullptr, cut.run_dir).exit_code, 0) << err;
}

TEST(Resume, DiscardsWhateverWasWrittenAfterTheLastCheckpoint) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 800);
  const RunOutcome whole = run(cfg, "whole");
  CampaignConfig first = cfg;
  first.interrupt_after = 350;
  const RunOutcome cut = run(first, "crashy");
  ASSERT_EQ(cut.exit_code, 130);
  // Simulate a kill after more output reached the files but before the next checkpoint was written.
  { std::ofstream(fs::path(cut.run_dir) / "events.bin", std::ios::binary | std::ios::app) << std::string(72 * 5 + 11, '\x7f'); }
  { std::ofstream(fs::path(cut.run_dir) / "risk_curve.csv", std::ios::app) << "999,op,1,1,1,1,1,1,1,1,1,1\n"; }
  const RunOutcome done = run(cfg, "x", nullptr, nullptr, cut.run_dir);
  ASSERT_EQ(done.exit_code, 0);
  EXPECT_EQ(slurp(fs::path(done.run_dir) / "events.bin"), slurp(fs::path(whole.run_dir) / "events.bin"));
  EXPECT_EQ(slurp(fs::path(done.run_dir) / "risk_curve.csv"), slurp(fs::path(whole.run_dir) / "risk_curve.csv"));
}

TEST(Resume, FailureFoldersAreRewrittenNotDuplicatedOrLeftStale) {
  TempDir tmp;
  CampaignConfig cfg = base_config(tmp.str(), 600);
  cfg.corrupt_core_every = 100;
  const RunOutcome whole = run(cfg, "whole");
  CampaignConfig first = cfg;
  first.interrupt_after = 250;
  const RunOutcome cut = run(first, "fail");
  ASSERT_EQ(cut.exit_code, 130);
  // A folder for program 299 exists although the checkpoint stops at 250 (the process was killed before checkpointing).
  fs::create_directories(fs::path(cut.run_dir) / "failures" / "program-000000299");
  { std::ofstream(fs::path(cut.run_dir) / "failures" / "program-000000299" / "failure.txt") << "stale"; }
  const RunOutcome done = run(cfg, "x", nullptr, nullptr, cut.run_dir);
  ASSERT_EQ(done.exit_code, 1);
  EXPECT_EQ(done.failures, 6u);
  auto listing = [](const std::string& dir) {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(fs::path(dir) / "failures")) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
  };
  EXPECT_EQ(listing(done.run_dir), listing(whole.run_dir));
  for (const std::string& name : listing(done.run_dir))
    EXPECT_EQ(slurp(fs::path(done.run_dir) / "failures" / name / "failure.txt"), slurp(fs::path(whole.run_dir) / "failures" / name / "failure.txt")) << name;
  EXPECT_EQ(stable_summary(done.run_dir), stable_summary(whole.run_dir));
}

TEST(Resume, BrokenOrMissingCheckpointsAreExplained) {
  TempDir tmp;
  std::string err;
  CampaignConfig cfg = base_config(tmp.str(), 200);
  cfg.interrupt_after = 100;
  const RunOutcome cut = run(cfg, "brk");
  ASSERT_EQ(cut.exit_code, 130);
  CampaignConfig again = base_config(tmp.str(), 200);

  const fs::path cp = fs::path(cut.run_dir) / "checkpoint.bin";
  std::string good = slurp(cp);
  { std::ofstream(cp, std::ios::binary | std::ios::trunc) << good.substr(0, good.size() - 20); }
  EXPECT_EQ(run(again, "x", &err, nullptr, cut.run_dir).exit_code, 3);
  EXPECT_NE(err.find("damaged"), std::string::npos) << err;
  fs::remove(cp);
  EXPECT_EQ(run(again, "x", &err, nullptr, cut.run_dir).exit_code, 3);
  EXPECT_NE(err.find("no checkpoint"), std::string::npos) << err;
  { std::ofstream(cp, std::ios::binary) << good; }
  fs::resize_file(fs::path(cut.run_dir) / "events.bin", 100);  // far shorter than the checkpoint claims
  EXPECT_EQ(run(again, "x", &err, nullptr, cut.run_dir).exit_code, 3);
  EXPECT_NE(err.find("shorter than the checkpoint"), std::string::npos) << err;
}

TEST(Resume, CommandLineResumeUsesTheSavedSettingsAndAcceptsOverrides) {
  TempDir tmp;
  std::ostringstream out, err;
  ASSERT_EQ(run_command({"--max-programs", "300", "--seed", "7", "--workers", "2", "--results-dir", tmp.str(), "--run-id", "cli", "--no-progress"}, out, err), 0);
  const std::string dir = tmp.str() + "/cli";
  std::ostringstream out2, err2;
  ASSERT_EQ(run_command({"--resume", dir, "--max-programs", "450", "--no-progress"}, out2, err2), 0) << err2.str();
  EXPECT_EQ(slurp(fs::path(dir) / "events.bin").size(), sizeof(EventsHeader) + 450 * sizeof(EventRecord));
  EXPECT_NE(slurp(fs::path(dir) / "config.toml").find("seed = 7"), std::string::npos) << "the saved seed was kept";
  EXPECT_NE(out2.str().find("programs run:   450 (seed 7"), std::string::npos) << out2.str();

  std::ostringstream e3;
  EXPECT_EQ(run_command({"--resume", dir, "--seed", "8"}, out2, e3), 3);
  EXPECT_NE(e3.str().find("differ from the saved run"), std::string::npos);
  std::ostringstream e4;
  EXPECT_EQ(run_command({"--resume", dir, "--config", "cutoff.toml"}, out2, e4), 3);
  EXPECT_NE(e4.str().find("do not combine"), std::string::npos);
  std::ostringstream e5;
  EXPECT_EQ(run_command({"--resume", tmp.str() + "/nowhere"}, out2, e5), 3);
  EXPECT_NE(e5.str().find("not a Cutoff run folder"), std::string::npos);
}

// ---------------------------------------------------------------- a real Ctrl-C

TEST(Resume, ARealSigintInterruptsCleanlyAndTheRunCanBeFinished) {
  TempDir tmp;
  const std::string& args_dir = tmp.str();
  std::fflush(nullptr);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::ostringstream out, err;
    const int code = run_command({"--max-programs", "50000000", "--workers", "2", "--checkpoint", "1000", "--results-dir", args_dir, "--run-id", "sig", "--no-progress"}, out, err);
    _exit(code);
  }
  // Wait until the run has demonstrably started (its first checkpoint exists), then press Ctrl-C.
  const fs::path cp = fs::path(args_dir) / "sig" / "checkpoint.bin";
  for (int i = 0; i < 300 && !fs::exists(cp); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_TRUE(fs::exists(cp)) << "the child never wrote a checkpoint";
  ASSERT_EQ(kill(child, SIGINT), 0);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 130);

  Checkpoint saved;
  std::string err;
  ASSERT_TRUE(read_checkpoint(cp.string(), saved, err)) << err;
  EXPECT_GT(saved.next_program_id, 0u);
  EXPECT_EQ(slurp(fs::path(args_dir) / "sig" / "events.bin").size(), saved.events_bytes) << "the stream ends exactly at the checkpoint";

  // Finish it with a small budget just above the interruption point and compare with a straight run of that length.
  const std::uint64_t total = saved.next_program_id + 500;
  std::ostringstream out, err2;
  ASSERT_EQ(run_command({"--resume", args_dir + "/sig", "--max-programs", std::to_string(total), "--no-progress"}, out, err2), 0) << err2.str();
  CampaignConfig fresh = base_config(args_dir, total);
  fresh.seed = 1;
  fresh.stop.checkpoint = 1000;
  fresh.stop.min_programs = 1000;
  const RunOutcome ref = run(fresh, "ref");
  EXPECT_EQ(slurp(fs::path(args_dir) / "sig" / "events.bin"), slurp(fs::path(ref.run_dir) / "events.bin"));
}
