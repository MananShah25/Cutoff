// Tests for the TOML subset, campaign settings, checker C1, the orchestrator (ordering, worker counts, worker
// death), the results files and `cutoff run`.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "campaign.hpp"
#include "campaign_config.hpp"
#include "check.hpp"
#include "protocol.hpp"
#include "results.hpp"
#include "run.hpp"
#include "run_command.hpp"
#include "toml_lite.hpp"

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

struct Collect : RecordSink {
  std::vector<EventRecord> records;
  bool on_record(const ProgramResult& r) override { records.push_back(r.rec); return true; }
};

CampaignConfig small_config(std::uint64_t programs = 200) {
  CampaignConfig c;
  c.programs = programs;
  c.workers = 2;
  return c;
}

bool same_bytes(const std::vector<EventRecord>& a, const std::vector<EventRecord>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(EventRecord)) == 0;
}

int count_substr(const std::string& s, const std::string& sub) {
  int n = 0;
  for (std::size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + 1)) ++n;
  return n;
}

}  // namespace

// ---------------------------------------------------------------- TOML subset

TEST(TomlLite, ParsesSectionsTypesCommentsAndEscapes) {
  const auto r = parse_toml(
      "top = 1\n# a comment\n\n[a]\nint = 1_000   # trailing\nneg = -7\nflt = 2.5e-1\nyes = true\nno=false\n"
      "str = \"he said \\\"hi\\\"\\n\"  # c\n[b-c]\nname = \"x#y\"\n",
      "t.toml");
  ASSERT_EQ(r.error, "");
  EXPECT_EQ(r.doc.at("top").i, 1);
  EXPECT_EQ(r.doc.at("a.int").i, 1000);
  EXPECT_EQ(r.doc.at("a.neg").i, -7);
  EXPECT_DOUBLE_EQ(r.doc.at("a.flt").d, 0.25);
  EXPECT_TRUE(r.doc.at("a.yes").b);
  EXPECT_FALSE(r.doc.at("a.no").b);
  EXPECT_EQ(r.doc.at("a.str").s, "he said \"hi\"\n");
  EXPECT_EQ(r.doc.at("b-c.name").s, "x#y") << "a # inside a string is not a comment";
  EXPECT_EQ(r.doc.at("a.int").line, 5);
}

TEST(TomlLite, ErrorsNameTheFileAndLineAndSayWhatIsWrong) {
  struct C { const char* text; const char* line; const char* expect; };
  const C cases[] = {
      {"a = 1\nb 2\n", "t:2:", "key = value"},
      {"s = \"abc\n", "t:1:", "unterminated"},
      {"a = 1\na = 2\n", "t:2:", "duplicate key 'a'"},
      {"[x\n", "t:1:", "']'"},
      {"[x y]\n", "t:1:", "invalid section"},
      {"v = abc\n", "t:1:", "double quotes"},
      {"v = \"a\\q\"\n", "t:1:", "escape"},
      {"v = \"a\" junk\n", "t:1:", "after the string"},
      {"v =\n", "t:1:", "missing value"},
      {"bad key = 1\n", "t:1:", "invalid key"},
  };
  for (const auto& c : cases) {
    const auto r = parse_toml(c.text, "t");
    EXPECT_NE(r.error.find(c.line), std::string::npos) << c.text << " -> " << r.error;
    EXPECT_NE(r.error.find(c.expect), std::string::npos) << c.text << " -> " << r.error;
    EXPECT_TRUE(r.doc.empty());
  }
}

// ---------------------------------------------------------------- campaign settings

TEST(CampaignConfigTest, DefaultsAreValidAndRoundTripThroughToml) {
  CampaignConfig c;
  EXPECT_EQ(validate(c), "");
  c.seed = 99;
  c.programs = 12345;
  c.gen.max_len = 77;
  c.max_failure_dirs = 3;
  c.crash_worker_at = 17;
  const std::string text = to_toml(c);
  const auto parsed = parse_toml(text, "rt");
  ASSERT_EQ(parsed.error, "");
  CampaignConfig d;
  ASSERT_EQ(apply_toml(d, parsed.doc, "rt"), "");
  EXPECT_EQ(to_toml(d), text);
  EXPECT_EQ(d.seed, 99u);
  EXPECT_EQ(d.gen.max_len, 77u);
  EXPECT_EQ(d.crash_worker_at, 17);
}

