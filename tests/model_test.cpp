// Reference-model tests. Every expected value below is hand-computed from the RV32I spec;
// none is produced by the code under test.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "bits.hpp"
#include "model.hpp"
#include "rv32i.hpp"
#include "smoke_programs.hpp"

using namespace cutoff;

namespace {

constexpr std::uint32_t kMin = 0x80000000u;  // INT_MIN
constexpr std::uint32_t kMax = 0x7fffffffu;  // INT_MAX
constexpr std::uint32_t kM1 = 0xffffffffu;   // -1

using RR = std::uint32_t (*)(unsigned, unsigned, unsigned);
using RI = std::uint32_t (*)(unsigned, unsigned, std::int32_t);
using Br = std::uint32_t (*)(unsigned, unsigned, std::int32_t);

// Executes one instruction at `pc` with x1 = a, x2 = b and returns the model.
Model one(std::uint32_t insn, std::uint32_t a, std::uint32_t b, std::uint32_t pc = 0x100) {
  Model m;
  m.write_word(pc, insn);
  m.set_reg(1, a);
  m.set_reg(2, b);
  m.set_pc(pc);
  m.step();
  return m;
}

struct RRCase { std::uint32_t a, b, expect; };

void check_rr(const char* name, RR enc, const std::vector<RRCase>& cases) {
  for (const auto& c : cases) {
    const Model m = one(enc(3, 1, 2), c.a, c.b);
    EXPECT_EQ(m.status(), Status::Running) << name;
    EXPECT_EQ(m.reg(3), c.expect) << name << std::hex << " a=0x" << c.a << " b=0x" << c.b;
    EXPECT_EQ(m.pc(), 0x104u) << name;
  }
}

struct RICase { std::uint32_t a; std::int32_t imm; std::uint32_t expect; };

void check_ri(const char* name, RI enc, const std::vector<RICase>& cases) {
  for (const auto& c : cases) {
    const Model m = one(enc(3, 1, c.imm), c.a, 0);
    EXPECT_EQ(m.reg(3), c.expect) << name << std::hex << " a=0x" << c.a << " imm=" << std::dec << c.imm;
  }
}

}  // namespace

// ---------------------------------------------------------------- encoder / decoder

TEST(Encoder, KnownAnswerWords) {
  // Words assembled field by field with an independent script (not with this encoder).
  struct KA { std::uint32_t expect, actual; const char* what; };
  const KA table[] = {
      {0x00000013, rv::nop(), "nop"},
      {0x00008067, rv::jalr(0, 1, 0), "ret"},
      {0x003100b3, rv::add(1, 2, 3), "add x1,x2,x3"},
      {0x403100b3, rv::sub(1, 2, 3), "sub x1,x2,x3"},
      {0x407352b3, rv::sra(5, 6, 7), "sra x5,x6,x7"},
      {0x00412083, rv::lw(1, 2, 4), "lw x1,4(x2)"},
      {0xfff54483, rv::lbu(9, 10, -1), "lbu x9,-1(x10)"},
      {0x00112223, rv::sw(1, 2, 4), "sw x1,4(x2)"},
      {0x81ff0023, rv::sb(31, 30, -2048), "sb x31,-2048(x30)"},
      {0x00208463, rv::beq(1, 2, 8), "beq x1,x2,8"},
      {0xfe209ee3, rv::bne(1, 2, -4), "bne x1,x2,-4"},
      {0x7e41ffe3, rv::bgeu(3, 4, 4094), "bgeu x3,x4,4094"},
      {0x8062c063, rv::blt(5, 6, -4096), "blt x5,x6,-4096"},
      {0x008000ef, rv::jal(1, 8), "jal x1,8"},
      {0xffdff06f, rv::jal(0, -4), "jal x0,-4"},
      {0x7fffffef, rv::jal(31, 1048574), "jal x31,1048574"},
      {0x8000016f, rv::jal(2, -1048576), "jal x2,-1048576"},
      {0x123450b7, rv::lui(1, 0x12345), "lui x1,0x12345"},
      {0xfffff197, rv::auipc(3, 0xfffff), "auipc x3,0xfffff"},
      {0xfff10093, rv::addi(1, 2, -1), "addi x1,x2,-1"},
      {0x01f11093, rv::slli(1, 2, 31), "slli x1,x2,31"},
      {0x40315093, rv::srai(1, 2, 3), "srai x1,x2,3"},
      {0xfff24213, rv::xori(4, 4, -1), "xori x4,x4,-1"},
      {0x0ff0000f, rv::fence(), "fence"},
      {0x00000073, rv::ecall(), "ecall"},
      {0x00100073, rv::ebreak(), "ebreak"},
  };
  for (const auto& k : table) EXPECT_EQ(k.actual, k.expect) << k.what;
}

