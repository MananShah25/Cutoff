#pragma once
// What the drill records for one (program, mutant) pair, one byte per selected mutant.

#include <cstdint>

namespace cutoff::engine {

inline constexpr std::uint8_t kLevelInfected = 1;    // the golden run reached the mutated expression and the mutation would have changed its value
inline constexpr std::uint8_t kLevelRan = 2;         // the mutant was actually simulated on this program (the activation filter skips the others)
inline constexpr std::uint8_t kLevelPropagated = 4;  // the mutant run differs from the golden run: final state, status, exit code or the untimed bus sequence
inline constexpr std::uint8_t kLevelDetected = 8;    // checker C1 flags the mutant run (compared with the reference model)

}  // namespace cutoff::engine
