#include "generator.hpp"

#include <array>
#include <stdexcept>

#include "memory_map.hpp"
#include "rng.hpp"
#include "rv32i.hpp"

namespace cutoff::gen {

namespace {

// The legal opcode set, in a fixed order: changing it changes every program, so it needs a version bump.
constexpr std::array<Op, 38> kLegalOps = {
    Op::LUI,  Op::AUIPC, Op::JAL,  Op::JALR, Op::BEQ,  Op::BNE,  Op::BLT,  Op::BGE,  Op::BLTU, Op::BGEU,
    Op::LB,   Op::LH,    Op::LW,   Op::LBU,  Op::LHU,  Op::SB,   Op::SH,   Op::SW,   Op::ADDI, Op::SLTI,
    Op::SLTIU, Op::XORI, Op::ORI,  Op::ANDI, Op::SLLI, Op::SRLI, Op::SRAI, Op::ADD,  Op::SUB,  Op::SLL,
    Op::SLT,  Op::SLTU,  Op::XOR,  Op::SRL,  Op::SRA,  Op::OR,   Op::AND,  Op::FENCE};

// One body slot. A JALR is always preceded by the AUIPC that computes its base address.
struct Slot {
  Op op = Op::ADDI;
  bool jalr_base = false;  // this AUIPC feeds the JALR in the next slot
};

class Builder {
 public:
  Builder(const GenConfig& cfg, std::uint64_t seed) : cfg_(cfg), rng_(seed) {}

  Program build(std::uint64_t seed) {
    Program p;
    p.seed = seed;
    const std::uint32_t len = cfg_.min_len + rng_.below(cfg_.max_len - cfg_.min_len + 1);
    p.code.reserve(kInitWords + len + kEpilogueWords);

    // Init block: random values in x1..x30, then the data base register.
    for (unsigned r = 1; r < kDataBaseReg; ++r) push_li(p.code, r, rng_.next32());
    push_li(p.code, kDataBaseReg, kDataBaseRegValue);
    p.body_begin = static_cast<std::uint32_t>(p.code.size());

    draw_slots(len);
    for (std::uint32_t i = 0; i < len; ++i) p.code.push_back(encode_slot(i, len));
    p.body_end = static_cast<std::uint32_t>(p.code.size());

    // Epilogue: ends the run with x1 as the exit code.
    push_li(p.code, kDataBaseReg, kMagicAddr);
    p.code.push_back(rv::sw(1, kDataBaseReg, 0));
    return p;
  }

 private:
  static void push_li(std::vector<std::uint32_t>& code, unsigned rd, std::uint32_t v) {
    const auto w = rv::li(rd, v);
    code.insert(code.end(), w.begin(), w.end());
  }

  unsigned reg() { return rng_.below(31); }  // x0..x30; x31 is reserved
  std::int32_t imm12() { return static_cast<std::int32_t>(rng_.below(4096)) - 2048; }

  // Pass 1: choose the opcode of every slot.
  void draw_slots(std::uint32_t len) {
    slots_.assign(len, Slot{});
    for (std::uint32_t i = 0; i < len;) {
      const Op op = kLegalOps[rng_.below(static_cast<std::uint32_t>(kLegalOps.size()))];
      if (op == Op::JALR) {
        if (i + 1 >= len) continue;  // no room for the AUIPC/JALR pair; draw again
        slots_[i] = {Op::AUIPC, true};
        slots_[i + 1] = {Op::JALR, false};
        i += 2;
      } else {
        slots_[i++] = {op, false};
      }
    }
  }

  // Slot index `t` of the body (t == len is the epilogue) a jump may land on: never the JALR half of a pair.
  bool can_land(std::uint32_t t, std::uint32_t len) const { return t == len || slots_[t].op != Op::JALR; }

