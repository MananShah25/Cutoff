#include "run_command.hpp"

#include <unistd.h>

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "run.hpp"

namespace cutoff {

namespace {

constexpr const char* kHelp =
    "usage: cutoff run [options]\n"
    "  Runs random programs on the core and the reference model and checks them (checker C1).\n"
    "options:\n"
    "  --config FILE        settings file (default: ./cutoff.toml if it exists)\n"
    "  --max-programs N     program budget: run at most N programs (alias: --programs)\n"
    "  --epsilon X          stop when the estimated risk of a new behaviour stays below X (0 = no stop rule)\n"
    "  --confirm W          ... for W consecutive checkpoints (default 3)\n"
    "  --min-programs N     never stop before N programs (default 1000)\n"
    "  --checkpoint N       evaluate the risk and save a checkpoint every N programs (default 1000)\n"
    "  --estimator NAME     good-turing (default), incidence or coverage\n"
    "  --max-hours H        wall-clock budget across all sessions of the run\n"
    "  --resume DIR         continue the run in results folder DIR from its checkpoint (e.g. after Ctrl-C, or with a larger budget)\n"
    "  --workers N          worker processes (default: one per hardware thread)\n"
    "  --seed N             campaign seed\n"
    "  --profile g0         generator profile\n"
    "  --checker c1         checker level\n"
    "  --max-cycles N       core cycle limit per program (hitting it is a hang)\n"
    "  --results-dir DIR    where result folders go (default: results)\n"
    "  --run-id ID          name of the result folder (default: run-<UTC time>-seed<seed>)\n"
    "  --progress | --no-progress   live progress line (default: only on a terminal)\n"
    "exit codes: 0 stopped at the threshold (or ran every program with the rule off) with no failure, 1 at least one failure,\n"
    "            2 a budget ended before the threshold was reached, 3 setup or environment error, 130 interrupted (resumable)\n";

bool is_number(const std::string& s) {
  std::uint64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return !s.empty() && r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool parse_double(const std::string& s, double& out) {
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return !s.empty() && end == s.c_str() + s.size();
}

}  // namespace

int run_command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  using engine::TomlValue;
  engine::CampaignConfig cfg;
  std::string config_path, run_id, resume_dir;
  int progress = -1;  // -1 = only if stderr is a terminal
  engine::TomlDoc overrides;

  auto set_int = [&](const std::string& key, const std::string& flag, const std::string& v) {
    if (!is_number(v)) {
      err << "cutoff: " << flag << " needs a whole number, not '" << v << "'. See `cutoff run --help`.\n";
      return false;
    }
    TomlValue t;
    t.type = TomlValue::Type::Int;
    t.i = static_cast<std::int64_t>(std::stoull(v));
    overrides[key] = t;
    return true;
  };
  auto set_float = [&](const std::string& key, const std::string& flag, const std::string& v) {
    double d = 0;
    if (!parse_double(v, d)) {
      err << "cutoff: " << flag << " needs a number, not '" << v << "'. See `cutoff run --help`.\n";
      return false;
    }
    TomlValue t;
    t.type = TomlValue::Type::Float;
    t.d = d;
    overrides[key] = t;
    return true;
  };
  auto set_str = [&](const std::string& key, const std::string& v) {
    TomlValue t;
    t.type = TomlValue::Type::String;
    t.s = v;
    overrides[key] = t;
  };

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= args.size()) {
        err << "cutoff: " << a << " needs a value. See `cutoff run --help`.\n";
        return false;
      }
      dst = args[++i];
      return true;
    };
    std::string v;
    if (a == "--help" || a == "-h") { out << kHelp; return 0; }
    else if (a == "--progress") progress = 1;
    else if (a == "--no-progress") progress = 0;
    else if (a == "--config") { if (!value(config_path)) return 3; }
    else if (a == "--run-id") { if (!value(run_id)) return 3; }
    else if (a == "--results-dir") { if (!value(v)) return 3; set_str("output.dir", v); }
    else if (a == "--profile") { if (!value(v)) return 3; set_str("generator.profile", v); }
    else if (a == "--checker") { if (!value(v)) return 3; set_str("checker.level", v); }
    else if (a == "--programs" || a == "--max-programs") { if (!value(v) || !set_int("campaign.programs", a, v)) return 3; }
    else if (a == "--confirm") { if (!value(v) || !set_int("stop.confirm", a, v)) return 3; }
    else if (a == "--min-programs") { if (!value(v) || !set_int("stop.min_programs", a, v)) return 3; }
    else if (a == "--checkpoint") { if (!value(v) || !set_int("stop.checkpoint", a, v)) return 3; }
    else if (a == "--epsilon") { if (!value(v) || !set_float("stop.epsilon", a, v)) return 3; }
    else if (a == "--max-hours") { if (!value(v) || !set_float("stop.max_hours", a, v)) return 3; }
    else if (a == "--estimator") { if (!value(v)) return 3; set_str("stop.estimator", v); }
    else if (a == "--resume") { if (!value(resume_dir)) return 3; }
    else if (a == "--workers") { if (!value(v) || !set_int("campaign.workers", a, v)) return 3; }
    else if (a == "--seed") { if (!value(v) || !set_int("campaign.seed", a, v)) return 3; }
    else if (a == "--max-cycles") { if (!value(v) || !set_int("checker.max_cycles", a, v)) return 3; }
    else {
      err << "cutoff: unknown option '" << a << "'. See `cutoff run --help`.\n";
      return 3;
    }
  }

  if (!resume_dir.empty()) {
    if (!config_path.empty() || !run_id.empty()) {
      err << "cutoff: --resume continues a saved run with its saved settings; do not combine it with --config or --run-id. Use options such as --max-programs to change settings.\n";
      return 3;
    }
    config_path = (std::filesystem::path(resume_dir) / "config.toml").string();
    if (!std::filesystem::exists(config_path)) {
      err << "cutoff: '" << resume_dir << "' is not a Cutoff run folder (no config.toml). Pass the folder printed as 'results folder' by `cutoff run`.\n";
      return 3;
    }
  } else if (config_path.empty() && std::filesystem::exists("cutoff.toml")) {
    config_path = "cutoff.toml";
  }
  if (!config_path.empty()) {
    std::ifstream f(config_path);
    if (!f) {
      err << "cutoff: cannot read config file '" << config_path << "'. Check the path or omit --config.\n";
      return 3;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const auto parsed = engine::parse_toml(ss.str(), config_path);
    if (!parsed.error.empty()) { err << "cutoff: " << parsed.error << "\n"; return 3; }
    if (const std::string e = engine::apply_toml(cfg, parsed.doc, config_path); !e.empty()) { err << "cutoff: " << e << "\n"; return 3; }
  }
  if (const std::string e = engine::apply_toml(cfg, overrides, "command line"); !e.empty()) { err << "cutoff: " << e << "\n"; return 3; }

  if (run_id.empty()) run_id = engine::default_run_id(cfg);
  const bool show = progress == 1 || (progress == -1 && isatty(STDERR_FILENO) != 0);
  return engine::execute_run(cfg, run_id, show, out, err, resume_dir).exit_code;
}

}  // namespace cutoff
