#include "risk_monitor.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace cutoff::engine {

namespace {

constexpr const char* kCurveHeader =
    "n,set,species_seen,q1,q2,v,good_turing,incidence,coverage,chao2,chao2_bias_corrected,programs_since_new\n";

}  // namespace

RiskMonitor::RiskMonitor(const CampaignConfig& cfg) : cfg_(cfg), rule_(cfg.stop) {
  for (const risk::SpeciesSet s : {risk::SpeciesSet::Op, risk::SpeciesSet::Cross})
    if (cfg.species.enabled(s)) trackers_.push_back({s, risk::SpeciesTracker(risk::universe_size(s))});
}

bool RiskMonitor::open_curve(const std::string& path, bool resume, std::uint64_t keep_bytes, std::string& error) {
  if (resume) {
    std::error_code ec;
    const auto size = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
    if (size < keep_bytes) {
      error = path + " is shorter than the checkpoint says; the run folder was modified and cannot be resumed.";
      return false;
    }
    std::filesystem::resize_file(path, keep_bytes, ec);
    if (ec) { error = "cannot truncate " + path + ": " + ec.message(); return false; }
    curve_.open(path, std::ios::binary | std::ios::app);
  } else {
    curve_.open(path, std::ios::binary | std::ios::trunc);
    curve_ << kCurveHeader;
  }
  if (!curve_) {
    error = "cannot write " + path + ". Check free disk space and permissions.";
    return false;
  }
  return true;
}

void RiskMonitor::add_program(const risk::SpeciesIds& species) {
  ++n_;
  for (Tracked& t : trackers_) t.tracker.add_program(species[static_cast<std::size_t>(t.set)]);
}

double RiskMonitor::current_risk() const {
  double worst = 0.0;
  for (const Tracked& t : trackers_) worst = std::max(worst, risk::risk_value(t.tracker.counts(), cfg_.stop.estimator));
  return trackers_.empty() ? 1.0 : worst;
}

double RiskMonitor::evaluate() {
  bool all_quiet = !trackers_.empty();
  for (const Tracked& t : trackers_) {
    const risk::Counts& c = t.tracker.counts();
    char line[320];
    std::snprintf(line, sizeof line, "%llu,%s,%llu,%llu,%llu,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%llu\n", static_cast<unsigned long long>(c.n),
                  risk::set_name(t.set), static_cast<unsigned long long>(c.s), static_cast<unsigned long long>(c.q1),
                  static_cast<unsigned long long>(c.q2), static_cast<unsigned long long>(c.v), risk::good_turing(c), risk::incidence_rate(c),
                  risk::coverage_risk(c), risk::chao2_classic(c), risk::chao2_bias_corrected(c),
                  static_cast<unsigned long long>(t.tracker.programs_since_new()));
    curve_ << line;
    if (t.tracker.programs_since_new() < cfg_.stop.plateau_window) all_quiet = false;
  }
  if (plateau_stop_at_ == 0 && all_quiet) plateau_stop_at_ = n_;
  const double r = current_risk();
  rule_.on_checkpoint(n_, r);
  return r;
}

std::uint64_t RiskMonitor::curve_bytes() {
  curve_.flush();
  return static_cast<std::uint64_t>(curve_.tellp());
}

std::vector<SetSummary> RiskMonitor::summary() const {
  std::vector<SetSummary> out;
  for (const Tracked& t : trackers_) {
    const risk::Counts& c = t.tracker.counts();
    SetSummary s;
    s.name = risk::set_name(t.set);
    s.universe = risk::universe_size(t.set);
    s.counts = c;
    s.good_turing = risk::good_turing(c);
    s.incidence = risk::incidence_rate(c);
    s.coverage = risk::coverage_risk(c);
    s.chao2 = risk::chao2_classic(c);
    s.chao2_bias_corrected = risk::chao2_bias_corrected(c);
    s.programs_since_new = t.tracker.programs_since_new();
    out.push_back(s);
  }
  return out;
}

void RiskMonitor::export_state(Checkpoint& cp) const {
  cp.stop_consecutive = rule_.consecutive();
  cp.plateau_stop_at = plateau_stop_at_;
  cp.sets.clear();
  for (const Tracked& t : trackers_) cp.sets.push_back({static_cast<std::uint8_t>(t.set), t.tracker.last_new_n(), t.tracker.per_species()});
}

bool RiskMonitor::import_state(const Checkpoint& cp, std::string& error) {
  if (cp.sets.size() != trackers_.size()) {
    error = "the checkpoint tracks a different set of species sets than the current settings (species.op / species.cross).";
    return false;
  }
  for (std::size_t i = 0; i < trackers_.size(); ++i) {
    const SetState& s = cp.sets[i];
    if (s.set != static_cast<std::uint8_t>(trackers_[i].set) || s.per_species.size() != risk::universe_size(trackers_[i].set)) {
      error = "the checkpoint's species data does not match the current species definitions.";
      return false;
    }
    trackers_[i].tracker = risk::SpeciesTracker::restore(s.per_species, cp.next_program_id, s.last_new_n);
  }
  n_ = cp.next_program_id;
  plateau_stop_at_ = cp.plateau_stop_at;
  rule_.restore(cp.stop_consecutive);
  return true;
}

}  // namespace cutoff::engine