TEST(Decoder, ImmediatesTakeTheirSignFromBit31) {
  struct D { std::uint32_t word; Op op; unsigned rd, rs1, rs2; std::int32_t imm; };
  const D table[] = {
      {0xfff54483, Op::LBU, 9, 10, 31, -1},                         // I-type: rs2 field is just imm bits
      {0x81ff0023, Op::SB, 0, 30, 31, -2048},                       // S-type
      {0x7e41ffe3, Op::BGEU, 31, 3, 4, 4094},                       // B-type maximum
      {0x8062c063, Op::BLT, 0, 5, 6, -4096},                        // B-type minimum
      {0x7fffffef, Op::JAL, 31, 31, 31, 1048574},                   // J-type maximum
      {0x8000016f, Op::JAL, 2, 0, 0, -1048576},                     // J-type minimum
      {0x123450b7, Op::LUI, 1, 8, 20, 0x12345000},                  // U-type
      {0xfffff197, Op::AUIPC, 3, 30, 31, static_cast<std::int32_t>(0xfffff000u)},
      {0x01f11093, Op::SLLI, 1, 2, 31, 31},                         // shift amount, not sign-extended imm
      {0x40315093, Op::SRAI, 1, 2, 3, 3},
      {0x403100b3, Op::SUB, 1, 2, 3, 0},
  };
  for (const auto& t : table) {
    const Decoded d = decode(t.word);
    EXPECT_EQ(d.op, t.op) << std::hex << t.word;
    EXPECT_EQ(d.imm, t.imm) << std::hex << t.word;
    // Register fields are only meaningful where the format has them; in U/J words those bits are immediate.
    const bool u_or_j = t.op == Op::LUI || t.op == Op::AUIPC || t.op == Op::JAL;
    const bool has_rd = t.op != Op::SB && t.op != Op::BGEU && t.op != Op::BLT;
    if (has_rd) EXPECT_EQ(d.rd, t.rd) << std::hex << t.word;
    if (!u_or_j) EXPECT_EQ(d.rs1, t.rs1) << std::hex << t.word;
    const bool has_rs2 = !u_or_j && t.op != Op::LBU && t.op != Op::SLLI && t.op != Op::SRAI;
    if (has_rs2) EXPECT_EQ(d.rs2, t.rs2) << std::hex << t.word;
  }
}

TEST(Decoder, RejectsEverythingOutsideRv32i) {
  const std::uint32_t illegal[] = {
      0x00000000,  // all zeros
      0xffffffff,  // all ones
      0x00000001,  // low bits 01: a 16-bit compressed encoding
      0x340110f3,  // csrrw x1, mscratch, x2
      0x023100b3,  // mul x1,x2,x3 (M extension)
      0x40111093,  // slli with funct7 = 0x20
      0x403140b3,  // xor with funct7 = 0x20
      0x00009067,  // jalr with funct3 = 1
      0x00003003,  // funct3 = 3 load (ld)
      0x00003023,  // funct3 = 3 store (sd)
      0x00002063,  // branch funct3 = 2
      0x0000100f,  // fence.i (Zifencei)
      0x00200073,  // SYSTEM with imm = 2 (not ecall/ebreak)
      0x30200073,  // mret
  };
  for (const auto w : illegal) EXPECT_EQ(decode(w).op, Op::Illegal) << std::hex << w;
  EXPECT_EQ(decode(0x0ff0000f).op, Op::FENCE);
  EXPECT_EQ(decode(0x8330000f).op, Op::FENCE) << "FENCE ignores its other fields";
  EXPECT_EQ(decode(0x00000073).op, Op::ECALL);
  EXPECT_EQ(decode(0x00100073).op, Op::EBREAK);
}

TEST(Decoder, EncodeDecodeRoundTripOnRandomFields) {
  std::mt19937 rng(20261010);  // NOLINT(bugprone-random-generator-seed): fixed seed keeps the test deterministic
  auto pick = [&](std::uint32_t n) { return rng() % n; };
  for (int i = 0; i < 20000; ++i) {
    const unsigned rd = pick(32), rs1 = pick(32), rs2 = pick(32);
    const std::int32_t i12 = static_cast<std::int32_t>(pick(4096)) - 2048;
    const std::int32_t b13 = (static_cast<std::int32_t>(pick(4096)) - 2048) * 2;
    const std::int32_t j21 = (static_cast<std::int32_t>(pick(1u << 20)) - (1 << 19)) * 2;
    const unsigned sh = pick(32);

    struct C { std::uint32_t w; Op op; std::int32_t imm; bool has_rd, has_rs1, has_rs2; };
    const C cases[] = {
        {rv::addi(rd, rs1, i12), Op::ADDI, i12, true, true, false},
        {rv::sltiu(rd, rs1, i12), Op::SLTIU, i12, true, true, false},
        {rv::lh(rd, rs1, i12), Op::LH, i12, true, true, false},
        {rv::jalr(rd, rs1, i12), Op::JALR, i12, true, true, false},
        {rv::sw(rs2, rs1, i12), Op::SW, i12, false, true, true},
        {rv::beq(rs1, rs2, b13), Op::BEQ, b13, false, true, true},
        {rv::bltu(rs1, rs2, b13), Op::BLTU, b13, false, true, true},
        {rv::jal(rd, j21), Op::JAL, j21, true, false, false},
        {rv::srai(rd, rs1, sh), Op::SRAI, static_cast<std::int32_t>(sh), true, true, false},
        {rv::sra(rd, rs1, rs2), Op::SRA, 0, true, true, true},
        {rv::lui(rd, pick(1u << 20)), Op::LUI, 0, true, false, false},
    };
    for (const auto& c : cases) {
      const Decoded d = decode(c.w);
      ASSERT_EQ(d.op, c.op) << std::hex << c.w;
      if (c.op != Op::LUI) ASSERT_EQ(d.imm, c.imm) << std::hex << c.w;
      if (c.has_rd) ASSERT_EQ(d.rd, rd);
      if (c.has_rs1) ASSERT_EQ(d.rs1, rs1);
      if (c.has_rs2) ASSERT_EQ(d.rs2, rs2);
    }
  }
}

