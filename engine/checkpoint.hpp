#pragma once
// checkpoint.bin: everything needed to continue a run exactly where it stopped. Written (atomically) at every stop-rule
// checkpoint, when a run is interrupted and when it ends, so a finished run can also be resumed with a larger budget.

#include <cstdint>
#include <string>
#include <vector>

#include "totals.hpp"

namespace cutoff::engine {

struct SetState {
  std::uint8_t set = 0;          // risk::SpeciesSet
  std::uint64_t last_new_n = 0;
  std::vector<std::uint32_t> per_species;
};

struct Checkpoint {
  std::uint64_t stream_hash = 0;       // see stream_hash(): refuses to resume a different program stream
  std::uint64_t next_program_id = 0;   // programs 0 .. next_program_id-1 are done
  std::uint64_t events_bytes = 0;      // length of events.bin that belongs to them
  std::uint64_t curve_bytes = 0;       // length of risk_curve.csv that belongs to them
  std::uint32_t stop_consecutive = 0;  // stop-rule state
  std::uint64_t plateau_stop_at = 0;   // baseline: first checkpoint where every set had been quiet for the window (0 = not yet)
  double elapsed_seconds = 0;          // wall time used by all sessions so far
  std::uint32_t sessions = 1;          // how many times this run has been started (1 + resumes)
  Totals totals;
  std::vector<SetState> sets;
  // drill runs only
  bool has_drill = false;
  std::uint64_t drill_stop_at = 0;               // program count at which the stop rule first fired (0 = not yet)
  std::vector<int> drill_ids;                    // the selected mutants
  std::vector<std::uint64_t> drill_words;        // DrillTally::to_words()
};

// Writes to <path>.tmp and renames, so a crash never leaves a half-written checkpoint.
bool write_checkpoint(const std::string& path, const Checkpoint& cp, std::string& error);
// error is set (and false returned) for a missing, truncated, corrupt or wrong-version file.
bool read_checkpoint(const std::string& path, Checkpoint& cp, std::string& error);

}  // namespace cutoff::engine