TEST(CampaignConfigTest, BadSettingsAreRejectedWithHelpfulMessages) {
  auto apply = [](const std::string& text) {
    CampaignConfig c;
    const auto p = parse_toml(text, "f");
    return p.error.empty() ? apply_toml(c, p.doc, "f") : p.error;
  };
  EXPECT_NE(apply("[campaign]\nprogramz = 5\n").find("Valid settings"), std::string::npos);
  EXPECT_NE(apply("[campaign]\nprogramz = 5\n").find("campaign.programs"), std::string::npos);
  EXPECT_NE(apply("[campaign]\nprograms = \"many\"\n").find("must be an integer"), std::string::npos);
  EXPECT_NE(apply("[campaign]\nprograms = -4\n").find("between"), std::string::npos);
  EXPECT_NE(apply("[generator]\nprofile = 3\n").find("quoted string"), std::string::npos);
  EXPECT_NE(apply("[generator]\nprofile = \"g9\"\n").find("one of g0"), std::string::npos);
  EXPECT_EQ(apply("[campaign]\nprograms = 5\n"), "") << "a valid file produces no error";
}

TEST(CampaignConfigTest, ValidationSaysWhatToChange) {
  auto msg = [](auto edit) {
    CampaignConfig c;
    edit(c);
    return validate(c);
  };
  EXPECT_NE(msg([](CampaignConfig& c) { c.programs = 0; }).find("programs"), std::string::npos);
  EXPECT_NE(msg([](CampaignConfig& c) { c.checker = "c2"; }).find("Step 8"), std::string::npos);
  EXPECT_NE(msg([](CampaignConfig& c) { c.max_cycles = 10; }).find("max_cycles"), std::string::npos);
  EXPECT_NE(msg([](CampaignConfig& c) { c.gen.max_len = 1; c.gen.min_len = 5; }).find("max_len"), std::string::npos);
  EXPECT_NE(msg([](CampaignConfig& c) { c.core_name = "ibex"; }).find("picorv32"), std::string::npos);
  EXPECT_NE(msg([](CampaignConfig& c) { c.gen.profile = gen::Profile::G2; }).find("G0"), std::string::npos);
}

TEST(CampaignConfigTest, ConfigHashCoversOnlyResultDeterminingSettings) {
  CampaignConfig a, b;
  b.workers = 7;
  b.batch_size = 3;
  b.results_dir = "elsewhere";
  b.max_failure_dirs = 99;
  EXPECT_EQ(config_hash(a), config_hash(b));
  b = a;
  b.seed = 2;
  EXPECT_NE(config_hash(a), config_hash(b));
  b = a;
  b.gen.max_len = 129;
  EXPECT_NE(config_hash(a), config_hash(b));
  b = a;
  b.max_cycles = 300000;
  EXPECT_NE(config_hash(a), config_hash(b));
  EXPECT_GE(resolved_workers(a), 1u);
  a.workers = 5;
  EXPECT_EQ(resolved_workers(a), 5u);
}

// ---------------------------------------------------------------- checker C1

namespace {
ModelResult good_model() {
  ModelResult m;
  m.status = Status::Exited;
  m.exit_code = 7;
  for (unsigned i = 0; i < 32; ++i) m.regs[i] = 100 + i;
  m.regs[0] = 0;
  m.mem_hash = 1234;
  return m;
}
RunResult matching_core(const ModelResult& m) {
  RunResult r;
  r.status = Status::Exited;
  r.exit_code = m.exit_code;
  r.regs = m.regs;
  r.mem_hash = m.mem_hash;
  return r;
}
}  // namespace

