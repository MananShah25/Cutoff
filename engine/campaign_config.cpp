#include "campaign_config.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>

#include "sim_drill.hpp"
#include <sstream>
#include <thread>
#include <vector>

namespace cutoff::engine {

namespace {

using Type = TomlValue::Type;

const char* type_name(Type t) {
  switch (t) {
    case Type::String: return "a quoted string";
    case Type::Int: return "an integer";
    case Type::Float: return "a number";
    case Type::Bool: return "true or false";
  }
  return "?";
}

struct Setting {
  std::string key;
  Type type;
  std::function<std::string(CampaignConfig&, const TomlValue&)> apply;  // returns an error or ""
};

std::string range_error(const std::string& key, std::int64_t v, std::int64_t lo, std::int64_t hi) {
  if (v >= lo && v <= hi) return {};
  return key + " must be between " + std::to_string(lo) + " and " + std::to_string(hi) + " (found " + std::to_string(v) + ")";
}

template <typename T>
Setting uint_setting(const std::string& key, T CampaignConfig::*field, std::int64_t hi) {
  // NOLINTNEXTLINE(bugprone-exception-escape): reported for the lambda's implicit move; it only builds strings
  return {key, Type::Int, [key, field, hi](CampaignConfig& c, const TomlValue& v) {
            std::string e = range_error(key, v.i, 0, hi);
            if (e.empty()) c.*field = static_cast<T>(v.i);
            return e;
          }};
}

Setting bool_setting(std::string key, const std::function<void(CampaignConfig&, bool)>& set) {
  return {std::move(key), Type::Bool, [set](CampaignConfig& c, const TomlValue& v) { set(c, v.b); return std::string(); }};
}

Setting double_setting(const std::string& key, const std::function<void(CampaignConfig&, double)>& set) {
  // NOLINTNEXTLINE(bugprone-exception-escape): reported for the lambda's implicit move; it only builds strings
  return {key, Type::Float, [key, set](CampaignConfig& c, const TomlValue& v) -> std::string {
            const double d = v.type == Type::Int ? static_cast<double>(v.i) : v.d;
            if (!(d >= 0.0) || d > 1e12) return key + " must be a non-negative number (found " + std::to_string(d) + ")";
            set(c, d);
            return {};
          }};
}

Setting string_setting(std::string key, std::string CampaignConfig::*field) {
  return {std::move(key), Type::String, [field](CampaignConfig& c, const TomlValue& v) { c.*field = v.s; return std::string(); }};
}

const std::vector<Setting>& settings() {
  static const std::vector<Setting> table = {
      string_setting("core.name", &CampaignConfig::core_name),
      {"core.mutant", Type::Int, [](CampaignConfig& c, const TomlValue& v) { auto e = range_error("core.mutant", v.i, -1, 65535); if (e.empty()) c.mutant = v.i; return e; }},
      uint_setting("campaign.seed", &CampaignConfig::seed, INT64_MAX),
      uint_setting("campaign.programs", &CampaignConfig::programs, 1'000'000'000'000LL),
      uint_setting("campaign.workers", &CampaignConfig::workers, 1024),
      uint_setting("campaign.batch_size", &CampaignConfig::batch_size, 1'000'000),
      {"generator.profile", Type::String, [](CampaignConfig& c, const TomlValue& v) -> std::string {
         std::string p = v.s;
         std::transform(p.begin(), p.end(), p.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
         if (p == "g0") c.gen.profile = gen::Profile::G0;
         else if (p == "g1") c.gen.profile = gen::Profile::G1;
         else if (p == "g2") c.gen.profile = gen::Profile::G2;
         else if (p == "g3") c.gen.profile = gen::Profile::G3;
         else return "generator.profile must be one of g0, g1, g2, g3 (found \"" + v.s + "\")";
         return {};
       }},
      {"generator.min_len", Type::Int, [](CampaignConfig& c, const TomlValue& v) { auto e = range_error("generator.min_len", v.i, 0, 1'000'000); if (e.empty()) c.gen.min_len = static_cast<std::uint32_t>(v.i); return e; }},
      {"generator.max_len", Type::Int, [](CampaignConfig& c, const TomlValue& v) { auto e = range_error("generator.max_len", v.i, 0, 1'000'000); if (e.empty()) c.gen.max_len = static_cast<std::uint32_t>(v.i); return e; }},
      {"generator.max_skip", Type::Int, [](CampaignConfig& c, const TomlValue& v) { auto e = range_error("generator.max_skip", v.i, 0, 1'000'000); if (e.empty()) c.gen.max_skip = static_cast<std::uint32_t>(v.i); return e; }},
      string_setting("checker.level", &CampaignConfig::checker),
      uint_setting("checker.max_cycles", &CampaignConfig::max_cycles, INT64_MAX),
      uint_setting("checker.max_instructions", &CampaignConfig::max_instructions, INT64_MAX),
      bool_setting("drill.enabled", [](CampaignConfig& c, bool b) { c.drill = b; }),
      uint_setting("drill.mutants", &CampaignConfig::drill_mutants, 100000),
      {"drill.sample_seed", Type::Int, [](CampaignConfig& c, const TomlValue& v) { auto e = range_error("drill.sample_seed", v.i, -1, INT64_MAX); if (e.empty()) c.drill_sample_seed = v.i; return e; }},
      string_setting("drill.operators", &CampaignConfig::drill_operators),
      bool_setting("drill.filter", [](CampaignConfig& c, bool b) { c.drill_filter = b; }),
      bool_setting("species.op", [](CampaignConfig& c, bool b) { c.species.op = b; }),
      bool_setting("species.cross", [](CampaignConfig& c, bool b) { c.species.cross = b; }),
      double_setting("stop.epsilon", [](CampaignConfig& c, double d) { c.stop.epsilon = d; }),
      {"stop.confirm", Type::Int, [](CampaignConfig& c, const TomlValue& v) { auto e = range_error("stop.confirm", v.i, 1, 1'000'000); if (e.empty()) c.stop.confirm = static_cast<std::uint32_t>(v.i); return e; }},
      {"stop.min_programs", Type::Int, [](CampaignConfig& c, const TomlValue& v) { auto e = range_error("stop.min_programs", v.i, 0, INT64_MAX); if (e.empty()) c.stop.min_programs = static_cast<std::uint64_t>(v.i); return e; }},
      {"stop.checkpoint", Type::Int, [](CampaignConfig& c, const TomlValue& v) { auto e = range_error("stop.checkpoint", v.i, 1, INT64_MAX); if (e.empty()) c.stop.checkpoint = static_cast<std::uint64_t>(v.i); return e; }},
      {"stop.plateau_window", Type::Int, [](CampaignConfig& c, const TomlValue& v) { auto e = range_error("stop.plateau_window", v.i, 1, INT64_MAX); if (e.empty()) c.stop.plateau_window = static_cast<std::uint64_t>(v.i); return e; }},
      {"stop.estimator", Type::String, [](CampaignConfig& c, const TomlValue& v) -> std::string {
         if (v.s == "good-turing") c.stop.estimator = risk::Estimator::GoodTuring;
         else if (v.s == "incidence") c.stop.estimator = risk::Estimator::IncidenceRate;
         else if (v.s == "coverage") c.stop.estimator = risk::Estimator::CoverageRisk;
         else return "stop.estimator must be \"good-turing\", \"incidence\" or \"coverage\" (found \"" + v.s + "\")";
         return {};
       }},
      double_setting("stop.max_hours", [](CampaignConfig& c, double d) { c.max_hours = d; }),
      string_setting("output.dir", &CampaignConfig::results_dir),
      uint_setting("output.max_failure_dirs", &CampaignConfig::max_failure_dirs, 100000),
      uint_setting("debug.corrupt_core_every", &CampaignConfig::corrupt_core_every, 1'000'000'000),
      uint_setting("debug.interrupt_after", &CampaignConfig::interrupt_after, INT64_MAX),
      {"debug.crash_worker_at", Type::Int, [](CampaignConfig& c, const TomlValue& v) { c.crash_worker_at = v.i; return std::string(); }},
  };
  return table;
}

const char* profile_name(gen::Profile p) {
  switch (p) {
    case gen::Profile::G0: return "g0";
    case gen::Profile::G1: return "g1";
    case gen::Profile::G2: return "g2";
    case gen::Profile::G3: return "g3";
  }
  return "?";
}

}  // namespace

std::string apply_toml(CampaignConfig& cfg, const TomlDoc& doc, const std::string& source_name) {
  for (const auto& [key, value] : doc) {
    const std::string where = value.line > 0 ? source_name + ":" + std::to_string(value.line) : source_name;
    const auto it = std::find_if(settings().begin(), settings().end(), [&](const Setting& s) { return s.key == key; });
    if (it == settings().end()) {
      std::string valid;
      for (const auto& s : settings()) {
        if (!valid.empty()) valid += ", ";
        valid += s.key;
      }
      std::string msg = where;
      msg += ": unknown setting '";
      msg += key;
      msg += "'. Valid settings: ";
      msg += valid;
      return msg;
    }
    if (value.type != it->type && !(it->type == Type::Float && value.type == Type::Int)) {
      std::string msg = where;
      msg += ": ";
      msg += key;
      msg += " must be ";
      msg += type_name(it->type);
      msg += ", not ";
      msg += type_name(value.type);
      return msg;
    }
    if (const std::string e = it->apply(cfg, value); !e.empty()) {
      std::string msg = where;
      msg += ": ";
      msg += e;
      return msg;
    }
  }
  return {};
}

std::string validate(const CampaignConfig& cfg) {
  if (cfg.core_name != "picorv32") return "core.name \"" + cfg.core_name + "\" is not supported; only \"picorv32\" exists so far.";
  if (cfg.mutant > drill_mutant_count()) return "core.mutant " + std::to_string(cfg.mutant) + " does not exist: this build has mutants 1 to " + std::to_string(drill_mutant_count()) + " (list them with `cutoff build --drill --list`).";
  if (cfg.drill && cfg.mutant >= 0) return "a drill runs its own mutants: leave core.mutant at -1 (or use `cutoff run --mutant N` for a single planted bug).";
  if (cfg.programs < 1) return "campaign.programs must be at least 1.";
  if (cfg.batch_size < 1) return "campaign.batch_size must be at least 1.";
  if (cfg.checker != "c1") return "checker.level \"" + cfg.checker + "\" is not available yet: only \"c1\" exists until Step 8 adds c2 and c3.";
  if (cfg.max_cycles < 100) return "checker.max_cycles must be at least 100, otherwise every program is reported as a hang.";
  if (cfg.max_instructions < 1) return "checker.max_instructions must be at least 1.";
  if (cfg.results_dir.empty()) return "output.dir must not be empty.";
  if (cfg.stop.epsilon >= 1.0 && cfg.stop.epsilon != 0.0) return "stop.epsilon is a risk in (0, 1); a value of 1 or more would stop immediately. Use 0 to switch the rule off.";
  if (cfg.stop.epsilon > 0.0 && !cfg.species.any()) return "stop.epsilon > 0 needs at least one species set: set species.op or species.cross to true.";
  if (cfg.stop.epsilon > 0.0 && cfg.stop.checkpoint > cfg.programs) return "stop.checkpoint is larger than campaign.programs, so the stop rule would never be evaluated. Lower stop.checkpoint.";
  return gen::validate(cfg.gen);
}

std::string to_toml(const CampaignConfig& c) {
  std::ostringstream os;
  os << "[core]\nname = \"" << c.core_name << "\"\nmutant = " << c.mutant << "\n\n";
  os << "[campaign]\nseed = " << c.seed << "\nprograms = " << c.programs << "\nworkers = " << c.workers
     << "\nbatch_size = " << c.batch_size << "\n\n";
  os << "[generator]\nprofile = \"" << profile_name(c.gen.profile) << "\"\nmin_len = " << c.gen.min_len
     << "\nmax_len = " << c.gen.max_len << "\nmax_skip = " << c.gen.max_skip << "\n\n";
  os << "[checker]\nlevel = \"" << c.checker << "\"\nmax_cycles = " << c.max_cycles << "\nmax_instructions = " << c.max_instructions << "\n\n";
  os << "[drill]\nenabled = " << (c.drill ? "true" : "false") << "\nmutants = " << c.drill_mutants << "\nsample_seed = " << c.drill_sample_seed
     << "\noperators = \"" << c.drill_operators << "\"\nfilter = " << (c.drill_filter ? "true" : "false") << "\n\n";
  char eps[40], hours[40];
  std::snprintf(eps, sizeof eps, "%.17g", c.stop.epsilon);
  std::snprintf(hours, sizeof hours, "%.17g", c.max_hours);
  os << "[species]\nop = " << (c.species.op ? "true" : "false") << "\ncross = " << (c.species.cross ? "true" : "false") << "\n\n";
  os << "[stop]\nepsilon = " << eps << "\nconfirm = " << c.stop.confirm << "\nmin_programs = " << c.stop.min_programs
     << "\ncheckpoint = " << c.stop.checkpoint << "\nestimator = \"" << risk::estimator_name(c.stop.estimator) << "\"\nplateau_window = "
     << c.stop.plateau_window << "\nmax_hours = " << hours << "\n\n";
  os << "[output]\ndir = \"" << c.results_dir << "\"\nmax_failure_dirs = " << c.max_failure_dirs << "\n\n";
  os << "[debug]\ncorrupt_core_every = " << c.corrupt_core_every << "\ncrash_worker_at = " << c.crash_worker_at << "\ninterrupt_after = " << c.interrupt_after << "\n";
  return os.str();
}

std::uint32_t resolved_workers(const CampaignConfig& cfg) {
  if (cfg.workers != 0) return cfg.workers;
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1 : n;
}

}  // namespace cutoff::engine

namespace cutoff::engine {

std::uint64_t stream_hash(const CampaignConfig& cfg) {
  std::ostringstream os;
  os << cfg.core_name << '|' << cfg.mutant << '|' << cfg.seed << '|' << static_cast<int>(cfg.gen.profile) << '|' << cfg.gen.min_len << '|' << cfg.gen.max_len
     << '|' << cfg.gen.max_skip << '|' << cfg.checker << '|' << cfg.max_cycles << '|' << cfg.max_instructions << '|' << cfg.species.op
     << cfg.species.cross << '|' << cfg.drill << '|' << cfg.drill_filter << '|' << cfg.drill_mutants << '|' << cfg.drill_sample_seed << '|' << cfg.drill_operators << '|' << cfg.corrupt_core_every << '|' << gen::kGeneratorVersion;
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const char ch : os.str()) {
    h ^= static_cast<unsigned char>(ch);
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace cutoff::engine
