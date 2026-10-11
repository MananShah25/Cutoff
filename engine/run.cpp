#include "run.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <ostream>

#include "campaign.hpp"
#include "results.hpp"
#include "system_info.hpp"

namespace fs = std::filesystem;

namespace cutoff::engine {

std::string default_run_id(const CampaignConfig& cfg) {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof buf, "run-%Y%m%d-%H%M%S", &tm);
  return std::string(buf) + "-seed" + std::to_string(cfg.seed);
}

RunOutcome execute_run(const CampaignConfig& cfg, const std::string& run_id, bool show_progress, std::ostream& out, std::ostream& err) {
  RunOutcome outcome;
  if (const std::string bad = validate(cfg); !bad.empty()) {
    err << "cutoff: " << bad << "\n";
    return outcome;
  }
  std::error_code ec;
  fs::create_directories(cfg.results_dir, ec);
  if (ec) {
    err << "cutoff: cannot create results directory '" << cfg.results_dir << "': " << ec.message() << ". Set output.dir to a writable path.\n";
    return outcome;
  }
  outcome.run_dir = (fs::path(cfg.results_dir) / run_id).string();
  std::string error;
  auto writer = ResultsWriter::create(cfg, outcome.run_dir, error);
  if (!writer) {
    err << "cutoff: " << error << "\n";
    return outcome;
  }

  const std::uint32_t workers = resolved_workers(cfg);
  const auto t0 = std::chrono::steady_clock::now();
  const std::string started = utc_timestamp();
  auto seconds_since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

  const CampaignStatus status = run_campaign(cfg, *writer, [&](std::uint64_t done) {
    if (!show_progress) return;
    const double s = seconds_since();
    char line[160];
    std::snprintf(line, sizeof line, "\r  %llu / %llu programs, %llu failures, %.0f programs/s ",
                  static_cast<unsigned long long>(done), static_cast<unsigned long long>(cfg.programs),
                  static_cast<unsigned long long>(writer->totals().failures), s > 0 ? static_cast<double>(done) / s : 0.0);
    err << line << std::flush;
  });
  if (show_progress) err << "\n";

  outcome.wall_seconds = seconds_since();
  outcome.programs = writer->totals().programs;
  outcome.failures = writer->totals().failures;
  if (status.ok) outcome.exit_code = outcome.failures > 0 ? 1 : 0;
  else outcome.exit_code = 3;

  std::string finish_error;
  if (!writer->finish(run_id, {started, outcome.wall_seconds}, workers, outcome.exit_code,
                      status.ok ? "program-count" : "error", finish_error)) {
    err << "cutoff: " << finish_error << "\n";
    outcome.exit_code = 3;
  }
  if (!status.ok) {
    err << "cutoff: campaign stopped after " << status.programs_done << " of " << cfg.programs << " programs: " << status.error << "\n";
    return outcome;
  }

  char rate[64];
  std::snprintf(rate, sizeof rate, "%.0f", outcome.wall_seconds > 0 ? static_cast<double>(outcome.programs) / outcome.wall_seconds : 0.0);
  const Totals& t = writer->totals();
  out << "programs run:   " << outcome.programs << " (seed " << cfg.seed << ", generator g0 v" << gen::kGeneratorVersion << ", checker " << cfg.checker << ")\n"
      << "failures:       " << outcome.failures;
  if (outcome.failures > 0) {
    out << "  (";
    bool first = true;
    for (std::size_t v = 1; v < t.by_verdict.size(); ++v)
      if (t.by_verdict[v] > 0) { out << (first ? "" : ", ") << to_string(static_cast<Verdict>(v)) << " " << t.by_verdict[v]; first = false; }
    out << "; details in " << outcome.run_dir << "/failures)";
  }
  out << "\nworkers:        " << workers << "\nthroughput:     " << rate << " programs/s over " << outcome.wall_seconds << " s\n"
      << "results folder: " << outcome.run_dir << "\n";
  return outcome;
}

}  // namespace cutoff::engine
