#include "rv32i.hpp"

#include "bits.hpp"

namespace cutoff {

namespace {

constexpr std::int32_t sext_i32(std::uint32_t value, unsigned bits) { return static_cast<std::int32_t>(sext(value, bits)); }

Decoded make(Op op, std::uint32_t insn, std::int32_t imm) {
  return {op, static_cast<std::uint8_t>((insn >> 7) & 0x1fu), static_cast<std::uint8_t>((insn >> 15) & 0x1fu),
          static_cast<std::uint8_t>((insn >> 20) & 0x1fu), imm};
}

}  // namespace

const char* op_name(Op op) {
  static constexpr const char* kNames[kNumOps] = {
      "lui",  "auipc", "jal",  "jalr", "beq", "bne",  "blt", "bge", "bltu", "bgeu", "lb",   "lh",    "lw",
      "lbu",  "lhu",   "sb",   "sh",   "sw",  "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli",
      "srai", "add",   "sub",  "sll",  "slt", "sltu", "xor",  "srl", "sra", "or",   "and",  "fence", "ecall",
      "ebreak", "illegal"};
  return kNames[static_cast<std::size_t>(op)];
}

Decoded decode(std::uint32_t insn) {
  const Decoded illegal{};
  if ((insn & 3u) != 3u) return illegal;  // not a 32-bit instruction (compressed is not supported)

  const std::uint32_t opcode = insn & 0x7fu;
  const std::uint32_t f3 = (insn >> 12) & 7u;
  const std::uint32_t f7 = insn >> 25;
  const std::int32_t imm_i = sext_i32(insn >> 20, 12);

  switch (opcode) {
    case 0x37: return make(Op::LUI, insn, static_cast<std::int32_t>(insn & 0xfffff000u));
    case 0x17: return make(Op::AUIPC, insn, static_cast<std::int32_t>(insn & 0xfffff000u));
    case 0x6f: {
      const std::uint32_t u = (((insn >> 31) & 1u) << 20) | (((insn >> 12) & 0xffu) << 12) | (((insn >> 20) & 1u) << 11) |
                              (((insn >> 21) & 0x3ffu) << 1);
      return make(Op::JAL, insn, sext_i32(u, 21));
    }
    case 0x67: return f3 == 0 ? make(Op::JALR, insn, imm_i) : illegal;
    case 0x63: {
      static constexpr Op kBranch[8] = {Op::BEQ, Op::BNE, Op::Illegal, Op::Illegal, Op::BLT, Op::BGE, Op::BLTU, Op::BGEU};
      if (kBranch[f3] == Op::Illegal) return illegal;
      const std::uint32_t u = (((insn >> 31) & 1u) << 12) | (((insn >> 7) & 1u) << 11) | (((insn >> 25) & 0x3fu) << 5) |
                              (((insn >> 8) & 0xfu) << 1);
      return make(kBranch[f3], insn, sext_i32(u, 13));
    }
    case 0x03: {
      static constexpr Op kLoad[8] = {Op::LB, Op::LH, Op::LW, Op::Illegal, Op::LBU, Op::LHU, Op::Illegal, Op::Illegal};
      return kLoad[f3] == Op::Illegal ? illegal : make(kLoad[f3], insn, imm_i);
    }
    case 0x23: {
      static constexpr Op kStore[8] = {Op::SB, Op::SH, Op::SW, Op::Illegal, Op::Illegal, Op::Illegal, Op::Illegal, Op::Illegal};
      if (kStore[f3] == Op::Illegal) return illegal;
      return make(kStore[f3], insn, sext_i32(((insn >> 25) << 5) | ((insn >> 7) & 0x1fu), 12));
    }
    case 0x13:
      switch (f3) {
        case 0: return make(Op::ADDI, insn, imm_i);
        case 2: return make(Op::SLTI, insn, imm_i);
        case 3: return make(Op::SLTIU, insn, imm_i);
        case 4: return make(Op::XORI, insn, imm_i);
        case 6: return make(Op::ORI, insn, imm_i);
        case 7: return make(Op::ANDI, insn, imm_i);
        case 1: return f7 == 0x00 ? make(Op::SLLI, insn, static_cast<std::int32_t>((insn >> 20) & 0x1fu)) : illegal;
        default:  // 5
          if (f7 == 0x00) return make(Op::SRLI, insn, static_cast<std::int32_t>((insn >> 20) & 0x1fu));
          if (f7 == 0x20) return make(Op::SRAI, insn, static_cast<std::int32_t>((insn >> 20) & 0x1fu));
          return illegal;
      }
    case 0x33: {
      if (f7 != 0x00 && f7 != 0x20) return illegal;
      const bool alt = f7 == 0x20;
      switch (f3) {
        case 0: return make(alt ? Op::SUB : Op::ADD, insn, 0);
        case 5: return make(alt ? Op::SRA : Op::SRL, insn, 0);
        default:
          if (alt) return illegal;  // only SUB and SRA have the alternate funct7
          static constexpr Op kAlu[8] = {Op::ADD, Op::SLL, Op::SLT, Op::SLTU, Op::XOR, Op::SRL, Op::OR, Op::AND};
          return make(kAlu[f3], insn, 0);
      }
    }
    case 0x0f: return f3 == 0 ? make(Op::FENCE, insn, 0) : illegal;  // other fields of FENCE are ignored; FENCE.I is not RV32I
    case 0x73:
      if (insn == 0x00000073u) return make(Op::ECALL, insn, 0);
      if (insn == 0x00100073u) return make(Op::EBREAK, insn, 0);
      return illegal;  // CSR instructions are outside the supported subset
    default: return illegal;
  }
}

}  // namespace cutoff
