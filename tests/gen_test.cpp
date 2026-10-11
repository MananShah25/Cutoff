// Generator tests: RNG known answers (from an independent Python reference), structural invariants checked
// on thousands of programs, dynamic invariants checked by running programs on the reference model,
// determinism and statelessness, and distribution checks.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "generator.hpp"
#include "model.hpp"
#include "rng.hpp"
#include "rv32i.hpp"

using namespace cutoff;
using namespace cutoff::gen;

// ---------------------------------------------------------------- RNG

TEST(Rng, SplitMixMatchesPublishedAndReferenceValues) {
  // Seed 0 is the widely published SplitMix64 test vector; seed 1234567 comes from the Python reference.
  std::uint64_t z = 0;
  const std::array<std::uint64_t, 3> expect0 = {0xe220a8397b1dcdafULL, 0x6e789e6aa1b965f4ULL, 0x06c45d188009454fULL};
  for (const auto e : expect0) {
    z += kSplitMixGamma;
    EXPECT_EQ(splitmix_mix(z), e);
  }
  z = 1234567;
  const std::array<std::uint64_t, 3> expect1 = {0x599ed017fb08fc85ULL, 0x2c73f08458540fa5ULL, 0x883ebce5a3f27c77ULL};
  for (const auto e : expect1) {
    z += kSplitMixGamma;
    EXPECT_EQ(splitmix_mix(z), e);
  }
}

TEST(Rng, Xoshiro256ssMatchesReference) {
  Rng a(0);
  for (const std::uint64_t e : {0x99ec5f36cb75f2b4ULL, 0xbf6e1f784956452aULL, 0x1a5f849d4933e6e0ULL, 0x6aa594f1262d2d2cULL})
    EXPECT_EQ(a.next64(), e);
  Rng b(42);
  for (const std::uint64_t e : {0x15780b2e0c2ec716ULL, 0x6104d9866d113a7eULL, 0xae17533239e499a1ULL, 0xecb8ad4703b360a1ULL})
    EXPECT_EQ(b.next64(), e);
}

TEST(Rng, BoundedDrawsMatchReference) {
  Rng a(7);
  for (const std::uint32_t e : {7u, 2u, 8u, 9u, 9u, 8u, 0u, 1u}) EXPECT_EQ(a.below(10), e);
  Rng b(7);
  for (const std::uint32_t e : {278751231u, 839627467u, 981097731u, 990860285u}) EXPECT_EQ(b.below(1000000007u), e);
  Rng c(7);
  for (int i = 0; i < 3; ++i) EXPECT_EQ(c.below(1), 0u);
}

TEST(Rng, BoundedDrawsAreUniformEvenForAwkwardBounds) {
  for (const std::uint32_t n : {3u, 7u, 31u, 38u, 4096u, 1000u}) {
    Rng rng(n);
    std::vector<int> hist(n, 0);
    const int draws = 200000;
    for (int i = 0; i < draws; ++i) {
      const std::uint32_t v = rng.below(n);
      ASSERT_LT(v, n);
      ++hist[v];
    }
    const double mean = static_cast<double>(draws) / n;
    for (std::uint32_t v = 0; v < n; ++v) EXPECT_NEAR(hist[v], mean, 6 * std::sqrt(mean)) << "n=" << n << " v=" << v;
  }
}

TEST(Rng, ProgramSeedsMatchReferenceAndDoNotCollide) {
  EXPECT_EQ(program_seed(0, 0), 0xe220a8397b1dcdafULL);
  EXPECT_EQ(program_seed(0, 1), 0x6e789e6aa1b965f4ULL);
  EXPECT_EQ(program_seed(0, 2), 0x06c45d188009454fULL);
  EXPECT_EQ(program_seed(2026, 5), 0x7494a57907985539ULL);
  std::set<std::uint64_t> seen;
  for (std::uint64_t i = 0; i < 100000; ++i) seen.insert(program_seed(2026, i));
  EXPECT_EQ(seen.size(), 100000u);
  EXPECT_NE(program_seed(1, 0), program_seed(2, 0));
}

