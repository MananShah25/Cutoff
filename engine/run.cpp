#include "run.hpp"

#include <csignal>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <ostream>

#include "campaign.hpp"
#include "checkpoint.hpp"
#include "drill_report.hpp"
#include "drill_select.hpp"
#include "drill_tally.hpp"
#include "results.hpp"
#include "risk_monitor.hpp"
#include "system_info.hpp"

namespace fs = std::filesystem;

namespace cutoff::engine {

namespace {

volatile std::sig_atomic_t g_interrupt = 0;
extern "C" void on_interrupt_signal(int) { g_interrupt = 1; }

enum class SinkStop : std::uint8_t { None, Threshold, TimeBudget, DebugInterrupt, Error };

// Ties together everything that happens for each delivered program, in program-id order.
class RunSink : public RecordSink {
 public:
  RunSink(const CampaignConfig& cfg, ResultsWriter& writer, RiskMonitor& monitor, DrillTally* tally, std::uint64_t stream, double elapsed_before, std::uint32_t sessions)
      : cfg_(cfg), writer_(writer), monitor_(monitor), tally_(tally), stream_(stream), elapsed_before_(elapsed_before), sessions_(sessions),
        t0_(std::chrono::steady_clock::now()) {}

  // Drill: the stop rule is only observed (its first firing is remembered), the campaign runs on to the budget so that the
  // planted bugs can also be counted after the stop point.
  std::uint64_t stop_at() const { return stop_at_; }
  void restore_stop_at(std::uint64_t n) { stop_at_ = n; }

  bool on_record(const ProgramResult& r) override {
    writer_.append(r.rec);
    if (tally_ != nullptr) tally_->observe(r.rec.program_id, r.drill);
    monitor_.add_program(r.species);
    const std::uint64_t n = monitor_.programs();
    if (monitor_.at_checkpoint()) {
      monitor_.evaluate();
      if (!save_checkpoint()) return stop(SinkStop::Error);
      if (monitor_.should_stop()) {
        if (tally_ == nullptr) return stop(SinkStop::Threshold);
        if (stop_at_ == 0) stop_at_ = n;
      }
      if (cfg_.max_hours > 0 && elapsed() > cfg_.max_hours * 3600.0) return stop(SinkStop::TimeBudget);
    }
    if (cfg_.interrupt_after != 0 && n == cfg_.interrupt_after) return stop(SinkStop::DebugInterrupt);
    return true;
  }

  double elapsed() const { return elapsed_before_ + std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(); }
  SinkStop stopped_for() const { return stop_; }
  const std::string& error() const { return error_; }

  // Persists the state after the programs delivered so far.
  bool save_checkpoint() {
    if (!writer_.flush(error_)) return false;
    Checkpoint cp;
    cp.stream_hash = stream_;
    cp.next_program_id = monitor_.programs();
    cp.events_bytes = writer_.events_bytes();
    cp.curve_bytes = monitor_.curve_bytes();
    cp.elapsed_seconds = elapsed();
    cp.sessions = sessions_;
    cp.totals = writer_.totals();
    monitor_.export_state(cp);
    if (tally_ != nullptr) {
      cp.has_drill = true;
      cp.drill_stop_at = stop_at_;
      cp.drill_ids = tally_->ids();
      cp.drill_words = tally_->to_words();
    }
    return write_checkpoint((fs::path(writer_.dir()) / "checkpoint.bin").string(), cp, error_);
  }

 private:
  bool stop(SinkStop why) {
    stop_ = why;
    return false;
  }

  const CampaignConfig& cfg_;
  ResultsWriter& writer_;
  RiskMonitor& monitor_;
  DrillTally* tally_;
  std::uint64_t stop_at_ = 0;
  std::uint64_t stream_;
  double elapsed_before_;
  std::uint32_t sessions_;
  std::chrono::steady_clock::time_point t0_;
  SinkStop stop_ = SinkStop::None;
  std::string error_;
};

}  // namespace

std::string default_run_id(const CampaignConfig& cfg) {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof buf, "run-%Y%m%d-%H%M%S", &tm);
  return std::string(buf) + "-seed" + std::to_string(cfg.seed);
}

