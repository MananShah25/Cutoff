// Directed differential tests: the same program runs on the reference model and on the Verilated
// PicoRV32; status, exit code and (when the program exits) all registers must agree.
// This is NOT the 100,000-program baseline gate (Step 4); it covers hand-chosen corner cases.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "model.hpp"
#include "rv32i.hpp"
#include "sim_core.hpp"

using namespace cutoff;

namespace {

constexpr std::uint32_t kMin = 0x80000000u, kMax = 0x7fffffffu, kM1 = 0xffffffffu;
using Code = std::vector<std::uint32_t>;

void emit(Code& c, std::initializer_list<std::uint32_t> words) { c.insert(c.end(), words); }
void emit_li(Code& c, unsigned rd, std::uint32_t v) {
  const auto w = rv::li(rd, v);
  c.insert(c.end(), w.begin(), w.end());
}
// Ends a program: store x3 to the magic address (clobbers x4).
void finish(Code& c) {
  emit_li(c, 4, kMagicAddr);
  emit(c, {rv::sw(3, 4, 0)});
}

std::optional<std::string> compare(const Code& code) {
  const ModelResult m = run_model(code, {20'000, false});
  const RunResult r = run_program(code, {400'000});
  std::ostringstream err;
  if (m.status != r.status) {
    err << "status model=" << to_string(m.status) << " core=" << to_string(r.status);
  } else if (m.status == Status::Exited) {
    if (m.exit_code != r.exit_code) err << "exit_code model=0x" << std::hex << m.exit_code << " core=0x" << r.exit_code;
    for (unsigned i = 1; i < 32 && err.str().empty(); ++i)
      if (m.regs[i] != r.regs[i]) err << "x" << std::dec << i << " model=0x" << std::hex << m.regs[i] << " core=0x" << r.regs[i];
  }
  if (err.str().empty()) return std::nullopt;
  return err.str();
}

// Collects mismatches so one run reports several, not just the first.
struct Diff {
  std::vector<std::string> bad;
  int total = 0;
  void check(const std::string& name, const Code& code) {
    ++total;
    if (const auto e = compare(code)) bad.push_back(name + ": " + *e);
  }
  void expect_clean() const {
    std::ostringstream os;
    for (std::size_t i = 0; i < bad.size() && i < 12; ++i) os << "\n  " << bad[i];
    std::printf("[ModelVsCore] compared %d programs, %zu differ\n", total, bad.size());
    EXPECT_TRUE(bad.empty()) << bad.size() << " of " << total << " programs differ" << os.str();
  }
};

constexpr std::array<std::uint32_t, 20> kValues = {0,          1,          2,          31,         32,         0x7ff,      0x800,
                                                   0xfff,      kMax,       kMin,       kM1,        0xfffffffe, 0x55555555, 0xaaaaaaaa,
                                                   0x12345678, 0x80000001, 0x0000ffff, 0xffff0000, 0xfffff800, 0x00000400};

std::string hex(std::uint32_t v) {
  std::ostringstream os;
  os << "0x" << std::hex << v;
  return os.str();
}

}  // namespace

TEST(ModelVsCore, RegisterRegisterOps) {
  using RR = std::uint32_t (*)(unsigned, unsigned, unsigned);
  const std::pair<const char*, RR> ops[] = {{"add", rv::add}, {"sub", rv::sub}, {"sll", rv::sll}, {"slt", rv::slt},
                                            {"sltu", rv::sltu}, {"xor", rv::xor_}, {"srl", rv::srl}, {"sra", rv::sra},
                                            {"or", rv::or_}, {"and", rv::and_}};
  Diff d;
  for (const auto& [name, enc] : ops)
    for (const auto a : kValues)
      for (const auto b : kValues) {
        Code c;
        emit_li(c, 1, a);
        emit_li(c, 2, b);
        emit(c, {enc(3, 1, 2)});
        finish(c);
        d.check(std::string(name) + " " + hex(a) + " " + hex(b), c);
      }
  d.expect_clean();
}

TEST(ModelVsCore, RegisterImmediateOps) {
  using RI = std::uint32_t (*)(unsigned, unsigned, std::int32_t);
  const std::pair<const char*, RI> ops[] = {{"addi", rv::addi}, {"slti", rv::slti}, {"sltiu", rv::sltiu},
                                            {"xori", rv::xori}, {"ori", rv::ori}, {"andi", rv::andi}};
  const std::int32_t imms[] = {0, 1, -1, 2, 31, 2047, -2048, 1024, -1024, 0x555, -0x556};
  Diff d;
  for (const auto& [name, enc] : ops)
    for (const auto a : kValues)
      for (const auto imm : imms) {
        Code c;
        emit_li(c, 1, a);
        emit(c, {enc(3, 1, imm)});
        finish(c);
        d.check(std::string(name) + " " + hex(a) + " " + std::to_string(imm), c);
      }
  d.expect_clean();
}

TEST(ModelVsCore, ImmediateShifts) {
  using SH = std::uint32_t (*)(unsigned, unsigned, unsigned);
  const std::pair<const char*, SH> ops[] = {{"slli", rv::slli}, {"srli", rv::srli}, {"srai", rv::srai}};
  Diff d;
  for (const auto& [name, enc] : ops)
    for (const auto a : kValues)
      for (unsigned sh : {0u, 1u, 2u, 15u, 16u, 30u, 31u}) {
        Code c;
        emit_li(c, 1, a);
        emit(c, {enc(3, 1, sh)});
        finish(c);
        d.check(std::string(name) + " " + hex(a) + " " + std::to_string(sh), c);
      }
  d.expect_clean();
}

TEST(ModelVsCore, UpperImmediatesAndAuipcAtDifferentPcs) {
  Diff d;
  for (const std::uint32_t imm : {0u, 1u, 0x7ffffu, 0x80000u, 0xfffffu, 0x12345u}) {
    for (unsigned pad : {0u, 3u, 100u}) {
      Code c;
      for (unsigned i = 0; i < pad; ++i) emit(c, {rv::nop()});
      emit(c, {rv::lui(5, imm), rv::auipc(3, imm)});
      finish(c);
      d.check("lui/auipc " + hex(imm) + " pad " + std::to_string(pad), c);
    }
  }
  d.expect_clean();
}

TEST(ModelVsCore, X0AndRdEqualsRsPatterns) {
  Diff d;
  Code c;
  emit(c, {rv::addi(0, 0, 5), rv::lui(0, 0x12345), rv::add(3, 0, 0)});
  finish(c);
  d.check("write x0 then read", c);
  Code e;
  emit_li(e, 1, 0x1234);
  emit(e, {rv::add(1, 1, 1), rv::sub(1, 1, 1), rv::addi(1, 1, 77), rv::xor_(3, 1, 1), rv::addi(3, 1, 0)});
  finish(e);
  d.check("rd == rs1 == rs2", e);
  d.expect_clean();
}

TEST(ModelVsCore, Branches) {
  using Br = std::uint32_t (*)(unsigned, unsigned, std::int32_t);
  const std::pair<const char*, Br> ops[] = {{"beq", rv::beq}, {"bne", rv::bne}, {"blt", rv::blt},
                                            {"bge", rv::bge}, {"bltu", rv::bltu}, {"bgeu", rv::bgeu}};
  Diff d;
  for (const auto& [name, enc] : ops)
    for (const auto a : kValues)
      for (const auto b : kValues) {
        Code c;
        emit_li(c, 1, a);
        emit_li(c, 2, b);
        emit(c, {enc(1, 2, 8), rv::addi(3, 0, 1), rv::addi(5, 0, 7)});  // taken: skips the addi to x3
        finish(c);
        d.check(std::string(name) + " " + hex(a) + " " + hex(b), c);
      }
  // A backward branch: count down from 5.
  Code loop;
  emit(loop, {rv::addi(1, 0, 5), rv::addi(3, 3, 3), rv::addi(1, 1, -1), rv::bne(1, 0, -8)});
  finish(loop);
  d.check("backward bne loop", loop);
  d.expect_clean();
}

TEST(ModelVsCore, JumpsAndLinks) {
  Diff d;
  Code a;  // jal forward, link register keeps pc+4
  emit(a, {rv::nop(), rv::jal(5, 12), rv::addi(3, 0, 99), rv::addi(3, 0, 98), rv::addi(6, 5, 0)});
  finish(a);
  d.check("jal forward", a);
  Code b;  // jal x0 does not link
  emit(b, {rv::jal(0, 8), rv::addi(3, 0, 99), rv::addi(3, 3, 1)});
  finish(b);
  d.check("jal x0", b);
  for (const std::int32_t low : {0, 1, 2, 3}) {  // jalr with every combination of low target bits
    Code c;
    emit_li(c, 1, 24 + static_cast<std::uint32_t>(low));  // target pc 24 (+ low bits)
    emit(c, {rv::jalr(5, 1, 0)});                          // pc 8 -> link 12
    emit(c, {rv::addi(3, 0, 99), rv::addi(3, 0, 98), rv::addi(3, 0, 97), rv::addi(3, 3, 1)});
    finish(c);
    d.check("jalr low bits " + std::to_string(low), c);
  }
  Code e;  // jalr with rd == rs1
  emit_li(e, 1, 16);
  emit(e, {rv::jalr(1, 1, 0), rv::addi(3, 0, 99), rv::addi(3, 1, 0)});
  finish(e);
  d.check("jalr rd==rs1", e);
  d.expect_clean();
}

TEST(ModelVsCore, MemoryLanesAndExtension) {
  Diff d;
  using St = std::uint32_t (*)(unsigned, unsigned, std::int32_t);
  struct S { const char* name; St enc; std::vector<std::int32_t> offs; };
  const S stores[] = {{"sw", rv::sw, {0}}, {"sh", rv::sh, {0, 2}}, {"sb", rv::sb, {0, 1, 2, 3}}};
  for (const auto& s : stores)
    for (const auto off : s.offs)
      for (const std::uint32_t v : {0x00000000u, 0x80ff7f01u, 0xfedcba98u, 0x7f80ff00u}) {
        Code c;
        emit_li(c, 1, 0x1000);
        emit_li(c, 2, 0x81c3a55au);
        emit(c, {rv::sw(2, 1, 0)});  // known background word
        emit_li(c, 2, v);
        emit(c, {s.enc(2, 1, off)});
        emit(c, {rv::lw(3, 1, 0), rv::lb(5, 1, 0), rv::lb(6, 1, 1), rv::lb(7, 1, 2), rv::lb(8, 1, 3), rv::lh(9, 1, 0),
                 rv::lh(10, 1, 2), rv::lhu(11, 1, 0), rv::lhu(12, 1, 2), rv::lbu(13, 1, 1), rv::lbu(14, 1, 3)});
        finish(c);
        d.check(std::string(s.name) + " off " + std::to_string(off) + " val " + hex(v), c);
      }
  Code neg;  // negative offsets
  emit_li(neg, 1, 0x1008);
  emit_li(neg, 2, 0xcafe0123u);
  emit(neg, {rv::sw(2, 1, -4), rv::lw(3, 1, -4), rv::lh(5, 1, -2), rv::lb(6, 1, -1)});
  finish(neg);
  d.check("negative offsets", neg);
  Code zero;  // load into x0 changes nothing
  emit(zero, {rv::lw(0, 0, 0), rv::addi(3, 0, 9)});
  finish(zero);
  d.check("load into x0", zero);
  d.expect_clean();
}

TEST(ModelVsCore, MagicStoreWidths) {
  Diff d;
  using St = std::uint32_t (*)(unsigned, unsigned, std::int32_t);
  const std::pair<const char*, St> stores[] = {{"sw", rv::sw}, {"sh", rv::sh}, {"sb", rv::sb}};
  for (const auto& [name, enc] : stores)
    for (const std::int32_t off : {0, 1, 2, 3}) {
      Code c;
      emit_li(c, 4, kMagicAddr);
      emit_li(c, 3, 0xa1b2c3d4u);
      emit(c, {enc(3, 4, off)});
      d.check(std::string("magic ") + name + " +" + std::to_string(off), c);
    }
  d.expect_clean();
}

TEST(ModelVsCore, TrapsAndFaults) {
  Diff d;
  // Unsupported or illegal encodings.
  const std::pair<const char*, std::uint32_t> words[] = {
      {"all zeros", 0x00000000}, {"all ones", 0xffffffff}, {"ecall", rv::ecall()}, {"ebreak", rv::ebreak()},
      {"csrrw", 0x340110f3},     {"mul", 0x023100b3},      {"slli f7=0x20", 0x40111093}, {"xor f7=0x20", 0x403140b3},
      {"jalr f3=1", 0x00009067}, {"ld", 0x00003003},       {"sd", 0x00003023}, {"branch f3=2", 0x00002063},
      {"fence.i", 0x0000100f},   {"compressed pattern", 0x00000001}, {"mret", 0x30200073},
      {"rdcycle", 0xc0002573},   {"addi f7 junk srli", 0x41015093 | 0x04000000},
  };
  for (const auto& [name, w] : words) {
    Code c;
    emit(c, {rv::addi(3, 0, 1), w});
    finish(c);
    d.check(std::string("illegal: ") + name, c);
  }
  // FENCE variants must run as no-ops.
  for (const std::uint32_t w : {rv::fence(), 0x8330000fu, 0x0000000fu}) {
    Code c;
    emit(c, {rv::addi(3, 0, 5), w, rv::addi(3, 3, 1)});
    finish(c);
    d.check("fence " + hex(w), c);
  }
  // Misaligned data accesses trap; byte accesses never do.
  using Ld = std::uint32_t (*)(unsigned, unsigned, std::int32_t);
  const std::pair<const char*, Ld> loads[] = {{"lw", rv::lw}, {"lh", rv::lh}, {"lhu", rv::lhu}, {"lb", rv::lb}, {"lbu", rv::lbu}};
  for (const auto& [name, enc] : loads)
    for (const std::int32_t off : {0, 1, 2, 3}) {
      Code c;
      emit_li(c, 1, 0x1000);
      emit(c, {enc(3, 1, off)});
      finish(c);
      d.check(std::string("load ") + name + " +" + std::to_string(off), c);
    }
  using St = std::uint32_t (*)(unsigned, unsigned, std::int32_t);
  const std::pair<const char*, St> stores[] = {{"sw", rv::sw}, {"sh", rv::sh}, {"sb", rv::sb}};
  for (const auto& [name, enc] : stores)
    for (const std::int32_t off : {0, 1, 2, 3}) {
      Code c;
      emit_li(c, 1, 0x1000);
      emit_li(c, 2, 0x77);
      emit(c, {enc(2, 1, off), rv::lw(3, 1, 0)});
      finish(c);
      d.check(std::string("store ") + name + " +" + std::to_string(off), c);
    }
  // Out of range.
  for (const std::uint32_t addr : {0xfffcu, 0xffffu, 0x10000u, 0x10004u, kMagicAddr, kMagicAddr + 4, 0xfffffffcu, 0x00fffff0u}) {
    Code l;
    emit_li(l, 1, addr);
    emit(l, {rv::lb(3, 1, 0)});
    finish(l);
    d.check("lb @" + hex(addr), l);
    Code lw;
    emit_li(lw, 1, addr & ~3u);
    emit(lw, {rv::lw(3, 1, 0)});
    finish(lw);
    d.check("lw @" + hex(addr & ~3u), lw);
    Code s;
    emit_li(s, 1, addr & ~3u);
    emit_li(s, 2, 0x1234);
    emit(s, {rv::sw(2, 1, 0), rv::addi(3, 0, 1)});
    finish(s);
    d.check("sw @" + hex(addr & ~3u), s);
  }
  d.expect_clean();
}

// Spec open question: "A jump or branch to a target that is not 4-byte aligned traps in a core without
// compressed instructions." This pins the behaviour of the pinned core for every flavour of jump.
TEST(ModelVsCore, MisalignedControlFlowTargets) {
  Diff d;
  using Br = std::uint32_t (*)(unsigned, unsigned, std::int32_t);
  const std::pair<const char*, Br> branches[] = {{"beq", rv::beq}, {"bne", rv::bne}};
  for (const auto& [name, enc] : branches)
    for (const std::int32_t off : {2, 6, -2, 4094}) {
      Code taken;  // x1 == x2, so beq is taken and bne is not
      emit(taken, {rv::addi(1, 0, 1), rv::addi(2, 0, 1), enc(1, 2, off), rv::addi(3, 0, 42)});
      finish(taken);
      d.check(std::string(name) + " misaligned " + std::to_string(off) + " (x1==x2)", taken);
    }
  for (const std::int32_t off : {2, 6, 1 << 10, -2}) {
    Code c;
    emit(c, {rv::jal(5, off), rv::addi(3, 0, 42)});
    finish(c);
    d.check("jal misaligned " + std::to_string(off), c);
  }
  for (const std::uint32_t target : {2u, 6u, 3u, 0x1002u, 0x1003u, 0x1001u}) {
    Code c;
    emit_li(c, 1, target);
    emit(c, {rv::jalr(5, 1, 0), rv::addi(3, 0, 42)});
    finish(c);
    d.check("jalr to " + hex(target), c);
  }
  d.expect_clean();
}