// ---------------------------------------------------------------- structural invariants

namespace {

bool writes_rd(Op op) {
  switch (op) {
    case Op::BEQ: case Op::BNE: case Op::BLT: case Op::BGE: case Op::BLTU: case Op::BGEU:
    case Op::SB: case Op::SH: case Op::SW: case Op::FENCE: case Op::ECALL: case Op::EBREAK: case Op::Illegal:
      return false;
    default: return true;
  }
}

unsigned access_size(Op op) {
  switch (op) {
    case Op::LW: case Op::SW: return 4;
    case Op::LH: case Op::LHU: case Op::SH: return 2;
    default: return 1;
  }
}

// Returns "" when every static invariant holds, otherwise a description of the first violation.
std::string check_structure(const Program& p, const GenConfig& cfg) {
  std::ostringstream err;
  auto fail = [&](const std::string& m) { err << m; return err.str(); };

  if (p.body_begin != kInitWords) return fail("body does not start after the init block");
  const std::uint32_t len = p.body_end - p.body_begin;
  if (len < cfg.min_len || len > cfg.max_len) return fail("body length " + std::to_string(len) + " outside configured range");
  if (p.code.size() != p.body_end + kEpilogueWords) return fail("code size != body_end + epilogue");
  if (p.code.size() > kMaxCodeWords) return fail("code reaches the data window");

  // Init block: lui+addi pairs for x1..x31, x31 holding the data base.
  for (unsigned r = 1; r <= 31; ++r) {
    const std::size_t first = 2 * static_cast<std::size_t>(r - 1);
    const Decoded hi = decode(p.code[first]);
    const Decoded lo = decode(p.code[first + 1]);
    if (hi.op != Op::LUI || hi.rd != r || lo.op != Op::ADDI || lo.rd != r || lo.rs1 != r) return fail("malformed init pair for x" + std::to_string(r));
    if (r == 31 && static_cast<std::uint32_t>(hi.imm) + static_cast<std::uint32_t>(lo.imm) != kDataBaseRegValue) return fail("x31 init value wrong");
  }
  // Epilogue.
  const auto li = rv::li(31, kMagicAddr);
  if (p.code[p.body_end] != li[0] || p.code[p.body_end + 1] != li[1] || p.code[p.body_end + 2] != rv::sw(1, 31, 0)) return fail("epilogue is not 'li x31, magic; sw x1, 0(x31)'");

  const std::uint32_t epilogue_pc = 4 * p.body_end;
  std::set<std::uint32_t> jalr_pcs;
  for (std::uint32_t w = p.body_begin; w < p.body_end; ++w)
    if (decode(p.code[w]).op == Op::JALR) jalr_pcs.insert(4 * w);

  auto bad_target = [&](std::uint32_t pc, std::uint32_t target) -> std::string {
    if ((target & 3u) != 0) return "target not 4-byte aligned";
    if (target <= pc) return "target not strictly forward";
    if (target > epilogue_pc) return "target beyond the start of the epilogue";
    if (jalr_pcs.count(target) != 0) return "target lands on the JALR half of an AUIPC/JALR pair";
    return {};
  };

  for (std::uint32_t w = p.body_begin; w < p.body_end; ++w) {
    const std::uint32_t pc = 4 * w;
    const Decoded d = decode(p.code[w]);
    const std::string at = " at word " + std::to_string(w);
    if (d.op == Op::Illegal || d.op == Op::ECALL || d.op == Op::EBREAK) return fail("excluded opcode" + at);
    if (writes_rd(d.op) && d.rd == kDataBaseReg) return fail("body writes the reserved data base register" + at);
    switch (d.op) {
      case Op::LB: case Op::LH: case Op::LW: case Op::LBU: case Op::LHU:
      case Op::SB: case Op::SH: case Op::SW: {
        if (d.rs1 != kDataBaseReg) return fail("memory access not based on x31" + at);
        const std::uint32_t addr = kDataBaseRegValue + static_cast<std::uint32_t>(d.imm);
        const unsigned size = access_size(d.op);
        if (addr % size != 0) return fail("misaligned access" + at);
        if (addr < kDataBase || addr + size > kDataBase + kDataBytes) return fail("access outside the data window" + at);
        break;
      }
      case Op::BEQ: case Op::BNE: case Op::BLT: case Op::BGE: case Op::BLTU: case Op::BGEU: case Op::JAL: {
        const std::string e = bad_target(pc, pc + static_cast<std::uint32_t>(d.imm));
        if (!e.empty()) return fail(e + at);
        break;
      }
      case Op::JALR: {
        if (w == p.body_begin) return fail("JALR without its AUIPC" + at);
        const Decoded base = decode(p.code[w - 1]);
        if (base.op != Op::AUIPC || base.imm != 0 || base.rd != d.rs1 || d.rs1 == 0) return fail("JALR not preceded by 'auipc rb, 0'" + at);
        const std::uint32_t target = (4 * (w - 1) + static_cast<std::uint32_t>(d.imm)) & ~1u;
        const std::string e = bad_target(pc, target);
        if (!e.empty()) return fail(e + at);
        break;
      }
      default: break;
    }
  }
  return {};
}

struct Failures {
  std::vector<std::string> list;
  void add(std::uint64_t seed, const std::string& msg) {
    if (list.size() < 8) list.push_back("seed " + std::to_string(seed) + ": " + msg);
  }
  void expect_none(const char* what, int n) const {
    std::ostringstream os;
    for (const auto& s : list) os << "\n  " << s;
    EXPECT_TRUE(list.empty()) << what << ": violations in " << n << " programs checked" << os.str();
  }
};

}  // namespace