RunOutcome execute_run(const CampaignConfig& cfg_in, const std::string& run_id_in, bool show_progress, std::ostream& out, std::ostream& err,
                       const std::string& resume_dir) {
  RunOutcome outcome;
  CampaignConfig cfg = cfg_in;  // the drill selection is resolved into this copy
  // A drill program costs about one simulation per mutant, so batches of 64 programs would leave most workers idle.
  if (cfg.drill) cfg.batch_size = std::min<std::uint32_t>(cfg.batch_size, 4);
  if (const std::string bad = validate(cfg); !bad.empty()) {
    err << "cutoff: " << bad << "\n";
    return outcome;
  }
  if (cfg.drill) {
    std::string ops_text, select_error;
    if (!cfg.drill_operators.empty()) {
      std::ifstream f(cfg.drill_operators);
      if (!f) {
        err << "cutoff: cannot read the operators file '" << cfg.drill_operators << "'. Check drill.operators / --operators.\n";
        return outcome;
      }
      std::stringstream ss;
      ss << f.rdbuf();
      ops_text = ss.str();
    }
    cfg.drill_ids = select_mutants(ops_text, cfg.drill_mutants, cfg.drill_sample_seed >= 0 ? static_cast<std::uint64_t>(cfg.drill_sample_seed) : cfg.seed, select_error);
    if (!select_error.empty()) {
      err << "cutoff: " << select_error << "\n";
      return outcome;
    }
  }
  const bool resuming = !resume_dir.empty();
  std::string run_id = run_id_in;
  std::string error;

  Checkpoint cp;
  std::unique_ptr<ResultsWriter> writer;
  if (resuming) {
    outcome.run_dir = resume_dir;
    run_id = fs::path(resume_dir).filename().string();
    if (run_id.empty()) run_id = fs::path(resume_dir).parent_path().filename().string();
    if (!read_checkpoint((fs::path(resume_dir) / "checkpoint.bin").string(), cp, error)) {
      err << "cutoff: " << error << "\n";
      return outcome;
    }
    if (cp.stream_hash != stream_hash(cfg)) {
      err << "cutoff: cannot resume " << resume_dir << ": the settings that decide what each program is (seed, generator, checker limits, "
          << "species sets, core) differ from the saved run. Resuming would mix two different campaigns. Remove the conflicting overrides, "
          << "or start a new run.\n";
      return outcome;
    }
    if (cp.has_drill != cfg.drill || (cfg.drill && cp.drill_ids != cfg.drill_ids)) {
      err << "cutoff: cannot resume " << resume_dir << ": it was started " << (cp.has_drill ? "as a drill" : "without a drill")
          << (cp.has_drill && cfg.drill ? " with a different selection of mutants" : "") << ". Use the same drill settings (drill.mutants, drill.sample_seed, drill.operators).\n";
      return outcome;
    }
    writer = ResultsWriter::resume(cfg, resume_dir, cp, error);
  } else {
    std::error_code ec;
    fs::create_directories(cfg.results_dir, ec);
    if (ec) {
      err << "cutoff: cannot create results directory '" << cfg.results_dir << "': " << ec.message() << ". Set output.dir to a writable path.\n";
      return outcome;
    }
    outcome.run_dir = (fs::path(cfg.results_dir) / run_id).string();
    writer = ResultsWriter::create(cfg, outcome.run_dir, error);
  }
  if (!writer) {
    err << "cutoff: " << error << "\n";
    return outcome;
  }

  RiskMonitor monitor(cfg);
  const std::string curve_path = (fs::path(outcome.run_dir) / "risk_curve.csv").string();
  if (!monitor.open_curve(curve_path, resuming, cp.curve_bytes, error) || (resuming && !monitor.import_state(cp, error))) {
    err << "cutoff: " << error << "\n";
    return outcome;
  }

  const std::uint32_t workers = resolved_workers(cfg);
  const std::uint32_t sessions = resuming ? cp.sessions + 1 : 1;
  const double elapsed_before = resuming ? cp.elapsed_seconds : 0.0;
  const std::string started = utc_timestamp();
  const std::uint64_t first_id = resuming ? cp.next_program_id : 0;
  DrillTally tally;
  if (cfg.drill) {
    tally = DrillTally(cfg.drill_ids);
    if (resuming) {
      bool ok = false;
      tally = DrillTally::from_words(cfg.drill_ids, cp.drill_words, ok);
      if (!ok) {
        err << "cutoff: the checkpoint's drill data does not match the current mutant selection; it cannot be resumed.\n";
        return outcome;
      }
    }
  }
  RunSink sink(cfg, *writer, monitor, cfg.drill ? &tally : nullptr, stream_hash(cfg), elapsed_before, sessions);
  if (resuming && cfg.drill) sink.restore_stop_at(cp.drill_stop_at);

  // A resumed run whose saved state already satisfies the (possibly new) stop rule has nothing left to do.
  const bool already_satisfied = resuming && !cfg.drill && monitor.should_stop();

  g_interrupt = 0;
  struct sigaction handler {}, old_int {}, old_term {};
  handler.sa_handler = on_interrupt_signal;
  sigemptyset(&handler.sa_mask);
  sigaction(SIGINT, &handler, &old_int);
  sigaction(SIGTERM, &handler, &old_term);

  CampaignStatus status;
  status.ok = true;
  status.next_id = first_id;
  if (!already_satisfied) {
    CampaignControl control;
    control.first_id = first_id;
    control.interrupt = &g_interrupt;
    status = run_campaign(cfg, sink, control, [&](std::uint64_t next) {
      if (!show_progress) return;
      const double s = sink.elapsed() - elapsed_before;
      char line[200];
      std::snprintf(line, sizeof line, "\r  %llu / %llu programs, %llu failures, risk %.4g, %.0f programs/s ", static_cast<unsigned long long>(next),
                    static_cast<unsigned long long>(cfg.programs), static_cast<unsigned long long>(writer->totals().failures),
                    monitor.tracking() ? monitor.current_risk() : 0.0, s > 0 ? static_cast<double>(next - first_id) / s : 0.0);
      err << line << std::flush;
    });
  }
  sigaction(SIGINT, &old_int, nullptr);
  sigaction(SIGTERM, &old_term, nullptr);
  if (show_progress) err << "\n";

  outcome.wall_seconds = sink.elapsed();
  outcome.programs = writer->totals().programs;
  outcome.failures = writer->totals().failures;

  // Persist the state after the last delivered program, whatever the reason the run ended.
  std::string save_error = sink.error();
  const bool saved = save_error.empty() && sink.save_checkpoint();
  if (!saved && save_error.empty()) save_error = sink.error();

  const bool interrupted = status.interrupted || sink.stopped_for() == SinkStop::DebugInterrupt;
  if (interrupted && saved) {
    outcome.exit_code = 130;
    outcome.stop_reason = "interrupted";
    err << "cutoff: interrupted after " << outcome.programs << " programs. Continue with: cutoff run --resume " << outcome.run_dir << "\n";
    return outcome;
  }

  auto make_summary = [&] {
    SummaryInput in;
    in.run_id = run_id;
    in.started_utc = started;
    in.wall_seconds = outcome.wall_seconds;
    in.workers = workers;
    in.sessions = sessions;
    in.exit_code = outcome.exit_code;
    in.stop_reason = outcome.stop_reason;
    in.programs_at_stop = outcome.programs;
    in.sets = monitor.summary();
    in.risk_used = monitor.tracking() ? monitor.current_risk() : 0.0;
    in.plateau_stop_at = monitor.plateau_stop_at();
    in.stop_consecutive = monitor.rule().consecutive();
    if (cfg.drill) in.drill_json = drill_json(make_drill_report(tally, all_mutants(), sink.stop_at()));
    return in;
  };

  if (!status.ok || !saved) {
    outcome.exit_code = 3;
    outcome.stop_reason = "error";
    std::string ignored;
    writer->write_summary(make_summary(), ignored);  // best effort: CI looks at the summary even after a failure
    if (!status.ok) err << "cutoff: campaign stopped after " << status.next_id << " of " << cfg.programs << " programs: " << status.error << "\n";
    if (!saved) err << "cutoff: " << save_error << "\n";
    else err << "cutoff: the programs done so far are saved; fix the problem and continue with: cutoff run --resume " << outcome.run_dir << "\n";
    return outcome;
  }

  const bool rule_on = cfg.stop.epsilon > 0;
  if (sink.stopped_for() == SinkStop::Threshold || already_satisfied || (cfg.drill && sink.stop_at() > 0 && sink.stopped_for() == SinkStop::None)) outcome.stop_reason = "threshold";
  else if (sink.stopped_for() == SinkStop::TimeBudget) outcome.stop_reason = "time-budget";
  else outcome.stop_reason = rule_on ? "budget" : "program-count";

  if (outcome.failures > 0) outcome.exit_code = 1;
  else if (outcome.stop_reason == "budget" || outcome.stop_reason == "time-budget") outcome.exit_code = 2;
  else outcome.exit_code = 0;

  const SummaryInput in = make_summary();
  std::string summary_error;
  if (cfg.drill && !writer->write_aux("drill.tsv", drill_tsv(tally, all_mutants(), sink.stop_at()), summary_error)) {
    err << "cutoff: " << summary_error << "\n";
    outcome.exit_code = 3;
    return outcome;
  }
  if (!writer->write_summary(in, summary_error)) {
    err << "cutoff: " << summary_error << "\n";
    outcome.exit_code = 3;
    return outcome;
  }

  char rate[64];
  const double session_seconds = outcome.wall_seconds - elapsed_before;
  std::snprintf(rate, sizeof rate, "%.0f", session_seconds > 0 ? static_cast<double>(status.next_id - first_id) / session_seconds : 0.0);
  const Totals& t = writer->totals();
  const char* profile = cfg.gen.profile == gen::Profile::G0 ? "g0" : cfg.gen.profile == gen::Profile::G1 ? "g1" : cfg.gen.profile == gen::Profile::G2 ? "g2" : "g3";
  out << "programs run:   " << outcome.programs << " (seed " << cfg.seed << ", generator " << profile << " v" << gen::kGeneratorVersion << ", checker " << cfg.checker << ")\n";
  char risk_text[64];
  std::snprintf(risk_text, sizeof risk_text, "%.4g", in.risk_used);
  if (outcome.stop_reason == "threshold")
    out << "stopped:        risk threshold reached: " << risk::estimator_name(cfg.stop.estimator) << " risk " << risk_text << " has been below epsilon "
        << cfg.stop.epsilon << " for " << cfg.stop.confirm << " consecutive checkpoints (" << cfg.stop.checkpoint << " programs apart)\n";
  else if (outcome.stop_reason == "budget")
    out << "stopped:        program budget (" << cfg.programs << ") used up before the risk fell below epsilon " << cfg.stop.epsilon << " (risk now " << risk_text << ")\n";
  else if (outcome.stop_reason == "time-budget")
    out << "stopped:        time budget (" << cfg.max_hours << " h) used up before the risk fell below epsilon " << cfg.stop.epsilon << " (risk now " << risk_text << ")\n";
  else
    out << "stopped:        all " << cfg.programs << " programs ran (stop rule off)\n";
  if (cfg.drill) out << drill_text(make_drill_report(tally, all_mutants(), sink.stop_at()));
  for (const SetSummary& s : in.sets) {
    char line[200];
    std::snprintf(line, sizeof line, "species %-6s %llu of %zu cells seen, %llu singletons, good-turing %.4g\n", (s.name + ":").c_str(),
                  static_cast<unsigned long long>(s.counts.s), s.universe, static_cast<unsigned long long>(s.counts.q1), s.good_turing);
    out << line;
  }
  out << "failures:       " << outcome.failures;
  if (outcome.failures > 0) {
    out << "  (";
    bool first = true;
    for (std::size_t v = 1; v < t.by_verdict.size(); ++v)
      if (t.by_verdict[v] > 0) { out << (first ? "" : ", ") << to_string(static_cast<Verdict>(v)) << " " << t.by_verdict[v]; first = false; }
    out << "; details in " << outcome.run_dir << "/failures)";
  }
  out << "\nworkers:        " << workers << "\nthroughput:     " << rate << " programs/s over " << session_seconds << " s"
      << (resuming ? " (this session)" : "") << "\n"
      << "results folder: " << outcome.run_dir << "\n";
  return outcome;
}

}  // namespace cutoff::engine