TEST(Encoder, LiLoadsAnyConstant) {
  const std::uint32_t values[] = {0, 1, 0x7ff, 0x800, 0x801, 0xfff, 0x1000, 0xfffff7ff, 0xfffff800, 0xfffff801,
                                  kMin, kMax, kM1, 0x12345678, 0xdeadbeef, 0x7ffff800, 0x80000800};
  for (const auto v : values) {
    Model m;
    const auto code = rv::li(5, v);
    m.load(code);
    m.step();
    m.step();
    EXPECT_EQ(m.reg(5), v) << std::hex << v;
  }
  std::mt19937 rng(7);  // NOLINT(bugprone-random-generator-seed): fixed seed keeps the test deterministic
  for (int i = 0; i < 1000; ++i) {
    const std::uint32_t v = rng();
    Model m;
    m.load(rv::li(31, v));
    m.step();
    m.step();
    ASSERT_EQ(m.reg(31), v) << std::hex << v;
  }
}

TEST(Bits, SignExtension) {
  EXPECT_EQ(sext(0x80, 8), 0xffffff80u);
  EXPECT_EQ(sext(0x7f, 8), 0x7fu);
  EXPECT_EQ(sext(0x1ff, 8), 0x000000ffu | 0xffffff00u) << "only the low 8 bits count";
  EXPECT_EQ(sext(0x800, 12), 0xfffff800u);
  EXPECT_EQ(sext(0x7ff, 12), 0x7ffu);
  EXPECT_EQ(sext(0xffffffff, 32), 0xffffffffu);
  EXPECT_EQ(sext(0x80000000, 32), 0x80000000u);
  EXPECT_EQ(sext(1, 1), 0xffffffffu);
  EXPECT_EQ(sext(0, 1), 0u);
}

TEST(Bits, ArithmeticShiftMatchesTheCompilersSignedShift) {
  EXPECT_EQ(sra(0x80000000, 31), 0xffffffffu);
  EXPECT_EQ(sra(0x80000000, 0), 0x80000000u);
  EXPECT_EQ(sra(0x7fffffff, 31), 0u);
  EXPECT_EQ(sra(0xf0000000, 4), 0xff000000u);
  std::mt19937 rng(99);  // NOLINT(bugprone-random-generator-seed): fixed seed keeps the test deterministic
  for (int i = 0; i < 20000; ++i) {
    const std::uint32_t a = rng();
    const unsigned s = rng() % 32;
    // C++20 defines >> on negative signed values as an arithmetic shift, so this is an independent oracle.
    const std::int32_t oracle = static_cast<std::int32_t>(a) >> s;  // NOLINT(bugprone-signed-bitwise): this is the oracle
    ASSERT_EQ(sra(a, s), static_cast<std::uint32_t>(oracle)) << std::hex << a << " >> " << s;
  }
}

// ---------------------------------------------------------------- arithmetic and logic

