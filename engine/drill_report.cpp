#include "drill_report.hpp"

#include <cstdio>
#include <map>
#include <sstream>

namespace cutoff::engine {

namespace {

std::string pct(double x) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f%%", 100.0 * x);
  return buf;
}

std::string num(std::uint64_t v) { return v == kNever ? "-" : std::to_string(v); }

}  // namespace

DrillReport make_drill_report(const DrillTally& tally, const std::vector<MutantInfo>& infos, std::uint64_t stop_at) {
  DrillReport r;
  r.planted = tally.ids().size();
  r.programs = tally.programs();
  r.stop_at = stop_at;
  r.pairs_total = tally.pairs_total();
  r.pairs_skipped = tally.pairs_skipped();
  r.propagated_without_infection = tally.propagated_without_infection();
  r.detected_without_propagation = tally.detected_without_propagation();
  std::map<std::string, DrillReport::Row> cats;
  for (std::size_t j = 0; j < r.planted; ++j) {
    const MutantTally& t = tally.mutants()[j];
    const MutantInfo& info = infos[static_cast<std::size_t>(tally.ids()[j] - 1)];
    DrillReport::Row& row = cats[info.category];
    row.category = info.category;
    ++row.planted;
    if (t.infected > 0) ++r.infected_any;
    if (t.propagated > 0) ++r.propagated_any;
    switch (DrillTally::outcome(t)) {
      case Outcome::Killed:
        ++r.killed;
        ++row.killed;
        if (stop_at > 0 && t.first_detected < stop_at) ++r.killed_by_stop;
        break;
      case Outcome::NeverInfected: ++r.never_infected; break;
      case Outcome::InfectedNotPropagated: ++r.infected_not_propagated; break;
      case Outcome::PropagatedNotDetected: ++r.propagated_not_detected; break;
    }
  }
  for (auto& [name, row] : cats) r.by_category.push_back(row);
  r.score_raw = r.planted == 0 ? 0 : static_cast<double>(r.killed) / static_cast<double>(r.planted);
  const std::size_t denom = r.planted - r.infected_not_propagated;
  r.score_upper = denom == 0 ? 0 : static_cast<double>(r.killed) / static_cast<double>(denom);
  return r;
}

std::string drill_tsv(const DrillTally& tally, const std::vector<MutantInfo>& infos, std::uint64_t stop_at) {
  std::ostringstream os;
  os << "id\tcategory\top_class\tkind\tline\tran\tinfected\tpropagated\tdetected\tfirst_infected\tfirst_propagated\tfirst_detected\toutcome\tfound_by_stop\n";
  for (std::size_t j = 0; j < tally.ids().size(); ++j) {
    const MutantTally& t = tally.mutants()[j];
    const MutantInfo& m = infos[static_cast<std::size_t>(tally.ids()[j] - 1)];
    const Outcome o = DrillTally::outcome(t);
    const char* by_stop = stop_at == 0 ? "-" : (o == Outcome::Killed && t.first_detected < stop_at) ? "yes" : "no";
    os << m.id << '\t' << m.category << '\t' << m.op_class << '\t' << m.kind << '\t' << m.line << '\t' << t.ran << '\t' << t.infected << '\t' << t.propagated << '\t'
       << t.detected << '\t' << num(t.first_infected) << '\t' << num(t.first_propagated) << '\t' << num(t.first_detected) << '\t' << to_string(o) << '\t' << by_stop << '\n';
  }
  return os.str();
}

std::string drill_text(const DrillReport& r) {
  std::ostringstream os;
  os << "drill:          " << r.planted << " planted bugs against " << r.programs << " programs\n"
     << "funnel:         planted " << r.planted << " > infected " << r.infected_any << " > propagated " << r.propagated_any << " > detected " << r.killed << "\n";
  if (r.stop_at > 0)
    os << "found by stop:  " << r.killed_by_stop << " of " << r.planted << " (" << pct(r.planted ? static_cast<double>(r.killed_by_stop) / static_cast<double>(r.planted) : 0)
       << ") by the time the stop rule fired at " << r.stop_at << " programs; " << r.killed << " by the end of the " << r.programs << "-program budget\n";
  else
    os << "found:          " << r.killed << " of " << r.planted << " (" << pct(r.score_raw) << ") within the " << r.programs << "-program budget (the stop rule did not fire)\n";
  os << "survivors:      " << (r.planted - r.killed) << " = " << r.never_infected << " never infected (stimulus gaps or dead logic) + " << r.infected_not_propagated
     << " infected but never propagated (masked or equivalent) + " << r.propagated_not_detected << " propagated but not detected by C1 (checker gaps)\n"
     << "mutation score: raw " << r.killed << "/" << r.planted << " = " << pct(r.score_raw) << "; at most " << pct(r.score_upper)
     << " if every infected-but-never-propagated survivor were equivalent (none is proven equivalent)\n"
     << "activation filter: " << r.pairs_skipped << " of " << r.pairs_total << " mutant runs skipped (" << pct(r.pairs_total ? static_cast<double>(r.pairs_skipped) / static_cast<double>(r.pairs_total) : 0)
     << "); soundness violations: " << r.propagated_without_infection << (r.pairs_skipped == 0 ? " (every pair was simulated, so this is a full check)" : " (checked only on the pairs that were simulated)") << "\n";
  os << "by category:   ";
  for (const auto& row : r.by_category) os << " " << row.category << " " << row.killed << "/" << row.planted << ",";
  os << "\n";
  return os.str();
}

std::string drill_json(const DrillReport& r) {
  std::ostringstream os;
  // The two field names alone take most of 64 bytes.  Leave room for both
  // formatted doubles so a realistic report cannot silently truncate JSON.
  char score[128];
  std::snprintf(score, sizeof score, "\"mutation_score_raw\": %.6g, \"mutation_score_upper_bound\": %.6g", r.score_raw, r.score_upper);
  os << "{\"planted\": " << r.planted << ", \"programs\": " << r.programs << ", \"infected\": " << r.infected_any << ", \"propagated\": " << r.propagated_any
     << ", \"killed\": " << r.killed << ", \"stop_at\": " << (r.stop_at == 0 ? std::string("null") : std::to_string(r.stop_at)) << ", \"killed_by_stop\": "
     << (r.stop_at == 0 ? std::string("null") : std::to_string(r.killed_by_stop)) << ", \"survivors\": {\"never_infected\": " << r.never_infected
     << ", \"infected_not_propagated\": " << r.infected_not_propagated << ", \"propagated_not_detected\": " << r.propagated_not_detected << "}, " << score
     << ", \"filter\": {\"pairs_total\": " << r.pairs_total << ", \"pairs_skipped\": " << r.pairs_skipped << ", \"propagated_without_infection\": "
     << r.propagated_without_infection << ", \"detected_without_propagation\": " << r.detected_without_propagation << "}, \"by_category\": {";
  for (std::size_t i = 0; i < r.by_category.size(); ++i)
    os << (i ? ", " : "") << "\"" << r.by_category[i].category << "\": {\"planted\": " << r.by_category[i].planted << ", \"killed\": " << r.by_category[i].killed << "}";
  os << "}}";
  return os.str();
}

}  // namespace cutoff::engine