TEST(CheckC1, VerdictsAndPrecedence) {
  ModelResult m = good_model();
  RunResult c = matching_core(m);
  EXPECT_EQ(check_c1(m, c).verdict, Verdict::Pass);

  c.mem_hash ^= 1;
  EXPECT_EQ(check_c1(m, c).verdict, Verdict::MemMismatch);
  c.regs[30] ^= 1;
  c.regs[9] ^= 1;
  CheckOutcome o = check_c1(m, c);
  EXPECT_EQ(o.verdict, Verdict::RegMismatch);
  EXPECT_EQ(o.detail, 9) << "the first (lowest) differing register is reported";
  c.exit_code ^= 1;
  EXPECT_EQ(check_c1(m, c).verdict, Verdict::ExitCodeMismatch);
  c.status = Status::Trap;
  EXPECT_EQ(check_c1(m, c).verdict, Verdict::StatusMismatch);
  c.status = Status::MemFault;
  EXPECT_EQ(check_c1(m, c).verdict, Verdict::StatusMismatch);
  c.status = Status::Timeout;
  EXPECT_EQ(check_c1(m, c).verdict, Verdict::Hang) << "a hang is its own failure class, not a mismatch";

  m.status = Status::Trap;  // the model failing means nothing can be said about the core
  EXPECT_EQ(check_c1(m, c).verdict, Verdict::GeneratorFault);
  m.status = Status::Timeout;
  c = matching_core(good_model());
  EXPECT_EQ(check_c1(m, c).verdict, Verdict::GeneratorFault);
}

TEST(CheckC1, X0IsNeverCompared) {
  ModelResult m = good_model();
  RunResult c = matching_core(m);
  c.regs[0] = 0xdead;  // the worker never reads x0 from the core, and C1 ignores it
  EXPECT_EQ(check_c1(m, c).verdict, Verdict::Pass);
}

TEST(Evaluate, RecordsAreConsistentAndDeterministic) {
  const CampaignConfig cfg = small_config();
  for (std::uint64_t id = 0; id < 150; ++id) {
    const EventRecord a = evaluate_program(cfg, id);
    const EventRecord b = evaluate_program(cfg, id);
    EXPECT_EQ(std::memcmp(&a, &b, sizeof a), 0) << id;
    EXPECT_EQ(a.program_id, id);
    EXPECT_EQ(a.seed, gen::program_seed(cfg.seed, id));
    EXPECT_EQ(a.program_hash, gen::program_hash(gen::generate(cfg.gen, a.seed)));
    EXPECT_EQ(a.verdict, static_cast<std::uint8_t>(Verdict::Pass)) << id;
    EXPECT_EQ(a.model_status, static_cast<std::uint8_t>(Status::Exited));
    EXPECT_EQ(a.core_status, static_cast<std::uint8_t>(Status::Exited));
    EXPECT_EQ(a.model_mem_hash, a.core_mem_hash);
    EXPECT_EQ(a.model_exit_code, a.core_exit_code);
    EXPECT_GT(a.core_cycles, 0u);
    EXPECT_GT(a.model_instret, 60u) << "at least the init block retires";
  }
}

TEST(Evaluate, SeedChangesTheProgramsAndCampaignsDoNotOverlap) {
  CampaignConfig a = small_config(), b = small_config();
  b.seed = 2;
  EXPECT_NE(evaluate_program(a, 0).program_hash, evaluate_program(b, 0).program_hash);
  EXPECT_NE(evaluate_program(a, 0).program_hash, evaluate_program(a, 1).program_hash);
}

TEST(Evaluate, TestHooksProduceTheExpectedFailures) {
  CampaignConfig cfg = small_config();
  cfg.corrupt_core_every = 5;
  for (std::uint64_t id = 0; id < 20; ++id) {
    const EventRecord r = evaluate_program(cfg, id);
    if ((id + 1) % 5 == 0) {
      EXPECT_EQ(r.verdict, static_cast<std::uint8_t>(Verdict::RegMismatch)) << id;
      EXPECT_EQ(r.detail, 5);
    } else {
      EXPECT_EQ(r.verdict, static_cast<std::uint8_t>(Verdict::Pass)) << id;
    }
  }
  CampaignConfig slow = small_config();
  slow.max_cycles = 100;  // far fewer cycles than the init block alone needs
  for (std::uint64_t id = 0; id < 10; ++id) {
    const EventRecord r = evaluate_program(slow, id);
    EXPECT_EQ(r.verdict, static_cast<std::uint8_t>(Verdict::Hang)) << id;
    EXPECT_EQ(r.core_status, static_cast<std::uint8_t>(Status::Timeout));
    EXPECT_EQ(r.model_status, static_cast<std::uint8_t>(Status::Exited));
  }
}

