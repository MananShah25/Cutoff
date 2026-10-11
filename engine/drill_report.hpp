#pragma once
// The drill's results: per-mutant funnel table, totals, and the text and JSON the run prints and stores.

#include <cstdint>
#include <string>
#include <vector>

#include "drill_select.hpp"
#include "drill_tally.hpp"

namespace cutoff::engine {

struct DrillReport {
  std::size_t planted = 0, killed = 0, never_infected = 0, infected_not_propagated = 0, propagated_not_detected = 0;
  std::size_t infected_any = 0, propagated_any = 0;
  std::uint64_t programs = 0;
  std::uint64_t stop_at = 0;               // programs at which the stop rule first fired; 0 = it did not
  std::size_t killed_by_stop = 0;          // killed on a program before the stop point (meaningful only if stop_at > 0)
  std::uint64_t pairs_total = 0, pairs_skipped = 0;
  std::uint64_t propagated_without_infection = 0, detected_without_propagation = 0;
  double score_raw = 0;                    // killed / planted
  double score_upper = 0;                  // killed / (planted - infected_not_propagated): if every masked survivor were equivalent
  struct Row { std::string category; std::size_t planted = 0, killed = 0; };
  std::vector<Row> by_category;
};

DrillReport make_drill_report(const DrillTally& tally, const std::vector<MutantInfo>& infos, std::uint64_t stop_at);

// drill.tsv: one row per selected mutant.
std::string drill_tsv(const DrillTally& tally, const std::vector<MutantInfo>& infos, std::uint64_t stop_at);
// Human-readable block for the terminal.
std::string drill_text(const DrillReport& r);
// A JSON object (no trailing comma) for summary.json.
std::string drill_json(const DrillReport& r);

}  // namespace cutoff::engine