TEST(GenStructure, EveryProgramSatisfiesTheStaticInvariants) {
  const GenConfig cfg;
  Failures f;
  const int n = 20000;
  for (int i = 0; i < n; ++i) {
    const std::uint64_t seed = program_seed(1, i);
    const std::string e = check_structure(generate(cfg, seed), cfg);
    if (!e.empty()) f.add(seed, e);
  }
  f.expect_none("static invariants", n);
}

TEST(GenStructure, HoldsAtTheEdgesOfTheConfigSpace) {
  struct C { std::uint32_t min_len, max_len, max_skip; };
  const C configs[] = {{1, 1, 1}, {1, 3, 1}, {2, 2, 8}, {1, 40, 256}, {200, 400, 1}, {64, 64, 3}, {1000, 1000, 256}};
  for (const auto& c : configs) {
    GenConfig cfg;
    cfg.min_len = c.min_len;
    cfg.max_len = c.max_len;
    cfg.max_skip = c.max_skip;
    Failures f;
    for (int i = 0; i < 3000; ++i) {
      const std::uint64_t seed = program_seed(c.min_len * 1000 + c.max_skip, i);
      const std::string e = check_structure(generate(cfg, seed), cfg);
      if (!e.empty()) f.add(seed, e);
    }
    f.expect_none(("len " + std::to_string(c.min_len) + ".." + std::to_string(c.max_len) + " skip " + std::to_string(c.max_skip)).c_str(), 3000);
  }
}