// ---------------------------------------------------------------- orchestrator

TEST(OrderedDeliveryTest, ReleasesRecordsInIdOrderWhateverTheArrivalOrder) {
  std::mt19937 rng(5);  // NOLINT(bugprone-random-generator-seed): fixed seed keeps the test deterministic
  for (const std::size_t n : {1u, 2u, 17u, 1000u}) {
    std::vector<std::uint64_t> arrival(n);
    std::iota(arrival.begin(), arrival.end(), 0);
    std::shuffle(arrival.begin(), arrival.end(), rng);
    Collect sink;
    OrderedDelivery d(sink, 0);
    std::size_t max_waiting = 0;
    for (const auto id : arrival) {
      ProgramResult r;
      r.rec.program_id = id;
      ASSERT_TRUE(d.push(r));
      max_waiting = std::max(max_waiting, d.waiting());
      // Whatever has been delivered so far is exactly the prefix 0..k-1.
      for (std::size_t i = 0; i < sink.records.size(); ++i) ASSERT_EQ(sink.records[i].program_id, i);
    }
    EXPECT_EQ(sink.records.size(), n);
    EXPECT_EQ(d.next_id(), n);
    EXPECT_EQ(d.waiting(), 0u);
    if (n >= 17) EXPECT_GT(max_waiting, 1u) << "the shuffle really did deliver records out of order";
  }
}

TEST(OrderedDeliveryTest, HoldsBackRecordsUntilTheGapIsFilledAndRejectsBadIds) {
  Collect sink;
  OrderedDelivery d(sink, 0);
  ProgramResult r;
  r.rec.program_id = 2;
  EXPECT_TRUE(d.push(r));
  r.rec.program_id = 1;
  EXPECT_TRUE(d.push(r));
  EXPECT_EQ(sink.records.size(), 0u) << "id 0 is missing, so nothing may be released";
  EXPECT_EQ(d.waiting(), 2u);
  EXPECT_FALSE(d.push(r)) << "duplicate of a waiting id";
  r.rec.program_id = 0;
  EXPECT_TRUE(d.push(r));
  EXPECT_EQ(sink.records.size(), 3u);
  EXPECT_FALSE(d.push(r)) << "id already delivered";
  EXPECT_EQ(sink.records.size(), 3u);
}

TEST(Campaign, DeliversEveryProgramInIdOrder) {
  CampaignConfig cfg = small_config(500);
  cfg.workers = 4;
  cfg.batch_size = 8;
  Collect sink;
  const CampaignStatus st = run_campaign(cfg, sink);
  ASSERT_TRUE(st.ok) << st.error;
  ASSERT_EQ(sink.records.size(), 500u);
  for (std::uint64_t i = 0; i < 500; ++i) ASSERT_EQ(sink.records[i].program_id, i);
  EXPECT_EQ(st.next_id, 500u);
}

TEST(Campaign, ResultsAreIdenticalForAnyWorkerCountAndBatchSize) {
  CampaignConfig base = small_config(700);
  Collect reference;
  base.workers = 1;
  base.batch_size = 64;
  ASSERT_TRUE(run_campaign(base, reference).ok);
  struct V { std::uint32_t workers, batch; };
  for (const V v : {V{2, 64}, V{5, 7}, V{8, 1}, V{3, 1000}, V{8, 64}, V{1, 1}}) {
    CampaignConfig cfg = base;
    cfg.workers = v.workers;
    cfg.batch_size = v.batch;
    Collect got;
    const CampaignStatus st = run_campaign(cfg, got);
    ASSERT_TRUE(st.ok) << st.error;
    EXPECT_TRUE(same_bytes(reference.records, got.records)) << "workers=" << v.workers << " batch=" << v.batch;
  }
}

TEST(Campaign, MoreWorkersThanWorkStillWorks) {
  CampaignConfig cfg = small_config(3);
  cfg.workers = 8;
  cfg.batch_size = 64;
  Collect sink;
  ASSERT_TRUE(run_campaign(cfg, sink).ok);
  EXPECT_EQ(sink.records.size(), 3u);
}