TEST(ModelAlu, RegisterRegister) {
  check_rr("add", rv::add, {{5, 7, 12}, {kMax, 1, kMin}, {kM1, 1, 0}, {kMin, kM1, kMax}, {0, 0, 0}});
  check_rr("sub", rv::sub, {{5, 7, 0xfffffffe}, {0, 1, kM1}, {kMin, 1, kMax}, {kMax, kM1, kMin}, {kM1, kM1, 0}});
  check_rr("sll", rv::sll, {{1, 31, kMin}, {1, 32, 1}, {kM1, 1, 0xfffffffe}, {0x12345678, 0, 0x12345678}, {3, 33, 6},
                            {1, 0xffffffe5, 32}});
  check_rr("srl", rv::srl, {{kMin, 31, 1}, {kMin, 0, kMin}, {kM1, 1, kMax}, {kM1, 32, kM1}, {0x80000001, 4, 0x08000000}});
  check_rr("sra", rv::sra, {{kMin, 31, kM1}, {kMin, 1, 0xc0000000}, {kMax, 31, 0}, {kM1, 1, kM1}, {kMin, 32, kMin},
                            {kMax, 4, 0x07ffffff}});
  check_rr("slt", rv::slt, {{kM1, 0, 1}, {0, kM1, 0}, {kMin, kMax, 1}, {kMax, kMin, 0}, {5, 5, 0}, {kMin, kMin, 0}});
  check_rr("sltu", rv::sltu, {{kM1, 0, 0}, {0, kM1, 1}, {kMin, kMax, 0}, {kMax, kMin, 1}, {5, 5, 0}});
  check_rr("xor", rv::xor_, {{0xaaaaaaaa, 0x55555555, kM1}, {kM1, kM1, 0}, {0x0f0f0f0f, 0xffff0000, 0xf0f00f0f}});
  check_rr("or", rv::or_, {{0xaaaaaaaa, 0x55555555, kM1}, {0xf0f00000, 0x0000f0f0, 0xf0f0f0f0}, {0, 0, 0}});
  check_rr("and", rv::and_, {{0xaaaaaaaa, 0x55555555, 0}, {kM1, 0x1234, 0x1234}, {0xff00ff00, 0x0ff00ff0, 0x0f000f00}});
}

TEST(ModelAlu, RegisterImmediate) {
  check_ri("addi", rv::addi, {{0, -1, kM1}, {kMax, 1, kMin}, {0, -2048, 0xfffff800}, {1, 2047, 0x800}, {5, 0, 5}, {kMin, -1, kMax}});
  check_ri("slti", rv::slti, {{kM1, 0, 1}, {0, -1, 0}, {kMin, -2048, 1}, {5, 5, 0}, {3, 4, 1}, {kMax, -1, 0}});
  // SLTIU sign-extends the immediate first, then compares unsigned.
  check_ri("sltiu", rv::sltiu, {{0, -1, 1}, {kM1, -1, 0}, {kM1, 0, 0}, {5, 6, 1}, {0, 0, 0}, {0x800, -2048, 1}});
  check_ri("xori", rv::xori, {{0x0f0f0f0f, -1, 0xf0f0f0f0}, {0x12345678, 0, 0x12345678}, {kM1, 0x7ff, 0xfffff800}});
  check_ri("ori", rv::ori, {{0, -2048, 0xfffff800}, {0x0f, 0x70, 0x7f}, {kMin, 1, 0x80000001}});
  check_ri("andi", rv::andi, {{kM1, -2048, 0xfffff800}, {kM1, 2047, 0x7ff}, {0x12345678, 255, 0x78}});
}

TEST(ModelAlu, ImmediateShifts) {
  using SH = std::uint32_t (*)(unsigned, unsigned, unsigned);
  struct C { SH enc; const char* name; std::uint32_t a; unsigned sh; std::uint32_t expect; };
  const C cases[] = {
      {rv::slli, "slli", 1, 31, kMin}, {rv::slli, "slli", 1, 0, 1}, {rv::slli, "slli", kM1, 1, 0xfffffffe},
      {rv::srli, "srli", kMin, 31, 1}, {rv::srli, "srli", kM1, 0, kM1}, {rv::srli, "srli", kM1, 1, kMax},
      {rv::srai, "srai", kMin, 31, kM1}, {rv::srai, "srai", kMin, 1, 0xc0000000}, {rv::srai, "srai", kMax, 31, 0},
      {rv::srai, "srai", kMax, 0, kMax},
  };
  for (const auto& c : cases) {
    const Model m = one(c.enc(3, 1, c.sh), c.a, 0);
    EXPECT_EQ(m.reg(3), c.expect) << c.name << " a=" << std::hex << c.a << " sh=" << std::dec << c.sh;
  }
}

TEST(ModelAlu, UpperImmediates) {
  EXPECT_EQ(one(rv::lui(3, 0x80000), 0, 0).reg(3), 0x80000000u);
  EXPECT_EQ(one(rv::lui(3, 0xfffff), 0, 0).reg(3), 0xfffff000u);
  EXPECT_EQ(one(rv::lui(3, 0), 5, 5).reg(3), 0u);
  EXPECT_EQ(one(rv::auipc(3, 1), 0, 0, 0x100).reg(3), 0x1100u);
  EXPECT_EQ(one(rv::auipc(3, 0xfffff), 0, 0, 0x100).reg(3), 0xfffff100u);  // wraps modulo 2^32 only through the add
  EXPECT_EQ(one(rv::auipc(3, 0), 0, 0, 0x100).reg(3), 0x100u);
}

