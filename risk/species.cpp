#include "species.hpp"

#include <bitset>

namespace cutoff::risk {

const char* set_name(SpeciesSet s) { return s == SpeciesSet::Op ? "op" : "cross"; }

const char* class_name(OperandClass c) {
  static constexpr const char* kNames[8] = {"zero", "one", "minus-one", "int-min", "int-max", "small-positive", "small-negative", "other"};
  return kNames[static_cast<unsigned>(c)];
}

SpeciesIds extract_species(std::span<const RetireRecord> retired, const SpeciesConfig& cfg) {
  std::bitset<universe_size(SpeciesSet::Op)> seen_op;
  std::bitset<universe_size(SpeciesSet::Cross)> seen_cross;
  for (const RetireRecord& r : retired) {
    const Op op = decode(r.insn).op;
    if (cfg.op) seen_op.set(op_species(op));
    if (cfg.cross) seen_cross.set(cross_species(op, r.rs1_value, r.rs2_value));
  }
  SpeciesIds out;
  if (cfg.op)
    for (std::size_t i = 0; i < seen_op.size(); ++i)
      if (seen_op.test(i)) out[static_cast<std::size_t>(SpeciesSet::Op)].push_back(static_cast<std::uint16_t>(i));
  if (cfg.cross)
    for (std::size_t i = 0; i < seen_cross.size(); ++i)
      if (seen_cross.test(i)) out[static_cast<std::size_t>(SpeciesSet::Cross)].push_back(static_cast<std::uint16_t>(i));
  return out;
}

}  // namespace cutoff::risk