  // Forward target in [first, len], preferring the next max_skip slots.
  std::uint32_t pick_target(std::uint32_t i, std::uint32_t first, std::uint32_t len) {
    const std::uint32_t last = std::min(len, i + cfg_.max_skip);
    std::uint32_t count = 0;
    for (std::uint32_t t = first; t <= last; ++t) count += can_land(t, len) ? 1 : 0;
    if (count == 0) {  // the window held only JALR halves: take the next legal slot after it
      std::uint32_t t = last + 1;
      while (!can_land(t, len)) ++t;
      return t;
    }
    std::uint32_t nth = rng_.below(count);
    for (std::uint32_t t = first; t <= last; ++t)
      if (can_land(t, len) && nth-- == 0) return t;
    return len;  // unreachable
  }

  // Pass 2: operands and encoding.
  std::uint32_t encode_slot(std::uint32_t i, std::uint32_t len) {
    const Slot& s = slots_[i];
    switch (s.op) {
      case Op::LUI: { const unsigned rd = reg(); return rv::lui(rd, rng_.below(1u << 20)); }
      case Op::AUIPC: {
        if (s.jalr_base) {
          jalr_base_reg_ = 1 + rng_.below(30);  // x1..x30: x0 would make the base 0
          return rv::auipc(jalr_base_reg_, 0);
        }
        const unsigned rd = reg();
        return rv::auipc(rd, rng_.below(1u << 20));
      }
      case Op::JAL: {
        const unsigned rd = reg();
        const std::uint32_t t = pick_target(i, i + 1, len);
        return rv::jal(rd, 4 * static_cast<std::int32_t>(t - i));
      }
      case Op::JALR: {
        // The AUIPC one slot earlier set base = its own pc; the offset (plus an optional odd bit that JALR clears)
        // lands on slot t. The AUIPC sits at slot i-1.
        const unsigned rd = reg();
        const std::uint32_t t = pick_target(i, i + 1, len);
        const std::int32_t imm = 4 * static_cast<std::int32_t>(t - (i - 1)) + static_cast<std::int32_t>(rng_.below(2));
        return rv::jalr(rd, jalr_base_reg_, imm);
      }
      case Op::BEQ: case Op::BNE: case Op::BLT: case Op::BGE: case Op::BLTU: case Op::BGEU: {
        const unsigned rs1 = reg();
        const unsigned rs2 = reg();
        const std::int32_t off = 4 * static_cast<std::int32_t>(pick_target(i, i + 1, len) - i);
        switch (s.op) {
          case Op::BEQ: return rv::beq(rs1, rs2, off);
          case Op::BNE: return rv::bne(rs1, rs2, off);
          case Op::BLT: return rv::blt(rs1, rs2, off);
          case Op::BGE: return rv::bge(rs1, rs2, off);
          case Op::BLTU: return rv::bltu(rs1, rs2, off);
          default: return rv::bgeu(rs1, rs2, off);
        }
      }
      case Op::LW: { const unsigned rd = reg(); return rv::lw(rd, kDataBaseReg, 4 * (static_cast<std::int32_t>(rng_.below(1024)) - 512)); }
      case Op::LH: { const unsigned rd = reg(); return rv::lh(rd, kDataBaseReg, 2 * (static_cast<std::int32_t>(rng_.below(2048)) - 1024)); }
      case Op::LHU: { const unsigned rd = reg(); return rv::lhu(rd, kDataBaseReg, 2 * (static_cast<std::int32_t>(rng_.below(2048)) - 1024)); }
      case Op::LB: { const unsigned rd = reg(); return rv::lb(rd, kDataBaseReg, imm12()); }
      case Op::LBU: { const unsigned rd = reg(); return rv::lbu(rd, kDataBaseReg, imm12()); }
      case Op::SW: { const unsigned rs2 = reg(); return rv::sw(rs2, kDataBaseReg, 4 * (static_cast<std::int32_t>(rng_.below(1024)) - 512)); }
      case Op::SH: { const unsigned rs2 = reg(); return rv::sh(rs2, kDataBaseReg, 2 * (static_cast<std::int32_t>(rng_.below(2048)) - 1024)); }
      case Op::SB: { const unsigned rs2 = reg(); return rv::sb(rs2, kDataBaseReg, imm12()); }
      case Op::ADDI: case Op::SLTI: case Op::SLTIU: case Op::XORI: case Op::ORI: case Op::ANDI: {
        const unsigned rd = reg();
        const unsigned rs1 = reg();
        const std::int32_t imm = imm12();
        switch (s.op) {
          case Op::ADDI: return rv::addi(rd, rs1, imm);
          case Op::SLTI: return rv::slti(rd, rs1, imm);
          case Op::SLTIU: return rv::sltiu(rd, rs1, imm);
          case Op::XORI: return rv::xori(rd, rs1, imm);
          case Op::ORI: return rv::ori(rd, rs1, imm);
          default: return rv::andi(rd, rs1, imm);
        }
      }
      case Op::SLLI: case Op::SRLI: case Op::SRAI: {
        const unsigned rd = reg();
        const unsigned rs1 = reg();
        const unsigned shamt = rng_.below(32);
        return s.op == Op::SLLI ? rv::slli(rd, rs1, shamt) : s.op == Op::SRLI ? rv::srli(rd, rs1, shamt) : rv::srai(rd, rs1, shamt);
      }
      case Op::ADD: case Op::SUB: case Op::SLL: case Op::SLT: case Op::SLTU:
      case Op::XOR: case Op::SRL: case Op::SRA: case Op::OR: case Op::AND: {
        const unsigned rd = reg();
        const unsigned rs1 = reg();
        const unsigned rs2 = reg();
        switch (s.op) {
          case Op::ADD: return rv::add(rd, rs1, rs2);
          case Op::SUB: return rv::sub(rd, rs1, rs2);
          case Op::SLL: return rv::sll(rd, rs1, rs2);
          case Op::SLT: return rv::slt(rd, rs1, rs2);
          case Op::SLTU: return rv::sltu(rd, rs1, rs2);
          case Op::XOR: return rv::xor_(rd, rs1, rs2);
          case Op::SRL: return rv::srl(rd, rs1, rs2);
          case Op::SRA: return rv::sra(rd, rs1, rs2);
          case Op::OR: return rv::or_(rd, rs1, rs2);
          default: return rv::and_(rd, rs1, rs2);
        }
      }
      case Op::FENCE: return rv::fence();
      default: break;
    }
    throw std::logic_error("generator drew an opcode outside the legal set");
  }

