#pragma once
// RV32I instruction kinds, a decoder, and an encoder. The decoder follows the unprivileged
// spec's base opcode map strictly: any encoding outside the supported set decodes to Illegal.

#include <array>
#include <cstdint>
#include <cstdlib>

#include "bits.hpp"

namespace cutoff {

enum class Op : std::uint8_t {
  LUI, AUIPC, JAL, JALR,
  BEQ, BNE, BLT, BGE, BLTU, BGEU,
  LB, LH, LW, LBU, LHU,
  SB, SH, SW,
  ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI,
  ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND,
  FENCE, ECALL, EBREAK,
  Illegal,
};
inline constexpr std::size_t kNumOps = static_cast<std::size_t>(Op::Illegal) + 1;

const char* op_name(Op op);

struct Decoded {
  Op op = Op::Illegal;
  std::uint8_t rd = 0, rs1 = 0, rs2 = 0;
  std::int32_t imm = 0;  // sign comes from instruction bit 31; U-type holds the value already shifted left 12
};

Decoded decode(std::uint32_t insn);

// Encoder. Out-of-range immediates abort (also in release builds): a bad encoding must never be silent.
namespace rv {

namespace detail {
constexpr void require(bool ok) {
  if (!ok) std::abort();
}
constexpr std::uint32_t reg(unsigned r) {
  require(r < 32);
  return r;
}
constexpr std::uint32_t r_type(unsigned f7, unsigned rs2, unsigned rs1, unsigned f3, unsigned rd, unsigned opc) {
  return (f7 << 25) | (reg(rs2) << 20) | (reg(rs1) << 15) | (f3 << 12) | (reg(rd) << 7) | opc;
}
constexpr std::uint32_t i_type(std::int32_t imm, unsigned rs1, unsigned f3, unsigned rd, unsigned opc) {
  require(imm >= -2048 && imm <= 2047);
  return ((static_cast<std::uint32_t>(imm) & 0xfffu) << 20) | (reg(rs1) << 15) | (f3 << 12) | (reg(rd) << 7) | opc;
}
constexpr std::uint32_t s_type(std::int32_t imm, unsigned rs2, unsigned rs1, unsigned f3) {
  require(imm >= -2048 && imm <= 2047);
  const std::uint32_t u = static_cast<std::uint32_t>(imm) & 0xfffu;
  return ((u >> 5) << 25) | (reg(rs2) << 20) | (reg(rs1) << 15) | (f3 << 12) | ((u & 0x1fu) << 7) | 0x23;
}
constexpr std::uint32_t b_type(std::int32_t off, unsigned rs2, unsigned rs1, unsigned f3) {
  require(off >= -4096 && off <= 4094 && off % 2 == 0);
  const std::uint32_t u = static_cast<std::uint32_t>(off) & 0x1fffu;
  return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3fu) << 25) | (reg(rs2) << 20) | (reg(rs1) << 15) | (f3 << 12) |
         (((u >> 1) & 0xfu) << 8) | (((u >> 11) & 1u) << 7) | 0x63;
}
constexpr std::uint32_t u_type(std::uint32_t imm20, unsigned rd, unsigned opc) {
  require(imm20 < (1u << 20));
  return (imm20 << 12) | (reg(rd) << 7) | opc;
}
}  // namespace detail

constexpr std::uint32_t lui(unsigned rd, std::uint32_t imm20) { return detail::u_type(imm20, rd, 0x37); }
constexpr std::uint32_t auipc(unsigned rd, std::uint32_t imm20) { return detail::u_type(imm20, rd, 0x17); }
constexpr std::uint32_t jal(unsigned rd, std::int32_t off) {
  detail::require(off >= -1048576 && off <= 1048574 && off % 2 == 0);
  const std::uint32_t u = static_cast<std::uint32_t>(off) & 0x1fffffu;
  return (((u >> 20) & 1u) << 31) | (((u >> 1) & 0x3ffu) << 21) | (((u >> 11) & 1u) << 20) | (((u >> 12) & 0xffu) << 12) |
         (detail::reg(rd) << 7) | 0x6f;
}
constexpr std::uint32_t jalr(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 0, rd, 0x67); }

