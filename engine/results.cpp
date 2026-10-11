#include "results.hpp"

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "check.hpp"
#include "cutoff_version.hpp"
#include "system_info.hpp"

namespace fs = std::filesystem;

namespace cutoff::engine {

namespace {

std::string hex64(std::uint64_t v) {
  char buf[19];
  std::snprintf(buf, sizeof buf, "0x%016" PRIx64, v);
  return buf;
}

std::string hex32(std::uint32_t v) {
  char buf[11];
  std::snprintf(buf, sizeof buf, "0x%08" PRIx32, v);
  return buf;
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) out += '?';
        else out += c;
    }
  }
  return out;
}

bool write_text(const fs::path& path, const std::string& text, std::string& error) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  f.flush();
  if (!f) {
    error = "cannot write " + path.string() + ". Check free disk space and permissions.";
    return false;
  }
  return true;
}

}  // namespace

std::uint64_t config_hash(const CampaignConfig& cfg) {
  CampaignConfig canonical = cfg;  // only settings that can change results take part
  canonical.workers = 0;
  canonical.batch_size = 64;
  canonical.results_dir.clear();
  canonical.max_failure_dirs = 0;
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const char c : to_toml(canonical)) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ULL;
  }
  return h;
}

std::unique_ptr<ResultsWriter> ResultsWriter::create(const CampaignConfig& cfg, const std::string& run_dir, std::string& error) {
  std::error_code ec;
  if (fs::exists(run_dir, ec)) {
    error = "results folder " + run_dir + " already exists. Pick another --run-id or remove it.";
    return nullptr;
  }
  fs::create_directories(fs::path(run_dir) / "failures", ec);
  if (ec) {
    error = "cannot create " + run_dir + ": " + ec.message() + ". Check output.dir and permissions.";
    return nullptr;
  }
  std::unique_ptr<ResultsWriter> w(new ResultsWriter(cfg, run_dir));
  if (!write_text(fs::path(run_dir) / "config.toml", to_toml(cfg), error)) return nullptr;
  w->events_.open(fs::path(run_dir) / "events.bin", std::ios::binary);
  EventsHeader h;
  h.generator_version = gen::kGeneratorVersion;
  h.profile = static_cast<std::uint32_t>(cfg.gen.profile);
  h.campaign_seed = cfg.seed;
  h.checker = 1;
  w->events_.write(reinterpret_cast<const char*>(&h), sizeof h);
  if (!w->events_) {
    error = "cannot write " + run_dir + "/events.bin. Check free disk space.";
    return nullptr;
  }
  return w;
}

void ResultsWriter::on_record(const EventRecord& rec) {
  events_.write(reinterpret_cast<const char*>(&rec), sizeof rec);
  ++totals_.programs;
  totals_.core_cycles += rec.core_cycles;
  totals_.model_instructions += rec.model_instret;
  if (rec.verdict != static_cast<std::uint8_t>(Verdict::Pass)) {
    ++totals_.failures;
    if (rec.verdict < totals_.by_verdict.size()) ++totals_.by_verdict[rec.verdict];
    if (totals_.failure_dirs_written < cfg_.max_failure_dirs) write_failure_dir(rec);
  }
}