TEST(ModelAlu, X0IsHardwiredToZero) {
  Model m;
  m.load(std::array<std::uint32_t, 4>{rv::addi(0, 0, 5), rv::add(3, 0, 0), rv::addi(4, 0, 7), rv::lui(0, 0x12345)});
  for (int i = 0; i < 4; ++i) m.step();
  EXPECT_EQ(m.reg(0), 0u);
  EXPECT_EQ(m.reg(3), 0u);
  EXPECT_EQ(m.reg(4), 7u);
  m.set_reg(0, 99);
  EXPECT_EQ(m.reg(0), 0u);
}

// ---------------------------------------------------------------- control flow

TEST(ModelBranch, TakenAndNotTaken) {
  struct C { const char* name; Br enc; std::uint32_t a, b; bool taken; };
  const C cases[] = {
      {"beq", rv::beq, 5, 5, true},       {"beq", rv::beq, 5, 6, false},
      {"bne", rv::bne, 5, 6, true},       {"bne", rv::bne, 5, 5, false},
      {"blt", rv::blt, kM1, 0, true},     {"blt", rv::blt, 0, kM1, false},   {"blt", rv::blt, 5, 5, false},
      {"blt", rv::blt, kMin, kMax, true},
      {"bge", rv::bge, 0, kM1, true},     {"bge", rv::bge, kM1, 0, false},   {"bge", rv::bge, 5, 5, true},
      {"bge", rv::bge, kMin, kMax, false},
      {"bltu", rv::bltu, 0, kM1, true},   {"bltu", rv::bltu, kM1, 0, false}, {"bltu", rv::bltu, 5, 5, false},
      {"bgeu", rv::bgeu, kM1, 0, true},   {"bgeu", rv::bgeu, 0, kM1, false}, {"bgeu", rv::bgeu, 5, 5, true},
  };
  for (const auto& c : cases) {
    const Model fwd = one(c.enc(1, 2, 8), c.a, c.b);
    EXPECT_EQ(fwd.status(), Status::Running) << c.name;
    EXPECT_EQ(fwd.pc(), c.taken ? 0x108u : 0x104u) << c.name << std::hex << " a=" << c.a << " b=" << c.b;
    const Model back = one(c.enc(1, 2, -8), c.a, c.b);
    EXPECT_EQ(back.pc(), c.taken ? 0xf8u : 0x104u) << c.name;
  }
}

TEST(ModelBranch, ImmediateExtremes) {
  EXPECT_EQ(one(rv::beq(1, 2, 4092), 1, 1, 0x2000).pc(), 0x2ffcu);
  EXPECT_EQ(one(rv::beq(1, 2, -4096), 1, 1, 0x2000).pc(), 0x1000u);
}

TEST(ModelBranch, MisalignedTargetTrapsOnlyWhenTaken) {
  EXPECT_EQ(one(rv::beq(1, 2, 2), 1, 1).status(), Status::Trap);
  EXPECT_EQ(one(rv::beq(1, 2, 6), 1, 1).status(), Status::Trap);
  const Model not_taken = one(rv::bne(1, 2, 2), 1, 1);
  EXPECT_EQ(not_taken.status(), Status::Running);
  EXPECT_EQ(not_taken.pc(), 0x104u);
}

TEST(ModelJump, Jal) {
  Model m = one(rv::jal(1, 8), 0, 0, 0x8000);
  EXPECT_EQ(m.pc(), 0x8008u);
  EXPECT_EQ(m.reg(1), 0x8004u);
  m = one(rv::jal(1, -4), 0, 0, 0x8000);
  EXPECT_EQ(m.pc(), 0x7ffcu);
  EXPECT_EQ(m.reg(1), 0x8004u);
  m = one(rv::jal(31, 1048572), 0, 0, 0x8000);
  EXPECT_EQ(m.pc(), 0x107ffcu);
  EXPECT_EQ(m.reg(31), 0x8004u);
  m = one(rv::jal(1, -1048576), 0, 0, 0x8000);
  EXPECT_EQ(m.pc(), 0xfff08000u);  // 0x8000 - 0x100000 modulo 2^32
  m = one(rv::jal(0, 8), 0, 0, 0x8000);
  EXPECT_EQ(m.reg(0), 0u);
  EXPECT_EQ(m.pc(), 0x8008u);
}

TEST(ModelJump, JalMisalignedTargetTrapsWithoutLinking) {
  const Model m = one(rv::jal(1, 2), 0, 0, 0x8000);
  EXPECT_EQ(m.status(), Status::Trap);
  EXPECT_EQ(m.reg(1), 0u);
}

