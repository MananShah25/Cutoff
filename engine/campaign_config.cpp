#include "campaign_config.hpp"

#include <algorithm>
#include <functional>
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

Setting string_setting(std::string key, std::string CampaignConfig::*field) {
  return {std::move(key), Type::String, [field](CampaignConfig& c, const TomlValue& v) { c.*field = v.s; return std::string(); }};
}

const std::vector<Setting>& settings() {
  static const std::vector<Setting> table = {
      string_setting("core.name", &CampaignConfig::core_name),
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
      string_setting("output.dir", &CampaignConfig::results_dir),
      uint_setting("output.max_failure_dirs", &CampaignConfig::max_failure_dirs, 100000),
      uint_setting("debug.corrupt_core_every", &CampaignConfig::corrupt_core_every, 1'000'000'000),
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
  if (cfg.programs < 1) return "campaign.programs must be at least 1.";
  if (cfg.batch_size < 1) return "campaign.batch_size must be at least 1.";
  if (cfg.checker != "c1") return "checker.level \"" + cfg.checker + "\" is not available yet: only \"c1\" exists until Step 8 adds c2 and c3.";
  if (cfg.max_cycles < 100) return "checker.max_cycles must be at least 100, otherwise every program is reported as a hang.";
  if (cfg.max_instructions < 1) return "checker.max_instructions must be at least 1.";
  if (cfg.results_dir.empty()) return "output.dir must not be empty.";
  return gen::validate(cfg.gen);
}

std::string to_toml(const CampaignConfig& c) {
  std::ostringstream os;
  os << "[core]\nname = \"" << c.core_name << "\"\n\n";
  os << "[campaign]\nseed = " << c.seed << "\nprograms = " << c.programs << "\nworkers = " << c.workers
     << "\nbatch_size = " << c.batch_size << "\n\n";
  os << "[generator]\nprofile = \"" << profile_name(c.gen.profile) << "\"\nmin_len = " << c.gen.min_len
     << "\nmax_len = " << c.gen.max_len << "\nmax_skip = " << c.gen.max_skip << "\n\n";
  os << "[checker]\nlevel = \"" << c.checker << "\"\nmax_cycles = " << c.max_cycles << "\nmax_instructions = " << c.max_instructions << "\n\n";
  os << "[output]\ndir = \"" << c.results_dir << "\"\nmax_failure_dirs = " << c.max_failure_dirs << "\n\n";
  os << "[debug]\ncorrupt_core_every = " << c.corrupt_core_every << "\ncrash_worker_at = " << c.crash_worker_at << "\n";
  return os.str();
}

std::uint32_t resolved_workers(const CampaignConfig& cfg) {
  if (cfg.workers != 0) return cfg.workers;
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1 : n;
}

}  // namespace cutoff::engine
