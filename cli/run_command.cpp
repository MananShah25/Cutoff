#include "run_command.hpp"

#include <unistd.h>

#include <charconv>
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
    "  --programs N         number of programs to run\n"
    "  --workers N          worker processes (default: one per hardware thread)\n"
    "  --seed N             campaign seed\n"
    "  --profile g0         generator profile\n"
    "  --checker c1         checker level\n"
    "  --max-cycles N       core cycle limit per program (hitting it is a hang)\n"
    "  --results-dir DIR    where result folders go (default: results)\n"
    "  --run-id ID          name of the result folder (default: run-<UTC time>-seed<seed>)\n"
    "  --progress | --no-progress   live progress line (default: only on a terminal)\n"
    "exit codes: 0 no failures, 1 at least one failure, 3 setup or environment error\n";

bool is_number(const std::string& s) {
  std::uint64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return !s.empty() && r.ec == std::errc() && r.ptr == s.data() + s.size();
}

}  // namespace

int run_command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  using engine::TomlValue;
  engine::CampaignConfig cfg;
  std::string config_path, run_id;
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
    else if (a == "--programs") { if (!value(v) || !set_int("campaign.programs", a, v)) return 3; }
    else if (a == "--workers") { if (!value(v) || !set_int("campaign.workers", a, v)) return 3; }
    else if (a == "--seed") { if (!value(v) || !set_int("campaign.seed", a, v)) return 3; }
    else if (a == "--max-cycles") { if (!value(v) || !set_int("checker.max_cycles", a, v)) return 3; }
    else {
      err << "cutoff: unknown option '" << a << "'. See `cutoff run --help`.\n";
      return 3;
    }
  }

  if (config_path.empty() && std::filesystem::exists("cutoff.toml")) config_path = "cutoff.toml";
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
  return engine::execute_run(cfg, run_id, show, out, err).exit_code;
}

}  // namespace cutoff