TEST(ModelJump, JalrClearsBitZeroOfTarget) {
  Model m = one(rv::jalr(2, 1, 0), 0x1001, 0, 0x200);
  EXPECT_EQ(m.pc(), 0x1000u);
  EXPECT_EQ(m.reg(2), 0x204u);
  m = one(rv::jalr(2, 1, -4), 0x1008, 0, 0x200);
  EXPECT_EQ(m.pc(), 0x1004u);
  m = one(rv::jalr(2, 1, 1), 0x1003, 0, 0x200);  // 0x1003 + 1 = 0x1004
  EXPECT_EQ(m.status(), Status::Running);
  EXPECT_EQ(m.pc(), 0x1004u);
  m = one(rv::jalr(2, 1, 5), kM1, 0, 0x200);  // wraps to 4
  EXPECT_EQ(m.pc(), 4u);
}

TEST(ModelJump, JalrWithRdEqualRs1UsesTheOldValue) {
  const Model m = one(rv::jalr(1, 1, 0), 0x2001, 0, 0x200);
  EXPECT_EQ(m.pc(), 0x2000u);
  EXPECT_EQ(m.reg(1), 0x204u);
}

TEST(ModelJump, JalrMisalignedTargetTraps) {
  EXPECT_EQ(one(rv::jalr(2, 1, 2), 0x1000, 0).status(), Status::Trap);   // 0x1002
  EXPECT_EQ(one(rv::jalr(2, 1, 0), 0x1002, 0).status(), Status::Trap);
  EXPECT_EQ(one(rv::jalr(2, 1, 1), 0x1002, 0).status(), Status::Trap);   // 0x1003 -> 0x1002
  EXPECT_EQ(one(rv::jalr(2, 1, 3), 0x1000, 0).status(), Status::Trap);   // 0x1003 -> 0x1002
}

// ---------------------------------------------------------------- memory

namespace {

Model with_words(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> words) {
  Model m;
  for (const auto& [addr, value] : words) m.write_word(addr, value);
  return m;
}

}  // namespace

TEST(ModelMemory, LoadsExtendCorrectlyAndUseLittleEndianLanes) {
  // 0x1000: bytes 80 01 fe 7f   0x1004: bytes 80 ff 01 80
  struct C { std::uint32_t (*enc)(unsigned, unsigned, std::int32_t); const char* name; std::int32_t off; std::uint32_t expect; };
  const C cases[] = {
      {rv::lw, "lw", 0, 0x7ffe0180},   {rv::lw, "lw", 4, 0x8001ff80},
      {rv::lh, "lh", 0, 0x00000180},   {rv::lh, "lh", 2, 0x00007ffe},
      {rv::lh, "lh", 4, 0xffffff80},   {rv::lh, "lh", 6, 0xffff8001},
      {rv::lhu, "lhu", 4, 0x0000ff80}, {rv::lhu, "lhu", 6, 0x00008001},
      {rv::lb, "lb", 0, 0xffffff80},   {rv::lb, "lb", 1, 0x00000001},
      {rv::lb, "lb", 3, 0x0000007f},   {rv::lb, "lb", 5, 0xffffffff},
      {rv::lb, "lb", 6, 0x00000001},   {rv::lb, "lb", 7, 0xffffff80},
      {rv::lbu, "lbu", 0, 0x00000080}, {rv::lbu, "lbu", 5, 0x000000ff},
      {rv::lbu, "lbu", 7, 0x00000080},
  };
  for (const auto& c : cases) {
    Model m = with_words({{0x1000, 0x7ffe0180}, {0x1004, 0x8001ff80}, {0x100, c.enc(3, 1, c.off)}});
    m.set_reg(1, 0x1000);
    m.set_pc(0x100);
    m.step();
    EXPECT_EQ(m.status(), Status::Running) << c.name;
    EXPECT_EQ(m.reg(3), c.expect) << c.name << " off=" << c.off;
  }
}

TEST(ModelMemory, NegativeOffsetAndX0Destination) {
  Model m = with_words({{0x1004, 0x8001ff80}, {0x100, rv::lw(3, 1, -4)}, {0x104, rv::lw(0, 1, -4)}});
  m.set_reg(1, 0x1008);
  m.set_pc(0x100);
  m.step();
  m.step();
  EXPECT_EQ(m.reg(3), 0x8001ff80u);
  EXPECT_EQ(m.reg(0), 0u);
}

TEST(ModelMemory, StoresWriteOnlyTheirLanes) {
  struct C { std::uint32_t (*enc)(unsigned, unsigned, std::int32_t); const char* name; std::int32_t off; std::uint32_t expect; };
  const C cases[] = {
      {rv::sw, "sw", 0, 0xaabbccdd}, {rv::sh, "sh", 0, 0x1122ccdd}, {rv::sh, "sh", 2, 0xccdd3344},
      {rv::sb, "sb", 0, 0x112233dd}, {rv::sb, "sb", 1, 0x1122dd44}, {rv::sb, "sb", 2, 0x11dd3344},
      {rv::sb, "sb", 3, 0xdd223344},
  };
  for (const auto& c : cases) {
    Model m = with_words({{0x1000, 0x11223344}, {0x100, c.enc(2, 1, c.off)}});
    m.set_reg(1, 0x1000);
    m.set_reg(2, 0xaabbccdd);
    m.set_pc(0x100);
    m.step();
    EXPECT_EQ(m.status(), Status::Running) << c.name;
    EXPECT_EQ(m.read_word(0x1000), c.expect) << c.name << " off=" << c.off;
  }
}