constexpr std::uint32_t beq(unsigned rs1, unsigned rs2, std::int32_t off) { return detail::b_type(off, rs2, rs1, 0); }
constexpr std::uint32_t bne(unsigned rs1, unsigned rs2, std::int32_t off) { return detail::b_type(off, rs2, rs1, 1); }
constexpr std::uint32_t blt(unsigned rs1, unsigned rs2, std::int32_t off) { return detail::b_type(off, rs2, rs1, 4); }
constexpr std::uint32_t bge(unsigned rs1, unsigned rs2, std::int32_t off) { return detail::b_type(off, rs2, rs1, 5); }
constexpr std::uint32_t bltu(unsigned rs1, unsigned rs2, std::int32_t off) { return detail::b_type(off, rs2, rs1, 6); }
constexpr std::uint32_t bgeu(unsigned rs1, unsigned rs2, std::int32_t off) { return detail::b_type(off, rs2, rs1, 7); }

constexpr std::uint32_t lb(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 0, rd, 0x03); }
constexpr std::uint32_t lh(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 1, rd, 0x03); }
constexpr std::uint32_t lw(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 2, rd, 0x03); }
constexpr std::uint32_t lbu(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 4, rd, 0x03); }
constexpr std::uint32_t lhu(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 5, rd, 0x03); }
constexpr std::uint32_t sb(unsigned rs2, unsigned rs1, std::int32_t imm) { return detail::s_type(imm, rs2, rs1, 0); }
constexpr std::uint32_t sh(unsigned rs2, unsigned rs1, std::int32_t imm) { return detail::s_type(imm, rs2, rs1, 1); }
constexpr std::uint32_t sw(unsigned rs2, unsigned rs1, std::int32_t imm) { return detail::s_type(imm, rs2, rs1, 2); }

constexpr std::uint32_t addi(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 0, rd, 0x13); }
constexpr std::uint32_t slti(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 2, rd, 0x13); }
constexpr std::uint32_t sltiu(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 3, rd, 0x13); }
constexpr std::uint32_t xori(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 4, rd, 0x13); }
constexpr std::uint32_t ori(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 6, rd, 0x13); }
constexpr std::uint32_t andi(unsigned rd, unsigned rs1, std::int32_t imm) { return detail::i_type(imm, rs1, 7, rd, 0x13); }
constexpr std::uint32_t slli(unsigned rd, unsigned rs1, unsigned shamt) {
  detail::require(shamt < 32);
  return detail::r_type(0x00, shamt, rs1, 1, rd, 0x13);
}
constexpr std::uint32_t srli(unsigned rd, unsigned rs1, unsigned shamt) {
  detail::require(shamt < 32);
  return detail::r_type(0x00, shamt, rs1, 5, rd, 0x13);
}
constexpr std::uint32_t srai(unsigned rd, unsigned rs1, unsigned shamt) {
  detail::require(shamt < 32);
  return detail::r_type(0x20, shamt, rs1, 5, rd, 0x13);
}

constexpr std::uint32_t add(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x00, rs2, rs1, 0, rd, 0x33); }
constexpr std::uint32_t sub(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x20, rs2, rs1, 0, rd, 0x33); }
constexpr std::uint32_t sll(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x00, rs2, rs1, 1, rd, 0x33); }
constexpr std::uint32_t slt(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x00, rs2, rs1, 2, rd, 0x33); }
constexpr std::uint32_t sltu(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x00, rs2, rs1, 3, rd, 0x33); }
constexpr std::uint32_t xor_(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x00, rs2, rs1, 4, rd, 0x33); }
constexpr std::uint32_t srl(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x00, rs2, rs1, 5, rd, 0x33); }
constexpr std::uint32_t sra(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x20, rs2, rs1, 5, rd, 0x33); }
constexpr std::uint32_t or_(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x00, rs2, rs1, 6, rd, 0x33); }
constexpr std::uint32_t and_(unsigned rd, unsigned rs1, unsigned rs2) { return detail::r_type(0x00, rs2, rs1, 7, rd, 0x33); }

constexpr std::uint32_t fence() { return 0x0ff0000f; }  // fence iorw, iorw
constexpr std::uint32_t ecall() { return 0x00000073; }
constexpr std::uint32_t ebreak() { return 0x00100073; }
constexpr std::uint32_t nop() { return addi(0, 0, 0); }

// Loads any 32-bit constant into rd with exactly two instructions (lui, addi).
constexpr std::array<std::uint32_t, 2> li(unsigned rd, std::uint32_t value) {
  const std::uint32_t hi = (value + 0x800u) >> 12;
  const std::int32_t lo = static_cast<std::int32_t>(sext(value, 12));
  return {lui(rd, hi & 0xfffffu), addi(rd, rd, lo)};
}

}  // namespace rv
}  // namespace cutoff