TEST(Campaign, ReportsProgressUpToTheTotal) {
  CampaignConfig cfg = small_config(300);
  Collect sink;
  std::uint64_t last = 0;
  ASSERT_TRUE(run_campaign(cfg, sink, {}, [&](std::uint64_t done) { EXPECT_GE(done, last); last = done; }).ok);
  EXPECT_EQ(last, 300u);
}

TEST(Campaign, ADyingWorkerStopsTheRunWithAClearMessageInsteadOfHanging) {
  CampaignConfig cfg = small_config(400);
  cfg.workers = 3;
  cfg.batch_size = 16;
  cfg.crash_worker_at = 50;
  Collect sink;
  const auto t0 = std::chrono::steady_clock::now();
  const CampaignStatus st = run_campaign(cfg, sink);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_FALSE(st.ok);
  EXPECT_NE(st.error.find("died"), std::string::npos) << st.error;
  EXPECT_NE(st.error.find("exit status 9"), std::string::npos) << st.error;
  EXPECT_LT(st.next_id, 400u);
  EXPECT_LT(secs, 30.0);
  // Records delivered before the failure are still the correct prefix.
  for (std::size_t i = 0; i < sink.records.size(); ++i) EXPECT_EQ(sink.records[i].program_id, i);
}

// ---------------------------------------------------------------- results files and `cutoff run`

TEST(Run, WritesEventsConfigAndSummary) {
  TempDir tmp;
  CampaignConfig cfg = small_config(300);
  cfg.results_dir = tmp.str();
  std::ostringstream out, err;
  const RunOutcome r = execute_run(cfg, "t1", false, out, err);
  ASSERT_EQ(r.exit_code, 0) << err.str();
  EXPECT_EQ(r.programs, 300u);
  EXPECT_EQ(r.failures, 0u);

  const std::string events = slurp(fs::path(r.run_dir) / "events.bin");
  ASSERT_EQ(events.size(), sizeof(EventsHeader) + 300 * sizeof(EventRecord));
  EventsHeader h;
  std::memcpy(&h, events.data(), sizeof h);
  EXPECT_EQ(std::string(h.magic, 8), "CUTOFFEV");
  EXPECT_EQ(h.version, kEventsVersion);
  EXPECT_EQ(h.record_size, sizeof(EventRecord));
  EXPECT_EQ(h.generator_version, gen::kGeneratorVersion);
  EXPECT_EQ(h.campaign_seed, cfg.seed);
  for (std::uint64_t i = 0; i < 300; ++i) {
    EventRecord rec;
    std::memcpy(&rec, events.data() + sizeof h + i * sizeof rec, sizeof rec);
    ASSERT_EQ(rec.program_id, i);
    ASSERT_EQ(rec.seed, gen::program_seed(cfg.seed, i));
  }
  const std::string cfg_text = slurp(fs::path(r.run_dir) / "config.toml");
  EXPECT_EQ(cfg_text, to_toml(cfg));
  const std::string summary = slurp(fs::path(r.run_dir) / "summary.json");
  EXPECT_NE(summary.find("\"programs_run\": 300"), std::string::npos);
  EXPECT_NE(summary.find("\"exit_code\": 0"), std::string::npos);
  EXPECT_NE(summary.find("\"stop_reason\": \"program-count\""), std::string::npos);
  EXPECT_NE(summary.find("\"git_commit\""), std::string::npos);
  EXPECT_NE(out.str().find("failures:       0"), std::string::npos);
}