TEST(ModelMemory, MisalignedAccessesTrapButBytesNeverDo) {
  struct C { std::uint32_t insn; std::uint32_t base; Status expect; const char* name; };
  const C cases[] = {
      {rv::lw(3, 1, 0), 0x1001, Status::Trap, "lw +1"},   {rv::lw(3, 1, 0), 0x1002, Status::Trap, "lw +2"},
      {rv::lw(3, 1, 0), 0x1003, Status::Trap, "lw +3"},   {rv::lh(3, 1, 0), 0x1001, Status::Trap, "lh +1"},
      {rv::lh(3, 1, 0), 0x1003, Status::Trap, "lh +3"},   {rv::lhu(3, 1, 0), 0x1003, Status::Trap, "lhu +3"},
      {rv::sw(2, 1, 0), 0x1002, Status::Trap, "sw +2"},   {rv::sh(2, 1, 0), 0x1001, Status::Trap, "sh +1"},
      {rv::lb(3, 1, 0), 0x1001, Status::Running, "lb +1"}, {rv::sb(2, 1, 0), 0x1003, Status::Running, "sb +3"},
      {rv::lh(3, 1, 0), 0x1002, Status::Running, "lh +2"}, {rv::lw(3, 1, 3), 0x0fff + 1 - 3, Status::Running, "lw base+3 ok"},
  };
  for (const auto& c : cases) EXPECT_EQ(one(c.insn, c.base, 0x55).status(), c.expect) << c.name;
}

TEST(ModelMemory, OutOfRangeIsMemFaultAndAlignmentComesFirst) {
  EXPECT_EQ(one(rv::lw(3, 1, 0), 0x10000, 0).status(), Status::MemFault);
  EXPECT_EQ(one(rv::lw(3, 1, 0), 0xfffc, 0).status(), Status::Running);   // last word of RAM
  EXPECT_EQ(one(rv::lb(3, 1, 0), 0xffff, 0).status(), Status::Running);   // last byte of RAM
  EXPECT_EQ(one(rv::lb(3, 1, 0), 0x10000, 0).status(), Status::MemFault);
  EXPECT_EQ(one(rv::sw(2, 1, 0), 0x10000, 0).status(), Status::MemFault);
  EXPECT_EQ(one(rv::lw(3, 1, 0), kMagicAddr, 0).status(), Status::MemFault) << "loads from the magic word fault";
  EXPECT_EQ(one(rv::lw(3, 1, 0), 0x10001, 0).status(), Status::Trap) << "misaligned beats out-of-range";
  EXPECT_EQ(one(rv::sw(2, 1, 0), kMagicAddr + 4, 0).status(), Status::MemFault) << "only the magic word terminates";
  Model fetch;
  fetch.set_pc(0x10000);
  EXPECT_EQ(fetch.step(), Status::MemFault);
  Model fetch_misaligned;
  fetch_misaligned.set_pc(0x10002);
  EXPECT_EQ(fetch_misaligned.step(), Status::Trap);
}

TEST(ModelMemory, MagicStoreEndsTheRunWithTheBusWord) {
  EXPECT_EQ(one(rv::sw(2, 1, 0), kMagicAddr, 0xaabbccdd).status(), Status::Exited);
  auto exit_code = [](std::uint32_t insn, std::uint32_t addr) {
    Model m;
    m.write_word(0, insn);
    m.set_reg(1, addr);
    m.set_reg(2, 0xaabbccdd);
    m.step();
    EXPECT_EQ(m.status(), Status::Exited);
    return m.result().exit_code;
  };
  EXPECT_EQ(exit_code(rv::sw(2, 1, 0), kMagicAddr), 0xaabbccddu);
  EXPECT_EQ(exit_code(rv::sb(2, 1, 1), kMagicAddr), 0xddddddddu);  // byte replicated into all lanes
  EXPECT_EQ(exit_code(rv::sh(2, 1, 2), kMagicAddr), 0xccddccddu);  // halfword replicated
}

// ---------------------------------------------------------------- traps and misc

TEST(ModelTrap, UnsupportedInstructionsTrap) {
  const std::uint32_t words[] = {0x00000000, 0xffffffff, rv::ecall(), rv::ebreak(), 0x340110f3, 0x023100b3, 0x0000100f};
  for (const auto w : words) {
    Model m;
    m.write_word(0, w);
    EXPECT_EQ(m.step(), Status::Trap) << std::hex << w;
  }
}