TEST(GenStructure, LargestAllowedProgramFitsBelowTheDataWindow) {
  GenConfig cfg;
  cfg.min_len = cfg.max_len = kMaxCodeWords - kInitWords - kEpilogueWords;
  ASSERT_TRUE(validate(cfg).empty());
  const Program p = generate(cfg, 5);
  EXPECT_EQ(p.code.size(), kMaxCodeWords);
  EXPECT_EQ(check_structure(p, cfg), "");
  const ModelResult r = run_model(p.code, {100'000, false});
  EXPECT_EQ(r.status, Status::Exited);
}

// A checker that never fails proves nothing: hand-built programs that break each invariant must be rejected.
namespace {

// Valid init block and epilogue (taken from a generated program) around a hand-written body.
Program with_body(const std::vector<std::uint32_t>& body) {
  GenConfig cfg;
  cfg.min_len = cfg.max_len = static_cast<std::uint32_t>(body.size());
  Program p = generate(cfg, 77);
  for (std::size_t i = 0; i < body.size(); ++i) p.code[p.body_begin + i] = body[i];
  return p;
}

}  // namespace

TEST(GenStructure, TheCheckerAcceptsAValidHandBuiltProgramAndRejectsEachViolation) {
  const GenConfig five = [] { GenConfig c; c.min_len = c.max_len = 5; return c; }();
  // slot: 0 branch  1 auipc  2 jalr (lands on slot 3)  3 addi  4 addi
  auto valid = [](std::int32_t branch_off) {
    return std::vector<std::uint32_t>{rv::beq(0, 0, branch_off), rv::auipc(5, 0), rv::jalr(0, 5, 8), rv::addi(1, 0, 1), rv::addi(2, 0, 2)};
  };
  EXPECT_EQ(check_structure(with_body(valid(4)), five), "") << "control case must pass";
  EXPECT_NE(check_structure(with_body(valid(8)), five).find("JALR half"), std::string::npos) << "branch into the middle of a pair";
  EXPECT_NE(check_structure(with_body(valid(-8)), five).find("forward"), std::string::npos);
  EXPECT_NE(check_structure(with_body(valid(2)), five).find("aligned"), std::string::npos);
  EXPECT_NE(check_structure(with_body(valid(24)), five).find("beyond"), std::string::npos) << "target past the start of the epilogue";
  EXPECT_EQ(check_structure(with_body(valid(20)), five), "") << "the epilogue start itself is a legal target";

  auto with_slot = [&](std::size_t slot, std::uint32_t w) {
    auto b = valid(4);
    b[slot] = w;
    return check_structure(with_body(b), five);
  };
  EXPECT_NE(with_slot(3, rv::ecall()).find("excluded"), std::string::npos);
  EXPECT_NE(with_slot(3, rv::ebreak()).find("excluded"), std::string::npos);
  EXPECT_NE(with_slot(3, 0x00000000).find("excluded"), std::string::npos);
  EXPECT_NE(with_slot(3, rv::addi(31, 0, 0)).find("reserved"), std::string::npos);
  EXPECT_NE(with_slot(3, rv::lw(3, 31, 2)).find("misaligned"), std::string::npos);
  EXPECT_NE(with_slot(3, rv::lh(3, 31, 1)).find("misaligned"), std::string::npos);
  EXPECT_NE(with_slot(3, rv::sw(3, 31, -2)).find("misaligned"), std::string::npos);
  EXPECT_NE(with_slot(3, rv::lw(3, 5, 4)).find("x31"), std::string::npos);
  EXPECT_EQ(with_slot(3, rv::lb(3, 31, 3)), "") << "byte accesses may be at any offset";
  EXPECT_EQ(with_slot(3, rv::sw(3, 31, 2044)), "");
  EXPECT_NE(with_slot(2, rv::jalr(0, 5, 8) | (1u << 12)).find("excluded"), std::string::npos) << "JALR with funct3 != 0 is illegal";
  EXPECT_NE(with_slot(1, rv::addi(5, 0, 0)).find("JALR"), std::string::npos) << "JALR without its AUIPC";
  EXPECT_NE(with_slot(1, rv::auipc(5, 1)).find("JALR"), std::string::npos) << "AUIPC base must have imm 0";
  EXPECT_NE(with_slot(2, rv::jalr(0, 5, 4)).find("forward"), std::string::npos) << "JALR must go past itself";
}

// ---------------------------------------------------------------- dynamic invariants (run on the model)

TEST(GenDynamic, EveryProgramTerminatesCleanlyOnTheModel) {
  const GenConfig cfg;
  Failures f;
  const int n = 6000;
  for (int i = 0; i < n; ++i) {
    const std::uint64_t seed = program_seed(2, i);
    const Program p = generate(cfg, seed);
    const ModelResult r = run_model(p.code, {static_cast<std::uint64_t>(p.code.size()) + 1, true});
    // Forward-only control flow means at most one pass over the code.
    if (r.status != Status::Exited) { f.add(seed, std::string("status ") + to_string(r.status)); continue; }
    if (r.instret > p.code.size()) f.add(seed, "retired more instructions than the program has");
    if (r.regs[31] != kMagicAddr) f.add(seed, "x31 was not the magic address at exit");
    if (r.exit_code != r.regs[1]) f.add(seed, "exit code is not x1");
    for (const auto& s : r.stores)
      if (s.addr < kDataBase || s.addr >= kDataBase + kDataBytes) f.add(seed, "store outside the data window at " + std::to_string(s.addr));
  }
  f.expect_none("dynamic invariants", n);
}

TEST(GenDynamic, ProgramsActuallyExerciseBranchesJumpsAndMemory) {
  // Guards against a generator that is "safe" only because it emits nothing interesting.
  const GenConfig cfg;
  long taken = 0, not_taken = 0, jumps = 0, loads_after_store = 0, store_records = 0;
  const int n = 2000;
  for (int i = 0; i < n; ++i) {
    const Program p = generate(cfg, program_seed(3, i));
    const ModelResult r = run_model(p.code, {10'000, true});
    store_records += static_cast<long>(r.stores.size());
    for (const auto& rec : r.retired) {
      const Decoded d = decode(rec.insn);
      const bool branch = d.op >= Op::BEQ && d.op <= Op::BGEU;
      if (branch) (rec.next_pc != rec.pc + 4 ? taken : not_taken)++;
      if (d.op == Op::JAL || d.op == Op::JALR) ++jumps;
    }
    if (!r.stores.empty()) ++loads_after_store;
  }
  EXPECT_GT(taken, 1000);
  EXPECT_GT(not_taken, 1000);
  EXPECT_GT(jumps, 500);
  EXPECT_GT(store_records, 5000);
  EXPECT_GT(loads_after_store, n / 2);
}

// ---------------------------------------------------------------- determinism and statelessness

TEST(GenDeterminism, SameVersionConfigAndSeedGiveTheSameProgram) {
  const GenConfig cfg;
  for (int i = 0; i < 500; ++i) {
    const std::uint64_t seed = program_seed(4, i);
    const Program a = generate(cfg, seed);
    const Program b = generate(cfg, seed);
    ASSERT_EQ(a.code, b.code);
    ASSERT_EQ(a.body_end, b.body_end);
  }
}

TEST(GenDeterminism, DifferentSeedsAlmostAlwaysGiveDifferentPrograms) {
  std::set<std::uint64_t> hashes;
  for (int i = 0; i < 5000; ++i) hashes.insert(program_hash(generate({}, program_seed(5, i))));
  EXPECT_EQ(hashes.size(), 5000u);
}

TEST(GenDeterminism, ConfigIsPartOfTheInput) {
  // A seed does not fix the program on its own: the config is part of the input. (A tiny config change can leave a
  // given seed's drawn length, and so the whole program, unchanged, so compare across many seeds.)
  GenConfig fixed;
  fixed.min_len = fixed.max_len = 50;
  int differing = 0;
  for (int i = 0; i < 200; ++i) {
    const std::uint64_t seed = program_seed(9, i);
    const Program a = generate({}, seed);
    const Program b = generate(fixed, seed);
    EXPECT_EQ(b.body_end - b.body_begin, 50u);
    if (a.code != b.code) ++differing;
  }
  EXPECT_GE(differing, 190);
}

// Regression pin: these hashes were produced by this generator at kGeneratorVersion 1 (they are a pin, not an
// independent check). If a change to the generator moves them, bump kGeneratorVersion and re-pin deliberately.
TEST(GenDeterminism, GoldenHashesPinGeneratorVersionOne) {
  EXPECT_EQ(kGeneratorVersion, 1u);
  const std::array<std::uint64_t, 6> golden = {6810181434838770933ULL, 9028489777701505244ULL, 12520415729436692383ULL,
                                              16856710952033124212ULL, 14852901968629487806ULL, 12823755400592269455ULL};
  for (std::size_t i = 0; i < golden.size(); ++i)
    EXPECT_EQ(program_hash(generate({}, program_seed(0, i))), golden[i]) << "program " << i;
}

TEST(GenStateless, OutputDoesNotDependOnWhatWasGeneratedBefore) {
  const GenConfig cfg;
  const Program reference = generate(cfg, 12345);
  for (int i = 0; i < 200; ++i) (void)generate(cfg, program_seed(6, i));
  GenConfig other;
  other.max_len = 50;
  (void)generate(other, 99);
  EXPECT_EQ(generate(cfg, 12345).code, reference.code);
}

TEST(GenStateless, ConcurrentCallsAgreeWithSequentialOnes) {
  const GenConfig cfg;
  std::vector<std::uint64_t> sequential(64);
  for (std::size_t i = 0; i < sequential.size(); ++i) sequential[i] = program_hash(generate(cfg, program_seed(7, i)));
  std::vector<std::uint64_t> threaded(64);
  std::vector<std::thread> threads;
  threads.reserve(8);
  for (std::size_t t = 0; t < 8; ++t)
    threads.emplace_back([&, t] {
      for (std::size_t i = t; i < threaded.size(); i += 8) threaded[i] = program_hash(generate(cfg, program_seed(7, i)));
    });
  for (auto& th : threads) th.join();
  EXPECT_EQ(threaded, sequential);
}

TEST(GenStateless, GeneratorTakesNoInputFromOutcomes) {
  // The API is generate(config, seed) -> Program: there is no parameter through which a past result could enter.
  static_assert(std::is_same_v<decltype(&generate), Program (*)(const GenConfig&, std::uint64_t)>);
  SUCCEED();
}

// ---------------------------------------------------------------- distribution (profile G0 is "uniform")

namespace {
struct Stats {
  std::map<Op, long> ops;
  long slots = 0;
  std::map<unsigned, long> rd, rs1, rs2;  // from R-type instructions only
  std::map<int, long> addi_imm_bucket;     // ADDI immediates in 8 buckets of 512
  std::map<unsigned, long> shamt;          // SLLI/SRLI/SRAI
  long lengths_sum = 0, len_min = 1 << 30, len_max = 0;
};

Stats collect(int programs) {
  Stats s;
  const GenConfig cfg;
  for (int i = 0; i < programs; ++i) {
    const Program p = generate(cfg, program_seed(8, i));
    const long len = p.body_end - p.body_begin;
    s.lengths_sum += len;
    s.len_min = std::min(s.len_min, len);
    s.len_max = std::max(s.len_max, len);
    for (std::uint32_t w = p.body_begin; w < p.body_end; ++w) {
      const Decoded d = decode(p.code[w]);
      ++s.ops[d.op];
      ++s.slots;
      if (d.op >= Op::ADD && d.op <= Op::AND) { ++s.rd[d.rd]; ++s.rs1[d.rs1]; ++s.rs2[d.rs2]; }
      if (d.op == Op::ADDI) ++s.addi_imm_bucket[(d.imm + 2048) / 512];
      if (d.op >= Op::SLLI && d.op <= Op::SRAI) ++s.shamt[static_cast<unsigned>(d.imm)];
    }
  }
  return s;
}
}  // namespace

TEST(GenDistribution, G0IsUniformOverTheLegalOpcodes) {
  const Stats s = collect(10000);
  // Each of the 38 opcodes is drawn with probability 1/38; a JALR draw takes two slots (AUIPC + JALR), so per slot
  // every opcode has frequency 1/39, except AUIPC which gets its own draws plus one per JALR (2/39).
  EXPECT_EQ(s.ops.count(Op::Illegal), 0u);
  EXPECT_EQ(s.ops.count(Op::ECALL), 0u);
  EXPECT_EQ(s.ops.count(Op::EBREAK), 0u);
  for (const auto& [op, n] : s.ops) {
    const double expected = (op == Op::AUIPC ? 2.0 : 1.0) / 39.0 * static_cast<double>(s.slots);
    EXPECT_NEAR(static_cast<double>(n), expected, 0.08 * expected) << op_name(op);
  }
  EXPECT_EQ(s.ops.size(), 38u) << "every legal opcode appears";
}

TEST(GenDistribution, RegistersAreUniformOverX0ToX30AndNeverX31) {
  const Stats s = collect(10000);
  for (const auto* hist : {&s.rd, &s.rs1, &s.rs2}) {
    long total = 0;
    for (const auto& [r, n] : *hist) total += n;
    EXPECT_EQ(hist->count(31), 0u);
    EXPECT_EQ(hist->size(), 31u);
    for (const auto& [r, n] : *hist) EXPECT_NEAR(static_cast<double>(n), static_cast<double>(total) / 31, 0.08 * static_cast<double>(total) / 31) << "x" << r;
  }
}

TEST(GenDistribution, ImmediatesShiftAmountsAndLengthsAreUniform) {
  const Stats s = collect(10000);
  long addi = 0;
  for (const auto& [b, n] : s.addi_imm_bucket) addi += n;
  ASSERT_EQ(s.addi_imm_bucket.size(), 8u);
  for (const auto& [b, n] : s.addi_imm_bucket) EXPECT_NEAR(static_cast<double>(n), static_cast<double>(addi) / 8, 0.08 * static_cast<double>(addi) / 8) << "bucket " << b;
  long sh = 0;
  for (const auto& [v, n] : s.shamt) sh += n;
  ASSERT_EQ(s.shamt.size(), 32u);
  for (const auto& [v, n] : s.shamt) EXPECT_NEAR(static_cast<double>(n), static_cast<double>(sh) / 32, 0.15 * static_cast<double>(sh) / 32) << "shamt " << v;
  EXPECT_EQ(s.len_min, 32);
  EXPECT_EQ(s.len_max, 128);
  EXPECT_NEAR(static_cast<double>(s.lengths_sum) / 10000, 80.0, 0.8);
}

// ---------------------------------------------------------------- configuration

TEST(GenConfigCheck, UnusableConfigsSayWhatToChange) {
  GenConfig c;
  c.min_len = 0;
  EXPECT_NE(validate(c).find("min_len"), std::string::npos);
  c = {};
  c.max_len = 10;
  EXPECT_NE(validate(c).find("max_len"), std::string::npos);
  c = {};
  c.max_len = kMaxCodeWords;
  EXPECT_NE(validate(c).find("Lower max_len"), std::string::npos);
  c = {};
  c.max_skip = 0;
  EXPECT_NE(validate(c).find("max_skip"), std::string::npos);
  c.max_skip = 257;
  EXPECT_NE(validate(c).find("max_skip"), std::string::npos);
  for (const Profile p : {Profile::G1, Profile::G2, Profile::G3}) {
    c = {};
    c.profile = p;
    EXPECT_EQ(validate(c), "");
    EXPECT_NO_THROW((void)generate(c, 1));
  }
  EXPECT_EQ(validate({}), "");
}

TEST(GenProfiles, G1G2G3RemainDeterministicAndSatisfyTheCommonInvariants) {
  for (const Profile profile : {Profile::G1, Profile::G2, Profile::G3}) {
    GenConfig cfg;
    cfg.profile = profile;
    for (int i = 0; i < 500; ++i) {
      const std::uint64_t seed = program_seed(900 + static_cast<unsigned>(profile), i);
      const Program a = generate(cfg, seed), b = generate(cfg, seed);
      EXPECT_EQ(a.code, b.code);
      EXPECT_EQ(check_structure(a, cfg), "") << "profile " << static_cast<unsigned>(profile) << " seed " << seed;
      EXPECT_EQ(run_model(a.code, {100000, false}).status, Status::Exited);
    }
  }
}