TEST(Run, EventsAndTotalsDoNotDependOnTheWorkerCount) {
  TempDir tmp;
  std::string events[2], totals[2];
  int k = 0;
  for (const std::uint32_t workers : {1u, 4u}) {
    CampaignConfig cfg = small_config(1500);
    cfg.results_dir = tmp.str();
    cfg.workers = workers;
    std::ostringstream out, err;
    const RunOutcome r = execute_run(cfg, "w" + std::to_string(workers), false, out, err);
    ASSERT_EQ(r.exit_code, 0) << err.str();
    events[k] = slurp(fs::path(r.run_dir) / "events.bin");
    const std::string summary = slurp(fs::path(r.run_dir) / "summary.json");
    for (const char* key : {"\"totals\"", "\"failures\"", "\"config_hash\""}) {
      const std::size_t p = summary.find(key);
      ASSERT_NE(p, std::string::npos);
      totals[k] += summary.substr(p, summary.find('\n', p) - p);
    }
    ++k;
  }
  EXPECT_EQ(events[0], events[1]) << "events.bin must be byte-identical for 1 and 4 workers";
  EXPECT_EQ(totals[0], totals[1]);
}

TEST(Run, FailuresAreCountedWrittenUpAndSetExitCodeOne) {
  TempDir tmp;
  CampaignConfig cfg = small_config(450);
  cfg.results_dir = tmp.str();
  cfg.corrupt_core_every = 100;  // programs 99, 199, 299, 399
  cfg.max_failure_dirs = 3;
  std::ostringstream out, err;
  const RunOutcome r = execute_run(cfg, "f", false, out, err);
  EXPECT_EQ(r.exit_code, 1);
  EXPECT_EQ(r.failures, 4u);
  EXPECT_NE(out.str().find("register-mismatch 4"), std::string::npos) << out.str();

  const fs::path failures = fs::path(r.run_dir) / "failures";
  std::vector<std::string> dirs;
  for (const auto& e : fs::directory_iterator(failures)) dirs.push_back(e.path().filename().string());
  std::sort(dirs.begin(), dirs.end());
  ASSERT_EQ(dirs.size(), 3u) << "only max_failure_dirs folders are written, but all failures are counted";
  EXPECT_EQ(dirs[0], "program-000000099");
  const std::string text = slurp(failures / dirs[0] / "failure.txt");
  EXPECT_NE(text.find("program id:        99"), std::string::npos) << text;
  EXPECT_NE(text.find("register-mismatch (first differing register: x5)"), std::string::npos) << text;
  EXPECT_NE(text.find("x5:"), std::string::npos);
  EXPECT_NE(text.find(gen::program_seed(cfg.seed, 99) == 0 ? "0x0" : "0x"), std::string::npos);
  EXPECT_GT(count_substr(slurp(failures / dirs[0] / "program.hex"), "\n"), 60);
  const std::string summary = slurp(fs::path(r.run_dir) / "summary.json");
  EXPECT_NE(summary.find("\"register-mismatch\": 4"), std::string::npos);
  EXPECT_NE(summary.find("\"failure_dirs_written\": 3"), std::string::npos);
  EXPECT_NE(summary.find("\"exit_code\": 1"), std::string::npos);
}

TEST(Run, AHangIsReportedAsAHangFailure) {
  TempDir tmp;
  CampaignConfig cfg = small_config(40);
  cfg.results_dir = tmp.str();
  cfg.max_cycles = 100;
  std::ostringstream out, err;
  const RunOutcome r = execute_run(cfg, "h", false, out, err);
  EXPECT_EQ(r.exit_code, 1);
  EXPECT_EQ(r.failures, 40u);
  EXPECT_NE(slurp(fs::path(r.run_dir) / "summary.json").find("\"hang\": 40"), std::string::npos);
}

TEST(Run, SetupProblemsGiveExitCodeThreeAndNeverOverwrite) {
  TempDir tmp;
  CampaignConfig cfg = small_config(10);
  cfg.results_dir = tmp.str();
  std::ostringstream out, err;
  ASSERT_EQ(execute_run(cfg, "same", false, out, err).exit_code, 0);
  std::ostringstream err2;
  EXPECT_EQ(execute_run(cfg, "same", false, out, err2).exit_code, 3);
  EXPECT_NE(err2.str().find("already exists"), std::string::npos);
  EXPECT_NE(err2.str().find("--run-id"), std::string::npos);

  CampaignConfig bad = cfg;
  bad.programs = 0;
  std::ostringstream err3;
  EXPECT_EQ(execute_run(bad, "bad", false, out, err3).exit_code, 3);
  EXPECT_FALSE(fs::exists(fs::path(tmp.str()) / "bad")) << "no folder for an invalid config";

  CampaignConfig crash = cfg;
  crash.programs = 100;
  crash.crash_worker_at = 10;
  std::ostringstream err4;
  const RunOutcome r = execute_run(crash, "crash", false, out, err4);
  EXPECT_EQ(r.exit_code, 3);
  EXPECT_NE(err4.str().find("died"), std::string::npos);
  EXPECT_NE(slurp(fs::path(r.run_dir) / "summary.json").find("\"stop_reason\": \"error\""), std::string::npos);
}