  GenConfig cfg_;
  Rng rng_;
  std::vector<Slot> slots_;
  unsigned jalr_base_reg_ = 1;
};

}  // namespace

std::string validate(const GenConfig& cfg) {
  if (cfg.profile != Profile::G0) return "only profile G0 exists so far; G1-G3 arrive in Step 8. Set profile = G0.";
  if (cfg.min_len < 1) return "min_len must be at least 1.";
  if (cfg.max_len < cfg.min_len) return "max_len must not be smaller than min_len.";
  if (kInitWords + cfg.max_len + kEpilogueWords > kMaxCodeWords)
    return "max_len is too large: the program would reach the data window. Lower max_len to at most " +
           std::to_string(kMaxCodeWords - kInitWords - kEpilogueWords) + ".";
  if (cfg.max_skip < 1 || cfg.max_skip > 256) return "max_skip must be between 1 and 256.";
  return {};
}

Program generate(const GenConfig& cfg, std::uint64_t seed) {
  if (const std::string err = validate(cfg); !err.empty()) throw std::invalid_argument(err);
  return Builder(cfg, seed).build(seed);
}

std::uint64_t program_hash(const Program& p) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const std::uint32_t w : p.code)
    for (unsigned b = 0; b < 4; ++b) {
      h ^= (w >> (8 * b)) & 0xffu;
      h *= 0x100000001b3ULL;
    }
  return h;
}

}  // namespace cutoff::gen
