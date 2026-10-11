#pragma once
// Stateless, seeded random-program generators (profile G0 so far).
//
// A generated program is a pure function of (kGeneratorVersion, GenConfig, seed): it never looks at the
// outcome of any earlier program. The risk estimators in risk/ rely on that independence.
//
// Layout of every program (word indices):
//   [0, 62)            init block: x1..x30 = random 32-bit values, x31 = data base register
//   [62, 62 + L)       body of L instructions (L drawn from [min_len, max_len])
//   [62 + L, 62 + L+3) epilogue: x31 = magic address; sw x1, 0(x31) ends the run
// Guarantees (each has a test):
//   - terminates: every branch/jump goes strictly forward and never past the epilogue start;
//     the first epilogue instruction is the furthest legal target, and no target lands in the middle of
//     an AUIPC/JALR pair or inside the epilogue;
//   - memory accesses are aligned and use x31 (never written after init) plus an immediate, so they stay inside
//     [kDataBase, kDataBase + kDataBytes), far from the code, so programs never modify themselves;
//   - only RV32I instructions the core and model agree on: no ECALL, EBREAK, CSR or illegal words.

#include <cstdint>
#include <string>
#include <vector>

#include "rng.hpp"

namespace cutoff::gen {

// Bump whenever any (config, seed) -> program mapping changes, and update the golden hashes in tests/gen_test.cpp.
inline constexpr std::uint32_t kGeneratorVersion = 1;

inline constexpr std::uint32_t kDataBase = 0x8000;
inline constexpr std::uint32_t kDataBytes = 0x1000;
inline constexpr unsigned kDataBaseReg = 31;  // holds kDataBase + 0x800 so signed 12-bit offsets cover the window
inline constexpr std::uint32_t kDataBaseRegValue = kDataBase + 0x800;
inline constexpr std::uint32_t kInitWords = 62;  // 31 registers, two instructions each
inline constexpr std::uint32_t kEpilogueWords = 3;
inline constexpr std::uint32_t kMaxCodeWords = kDataBase / 4;  // code must stay below the data window

enum class Profile : std::uint8_t { G0, G1, G2, G3 };

struct GenConfig {
  Profile profile = Profile::G0;
  std::uint32_t min_len = 32;   // body length L is uniform in [min_len, max_len]
  std::uint32_t max_len = 128;
  std::uint32_t max_skip = 8;   // forward control flow prefers targets at most this many slots ahead
};

struct Program {
  std::vector<std::uint32_t> code;
  std::uint32_t body_begin = 0;  // word index of the first body instruction
  std::uint32_t body_end = 0;    // word index of the first epilogue instruction
  std::uint64_t seed = 0;
};

// Empty string if the config is usable, otherwise a message saying what to change.
std::string validate(const GenConfig& cfg);

// Throws std::invalid_argument (with the validate() message) for an unusable config.
Program generate(const GenConfig& cfg, std::uint64_t seed);

// FNV-1a over the code words; used to pin generator output and to name programs in reports.
std::uint64_t program_hash(const Program& p);

}  // namespace cutoff::gen