TEST(ModelMisc, FenceIsANoOp) {
  Model m;
  m.load(std::array<std::uint32_t, 2>{rv::fence(), rv::addi(3, 0, 9)});
  EXPECT_EQ(m.step(), Status::Running);
  EXPECT_EQ(m.pc(), 4u);
  m.step();
  EXPECT_EQ(m.reg(3), 9u);
}

TEST(ModelMisc, StatusIsStickyAndRunStopsAtTimeout) {
  Model m;
  m.write_word(0, 0);
  EXPECT_EQ(m.step(), Status::Trap);
  EXPECT_EQ(m.step(), Status::Trap);

  const std::array<std::uint32_t, 1> spin{rv::jal(0, 0)};
  const ModelResult r = run_model(spin, {1000, false});
  EXPECT_EQ(r.status, Status::Timeout);
  EXPECT_EQ(r.instret, 1000u);
}

// ---------------------------------------------------------------- whole programs

TEST(ModelProgram, SmokeAddProgram) {
  const ModelResult r = run_model(smoke::kAdd);
  EXPECT_EQ(r.status, Status::Exited);
  EXPECT_EQ(r.exit_code, 12u);
  EXPECT_EQ(r.instret, 5u);
  EXPECT_EQ(r.regs[1], 5u);
  EXPECT_EQ(r.regs[2], 7u);
  EXPECT_EQ(r.regs[3], 12u);
  EXPECT_EQ(r.regs[4], kMagicAddr);
}

TEST(ModelProgram, SmokeJalrOddProgram) {
  const ModelResult r = run_model(smoke::kJalrOdd);
  EXPECT_EQ(r.status, Status::Exited);
  EXPECT_EQ(r.exit_code, 8u);
  EXPECT_EQ(r.regs[5], 0u);
  EXPECT_EQ(r.regs[2], 8u);
}

TEST(ModelProgram, LoopSumsOneToTen) {
  // x1 = 10; x2 = 0; do { x2 += x1; x1 -= 1; } while (x1 != 0); store x2 to the magic address.
  std::vector<std::uint32_t> p = {
      rv::addi(1, 0, 10),
      rv::addi(2, 0, 0),
      rv::add(2, 2, 1),    // 0x08
      rv::addi(1, 1, -1),  // 0x0c
      rv::bne(1, 0, -8),   // 0x10 -> 0x08
  };
  for (const auto w : rv::li(4, kMagicAddr)) p.push_back(w);
  p.push_back(rv::sw(2, 4, 0));
  const ModelResult r = run_model(p);
  EXPECT_EQ(r.status, Status::Exited);
  EXPECT_EQ(r.exit_code, 55u);
  EXPECT_EQ(r.instret, 2u + 3u * 10u + 3u);
}

TEST(ModelProgram, TraceRecordsRetiresAndStores) {
  std::vector<std::uint32_t> p = {rv::addi(1, 0, 0x7f), rv::addi(2, 0, 0x1000 - 0x1000 + 0x100), rv::sb(1, 2, 3), rv::sh(1, 2, 4)};
  for (const auto w : rv::li(4, kMagicAddr)) p.push_back(w);
  p.push_back(rv::sw(1, 4, 0));
  const ModelResult r = run_model(p, {1000, true});
  ASSERT_EQ(r.status, Status::Exited);
  EXPECT_EQ(r.retired.size(), r.instret);
  EXPECT_EQ(r.retired[0].pc, 0u);
  EXPECT_EQ(r.retired[0].rd, 1u);
  EXPECT_EQ(r.retired[0].rd_value, 0x7fu);
  EXPECT_EQ(r.retired[2].rd, 0u) << "a store writes no register";
  EXPECT_EQ(r.retired[2].next_pc, 12u);
  ASSERT_EQ(r.stores.size(), 2u) << "the terminating magic store is not recorded";
  EXPECT_EQ(r.stores[0].addr, 0x100u);
  EXPECT_EQ(r.stores[0].data, 0x7f7f7f7fu);
  EXPECT_EQ(r.stores[0].strobe, 0x8u);
  EXPECT_EQ(r.stores[1].addr, 0x104u);
  EXPECT_EQ(r.stores[1].data, 0x007f007fu);
  EXPECT_EQ(r.stores[1].strobe, 0x3u);
}

TEST(ModelProgram, MemoryHashSeesStoresAndIsDeterministic) {
  std::vector<std::uint32_t> p = {rv::addi(1, 0, 0x55), rv::sw(1, 0, 0x200)};
  for (const auto w : rv::li(4, kMagicAddr)) p.push_back(w);
  p.push_back(rv::sw(1, 4, 0));
  const ModelResult a = run_model(p);
  const ModelResult b = run_model(p);
  EXPECT_EQ(a.mem_hash, b.mem_hash);
  p[1] = rv::sw(1, 0, 0x204);
  EXPECT_NE(run_model(p).mem_hash, a.mem_hash) << "the same value stored at another address must change the hash";
}