void ResultsWriter::write_failure_dir(const EventRecord& rec) {
  char name[48];
  std::snprintf(name, sizeof name, "program-%09" PRIu64, rec.program_id);
  const fs::path dir = fs::path(dir_) / "failures" / name;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return;
  ++totals_.failure_dirs_written;

  const Execution e = execute_program(cfg_, rec.program_id);
  const CheckOutcome outcome = check_c1(e.model, e.core);
  std::ostringstream t;
  t << "program id:        " << rec.program_id << "\n"
    << "program seed:      " << hex64(rec.seed) << "\n"
    << "campaign seed:     " << cfg_.seed << "\n"
    << "generator:         version " << gen::kGeneratorVersion << ", profile g0, body length " << (e.program.body_end - e.program.body_begin)
    << " words (min_len " << cfg_.gen.min_len << ", max_len " << cfg_.gen.max_len << ", max_skip " << cfg_.gen.max_skip << ")\n"
    << "verdict:           " << to_string(static_cast<Verdict>(rec.verdict));
  if (rec.verdict == static_cast<std::uint8_t>(Verdict::RegMismatch)) t << " (first differing register: x" << int(rec.detail) << ")";
  t << "\n";
  if (outcome.verdict != static_cast<Verdict>(rec.verdict)) t << "WARNING:           re-running the program gave verdict " << to_string(outcome.verdict) << ": the failure is not reproducible\n";
  t << "\nmodel: status " << to_string(e.model.status) << ", exit code " << hex32(e.model.exit_code) << ", " << e.model.instret
    << " instructions retired, memory hash " << hex64(e.model.mem_hash) << "\n"
    << "core:  status " << to_string(e.core.status) << ", exit code " << hex32(e.core.exit_code) << ", " << e.core.cycles
    << " cycles, memory hash " << hex64(e.core.mem_hash) << "\n";
  bool any = false;
  for (unsigned i = 1; i < 32; ++i) {
    if (e.model.regs[i] == e.core.regs[i]) continue;
    if (!any) t << "\ndiffering registers (model vs core):\n";
    any = true;
    t << "  x" << i << ": " << hex32(e.model.regs[i]) << " vs " << hex32(e.core.regs[i]) << "\n";
  }
  t << "\nTo reproduce: the program is regenerated from the program seed with the generator settings above\n"
       "(program.hex holds the exact words). The `cutoff replay` command arrives in a later step.\n";
  std::string ignored;
  write_text(dir / "failure.txt", t.str(), ignored);

  std::ostringstream hex;
  for (std::size_t i = 0; i < e.program.code.size(); ++i) hex << hex32(static_cast<std::uint32_t>(4 * i)) << " " << hex32(e.program.code[i]) << "\n";
  write_text(dir / "program.hex", hex.str(), ignored);
}

bool ResultsWriter::finish(const std::string& run_id, const Timing& timing, std::uint32_t workers, int exit_code,
                           const std::string& stop_reason, std::string& error) {
  events_.flush();
  if (!events_) {
    error = "cannot write " + dir_ + "/events.bin. Check free disk space.";
    return false;
  }
  events_.close();

  std::ostringstream j;
  j << "{\n"
    << "  \"schema\": \"cutoff-summary-provisional-1\",\n"
    << "  \"run\": {\"id\": \"" << json_escape(run_id) << "\", \"tool_version\": \"0.0.1\", \"git_commit\": \"" << CUTOFF_GIT_COMMIT
    << "\", \"git_tree_dirty\": " << (CUTOFF_GIT_DIRTY != 0 ? "true" : "false") << ", \"config_hash\": \"" << hex64(config_hash(cfg_)) << "\"},\n"
    << "  \"environment\": {\"cpu\": \"" << json_escape(cpu_model()) << "\", \"logical_cores\": " << logical_cores()
    << ", \"verilator\": \"" << json_escape(verilator_version_string()) << "\", \"compiler\": \"" << json_escape(compiler_version()) << "\"},\n"
    << "  \"core\": {\"name\": \"" << cfg_.core_name << "\", \"commit\": \"" << CUTOFF_CORE_COMMIT << "\"},\n"
    << "  \"campaign\": {\"seed\": " << cfg_.seed << ", \"programs_requested\": " << cfg_.programs << ", \"programs_run\": " << totals_.programs
    << ", \"generator_version\": " << gen::kGeneratorVersion << ", \"profile\": \"g0\", \"checker\": \"" << cfg_.checker
    << "\", \"stop_reason\": \"" << json_escape(stop_reason) << "\", \"exit_code\": " << exit_code << "},\n"
    << "  \"failures\": {\"count\": " << totals_.failures << ", \"failure_dirs_written\": " << totals_.failure_dirs_written << ", \"by_verdict\": {";
  for (std::size_t v = 1; v < totals_.by_verdict.size(); ++v)
    j << (v > 1 ? ", " : "") << "\"" << to_string(static_cast<Verdict>(v)) << "\": " << totals_.by_verdict[v];
  j << "}},\n"
    << "  \"totals\": {\"core_cycles\": " << totals_.core_cycles << ", \"model_instructions\": " << totals_.model_instructions << "},\n"
    << "  \"workers\": " << workers << ",\n"
    << "  \"timing\": {\"started_utc\": \"" << timing.started_utc << "\", \"wall_seconds\": " << timing.wall_seconds << ", \"programs_per_second\": "
    << (timing.wall_seconds > 0 ? static_cast<double>(totals_.programs) / timing.wall_seconds : 0.0) << "}\n"
    << "}\n";
  return write_text(fs::path(dir_) / "summary.json", j.str(), error);
}

}  // namespace cutoff::engine
