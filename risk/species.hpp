#pragma once
// Species: the observable behaviours whose discovery the stop rule tracks (spec section 9).
//  SP-op    the RV32I instruction kinds a program retired
//  SP-cross (instruction kind, class of rs1 value, class of rs2 value) cells a program retired, so that
//           data-dependent behaviour (INT_MIN operands, shifts of zero, ...) is visible to the estimators
// A species is counted once per program however often it occurs (incidence data).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "model.hpp"

namespace cutoff::risk {

// Sets are numbered so that the wire format and checkpoints can reserve room for the ones added later
// (SP-cross+, SP-dep).
enum class SpeciesSet : std::uint8_t { Op = 0, Cross = 1 };
inline constexpr std::size_t kMaxSpeciesSets = 4;
inline constexpr std::size_t kImplementedSets = 2;

const char* set_name(SpeciesSet s);  // "op", "cross"
constexpr std::size_t universe_size(SpeciesSet s) {
  return s == SpeciesSet::Op ? kNumOps : kNumOps * 64;  // cross: kind x 8 rs1 classes x 8 rs2 classes
}

struct SpeciesConfig {
  bool op = true;
  bool cross = true;
  bool enabled(SpeciesSet s) const { return s == SpeciesSet::Op ? op : cross; }
  bool any() const { return op || cross; }
};

// Value classes for SP-cross, in priority order (a value gets the first class that fits).
enum class OperandClass : std::uint8_t { Zero, One, MinusOne, IntMin, IntMax, SmallPositive, SmallNegative, Other };
inline constexpr std::uint32_t kSmallLimit = 255;  // |v| <= 255 counts as "small", apart from 0, 1 and -1

constexpr OperandClass classify(std::uint32_t v) {
  if (v == 0) return OperandClass::Zero;
  if (v == 1) return OperandClass::One;
  if (v == 0xffffffffu) return OperandClass::MinusOne;
  if (v == 0x80000000u) return OperandClass::IntMin;
  if (v == 0x7fffffffu) return OperandClass::IntMax;
  if (v <= kSmallLimit) return OperandClass::SmallPositive;
  if (v >= 0u - kSmallLimit) return OperandClass::SmallNegative;
  return OperandClass::Other;
}
const char* class_name(OperandClass c);

// Species id of a retired instruction: SP-op uses the kind; SP-cross uses kind * 64 + rs1 class * 8 + rs2 class,
// with class 0 standing in for an operand the kind does not read.
constexpr std::uint16_t op_species(Op op) { return static_cast<std::uint16_t>(op); }
constexpr std::uint16_t cross_species(Op op, std::uint32_t rs1_value, std::uint32_t rs2_value) {
  const unsigned c1 = reads_rs1(op) ? static_cast<unsigned>(classify(rs1_value)) : 0u;
  const unsigned c2 = reads_rs2(op) ? static_cast<unsigned>(classify(rs2_value)) : 0u;
  return static_cast<std::uint16_t>(static_cast<unsigned>(op) * 64u + c1 * 8u + c2);
}

// Species ids per set, each sorted ascending and free of duplicates.
using SpeciesIds = std::array<std::vector<std::uint16_t>, kMaxSpeciesSets>;

// Species a program exercised, from the model's retire trace (ModelConfig::record_trace must be on).
SpeciesIds extract_species(std::span<const RetireRecord> retired, const SpeciesConfig& cfg);

}  // namespace cutoff::risk