TEST(RunCommand, ParsesOptionsAndReportsMistakesClearly) {
  TempDir tmp;
  auto run = [&](const std::vector<std::string>& args, std::string* out_text = nullptr, std::string* err_text = nullptr) {
    std::ostringstream out, err;
    const int code = run_command(args, out, err);
    if (out_text) *out_text = out.str();
    if (err_text) *err_text = err.str();
    return code;
  };
  std::string out, err;
  EXPECT_EQ(run({"--help"}, &out), 0);
  EXPECT_NE(out.find("usage: cutoff run"), std::string::npos);

  EXPECT_EQ(run({"--bogus"}, &out, &err), 3);
  EXPECT_NE(err.find("unknown option '--bogus'"), std::string::npos);
  EXPECT_EQ(run({"--programs"}, &out, &err), 3);
  EXPECT_NE(err.find("needs a value"), std::string::npos);
  EXPECT_EQ(run({"--programs", "lots"}, &out, &err), 3);
  EXPECT_NE(err.find("whole number"), std::string::npos);
  EXPECT_EQ(run({"--checker", "c2", "--results-dir", tmp.str()}, &out, &err), 3);
  EXPECT_NE(err.find("Step 8"), std::string::npos);
  EXPECT_EQ(run({"--config", tmp.str() + "/missing.toml"}, &out, &err), 3);
  EXPECT_NE(err.find("cannot read config file"), std::string::npos);
  EXPECT_EQ(run({"--profile", "g7"}, &out, &err), 3);
  EXPECT_NE(err.find("command line"), std::string::npos);

  EXPECT_EQ(run({"--programs", "25", "--workers", "2", "--seed", "5", "--results-dir", tmp.str(), "--run-id", "a", "--no-progress"}, &out, &err), 0) << err;
  EXPECT_NE(out.find("programs run:   25 (seed 5"), std::string::npos) << out;
  EXPECT_EQ(slurp(fs::path(tmp.str()) / "a" / "events.bin").size(), sizeof(EventsHeader) + 25 * sizeof(EventRecord));
}

TEST(RunCommand, ConfigFileIsReadAndCommandLineOverridesIt) {
  TempDir tmp;
  const std::string cfg_path = tmp.str() + "/c.toml";
  std::ofstream(cfg_path) << "[campaign]\nprograms = 30\nseed = 9\nworkers = 1\n[output]\ndir = \"" << tmp.str() << "\"\n";
  std::ostringstream out, err;
  ASSERT_EQ(run_command({"--config", cfg_path, "--run-id", "x", "--no-progress"}, out, err), 0) << err.str();
  EXPECT_EQ(slurp(fs::path(tmp.str()) / "x" / "events.bin").size(), sizeof(EventsHeader) + 30 * sizeof(EventRecord));
  ASSERT_EQ(run_command({"--config", cfg_path, "--programs", "12", "--run-id", "y", "--no-progress"}, out, err), 0) << err.str();
  EXPECT_EQ(slurp(fs::path(tmp.str()) / "y" / "events.bin").size(), sizeof(EventsHeader) + 12 * sizeof(EventRecord));
  EXPECT_NE(slurp(fs::path(tmp.str()) / "y" / "config.toml").find("seed = 9"), std::string::npos) << "file settings survive an override of another key";

  std::ofstream(cfg_path) << "[campaign]\nprogramz = 3\n";
  std::ostringstream err2;
  EXPECT_EQ(run_command({"--config", cfg_path}, out, err2), 3);
  EXPECT_NE(err2.str().find(cfg_path + ":2: unknown setting 'campaign.programz'"), std::string::npos) << err2.str();
}
