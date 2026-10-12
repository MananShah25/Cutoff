#include "replay_command.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "campaign_config.hpp"
#include "check.hpp"
#include "toml_lite.hpp"

namespace cutoff {
int replay_command(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  if (args.size() != 2 || args[0] == "--help") {
    out << "usage: cutoff replay <results-folder> <program-id>\n"
           "Re-runs a saved program using its resolved config and prints its checker result.\n";
    return args.size() == 1 ? 0 : 3;
  }
  std::uint64_t id = 0;
  const auto parsed_id = std::from_chars(args[1].data(), args[1].data() + args[1].size(), id);
  if (parsed_id.ec != std::errc{} || parsed_id.ptr != args[1].data() + args[1].size()) {
    err << "cutoff: program-id must be a non-negative integer.\n";
    return 3;
  }
  const std::filesystem::path config_path = std::filesystem::path(args[0]) / "config.toml";
  std::ifstream f(config_path);
  if (!f) { err << "cutoff: cannot read '" << config_path.string() << "'.\n"; return 3; }
  std::stringstream text;
  text << f.rdbuf();
  const auto toml = engine::parse_toml(text.str(), config_path.string());
  if (!toml.error.empty()) { err << "cutoff: " << toml.error << "\n"; return 3; }
  engine::CampaignConfig cfg;
  if (const std::string e = engine::apply_toml(cfg, toml.doc, config_path.string()); !e.empty()) { err << "cutoff: " << e << "\n"; return 3; }
  const engine::Execution e = engine::execute_program(cfg, id);
  const engine::CheckOutcome result = cfg.checker == "c3" ? engine::check_c3(e.model, e.core, e.rvfi)
      : cfg.checker == "c2" ? engine::check_c2(e.model, e.core) : engine::check_c1(e.model, e.core);
  const char* profile = cfg.gen.profile == gen::Profile::G0 ? "g0" : cfg.gen.profile == gen::Profile::G1 ? "g1"
      : cfg.gen.profile == gen::Profile::G2 ? "g2" : "g3";
  out << "program " << id << " (seed " << e.program.seed << ", profile " << profile
      << ", checker " << cfg.checker << "): " << engine::to_string(result.verdict) << "\n"
      << "model: " << to_string(e.model.status) << ", " << e.model.instret << " retired\n"
      << "core:  " << to_string(e.core.status) << ", " << e.core.cycles << " cycles\n";
  return result.verdict == engine::Verdict::Pass ? 0 : 1;
}
}  // namespace cutoff
